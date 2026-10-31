#include "power.h"
#include "board.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "cJSON.h"

static const char *TAG = "ff_power";

static ff_power_cfg_t s_cfg;
static bool s_configured;
static SemaphoreHandle_t s_lock; /* serialises device exchanges */
static char s_child_id[64];      /* cached strip outlet id —
                                  * 42 chars (40 device + 2 suffix);
                                  * a 40-byte buffer silently
                                  * truncated it in v28–v33 and every
                                  * set got err -14 "entry not exist" */
static bool s_child_resolved;
static bool s_child_id_logged;
static bool s_child_is_strip;    /* sysinfo had children */
static volatile bool s_restarting;
/* Poll cache (v36): written by the poll task (and by ff_power_set
 * for the relay bit), read by the HTTP handler. */
static volatile bool s_cache_on, s_cache_reachable, s_cache_valid;
static volatile float s_cache_watts;
static bool s_poll_started;
/* Set under s_lock for the duration of an exchange: the poll
 * task's routine exchanges stay silent so a strip that is merely
 * off-network doesn't spam the console every poll. */
static bool s_quiet;

void ff_power_configure(const ff_power_cfg_t *cfg)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
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
const char *ff_power_name(void) { return s_cfg.name; }
bool ff_power_restarting(void) { return s_restarting; }

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

/* Send one JSON command, return the malloc'd NUL-terminated JSON
 * reply (caller frees), or NULL on any failure. Timeouts are short:
 * this runs on a LAN and one caller is an HTTP handler. */
static char *kasa_exchange(const char *request)
{
    char *result = NULL;
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", s_cfg.port);
    if (getaddrinfo(s_cfg.host, port_str, &hints, &res) != 0 || !res) {
        if (!s_quiet) ESP_LOGW(TAG, "cannot resolve %s", s_cfg.host);
        return NULL;
    }
    int sock = socket(res->ai_family, res->ai_socktype, 0);
    if (sock < 0) {
        freeaddrinfo(res);
        return NULL;
    }
    struct timeval tv = {.tv_sec = 2, .tv_usec = 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (connect(sock, res->ai_addr, res->ai_addrlen) != 0) {
        if (!s_quiet)
            ESP_LOGW(TAG, "connect to %s:%d failed (errno %d: %s)",
                     s_cfg.host, s_cfg.port, errno, strerror(errno));
        freeaddrinfo(res);
        close(sock);
        return NULL;
    }
    freeaddrinfo(res);

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

/* Run an exchange under the module lock. quiet=true silences the
 * per-failure logs (the background poller logs transitions
 * itself, once, instead of every retry). */
static char *exchange_locked_q(const char *request, bool quiet)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_quiet = quiet;
    char *r = kasa_exchange(request);
    s_quiet = false;
    xSemaphoreGive(s_lock);
    return r;
}

static char *exchange_locked(const char *request)
{
    return exchange_locked_q(request, false);
}

/* get_sysinfo, and from it resolve (once) which child we control. */
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
    return root; /* caller deletes root; sys borrows from it */
}

static esp_err_t query_impl(bool quiet, bool *on_out)
{
    if (!s_configured) return ESP_ERR_INVALID_STATE;
    cJSON *root = get_sysinfo(quiet);
    if (!root) return ESP_FAIL;
    cJSON *sys = cJSON_GetObjectItem(
        cJSON_GetObjectItem(root, "system"), "get_sysinfo");
    cJSON *children = cJSON_GetObjectItem(sys, "children");
    esp_err_t err = ESP_OK;
    if (cJSON_IsArray(children) &&
        cJSON_GetArraySize(children) > 0) {
        static bool sysinfo_logged;
        if (!sysinfo_logged) {
            sysinfo_logged = true;
            ESP_LOGI(TAG, "device: %s, sw %s, hw %s",
                     cJSON_GetStringValue(
                         cJSON_GetObjectItem(sys, "model")),
                     cJSON_GetStringValue(
                         cJSON_GetObjectItem(sys, "sw_ver")),
                     cJSON_GetStringValue(
                         cJSON_GetObjectItem(sys, "hw_ver")));
        }
        int idx = s_cfg.outlet - 1;
        cJSON *child = cJSON_GetArrayItem(children, idx);
        if (!child) {
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
    return err;
}

esp_err_t ff_power_query(bool *on_out)
{
    return query_impl(false, on_out);
}

esp_err_t ff_power_set(bool on)
{
    if (!s_configured) return ESP_ERR_INVALID_STATE;
    if (!s_child_resolved) {
        bool dummy;
        if (ff_power_query(&dummy) != ESP_OK) return ESP_FAIL;
    }
    cJSON *root = cJSON_CreateObject();
    /* Context FIRST, then the module — byte-identical to the
     * request python-kasa sent successfully against Kris's strip
     * (captured 2026-10-30, err_code 0). Two bugs were stacked:
     * v28–v33 also truncated the child id (see s_child_id), so the
     * v33 experiment that swapped this order could not show an
     * effect — both variants failed on the truncated id alone.
     * With the id fixed, this order is the proven one. */
    if (s_child_is_strip) {
        cJSON *ctx = cJSON_AddObjectToObject(root, "context");
        cJSON *ids = cJSON_AddArrayToObject(ctx, "child_ids");
        cJSON_AddItemToArray(ids, cJSON_CreateString(s_child_id));
    }
    cJSON *sys = cJSON_AddObjectToObject(root, "system");
    cJSON *cmd = cJSON_AddObjectToObject(sys, "set_relay_state");
    cJSON_AddNumberToObject(cmd, "state", on ? 1 : 0);
    char *req = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!req) return ESP_ERR_NO_MEM;
    char *resp = exchange_locked(req);
    if (!resp) {
        ESP_LOGE(TAG, "set_relay_state: no reply to request %s", req);
        free(req);
        return ESP_FAIL;
    }
    free(req);
    cJSON *rroot = cJSON_Parse(resp);
    esp_err_t err = ESP_FAIL;
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
    if (err == ESP_OK) {
        /* Keep the poll cache truthful without waiting a cycle. */
        s_cache_on = on;
        ESP_LOGI(TAG, "%s outlet %d -> %s", s_cfg.name, s_cfg.outlet,
                 on ? "ON" : "OFF");
    } else {
        /* A failed set often means the strip renumbered children;
         * drop the cached id so the next call re-resolves. */
        s_child_resolved = false;
        ESP_LOGE(TAG, "set_relay_state(%d) failed", on);
    }
    return err;
}

bool ff_power_cached_state(bool *on_out, float *watts_out,
                           bool *reachable_out)
{
    if (on_out) *on_out = s_cache_on;
    if (watts_out) *watts_out = s_cache_watts;
    if (reachable_out) *reachable_out = s_cache_reachable;
    return s_cache_valid;
}

/* One background poll: relay state from sysinfo, watts from the
 * emeter's get_realtime (context-addressed on a strip, like the
 * set command; reply carries power_mw). Logs transitions only —
 * including flips made outside this UI, which is the point. */
static bool poll_once(void)
{
    bool on = false;
    if (query_impl(true, &on) != ESP_OK) {
        if (s_cache_reachable) {
            ESP_LOGW(TAG, "power strip at %s became unreachable",
                     s_cfg.host);
        }
        s_cache_reachable = false;
        return false;
    }
    float watts = s_cache_watts; /* keep last if emeter fails */
    cJSON *root = cJSON_CreateObject();
    if (s_child_is_strip) {
        cJSON *ctx = cJSON_AddObjectToObject(root, "context");
        cJSON *ids = cJSON_AddArrayToObject(ctx, "child_ids");
        cJSON_AddItemToArray(ids, cJSON_CreateString(s_child_id));
    }
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
    bool was_reachable = s_cache_reachable;
    bool had = s_cache_valid;
    bool prev_on = s_cache_on;
    s_cache_on = on;
    s_cache_watts = watts;
    s_cache_reachable = true;
    s_cache_valid = true;
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

void ff_power_start_polling(void)
{
    if (s_poll_started || !s_configured) return;
    s_poll_started = true;
    /* Same sizing as the restart task: two exchanges + cJSON on a
     * full sysinfo every cycle. */
    xTaskCreate(poll_task, "ff_pwr_poll", 6144, NULL, 4, NULL);
}

static void restart_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "restarting %s: off, %d s, on", s_cfg.name,
             s_cfg.delay_s);
    bool ok = ff_power_set(false) == ESP_OK;
    if (ok) {
        vTaskDelay(pdMS_TO_TICKS(s_cfg.delay_s * 1000));
        ok = ff_power_set(true) == ESP_OK;
    }
    if (ok) {
        ESP_LOGI(TAG, "restart of %s complete", s_cfg.name);
    } else {
        ESP_LOGE(TAG, "restart of %s aborted: a power command "
                      "failed, check the outlet state manually",
                 s_cfg.name);
    }
    s_restarting = false;
    vTaskDelete(NULL);
}

void ff_power_restart_async(void)
{
    if (!s_configured || s_restarting) return;
    s_restarting = true;
    /* Socket exchanges + cJSON on a 2 KB sysinfo: 6144, not the
     * 4096 that has overflowed twice elsewhere in this project. */
    if (xTaskCreate(restart_task, "ff_pwr", 6144, NULL, 4, NULL) !=
        pdPASS) {
        s_restarting = false;
    }
}
