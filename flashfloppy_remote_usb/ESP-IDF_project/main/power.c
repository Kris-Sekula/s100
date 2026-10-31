#include "power.h"
#include "board.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/inet.h"
#include "cJSON.h"

static const char *TAG = "ff_power";

#define KASA_IO_TIMEOUT_MS 2000
/* Restart's "on" step: a single Wi-Fi hiccup must not leave the
 * machine switched off (v39). */
#define KASA_ON_ATTEMPTS  3
#define KASA_OFF_ATTEMPTS 2
#define KASA_RETRY_GAP_MS 2000

static ff_power_cfg_t s_cfg;
static bool s_configured;

/* s_lock (recursive) guards every device exchange AND the outlet
 * resolution below — v38 wrote s_child_* outside it, so the poll
 * task, an HTTP-triggered set and the restart task could interleave
 * and send a half-written child id. Held across a whole
 * query-then-set sequence, hence recursive. */
static SemaphoreHandle_t s_lock;
static char s_child_id[64];      /* cached strip outlet id —
                                  * 42 chars (40 device + 2 suffix);
                                  * a 40-byte buffer silently
                                  * truncated it in v28–v33 and every
                                  * set got err -14 "entry not exist" */
static bool s_child_resolved;
static bool s_child_id_logged;
static bool s_child_is_strip;    /* sysinfo had children */
/* Set under s_lock for the duration of an exchange: the poll
 * task's routine exchanges stay silent so a strip that is merely
 * off-network doesn't spam the console every poll. */
static bool s_quiet;

/* s_stat_lock guards the cache the HTTP handler reads, plus the
 * busy/error fields. Never held across network I/O. */
static SemaphoreHandle_t s_stat_lock;
static ff_power_status_t s_stat;

static QueueHandle_t s_queue;
static bool s_started;

static void stat_lock(void) { xSemaphoreTake(s_stat_lock, portMAX_DELAY); }
static void stat_unlock(void) { xSemaphoreGive(s_stat_lock); }

static void dev_lock(void) { xSemaphoreTakeRecursive(s_lock, portMAX_DELAY); }
static void dev_unlock(void) { xSemaphoreGiveRecursive(s_lock); }

void ff_power_configure(const ff_power_cfg_t *cfg)
{
    if (!s_lock) s_lock = xSemaphoreCreateRecursiveMutex();
    if (!s_stat_lock) s_stat_lock = xSemaphoreCreateMutex();
    s_cfg = *cfg;
    if (!s_cfg.port) s_cfg.port = 9999;
    if (!s_cfg.outlet) s_cfg.outlet = 1;
    if (!s_cfg.delay_s) s_cfg.delay_s = 5;
    if (!s_cfg.name[0]) snprintf(s_cfg.name, sizeof(s_cfg.name),
                                 "Machine");
    s_configured = s_cfg.host[0] != 0;
    s_child_resolved = false;
    if (s_configured) {
        ESP_LOGI(TAG, "power control: %s at %s:%d, outlet %d, "
                      "restart delay %d s",
                 s_cfg.name, s_cfg.host, s_cfg.port, s_cfg.outlet,
                 s_cfg.delay_s);
    }
}

bool ff_power_configured(void) { return s_configured; }

/* The Kasa autokey XOR: key starts at 171; each byte is XORed with
 * the running key, which then becomes the cipher byte just
 * produced (encrypt) or consumed (decrypt). */
static void kasa_crypt(const uint8_t *in, uint8_t *out, size_t len,
                       bool encrypt)
{
    uint8_t key = 171;
    for (size_t i = 0; i < len; i++) {
        if (encrypt) {
            out[i] = in[i] ^ key;
            key = out[i];
        } else {
            /* Save the cipher byte BEFORE writing: replies are
             * decrypted in place (in == out), and key must chain on
             * the cipher byte consumed. Reading in[i] after the
             * write returns the plaintext byte instead and garbles
             * everything past byte 0 — that was the v28–v30 bug:
             * requests went out fine, every reply decrypted to
             * garbage, JSON parsing failed, UI said "unreachable". */
            uint8_t c = in[i];
            out[i] = c ^ key;
            key = c;
        }
    }
}

/* connect() with a hard timeout: non-blocking connect, then select()
 * for writability, then SO_ERROR for the outcome. Leaves the socket
 * blocking again (with the SO_*TIMEO bounds) on success. */
static int connect_timeout(int sock, const struct sockaddr *sa,
                           socklen_t len, int timeout_ms)
{
    int fl = fcntl(sock, F_GETFL, 0);
    if (fl < 0 || fcntl(sock, F_SETFL, fl | O_NONBLOCK) < 0) return -1;
    int r = connect(sock, sa, len);
    if (r != 0) {
        if (errno != EINPROGRESS) return -1;
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(sock, &wfds);
        struct timeval tv = {.tv_sec = timeout_ms / 1000,
                             .tv_usec = (timeout_ms % 1000) * 1000};
        r = select(sock + 1, NULL, &wfds, NULL, &tv);
        if (r <= 0) {
            if (r == 0) errno = ETIMEDOUT;
            return -1;
        }
        int soerr = 0;
        socklen_t sl = sizeof(soerr);
        if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &soerr, &sl) != 0) {
            return -1;
        }
        if (soerr) {
            errno = soerr;
            return -1;
        }
    }
    return fcntl(sock, F_SETFL, fl) < 0 ? -1 : 0;
}

/* Send one JSON command, return the malloc'd NUL-terminated JSON
 * reply (caller frees), or NULL on any failure. Every step has a
 * KASA_IO_TIMEOUT_MS bound. Call with s_lock held. */
static char *kasa_exchange(const char *request)
{
    char *result = NULL;
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)s_cfg.port);
    /* An IP literal (the usual POWER.CFG form) needs no DNS at all;
     * only a hostname goes through the resolver. */
    if (inet_pton(AF_INET, s_cfg.host, &sa.sin_addr) != 1) {
        struct addrinfo hints = {0}, *res = NULL;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(s_cfg.host, NULL, &hints, &res) != 0 || !res) {
            if (!s_quiet) ESP_LOGW(TAG, "cannot resolve %s", s_cfg.host);
            return NULL;
        }
        sa.sin_addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
        freeaddrinfo(res);
    }
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return NULL;
    struct timeval tv = {.tv_sec = KASA_IO_TIMEOUT_MS / 1000,
                         .tv_usec = (KASA_IO_TIMEOUT_MS % 1000) * 1000};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    /* SO_SNDTIMEO does not bound connect() in lwIP: an absent strip
     * used to hold the caller for the full TCP SYN retry cycle
     * (tens of seconds). Bounded explicitly since v39. */
    if (connect_timeout(sock, (struct sockaddr *)&sa, sizeof(sa),
                        KASA_IO_TIMEOUT_MS) != 0) {
        if (!s_quiet)
            ESP_LOGW(TAG, "connect to %s:%d failed (errno %d: %s)",
                     s_cfg.host, s_cfg.port, errno, strerror(errno));
        close(sock);
        return NULL;
    }

    size_t qlen = strlen(request);
    uint8_t *frame = malloc(qlen + 4);
    if (!frame) { close(sock); return NULL; }
    frame[0] = (uint8_t)(qlen >> 24);
    frame[1] = (uint8_t)(qlen >> 16);
    frame[2] = (uint8_t)(qlen >> 8);
    frame[3] = (uint8_t)(qlen);
    kasa_crypt((const uint8_t *)request, frame + 4, qlen, true);
    size_t sent = 0;
    bool ok = true;
    while (sent < qlen + 4) {
        int n = send(sock, frame + sent, qlen + 4 - sent, 0);
        if (n <= 0) { ok = false; break; }
        sent += (size_t)n;
    }
    free(frame);
    if (!ok) {
        if (!s_quiet)
            ESP_LOGW(TAG, "send to %s failed (errno %d: %s)",
                     s_cfg.host, errno, strerror(errno));
        close(sock);
        return NULL;
    }

    uint8_t hdr[4];
    size_t got = 0;
    while (got < 4) {
        int n = recv(sock, hdr + got, 4 - got, 0);
        if (n <= 0) {
            if (!s_quiet)
                ESP_LOGW(TAG, "no reply header from %s (n=%d, "
                              "errno %d: %s) — connection closed "
                              "or timed out",
                         s_cfg.host, n, errno, strerror(errno));
            close(sock);
            return NULL;
        }
        got += (size_t)n;
    }
    uint32_t rlen = ((uint32_t)hdr[0] << 24) | ((uint32_t)hdr[1] << 16) |
                    ((uint32_t)hdr[2] << 8) | hdr[3];
    if (rlen == 0 || rlen > 32768) {
        if (!s_quiet)
            ESP_LOGW(TAG, "implausible reply length %u from %s",
                     (unsigned)rlen, s_cfg.host);
        close(sock);
        return NULL;
    }
    uint8_t *buf = malloc(rlen + 1);
    if (!buf) { close(sock); return NULL; }
    got = 0;
    while (got < rlen) {
        int n = recv(sock, buf + got, rlen - got, 0);
        if (n <= 0) {
            if (!s_quiet)
                ESP_LOGW(TAG, "reply body truncated at %u/%u bytes",
                         (unsigned)got, (unsigned)rlen);
            free(buf);
            close(sock);
            return NULL;
        }
        got += (size_t)n;
    }
    close(sock);
    kasa_crypt(buf, buf, rlen, false);
    buf[rlen] = 0;
    result = (char *)buf;
    return result;
}

/* Run an exchange under the device lock. quiet=true silences the
 * per-failure logs (the background poller logs transitions itself,
 * once, instead of every retry). */
static char *exchange_locked_q(const char *request, bool quiet)
{
    dev_lock();
    bool was_quiet = s_quiet;
    s_quiet = quiet;
    char *r = kasa_exchange(request);
    s_quiet = was_quiet;
    dev_unlock();
    return r;
}

/* get_sysinfo; returns the parsed root (caller deletes). */
static cJSON *get_sysinfo(bool quiet)
{
    char *resp = exchange_locked_q(
        "{\"system\":{\"get_sysinfo\":{}}}", quiet);
    if (!resp) return NULL;
    cJSON *root = cJSON_Parse(resp);
    if (!root) {
        /* Show what actually came back — an error object instead of
         * sysinfo would be the tell-tale of a protocol mismatch. */
        if (!quiet)
            ESP_LOGW(TAG, "sysinfo reply is not parseable JSON: "
                          "%.160s", resp);
        free(resp);
        return NULL;
    }
    free(resp);
    cJSON *sys = cJSON_GetObjectItem(
        cJSON_GetObjectItem(root, "system"), "get_sysinfo");
    if (!sys) {
        if (!quiet)
            ESP_LOGW(TAG, "sysinfo reply lacks system.get_sysinfo");
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

/* Query relay state and (re)resolve which child we control. The
 * whole thing runs under the device lock. */
static esp_err_t query_impl(bool quiet, bool *on_out)
{
    if (!s_configured) return ESP_ERR_INVALID_STATE;
    dev_lock();
    cJSON *root = get_sysinfo(quiet);
    if (!root) {
        dev_unlock();
        return ESP_FAIL;
    }
    cJSON *sys = cJSON_GetObjectItem(
        cJSON_GetObjectItem(root, "system"), "get_sysinfo");
    cJSON *children = cJSON_GetObjectItem(sys, "children");
    esp_err_t err = ESP_OK;
    if (cJSON_IsArray(children) &&
        cJSON_GetArraySize(children) > 0) {
        static bool sysinfo_logged;
        if (!sysinfo_logged) {
            sysinfo_logged = true;
            const char *model = cJSON_GetStringValue(
                cJSON_GetObjectItem(sys, "model"));
            const char *sw = cJSON_GetStringValue(
                cJSON_GetObjectItem(sys, "sw_ver"));
            const char *hw = cJSON_GetStringValue(
                cJSON_GetObjectItem(sys, "hw_ver"));
            ESP_LOGI(TAG, "device: %s, sw %s, hw %s",
                     model ? model : "?", sw ? sw : "?", hw ? hw : "?");
        }
        int idx = s_cfg.outlet - 1;
        cJSON *child = cJSON_GetArrayItem(children, idx);
        if (!child) {
            if (!quiet)
                ESP_LOGE(TAG, "outlet %d out of range (strip has %d)",
                         s_cfg.outlet, cJSON_GetArraySize(children));
            err = ESP_ERR_NOT_FOUND;
        } else {
            cJSON *st = cJSON_GetObjectItem(child, "state");
            *on_out = cJSON_IsNumber(st) && st->valueint != 0;
            const char *id = cJSON_GetStringValue(
                cJSON_GetObjectItem(child, "id"));
            if (id) {
                snprintf(s_child_id, sizeof(s_child_id), "%s", id);
                s_child_resolved = true;
                s_child_is_strip = true;
                if (!s_child_id_logged) {
                    s_child_id_logged = true;
                    /* Print the STORED copy — that's what set
                     * commands will actually send. */
                    ESP_LOGI(TAG, "outlet %d resolved: child %s, "
                                  "state %d",
                             s_cfg.outlet, s_child_id,
                             (int)*on_out);
                }
            }
        }
    } else {
        cJSON *st = cJSON_GetObjectItem(sys, "relay_state");
        if (cJSON_IsNumber(st)) {
            *on_out = st->valueint != 0;
            s_child_resolved = true;
            s_child_is_strip = false;
        } else {
            err = ESP_FAIL;
        }
    }
    cJSON_Delete(root);
    dev_unlock();
    return err;
}

/* Request object addressed at our outlet: a strip needs the child
 * context, a single plug does not. Call under the device lock. */
static cJSON *new_addressed_request(void)
{
    cJSON *root = cJSON_CreateObject();
    /* Context FIRST, then the module — byte-identical to the
     * request python-kasa sent successfully against Kris's strip
     * (captured 2026-10-30, err_code 0). */
    if (s_child_is_strip) {
        cJSON *ctx = cJSON_AddObjectToObject(root, "context");
        cJSON *ids = cJSON_AddArrayToObject(ctx, "child_ids");
        cJSON_AddItemToArray(ids, cJSON_CreateString(s_child_id));
    }
    return root;
}

static void cache_set_on(bool on)
{
    stat_lock();
    s_stat.on = on;
    stat_unlock();
}

/* One set_relay_state, resolving the outlet first if needed. The
 * device lock is held across resolve + set. */
static esp_err_t power_set_once(bool on)
{
    if (!s_configured) return ESP_ERR_INVALID_STATE;
    dev_lock();
    if (!s_child_resolved) {
        bool dummy;
        if (query_impl(false, &dummy) != ESP_OK) {
            dev_unlock();
            return ESP_FAIL;
        }
    }
    cJSON *root = new_addressed_request();
    cJSON *sys = cJSON_AddObjectToObject(root, "system");
    cJSON *cmd = cJSON_AddObjectToObject(sys, "set_relay_state");
    cJSON_AddNumberToObject(cmd, "state", on ? 1 : 0);
    char *req = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!req) {
        dev_unlock();
        return ESP_ERR_NO_MEM;
    }
    char *resp = exchange_locked_q(req, false);
    esp_err_t err = ESP_FAIL;
    if (!resp) {
        ESP_LOGE(TAG, "set_relay_state: no reply to request %s", req);
    } else {
        cJSON *rroot = cJSON_Parse(resp);
        if (rroot) {
            cJSON *rc = cJSON_GetObjectItem(
                cJSON_GetObjectItem(
                    cJSON_GetObjectItem(rroot, "system"),
                    "set_relay_state"),
                "err_code");
            if (cJSON_IsNumber(rc) && rc->valueint == 0) err = ESP_OK;
            cJSON_Delete(rroot);
        }
        if (err != ESP_OK) {
            /* Show the device's actual answer — an err_code or an
             * error object names what it objected to. */
            ESP_LOGE(TAG, "set_relay_state reply: %.200s", resp);
        }
        free(resp);
    }
    free(req);
    if (err != ESP_OK) {
        /* A failed set often means the strip renumbered children;
         * drop the cached id so the next call re-resolves. */
        s_child_resolved = false;
    }
    dev_unlock();
    if (err == ESP_OK) {
        cache_set_on(on); /* truthful without waiting a poll cycle */
        ESP_LOGI(TAG, "%s outlet %d -> %s", s_cfg.name, s_cfg.outlet,
                 on ? "ON" : "OFF");
    }
    return err;
}

static esp_err_t power_set_retry(bool on, int attempts)
{
    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < attempts; i++) {
        if (i) {
            ESP_LOGW(TAG, "power %s: retry %d of %d", on ? "on" : "off",
                     i + 1, attempts);
            vTaskDelay(pdMS_TO_TICKS(KASA_RETRY_GAP_MS));
        }
        err = power_set_once(on);
        if (err == ESP_OK) break;
    }
    return err;
}

/* One background poll: relay state from sysinfo, watts from the
 * emeter's get_realtime (context-addressed on a strip, like the
 * set command; reply carries power_mw). Logs transitions only —
 * including flips made outside this UI, which is the point. */
static bool poll_once(void)
{
    bool on = false;
    dev_lock();
    if (query_impl(true, &on) != ESP_OK) {
        dev_unlock();
        stat_lock();
        bool was = s_stat.reachable;
        s_stat.reachable = false;
        stat_unlock();
        if (was) {
            ESP_LOGW(TAG, "power strip at %s became unreachable",
                     s_cfg.host);
        }
        return false;
    }
    stat_lock();
    float watts = s_stat.watts; /* keep last if emeter fails */
    stat_unlock();
    cJSON *root = new_addressed_request();
    cJSON *em = cJSON_AddObjectToObject(root, "emeter");
    cJSON_AddObjectToObject(em, "get_realtime");
    char *req = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (req) {
        char *resp = exchange_locked_q(req, true);
        free(req);
        if (resp) {
            cJSON *rr = cJSON_Parse(resp);
            free(resp);
            if (rr) {
                cJSON *rt = cJSON_GetObjectItem(
                    cJSON_GetObjectItem(rr, "emeter"), "get_realtime");
                cJSON *mw = cJSON_GetObjectItem(rt, "power_mw");
                cJSON *pw = cJSON_GetObjectItem(rt, "power");
                if (cJSON_IsNumber(mw)) {
                    watts = (float)mw->valuedouble / 1000.0f;
                } else if (cJSON_IsNumber(pw)) {
                    watts = (float)pw->valuedouble;
                }
                cJSON_Delete(rr);
            }
        }
    }
    dev_unlock();
    stat_lock();
    bool was_reachable = s_stat.reachable;
    bool had = s_stat.valid;
    bool prev_on = s_stat.on;
    s_stat.on = on;
    s_stat.watts = watts;
    s_stat.reachable = true;
    s_stat.valid = true;
    stat_unlock();
    if (!was_reachable) {
        ESP_LOGI(TAG, "power strip reachable: %s outlet %d %s, "
                      "%.1f W",
                 s_cfg.name, s_cfg.outlet, on ? "ON" : "OFF",
                 (double)watts);
    } else if (had && prev_on != on) {
        ESP_LOGI(TAG, "%s outlet %d now %s (changed outside this "
                      "UI?)",
                 s_cfg.name, s_cfg.outlet, on ? "ON" : "OFF");
    }
    return true;
}

static void poll_task(void *arg)
{
    (void)arg;
    for (;;) {
        bool ok = poll_once();
        /* 5 s while the strip answers (Kris's ask). While it
         * doesn't, back off to 30 s: an absent strip — or AP
         * fallback mode, where the home LAN is unreachable by
         * design — shouldn't be retried at full rate forever. */
        vTaskDelay(pdMS_TO_TICKS(ok ? FF_POWER_POLL_MS : 30000));
    }
}

static void set_error(const char *msg)
{
    stat_lock();
    snprintf(s_stat.error, sizeof(s_stat.error), "%s", msg);
    stat_unlock();
    if (msg[0]) ESP_LOGE(TAG, "%s", msg);
}

/* Executes queued commands one at a time. */
static void worker_task(void *arg)
{
    (void)arg;
    for (;;) {
        ff_power_cmd_t cmd;
        if (xQueueReceive(s_queue, &cmd, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        char msg[96];
        msg[0] = 0;
        if (cmd == FF_POWER_ON) {
            if (power_set_retry(true, KASA_ON_ATTEMPTS) != ESP_OK) {
                snprintf(msg, sizeof(msg), "Power on failed - %.60s did "
                         "not answer", s_cfg.host);
            }
        } else if (cmd == FF_POWER_OFF) {
            if (power_set_retry(false, KASA_OFF_ATTEMPTS) != ESP_OK) {
                snprintf(msg, sizeof(msg), "Power off failed - %.60s did "
                         "not answer", s_cfg.host);
            }
        } else {
            ESP_LOGI(TAG, "restarting %s: off, %d s, on", s_cfg.name,
                     s_cfg.delay_s);
            if (power_set_retry(false, KASA_OFF_ATTEMPTS) != ESP_OK) {
                snprintf(msg, sizeof(msg), "Restart aborted: power off "
                         "failed (machine left as it was)");
            } else {
                vTaskDelay(pdMS_TO_TICKS(s_cfg.delay_s * 1000));
                if (power_set_retry(true, KASA_ON_ATTEMPTS) != ESP_OK) {
                    snprintf(msg, sizeof(msg), "Restart: power ON "
                             "failed after %d tries - machine is OFF",
                             KASA_ON_ATTEMPTS);
                } else {
                    ESP_LOGI(TAG, "restart of %s complete", s_cfg.name);
                }
            }
        }
        set_error(msg);
        stat_lock();
        s_stat.busy = false;
        s_stat.restarting = false;
        stat_unlock();
    }
}

void ff_power_start(void)
{
    if (s_started || !s_configured) return;
    s_queue = xQueueCreate(1, sizeof(ff_power_cmd_t));
    if (!s_queue) return;
    s_started = true;
    /* Socket exchanges + cJSON on a 2 KB sysinfo: 6144, not the
     * 4096 that has overflowed twice elsewhere in this project. */
    xTaskCreate(worker_task, "ff_pwr", 6144, NULL, 4, NULL);
    xTaskCreate(poll_task, "ff_pwr_poll", 6144, NULL, 4, NULL);
}

esp_err_t ff_power_request(ff_power_cmd_t cmd)
{
    if (!s_configured || !s_started) return ESP_ERR_INVALID_STATE;
    /* Check-and-set of busy under the lock: two callers (HTTP task,
     * apply task) can never both queue. */
    stat_lock();
    if (s_stat.busy) {
        stat_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    s_stat.busy = true;
    s_stat.restarting = cmd == FF_POWER_RESTART;
    s_stat.error[0] = 0;
    stat_unlock();
    if (xQueueSend(s_queue, &cmd, 0) != pdTRUE) {
        stat_lock();
        s_stat.busy = false;
        s_stat.restarting = false;
        stat_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

void ff_power_restart_async(void)
{
    if (ff_power_request(FF_POWER_RESTART) != ESP_OK) {
        ESP_LOGW(TAG, "restart-after-swap skipped: power control "
                      "busy or not configured");
    }
}

void ff_power_get_status(ff_power_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->configured = s_configured;
    if (!s_configured || !s_stat_lock) return;
    stat_lock();
    *out = s_stat;
    stat_unlock();
    out->configured = true;
    snprintf(out->name, sizeof(out->name), "%s", s_cfg.name);
}
