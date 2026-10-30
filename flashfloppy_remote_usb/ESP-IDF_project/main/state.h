#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/* Swap/reinsert state machine. One writer at a time, via the apply
 * task in main.c; HTTP handlers only request a transition. In the
 * v15 whole-card design a "swap" is a local window: the card is
 * pulled from USB, mounted locally, edited (IMAGE_A.CFG, staged
 * uploads), and handed back. */
typedef enum {
    FF_STATE_IDLE,        /* card exposed to USB (or none yet), normal */
    FF_STATE_SWAP_PENDING,/* select/apply/eject requested, waiting for USB idle */
    FF_STATE_DISCONNECTED,/* card pulled from USB, local window starting */
    FF_STATE_APPLYING,    /* locally mounted: editing the card */
    FF_STATE_PRESENTED,   /* card handed back to USB, awaiting Gotek mount */
} ff_state_t;

typedef struct {
    ff_state_t state;
    SemaphoreHandle_t sd_mutex; /* guards this struct and staged state */
    bool presented;             /* card currently exposed to USB */
    bool force_swap;            /* user-confirmed override, skips idle check */
    bool eject_requested;       /* pending op is an eject: stay unmounted */
    char current_path[256];     /* image named by IMAGE_A.CFG — since
                                   v18 a bare root filename ("disk.hfe"),
                                   "" if none */
    char pending_path[256];     /* image requested by /api/select, same form */
    char wifi_desc[64];         /* "ap 192.168.4.1" / "sta 192.168.1.50",
                                   for /api/status; set by the Wi-Fi init */
    char wifi_ssid[33];         /* SSID in use, for the LCD footer */
    char wifi_ip[16];           /* our IP in use, for the LCD footer */
    time_t last_msc_write_ms;
    time_t last_msc_read_ms;
    time_t last_write_wall_ms;  /* for /api/status */
} ff_state_t_store;

extern ff_state_t_store g_ff;

void ff_state_init(void);
void ff_state_lock(void);   /* sd_mutex, for non-MSC code (swap, HTTP, sync) */
void ff_state_unlock(void);
bool ff_state_try_lock(void); /* MSC callbacks: never block the USB task */
const char *ff_state_name(ff_state_t s);
