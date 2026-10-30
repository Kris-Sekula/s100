#pragma once
#include "esp_err.h"

/* ST7789 status display (driver since v23; a no-op stub before).
 * Pins are in board.h (RST 39, CLK 40, DC 41, CS 42, DIN 45, BL 48).
 * ff_lcd_show_status(title, name) re-renders the whole screen: the
 * title small, the selected file name large, and a footer with the
 * Wi-Fi SSID and IP address from g_ff (wifi_ssid / wifi_ip). */
esp_err_t ff_lcd_init(void);
void ff_lcd_show_status(const char *line1, const char *line2);
