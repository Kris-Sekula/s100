#include "util.h"
#include <string.h>
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"

void ff_copy_field(char *dst, size_t cap, const char *src)
{
    if (!cap) return;
    size_t n = strlen(src);
    if (n > cap - 1) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

void ff_trim(char *s)
{
    char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' ||
                 s[n - 1] == '\r' || s[n - 1] == '\n')) {
        s[--n] = 0;
    }
}

uint32_t ff_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

void ff_nvs_save_last(const char *name)
{
    nvs_handle_t nv;
    if (nvs_open("ff", NVS_READWRITE, &nv) != ESP_OK) return;
    /* Skip the flash write when the record already says this. */
    char cur[256];
    size_t len = sizeof(cur);
    if (nvs_get_str(nv, "last", cur, &len) != ESP_OK ||
        strcmp(cur, name) != 0) {
        nvs_set_str(nv, "last", name);
        nvs_commit(nv);
    }
    nvs_close(nv);
}

esp_err_t ff_nvs_load_last(char *out, size_t cap)
{
    nvs_handle_t nv;
    esp_err_t err = nvs_open("ff", NVS_READONLY, &nv);
    if (err != ESP_OK) return err;
    size_t len = cap;
    err = nvs_get_str(nv, "last", out, &len);
    nvs_close(nv);
    return err;
}
