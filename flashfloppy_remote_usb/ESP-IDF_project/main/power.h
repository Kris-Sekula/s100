#pragma once

#include <stdbool.h>
#include "esp_err.h"

/* TP-Link Kasa power control (v28) — Kris's host machine hangs off
 * one outlet of a Kasa strip (HS300 or similar), so after a swap
 * he can power-cycle the machine from the same web UI and re-test
 * the new image without leaving the page.
 *
 * Protocol: the Kasa local API — TCP port 9999, JSON commands
 * framed with a 4-byte big-endian length and obfuscated with the
 * well-known autokey XOR (initial key 171). Per-outlet control on
 * a strip goes through a "context" naming the child (outlet) id,
 * resolved once from get_sysinfo and cached. A single-socket
 * device (HS100/HS110 style, no children in sysinfo) is handled
 * too: commands then carry no context.
 *
 * Configuration comes from /sdcard/POWER.CFG (parsed by main.c
 * during the boot window, like WIFI.CFG) via ff_power_configure():
 *   host=192.168.0.60     (required; IP or hostname of the strip)
 *   port=9999             (optional)
 *   outlet=1              (1-based outlet index on a strip;
 *                          ignored for single-socket devices)
 *   delay=5               (restart: seconds between off and on)
 *   name=Machine          (label shown in the UI)
 * Nothing here ever touches the SD card: the module is pure LAN. */

typedef struct {
    char host[64];
    int port;        /* 0 -> 9999 */
    int outlet;      /* 1-based; 0 -> 1 */
    int delay_s;     /* 0 -> 5 */
    char name[32];   /* "" -> "Machine" */
} ff_power_cfg_t;

void ff_power_configure(const ff_power_cfg_t *cfg);
bool ff_power_configured(void);
const char *ff_power_name(void);

/* Live query of the configured outlet's relay state. */
esp_err_t ff_power_query(bool *on_out);
esp_err_t ff_power_set(bool on);
/* Background polling (v36): after ff_power_start_polling(), a
 * task re-reads the outlet every FF_POWER_POLL_MS — relay state
 * AND emeter watts — so flips made outside this UI (Kasa app,
 * another client, the physical button) show up on their own.
 * HTTP serves the cache, not the device. ff_power_cached_state()
 * returns false until the first successful poll lands. */
void ff_power_start_polling(void);
bool ff_power_cached_state(bool *on_out, float *watts_out,
                           bool *reachable_out);
/* Off, wait the configured delay, on — in a background task, so
 * HTTP handlers and swap completion never block on it. A second
 * request while one is running is ignored. */
void ff_power_restart_async(void);
bool ff_power_restarting(void);
