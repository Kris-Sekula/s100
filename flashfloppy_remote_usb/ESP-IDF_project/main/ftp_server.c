#include "ftp_server.h"
#include "board.h"
#include "state.h"
#include "storage.h"
#include "paths.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h> /* open()/read() for RETR (v10) */
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "esp_log.h"
#include "esp_heap_caps.h" /* DMA-capable RETR buffer (v10) */
#include "sdkconfig.h" /* CONFIG_LWIP_MAX_SOCKETS self-report */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ff_ftp";

/* A minimal RFC 959 server, hand-rolled in this codebase's
 * style. Deliberate limits (v07 in the generic remote-USB
 * project, ported here as v43): passive data connections only,
 * one session at a time, no REST/resume, no TLS. Windows'
 * built-in ftp.exe is active-mode only and will NOT work;
 * FileZilla / WinSCP / any modern client will.
 *
 * FlashFloppy divergences from the generic original: this
 * gadget has no folder concept — the store is root-only — so
 * MKD/RMD are refused, CWD only ever sits at the root, and
 * nested paths 550. The LCD FTP badge (v44, as in the generic
 * project's v17) is driven from here: ftp_gate() /
 * ftp_note_connected() set lcd_dirty and kick the apply task so
 * the badge appears (cyan), turns green while a transfer
 * streams, and clears on disconnect. The protected
 * set (paths.c) is IMAGE_A.CFG / INIT_A.CFG / FTP.CFG. */

/* ---- credentials (FTP.CFG, read once at start) ---- */
static char s_user[33] = FF_FTP_USER_DEFAULT;
static char s_pass[65] = FF_FTP_PASS_DEFAULT;
static bool s_pass_required = true;

static void read_ftp_cfg(void)
{
    FILE *f = fopen(FF_FTP_CFG_PATH, "r");
    if (!f) {
        ESP_LOGI(TAG, "no FTP.CFG: default user '%s'", s_user);
        return;
    }
    char line[160];
    bool have_pass = false;
    while (fgets(line, sizeof(line), f)) {
        ff_trim(line);
        if (!line[0] || line[0] == '#') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = line, *val = eq + 1;
        ff_trim(key);
        ff_trim(val);
        if (strcasecmp(key, "user") == 0 && val[0]) {
            ff_copy_field(s_user, sizeof(s_user), val);
        } else if (strcasecmp(key, "pass") == 0) {
            ff_copy_field(s_pass, sizeof(s_pass), val);
            have_pass = true;
        }
    }
    fclose(f);
    /* An explicitly empty pass= disables the password check. */
    if (have_pass && !s_pass[0]) s_pass_required = false;
    ESP_LOGI(TAG, "FTP.CFG: user '%s', password %s", s_user,
             s_pass_required ? "required" : "not required");
}

/* ---- session ---- */
typedef struct {
    int ctrl;        /* control socket */
    int pasv_listen; /* passive listener, -1 when none */
    bool user_ok;    /* USER named the configured user */
    bool logged_in;
    char cwd[FF_REL_PATH_MAX + 1]; /* normalised; "" = root */
    char rnfr[FF_REL_PATH_MAX + 1];
    bool rnfr_set;
} ftp_sess_t;

/* Single session at a time, so the session and its scratch
 * buffers are plain statics (this also keeps the task stack
 * small). */
static ftp_sess_t s_sess;
static char s_line[512];  /* one command line */
static char s_rbuf[768];  /* reply / listing assembly */

static bool send_all(int fd, const char *buf, size_t len)
{
    while (len) {
        ssize_t n = send(fd, buf, len, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        buf += n;
        len -= (size_t)n;
    }
    return true;
}

/* One reply line: "NNN text\r\n". */
static void reply(ftp_sess_t *s, int code, const char *fmt, ...)
{
    char body[640];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    int n = snprintf(s_rbuf, sizeof(s_rbuf), "%d %s\r\n", code, body);
    if (n > 0) send_all(s->ctrl, s_rbuf, (size_t)n);
}

/* Read one command line (CRLF or LF terminated) byte at a time
 * — control traffic is tiny and this stays obviously correct.
 * Returns the line length, or -1 when the connection is gone
 * (EOF, error, or the idle timeout fired). Overlong lines are
 * consumed and flagged so the caller can refuse them. */
static int read_line(ftp_sess_t *s, char *out, size_t cap, bool *too_long)
{
    size_t n = 0;
    *too_long = false;
    for (;;) {
        char c;
        ssize_t r = recv(s->ctrl, &c, 1, 0);
        if (r == 0) return -1;
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (c == '\n') break;
        if (c == '\r') continue;
        if (n + 1 < cap) {
            out[n++] = c;
        } else {
            *too_long = true;
        }
    }
    out[n] = 0;
    return (int)n;
}

/* ---- path resolution (the confinement surface) ----
 * FTP clients send paths absolute ("/a/b"), cwd-relative
 * ("b", "./b") or with dot segments ("../b"), none of which
 * ff_path_normalize() accepts as-is: it refuses "." and ".."
 * outright. Resolve the dot segments HERE, on a segment
 * stack, then let ff_path_normalize() validate the result —
 * so the caps, the illegal-character rules and the hidden-
 * name rule live in exactly one place (paths.c). ".." at the
 * root stays at the root, per FTP convention. An argument
 * starting with '/' is absolute; anything else is relative
 * to the session cwd. Returns FF_PATH_OK or an error code. */
static char s_segs[24][FF_COMPONENT_MAX + 1];

static int ftp_resolve(const char *cwd, const char *arg,
                       char *out, size_t outcap)
{
    int nseg = 0;
    const char *src[2];
    int nsrc = 0;
    if (arg && arg[0] == '/') {
        src[nsrc++] = arg; /* absolute: cwd plays no part */
    } else {
        if (cwd && cwd[0]) src[nsrc++] = cwd;
        if (arg) src[nsrc++] = arg;
    }
    for (int si = 0; si < nsrc; si++) {
        const char *p = src[si];
        while (*p) {
            while (*p == '/') p++;
            if (!*p) break;
            const char *slash = strchr(p, '/');
            size_t len = slash ? (size_t)(slash - p) : strlen(p);
            if (len == 1 && p[0] == '.') {
                /* "." — skip */
            } else if (len == 2 && p[0] == '.' && p[1] == '.') {
                if (nseg > 0) nseg--; /* ".." pops, root sticks */
            } else {
                if (len > FF_COMPONENT_MAX) {
                    return FF_PATH_ERR_COMPONENT;
                }
                if (nseg >= (int)(sizeof(s_segs) / sizeof(s_segs[0]))) {
                    return FF_PATH_ERR_TOO_DEEP;
                }
                memcpy(s_segs[nseg], p, len);
                s_segs[nseg][len] = 0;
                nseg++;
            }
            p = slash ? slash + 1 : p + len;
        }
    }
    /* Join and hand over to the one validator. A joined path
     * longer than FF_REL_PATH_MAX fails inside normalize. */
    char joined[512];
    size_t at = 0;
    joined[0] = 0;
    for (int i = 0; i < nseg; i++) {
        size_t l = strlen(s_segs[i]);
        if (at + (at ? 1 : 0) + l >= sizeof(joined)) {
            return FF_PATH_ERR_TOO_LONG;
        }
        if (at) joined[at++] = '/';
        memcpy(joined + at, s_segs[i], l + 1);
        at += l;
    }
    return ff_path_normalize(joined, out, outcap);
}

/* ---- snapshot lookups (browse side; any state) ---- */

/* Copy one snapshot entry out from under the state lock. */
static bool snap_get(const char *rel, ff_entry_t *out)
{
    bool found = false;
    ff_state_lock();
    for (int i = 0; i < ff_store_count(); i++) {
        const ff_entry_t *en = ff_store_entry(i);
        if (en && strcmp(en->name, rel) == 0) {
            *out = *en;
            found = true;
            break;
        }
    }
    ff_state_unlock();
    return found;
}

static bool snap_is_dir(const char *rel)
{
    /* Root-only store: no directory entries exist, so only the
     * root itself is a directory. */
    return !rel[0];
}

/* ---- card-ownership gate (mutation side) ---- */

/* Mutations and RETR read or write card bytes, so they need
 * the ejected state: state IDLE and not presented. With
 * claim=true the matching transfer flag is set atomically
 * (STOR/RETR hold it for the whole stream; window requests in
 * http_server.c refuse while either is set). Instant ops pass
 * claim=false — they take microseconds; the residual race with
 * a window starting mid-op is the one the v02 design already
 * accepted, and FatFs serialises the actual access.
 *
 * v43.1: the busy set is exactly SWAP_PENDING / DISCONNECTED /
 * APPLYING — the same three states the HTTP upload refuses on.
 * FF_STATE_PRESENTED is NOT busy: it is the steady presented
 * state (the apply task leaves it there after a re-insert, and
 * nothing ever sets it back to IDLE), so a plain `state != IDLE`
 * test here swallowed the presented branch and every FTP
 * mutation while presented came back "busy" instead of
 * "eject it first". Returns 0 when clear to proceed, else the
 * reply is sent and a nonzero code comes back. */
static int ftp_gate(ftp_sess_t *s, bool claim, bool upload)
{
    ff_state_lock();
    int rc = 0;
    if (g_ff.upload_active || g_ff.download_active) {
        rc = 2;
    } else if (g_ff.state == FF_STATE_SWAP_PENDING ||
               g_ff.state == FF_STATE_DISCONNECTED ||
               g_ff.state == FF_STATE_APPLYING) {
        rc = 2;
    } else if (g_ff.presented) {
        rc = 1;
    } else if (claim) {
        if (upload) {
            g_ff.upload_active = true;
        } else {
            g_ff.download_active = true;
        }
        g_ff.lcd_dirty = true; /* badge turns green (v44) */
    }
    ff_state_unlock();
    if (rc == 0 && claim) ff_apply_kick();
    if (rc == 1) {
        reply(s, 550, "The drive is presented to USB - eject it "
                      "from the web page first");
    } else if (rc == 2) {
        reply(s, 550, "The drive is busy - try again in a moment");
    }
    return rc;
}

static void ftp_unclaim(bool upload)
{
    ff_state_lock();
    if (upload) {
        g_ff.upload_active = false;
    } else {
        g_ff.download_active = false;
    }
    g_ff.lcd_dirty = true; /* badge back to idle cyan (v44) */
    ff_state_unlock();
    ff_apply_kick();
}

static void abs_path(char *out, size_t cap, const char *rel)
{
    if (rel && rel[0]) {
        snprintf(out, cap, "%s/%s", FF_SD_MOUNT_POINT, rel);
    } else {
        snprintf(out, cap, "%s", FF_SD_MOUNT_POINT);
    }
}

/* ---- passive data connections ---- */

static void pasv_close(ftp_sess_t *s)
{
    if (s->pasv_listen >= 0) {
        close(s->pasv_listen);
        s->pasv_listen = -1;
    }
}

static bool pasv_open(ftp_sess_t *s)
{
    pasv_close(s);
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        /* v07's first bench failed exactly here: errno 23
         * (ENFILE) when the lwIP socket table (default 10) was
         * exhausted by httpd sessions + listeners — the client
         * only saw "Cannot open the data port". Log the errno
         * so the next socket famine names itself. */
        ESP_LOGE(TAG, "PASV socket() failed: errno %d", errno);
        return false;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(FF_FTP_PASV_PORT);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
        ESP_LOGE(TAG, "PASV bind(%d) failed: errno %d",
                 FF_FTP_PASV_PORT, errno);
        close(fd);
        return false;
    }
    if (listen(fd, 4) != 0) {
        ESP_LOGE(TAG, "PASV listen failed: errno %d", errno);
        close(fd);
        return false;
    }
    s->pasv_listen = fd;
    return true;
}

/* Accept the pending data connection (the client connected
 * after PASV, or connects now that we sent 150). The listener
 * is consumed either way. Returns the data socket or -1.
 *
 * v09: this was a select() + accept() pair in v07/v08, and at
 * Kris's bench the data connection never landed ("Data
 * connection failed" = this function returning -1) even
 * though the listener opened fine. select() on a listening
 * socket is the one exotic call in the whole server — ESP-IDF
 * socket fds live behind the VFS fd offset, exactly the class
 * of thing select() implementations trip over — so the accept
 * is now the plain pattern the other ESP32 FTP servers use:
 * SO_RCVTIMEO on the LISTENER bounds a blocking accept()
 * (lwIP honours the timeout in accept). The outcome is logged
 * either way, so a failure names its errno. */
static int pasv_accept(ftp_sess_t *s)
{
    if (s->pasv_listen < 0) return -1;
    int lfd = s->pasv_listen;
    s->pasv_listen = -1;
    struct timeval tv = {15, 0};
    setsockopt(lfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct sockaddr_in peer;
    socklen_t plen = sizeof(peer);
    int data = accept(lfd, (struct sockaddr *)&peer, &plen);
    if (data < 0) {
        ESP_LOGE(TAG, "PASV accept failed: errno %d", errno);
    } else {
        unsigned ip = ntohl(peer.sin_addr.s_addr);
        ESP_LOGI(TAG, "data connection from %u.%u.%u.%u",
                 (ip >> 24) & 0xff, (ip >> 16) & 0xff,
                 (ip >> 8) & 0xff, ip & 0xff);
        struct timeval dto = {30, 0};
        setsockopt(data, SOL_SOCKET, SO_SNDTIMEO, &dto, sizeof(dto));
        setsockopt(data, SOL_SOCKET, SO_RCVTIMEO, &dto, sizeof(dto));
    }
    close(lfd);
    return data;
}

/* ---- LIST / NLST ---- */

static const char *const k_months[12] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

/* Build the listing for `dir` (or the single entry `dir` when
 * it names a file) from the snapshot into a malloc'd buffer.
 * Root-only: every snapshot entry is a file in the root, so a
 * directory target is always "" and a file target lists just
 * itself. The snapshot is read under the state lock; the send
 * happens later, unlocked. Worst case is the snapshot cap times
 * a maximal line — 48 KB is generous headroom; the build stops
 * cleanly rather than overflowing. */
static char *build_listing(const char *dir, bool names_only,
                           size_t *len_out)
{
    const size_t cap = 48 * 1024;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    size_t at = 0;
    buf[0] = 0;
    ff_state_lock();
    int n = ff_store_count();
    for (int i = 0; i < n && at + 256 < cap; i++) {
        const ff_entry_t *en = ff_store_entry(i);
        if (!en) continue;
        /* The target itself is listed only when it names a file;
         * a directory target (the root) lists all entries. */
        if (dir[0] && strcmp(en->name, dir) != 0) continue;
        if (names_only) {
            at += (size_t)snprintf(buf + at, cap - at, "%s\r\n",
                                   en->name);
        } else {
            time_t t = (time_t)en->mtime;
            struct tm tmv;
            gmtime_r(&t, &tmv);
            int mon = tmv.tm_mon;
            if (mon < 0 || mon > 11) mon = 0;
            at += (size_t)snprintf(
                buf + at, cap - at,
                "-rw-r--r-- 1 flashfloppy flashfloppy %10lu %s %2d "
                "%02d:%02d %s\r\n",
                (unsigned long)en->size, k_months[mon],
                tmv.tm_mday, tmv.tm_hour, tmv.tm_min, en->name);
        }
    }
    ff_state_unlock();
    *len_out = at;
    return buf;
}

/* Send a prepared listing over a fresh data connection. */
static void send_listing(ftp_sess_t *s, char *listing, size_t len)
{
    if (s->pasv_listen < 0) {
        reply(s, 425, "Use PASV first");
        free(listing);
        return;
    }
    reply(s, 150, "Opening data connection");
    int data = pasv_accept(s);
    if (data < 0) {
        reply(s, 425, "Data connection failed");
        free(listing);
        return;
    }
    bool ok = send_all(data, listing, len);
    close(data);
    free(listing);
    reply(s, ok ? 226 : 426,
          ok ? "Transfer complete" : "Transfer aborted");
}

/* ---- transfers ---- */

typedef struct { int fd; } stor_ctx_t;

static int stor_fill(void *vctx, uint8_t *buf, size_t cap)
{
    int fd = ((stor_ctx_t *)vctx)->fd;
    for (;;) {
        ssize_t n = recv(fd, buf, cap, 0);
        if (n > 0) return (int)n;
        if (n == 0) return 0;
        if (errno == EINTR) continue;
        return -1; /* includes the 30 s stall timeout */
    }
}

static void cmd_retr(ftp_sess_t *s, const char *rel)
{
    if (ff_path_is_protected(rel)) {
        reply(s, 550, "That file is protected");
        return;
    }
    ff_entry_t en;
    if (!snap_get(rel, &en)) {
        reply(s, 550, "No such file");
        return;
    }
    if (s->pasv_listen < 0) {
        reply(s, 425, "Use PASV first");
        return;
    }
    if (ftp_gate(s, true, false) != 0) return;
    char abs[192];
    abs_path(abs, sizeof(abs), rel);
    int fd = open(abs, O_RDONLY);
    if (fd < 0) {
        ftp_unclaim(false);
        reply(s, 550, "Could not open the file");
        return;
    }
    reply(s, 150, "Opening data connection");
    int data = pasv_accept(s);
    if (data < 0) {
        close(fd);
        ftp_unclaim(false);
        reply(s, 425, "Data connection failed");
        return;
    }
    /* v10: same DMA reasoning as the upload path in storage.c
     * — a DMA-capable buffer + raw read(), so sdmmc reads
     * need no per-chunk bounce allocation on downloads
     * either (the old stack buffer + fread path had the same
     * allocate_dma_buf exposure as uploads). */
    uint8_t *buf = heap_caps_malloc(4096, MALLOC_CAP_DMA |
                                          MALLOC_CAP_INTERNAL);
    if (!buf) buf = heap_caps_malloc(4096, MALLOC_CAP_DEFAULT);
    bool ok = buf != NULL;
    size_t total = 0;
    while (ok) {
        ssize_t n = read(fd, buf, 4096);
        if (n < 0) { ok = false; break; }
        if (n == 0) break;
        if (!send_all(data, (const char *)buf, (size_t)n)) {
            ok = false;
            break;
        }
        total += (size_t)n;
    }
    heap_caps_free(buf);
    close(fd);
    close(data);
    ftp_unclaim(false);
    ESP_LOGI(TAG, "RETR %s (%u bytes, %s)", rel, (unsigned)total,
             ok ? "ok" : "FAILED");
    reply(s, ok ? 226 : 426,
          ok ? "Transfer complete" : "Transfer aborted");
}

static void cmd_stor(ftp_sess_t *s, const char *rel)
{
    if (ff_path_is_protected(rel)) {
        reply(s, 550, "That file is protected");
        return;
    }
    /* Root-only store: a nested target would write into a folder
     * the snapshot never lists (an invisible orphan) — the parent
     * check below would even let it through when the folder
     * physically exists. Refuse up front instead. */
    if (strchr(rel, '/')) {
        reply(s, 550, "Folders are not supported on this gadget");
        return;
    }
    const char *problem = ff_store_name_problem(ff_path_basename(rel));
    if (problem) {
        reply(s, 550, "%s", problem);
        return;
    }
    if (s->pasv_listen < 0) {
        reply(s, 425, "Use PASV first");
        return;
    }
    if (ftp_gate(s, true, true) != 0) return;
    /* The target folder must exist on the card (checked now,
     * while we own it, rather than failing the .part create
     * after the client has connected). */
    char parent[FF_REL_PATH_MAX + 1];
    ff_path_parent(rel, parent, sizeof(parent));
    if (parent[0]) {
        char pabs[192];
        abs_path(pabs, sizeof(pabs), parent);
        struct stat st;
        if (stat(pabs, &st) != 0 || !S_ISDIR(st.st_mode)) {
            ftp_unclaim(true);
            reply(s, 550, "Target folder does not exist");
            return;
        }
    }
    reply(s, 150, "Opening data connection");
    int data = pasv_accept(s);
    if (data < 0) {
        ftp_unclaim(true);
        reply(s, 425, "Data connection failed");
        return;
    }
    stor_ctx_t ctx = {data};
    size_t written = 0;
    esp_err_t err = ff_store_write_stream(rel, stor_fill, &ctx,
                                          &written);
    close(data);
    ftp_unclaim(true);
    if (err != ESP_OK) {
        ff_state_set_error("FTP upload of %.80s failed (card "
                           "full, media error or connection lost)",
                           rel);
        reply(s, 451, "Upload failed - the partial file was "
                      "removed");
        return;
    }
    ff_store_refresh();
    reply(s, 226, "Transfer complete");
}

/* Resolve + protected-check shared by the instant mutations. */
static bool resolve_or_550(ftp_sess_t *s, const char *arg,
                           char *rel, size_t relcap)
{
    int rc = ftp_resolve(s->cwd, arg, rel, relcap);
    if (rc != FF_PATH_OK) {
        reply(s, 550, "%s", ff_path_err_str(rc));
        return false;
    }
    if (!rel[0]) {
        reply(s, 550, "That is the card root");
        return false;
    }
    if (ff_path_is_protected(rel)) {
        reply(s, 550, "That file is protected");
        return false;
    }
    return true;
}

/* ---- command dispatch ---- */

/* Commands that need no login. Everything else is refused
 * with 530 until USER/PASS succeeds. */
static bool needs_login(const char *verb)
{
    return strcmp(verb, "USER") != 0 && strcmp(verb, "PASS") != 0 &&
           strcmp(verb, "SYST") != 0 && strcmp(verb, "FEAT") != 0 &&
           strcmp(verb, "NOOP") != 0 && strcmp(verb, "QUIT") != 0 &&
           strcmp(verb, "AUTH") != 0;
}

static void handle_command(ftp_sess_t *s, char *line, bool *done)
{
    /* Split verb / arg at the first space. The arg keeps any
     * further spaces (folder names contain them). */
    char *arg = strchr(line, ' ');
    char verb[16];
    size_t vl;
    if (arg) {
        vl = (size_t)(arg - line);
        while (*arg == ' ') arg++;
    } else {
        vl = strlen(line);
        arg = line + vl;
    }
    if (vl >= sizeof(verb)) vl = sizeof(verb) - 1;
    memcpy(verb, line, vl);
    verb[vl] = 0;
    for (size_t i = 0; i < vl; i++) {
        if (verb[i] >= 'a' && verb[i] <= 'z') verb[i] -= 'a' - 'A';
    }
    /* Trailing spaces are never meaningful (our names cannot
     * end in one anyway). */
    size_t al = strlen(arg);
    while (al && arg[al - 1] == ' ') arg[--al] = 0;

    if (needs_login(verb) && !s->logged_in) {
        reply(s, 530, "Log in first");
        return;
    }

    if (strcmp(verb, "USER") == 0) {
        s->user_ok = arg[0] && strcmp(arg, s_user) == 0;
        s->logged_in = false;
        if (s->user_ok && !s_pass_required) {
            s->logged_in = true;
            reply(s, 230, "Logged in");
        } else {
            /* Same reply for a wrong user: don't leak which. */
            reply(s, 331, "Password required");
        }
    } else if (strcmp(verb, "PASS") == 0) {
        if (s->user_ok &&
            (!s_pass_required || strcmp(arg, s_pass) == 0)) {
            s->logged_in = true;
            reply(s, 230, "Logged in");
        } else {
            reply(s, 530, "Login incorrect");
        }
    } else if (strcmp(verb, "QUIT") == 0) {
        reply(s, 221, "Bye");
        *done = true;
    } else if (strcmp(verb, "SYST") == 0) {
        reply(s, 215, "UNIX Type: L8");
    } else if (strcmp(verb, "FEAT") == 0) {
        send_all(s->ctrl,
                 "211-Features:\r\n EPSV\r\n MDTM\r\n PASV\r\n"
                 " SIZE\r\n UTF8\r\n211 End\r\n",
                 strlen("211-Features:\r\n EPSV\r\n MDTM\r\n PASV\r\n"
                        " SIZE\r\n UTF8\r\n211 End\r\n"));
    } else if (strcmp(verb, "OPTS") == 0) {
        if (strncasecmp(arg, "UTF8", 4) == 0) {
            reply(s, 200, "OK");
        } else {
            reply(s, 502, "Unknown option");
        }
    } else if (strcmp(verb, "AUTH") == 0) {
        reply(s, 502, "Plain FTP only - no TLS");
    } else if (strcmp(verb, "NOOP") == 0) {
        reply(s, 200, "OK");
    } else if (strcmp(verb, "TYPE") == 0) {
        if (strcmp(arg, "I") == 0 || strcasecmp(arg, "L 8") == 0) {
            reply(s, 200, "Type set to I");
        } else if (arg[0] == 'A' || arg[0] == 'a') {
            /* We move bytes either way; ASCII mode's CRLF
             * translation is not implemented (nobody sane uses
             * it for a flash drive). */
            reply(s, 200, "Type set to A (transfers are binary)");
        } else {
            reply(s, 504, "Unknown type");
        }
    } else if (strcmp(verb, "MODE") == 0) {
        reply(s, strcasecmp(arg, "S") == 0 ? 200 : 504,
              strcasecmp(arg, "S") == 0 ? "Mode set to S"
                                        : "Unknown mode");
    } else if (strcmp(verb, "STRU") == 0) {
        reply(s, strcasecmp(arg, "F") == 0 ? 200 : 504,
              strcasecmp(arg, "F") == 0 ? "Structure set to F"
                                        : "Unknown structure");
    } else if (strcmp(verb, "PORT") == 0 || strcmp(verb, "EPRT") == 0) {
        reply(s, 502, "Active mode is not supported - use PASV");
    } else if (strcmp(verb, "PASV") == 0) {
        if (!pasv_open(s)) {
            reply(s, 425, "Cannot open the data port");
        } else {
            struct sockaddr_in la;
            socklen_t ll = sizeof(la);
            unsigned ip = 0;
            if (getsockname(s->ctrl, (struct sockaddr *)&la, &ll) == 0) {
                ip = ntohl(la.sin_addr.s_addr);
            }
            reply(s, 227,
                  "Entering Passive Mode (%u,%u,%u,%u,%u,%u)",
                  (ip >> 24) & 0xff, (ip >> 16) & 0xff,
                  (ip >> 8) & 0xff, ip & 0xff,
                  (FF_FTP_PASV_PORT >> 8) & 0xff,
                  FF_FTP_PASV_PORT & 0xff);
        }
    } else if (strcmp(verb, "EPSV") == 0) {
        if (!pasv_open(s)) {
            reply(s, 425, "Cannot open the data port");
        } else {
            reply(s, 229, "Entering Extended Passive Mode (|||%d|)",
                  FF_FTP_PASV_PORT);
        }
    } else if (strcmp(verb, "PWD") == 0 ||
               strcmp(verb, "XPWD") == 0) {
        reply(s, 257, "\"/%s\" is the current directory", s->cwd);
    } else if (strcmp(verb, "CWD") == 0 ||
               strcmp(verb, "XCWD") == 0) {
        char rel[FF_REL_PATH_MAX + 1];
        int rc = ftp_resolve(s->cwd, arg, rel, sizeof(rel));
        if (rc != FF_PATH_OK) {
            reply(s, 550, "%s", ff_path_err_str(rc));
        } else if (!snap_is_dir(rel)) {
            reply(s, 550, "No such folder");
        } else {
            ff_copy_field(s->cwd, sizeof(s->cwd), rel);
            s->rnfr_set = false;
            reply(s, 250, "Directory changed");
        }
    } else if (strcmp(verb, "CDUP") == 0 ||
               strcmp(verb, "XCUP") == 0) {
        char parent[FF_REL_PATH_MAX + 1];
        if (ff_path_parent(s->cwd, parent, sizeof(parent))) {
            ff_copy_field(s->cwd, sizeof(s->cwd), parent);
        } else {
            s->cwd[0] = 0;
        }
        reply(s, 250, "Directory changed");
    } else if (strcmp(verb, "LIST") == 0 ||
               strcmp(verb, "NLST") == 0) {
        bool names = verb[0] == 'N';
        /* An arg starting with '-' is client options (-la
         * etc.), not a path. */
        const char *a = (arg[0] == '-') ? "" : arg;
        char rel[FF_REL_PATH_MAX + 1];
        int rc = ftp_resolve(s->cwd, a, rel, sizeof(rel));
        if (rc != FF_PATH_OK) {
            reply(s, 550, "%s", ff_path_err_str(rc));
        } else if (!snap_is_dir(rel) && !snap_get(rel, &(ff_entry_t){0})) {
            reply(s, 550, "No such file or folder");
        } else {
            size_t len = 0;
            char *listing = build_listing(rel, names, &len);
            if (!listing) {
                reply(s, 451, "Out of memory");
            } else {
                send_listing(s, listing, len);
            }
        }
    } else if (strcmp(verb, "RETR") == 0) {
        char rel[FF_REL_PATH_MAX + 1];
        int rc = ftp_resolve(s->cwd, arg, rel, sizeof(rel));
        if (rc != FF_PATH_OK) {
            reply(s, 550, "%s", ff_path_err_str(rc));
        } else if (!rel[0]) {
            reply(s, 550, "That is the card root");
        } else {
            cmd_retr(s, rel);
        }
    } else if (strcmp(verb, "STOR") == 0) {
        char rel[FF_REL_PATH_MAX + 1];
        int rc = ftp_resolve(s->cwd, arg, rel, sizeof(rel));
        if (rc != FF_PATH_OK) {
            reply(s, 550, "%s", ff_path_err_str(rc));
        } else if (!rel[0]) {
            reply(s, 550, "That is the card root");
        } else {
            cmd_stor(s, rel);
        }
    } else if (strcmp(verb, "DELE") == 0) {
        char rel[FF_REL_PATH_MAX + 1];
        if (!resolve_or_550(s, arg, rel, sizeof(rel))) return;
        ff_entry_t en;
        if (!snap_get(rel, &en)) {
            reply(s, 550, "No such file");
        } else if (ftp_gate(s, false, false) == 0) {
            char abs[192];
            abs_path(abs, sizeof(abs), rel);
            if (unlink(abs) != 0) {
                reply(s, 550, "Delete failed");
            } else {
                ESP_LOGI(TAG, "DELE %s", rel);
                ff_store_refresh();
                reply(s, 250, "Deleted");
            }
        }
    } else if (strcmp(verb, "MKD") == 0 ||
               strcmp(verb, "XMKD") == 0) {
        /* Root-only store: no folders on this gadget. */
        reply(s, 550, "Folders are not supported on this gadget");
    } else if (strcmp(verb, "RMD") == 0 ||
               strcmp(verb, "XRMD") == 0) {
        reply(s, 550, "Folders are not supported on this gadget");
    } else if (strcmp(verb, "RNFR") == 0) {
        char rel[FF_REL_PATH_MAX + 1];
        if (!resolve_or_550(s, arg, rel, sizeof(rel))) return;
        if (!snap_get(rel, &(ff_entry_t){0})) {
            reply(s, 550, "No such file or folder");
        } else {
            ff_copy_field(s->rnfr, sizeof(s->rnfr), rel);
            s->rnfr_set = true;
            reply(s, 350, "Ready for RNTO");
        }
    } else if (strcmp(verb, "RNTO") == 0) {
        if (!s->rnfr_set) {
            reply(s, 503, "Send RNFR first");
            return;
        }
        char rel[FF_REL_PATH_MAX + 1];
        bool ok = resolve_or_550(s, arg, rel, sizeof(rel));
        /* Root-only store: same orphan rule as STOR (see
         * cmd_stor) — a nested rename target would land in a
         * folder the snapshot never lists. */
        if (ok && strchr(rel, '/')) {
            reply(s, 550, "Folders are not supported on this gadget");
            ok = false;
        }
        if (ok && ftp_gate(s, false, false) == 0) {
            char err[64] = {0};
            if (ff_store_rename(s->rnfr, rel, err,
                                sizeof(err)) == ESP_OK) {
                ff_store_refresh();
                reply(s, 250, "Renamed");
            } else {
                reply(s, 550, "%s", err[0] ? err : "rename failed");
            }
        }
        s->rnfr_set = false;
    } else if (strcmp(verb, "SIZE") == 0) {
        char rel[FF_REL_PATH_MAX + 1];
        int rc = ftp_resolve(s->cwd, arg, rel, sizeof(rel));
        ff_entry_t en;
        if (rc != FF_PATH_OK) {
            reply(s, 550, "%s", ff_path_err_str(rc));
        } else if (!snap_get(rel, &en)) {
            reply(s, 550, "No such file");
        } else {
            reply(s, 213, "%lu", (unsigned long)en.size);
        }
    } else if (strcmp(verb, "MDTM") == 0) {
        char rel[FF_REL_PATH_MAX + 1];
        int rc = ftp_resolve(s->cwd, arg, rel, sizeof(rel));
        ff_entry_t en;
        if (rc != FF_PATH_OK) {
            reply(s, 550, "%s", ff_path_err_str(rc));
        } else if (!snap_get(rel, &en)) {
            reply(s, 550, "No such file");
        } else {
            time_t t = (time_t)en.mtime;
            struct tm tmv;
            gmtime_r(&t, &tmv);
            reply(s, 213, "%04d%02d%02d%02d%02d%02d",
                  tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                  tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
        }
    } else if (strcmp(verb, "REST") == 0) {
        reply(s, 502, "Resume is not supported");
    } else if (strcmp(verb, "APPE") == 0) {
        reply(s, 502, "Append is not supported - use STOR");
    } else if (strcmp(verb, "ABOR") == 0) {
        reply(s, 226, "Nothing to abort");
    } else {
        reply(s, 502, "Not implemented");
    }
}

/* ---- server task ---- */

static void run_session(int ctrl)
{
    ftp_sess_t *s = &s_sess;
    memset(s, 0, sizeof(*s));
    s->ctrl = ctrl;
    s->pasv_listen = -1;
    struct timeval tv = {300, 0}; /* 5 min idle closes the session */
    setsockopt(ctrl, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct timeval stv = {30, 0};
    setsockopt(ctrl, SOL_SOCKET, SO_SNDTIMEO, &stv, sizeof(stv));
    reply(s, 220, "FlashFloppy FTP ready");
    bool done = false;
    while (!done) {
        bool too_long = false;
        int n = read_line(s, s_line, sizeof(s_line), &too_long);
        if (n < 0) break;
        if (too_long) {
            reply(s, 500, "Command line too long");
            continue;
        }
        if (n == 0) continue;
        handle_command(s, s_line, &done);
    }
    pasv_close(s);
    close(ctrl);
    ESP_LOGI(TAG, "FTP session ended");
}

/* Publish the session state for /api/status and the LCD badge
 * (v44). The dirty flag makes the apply task repaint, the same
 * mechanism a Wi-Fi address change uses; the kick makes it
 * prompt. */
static void ftp_note_connected(bool connected)
{
    ff_state_lock();
    g_ff.ftp_connected = connected;
    g_ff.lcd_dirty = true;
    ff_state_unlock();
    ff_apply_kick();
}

static void ftp_task(void *arg)
{
    (void)arg;
    int srv = -1;
    for (;;) {
        if (srv < 0) {
            srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (srv < 0) {
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
            int one = 1;
            setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one,
                       sizeof(one));
            struct sockaddr_in a = {0};
            a.sin_family = AF_INET;
            a.sin_addr.s_addr = htonl(INADDR_ANY);
            a.sin_port = htons(FF_FTP_PORT);
            if (bind(srv, (struct sockaddr *)&a, sizeof(a)) != 0 ||
                listen(srv, 4) != 0) {
                ESP_LOGE(TAG, "cannot bind port %d; retrying",
                         FF_FTP_PORT);
                close(srv);
                srv = -1;
                vTaskDelay(pdMS_TO_TICKS(5000));
                continue;
            }
            ESP_LOGI(TAG, "FTP server listening on port %d "
                          "(passive data port %d)",
                     FF_FTP_PORT, FF_FTP_PASV_PORT);
        }
        int ctrl = accept(srv, NULL, NULL);
        if (ctrl < 0) {
            if (errno == EINTR) continue;
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        ESP_LOGI(TAG, "FTP session started");
        ftp_note_connected(true);
        run_session(ctrl);
        ftp_note_connected(false);
    }
}

esp_err_t ff_ftp_start(void)
{
    read_ftp_cfg();
    /* v09: report the socket budget the build ACTUALLY got.
     * The v07/v08 ENFILE saga (httpd accept errno 23, PASV
     * failures) hinged on whether CONFIG_LWIP_MAX_SOCKETS=16
     * from sdkconfig.defaults survived Kris's sdkconfig — a
     * question that took two bench rounds to ask. Now the boot
     * log answers it unasked. */
#ifdef CONFIG_LWIP_MAX_SOCKETS
    ESP_LOGI(TAG, "socket budget: CONFIG_LWIP_MAX_SOCKETS=%d",
             CONFIG_LWIP_MAX_SOCKETS);
#else
    ESP_LOGW(TAG, "CONFIG_LWIP_MAX_SOCKETS not in sdkconfig.h "
                  "(lwIP default applies)");
#endif
    /* DMA heap baseline at boot, the reference point for the
     * RETR buffer below and the "upload start" line in
     * ff_store_write_stream (sdmmc reads/writes need DMA-capable
     * buffers; a plain stack buffer is the old failure mode). */
    ESP_LOGI(TAG, "DMA baseline: free %u, largest %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(
                 MALLOC_CAP_DMA));
    /* 8192: the session's call chain (stdio over FatFs, socket
     * transfers, the 24-segment resolver scratch is static) —
     * same headroom reasoning as the apply task. */
    if (xTaskCreate(ftp_task, "ff_ftp", 8192, NULL, 5, NULL) !=
        pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
