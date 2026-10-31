#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/* Swap/reinsert state machine. One writer at a time, via the apply
 * task in main.c; HTTP handlers only request a transition. In the
 * v15 whole-card design a "swap" is a local window: the card is
 * pulled from USB, mounted locally, edited (IMAGE_A.CFG), and
 * handed back. Eject leaves it locally mounted — and since v37
 * that ejected state is also the upload state: uploads stream
 * straight to the card while upload_active is set. */
typedef enum {
    FF_STATE_IDLE,        /* card exposed to USB (or none yet), normal */
    FF_STATE_SWAP_PENDING,/* select/reinsert/eject requested, waiting for
                             USB idle (or force) before the window */
    FF_STATE_DISCONNECTED,/* card pulled from USB, local window starting */
    FF_STATE_APPLYING,    /* locally mounted: editing the card */
    FF_STATE_PRESENTED,   /* card handed back to USB, awaiting Gotek mount */
} ff_state_t;

typedef struct {
    ff_state_t state;
    SemaphoreHandle_t sd_mutex; /* guards this struct and staged state */
    bool presented;             /* card currently exposed to USB */
    bool force_swap;            /* user-confirmed override, skips the USB
                                   idle wait (/api/force, v39) */
    bool waiting_usb;           /* SWAP_PENDING and held back by USB
                                   activity right now (v39, for the UI) */
    bool eject_requested;       /* pending op is an eject: stay unmounted */
    bool restart_after_swap;    /* power-cycle the host after this
                                   window completes (v28) */
    bool upload_active;         /* a direct-to-card upload is
                                   streaming (v37); windows are
                                   refused until it finishes */
    char current_path[256];     /* image named by IMAGE_A.CFG — since
                                   v18 a bare root filename ("disk.hfe"),
                                   "" if none */
    char pending_path[256];     /* image requested by /api/select, same form */
    char wifi_desc[64];         /* "ap 192.168.4.1" / "sta 192.168.1.50",
                                   for /api/status; set by the Wi-Fi init */
    char wifi_ssid[33];         /* SSID in use, for the LCD footer */
    char wifi_ip[16];           /* our IP in use, for the LCD footer */
    uint32_t pending_since_ms;  /* when the current request was queued */
    uint32_t gen;               /* bumped at every local window (v39):
                                   the peek task drops a result whose
                                   read straddled a window */
    bool lcd_dirty;             /* Wi-Fi footer changed; the apply task
                                   repaints (v39) */
    char last_error[128];       /* last failed operation, "" if none;
                                   shown by the UI (v39) */
    /* USB MSC activity, stamped by the read10/write10 wrappers in
     * sdcard.c from the TinyUSB task WITHOUT the lock (32-bit
     * stores are atomic here). ff_now_ms() time base. (v39) */
    volatile uint32_t last_msc_write_ms;
    volatile uint32_t last_msc_read_ms;
} ff_state_t_store;

extern ff_state_t_store g_ff;

void ff_state_init(void);
void ff_state_lock(void);   /* sd_mutex, for non-MSC code (swap, HTTP, sync) */
void ff_state_unlock(void);
const char *ff_state_name(ff_state_t s);

/* Record / clear the error the UI shows (printf-style; takes the
 * lock itself, so call it WITHOUT holding ff_state_lock). */
void ff_state_set_error(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));
void ff_state_clear_error(void);

/* Wake the apply task (main.c) after queuing a request (v39: it
 * waits on a task notification instead of polling every 100 ms). */
void ff_apply_kick(void);
