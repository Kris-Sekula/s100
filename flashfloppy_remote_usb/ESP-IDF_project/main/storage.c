#include "storage.h"
#include "board.h"
#include "state.h"
#include "sdcard.h" /* ff_sdcard_handle() for the sector peek */
#include <stdio.h>
#include <stdlib.h> /* malloc()/free() for the upload buffer */
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <unistd.h> /* fsync() for the IMAGE_A.CFG write verify */
#include <sys/stat.h>
#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "ff_store";

#define FF_STORE_MAX_ENTRIES 64
static ff_entry_t s_entries[FF_STORE_MAX_ENTRIES];
static int s_entry_count;

static bool is_config_name(const char *name)
{
    /* POWER.CFG (v38) is a config file, not a disk image: it joins
     * FF.CFG/IMG.CFG in the UI's Config Files section and can no
     * longer be "selected" as an image. WIFI.CFG stays unlisted
     * entirely (it holds a password). */
    return strcasecmp(name, "FF.CFG") == 0 ||
           strcasecmp(name, "IMG.CFG") == 0 ||
           strcasecmp(name, "POWER.CFG") == 0;
}

static bool is_backup_name(const char *name)
{
    size_t n = strlen(name);
    return n > 4 && strcasecmp(name + n - 4, ".bak") == 0;
}

static bool is_partial_name(const char *name)
{
    size_t n = strlen(name);
    return n > 5 && strcasecmp(name + n - 5, ".part") == 0;
}

static bool file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

/* Bounded copy, always NUL-terminating. snprintf(dst, cap, "%s",
 * src) into a smaller field trips -Werror=format-truncation when
 * GCC can bound the source (d_name is up to 255 bytes, the entry
 * name field is 160) — the v12/v13 lesson, applied here in v16. */
static void copy_field(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);
    if (n > cap - 1) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

/* Make the root FF.CFG select via IMAGE_A.CFG. Kris's bench test
 * (2026-10-30): the explicit `image-on-startup = last` line is what
 * makes FlashFloppy read IMAGE_A.CFG on a hot re-insert. Absent line
 * -> append it. Different value -> respect his file, but warn loudly:
 * web selection will not take effect until he changes it. */
static void ensure_ff_cfg(void)
{
    static char content[8192];
    FILE *f = fopen(FF_FF_CFG_PATH, "r");
    if (!f) {
        f = fopen(FF_FF_CFG_PATH, "w");
        if (f) {
            fputs("# Written by the FlashFloppy ESP32 frontend: web "
                  "selection works by naming the image in IMAGE_A.CFG,\n"
                  "# which FlashFloppy only follows with this setting.\n"
                  "image-on-startup = last\n", f);
            fclose(f);
            ESP_LOGI(TAG, "created FF.CFG with image-on-startup = last");
        }
        return;
    }
    size_t len = fread(content, 1, sizeof(content) - 1, f);
    fclose(f);
    content[len] = 0;

    bool found = false;
    char value[32] = {0};
    char *save = NULL;
    for (char *line = strtok_r(content, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (strncasecmp(p, "image-on-startup", 16) != 0) continue;
        /* The next char must end the key (not a longer key that
         * merely starts with the same text). */
        if (!(p[16] == ' ' || p[16] == '\t' || p[16] == '=')) continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        /* Key must be exactly the setting, not a longer name. */
        bool key_ok = true;
        for (char *k = p; k < eq; k++) {
            if (!((*k >= 'a' && *k <= 'z') || (*k >= 'A' && *k <= 'Z') ||
                  *k == '-' || *k == ' ' || *k == '\t')) { key_ok = false; }
        }
        if (!key_ok) continue;
        found = true;
        char *v = eq + 1;
        while (*v == ' ' || *v == '\t') v++;
        size_t vn = strcspn(v, " \t\r#");
        if (vn >= sizeof(value)) vn = sizeof(value) - 1;
        memcpy(value, v, vn);
        value[vn] = 0;
        break;
    }
    if (found) {
        if (strcasecmp(value, "last") != 0) {
            ESP_LOGW(TAG, "FF.CFG has image-on-startup = %s; web "
                          "selection needs 'last' and will NOT take "
                          "effect until that is changed", value);
        }
        return;
    }
    f = fopen(FF_FF_CFG_PATH, "a");
    if (f) {
        fputs("\n# Added by the FlashFloppy ESP32 frontend: web "
              "selection names the image in IMAGE_A.CFG.\n"
              "image-on-startup = last\n", f);
        fclose(f);
        ESP_LOGI(TAG, "appended image-on-startup = last to FF.CFG");
    }
}

esp_err_t ff_store_prepare(void)
{
    /* No layout migration (Kris, 2026-10-30: the card's files are
     * already in the root; the pre-v18 /library folder is simply
     * ignored). Preparation is just the FF.CFG guarantee. */
    ensure_ff_cfg();
    return ESP_OK;
}

esp_err_t ff_store_refresh(void)
{
    /* Everything lives in the root (v18). IMAGE_A.CFG is FlashFloppy's
     * own state and WIFI.CFG is ours; neither is listed. Directories
     * and dotfiles (a card reader's housekeeping included) are
     * skipped by the regular-file and name checks. */
    s_entry_count = 0;
    DIR *d = opendir(FF_SD_MOUNT_POINT);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL &&
               s_entry_count < FF_STORE_MAX_ENTRIES) {
            if (e->d_name[0] == '.') continue;
            if (strcasecmp(e->d_name, "IMAGE_A.CFG") == 0 ||
                strcasecmp(e->d_name, "WIFI.CFG") == 0) continue;
            char path[420];
            snprintf(path, sizeof(path), "%s/%s", FF_SD_MOUNT_POINT,
                     e->d_name);
            if (is_partial_name(e->d_name)) {
                /* Leftover of an interrupted direct upload (v37):
                 * never listed, and tidied away here — refresh only
                 * runs while the card is locally mounted, and an
                 * upload in progress holds its claim, so a .part
                 * seen now is always stale. */
                unlink(path);
                continue;
            }
            struct stat st;
            if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
            ff_entry_t *en = &s_entries[s_entry_count++];
            copy_field(en->name, sizeof(en->name), e->d_name);
            en->size = (uint32_t)st.st_size;
            en->kind = is_config_name(e->d_name) ? FF_KIND_CONFIG
                       : is_backup_name(e->d_name) ? FF_KIND_BACKUP
                                                   : FF_KIND_IMAGE;
        }
        closedir(d);
    }

    /* Current image, straight from IMAGE_A.CFG: the shared truth
     * between us and FlashFloppy, whichever side changed it last.
     * Log the raw bytes (non-printables escaped): what FlashFloppy
     * left in the file after a re-insert is the ground truth when a
     * selection does not stick — FF rewrites the file in its own
     * form, and clears it entirely if it cannot parse the name. */
    char current[256] = {0};
    FILE *f = fopen(FF_IMAGE_A_PATH, "r");
    if (f) {
        size_t raw_len = fread(current, 1, sizeof(current) - 1, f);
        fclose(f);
        char preview[160];
        size_t w = 0;
        for (size_t i = 0; i < raw_len && w + 5 < sizeof(preview); i++) {
            unsigned char c = (unsigned char)current[i];
            if (c >= 0x20 && c < 0x7f) {
                preview[w++] = (char)c;
            } else {
                w += (size_t)snprintf(preview + w, sizeof(preview) - w,
                                      "\\x%02x", c);
            }
        }
        preview[w] = 0;
        ESP_LOGI(TAG, "IMAGE_A.CFG on card: %u bytes: \"%s\"",
                 (unsigned)raw_len, preview);
        current[strcspn(current, "\r\n")] = 0;
    }
    ff_state_lock();
    snprintf(g_ff.current_path, sizeof(g_ff.current_path), "%s", current);
    ff_state_unlock();

    /* Keep the NVS last-used record in step with the card, so
     * auto-present at the next boot follows Gotek-side selections
     * too — but only trust names that still exist. */
    if (current[0]) {
        char path[420];
        snprintf(path, sizeof(path), "%s/%s", FF_SD_MOUNT_POINT, current);
        if (file_exists(path)) {
            nvs_handle_t nv;
            if (nvs_open("ff", NVS_READWRITE, &nv) == ESP_OK) {
                nvs_set_str(nv, "last", current);
                nvs_commit(nv);
                nvs_close(nv);
            }
        }
    }
    return ESP_OK;
}

int ff_store_count(void)
{
    return s_entry_count;
}

const ff_entry_t *ff_store_entry(int i)
{
    return (i >= 0 && i < s_entry_count) ? &s_entries[i] : NULL;
}

esp_err_t ff_store_write_image_a(const char *rel)
{
    /* Write IMAGE_A.CFG in FlashFloppy's own canonical form. From
     * the FF source (src/main.c, native_update): FF opens the file
     * RW, seeks past any folder prefix, writes the bare filename —
     * NO trailing newline, NO NUL terminator — and truncates there.
     * Our v15-v19 form (name + '\n') parses fine on FF's reader, but
     * matching its writer byte-for-byte removes a variable, and the
     * read-back verify below turns the next failure, if any, into a
     * log line instead of a mystery (Kris's bench, 2026-10-30:
     * IMAGE_A.CFG seemingly not updated on swap, with stray NUL
     * bytes at the end of the file that neither writer should
     * produce). */
    FILE *f = fopen(FF_IMAGE_A_PATH, "w");
    if (!f) {
        ESP_LOGE(TAG, "IMAGE_A.CFG open for write failed");
        return ESP_FAIL;
    }
    size_t want = strlen(rel);
    bool ok = fwrite(rel, 1, want, f) == want;
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    if (!ok) {
        ESP_LOGE(TAG, "IMAGE_A.CFG write failed for %s", rel);
        return ESP_FAIL;
    }
    /* Read back within the same window and compare byte-for-byte. */
    char back[300];
    size_t got = 0;
    f = fopen(FF_IMAGE_A_PATH, "r");
    if (f) {
        got = fread(back, 1, sizeof(back) - 1, f);
        fclose(f);
    }
    back[got] = 0;
    if (got != want || memcmp(back, rel, want) != 0) {
        ESP_LOGE(TAG, "IMAGE_A.CFG verify FAILED: wrote '%s' "
                      "(%u bytes), read back %u bytes '%s'",
                 rel, (unsigned)want, (unsigned)got, back);
        return ESP_FAIL;
    }
    ff_state_lock();
    snprintf(g_ff.current_path, sizeof(g_ff.current_path), "%s", rel);
    ff_state_unlock();
    ESP_LOGI(TAG, "IMAGE_A.CFG now names %s (%u bytes, verified)",
             rel, (unsigned)want);
    return ESP_OK;
}

/* ---- direct streaming upload (v37) ---- */

esp_err_t ff_store_write_stream(const char *name, ff_fill_fn fill,
                                void *ctx, size_t *written_out)
{
    char target[420], part[440];
    snprintf(target, sizeof(target), "%s/%s", FF_SD_MOUNT_POINT,
             name);
    snprintf(part, sizeof(part), "%s.part", target);
    FILE *f = fopen(part, "wb");
    if (!f) {
        ESP_LOGE(TAG, "upload: cannot create %s", part);
        return ESP_FAIL;
    }
    uint8_t *buf = malloc(8192);
    if (!buf) {
        fclose(f);
        unlink(part);
        return ESP_ERR_NO_MEM;
    }
    size_t total = 0;
    esp_err_t err = ESP_OK;
    for (;;) {
        int n = fill(ctx, buf, 8192);
        if (n < 0) { err = ESP_FAIL; break; }
        if (n == 0) break;
        if (fwrite(buf, 1, (size_t)n, f) != (size_t)n) {
            err = ESP_FAIL; /* card full or media error */
            break;
        }
        total += (size_t)n;
    }
    if (err == ESP_OK) {
        fflush(f);
        if (fsync(fileno(f)) != 0) err = ESP_FAIL;
    }
    fclose(f);
    free(buf);
    if (err != ESP_OK) {
        unlink(part);
        ESP_LOGE(TAG, "upload of %s aborted after %u bytes; the "
                      "partial file was removed",
                 name, (unsigned)total);
        return err;
    }
    /* FatFs rename will not overwrite an existing file: clear the
     * way first (the container-era lesson). */
    unlink(target);
    if (rename(part, target) != 0) {
        unlink(part);
        ESP_LOGE(TAG, "upload of %s: rename into place failed",
                 name);
        return ESP_FAIL;
    }
    if (written_out) *written_out = total;
    ESP_LOGI(TAG, "uploaded %s (%u bytes) directly to the card",
             name, (unsigned)total);
    /* A freshly landed FF.CFG may still lack the setting selection
     * depends on (same guarantee the staged flush used to run). */
    if (is_config_name(name)) ensure_ff_cfg();
    return ESP_OK;
}

/* ---- Read-only peek at IMAGE_A.CFG while USB owns the card (v26)
 *
 * Kris's scenario: after our swap, the user picks a different image
 * on the Gotek's physical buttons; FlashFloppy rewrites IMAGE_A.CFG
 * itself, and our UI/LCD would show the stale name until the next
 * local window. The filesystem cannot be read while USB owns the
 * card — but raw sectors can: this walks MBR -> BPB -> root
 * directory -> the file's first cluster with sdmmc_read_sectors.
 * It NEVER writes, so FlashFloppy's cached filesystem state is
 * untouchable; and a result is only believed after the caller
 * confirms it with a second identical read (a button-side rewrite
 * caught mid-update can yield a torn sample). Supports FAT16 and
 * FAT32, 512-byte sectors, IMAGE_A.CFG under its plain 8.3 name. */

typedef struct {
    bool valid;
    uint8_t spc;            /* sectors per cluster */
    uint32_t fat_lba;       /* first FAT */
    uint32_t data_lba;      /* first data sector */
    uint32_t root_cluster;  /* FAT32 root start cluster; 0 = FAT16 */
    uint32_t root_dir_lba;  /* FAT16 fixed root region */
    uint32_t root_sectors;  /* FAT16 root region size in sectors */
} peek_geo_t;

static peek_geo_t s_geo;
static uint8_t s_sector[512];

static uint16_t pk16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t pk32(const uint8_t *p)
{
    return pk16(p) | ((uint32_t)pk16(p + 2) << 16);
}

static esp_err_t peek_read(uint32_t lba)
{
    sdmmc_card_t *card = ff_sdcard_handle();
    if (!card) return ESP_ERR_INVALID_STATE;
    return sdmmc_read_sectors(card, s_sector, lba, 1);
}

static esp_err_t peek_geo_init(void)
{
    if (s_geo.valid) return ESP_OK;
    if (peek_read(0) != ESP_OK) return ESP_FAIL;
    const uint8_t *bpb = s_sector;
    uint32_t part = 0;
    if (!(bpb[0] == 0xEB || bpb[0] == 0xE9)) {
        /* Not a superfloppy: parse MBR partition 1. */
        const uint8_t *pe = s_sector + 0x1BE;
        if (s_sector[510] != 0x55 || s_sector[511] != 0xAA ||
            pe[4] == 0) {
            return ESP_FAIL;
        }
        part = pk32(pe + 8);
        if (peek_read(part) != ESP_OK) return ESP_FAIL;
        bpb = s_sector;
    }
    if (pk16(bpb + 11) != 512 || bpb[13] == 0) return ESP_FAIL;
    uint16_t reserved = pk16(bpb + 14);
    uint8_t fats = bpb[16];
    uint16_t root_entries = pk16(bpb + 17);
    uint32_t fat_size = pk16(bpb + 22);
    if (!fat_size) fat_size = pk32(bpb + 36);
    if (!fats || !fat_size) return ESP_FAIL;
    s_geo.spc = bpb[13];
    s_geo.fat_lba = part + reserved;
    s_geo.root_sectors = ((uint32_t)root_entries * 32 + 511) / 512;
    s_geo.root_dir_lba = s_geo.fat_lba + (uint32_t)fats * fat_size;
    s_geo.data_lba = s_geo.root_dir_lba + s_geo.root_sectors;
    s_geo.root_cluster = root_entries == 0 ? pk32(bpb + 44) : 0;
    s_geo.valid = true;
    ESP_LOGI(TAG, "peek: %s volume (partition LBA %u, %u sect/cluster)",
             s_geo.root_cluster ? "FAT32" : "FAT16", (unsigned)part,
             s_geo.spc);
    return ESP_OK;
}

static uint32_t peek_cluster_lba(uint32_t cl)
{
    return s_geo.data_lba + (cl - 2) * s_geo.spc;
}

static uint32_t peek_fat_next(uint32_t cl)
{
    uint32_t off = s_geo.root_cluster ? cl * 4 : cl * 2;
    if (peek_read(s_geo.fat_lba + off / 512) != ESP_OK) return 0;
    if (s_geo.root_cluster) {
        return pk32(s_sector + (off % 512)) & 0x0FFFFFFF;
    }
    return pk16(s_sector + (off % 512));
}

/* Scan one directory sector for IMAGE_A.CFG.
 * Returns 1 = found (cluster and size set), 0 = not here,
 * -1 = end-of-directory marker reached. */
static int peek_scan_sector(uint32_t *cluster, uint32_t *size)
{
    for (int i = 0; i < 16; i++) {
        const uint8_t *e = s_sector + i * 32;
        if (e[0] == 0x00) return -1;
        if (e[0] == 0xE5) continue;
        if ((e[11] & 0x0F) == 0x0F) continue; /* LFN fragment */
        if (memcmp(e, "IMAGE_A CFG", 11) == 0) {
            *cluster = pk16(e + 26) | ((uint32_t)pk16(e + 20) << 16);
            *size = pk32(e + 28);
            return 1;
        }
    }
    return 0;
}

esp_err_t ff_store_peek_image_a(char *out, size_t cap)
{
    if (peek_geo_init() != ESP_OK) return ESP_FAIL;
    uint32_t fcluster = 0, fsize = 0;
    bool found = false, ended = false;
    if (s_geo.root_cluster) {
        uint32_t cl = s_geo.root_cluster;
        for (int hops = 0; hops < 4 && cl >= 2 && !found && !ended;
             hops++) {
            for (uint32_t s = 0; s < s_geo.spc && !found && !ended;
                 s++) {
                if (peek_read(peek_cluster_lba(cl) + s) != ESP_OK) {
                    return ESP_FAIL;
                }
                int r = peek_scan_sector(&fcluster, &fsize);
                if (r == 1) found = true;
                else if (r < 0) ended = true;
            }
            if (!found && !ended) {
                cl = peek_fat_next(cl);
                if (cl < 2 || cl >= 0x0FFFFFF8) break;
            }
        }
    } else {
        for (uint32_t s = 0; s < s_geo.root_sectors && !found && !ended;
             s++) {
            if (peek_read(s_geo.root_dir_lba + s) != ESP_OK) {
                return ESP_FAIL;
            }
            int r = peek_scan_sector(&fcluster, &fsize);
            if (r == 1) found = true;
            else if (r < 0) ended = true;
        }
    }
    if (!found) {
        /* Trustworthy "absent" only if the scan reached the
         * end-of-directory marker; otherwise it was inconclusive. */
        if (!ended) return ESP_FAIL;
        if (cap) out[0] = 0;
        return ESP_OK;
    }
    if (fsize == 0) { /* FlashFloppy cleared it */
        if (cap) out[0] = 0;
        return ESP_OK;
    }
    if (fsize > 1000 || fcluster < 2) return ESP_FAIL; /* sanity */
    if (peek_read(peek_cluster_lba(fcluster)) != ESP_OK) {
        return ESP_FAIL;
    }
    size_t n = fsize < cap - 1 ? fsize : cap - 1;
    if (n > 511) n = 511;
    size_t len = 0;
    while (len < n) {
        uint8_t c = s_sector[len];
        if (c == 0 || c == '\r' || c == '\n') break;
        if (c < 0x20 || c > 0x7E) return ESP_FAIL; /* torn read? */
        out[len++] = (char)c;
    }
    out[len] = 0;
    return ESP_OK;
}
