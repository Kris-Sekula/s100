#include "board.h"
#include "state.h"
#include "sdcard.h"
#include "storage.h"
#include "power.h"
#include "http_server.h"
#include "lcd.h"
#include "util.h"
#include <stdio.h>  /* snprintf() */
#include <stdlib.h> /* atoi() for the POWER.CFG parser */
#include <string.h>
#include <strings.h> /* strcasecmp() for the WIFI.CFG parser */
#include <sys/stat.h> /* stat() for the boot auto-present check */
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

static const char *TAG = "ff_main";

/* ---- Wi-Fi: home-network station mode (optional) + AP fallback ----
 * Default behaviour is the original access point. If /sdcard/WIFI.CFG
 * holds ssid=... / password=... / mode=auto|sta|ap, station mode is
 * attempted first: mode auto falls back to the AP when the join does
 * not complete within FF_WIFI_JOIN_TIMEOUT_MS, mode sta keeps
 * retrying in the background. */
typedef struct {
    char ssid[32]; /* Wi-Fi SSIDs are at most 32 bytes */
    char pass[64]; /* WPA passphrases are at most 63 chars */
    char mode[8];  /* "auto" (default), "sta", "ap" */
} sta_cfg_t;

static EventGroupHandle_t s_wifi_evt;
#define WIFI_GOT_IP_BIT BIT0
/* True while a station join is wanted. The event handler re-issues
 * esp_wifi_connect() on every disconnect while this is set — without
 * that, one failed association ends all attempts (v16 behaviour:
 * Kris's log showed a single auth->assoc->init cycle at ~1.8 s and
 * then 12 s of silence before the fallback). Cleared before the AP
 * fallback stops the driver, so late disconnect events cannot
 * resurrect station mode. */
static bool s_sta_wanted;

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_sta_wanted) {
            wifi_event_sta_disconnected_t *d =
                (wifi_event_sta_disconnected_t *)data;
            ESP_LOGW(TAG, "home Wi-Fi disconnected (reason %d); "
                          "retrying", d ? d->reason : -1);
            /* v39: the old address is no longer ours — don't keep
             * showing it in the UI status or on the LCD. */
            ff_state_lock();
            bool had_ip = g_ff.wifi_ip[0] != 0;
            g_ff.wifi_ip[0] = 0;
            snprintf(g_ff.wifi_desc, sizeof(g_ff.wifi_desc),
                     "sta joining");
            if (had_ip) g_ff.lcd_dirty = true;
            ff_state_unlock();
            if (had_ip) ff_apply_kick(); /* repaint the footer */
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        char ip[16];
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&e->ip_info.ip));
        ff_state_lock();
        snprintf(g_ff.wifi_desc, sizeof(g_ff.wifi_desc), "sta %s", ip);
        snprintf(g_ff.wifi_ip, sizeof(g_ff.wifi_ip), "%s", ip);
        g_ff.lcd_dirty = true;
        ff_state_unlock();
        ff_apply_kick();
        ESP_LOGI(TAG, "joined home Wi-Fi, web UI at http://%s/", ip);
        xEventGroupSetBits(s_wifi_evt, WIFI_GOT_IP_BIT);
    }
}

/* Returns true when a usable station config was found. */
static bool read_sta_cfg(sta_cfg_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    strcpy(cfg->mode, "auto");
    FILE *f = fopen(FF_WIFI_CFG_PATH, "r");
    if (!f) return false;
    char line[192];
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = line, *val = eq + 1;
        ff_trim(key);
        ff_trim(val);
        if (strcasecmp(key, "ssid") == 0) {
            ff_copy_field(cfg->ssid, sizeof(cfg->ssid), val);
        } else if (strcasecmp(key, "password") == 0) {
            ff_copy_field(cfg->pass, sizeof(cfg->pass), val);
        } else if (strcasecmp(key, "mode") == 0) {
            ff_copy_field(cfg->mode, sizeof(cfg->mode), val);
        }
    }
    fclose(f);
    return cfg->ssid[0] && strcasecmp(cfg->mode, "ap") != 0;
}

/* POWER.CFG (v28): the Kasa strip that feeds the host machine, so
 * the web UI can power-cycle it after a swap. Same line format and
 * boot-window timing as WIFI.CFG; absent file = feature off. */
static void read_power_cfg(void)
{
    ff_power_cfg_t cfg = {0};
    FILE *f = fopen(FF_POWER_CFG_PATH, "r");
    if (!f) return;
    char line[192];
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = line, *val = eq + 1;
        ff_trim(key);
        ff_trim(val);
        if (strcasecmp(key, "host") == 0) {
            ff_copy_field(cfg.host, sizeof(cfg.host), val);
        } else if (strcasecmp(key, "port") == 0) {
            cfg.port = atoi(val);
        } else if (strcasecmp(key, "outlet") == 0) {
            cfg.outlet = atoi(val);
        } else if (strcasecmp(key, "delay") == 0) {
            cfg.delay_s = atoi(val);
        } else if (strcasecmp(key, "name") == 0) {
            ff_copy_field(cfg.name, sizeof(cfg.name), val);
        }
    }
    fclose(f);
    if (cfg.host[0]) ff_power_configure(&cfg);
}

static void wifi_start_ap(void)
{
    esp_netif_create_default_wifi_ap();
    wifi_config_t ap = {0};
    /* Copy SSID/password at runtime: .ap.ssid is a uint8_t array, and a
     * string-literal array initializer for it is not portable across IDF
     * versions / -Werror settings. */
    memcpy(ap.ap.ssid, FF_WIFI_SSID, sizeof(FF_WIFI_SSID) - 1);
    ap.ap.ssid_len = sizeof(FF_WIFI_SSID) - 1;
    memcpy(ap.ap.password, FF_WIFI_PASS, sizeof(FF_WIFI_PASS) - 1);
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());
    snprintf(g_ff.wifi_desc, sizeof(g_ff.wifi_desc), "ap 192.168.4.1");
    snprintf(g_ff.wifi_ssid, sizeof(g_ff.wifi_ssid), "%s", FF_WIFI_SSID);
    snprintf(g_ff.wifi_ip, sizeof(g_ff.wifi_ip), "192.168.4.1");
    ESP_LOGI(TAG, "Wi-Fi AP %s up, web UI at http://192.168.4.1/",
             FF_WIFI_SSID);
}

static void wifi_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    s_wifi_evt = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_handler_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));

    sta_cfg_t sc;
    if (read_sta_cfg(&sc)) {
        esp_netif_create_default_wifi_sta();
        wifi_config_t sta = {0};
        ff_copy_field((char *)sta.sta.ssid, sizeof(sta.sta.ssid), sc.ssid);
        ff_copy_field((char *)sta.sta.password, sizeof(sta.sta.password),
                   sc.pass);
        sta.sta.threshold.authmode =
            sc.pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        s_sta_wanted = true;
        snprintf(g_ff.wifi_ssid, sizeof(g_ff.wifi_ssid), "%s", sc.ssid);
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
        ESP_ERROR_CHECK(esp_wifi_start());
        ESP_LOGI(TAG, "joining home Wi-Fi '%s' (mode %s)...",
                 sc.ssid, sc.mode);
        EventBits_t bits = xEventGroupWaitBits(
            s_wifi_evt, WIFI_GOT_IP_BIT, pdFALSE, pdTRUE,
            pdMS_TO_TICKS(FF_WIFI_JOIN_TIMEOUT_MS));
        if (bits & WIFI_GOT_IP_BIT) return; /* desc set by the handler */
        if (strcasecmp(sc.mode, "sta") == 0) {
            ESP_LOGW(TAG, "not joined yet; mode=sta, so the driver "
                          "keeps retrying in the background");
            snprintf(g_ff.wifi_desc, sizeof(g_ff.wifi_desc),
                     "sta joining");
            return;
        }
        ESP_LOGW(TAG, "home Wi-Fi not joined in %d s; falling back "
                      "to the access point",
                 FF_WIFI_JOIN_TIMEOUT_MS / 1000);
        s_sta_wanted = false;
        ESP_ERROR_CHECK(esp_wifi_stop());
    }
    wifi_start_ap();
}

/* Apply task: the ONLY place that runs a local window. In the v15
 * whole-card design a window is:
 *  0. (v39) Wait for USB idle: no MSC write for FF_IDLE_WRITE_MS and
 *     no read for FF_IDLE_READ_MS — reads alone stop blocking after
 *     FF_SWAP_MAX_WAIT_MS. Skipped when the card is not with USB
 *     (ejected) or the user forced it (/api/force).
 *  1. Pull the card from USB and mount it locally (the storage
 *     component signals media removal; FlashFloppy sees the stick
 *     leave, exactly like Kris's physical bench test).
 *  2. Dwell (skipped for eject), so the removal registers before
 *     the card comes back.
 *  3. Refresh our view from the card (IMAGE_A.CFG may have been
 *     changed by FlashFloppy itself, e.g. a Gotek-side selection).
 *  4. On a select, write IMAGE_A.CFG naming the chosen image.
 *  5. Refresh again (the UI now lists what really landed), then hand
 *     the card back — FlashFloppy re-reads it and, with
 *     image-on-startup = last, mounts the named image. Eject skips
 *     the hand-back: the card stays locally mounted and absent —
 *     which since v37 is also the upload state, in which uploads
 *     stream straight to the card with no staging step here.
 * Failures land in g_ff.last_error, which the UI shows (v39). */
static TaskHandle_t s_apply_task;

void ff_apply_kick(void)
{
    if (s_apply_task) xTaskNotifyGive(s_apply_task);
}

/* True when the queued request may start now. */
static bool usb_quiet_enough(void)
{
    ff_state_lock();
    bool force = g_ff.force_swap;
    bool with_usb = g_ff.presented;
    uint32_t since = g_ff.pending_since_ms;
    ff_state_unlock();
    if (force || !with_usb) return true;
    uint32_t now = ff_now_ms();
    bool writes_quiet = now - g_ff.last_msc_write_ms >= FF_IDLE_WRITE_MS;
    bool reads_quiet = now - g_ff.last_msc_read_ms >= FF_IDLE_READ_MS;
    bool waited = now - since >= FF_SWAP_MAX_WAIT_MS;
    /* Writes are never interrupted unless forced: a cut-off write
     * is how an image gets corrupted. A steady trickle of reads
     * (FlashFloppy/host polling) only delays the swap up to
     * FF_SWAP_MAX_WAIT_MS — reads are safe to interrupt. */
    return writes_quiet && (reads_quiet || waited);
}

static void set_idle(void)
{
    ff_state_lock();
    g_ff.state = FF_STATE_IDLE;
    g_ff.gen++;
    ff_state_unlock();
}

static void apply_task(void *arg)
{
    (void)arg;
    bool waiting = false;
    for (;;) {
        /* Woken by ff_apply_kick(); while holding a request back
         * for USB activity, re-check every 100 ms. */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(waiting ? 100 : 1000));

        ff_state_lock();
        bool repaint = g_ff.lcd_dirty;
        bool pending_req = g_ff.state == FF_STATE_SWAP_PENDING;
        ff_state_unlock();
        if (repaint) {
            ff_lcd_show_status(g_ff.presented ? "Presented" : "Ejected",
                               NULL);
        }
        if (!pending_req) {
            waiting = false;
            continue;
        }
        if (!usb_quiet_enough()) {
            if (!waiting) {
                ESP_LOGI(TAG, "request queued: waiting for the USB "
                              "host to go idle");
            }
            waiting = true;
            ff_state_lock();
            g_ff.waiting_usb = true;
            ff_state_unlock();
            continue;
        }
        waiting = false;

        ff_state_lock();
        if (g_ff.state != FF_STATE_SWAP_PENDING) { /* raced away */
            ff_state_unlock();
            continue;
        }
        char pending[256];
        snprintf(pending, sizeof(pending), "%s", g_ff.pending_path);
        bool eject = g_ff.eject_requested;
        bool restart = g_ff.restart_after_swap;
        bool forced = g_ff.force_swap;
        g_ff.pending_path[0] = 0;
        g_ff.eject_requested = false;
        g_ff.restart_after_swap = false;
        g_ff.force_swap = false;
        g_ff.waiting_usb = false;
        g_ff.state = FF_STATE_DISCONNECTED;
        g_ff.gen++; /* invalidates any peek in flight */
        ff_state_unlock();
        if (forced) ESP_LOGW(TAG, "USB idle wait skipped (forced)");

        if (ff_sdcard_local_begin() != ESP_OK) {
            set_idle();
            ff_state_set_error("Could not take the card from USB "
                               "(local mount failed); nothing changed");
            continue;
        }
        ff_state_lock();
        g_ff.state = FF_STATE_APPLYING;
        ff_state_unlock();

        if (!eject) {
            /* The dwell lets a swap's removal register before the
             * card comes back; an eject never comes back, so it
             * skips the wait (v38: eject felt very slow). */
            vTaskDelay(pdMS_TO_TICKS(FF_DISCONNECT_DWELL_MS));
        }

        ff_store_refresh();
        if (!eject && pending[0]) {
            if (ff_store_write_image_a(pending) != ESP_OK) {
                ff_state_set_error("Writing IMAGE_A.CFG for %.100s "
                                   "failed; the previous image stays "
                                   "selected", pending);
                restart = false; /* don't reboot into the old image */
            }
            ff_store_refresh();
        }

        if (eject) {
            set_idle();
            ESP_LOGI(TAG, "ejected: card stays unmounted until the "
                          "next select");
            ff_lcd_show_status("Ejected", NULL);
            continue;
        }
        /* Hand back, with a couple of retries (v39): a failure here
         * used to leave the card invisible to the Gotek with nothing
         * in the UI. */
        esp_err_t end_err = ESP_FAIL;
        for (int attempt = 0; attempt < 3 && end_err != ESP_OK;
             attempt++) {
            if (attempt) vTaskDelay(pdMS_TO_TICKS(500));
            end_err = ff_sdcard_local_end();
        }
        if (end_err == ESP_OK) {
            char cur[256];
            ff_state_lock();
            g_ff.state = FF_STATE_PRESENTED;
            g_ff.gen++;
            snprintf(cur, sizeof(cur), "%s", g_ff.current_path);
            ff_state_unlock();
            ESP_LOGI(TAG, "card re-inserted; FlashFloppy should mount "
                          "%s", cur);
            ff_lcd_show_status("Presented", cur);
            if (restart) {
                /* The point of the power feature: swap the image,
                 * then bounce the host so it boots the new one. */
                ESP_LOGI(TAG, "restart-after-swap requested");
                ff_power_restart_async();
            }
        } else {
            set_idle(); /* card still local: same as ejected */
            ff_state_set_error("Could not hand the card back to the "
                               "Gotek; the drive is ejected - press "
                               "Re-insert to try again");
            ff_lcd_show_status("Ejected", NULL);
        }
    }
}

/* Peek task (v26): while the card is with USB, poll IMAGE_A.CFG
 * by raw sectors every few seconds so a selection made on the
 * Gotek's own buttons — which makes FlashFloppy rewrite the file
 * itself — is learned without a local window: update the shared
 * current image, the NVS last-used record, and the LCD. A changed
 * sample is only believed after a second identical read 200 ms
 * later (the button-side rewrite caught mid-update can tear).
 * v39: the result is also dropped if a local window started or
 * ended while the reads were in flight (g_ff.gen changed) — before,
 * a stale sample could overwrite a selection the apply task had just
 * written. Read-only throughout: this task can never write the card. */
static void peek_task(void *arg)
{
    (void)arg;
    char name[256], confirm[256];
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ff_state_lock();
        bool ours = g_ff.presented &&
                    (g_ff.state == FF_STATE_PRESENTED ||
                     g_ff.state == FF_STATE_IDLE);
        uint32_t gen = g_ff.gen;
        ff_state_unlock();
        if (!ours) continue;
        if (ff_store_peek_image_a(name, sizeof(name)) != ESP_OK) {
            continue;
        }
        ff_state_lock();
        bool changed = strcmp(name, g_ff.current_path) != 0;
        ff_state_unlock();
        if (!changed) continue;
        vTaskDelay(pdMS_TO_TICKS(200));
        if (ff_store_peek_image_a(confirm, sizeof(confirm)) !=
                ESP_OK ||
            strcmp(name, confirm) != 0) {
            continue;
        }
        ff_state_lock();
        bool still_valid = g_ff.gen == gen && g_ff.presented &&
                           (g_ff.state == FF_STATE_PRESENTED ||
                            g_ff.state == FF_STATE_IDLE);
        if (still_valid) {
            snprintf(g_ff.current_path, sizeof(g_ff.current_path),
                     "%s", name);
        }
        ff_state_unlock();
        if (!still_valid) continue;
        if (name[0]) ff_nvs_save_last(name);
        ESP_LOGI(TAG, "IMAGE_A.CFG changed on the card: now names "
                      "%s (Gotek-side selection?)",
                 name[0] ? name : "(none)");
        ff_lcd_show_status("Presented", name);
    }
}

void app_main(void)
{
    ff_state_init();
    ESP_ERROR_CHECK(nvs_flash_init());

    /* Card + USB: register the whole card with the TinyUSB MSC
     * storage component, install the driver, then run the boot local
     * window while the card is still ours: prepare configs, apply
     * the auto-present record, and take the first snapshot. The
     * window stays open across Wi-Fi init so WIFI.CFG can be read
     * from the card, and closes before the apply task starts. */
    bool card_ok = ff_sdcard_init() == ESP_OK;
    if (card_ok) {
        ff_usb_install();
        if (ff_sdcard_local_begin() == ESP_OK) {
            ff_store_prepare();
            read_power_cfg(); /* Kasa strip config, if any (v28) */
            /* Auto-present (carried over from v12, now trivial):
             * the NVS record names the last used image; if it is
             * still on the card, make IMAGE_A.CFG name it — that is
             * all "presenting" means in the whole-card design.
             * Without a record, IMAGE_A.CFG stays whatever
             * FlashFloppy itself last wrote, which is the same
             * thing from its point of view. */
            char last[256] = {0};
            if (ff_nvs_load_last(last, sizeof(last)) == ESP_OK &&
                last[0]) {
                char path[420];
                snprintf(path, sizeof(path), "%s/%s",
                         FF_SD_MOUNT_POINT, last);
                struct stat st;
                if (stat(path, &st) == 0) {
                    ff_store_write_image_a(last);
                    ESP_LOGI(TAG, "auto-present: IMAGE_A.CFG "
                                  "names %s", last);
                } else {
                    ESP_LOGW(TAG, "last used file %s is gone; "
                                  "leaving IMAGE_A.CFG as-is",
                             last);
                }
            }
            ff_store_refresh();
        }
    } else {
        ESP_LOGE(TAG, "no SD card: web UI comes up, storage does not");
    }
    ff_lcd_init();
    wifi_init();
    /* Repaint now that the SSID/IP footer has real values. */
    ff_lcd_show_status("Presented", NULL);
    ff_http_start();
    if (card_ok) {
        ff_sdcard_local_end(); /* hand the card to the Gotek */
    }
    /* 8192: the apply task's call chain (storage refresh, stdio
     * over FatFs) needs the same headroom the swap task did —
     * 4096 overflowed twice in this project's history. */
    xTaskCreate(apply_task, "ff_apply", 8192, NULL, 5, &s_apply_task);
    if (card_ok) {
        /* Watches for Gotek-side selections via the sector peek. */
        xTaskCreate(peek_task, "ff_peek", 4096, NULL, 4, NULL);
    }
    /* v36: keep the Kasa outlet's state + watts fresh in the
     * background; v39: plus the command worker (no-op when
     * POWER.CFG is absent). */
    ff_power_start();
    ESP_LOGI(TAG, "FlashFloppy ESP32 frontend up (whole-card design)");
}
