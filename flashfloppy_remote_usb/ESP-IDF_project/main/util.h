#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* Small helpers shared by main.c and storage.c (v39: these were
 * duplicated per file before). */

/* Bounded string copy that always NUL-terminates. Used instead of
 * snprintf(dst, cap, "%s", src): with a larger source buffer GCC's
 * -Werror=format-truncation rejects the snprintf form even when it
 * would be safe (Kris's v12 build). */
void ff_copy_field(char *dst, size_t cap, const char *src);

/* Strip leading spaces/tabs and trailing whitespace/CR/LF in place. */
void ff_trim(char *s);

/* Milliseconds since boot, wrapping at 2^32 (~49 days). Compare
 * with unsigned subtraction: (now - then) >= interval. */
uint32_t ff_now_ms(void);

/* NVS "last used image" record (namespace "ff", key "last"). */
void ff_nvs_save_last(const char *name);
esp_err_t ff_nvs_load_last(char *out, size_t cap);
