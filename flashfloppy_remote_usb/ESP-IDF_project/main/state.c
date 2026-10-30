#include "state.h"
#include <string.h>
#include "esp_log.h"

static const char *TAG = "ff_state";
ff_state_t_store g_ff;

void ff_state_init(void)
{
    memset(&g_ff, 0, sizeof(g_ff));
    g_ff.sd_mutex = xSemaphoreCreateMutex();
    g_ff.state = FF_STATE_IDLE;
}

void ff_state_lock(void) { xSemaphoreTake(g_ff.sd_mutex, portMAX_DELAY); }
void ff_state_unlock(void) { xSemaphoreGive(g_ff.sd_mutex); }
bool ff_state_try_lock(void) { return xSemaphoreTake(g_ff.sd_mutex, 0) == pdTRUE; }

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
    (void)TAG;
}
