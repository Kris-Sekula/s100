#include "http_server.h"
#include "board.h"
#include "state.h"
#include "storage.h"
#include "power.h"
#include "web_ui.h"
#include "util.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "cJSON.h"

static const char *TAG = "ff_http";

static esp_err_t send_json(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json");
    /* The UI polls these endpoints; a heuristically cached response
     * would freeze the status line at a stale state/name (v22). */
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

/* httpd_err_code_t has no 409 member (its codes jump 400 -> 500 with
 * named slots only for a few 4xx), so HTTPD_409_CONFLICT does not
 * exist — passing it is what broke the v01 build. Send a real 409 by
 * setting the status line manually. */
static esp_err_t send_conflict(httpd_req_t *req, const char *msg)
{
    httpd_resp_set_status(req, "409 Conflict");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, msg, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    /* The page JS changes between firmware versions and Kris
     * reflashes often; never let a browser serve it from cache. */
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, FF_WEB_UI_HTML, HTTPD_RESP_USE_STRLEN);
}

/* Serialise a cJSON tree and send it; frees the tree. */
static esp_err_t send_cjson(httpd_req_t *req, cJSON *root)
{
    char *json = root ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    if (!json) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "out of memory");
    }
    esp_err_t err = send_json(req, json);
    cJSON_free(json);
    return err;
}

static esp_err_t h_status(httpd_req_t *req)
{
    /* v39: built with cJSON (a filename containing '"' or '\' used to
     * produce invalid JSON and stall the UI's polling) and copied
     * under the state lock (no torn strings). */
    cJSON *root = cJSON_CreateObject();
    ff_state_lock();
    bool ejected = !g_ff.presented && g_ff.state == FF_STATE_IDLE;
    cJSON_AddStringToObject(root, "current", g_ff.current_path);
    cJSON_AddStringToObject(root, "state", ff_state_name(g_ff.state));
    cJSON_AddBoolToObject(root, "presented", g_ff.presented);
    cJSON_AddBoolToObject(root, "ejected", ejected);
    cJSON_AddBoolToObject(root, "uploading", g_ff.upload_active);
    cJSON_AddBoolToObject(root, "waiting_usb",
                          g_ff.state == FF_STATE_SWAP_PENDING &&
                              g_ff.waiting_usb);
    cJSON_AddStringToObject(root, "wifi", g_ff.wifi_desc);
    cJSON_AddStringToObject(root, "error", g_ff.last_error);
    ff_state_unlock();
    return send_cjson(req, root);
}

static esp_err_t h_library(httpd_req_t *req)
{
    /* The card itself is off-limits while USB owns it, so this
     * serves the snapshot taken at the last local window. While
     * ejected, uploads land on the card immediately and the
     * snapshot is refreshed after each one (v37). */
    cJSON *root = cJSON_CreateObject();
    cJSON *files = cJSON_AddArrayToObject(root, "files");
    /* Emit in passes so the UI list groups images first, backups
     * next, and the CFG files last instead of mixed in with the
     * real images (Kris, 2026-10-30). Within a group the card's
     * own directory order is kept. The snapshot is read under the
     * state lock (v39): a refresh publishes under the same lock. */
    ff_state_lock();
    for (int pass = 0; pass < 3; pass++) {
        for (int i = 0; i < ff_store_count(); i++) {
            const ff_entry_t *en = ff_store_entry(i);
            if (!en) continue;
            int rank = en->kind == FF_KIND_IMAGE ? 0
                       : en->kind == FF_KIND_BACKUP ? 1 : 2;
            if (rank != pass) continue;
            cJSON *f = cJSON_CreateObject();
            cJSON_AddStringToObject(f, "name", en->name);
            /* Since v18 everything lives in the root, so the path
             * the UI sends back on Select is simply the filename —
             * the same string IMAGE_A.CFG will hold. */
            cJSON_AddStringToObject(f, "path", en->name);
            cJSON_AddNumberToObject(f, "size", en->size);
            cJSON_AddStringToObject(f, "kind",
                                    en->kind == FF_KIND_CONFIG ? "config"
                                    : en->kind == FF_KIND_BACKUP
                                        ? "backup"
                                        : "image");
            cJSON_AddItemToArray(files, f);
        }
    }
    ff_state_unlock();
    return send_cjson(req, root);
}

/* Shared request-a-window logic for select / reinsert / eject. */
static esp_err_t request_window(httpd_req_t *req, const char *pending,
                                bool eject, bool restart)
{
    ff_state_lock();
    if (g_ff.upload_active) {
        ff_state_unlock();
        return send_conflict(req, "upload in progress");
    }
    if (g_ff.state == FF_STATE_SWAP_PENDING ||
        g_ff.state == FF_STATE_DISCONNECTED ||
        g_ff.state == FF_STATE_APPLYING) {
        ff_state_unlock();
        return send_conflict(req, "operation in progress");
    }
    if (pending) {
        snprintf(g_ff.pending_path, sizeof(g_ff.pending_path), "%s",
                 pending);
    } else {
        g_ff.pending_path[0] = 0;
    }
    g_ff.eject_requested = eject;
    g_ff.restart_after_swap = restart;
    g_ff.force_swap = false;
    g_ff.waiting_usb = false;
    g_ff.pending_since_ms = ff_now_ms();
    g_ff.last_error[0] = 0; /* a new request starts clean */
    g_ff.state = FF_STATE_SWAP_PENDING;
    ff_state_unlock();
    ff_apply_kick();
    return send_json(req, "{\"ok\":true}");
}

/* v42: non-HTTP twin of request_window() for the button task —
 * the same claim checks and field sequence, with no response to
 * send. Kept beside request_window so the two cannot drift. */
bool ff_request_select(const char *name)
{
    ff_state_lock();
    bool ok = !g_ff.upload_active &&
              g_ff.state != FF_STATE_SWAP_PENDING &&
              g_ff.state != FF_STATE_DISCONNECTED &&
              g_ff.state != FF_STATE_APPLYING;
    if (ok) {
        snprintf(g_ff.pending_path, sizeof(g_ff.pending_path), "%s",
                 name);
        g_ff.eject_requested = false;
        g_ff.restart_after_swap = false;
        g_ff.force_swap = false;
        g_ff.waiting_usb = false;
        g_ff.pending_since_ms = ff_now_ms();
        g_ff.last_error[0] = 0; /* a new request starts clean */
        g_ff.state = FF_STATE_SWAP_PENDING;
    }
    ff_state_unlock();
    if (ok) ff_apply_kick();
    return ok;
}

static esp_err_t h_select(httpd_req_t *req)
{
    char body[384];
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body");
    }
    body[len] = 0;
    cJSON *root = cJSON_Parse(body);
    cJSON *path = root ? cJSON_GetObjectItem(root, "path") : NULL;
    if (!cJSON_IsString(path)) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "path required");
    }
    /* The path must name a real image from the snapshot: config files
     * are applied by the storage layer, backups are not images, and
     * anything else is simply not on the card. */
    bool ok = ff_store_has_image(path->valuestring);
    char chosen[256];
    snprintf(chosen, sizeof(chosen), "%s", path->valuestring);
    /* Optional: power-cycle the host once the swap completes. */
    bool want_restart =
        cJSON_IsTrue(cJSON_GetObjectItem(root, "restart"));
    cJSON_Delete(root);
    if (!ok) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "not a selectable image on the card");
    }
    return request_window(req, chosen, false, want_restart);
}

static esp_err_t h_reinsert(httpd_req_t *req)
{
    /* Refresh our view and hand the card back, keeping the
     * current image. Also the way out of the ejected/upload
     * state. */
    return request_window(req, NULL, false, false);
}

static esp_err_t h_eject(httpd_req_t *req)
{
    /* Eject = pull the card and keep it pulled: the apply task runs
     * a window (our view refreshes) but does not hand the card
     * back. The state it leaves behind — locally mounted, absent
     * from USB — is the upload state (v37). */
    return request_window(req, NULL, true, false);
}

static esp_err_t h_force(httpd_req_t *req)
{
    /* v39: skip the USB idle wait for the queued request. The UI
     * offers this while the status says waiting_usb — i.e. the
     * Gotek keeps reading, or has written within FF_IDLE_WRITE_MS.
     * Forcing mid-write can corrupt what was being written. */
    ff_state_lock();
    bool pending = g_ff.state == FF_STATE_SWAP_PENDING;
    if (pending) g_ff.force_swap = true;
    ff_state_unlock();
    if (!pending) return send_conflict(req, "nothing is waiting");
    ff_apply_kick();
    return send_json(req, "{\"ok\":true}");
}

/* Kasa power control (v28): the strip feeding the host machine.
 * GET reports a live query; POST takes {"action":"on|off|restart"}. */
static esp_err_t h_power_get(httpd_req_t *req)
{
    /* Served from the power module's cache (v36) — this handler
     * never touches the network. cJSON since v39 (the name comes
     * from POWER.CFG and may contain anything). */
    ff_power_status_t st;
    ff_power_get_status(&st);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "configured", st.configured);
    if (st.configured) {
        cJSON_AddStringToObject(root, "name", st.name);
        cJSON_AddBoolToObject(root, "reachable", st.reachable);
        cJSON_AddBoolToObject(root, "on", st.on);
        cJSON_AddNumberToObject(root, "watts",
                                (double)((int)(st.watts * 10.0f)) / 10.0);
        cJSON_AddBoolToObject(root, "busy", st.busy);
        cJSON_AddBoolToObject(root, "restarting", st.restarting);
        cJSON_AddStringToObject(root, "error", st.error);
    }
    return send_cjson(req, root);
}

static esp_err_t h_power_post(httpd_req_t *req)
{
    if (!ff_power_configured()) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "power control not configured");
    }
    char body[192];
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body");
    }
    body[len] = 0;
    cJSON *root = cJSON_Parse(body);
    const char *action = cJSON_GetStringValue(
        root ? cJSON_GetObjectItem(root, "action") : NULL);
    int cmd = -1;
    if (action && strcmp(action, "on") == 0) cmd = FF_POWER_ON;
    else if (action && strcmp(action, "off") == 0) cmd = FF_POWER_OFF;
    else if (action && strcmp(action, "restart") == 0)
        cmd = FF_POWER_RESTART;
    cJSON_Delete(root);
    if (cmd < 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "action must be on, off or restart");
    }
    /* v39: queued, never executed here — v38 ran on/off on the
     * HTTP task, and an absent strip froze the whole web UI. The
     * outcome shows up in GET /api/power (busy, then on/error). */
    if (ff_power_request((ff_power_cmd_t)cmd) != ESP_OK) {
        return send_conflict(req, "a power command is already running");
    }
    return send_json(req, "{\"ok\":true,\"queued\":true}");
}

/* v39: names FAT cannot store, and our own upload temporaries, are
 * refused up front instead of failing at fopen() time. */
static const char *upload_name_problem(const char *name)
{
    size_t n = strlen(name);
    if (n == 0) return "empty file name";
    if (n > 120) return "file name too long (120 characters max)";
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        if (*p < 0x20 || *p == 0x7f || strchr("<>:\"|?*", *p)) {
            return "file name contains a character FAT cannot store "
                   "(< > : \" | ? * or a control character)";
        }
    }
    if (name[n - 1] == '.' || name[n - 1] == ' ') {
        return "file name cannot end with a dot or space";
    }
    if (ff_store_is_temp_name(name)) {
        return "names ending in .part or .prev are reserved";
    }
    return NULL;
}

/* In-place percent-decoding for the upload ?name= value (query strings
 * arrive encoded; filenames like "My Disk.hfe" come in as My%20Disk).
 * Decoded UTF-8 bytes pass through untouched. Malformed % sequences
 * are copied verbatim rather than failing the upload. */
static void url_decode_in_place(char *s)
{
    char *r = s, *w = s;
    while (*r) {
        if (*r == '%' && r[1] && r[2]) {
            unsigned v = 0;
            bool hex_ok = true;
            for (int i = 1; i <= 2; i++) {
                v <<= 4;
                char c = r[i];
                if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
                else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
                else { hex_ok = false; break; }
            }
            if (hex_ok) { *w++ = (char)v; r += 3; continue; }
        }
        if (*r == '+') { *w++ = ' '; r++; continue; }
        *w++ = *r++;
    }
    *w = 0;
}

/* Body source for ff_store_write_stream: pull the next chunk of
 * the HTTP request body. httpd_req_recv returns 0 when the body
 * is complete, negative on socket error/timeout. */
typedef struct { httpd_req_t *req; } up_ctx_t;
static int upload_fill(void *vctx, uint8_t *buf, size_t cap)
{
    int n = httpd_req_recv(((up_ctx_t *)vctx)->req, (char *)buf, cap);
    return n < 0 ? -1 : n;
}

static esp_err_t h_upload(httpd_req_t *req)
{
    /* ?name=<file>: the client (the embedded UI) sends the file as
     * the RAW request body, not multipart form data.
     *
     * v37 direct-to-card: uploads only run while the drive is
     * EJECTED — the state where the card is locally mounted and
     * the ESP32 is its sole owner. The body streams through an
     * 8 KB buffer into <name>.part on the card and is renamed
     * into place on success, so there is no size limit but the
     * card itself, and an interrupted upload leaves nothing
     * FlashFloppy could try to mount. While presented, the
     * handler refuses with "eject the drive before uploading"
     * and the UI greys the button out. */
    char query[192];
    char name_raw[128];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "name", name_raw,
                              sizeof(name_raw)) != ESP_OK) {
        ESP_LOGW(TAG, "upload without usable ?name= (uri: %s)", req->uri);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "name required");
    }
    url_decode_in_place(name_raw);
    /* Basename only: no directories, no traversal off the card. */
    const char *name = strrchr(name_raw, '/');
    name = name ? name + 1 : name_raw;
    const char *bs = strrchr(name, '\\');
    if (bs) name = bs + 1;
    if (!name[0] || strstr(name, "..")) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad name");
    }
    const char *problem = upload_name_problem(name);
    if (problem) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, problem);
    }
    /* IMAGE_A.CFG belongs to FlashFloppy: it writes the file itself,
     * and accepting an upload of it would fight the selection
     * mechanism. INIT_A.CFG is likewise refused (selection here is
     * IMAGE_A-based). FF.CFG / IMG.CFG are configuration and ARE
     * accepted, under their canonical uppercase names. */
    if (strcasecmp(name, "FF.CFG") == 0) {
        name = "FF.CFG";
    } else if (strcasecmp(name, "IMG.CFG") == 0) {
        name = "IMG.CFG";
    } else if (strcasecmp(name, "IMAGE_A.CFG") == 0) {
        return httpd_resp_send_err(
            req, HTTPD_400_BAD_REQUEST,
            "IMAGE_A.CFG is written by FlashFloppy itself");
    } else if (strcasecmp(name, "INIT_A.CFG") == 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "INIT_A.CFG is not used by this design");
    }
    /* Claim the ejected state for the duration of the stream.
     * The check-and-set is atomic under the state lock, and
     * request_window() refuses to start a window while the claim
     * is held, so nothing else can touch the card mid-upload. */
    ff_state_lock();
    bool busy = g_ff.upload_active ||
                g_ff.state == FF_STATE_SWAP_PENDING ||
                g_ff.state == FF_STATE_DISCONNECTED ||
                g_ff.state == FF_STATE_APPLYING;
    bool ejected = g_ff.state == FF_STATE_IDLE && !g_ff.presented;
    if (!busy && ejected) g_ff.upload_active = true;
    ff_state_unlock();
    if (busy) {
        return send_conflict(req, "operation in progress");
    }
    if (!ejected) {
        return send_conflict(req,
                             "eject the drive before uploading");
    }
    up_ctx_t ctx = {req};
    size_t written = 0;
    esp_err_t err = ff_store_write_stream(name, upload_fill, &ctx,
                                          &written);
    if (err == ESP_OK) {
        /* The library snapshot should list the new file at once. */
        ff_store_refresh();
    }
    ff_state_lock();
    g_ff.upload_active = false;
    ff_state_unlock();
    if (err != ESP_OK) {
        ff_state_set_error("Upload of %.80s failed (card full, media "
                           "error or connection lost)", name);
        return httpd_resp_send_err(req,
                                   HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "upload failed - see status");
    }
    /* POWER.CFG / WIFI.CFG are read only at boot. */
    bool needs_reboot = strcasecmp(name, "POWER.CFG") == 0 ||
                        strcasecmp(name, "WIFI.CFG") == 0;
    return send_json(req, needs_reboot
                              ? "{\"ok\":true,\"reboot_needed\":true}"
                              : "{\"ok\":true}");
}

esp_err_t ff_http_start(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    /* The v01 upload handler kept ~5.4 KB of buffers on the httpd
     * task's stack and overflowed the 4096 default (LoadProhibited on
     * Kris's first upload). Handlers here allocate on the heap/PSRAM,
     * but keep the headroom anyway. */
    config.stack_size = 8192;
    /* HTTPD_DEFAULT_CONFIG allows only 8 URI handlers; we register
     * 9 since /api/power gained a POST twin in v28 — whose
     * registration silently failed until v29, so GET worked but
     * every POST came back 405 Method Not Allowed (Kris's log). */
    config.max_uri_handlers = 16;
    /* Direct uploads (v37) stream bodies of any size; give a slow
     * sender generous gaps between chunks before the socket is
     * treated as dead. */
    config.recv_wait_timeout = 30;
    ESP_ERROR_CHECK(httpd_start(&server, &config));
    const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = h_root},
        {.uri = "/api/status", .method = HTTP_GET, .handler = h_status},
        {.uri = "/api/library", .method = HTTP_GET, .handler = h_library},
        {.uri = "/api/select", .method = HTTP_POST, .handler = h_select},
        {.uri = "/api/eject", .method = HTTP_POST, .handler = h_eject},
        {.uri = "/api/reinsert", .method = HTTP_POST, .handler = h_reinsert},
        {.uri = "/api/force", .method = HTTP_POST, .handler = h_force},
        {.uri = "/api/upload", .method = HTTP_POST, .handler = h_upload},
        {.uri = "/api/power", .method = HTTP_GET, .handler = h_power_get},
        {.uri = "/api/power", .method = HTTP_POST, .handler = h_power_post},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        esp_err_t r = httpd_register_uri_handler(server, &uris[i]);
        if (r != ESP_OK) {
            ESP_LOGE(TAG, "URI handler %s failed to register: %s",
                     uris[i].uri, esp_err_to_name(r));
        }
    }
    ESP_LOGI(TAG, "HTTP server up");
    return ESP_OK;
}
