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

/* v39: nothing outside this module talks to the device directly.
 * A worker task executes on/off/restart from a queue and a poll task
 * keeps a cache of relay state + watts; HTTP handlers only queue
 * commands and read the cache, so a slow or absent strip can never
 * stall the web server. All device exchanges have hard timeouts
 * (connect included). */
typedef enum { FF_POWER_ON, FF_POWER_OFF, FF_POWER_RESTART } ff_power_cmd_t;

/* Start the worker and the poller (no-op when not configured). */
void ff_power_start(void);

/* Queue a command. ESP_ERR_INVALID_STATE when not configured or a
 * command is already queued/running (the UI shows it as busy). */
esp_err_t ff_power_request(ff_power_cmd_t cmd);

/* Restart (off, delay, on) after a swap; same as
 * ff_power_request(FF_POWER_RESTART), result ignored. */
void ff_power_restart_async(void);

typedef struct {
    bool configured;
    bool valid;      /* at least one poll has succeeded */
    bool reachable;
    bool on;
    float watts;
    bool busy;       /* a command is queued or running */
    bool restarting; /* ...and it is a restart */
    char name[32];
    char error[96];  /* last failed command, "" if none */
} ff_power_status_t;
void ff_power_get_status(ff_power_status_t *out);
