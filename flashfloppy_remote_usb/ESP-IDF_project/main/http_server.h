#pragma once
#include <stdbool.h>
#include "esp_err.h"

/* HTTP API. Handlers only REQUEST state transitions; the swap task in
 * main.c owns the disconnect/build/reconnect sequence. */
esp_err_t ff_http_start(void);

/* Queue a select exactly as POST /api/select does, for callers
 * that are not HTTP handlers (the BOOT-button task, v42). Returns
 * false when a window is already queued/running or an upload is
 * streaming. */
bool ff_request_select(const char *name);

/* API surface:
 *   GET  /                embedded web UI (web_ui.h)
 *   GET  /api/status      {current, state, presented, dirty, last_write}
 *   GET  /api/library     {files: [{name, path, size}]}
 *   POST /api/select      {"path": "<library path>"}  (or {"force": true})
 *   POST /api/eject       {} — disconnect, no reconnect (or no-image
 *                          container with ejected-on-startup=yes later)
 *   POST /api/upload?name=<file>  raw image bytes in the body, streamed
 *                          to .staging then renamed into the library.
 *                          Refuses to overwrite the presented image:
 *                          swap away, upload, then select the new version.
 * Security, v1: HTTP basic auth at minimum, LAN-only, never port-forward.
 * Auth is a TODO in http_server.c — do not expose this beyond the LAN
 * until it lands. */
