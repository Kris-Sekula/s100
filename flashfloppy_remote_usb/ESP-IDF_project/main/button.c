#include "button.h"
#include "board.h"
#include "state.h"
#include "storage.h"
#include "lcd.h"
#include "http_server.h"
#include "util.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ff_btn";

/* Everything below belongs to the button task alone; shared state
 * is only ever read (or, for a commit, requested) through g_ff. */
static bool s_browsing;
static char s_candidate[256]; /* previewed image (bare filename) */
static uint32_t s_last_step_ms;
static char s_last_current[256];
static ff_state_t s_last_state;
static uint32_t s_last_activity_ms;

/* (Re)paint the normal status screen. Lock is never held across
 * the LCD call (render() takes it internally for its snapshot). */
static void show_normal(void)
{
    ff_state_lock();
    bool presented = g_ff.presented;
    ff_state_unlock();
    ff_lcd_show_status(presented ? "Presented" : "Ejected", NULL);
}

static void cancel_browse(bool repaint)
{
    if (!s_browsing) return;
    s_browsing = false;
    s_candidate[0] = 0;
    if (repaint) show_normal();
}

/* Step the preview to the next image after the current base —
 * the previewed candidate while browsing, else the presented
 * image. Images only, in snapshot order (the order the web UI
 * lists them), wrapping at the end. False when the card holds no
 * images at all. */
static bool step_next(uint32_t now)
{
    char name[256];
    int total = 0, target = 0;
    ff_state_lock();
    const char *base = s_browsing && s_candidate[0]
                           ? s_candidate
                           : g_ff.current_path;
    int base_idx = -1, count = 0;
    for (int i = 0; i < ff_store_count(); i++) {
        const ff_entry_t *en = ff_store_entry(i);
        if (!en || en->kind != FF_KIND_IMAGE) continue;
        if (strcmp(en->name, base) == 0) base_idx = count;
        count++;
    }
    total = count;
    name[0] = 0;
    if (total > 0) {
        target = (base_idx + 1) % total;
        int seen = -1;
        for (int i = 0; i < ff_store_count(); i++) {
            const ff_entry_t *en = ff_store_entry(i);
            if (!en || en->kind != FF_KIND_IMAGE) continue;
            if (++seen == target) {
                snprintf(name, sizeof(name), "%s", en->name);
                break;
            }
        }
    }
    ff_state_unlock();
    if (!name[0]) return false;
    snprintf(s_candidate, sizeof(s_candidate), "%s", name);
    s_browsing = true;
    s_last_step_ms = now;
    char title[24];
    snprintf(title, sizeof(title), "Preview %d/%d", target + 1,
             total);
    ff_lcd_show_status(title, s_candidate);
    ESP_LOGI(TAG, "preview %d/%d: %s", target + 1, total,
             s_candidate);
    return true;
}

/* The preview went idle for FF_PREVIEW_COMMIT_MS: apply it
 * through the same request path as POST /api/select. */
static void commit_preview(void)
{
    char cand[256];
    snprintf(cand, sizeof(cand), "%s", s_candidate);
    s_browsing = false;
    s_candidate[0] = 0;
    if (!ff_store_has_image(cand)) {
        ff_state_set_error("Previewed image %.100s is no longer on "
                           "the card; selection unchanged", cand);
        show_normal();
        return;
    }
    if (ff_request_select(cand)) {
        ESP_LOGI(TAG, "preview committed: selecting %s", cand);
    } else {
        /* Raced with a web request or an upload claim: that
         * request wins, the preview just ends. */
        show_normal();
    }
}

static void button_task(void *arg)
{
    (void)arg;
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOARD_BOOT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    ff_state_lock();
    snprintf(s_last_current, sizeof(s_last_current), "%s",
             g_ff.current_path);
    s_last_state = g_ff.state;
    ff_state_unlock();
    s_last_activity_ms = ff_now_ms();

    int raw = 1; /* last sampled level; 1 = released (pull-up) */
    uint32_t raw_since = s_last_activity_ms;
    bool down = false;     /* debounced pressed state */
    bool consumed = false; /* this press only woke the screen */
    uint32_t down_since = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(FF_BUTTON_POLL_MS));
        uint32_t now = ff_now_ms();

        /* --- shared-state changes: activity, wake, cancel --- */
        ff_state_lock();
        ff_state_t st = g_ff.state;
        bool presented = g_ff.presented;
        bool uploading = g_ff.upload_active;
        char cur[256];
        snprintf(cur, sizeof(cur), "%s", g_ff.current_path);
        ff_state_unlock();
        /* Resting = the card is settled with the Gotek (IDLE, or
         * PRESENTED, which is where the apply task leaves it after
         * a successful hand-back). */
        bool resting = st == FF_STATE_IDLE ||
                       st == FF_STATE_PRESENTED;
        bool cur_changed = strcmp(cur, s_last_current) != 0;
        bool st_changed = st != s_last_state;
        if (cur_changed || st_changed) {
            s_last_activity_ms = now;
            /* Whoever changed the selection already rendered the
             * new frame; unblanking reveals it. */
            if (ff_lcd_is_blanked()) ff_lcd_set_blank(false);
            snprintf(s_last_current, sizeof(s_last_current), "%s",
                     cur);
            s_last_state = st;
            if (s_browsing &&
                (cur_changed || st == FF_STATE_SWAP_PENDING ||
                 st == FF_STATE_DISCONNECTED ||
                 st == FF_STATE_APPLYING)) {
                /* A Gotek-side change (learned by the peek task),
                 * a web request, or a window starting: the preview
                 * is stale — the other actor wins. */
                cancel_browse(true);
            }
        }

        /* --- debounce: an edge counts once the level has been
         * stable for FF_BUTTON_DEBOUNCE_MS --- */
        int level = gpio_get_level(BOARD_BOOT_GPIO);
        if (level != raw) {
            raw = level;
            raw_since = now;
        }
        bool pressed_edge = false, released_edge = false;
        if (raw != (down ? 0 : 1) &&
            now - raw_since >= FF_BUTTON_DEBOUNCE_MS) {
            down = raw == 0;
            if (down) pressed_edge = true;
            else released_edge = true;
        }

        if (pressed_edge) {
            s_last_activity_ms = now;
            down_since = now;
            consumed = false;
            if (ff_lcd_is_blanked()) {
                /* Wake only: the press is spent on the wake and
                 * must not also step the preview. */
                ff_lcd_set_blank(false);
                show_normal();
                consumed = true;
            } else if (presented && resting && !uploading) {
                step_next(now);
            }
            /* Ejected or busy: the press counts as activity only
             * (v42: no browsing while ejected). */
        }
        /* Hold-to-repeat: first repeat once the button has been
         * down FF_BUTTON_HOLD_MS, then every FF_BUTTON_REPEAT_MS
         * (s_last_step_ms tracks the most recent step). */
        if (down && !consumed && s_browsing &&
            now - down_since >= FF_BUTTON_HOLD_MS &&
            now - s_last_step_ms >= FF_BUTTON_REPEAT_MS &&
            presented && resting && !uploading) {
            step_next(now);
        }
        if (released_edge) consumed = false;

        /* --- commit an idle preview --- */
        if (s_browsing &&
            now - s_last_step_ms >= FF_PREVIEW_COMMIT_MS) {
            if (presented && resting && !uploading) {
                commit_preview();
            } else {
                cancel_browse(true);
            }
        }

        /* --- screensaver --- */
        if (FF_LCD_SCREENSAVER_MS > 0 && !ff_lcd_is_blanked() &&
            now - s_last_activity_ms >= FF_LCD_SCREENSAVER_MS) {
            ff_lcd_set_blank(true);
        }
    }
}

void ff_button_start(void)
{
    /* 4096: the renders this task triggers carry a few hundred
     * bytes of line buffers on the caller's stack — the peek task
     * runs the same class of call chain on 4096. */
    xTaskCreate(button_task, "ff_btn", 4096, NULL, 4, NULL);
}
