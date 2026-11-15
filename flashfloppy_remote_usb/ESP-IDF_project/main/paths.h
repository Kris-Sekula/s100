#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Path normalisation / confinement for the FTP server (v43,
 * ported from the generic remote-USB project).
 *
 * Every FTP command passes its user-supplied path through
 * ff_path_normalize() before anything else, and only ever hands
 * the NORMALISED result to the filesystem layer, joined under
 * FF_SD_MOUNT_POINT. This module is pure string logic (no ESP or
 * filesystem dependencies) so it can be unit-tested on the host.
 * The store itself is root-only (no folders): nested paths
 * normalise fine but match nothing in the snapshot, so FTP
 * commands on them 550 — the confinement rules below are still
 * the one place that decides what a path may look like.
 *
 * Canonical form: '/'-separated relative path with no leading or
 * trailing slash; the card root is the empty string "".
 *
 * Rules (the research calls path safety the #1 risk of the whole
 * file-manager feature):
 *  - ".." segments are rejected outright (never resolved).
 *  - "." segments are rejected (canonical paths never need them).
 *  - Empty segments are rejected ("a//b"); one leading '/' and
 *    one trailing '/' on the whole input are tolerated.
 *  - Backslashes are ILLEGAL characters, not separators: a
 *    Windows-style "a\b" is a bad name, never a traversal.
 *  - Control characters and the FAT-illegal set < > : " | ? *
 *    are rejected in any segment; a segment may not end in a
 *    dot or a space (FAT cannot store that faithfully).
 *  - Segments starting with '.' are rejected: dotfiles are the
 *    card readers' housekeeping and are never manager targets
 *    (this also keeps upload temporaries unreachable).
 *  - Caps: FF_REL_PATH_MAX chars total, FF_COMPONENT_MAX chars
 *    per segment, FF_MAX_DEPTH segments deep.
 *
 * Config-file protection: the root files "IMAGE_A.CFG",
 * "INIT_A.CFG" and "FTP.CFG" (any case) are protected — see
 * ff_path_is_protected(). IMAGE_A.CFG / INIT_A.CFG match the web
 * upload's refusals; FTP.CFG is read once by the FTP server at
 * boot, so an FTP overwrite would silently not take effect.
 * WIFI.CFG / POWER.CFG are NOT protected: the web UI accepts them
 * (reboot to apply) and FTP is the same deal. A file with the
 * same name inside a subfolder would be an ordinary file — but
 * this gadget has no folders, so that case never arises. */

#define FF_REL_PATH_MAX 160 /* chars in a normalised relative path */
#define FF_COMPONENT_MAX 120 /* chars in one path segment */
#define FF_MAX_DEPTH 8       /* segments in a normalised path */

/* ff_path_normalize() result codes. */
enum {
    FF_PATH_OK = 0,
    FF_PATH_ERR_NULL,          /* NULL argument */
    FF_PATH_ERR_DOTDOT,        /* a ".." segment */
    FF_PATH_ERR_DOT,           /* a "." segment */
    FF_PATH_ERR_EMPTY_SEGMENT, /* "//" inside the path */
    FF_PATH_ERR_BACKSLASH,     /* '\' anywhere in the input */
    FF_PATH_ERR_BAD_CHAR,      /* control char, FAT-illegal char,
                                  or a segment ending in '.'/' ' */
    FF_PATH_ERR_HIDDEN,        /* a segment starting with '.' */
    FF_PATH_ERR_COMPONENT,     /* one segment over FF_COMPONENT_MAX */
    FF_PATH_ERR_TOO_LONG,      /* whole path over FF_REL_PATH_MAX */
    FF_PATH_ERR_TOO_DEEP,      /* more than FF_MAX_DEPTH segments */
};

/* Normalise `in` into `out` (capacity `outcap`, which must be at
 * least FF_REL_PATH_MAX + 1 for success to be possible). Returns
 * FF_PATH_OK and writes the canonical path ("" for the root), or
 * one of the FF_PATH_ERR_* codes; on error `out` is untouched. */
int ff_path_normalize(const char *in, char *out, size_t outcap);

/* Human-readable text for a result code, for API error bodies. */
const char *ff_path_err_str(int code);

/* Check one path segment (a file or folder name, no slashes)
 * against the character/length rules above. FF_PATH_OK or an
 * error code. Used for upload file names landing in a folder. */
int ff_path_check_component(const char *name);

/* True when the NORMALISED path is a protected root config file
 * (WIFI.CFG or FTP.CFG, case-insensitive, exactly one segment). */
bool ff_path_is_protected(const char *normalized);

/* True when `path` equals `ancestor` or lives underneath it
 * (both normalised). Used to refuse moving a folder into its own
 * subtree. A root ancestor ("") contains everything. */
bool ff_path_is_within(const char *path, const char *ancestor);

/* Parent of a NORMALISED path: writes the parent ("" for a root
 * entry) into `out`. Returns false only on bad arguments or a
 * too-small buffer. */
bool ff_path_parent(const char *normalized, char *out, size_t outcap);

/* Basename of a NORMALISED path: pointer into the string (after
 * the last '/'), or the string itself for a root entry. */
const char *ff_path_basename(const char *normalized);

/* Join a NORMALISED directory ("" for root) and a single valid
 * component into `out`. Returns FF_PATH_OK or an error code
 * (component rules, length cap). */
int ff_path_join(const char *dir_normalized, const char *component,
                 char *out, size_t outcap);
