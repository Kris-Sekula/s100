# FlashFloppy ESP32 Remote

A Wi-Fi remote control for a [FlashFloppy](https://github.com/keirf/flashfloppy)-equipped
Gotek floppy emulator. An ESP32-S3 board sits between a microSD card and the
Gotek's USB port, presenting the card to the Gotek as a normal USB stick while
also serving a small web UI: upload disk images over Wi-Fi, select which image
the Gotek mounts, and see at a glance what is currently presented — all without
touching the machine, pulling the stick, or walking across the room.

Simply insert into your Gotek drive and control remotly

![In action](https://github.com/Kris-Sekula/s100/blob/main/flashfloppy_remote_usb/remote_usb.jpg)

The board's own LCD shows the selected file name, the Wi-Fi network, and the
IP address.

## Features

- **Whole-card presentation** — the Gotek sees an ordinary FAT stick with all
  files in the root, exactly as if you had prepared the card in a PC.
- **Web UI** — library listing, image selection, uploads, eject and re-insert
  from any browser on the network. No app, no cloud, no account.
- **Selection via `IMAGE_A.CFG`** — the same mechanism FlashFloppy itself
  uses (`image-on-startup = last`), so web selection and the Gotek's physical
  buttons/rotary encoder stay in agreement.
- **Gotek-side changes are picked up live** — if you change image with the
  Gotek's buttons, the web UI and LCD follow within a few seconds.
- **Direct uploads while ejected** — eject the drive and uploads stream
  straight to the card over Wi-Fi, any size the card can hold; an upload
  never fights the Gotek for the filesystem because the Gotek doesn't
  have it at the time.
- **LCD status display** — selected file name in large type, plus SSID and IP
  address, on the board's 1.47" colour LCD.
- **BOOT-button browsing** — tap the board's BOOT button to preview
  the next image on the LCD, FlashFloppy-style; stop pressing for
  2 seconds and the previewed image is swapped in. The LCD blanks
  after 2 idle minutes; any press wakes it.
- **Your configs stay yours** — your `FF.CFG` / `IMG.CFG` are used as-is; the
  firmware only guarantees the one setting selection depends on
  (`image-on-startup = last`), creating `FF.CFG` with it if none exists.
- **Machine power control** — optionally switch the host machine's power
  from the same UI via a TP-Link Kasa strip/plug (e.g. HS300), including a
  one-click restart, an automatic restart right after an image swap, and a
  live power-draw readout.

## Hardware

| Part | Notes |
|---|---|
| [Waveshare ESP32-S3-LCD-1.47](https://amzn.to/4iyeZ1x) (*) | The **USB-A, non-touch** variant (not the 1.47B Type-C touch model). ESP32-S3R8, 16 MB flash, 8 MB PSRAM, 172×320 ST7789 LCD, microSD slot |
| Gotek with FlashFloppy | Any supported model; connects to the board's USB-A port |
| microSD card | FAT32 or FAT16 formatted |

(*) - As an Amazon Associate, I earn from qualifying purchases!

Pin usage is fixed by the board (defined in `main/board.h`):

| Function | Pins |
|---|---|
| USB (to Gotek) | GPIO19 (D−), GPIO20 (D+) |
| SD card (SDMMC 4-bit) | CLK 14, CMD 15, D0 16, D1 18, D2 17, D3 21 |
| LCD (SPI) | RST 39, CLK 40, DC 41, CS 42, DIN 45, backlight 48 |
| Debug UART (header) | RX 2, TX 3 |
| BOOT button | GPIO0 |

## Building and flashing

Requires **ESP-IDF v5.5.x** (developed against v5.5.6). The project pins
`espressif/esp_tinyusb` to `>=1.7.0,<2.0.0` in `main/idf_component.yml`,
because v2 changed the MSC storage API this code uses.

```sh
idf.py set-target esp32s3
idf.py build
idf.py flash
```

> If you change `sdkconfig.defaults`, delete the generated `sdkconfig`
> once so the new defaults are picked up on the next build.

**Serial logs** come from the header UART (TX = GPIO3, RX = GPIO2) at
115200 baud, not from USB: once the USB stack starts, the S3's single USB
PHY belongs to the Gotek connection.

## Preparing the SD card

Format the card FAT32 (or FAT16) and put everything in the **root**,
just like a normal FlashFloppy stick:

```
/FF.CFG        (optional — created if missing)
/IMG.CFG       (optional)
/IMAGE_A.CFG   (written by FlashFloppy / this device — don't edit by hand)
/WIFI.CFG      (optional — see below)
/POWER.CFG     (optional — see below)
*.hfe, *.img, *.adf, ...   your disk images
```

## Configuring Wi-Fi

Out of the box the device starts its own access point:

- **SSID:** `FlashFloppy`
- **Password:** `flashfloppy`
- **Web UI:** http://192.168.4.1/

To join your home network instead, create a file named **`WIFI.CFG`** in the
**root of the SD card**:

```ini
ssid=YourNetworkName
password=YourPassword
mode=auto
```

| Key | Values | Meaning |
|---|---|---|
| `ssid` | your network name | Required for station mode |
| `password` | your WPA password | Leave empty for an open network |
| `mode` | `auto` (default) | Try the home network for 30 s, fall back to the `FlashFloppy` access point if it doesn't join |
| | `sta` | Home network only; keeps retrying in the background |
| | `ap` | Ignore the rest of the file; always start the access point |

Keeping the credentials in `WIFI.CFG` on the card (rather than compiled in)
means firmware updates never wipe them.

Once joined, the device takes a DHCP address from your router. The address
is shown on the LCD and printed in the serial log; the web UI is then at
`http://<that-address>/`.

## Controlling machine power (optional)

If the host machine is powered through a TP-Link Kasa smart strip
(HS300 and relatives) or plug on the same LAN, the web UI can switch
that outlet too — handy for rebooting the machine into a freshly
swapped image. Create **`POWER.CFG`** in the root of the SD card:

```ini
host=192.168.0.16
outlet=1
delay=5
name=Machine
```

| Key | Values | Meaning |
|---|---|---|
| `host` | IP or hostname of the Kasa device | Required |
| `port` | default `9999` | Kasa local-API port |
| `outlet` | 1-based outlet number (default `1`) | Which strip outlet feeds the machine; ignored for single-socket plugs |
| `delay` | seconds (default `5`) | Pause between off and on for Restart |
| `name` | label (default `Machine`) | Shown in the UI |

The device's **Machine power** section appears in the UI only when
`POWER.CFG` exists on the card. It uses the Kasa *local* API (TCP 9999) — no TP-Link
cloud account is involved, and the strip and this device must be on the
same network. Tick **restart machine after swap** next to the Select
button to power-cycle the machine automatically once a swap completes.
While running, the device also re-reads the outlet every 5 seconds
(relay state and power draw), so the UI shows live watts and follows
changes made elsewhere — the Kasa app, another client, or the strip's
physical buttons.

## Using the web UI

![UI](https://github.com/Kris-Sekula/s100/blob/main/flashfloppy_remote_usb/ui.jpg)

**Status line** — `Presented: <file> | state: <state> | drive ejected/presented | wifi: <mode> <address>`.
It refreshes automatically every 2 seconds, so it follows selections,
eject/re-insert cycles, and Gotek-side changes on its own.

**Library list** — every file on the card, grouped: disk images first,
backups (`.bak`) next. Config files (`FF.CFG`, `IMG.CFG`, `POWER.CFG`)
appear in their own **Config Files** section at the bottom — listed
for reference, never selectable as images. Sizes are shown in short
human form (`626k`, `3.23M`).

**Selecting an image**

1. Click the radio button next to the image you want.
2. Click **Select** below the list.

The card briefly disconnects and reconnects (about a second) while the
selection is applied — the same thing that happens when you re-insert a
physical stick — and FlashFloppy mounts the chosen image.

**Waiting for the drive to go quiet** — Select, Re-insert and Eject
don't pull the card while the Gotek is using it. A swap waits until
nothing has been written for 5 seconds and nothing read for 2 seconds;
a steady trickle of reads delays it by at most 6 seconds, but writes
are never interrupted on their own. While it waits, the page says so
and offers **Force now** — use it only when you know nothing is being
written to the disk image (forcing mid-write can lose that write).
The thresholds are `FF_IDLE_WRITE_MS`, `FF_IDLE_READ_MS` and
`FF_SWAP_MAX_WAIT_MS` in `board.h`.

**Errors** — if something fails (the card can't be handed back,
`IMAGE_A.CFG` can't be written, an upload fails, the device refuses a
request because another one is running), a red line under the status
says what happened. The next request clears it.

**Uploading**

Uploads write directly to the card, so the drive must be **ejected**
first — the Upload button is greyed out until it is, and the hint
under it says which state you're in. Click **Eject**, choose a file,
click **Upload**: the file streams to the card at whatever size it
is (the only limit is free space), with a progress bar and percentage
while it transfers, and **Done** beside the button when it lands; the
new file appears in the library list straight away. Uploads go to a
temporary name first and are renamed into place only on a clean
finish, so an interrupted transfer never leaves a partial file
behind. Uploading a file that already exists replaces it safely: the
old copy is kept until the new one is in place, and is restored if
anything goes wrong. File names FAT can't store (`< > : " | ? *`) are
refused with a message. When you're done, **Re-insert card** hands the drive back
to the Gotek.
`FF.CFG` and `IMG.CFG` may be uploaded; `IMAGE_A.CFG` is refused,
because FlashFloppy owns it. `POWER.CFG` and `WIFI.CFG` are read only
at boot: after uploading one, power-cycle the dongle to apply it.

**Re-insert card** — runs a disconnect/edit/reconnect cycle without
changing the selection, and hands the card back after an eject.

**Eject** — disconnects the card and leaves it disconnected (the host
sees an empty drive) until the next Select or Re-insert. This is
also the state uploads happen in.

**Machine power** (only when `POWER.CFG` exists on the card) — shows the
outlet's live state and power draw with **Power on**, **Power off**, and
**Restart** buttons. Restart turns the outlet off, waits the configured
delay, and turns it back on (retrying the "on" up to three times, so a
single Wi-Fi hiccup can't leave the machine off). Commands run in the
background, so a strip that is slow or offline never freezes the page;
a failed command shows in red under the power state.

**LCD** — shows the presented file name, SSID, and IP address, updated on
every change, including selections made on the Gotek itself.

## Using the BOOT button

The board's BOOT button works the way FlashFloppy's own button
does. While the drive is presented, each press **previews** the
next image in the library on the LCD — the name in large type,
with a `Preview n/m` counter — without disturbing the Gotek.
Keep pressing (or hold the button down to step repeatedly) until
the image you want is showing, then stop: after **2 seconds**
with no further press, the previewed image is selected through
the same swap path as the web UI, including the wait for the
drive to go quiet. A selection from the web UI, or a change made
on the Gotek itself, cancels a preview in progress. Forward
only, wrapping from the last image back to the first.

While the drive is **ejected**, presses do nothing — the
ejected/upload state owns the card then.

The button also drives the **screensaver**: the LCD blanks
after 2 minutes with no activity (no presses, no selection
changes, no swaps). Any press wakes it — and a press that wakes
the screen is spent on waking; it does not also step the
preview. A selection change while the screen is blank wakes it
too, to show the new name.

One caution: BOOT is a strapping pin. It is only sampled at
reset — don't hold it while powering up or resetting the board,
or the chip boots its download loader instead of this firmware.
All button timings (`FF_PREVIEW_COMMIT_MS`,
`FF_LCD_SCREENSAVER_MS`, debounce and repeat) are in `board.h`.

## How it works

A USB stick has exactly one owner at a time, and this design keeps it
that way. The esp_tinyusb MSC storage component presents the whole card
to the Gotek; the ESP32 does not have the filesystem mounted at all in
that state, so it can never corrupt FlashFloppy's cached view of the
card.

When something needs to change, the firmware opens a short **local
window**: it fully detaches USB (the host sees a genuine unplug),
mounts the card locally, applies the change — write `IMAGE_A.CFG`,
refresh its view — then unmounts and re-attaches.
With `image-on-startup = last`, FlashFloppy reads `IMAGE_A.CFG` on the
fresh insertion and mounts the named image.

Uploads use the same ownership rule from the other side: **Eject**
leaves the card detached and locally mounted, and in that state an
upload streams straight to the card — no intermediary, no size limit
but free space. Re-insert hands the card back when you're done.

The firmware also watches USB traffic (it wraps TinyUSB's MSC read and
write callbacks at link time) so that a local window never starts while
the Gotek is writing — see *Waiting for the drive to go quiet*.

Because reading is not writing, one exception is safe: a background task
polls `IMAGE_A.CFG` every few seconds by reading raw sectors (never
writing), which is how Gotek-side button selections show up in the UI
and on the LCD without any disconnect.

Selection state is therefore shared, not duplicated: `IMAGE_A.CFG` on
the card is the single source of truth, whichever side changed it last.

## Notes and limitations

- **No authentication on the web UI.** Run it on a trusted LAN only;
  don't port-forward it.
- Uploads require the drive to be **ejected** (they write directly
  to the card while it is detached; see *Uploading* above). File
  size is limited only by free card space and FAT32's 4 GiB
  per-file ceiling.
- `FF.CFG` is expected in the card root (the `FF/` subfolder layout is
  not used).
- Works with images in the card root; subfolders are visible to
  FlashFloppy's own navigation but are not listed in the web UI.

## Project layout

```
main/
  main.c         boot, Wi-Fi (AP / station), apply task, peek task
  sdcard.c       SDMMC init, USB install, local-window mount switching
  storage.c      card snapshot, IMAGE_A.CFG read/write, direct
                 streaming upload, raw-sector peek
  http_server.c  REST API (/api/status, /api/library, /api/select, ...)
  web_ui.h       the embedded web page
  lcd.c          ST7789 status display
  power.c        TP-Link Kasa outlet control (POWER.CFG)
  font.h         generated 10x18 font (DejaVu Sans Mono)
  state.c/h      shared state
  util.c/h       small shared helpers (string copy, NVS record, clock)
  button.c       BOOT-button browsing + LCD screensaver (v42)
  board.h        pin map and tunables
```

