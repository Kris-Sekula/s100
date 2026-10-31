#include "http_server.h"
#include "board.h"
#include "state.h"
#include "storage.h"
#include "power.h"
#include "web_ui.h"
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

static esp_err_t h_status(httpd_req_t *req)
{
    char buf[768];
    bool ejected = !g_ff.presented && g_ff.state == FF_STATE_IDLE;
    snprintf(buf, sizeof(buf),
             "{\"current\":\"%s\",\"state\":\"%s\",\"presented\":%s,"
             "\"ejected\":%s,\"uploading\":%s,\"wifi\":\"%s\"}",
             g_ff.current_path, ff_state_name(g_ff.state),
             g_ff.presented ? "true" : "false",
             ejected ? "true" : "false",
             g_ff.upload_active ? "true" : "false",
             g_ff.wifi_desc);
    return send_json(req, buf);
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
     * own directory order is kept. */
    for (int pass = 0; pass < 3; pass++) {
    for (int i = 0; i < ff_store_count(); i++) {
        const ff_entry_t *en = ff_store_entry(i);
        if (!en) continue;
        int rank = en->kind == FF_KIND_IMAGE ? 0
                   : en->kind == FF_KIND_BACKUP ? 1 : 2;
        if (rank != pass) continue;
        cJSON *f = cJSON_CreateObject();
        cJSON_AddStringToObject(f, "name", en->name);
        /* Since v18 everything lives in the root, so the path the
         * UI sends back on Select is simply the filename — the same
         * string IMAGE_A.CFG will hold. */
        cJSON_AddStringToObject(f, "path", en->name);
        cJSON_AddNumberToObject(f, "size", en->size);
        cJSON_AddStringToObject(f, "kind",
                                en->kind == FF_KIND_CONFIG ? "config"
                                : en->kind == FF_KIND_BACKUP ? "backup"
                                                             : "image");
        cJSON_AddItemToArray(files, f);
    }
    } /* end passes */
    char *json = cJSON_PrintUnformatted(root);
    esp_err_t err = send_json(req, json);
    cJSON_free(json);
    cJSON_Delete(root);
    return err;
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
    g_ff.state = FF_STATE_SWAP_PENDING;
    ff_state_unlock();
    return send_json(req, "{\"ok\":true}");
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
    bool ok = false;
    for (int i = 0; i < ff_store_count(); i++) {
        const ff_entry_t *en = ff_store_entry(i);
        if (!en || en->kind != FF_KIND_IMAGE) continue;
        if (strcmp(path->valuestring, en->name) == 0) { ok = true; break; }
    }
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

/* Kasa power control (v28): the strip feeding the host machine.
 * GET reports a live query; POST takes {"action":"on|off|restart"}. */
static esp_err_t h_power_get(httpd_req_t *req)
{
    if (!ff_power_configured()) {
        return send_json(req, "{\"configured\":false}");
    }
    /* Served from the poll cache (v36): the background task keeps
     * it within FF_POWER_POLL_MS of the truth, so this handler
     * never blocks on the strip. */
    bool on = false, reachable = false;
    float watts = 0;
    ff_power_cached_state(&on, &watts, &reachable);
    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"configured\":true,\"name\":\"%s\",\"reachable\":%s,"
             "\"on\":%s,\"watts\":%.1f,\"restarting\":%s}",
             ff_power_name(), reachable ? "true" : "false",
             on ? "true" : "false", (double)watts,
             ff_power_restarting() ? "true" : "false");
    return send_json(req, buf);
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
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (action && strcmp(action, "on") == 0) {
        err = ff_power_set(true);
    } else if (action && strcmp(action, "off") == 0) {
        err = ff_power_set(false);
    } else if (action && strcmp(action, "restart") == 0) {
        ff_power_restart_async();
        err = ESP_OK;
    }
    cJSON_Delete(root);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "power command failed");
    }
    return send_json(req, "{\"ok\":true}");
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
    /* IMAGE_A.CFG belongs to FlashFloppy: it writes the file itself,
     * and accepting an upload of it would fight the selection
     * mechanism. INIT_A.CFG is likewise refused (selection here is
     * IMAGE_A-based). FF.CFG / IMG.CFG are configuration and ARE
     * accepted, under their canonical uppercase names. */
    static char canon[128];
    if (strcasecmp(name, "FF.CFG") == 0) {
        strcpy(canon, "FF.CFG");
        name = canon;
    } else if (strcasecmp(name, "IMG.CFG") == 0) {
        strcpy(canon, "IMG.CFG");
        name = canon;
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
        return httpd_resp_send_err(req,
                                   HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "upload failed");
    }
    return send_json(req, "{\"ok\":true}");
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
