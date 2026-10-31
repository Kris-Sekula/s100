#include "state.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "util.h"

static const char *TAG = "ff_state";
ff_state_t_store g_ff;

void ff_state_init(void)
{
    memset(&g_ff, 0, sizeof(g_ff));
    g_ff.sd_mutex = xSemaphoreCreateMutex();
    g_ff.state = FF_STATE_IDLE;
    /* "Long ago", so the USB idle check passes until real traffic
     * is seen. */
    g_ff.last_msc_write_ms = ff_now_ms() - 600000u;
    g_ff.last_msc_read_ms = g_ff.last_msc_write_ms;
}

void ff_state_lock(void) { xSemaphoreTake(g_ff.sd_mutex, portMAX_DELAY); }
void ff_state_unlock(void) { xSemaphoreGive(g_ff.sd_mutex); }

void ff_state_set_error(const char *fmt, ...)
{
    char msg[sizeof(g_ff.last_error)];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    ESP_LOGE(TAG, "%s", msg);
    ff_state_lock();
    memcpy(g_ff.last_error, msg, sizeof(msg));
    ff_state_unlock();
}

void ff_state_clear_error(void)
{
    ff_state_lock();
    g_ff.last_error[0] = 0;
    ff_state_unlock();
}

const char *ff_state_name(ff_state_t s)
{
    switch (s) {
        case FF_STATE_IDLE: return "idle";
        case FF_STATE_SWAP_PENDING: return "swap_pending";
        case FF_STATE_DISCONNECTED: return "disconnected";
        case FF_STATE_APPLYING: return "applying";
        case FF_STATE_PRESENTED: return "presented";
        default: return "unknown";
    }
}
