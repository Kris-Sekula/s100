#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "sdmmc_cmd.h" /* sdmmc_card_t for the peek getter below */

/* Card + USB plumbing for the v15 whole-card design.
 *
 * The SD card is presented to the Gotek as a whole (a normal USB
 * stick) via the esp_tinyusb MSC storage component, which owns the
 * TinyUSB MSC callbacks. Ownership of the card is temporal: at any
 * moment either USB owns it (the normal state) or the ESP32 has it
 * mounted locally (a "local window") to edit IMAGE_A.CFG and land
 * staged uploads. The component enforces the switch; these wrappers
 * track which side is active.
 *
 * ff_sdcard_init() must be called before ff_usb_install(), and no
 * filesystem path under FF_SD_MOUNT_POINT may be touched outside a
 * local window.
 *
 * The local windows are bracketed by a real USB detach/attach
 * (tud_disconnect/tud_connect, v21): the component's mount switch
 * alone does not convince a host the media changed — Windows kept
 * serving a cached IMAGE_A.CFG across it. A detach makes the host
 * treat local_end as a fresh insertion and re-read the card. */
esp_err_t ff_sdcard_init(void);
esp_err_t ff_usb_install(void);
esp_err_t ff_sdcard_local_begin(void); /* pull from USB, mount locally */
esp_err_t ff_sdcard_local_end(void);   /* unmount, hand back to USB */
bool ff_sdcard_local_active(void);
/* The raw card handle, for the storage layer's read-only sector
 * peek (v26). Callers must only ever READ through it, and only
 * while the card is with USB — never write, never during a local
 * window. */
sdmmc_card_t *ff_sdcard_handle(void);
