#pragma once
#include "esp_err.h"

/* Plain FTP server (v43, ported from the generic remote-USB
 * project's v07–v14) — the second file channel next to the web
 * UI's upload, from a real FTP client (FileZilla, WinSCP, ...),
 * while the drive is EJECTED. Browsing commands (PWD/CWD/
 * LIST/NLST/SIZE/MDTM) are served from the card snapshot and
 * work in any state, exactly like the web UI.
 *
 * Shape: control port 21, passive data connections only
 * (PASV/EPSV on a fixed port — active mode is refused), one
 * session at a time, server in its own FreeRTOS task so a
 * long-lived session can never park the httpd task.
 * Credentials come from /sdcard/FTP.CFG at boot (board.h).
 * Root-only: this gadget has no folder concept, so MKD/RMD are
 * refused and nested paths 550.
 *
 * Call ff_ftp_start() while the card is still locally mounted
 * (app_main's boot window), like WIFI.CFG: it reads FTP.CFG,
 * then spawns the server task. */
esp_err_t ff_ftp_start(void);
