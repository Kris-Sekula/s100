#pragma once

/* Waveshare ESP32-S3-LCD-1.47 (USB-A dongle, non-touch variant).
 * Pins verified against the Waveshare schematic 2026-10-28.
 * All board-specific numbers live here; no other file hard-codes a pin. */

/* Native USB to the Gotek: ESP32-S3 USB peripheral, GPIO19 = D-, GPIO20 = D+ */
#define BOARD_USB_DM_GPIO 19
#define BOARD_USB_DP_GPIO 20

/* SD card, SDMMC 4-bit mode (NOT SPI mode) */
#define BOARD_SD_CLK_GPIO 14
#define BOARD_SD_CMD_GPIO 15
#define BOARD_SD_D0_GPIO  16
#define BOARD_SD_D1_GPIO  18
#define BOARD_SD_D2_GPIO  17
#define BOARD_SD_D3_GPIO  21

/* Built-in 1.47" LCD, SPI. Optional: the MSC path does not depend on it. */
#define BOARD_LCD_RST_GPIO 39
#define BOARD_LCD_CLK_GPIO 40
#define BOARD_LCD_DC_GPIO  41
#define BOARD_LCD_CS_GPIO  42
#define BOARD_LCD_DIN_GPIO 45
#define BOARD_LCD_BL_GPIO  48

/* Header UART, for logs while native USB is the Gotek link */
#define BOARD_UART_RX_GPIO 2
#define BOARD_UART_TX_GPIO 3

#define BOARD_RGB_GPIO 38

/* BOOT button (v42): the board's strapping button on GPIO0. The
 * ROM samples it only at reset; while the app runs it is a free
 * input (button to GND, internal pull-up). Do not hold it while
 * powering up or resetting — that boots the download loader
 * instead of the app. Polled by the button task (button.c). */
#define BOARD_BOOT_GPIO 0

/* Paths on the SD card (mounted at /sdcard by the storage layer
 * during local windows; while the card is exposed to USB the ESP32
 * must not touch these paths at all — v15 whole-card design).
 * Everything lives in the root, exactly like a normal FlashFloppy
 * stick (v18: the /library folder was a container-design leftover;
 * images, configs and IMAGE_A.CFG all sit in the root). */
#define FF_SD_MOUNT_POINT "/sdcard"
#define FF_IMAGE_A_PATH   "/sdcard/IMAGE_A.CFG"
#define FF_FF_CFG_PATH    "/sdcard/FF.CFG"
#define FF_IMG_CFG_PATH   "/sdcard/IMG.CFG"

/* Uploads (v37): there is no staging area any more. Uploads stream
 * straight to the card while the drive is EJECTED (the card is
 * locally mounted then, so the ESP32 is its sole owner) — any file
 * size the card can hold. The v15–v36 PSRAM staging buffer and its
 * 7 MB / 4-file cap are gone. */

/* Swap idle thresholds from the 2026-10-27 design (heuristics: the ESP32
 * sees USB traffic, not the floppy bus). Swap only when the host is idle,
 * and reset the host after swapping a boot disk. Enforced by the apply
 * task since v39, from MSC read/write stamps (sdcard.c); they were
 * defined but unused in v15–v38. */
#define FF_IDLE_WRITE_MS 5000
#define FF_IDLE_READ_MS  2000
#define FF_DISCONNECT_DWELL_MS 750 /* bench-verify: 500-1000 ms range */
/* A swap waits for full idle (writes AND reads quiet), but reads alone
 * must not block it forever: with a Gotek attached, FlashFloppy or the
 * host can generate a steady trickle of reads indefinitely (v11 sat in
 * swap_pending permanently on Kris's bench). After this long with
 * writes quiet, the swap proceeds anyway — reads are safe to interrupt,
 * the host just re-reads. The UI's Force button skips the wait at once. */
#define FF_SWAP_MAX_WAIT_MS 6000 /* 15 s in v12/v13; 6 s from v14 (Kris's pick) */

/* How long station mode waits for the home network at boot before
 * mode=auto falls back to the access point. 30 s since v17: Kris's
 * router sometimes needs several association attempts ("gives up
 * too quickly" at the original 12 s) — and until v17 the event
 * handler never re-issued esp_wifi_connect() after a failed
 * attempt, so the window was mostly spent idle. The cost of a long
 * window is only paid when the network is truly absent. */
#define FF_WIFI_JOIN_TIMEOUT_MS 30000

/* Wi-Fi: access point by default (below). If /sdcard/WIFI.CFG exists
 * with ssid=... (and password=..., mode=auto|sta|ap), the device joins
 * that network instead — mode auto falls back to this AP if the join
 * fails, mode sta keeps retrying, mode ap ignores the ssid. The file
 * lives on the SD card precisely so credentials survive code updates
 * (the update script replaces main/ wholesale). Plaintext on the
 * card, like every embedded gadget; LAN-only either way. */
#define FF_WIFI_CFG_PATH "/sdcard/WIFI.CFG"
/* Optional Kasa power-strip config (v28): host=/port=/outlet=/
 * delay=/name= lines; parsed by main.c during the boot window. */
#define FF_POWER_CFG_PATH "/sdcard/POWER.CFG"
/* How often the power module re-reads the outlet (state + watts)
 * in the background (v36, Kris's ask: catch flips made elsewhere). */
#define FF_POWER_POLL_MS 5000

/* BOOT button behaviour (v42, FlashFloppy-style browsing): a press
 * previews the next image on the LCD; the preview commits through
 * the normal select path after FF_PREVIEW_COMMIT_MS with no
 * further press. Holding the button auto-repeats after
 * FF_BUTTON_HOLD_MS. The LCD blanks after FF_LCD_SCREENSAVER_MS
 * with no activity (button press, selection change, swap) and any
 * press wakes it; 0 disables the screensaver. Polled, not
 * interrupt-driven: the poll task also runs the commit timer and
 * the screensaver, and 20 ms granularity is plenty for a button. */
#define FF_BUTTON_POLL_MS 20
#define FF_BUTTON_DEBOUNCE_MS 30
#define FF_BUTTON_HOLD_MS 600
#define FF_BUTTON_REPEAT_MS 150
#define FF_PREVIEW_COMMIT_MS 2000
#define FF_LCD_SCREENSAVER_MS 120000
#define FF_WIFI_SSID "FlashFloppy"
#define FF_WIFI_PASS "flashfloppy"
