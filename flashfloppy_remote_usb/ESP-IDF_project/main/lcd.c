#include "lcd.h"
#include "board.h"
#include "state.h"
#include "font.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/* ST7789 status display (v23 — the real driver; this file was a
 * stub through v22). The Waveshare ESP32-S3-LCD-1.47 panel is
 * 172x320 portrait in a 240-wide ST7789 RAM, hence the +34 column
 * gap. Shows what Kris asked for: the selected file name (large),
 * the Wi-Fi SSID and the IP address (footer).
 *
 * Two things can only be confirmed on the real panel, both
 * one-line flips marked PANEL-TWEAK below: colour inversion
 * (these IPS panels normally want it ON) and RGB vs BGR element
 * order. If the first light-up looks wrong, flip and rebuild.
 * Orientation is the panel's natural portrait; no rotation. */

static const char *TAG = "ff_lcd";

#define LCD_W 172
#define LCD_H 320
#define LCD_GAP_X 34 /* (240 - 172) / 2 */
#define LCD_GAP_Y 0

#define COL_BG 0x0813     /* deep navy */
#define COL_HDR 0xfbe0    /* amber */
#define COL_NAME 0xffff   /* white */
#define COL_DIM 0x9d13    /* grey-blue */
#define COL_NET 0x07ff    /* cyan */

static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_fb; /* PSRAM framebuffer, RGB565 byte-swapped */
/* v39: render() is called from the apply task, the peek task and
 * app_main — the v26 peek task made "never concurrently" untrue.
 * s_lcd_lock serialises whole renders; s_flush_done lets a render
 * wait until the previous frame's SPI DMA has finished reading
 * s_fb before the framebuffer is overwritten. */
static SemaphoreHandle_t s_lcd_lock;
static SemaphoreHandle_t s_flush_done;
static bool s_blanked; /* screensaver state (v42) */

static bool flush_done_cb(esp_lcd_panel_io_handle_t io,
                          esp_lcd_panel_io_event_data_t *edata,
                          void *ctx)
{
    (void)io;
    (void)edata;
    (void)ctx;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_flush_done, &woken);
    return woken == pdTRUE;
}

static void fb_fill(uint16_t col565)
{
    uint16_t px = (uint16_t)((col565 >> 8) | (col565 << 8));
    for (int i = 0; i < LCD_W * LCD_H; i++) s_fb[i] = px;
}

static void fb_rect(int x, int y, int w, int h, uint16_t col565)
{
    uint16_t px = (uint16_t)((col565 >> 8) | (col565 << 8));
    for (int yy = y; yy < y + h && yy < LCD_H; yy++) {
        if (yy < 0) continue;
        for (int xx = x; xx < x + w && xx < LCD_W; xx++) {
            if (xx < 0) continue;
            s_fb[yy * LCD_W + xx] = px;
        }
    }
}

/* Draw text with character wrapping at max_w pixels. Returns the y
 * just past the last line. Glyphs outside ASCII 32..126 render as
 * '?'. Transparent background (the frame is pre-filled). */
static int fb_text(int x, int y, const char *s, int scale,
                   uint16_t col565, int max_w)
{
    int cx = x, cy = y;
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 32 || c > 126) c = '?';
        if (cx + 10 * scale > x + max_w) { cx = x; cy += 18 * scale + 2; }
        const uint8_t *g = ff_font_10x18[c - 32];
        for (int row = 0; row < 18; row++) {
            for (int col = 0; col < 10; col++) {
                uint8_t byte = g[row * 2 + (col >> 3)];
                if (byte & (0x80 >> (col & 7))) {
                    fb_rect(cx + col * scale, cy + row * scale,
                            scale, scale, col565);
                }
            }
        }
        cx += 10 * scale;
    }
    return cy + 18 * scale;
}

static void render(const char *title, const char *name)
{
    if (!s_panel || !s_fb || !s_lcd_lock) return;
    /* Copy the shared strings under the state lock first; callers
     * never hold it when they ask for a repaint. */
    char cur[256], ssid[33], ip[16];
    ff_state_lock();
    snprintf(cur, sizeof(cur), "%s", g_ff.current_path);
    snprintf(ssid, sizeof(ssid), "%s", g_ff.wifi_ssid);
    snprintf(ip, sizeof(ip), "%s", g_ff.wifi_ip);
    g_ff.lcd_dirty = false;
    ff_state_unlock();

    xSemaphoreTake(s_lcd_lock, portMAX_DELAY);
    /* Previous frame still being clocked out? Wait for it. */
    xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(200));
    fb_fill(COL_BG);
    fb_text(6, 6, "FLASHFLOPPY", 1, COL_HDR, LCD_W - 12);
    fb_rect(6, 30, LCD_W - 12, 1, COL_DIM);
    fb_text(6, 40, title && title[0] ? title : "Presented", 1,
            COL_DIM, LCD_W - 12);
    const char *nm = name && name[0] ? name : cur[0] ? cur : "(none)";
    fb_text(6, 64, nm, 2, COL_NAME, LCD_W - 12);
    fb_rect(6, 246, LCD_W - 12, 1, COL_DIM);
    int y = 258;
    if (ssid[0]) {
        char line[48];
        snprintf(line, sizeof(line), "SSID: %s", ssid);
        y = fb_text(6, y, line, 1, COL_NET, LCD_W - 12);
        snprintf(line, sizeof(line), "IP: %s",
                 ip[0] ? ip : "joining...");
        fb_text(6, y + 6, line, 1, COL_NET, LCD_W - 12);
    } else {
        fb_text(6, y, "WiFi: starting", 1, COL_NET, LCD_W - 12);
    }
    if (esp_lcd_panel_draw_bitmap(s_panel, 0, 0, LCD_W, LCD_H, s_fb) !=
        ESP_OK) {
        xSemaphoreGive(s_flush_done); /* nothing in flight */
    }
    xSemaphoreGive(s_lcd_lock);
}

esp_err_t ff_lcd_init(void)
{
    s_fb = heap_caps_malloc(LCD_W * LCD_H * sizeof(uint16_t),
                            MALLOC_CAP_SPIRAM);
    if (!s_fb) {
        ESP_LOGE(TAG, "no PSRAM for the LCD framebuffer");
        return ESP_ERR_NO_MEM;
    }
    const spi_bus_config_t bus = {
        .sclk_io_num = BOARD_LCD_CLK_GPIO,
        .mosi_io_num = BOARD_LCD_DIN_GPIO,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_W * LCD_H * sizeof(uint16_t),
    };
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus,
                                       SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(err));
        return err;
    }
    s_lcd_lock = xSemaphoreCreateMutex();
    s_flush_done = xSemaphoreCreateBinary();
    if (!s_lcd_lock || !s_flush_done) return ESP_ERR_NO_MEM;
    xSemaphoreGive(s_flush_done); /* no frame in flight yet */
    esp_lcd_panel_io_handle_t io;
    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = BOARD_LCD_CS_GPIO,
        .dc_gpio_num = BOARD_LCD_DC_GPIO,
        .spi_mode = 0,
        .pclk_hz = 40 * 1000 * 1000,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .on_color_trans_done = flush_done_cb,
    };
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST,
                                   &io_cfg, &io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel IO failed: %s", esp_err_to_name(err));
        return err;
    }
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = BOARD_LCD_RST_GPIO,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB, /* PANEL-TWEAK */
        .bits_per_pixel = 16,
    };
    err = esp_lcd_new_panel_st7789(io, &panel_cfg, &s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ST7789 panel failed: %s", esp_err_to_name(err));
        return err;
    }
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_set_gap(s_panel, LCD_GAP_X, LCD_GAP_Y);
    /* 180-degree rotation (Kris, 2026-10-30): mirror both axes.
     * The visible window stays put because the offsets are
     * symmetric on this panel: 34 + 172 + 34 = 240 columns, and
     * the full 320 rows are used. */
    esp_lcd_panel_mirror(s_panel, true, true);
    esp_lcd_panel_invert_color(s_panel, true); /* PANEL-TWEAK */
    esp_lcd_panel_disp_on_off(s_panel, true);
    /* Backlight: plain GPIO on this board (board.h pin map). */
    gpio_config_t bl = {
        .pin_bit_mask = 1ULL << BOARD_LCD_BL_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&bl);
    gpio_set_level(BOARD_LCD_BL_GPIO, 1);
    ESP_LOGI(TAG, "LCD up (%dx%d ST7789)", LCD_W, LCD_H);
    render("FlashFloppy", "starting...");
    return ESP_OK;
}

void ff_lcd_show_status(const char *line1, const char *line2)
{
    /* Full-screen re-render: title + big file name + the Wi-Fi
     * footer from g_ff. Safe from any task since v39 (see
     * s_lcd_lock); must not be called with ff_state_lock held. */
    render(line1, line2);
}

void ff_lcd_set_blank(bool blank)
{
    if (!s_panel || !s_lcd_lock || blank == s_blanked) return;
    /* Serialised with render(): a frame may be mid-DMA. */
    xSemaphoreTake(s_lcd_lock, portMAX_DELAY);
    s_blanked = blank;
    if (blank) {
        esp_lcd_panel_disp_on_off(s_panel, false);
        gpio_set_level(BOARD_LCD_BL_GPIO, 0);
    } else {
        esp_lcd_panel_disp_on_off(s_panel, true);
        gpio_set_level(BOARD_LCD_BL_GPIO, 1);
    }
    xSemaphoreGive(s_lcd_lock);
}

bool ff_lcd_is_blanked(void)
{
    return s_blanked;
}
