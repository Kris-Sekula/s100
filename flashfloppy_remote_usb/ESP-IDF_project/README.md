# FlashFloppy ESP32 frontend — ESP-IDF skeleton

**Target board:** Waveshare ESP32-S3-LCD-1.47 (USB-A dongle, non-touch variant,
from the schematic Kris linked 2026-10-28).
**Target framework:** ESP-IDF 5.5.6 (updated 2026-10-29 — the skeleton
was originally written against 5.3, but every API it uses is stable in
5.5, and 5.5 has the longest remaining support runway of the 5.3/5.4/5.5
options), with `espressif/esp_tinyusb` pinned below 2.0.
**Status: skeleton, NOT bench-tested, NOT compile-verified on this machine.**
There is no ESP-IDF toolchain on the machine that wrote this, so the first
build on a machine with ESP-IDF 5.5.6 installed is expected to surface small
API mismatches. The one integration point most likely to bite is the MSC
plumbing — see "First build task" below. Do not treat this as working
firmware until it has enumerated on a PC and then on a Gotek.

**Build fix, 2026-10-29 (first real compile, IDF 5.5.6):** `sdcard.c`
failed with `SDMMC_HOST_DEFAULT` / `sdmmc_slot_config_t` undeclared. In
IDF 5.4+ the SDMMC *host* driver lives in the separate `esp_driver_sdmmc`
component (`driver/sdmmc_host.h`); `sdmmc_cmd.h` alone is only the
protocol layer. Fixed by including that header and adding
`esp_driver_sdmmc` to `REQUIRES` in `main/CMakeLists.txt` (the legacy
umbrella `driver` requirement was dropped). Same-pass audit fixes:
`stdlib.h`/`strings.h` in `http_server.c`, `stdio.h` and a runtime
SSID/password copy in `main.c`, and removal of a TinyUSB internal MSC
header include in `msc_device.c`.

**Build fix, 2026-10-29 (second compile):** good progress — the build
reached step 1100/1113, with `sdcard.c` and `main.c` now compiling. Three
errors remained, all in code written here: (1) `web_ui.h` — a glob pattern
inside a block comment contains the two characters `/*`, which
`-Werror=comment` rejects; same bug in the `msc_device.c` comment added
by the first fix — both comments reworded, and a scan of all comments
for a nested `/*` now comes back clean; (2) `http_server.c` used
`HTTPD_409_CONFLICT`, which does not exist — ESP-IDF's
`httpd_err_code_t` jumps from 408 to 500, so a real 409 is now sent via
a custom status line in a new `send_conflict()` helper. Next untested
stage is linking, where the predicted `esp_tinyusb` MSC callback
question (duplicate `tud_msc` symbols) would show up if it is real.

**v02, 2026-10-29 (first hardware run of v01):** v01 built, flashed and
booted on the real board: SD mounted, Wi-Fi AP up, HTTP server up. Two
failures, both diagnosed from Kris's serial log:

1. *No USB enumeration.* Expected in hindsight: `ff_msc_init()` was
   still the log-only stub, so TinyUSB was never installed and the port
   presented no device at all (the serial logs Kris captured came from
   the chip's separate USB Serial/JTAG controller). v02 implements the
   install for real: `tinyusb_driver_install()` with
   `TINYUSB_DEFAULT_CONFIG()` (Kconfig-supplied default descriptors,
   MSC interface from `CONFIG_TINYUSB_MSC_ENABLED=y`), still using our
   own `tud_msc` callbacks and container file — the component's
   whole-card storage helpers remain deliberately unused. The link
   stage answered the old duplicate-callback question on its own: v01
   linked cleanly, so our callbacks are the ones in the firmware.
2. *Upload crashed the device* (Guru Meditation, LoadProhibited) right
   after a `400 name required` response. Root cause: the upload handler
   kept ~5.4 KB of buffers on its stack against the httpd task's
   default 4096-byte stack. v02: the 4 KB chunk buffer is static, the
   httpd stack is raised to 8192, early error exits drain the request
   body before responding, and the presented-image check compares
   basenames via `strrchr` instead of pointer arithmetic past a fixed
   prefix. Note the 400 itself means that request arrived without a
   `?name=` parameter — the embedded page's Upload button always sends
   one; a bare `curl --data-binary` POST does not.

**Console move (v02, read before flashing):** installing TinyUSB takes
the internal USB PHY away from USB-Serial-JTAG — the two controllers
share the single PHY and cannot run at the same time — so from v02 the
console is the header UART: UART0 remapped to TX = IO3 / RX = IO2
(`CONFIG_ESP_CONSOLE_UART_CUSTOM*` in `sdkconfig.defaults`). Connect a
3.3 V USB-UART adapter to the header (adapter TX → board RXD/IO2,
adapter RX → board TXD/IO3, GND → GND) for logs. `sdkconfig.defaults`
only applies when no `sdkconfig` exists yet: **delete the project's
`sdkconfig` file once**, then build. Flashing still works over the USB
port, but with the app owning the PHY you may need download mode
(hold BOOT, tap RESET) for `idf.py flash` to reach the ROM loader.

**v03, 2026-10-29:** v02 compiled fully (the `TINYUSB_DEFAULT_CONFIG()`
shape matched the resolved esp_tinyusb version) but failed at link:
the resolved TinyUSB requires the application to implement
`tud_msc_scsi_cb` — the hook for SCSI commands the class driver does
not handle itself — where older TinyUSB treated it as optional. v03
adds it in `msc_device.c`, answering -1 (unsupported) for all such
commands, which makes the class driver report ILLEGAL REQUEST. That
is the only change from v02.

**v03 hardware result, 2026-10-29:** v03 flashed and ran. TinyUSB
installed, and a Windows PC enumerated the device — Disk Management
showed "Removable (E:) No Media", which is exactly right while
nothing is presented. Upload attempts from the web UI no longer
crashed the device (the v02 stack fix held through three attempts),
and logs were captured from the header UART, so the v02 console move
took effect.

**v04, 2026-10-29:** those web UI uploads were still all answered
400 "name required", and with the crash gone the real cause stood
out: `h_upload` passed the full request URI to
`httpd_query_key_value()`, but that helper takes a bare query string
("param1=val1&param2=val2"), so it treated "/api/upload?name" as the
first key and never found "name". The page was sending the name
correctly all along. v04 extracts the query string with
`httpd_req_get_url_query_str()` first, then looks the key up, then
percent-decodes the value (the API does not decode; the page encodes
with `encodeURIComponent`). A warning log now prints the raw URI if
an upload still arrives without a usable name. Only
`main/http_server.c` changed.

**v04 hardware result, 2026-10-29:** uploads work — Kris uploaded
CCPM86A_IMD.img and CCPM86B_IMD.img from the web UI, and the first
select built the placeholder container (1,674,752 bytes, 3,271
sectors) and presented it. Every *subsequent* select then failed:
"rename /sdcard/.ff/current.new -> /sdcard/.ff/current.bin failed".

**v05, 2026-10-29:** FatFs `f_rename` refuses to overwrite an
existing destination, unlike POSIX rename — so the atomic
.new → live swap only ever worked when no container existed yet.
v05 unlinks the old `current.bin` before renaming (safe: builds run
only while disconnected and the MSC callbacks open the container by
path per operation), and logs `strerror(errno)` on rename/remove
failures. The "sync-back stub: dirty flag kept" warnings in the same
log are expected until the FatFs shim lands. Only `main/container.c`
changed.

**v05 hardware result, 2026-10-29:** swapping works end to end —
B, then A, then B again all built and presented cleanly.

**v06, 2026-10-29 — the container is a real filesystem now.** The
zero-filled placeholder is replaced by a generated FAT12 volume
(new `main/container_fat.c`): boot sector, two identical FATs, root
directory with a volume label, and the files laid out contiguously —
FF.CFG, IMG.CFG if one is in the library, and the image under its
real filename with proper VFAT long-name entries (8.3 alias +
checksum), so the Gotek display shows e.g. `CCPM86A_IMD.img`, not a
mangled alias. Design change from the original sketch: the volume is
generated directly instead of mounting a second FatFs over a disk-I/O
shim — no ESP-IDF volume-registration plumbing, deterministic output,
and host-verifiable. FF.CFG comes from the library folder if the user
has placed one there (their settings win); otherwise a built-in
minimal FF.CFG with just `image-on-startup = init` and
`ejected-on-startup = no` is used. Host verification (this is the
generator checked on a PC, not on the ESP32): compiled with gcc
`-Wall -Wextra -Werror`, built three containers (a 626,176-byte
`.img` at 3,271 sectors, a long-lowercase-name `.hfe` with spaces,
a clean 8.3 `.adf`), and an independent Python FAT12 parser walked
the BPB, both FATs, the root directory and the LFN chains and
extracted every file byte-identical to its source (full-content
comparison), with contiguous cluster chains throughout. Still a
stub: sync-back (host writes to the image are not yet copied back
to the library — the dirty flag just stays set).

**v06 hardware result, 2026-10-29:** swapping works on the PC with
the real filesystem — the presented volume shows the default FF.CFG
and it reads fine. One failure: uploading Kris's own FF.CFG was
refused with "400 Bad Request - reserved name". Kris also confirmed
his real FlashFloppy stick carries three config files: FF.CFG,
IMG.CFG, and IMAGE_A.CFG — the last one created by FlashFloppy
itself holding the last-used image name, so it must be writable by
FlashFloppy.

**v07, 2026-10-29:** config-file handling, in `http_server.c`,
`web_ui.h`, `container.c`/`container.h`:
- Upload accepts FF.CFG and IMG.CFG as configuration: stored in the
  library under canonical uppercase names and applied by the
  container builder on the next swap. IMAGE_A.CFG is still refused,
  now with the accurate message that FlashFloppy writes it itself;
  INIT_A.CFG remains unsupported.
- The library listing marks config files (`kind: "config"`); the UI
  shows them without a Select button, and `/api/select` refuses
  config names, so a config file can never be presented as an image.
- The builder now also seeds IMAGE_A.CFG (bare image name + newline)
  into every container. `image-on-startup = last` is FlashFloppy's
  default and what a user's own FF.CFG is likely to say; with the
  seed present, `last` lands on the same image `init` would. The
  volume is writable with ~1 MB free, so FlashFloppy can rewrite
  IMAGE_A.CFG in place while presented, exactly as on a real stick;
  sync-back continues to ignore it by design.
- Same FatFs lesson as v05, third instance: re-uploading any file
  already in the library silently failed at rename — the upload
  handler now unlinks the destination first, with errno logging.
The v07 container file set (FF.CFG + IMG.CFG + IMAGE_A.CFG + image)
was run through the same host verification as v06: all four files
extracted byte-identical from a generated container.

**v07 hardware result, 2026-10-30:** first select crashed the board —
"A stack overflow in task ff_swap". The build path was already
marginal on the swap task's 4096-byte stack in v06 (swap_task's two
256-byte path buffers + container_build's 512-byte path buffer +
the generator's 512-byte boot-sector buffer + stdio/FatFs depth);
v07's 300-byte IMAGE_A buffer was the addition that tipped it over
the canary.

**v08, 2026-10-30:** ff_swap stack raised to 8192, and the three big
buffers in the build path (container_build's path buffer and
IMAGE_A buffer, the generator's boot-sector buffer) are static now
— the build only ever runs on that one task, so this is safe, and
the stack raise is headroom rather than a tight fit. Same lesson as
the v01 httpd overflow: 4096 bytes is not enough stack for file
work; new tasks doing file/HTTP work start at 8192. Generator
re-verified on the host after the change (all containers still
byte-identical).

**v08 hardware result, 2026-10-30:** Kris confirms swapping works
and that his uploaded IMG.CFG now appears in the root of the
presented volume alongside FF.CFG, IMAGE_A.CFG and the image. The
PC-side pipeline (upload incl. config, select/swap, container) is
fully verified.

**v09, 2026-10-30 — sync-back implemented; the last big stub.**
Host writes to the presented image used to stay in the container
and be thrown away at the next swap. Now `container_fat.c` also has
a reader, `ff_fat12_extract()`: it parses the container's BPB, walks
the root directory (VFAT LFN aware, checksum-validated, case-
insensitive), and streams a named file out following its FAT chain.
`ff_container_sync_back()` implements the 2026-10-27 policy on top:
runs only while disconnected, copies back ONLY the image (writes
to FF.CFG / IMG.CFG / IMAGE_A.CFG in the container are ignored),
backs the library file up to `<name>.bak` before overwriting,
accepts a size increase (extend-image), refuses a size decrease
with the dirty flag kept, and on any failure preserves the raw
container as `.ff/current.bin.bak` before the pending build
replaces it. Also fixed: sync-back now runs on eject too — it
previously ran only on the select path, so eject-then-select would
have silently dropped host writes. Host verification: a generated
container was patched the way a host would (4 KB rewritten inside
the image, FF.CFG scribbled on) and `ff_fat12_extract` returned the
patched image byte-identical, found FF.CFG by a lowercase query
with its patched contents, and returned -3 for a missing file; the
build-side verification still passes unchanged. Remaining stubs:
ST7789 LCD driver, HTTP basic auth, busy-swap abort timeout.

**Gotek test, 2026-10-30:** with a correct-format image the Gotek
mounted and the host attempted to boot — but the boot reads failed
("attempts to read, but fails to get the data"). The earlier
ERROR 34 was with the IMD-content file, as Kris spotted himself:
FlashFloppy enumerated and mounted the volume fine even then.

**v10, 2026-10-30 — MSC read-path performance.** The floppy-side
rate is set by FlashFloppy and the image format (HD = 500 kbit/s
≈ 62.5 KB/s); what we control is how fast the "stick" answers. In
v09 every read callback did fopen + fseek + fread + fclose on the
container path, per 512-byte sector — a full FatFs path lookup and
open/teardown per sector, the obvious stall when FlashFloppy
refills track data sector by sector during a boot. v10 keeps the
container file open on a persistent handle across the presentation
(callback = fseek + fread only), closes it in ff_msc_disconnect()
(which also flushes host writes before sync-back reads the file),
honours the TinyUSB callback `offset` argument (previously ignored;
a latent data-corruption risk if the class driver ever splits a
block into sub-block calls), and drops the per-write fflush. It
also logs per-presentation read statistics at disconnect (calls,
KB served, avg/max µs per call, effective KB/s) so the next boot
attempt produces numbers instead of guesses. Discriminator if boot
still fails on v10: try the same image on a physical USB stick in
the same Gotek — boots there = our path; fails there = the image
or its geometry (IMG.CFG), not the ESP32.

**v11, 2026-10-30:** v10 failed to build on Kris's machine — my
header edit closed the msc_device.h comment block one paragraph
early, leaving the FIRST-BUILD NOTE sitting outside the comment as
if it were code (`expected '=', ',', ';' ... before '-' token`,
plus a cascade ending in an implicit ff_msc_init declaration).
v11 is v10 with that one `*/` moved back where it belongs; verified
this time by a standalone gcc syntax check of the header and a
comment-balance scan of every file in main/ (all balanced).

**v12, 2026-10-30 — unstuck swaps, home Wi-Fi, auto-present.**
Bench report against v11: with the Gotek attached the UI sat at
`state: swap_pending` forever (`CPM816_11PD_HD_v08.hfe` presented,
dirty set). The idle gate required reads quiet for 2 s AND writes
quiet for 5 s with no upper bound, and something in the
Gotek/FlashFloppy loop generates reads often enough that "idle"
never arrived. Changes:
- Swap release rules: full idle still releases immediately, but
  reads-only activity can no longer block past
  `FF_SWAP_MAX_WAIT_MS` (15 s, writes must be quiet — interrupting
  reads is safe, the host re-reads). While waiting, the swap task
  logs last-read/last-write ages every 5 s, and the release logs
  its reason. New `POST /api/force` plus a "Force swap now" button
  in the UI (visible only while swap_pending) sets force_swap for
  an immediate, user-confirmed swap.
- Home Wi-Fi (Kris's request): if `/sdcard/WIFI.CFG` exists with
  `ssid=` / `password=` / `mode=auto|sta|ap`, the device joins that
  network at boot (12 s join window). mode auto falls back to the
  FlashFloppy AP on failure; mode sta keeps retrying; no file (or
  mode ap) = AP exactly as before. Credentials live on the SD card
  so they survive the main/-replacing update script. The status
  line and `/api/status` now show the Wi-Fi mode and address.
- Auto-present last used file (Kris's request): every successful
  presentation saves its library path to NVS; at boot, if that file
  still exists, it is queued for presentation like a Select tap
  (the normal idle rules apply, so it lands seconds after boot).
  Eject does not clear the record, mirroring IMAGE_A.CFG.
- Library hygiene: sync-back `*.bak` files are listed as
  `[backup]` with no Select button, and `/api/select` refuses them
  (the library had grown `.bak` and even `.bak.bak` entries with
  Select buttons).

**v13, 2026-10-30:** v12 failed Kris's build with
`-Werror=format-truncation` on the SSID copy: the parser stored the
SSID in a 64-byte buffer, and snprintf-ing that into the Wi-Fi
config's 32-byte SSID field trips GCC's truncation analysis even
though snprintf itself is bounded. v13 sizes the parser buffers to
the real limits (SSID 32, password 64) and replaces the risky
`snprintf(dst, cap, "%s", src)` copies with an explicit bounded
`copy_field()` (memcpy + NUL). Both patterns were reproduced
host-side under the build's exact warning flags: old errors, new
compiles clean.

**v14, 2026-10-30 — swap speedups (Kris's pick of both).** v13 was
confirmed working end to end on the bench; the remaining complaint
was swap latency on the 3.2 MB HFE (~17–20 MB of SD traffic per
dirty swap, plus the idle wait). Two changes:
- Sync-back now compares the extracted image against the library
  copy first: if byte-identical — the usual case, where only
  FlashFloppy's IMAGE_A.CFG write set the dirty flag — the sync is
  skipped entirely: no `.bak`, no replace, log line "unchanged in
  container; library copy kept". (On `.bak` timing generally: it is
  made at swap-away/eject when dirty, holds the pre-sync library
  version, single generation.) The compare helper was host-tested
  verbatim: identical / last-byte-diff / size-diff / missing-file
  cases all correct.
- `FF_SWAP_MAX_WAIT_MS` 15000 → 6000: when the Gotek keeps reads
  trickling, a swap now proceeds after 6 s instead of 15 s (writes
  must still be quiet; Force remains instant).

**v15, 2026-10-30 — whole-card redesign (Kris's proposal).** Kris
suggested dropping the single-file container entirely: present the
whole SD card, let FlashFloppy browse it like a normal stick, and
make selection a rewrite of IMAGE_A.CFG plus a virtual re-insert.
His bench test proved the gating assumption: with
`image-on-startup = last` set explicitly in FF.CFG, FlashFloppy
honours IMAGE_A.CFG on a hot re-insert. v15 is that design:
- The card is registered with the esp_tinyusb MSC **storage
  component** (pinned < 2.0 API), which owns the MSC callbacks and
  the local/USB mount switch. Deleted: container.c/h,
  container_fat.c/h (the FAT12 generator), msc_device.c/h (our
  callbacks), and with them sync-back, the .bak logic, the
  byte-compare, the build copies and the dirty tracking. Host
  writes now land in the real files, as on a real stick.
- Ownership is temporal: an "apply task" runs local windows —
  storage_mount (FlashFloppy sees the stick leave) → dwell →
  refresh our view FROM the card (IMAGE_A.CFG is shared truth;
  Gotek-side selections are learned and synced to the NVS record)
  → flush staged uploads → write IMAGE_A.CFG on select → refresh
  again → storage_unmount (stick "re-inserted"). Eject runs the
  window but skips the hand-back.
- Uploads are staged in PSRAM (cap 7 MB, up to 4 files) because the
  ESP must never write the card while USB owns it; they land at the
  next window. The library listing is a snapshot from the last
  window, plus staged entries shown as `[staged]`.
- FF.CFG/IMG.CFG live in the card ROOT now (real-stick layout):
  first boot migrates them up from the library if needed, and
  storage.c ensures `image-on-startup = last` — created if FF.CFG
  is absent, appended if the line is missing, respected-with-a-
  warning if set to something else (the ensure parser was
  host-tested on six cases, including comment lines and a
  prefix-colliding key).
- Gone with the callbacks: the USB-activity timestamps, so the old
  idle gate (and the Force button) went too. A window starts
  promptly on request — pulling the "stick" is a normal FlashFloppy
  operation, and the window is ~1 s of edits instead of ~10 s of
  copies. Trade-off, stated plainly: don't tap Select in the middle
  of a host write; it's the same caution as yanking a real stick.
- sdkconfig.defaults gains CONFIG_TINYUSB_MSC_MOUNT_PATH="/sdcard"
  (the storage component's local mount point; must match
  FF_SD_MOUNT_POINT) — delete the project's sdkconfig once so the
  new defaults apply, same as at v02.
- Unverified surface (first build/bench will tell): the v1 storage
  component's exact call shapes (init_sdmmc / mount / unmount /
  mount-changed event fields) were written from the component's
  published header and the IDF device guide; if a signature has
  drifted, the compiler/linker errors will name it and it is a
  one-function fix in sdcard.c.

**v16, 2026-10-30:** v15's first build failed on exactly two lines,
both now fixed. (1) `tinyusb_msc_storage_mount()` in the resolved
component takes the mount point: `tinyusb_msc_storage_mount(const
char *base_path)` — Kris's compiler quoted the real declaration;
the call now passes FF_SD_MOUNT_POINT. (unmount() is arg-less, as
written — it compiled.) That also makes the v15
CONFIG_TINYUSB_MSC_MOUNT_PATH sdkconfig line redundant; it stays,
harmlessly, in case a future component revives the symbol. (2)
storage.c copied `d_name` (up to 255 bytes) into the 160-byte
snapshot name field with snprintf — the same
-Werror=format-truncation trap as v12; now a bounded copy_field().
Everything else in v15 compiled, including the storage-component
registration and the mount-changed event usage.

**v17, 2026-10-30:** v16 built and booted on the bench (the log
shows the card handed to USB 2 s in — the whole-card boot works).
Kris's remaining complaint was the home-Wi-Fi join: his router
('OurHouse') sometimes needs longer than the 12 s window. His log
showed the deeper problem — one auth->assoc->init cycle at ~1.8 s,
then silence: the event handler never re-issued esp_wifi_connect()
after a failed association, so the rest of the window was spent
idle. v17 fixes both: the handler now reconnects on every
STA_DISCONNECTED while a join is wanted (logging the disconnect
reason), and the window is FF_WIFI_JOIN_TIMEOUT_MS = 30 s. In
mode=sta the retries continue past the window indefinitely, as
before but now actually retrying.

**v18, 2026-10-30 — everything in the root.** Kris asked why images
live in /library at all; they no longer do. The folder was a
container-era leftover (the card was then the ESP's private store);
on a whole-card design the card is a normal FlashFloppy stick, and
normal sticks keep images, configs and IMAGE_A.CFG in the root.
Changes: the snapshot scans the root (skipping IMAGE_A.CFG,
WIFI.CFG, directories and dotfiles — card-reader housekeeping
included), staged uploads land in the root, select paths and
IMAGE_A.CFG values are bare filenames exactly like Kris's physical
stick, and the "library/" prefix is gone from the code paths. First
boot runs a one-time migration: every regular file in /library
moves to the root (root wins a name collision), the empty folder
is removed, and if IMAGE_A.CFG still says library/<name> it is
rewritten to <name> (NVS record fixed to match). After that the
migration is a no-op.

**v19, 2026-10-30:** the v18 migration is removed before Kris ever
ran it — his card's files are already in the root, so
ff_store_prepare() is back to just the FF.CFG guarantee, and the
FF_LIBRARY_DIR define is gone. Everything else from v18 (root
scanning, bare filenames, root uploads) is unchanged. If a stray
/library folder still exists on a card it is simply ignored by the
web UI (FlashFloppy itself can still browse into it).

**v20, 2026-10-30 — IMAGE_A.CFG forensics.** Kris's bench: on swap,
IMAGE_A.CFG looked un-updated, and the file ends with stray NUL
bytes. FlashFloppy's source (src/main.c, read directly) settles
what the file should look like: FF's writer (native_update) opens
it RW, seeks past any folder prefix, writes the bare filename — no
newline, no terminator — and truncates. Its reader tolerates a
trailing newline and even trailing NULs (the name match is a strcmp
that stops at the first NUL), but if the name does not match a
directory entry exactly (case-sensitive) it TRUNCATES the file to
empty and falls back to the first slot. Neither writer explains
NULs, so v20 removes variables and adds evidence: our write is now
byte-identical to FF's form, fsync'd, and read back and compared
within the same window ("verified" log, loud failure otherwise);
every refresh logs the file's raw bytes with non-printables
escaped, so one swap's log shows what we wrote, what came back,
and what FlashFloppy left in the file after the re-insert.

**v21, 2026-10-30 — real USB detach around windows.** Kris's v20
log closed the case: every write verified, and the NEXT window's
fresh read showed the previous selection persisted on the card —
our side was always correct. His PC (he is testing on PC, not the
Gotek) kept displaying the old content because Windows never
believed the media changed: the component's mount switch let the
PC keep issuing reads mid-window (the log's sdmmc timeouts) and
keep its cached copy afterwards. Fix: local windows now start
with tud_disconnect() (plus a 250 ms settle) and end with
tud_connect(), the electronic unplug/replug the hand-rolled MSC
code used when PC swaps were confirmed working in the container
era. Hosts now see a removal and a fresh insertion, drop caches,
and re-read. The mid-window read timeouts should disappear too.

**v22, 2026-10-30 — the UI polls.** With v21 swapping correctly,
Kris found the status line frozen at "Presented: <old name> |
state: swap_pending". The firmware was right (the log's
re-inserted line already named the new file); the page was not:
refresh() ran only at page load and immediately after a click —
the one moment the state really is swap_pending with the old name
— and never again. v22 adds setInterval(refresh, 2000), fetch
cache:'no-store' on the API calls, Cache-Control: no-store on the
JSON and page responses, and re-renders the library list only
when its JSON actually changed (so the 2 s poll cannot eat a
click or flicker the list).

**v23, 2026-10-30 — radio-button UI + the LCD driver.** At Kris's
request: (1) the library list now shows all files as before, but
images carry a radio button instead of a per-row Select; one
Select button below the list submits the checked radio, and the
radio for the currently presented image starts checked.
(2) lcd.c is no longer a stub: an esp_lcd ST7789 driver (172x320,
+34 column gap, 40 MHz SPI, PSRAM framebuffer, generated 10x18
font from DejaVu Sans Mono) renders a status screen — FLASHFLOPPY
header, the selected file name large and wrapped, and a footer
with the Wi-Fi SSID and IP (new g_ff.wifi_ssid / wifi_ip fields
kept by main.c; app_main repaints after Wi-Fi init, the apply
task repaints after every window). The layout was mock-rendered
host-side from the shipped font table and verified visually. Two
panel properties cannot be verified without the hardware and are
marked PANEL-TWEAK in lcd.c: colour inversion (set ON, standard
for these IPS panels) and RGB element order (set RGB). CMakeLists
gains esp_lcd / esp_driver_spi / esp_driver_gpio.

**v24, 2026-10-30:** the LCD works and reads correctly as built
(inversion and RGB order both right first try); at Kris's request
the image is rotated 180 degrees — esp_lcd_panel_mirror(panel,
true, true) after the gap setup. The visible window does not
shift because the panel offsets are symmetric (34 + 172 + 34 =
240 columns; full 320 rows used).

**v25, 2026-10-30:** /api/library emits the snapshot in grouped
passes — images first, backups next, CFG/config files last —
instead of raw card directory order, so the configs no longer sit
mixed in with the real images in the UI list.

**v26, 2026-10-30 — Gotek-side selections are learned live.**
Kris's question: after our swap, a selection made on the Gotek's
physical buttons makes FlashFloppy rewrite IMAGE_A.CFG itself;
our state then mismatches the real selection until the next local
window. The filesystem cannot be read while USB owns the card,
but the file can: ff_store_peek_image_a() walks MBR -> BPB ->
root directory -> first cluster with raw sdmmc_read_sectors
(FAT16/FAT32, validated host-side against a constructed FAT32
image). A new ff_peek task samples it every 5 s while the card
is presented; a change is believed only after a second identical
read 200 ms later, then current_path, the NVS last-used record,
and the LCD are updated and the change logged. The peek is
read-only — it cannot disturb FlashFloppy's cached filesystem
state. The residual risk is sharing the SDMMC controller with
the storage component's in-flight transfers; the cadence, the
few-sector reads, and the double-read confirmation keep the
blast radius at one dropped sample.

## What this is

Browser → Wi-Fi → ESP32-S3 → SD card (library) → virtual FAT container →
USB MSC → Gotek running *stock* FlashFloppy → host floppy interface.

The ESP32 never talks to FlashFloppy firmware. It pretends to be a USB stick
that always contains exactly one disk image. The SD card is the ESP32's own
storage, in normal host mode.

## Pin map (from the Waveshare schematic, verified 2026-10-28)

| Function | Pin |
|---|---|
| USB D- / D+ (native, to Gotek) | GPIO19 / GPIO20 |
| SDMMC CLK / CMD | IO14 / IO15 |
| SDMMC D0 / D1 / D2 / D3 | IO16 / IO18 / IO17 / IO21 |
| LCD RST / CLK / DC / CS / DIN / BL | IO39 / IO40 / IO41 / IO42 / IO45 / IO48 |
| Header UART RXD / TXD (logs while USB is the Gotek link) | IO2 / IO3 |
| RGB LED | IO38 |

All pins live in `main/board.h`. Nothing else is hard-coded elsewhere.

## Layout

```
CMakeLists.txt            project entry, target esp32s3
main/
  CMakeLists.txt          component, sources, esp_tinyusb dependency
  idf_component.yml       esp_tinyusb pinned < 2.0 (see below)
  board.h                 pin map + paths + Wi-Fi AP defaults
  main.c                  app_main: NVS, SD, Wi-Fi AP, HTTP, swap task, MSC
  state.h / state.c       swap state machine, SD mutex, dirty/presented flags
  sdcard.c / sdcard.h     SDMMC 4-bit mount at /sdcard, library/.ff dirs
  msc_device.c / .h       MSC sector interface + TinyUSB callbacks
  container.c / .h        container build + write-back sync (see stubs)
  http_server.c / .h      /api/status, /api/library, /api/select, /api/eject,
                          /api/upload (raw body, not multipart — see below)
  web_ui.h                minimal embedded HTML page for the above API
  lcd.c / lcd.h           ST7789 status display — STUB, optional, not required
sdkconfig.defaults        esp32s3, PSRAM, TinyUSB MSC, FATFS
.gitignore
```

## What is real, and what is a stub

Real, in the sense of written to the design (still uncompiled):
- SDMMC mount with the pinned 4-bit pins, and creation of
  `/sdcard/library` and `/sdcard/.ff`.
- The swap state machine: IDLE → SWAP_PENDING → DISCONNECTED → BUILDING →
  PRESENTED, with the idle check (no MSC write ~5 s, no MSC read ~2 s),
  a user-confirmed force override, and `dirty` tracking on MSC writes.
- MSC sector read/write: seek the container file to `lba * 512`, read/write
  that sector, under the single SD mutex. Callbacks do no FatFs work
  themselves. A write callback returns 0 ("not ready, call again") while a
  swap/build holds the mutex, instead of blocking the USB task.
- HTTP status/library/select/eject handlers, and the embedded web page.
- Write-back sync *policy*: only while disconnected, only the floppy image
  file by name, library file backed up to `<name>.bak` first, size increase
  accepted, size decrease refused without confirmation. The policy is in
  `container.c`; the mount-the-container step it needs is a stub (below).

Stubs, clearly marked TODO in the code:
- `container_build()`: creates the container file at image size + 1 MB
  slack and records its layout, but does **not** yet format it as FAT or
  copy `FF.CFG` / the image into it. That needs the second FatFs volume
  over a file-backed disk-I/O shim (`f_mkfs` + streamed copy in 4–8 KB
  chunks, as in the 2026-10-27 design). Until that shim lands, the MSC
  device will present a container whose filesystem is empty — a PC will
  see a drive with no files, which is the correct first smoke test, and
  nothing more.
- `container_sync_back()`: same reason — it can copy a file back once the
  container is mountable, but mounting the container is the missing shim.
- `lcd_*()`: no SPI/ST7789 driver yet. The display is optional; the MSC
  path does not depend on it.

Upload is deliberately **not** multipart in this skeleton. The web page
POSTs the raw image bytes to `/api/upload?name=<file>` and the handler
streams them to `/sdcard/library/.staging/<name>.part`, then renames into
the library on success. That avoids a multipart parser in v1. A form-based
multipart endpoint can replace it later without touching the MSC path.

## Wi-Fi

Default is a Wi-Fi access point, so the board works without joining Kris's
home network first: SSID `FlashFloppy`, password `flashfloppy` (change both
in `board.h` before first use on a shared network). The web UI is at
`http://192.168.4.1/`. Station mode, and any authentication beyond this,
are later work — the skeleton is LAN-only by construction, never
port-forward it.

## First build task (read before building)

`esp_tinyusb` changed its MSC API between v1 and v2 (v2 removed the
dedicated MSC callbacks and introduced a single storage-event callback).
This skeleton pins `espressif/esp_tinyusb` to `< 2.0` and implements the
classic TinyUSB callbacks (`tud_msc_read10_cb`, `tud_msc_write10_cb`,
`tud_msc_capacity_cb`, `tud_msc_test_unit_ready_cb`) in `msc_device.c`.

If the pinned component's MSC driver already implements those callbacks,
the build will fail with duplicate symbols. That is the expected first
conflict, and the fix is one of:
1. Use the TinyUSB driver only for USB plumbing and do not initialise the
   component's MSC storage driver, keeping the callbacks in `msc_device.c`;
   or
2. Drop the custom callbacks and adapt `msc_device.c` to the component's
   storage API for the pinned version.

Do not "fix" this by presenting the whole SD card as the MSC volume —
that reintroduces the storage-ownership problem the container exists to
avoid (FlashFloppy would see the whole library, and host writes would
land in library files directly).

## Build

```sh
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

Flash with the board plugged into a PC, **not** into the Gotek. The native
USB port is both the programming port and the Gotek link on this board.

## First bench steps (in order)

1. PC first: plug the flashed board into a PC. It should enumerate as a
   mass-storage device (empty filesystem until the container shim lands).
   Check the MSC read traffic in the log, not just enumeration.
2. Gotek, non-boot image: after the container shim lands, select an image
   from the web UI, watch the Gotek display, confirm the OLED name matches
   the original filename, and confirm the image reads.
3. Gotek, boot image: boots; reset the host after a swap; the new image
   boots.
4. Write test: save/format from the host, eject, sync, read the library
   file back on a PC and confirm the write survived.
5. Busy test: swap while the host is reading — a safe swap must abort,
   force swap is the only way through.
6. Power-loss test: pull power mid-host-write; the library must be the
   old version or the `.bak` recovery, never torn.

## Open bench verifications (carried from the 2026-10-27 design)

- Whether FlashFloppy writes `IMAGE_A.CFG` on an `init` mount — inspect
  the container after mounting, before any sync.
- The exact USB disconnect dwell this Gotek needs to reliably notice
  removal (skeleton uses 500 ms–1 s; verify against the Gotek display
  reaching no-media/unmounted). Fallback if software disconnect proves
  unreliable: a hardware USB switch IC on D+/D- driven by a GPIO.
- `FF.CFG` in the container uses `image-on-startup = init` and
  `ejected-on-startup = no`, so the single image in the container root
  is always the mounted one, without depending on the unverified
  `INIT_A.CFG` file format.
