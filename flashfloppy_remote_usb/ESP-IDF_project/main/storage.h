#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* Card-content layer for the v15 whole-card design. Every function
 * here operates on the locally mounted card and may only be called
 * inside a local window (see sdcard.h) or in the ejected state the
 * window leaves behind — except the snapshot accessors, which serve
 * the HTTP handlers from RAM at any time. */

typedef enum { FF_KIND_IMAGE = 0, FF_KIND_CONFIG, FF_KIND_BACKUP } ff_kind_t;

typedef struct {
    char name[160]; /* basename; everything lives in the card root */
    uint32_t size;
    ff_kind_t kind;
} ff_entry_t;

/* Boot-time preparation, inside the first local window: make sure
 * the root FF.CFG selects via IMAGE_A.CFG (image-on-startup = last
 * — Kris's bench test, 2026-10-30: the explicit setting is what
 * makes FlashFloppy honour IMAGE_A.CFG on a hot re-insert). No
 * layout migration: the card's files already live in the root. */
esp_err_t ff_store_prepare(void);

/* Refresh the in-RAM view of the card: library listing for the web
 * UI, the current image from IMAGE_A.CFG, and the NVS last-used
 * record, synced FROM the card so selections made on the Gotek's
 * own controls are learned too. Local window only. */
esp_err_t ff_store_refresh(void);
int ff_store_count(void);
const ff_entry_t *ff_store_entry(int i);

/* Write IMAGE_A.CFG naming rel — since v18 simply the image's
 * filename in the root (e.g. "disk.hfe"). Local window only. */
esp_err_t ff_store_write_image_a(const char *rel);

/* Direct streaming upload (v37, replacing PSRAM staging): the
 * body is written straight to the card as <name>.part and renamed
 * into place only when the stream completes cleanly, so an
 * interrupted upload never leaves a partial file FlashFloppy
 * could try to mount. Any size the card can hold. Ejected/local
 * state only. fill(ctx, buf, cap) returns >0 bytes read, 0 at
 * end of body, <0 on error. */
typedef int (*ff_fill_fn)(void *ctx, uint8_t *buf, size_t cap);
esp_err_t ff_store_write_stream(const char *name, ff_fill_fn fill,
                                void *ctx, size_t *written_out);

/* Read IMAGE_A.CFG by raw sectors while the card is with USB
 * (v26): walks MBR/BPB/root directory directly. Read-only — it can
 * never disturb FlashFloppy's filesystem state. Returns ESP_OK
 * with the name in out ("" if the file is absent or FlashFloppy
 * cleared it), ESP_FAIL when inconclusive (bad geometry, torn
 * read, scan overrun). Callers should confirm a CHANGED result
 * with a second identical read before believing it. */
esp_err_t ff_store_peek_image_a(char *out, size_t cap);
