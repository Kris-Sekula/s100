#include "paths.h"
#include <string.h>
#include <strings.h>

/* See paths.h for the rules. Pure string logic: no allocation,
 * no filesystem access, host-testable as-is. */

static bool seg_bad_char(char c)
{
    const unsigned char u = (unsigned char)c;
    if (u < 0x20 || u == 0x7f) return true;
    return strchr("<>:\"|?*", c) != NULL;
}

int ff_path_check_component(const char *name)
{
    if (!name) return FF_PATH_ERR_NULL;
    size_t n = strlen(name);
    if (n == 0) return FF_PATH_ERR_EMPTY_SEGMENT;
    if (strcmp(name, "..") == 0) return FF_PATH_ERR_DOTDOT;
    if (strcmp(name, ".") == 0) return FF_PATH_ERR_DOT;
    if (name[0] == '.') return FF_PATH_ERR_HIDDEN;
    if (strchr(name, '\\')) return FF_PATH_ERR_BACKSLASH;
    if (strchr(name, '/')) return FF_PATH_ERR_BAD_CHAR;
    for (size_t i = 0; i < n; i++) {
        if (seg_bad_char(name[i])) return FF_PATH_ERR_BAD_CHAR;
    }
    if (name[n - 1] == '.' || name[n - 1] == ' ') {
        return FF_PATH_ERR_BAD_CHAR;
    }
    if (n > FF_COMPONENT_MAX) return FF_PATH_ERR_COMPONENT;
    return FF_PATH_OK;
}

int ff_path_normalize(const char *in, char *out, size_t outcap)
{
    if (!in || !out) return FF_PATH_ERR_NULL;
    if (strchr(in, '\\')) return FF_PATH_ERR_BACKSLASH;

    /* Work into a private buffer first: `out` stays untouched on
     * error, and total-length accounting is exact. */
    char tmp[FF_REL_PATH_MAX + 1];
    size_t tlen = 0;
    tmp[0] = 0;
    int depth = 0;

    const char *p = in;
    if (*p == '/') p++; /* tolerate one leading slash */
    if (*p == 0) {      /* "" or "/" is the root */
        if (outcap < 1) return FF_PATH_ERR_TOO_LONG;
        out[0] = 0;
        return FF_PATH_OK;
    }
    while (*p) {
        const char *slash = strchr(p, '/');
        size_t seglen = slash ? (size_t)(slash - p) : strlen(p);
        if (seglen == 0) return FF_PATH_ERR_EMPTY_SEGMENT;
        /* Validate the segment in isolation. A NUL-terminated
         * copy keeps the component checker simple; its length
         * bound is checked first, in place. */
        if (seglen > FF_COMPONENT_MAX) return FF_PATH_ERR_COMPONENT;
        char seg[FF_COMPONENT_MAX + 1];
        memcpy(seg, p, seglen);
        seg[seglen] = 0;
        int rc = ff_path_check_component(seg);
        if (rc != FF_PATH_OK) return rc;
        depth++;
        if (depth > FF_MAX_DEPTH) return FF_PATH_ERR_TOO_DEEP;
        size_t add = seglen + (tlen ? 1 : 0);
        if (tlen + add > FF_REL_PATH_MAX) return FF_PATH_ERR_TOO_LONG;
        if (tlen) tmp[tlen++] = '/';
        memcpy(tmp + tlen, seg, seglen);
        tlen += seglen;
        tmp[tlen] = 0;
        if (!slash) break;
        p = slash + 1;
        /* A trailing slash leaves *p == 0 and the loop ends;
         * "//" yields a zero-length segment above. */
    }
    if (outcap < tlen + 1) return FF_PATH_ERR_TOO_LONG;
    memcpy(out, tmp, tlen + 1);
    return FF_PATH_OK;
}

const char *ff_path_err_str(int code)
{
    switch (code) {
        case FF_PATH_OK: return "ok";
        case FF_PATH_ERR_NULL: return "missing path";
        case FF_PATH_ERR_DOTDOT: return "path may not contain '..'";
        case FF_PATH_ERR_DOT: return "path may not contain '.' segments";
        case FF_PATH_ERR_EMPTY_SEGMENT:
            return "path may not contain empty segments ('//')";
        case FF_PATH_ERR_BACKSLASH:
            return "backslashes are not allowed in paths";
        case FF_PATH_ERR_BAD_CHAR:
            return "path contains a character FAT cannot store "
                   "(< > : \" | ? * or a control character), or a "
                   "name ending in a dot or space";
        case FF_PATH_ERR_HIDDEN:
            return "hidden names (starting with '.') are not allowed";
        case FF_PATH_ERR_COMPONENT: return "a path name is too long";
        case FF_PATH_ERR_TOO_LONG: return "path is too long";
        case FF_PATH_ERR_TOO_DEEP: return "path is nested too deep";
        default: return "bad path";
    }
}

bool ff_path_is_protected(const char *normalized)
{
    if (!normalized || strchr(normalized, '/')) return false;
    /* FlashFloppy (v43): the web upload refuses IMAGE_A.CFG
     * ("written by FlashFloppy itself") and INIT_A.CFG the same
     * way, and FTP.CFG is read once by the FTP server at boot —
     * an FTP overwrite would silently not take effect until a
     * reboot. WIFI.CFG / POWER.CFG are NOT protected: the web UI
     * accepts them with reboot_needed, and FTP is the same deal. */
    return strcasecmp(normalized, "IMAGE_A.CFG") == 0 ||
           strcasecmp(normalized, "INIT_A.CFG") == 0 ||
           strcasecmp(normalized, "FTP.CFG") == 0;
}

bool ff_path_is_within(const char *path, const char *ancestor)
{
    if (!path || !ancestor) return false;
    if (ancestor[0] == 0) return true; /* the root contains all */
    size_t n = strlen(ancestor);
    if (strncmp(path, ancestor, n) != 0) return false;
    return path[n] == 0 || path[n] == '/';
}

bool ff_path_parent(const char *normalized, char *out, size_t outcap)
{
    if (!normalized || !out) return false;
    const char *slash = strrchr(normalized, '/');
    size_t n = slash ? (size_t)(slash - normalized) : 0;
    if (outcap < n + 1) return false;
    memcpy(out, normalized, n);
    out[n] = 0;
    return true;
}

const char *ff_path_basename(const char *normalized)
{
    if (!normalized) return "";
    const char *slash = strrchr(normalized, '/');
    return slash ? slash + 1 : normalized;
}

int ff_path_join(const char *dir_normalized, const char *component,
                 char *out, size_t outcap)
{
    if (!dir_normalized || !out) return FF_PATH_ERR_NULL;
    int rc = ff_path_check_component(component);
    if (rc != FF_PATH_OK) return rc;
    /* Depth of the result: the dir's depth plus one. */
    int depth = 1;
    if (dir_normalized[0]) {
        depth = 1;
        for (const char *p = dir_normalized; *p; p++) {
            if (*p == '/') depth++;
        }
        depth++;
    }
    if (depth > FF_MAX_DEPTH) return FF_PATH_ERR_TOO_DEEP;
    size_t dl = strlen(dir_normalized), cl = strlen(component);
    size_t total = dl + (dl ? 1 : 0) + cl;
    if (total > FF_REL_PATH_MAX || outcap < total + 1) {
        return FF_PATH_ERR_TOO_LONG;
    }
    memcpy(out, dir_normalized, dl);
    size_t at = dl;
    if (dl) out[at++] = '/';
    memcpy(out + at, component, cl + 1);
    return FF_PATH_OK;
}
