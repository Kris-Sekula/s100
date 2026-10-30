# FlashFloppy ESP32 Remote

A Wi-Fi remote control for a [FlashFloppy](https://github.com/keirf/flashfloppy)-equipped
Gotek floppy emulator. An ESP32-S3 board sits between a microSD card and the
Gotek's USB port, presenting the card to the Gotek as a normal USB stick while
also serving a small web UI: upload disk images over Wi-Fi, select which image
the Gotek mounts, and see at a glance what is currently presented — all without
touching the machine, pulling the stick, or walking across the room.

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
- **Staged uploads** — files uploaded over Wi-Fi are held in RAM and written
  to the card at the next safe moment (see *How it works*), so an upload never
  fights the Gotek for the filesystem.
- **LCD status display** — selected file name in large type, plus SSID and IP
  address, on the board's 1.47" colour LCD.
- **Your configs stay yours** — your `FF.CFG` / `IMG.CFG` are used as-is; the
  firmware only guarantees the one setting selection depends on
  (`image-on-startup = last`), creating `FF.CFG` with it if none exists.

## Hardware

| Part | Notes |
|---|---|
| Waveshare ESP32-S3-LCD-1.47 | The **USB-A, non-touch** variant (not the 1.47B Type-C touch model). ESP32-S3R8, 16 MB flash, 8 MB PSRAM, 172×320 ST7789 LCD, microSD slot |
| Gotek with FlashFloppy | Any supported model; connects to the board's USB-A port |
| microSD card | FAT32 or FAT16 formatted |

Pin usage is fixed by the board (defined in `main/board.h`):

| Function | Pins |
|---|---|
| USB (to Gotek) | GPIO19 (D−), GPIO20 (D+) |
| SD card (SDMMC 4-bit) | CLK 14, CMD 15, D0 16, D1 18, D2 17, D3 21 |
| LCD (SPI) | RST 39, CLK 40, DC 41, CS 42, DIN 45, backlight 48 |
| Debug UART (header) | RX 2, TX 3 |

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

## Using the web UI

**Status line** — `Presented: <file> | state: <state> | staged: <n> | wifi: <mode> <address>`.
It refreshes automatically every 2 seconds, so it follows selections,
eject/re-insert cycles, and Gotek-side changes on its own.

**Library list** — every file on the card, grouped: disk images first,
backups (`.bak`) next, config files (`FF.CFG`, `IMG.CFG`) last. Staged
uploads are marked `[staged - not on card yet]`.

**Selecting an image**

1. Click the radio button next to the image you want.
2. Click **Select** below the list.

The card briefly disconnects and reconnects (about a second) while the
selection is applied — the same thing that happens when you re-insert a
physical stick — and FlashFloppy mounts the chosen image.

**Uploading**

Choose a file and click **Upload**. The file is staged in the device's RAM
(up to 7 MB across 4 files) and written to the card at the next
*local window*: the next Select, **Re-insert card**, or **Eject**.
`FF.CFG` and `IMG.CFG` may be uploaded (they land in the root);
`IMAGE_A.CFG` is refused, because FlashFloppy owns it.

**Re-insert card (apply uploads)** — runs a disconnect/edit/reconnect
cycle without changing the selection. Use it to land staged uploads.

**Eject** — disconnects the card and leaves it disconnected (the host
sees an empty drive) until the next Select or Re-insert.

**LCD** — shows the presented file name, SSID, and IP address, updated on
every change, including selections made on the Gotek itself.

## How it works

A USB stick has exactly one owner at a time, and this design keeps it
that way. The esp_tinyusb MSC storage component presents the whole card
to the Gotek; the ESP32 does not have the filesystem mounted at all in
that state, so it can never corrupt FlashFloppy's cached view of the
card.

When something needs to change, the firmware opens a short **local
window**: it fully detaches USB (the host sees a genuine unplug),
mounts the card locally, applies the change — write `IMAGE_A.CFG`,
flush staged uploads, refresh its view — then unmounts and re-attaches.
With `image-on-startup = last`, FlashFloppy reads `IMAGE_A.CFG` on the
fresh insertion and mounts the named image.

Because reading is not writing, one exception is safe: a background task
polls `IMAGE_A.CFG` every few seconds by reading raw sectors (never
writing), which is how Gotek-side button selections show up in the UI
and on the LCD without any disconnect.

Selection state is therefore shared, not duplicated: `IMAGE_A.CFG` on
the card is the single source of truth, whichever side changed it last.

## Notes and limitations

- **No authentication on the web UI.** Run it on a trusted LAN only;
  don't port-forward it.
- Uploads are limited by the RAM staging area: **7 MB total, up to 4
  files**, flushed at the next local window. For larger files, load
  them onto the card in a PC — it's just a normal stick.
- `FF.CFG` is expected in the card root (the `FF/` subfolder layout is
  not used).
- Works with images in the card root; subfolders are visible to
  FlashFloppy's own navigation but are not listed in the web UI.

## Project layout

```
main/
  main.c         boot, Wi-Fi (AP / station), apply task, peek task
  sdcard.c       SDMMC init, USB install, local-window mount switching
  storage.c      card snapshot, IMAGE_A.CFG read/write, upload staging,
                 raw-sector peek
  http_server.c  REST API (/api/status, /api/library, /api/select, ...)
  web_ui.h       the embedded web page
  lcd.c          ST7789 status display
  font.h         generated 10x18 font (DejaVu Sans Mono)
  state.c/h      shared state
  board.h        pin map and tunables
```

Development history is in [CHANGELOG.md](CHANGELOG.md).
