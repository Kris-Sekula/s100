# MFM emulator filename display — 128x64 SSD1306 OLED on J7 I2C2

A standalone daemon that shows the filename of the file currently being
emulated on a 0.96" I2C OLED. It does not modify `mfm_emu`, the PRU code,
or any emulation file, so a missing or dead display cannot affect
emulation.

Run it as display only:

![Display](https://github.com/Kris-Sekula/s100/blob/main/MFM_EMU_OLED/OLED_ONLY.jpg)

... or with navigation buttons and HDD LED

![DisplayPlus](https://github.com/Kris-Sekula/s100/blob/main/MFM_EMU_OLED/OLED_SW_LED.jpg)



## Dependencies: none beyond python3

The daemon uses **only the Python standard library**: raw `/dev/i2c-*`
ioctls to drive the SSD1306, plus a built-in 5x7 font. No luma.oled,
no Pillow, no pip, no apt installs.

## Verified against the following HW setup:

- Display: 128x64 SSD1306, I2C, **powered from 3.3V** (J7 pin 1 - 3.3V
  J7 pin 2 GND but NOTE: most cheap modules pull SDA/SCL up to their own VCC, so 5V
  power would back-drive 5V into the BeagleBone).
- Bus: J7 ping 19 - I2C2_SCL / J7 ping 20 I2C2_SDA.
- On this BeagleBone image I2C2 is **/dev/i2c-2** (older images used
  /dev/i2c-1) and the display answers at **0x3c** — confirmed with
  `sudo i2cdetect -y -r 2`. If your image differs, change `I2C_BUS` /
  `I2C_ADDR` at the top of `mfm_oled.py`.
- HDD LED J7 pin 16.
- Detection via filename source: the running `mfm_emu` process's command line
  (`/proc/<pid>/cmdline`), falling back to `/etc/mfm_emu.conf`
  (`EmuFN1` / `EmuFN2`) when the emulator is not running. `mfm_emu`
  never swaps images mid-run, so the display only changes at
  start/restart.

## Files

- `mfm_oled.py` — the daemon. Ticks every 0.25 s (mainly to advance
  the long-filename scroll), redraws only when the visible content
  changes, retries forever if the display is absent, clears the
  display on clean shutdown. Also supports `--once` (draw once, exit)
  and `--dry-run` (print the screen as ASCII art, no I2C).
- `mfm-oled.service` — systemd unit. Ordered after `mfm_emu.service`
  but deliberately *not* dependent on it, in either direction.
- `install.sh` — installer (run on the BeagleBone, as root). Needs no
  network: it first draws a test screen with `--once`, and only
  installs the service if that succeeds.

## Install

```bash
# On your own machine: copy this folder to the BeagleBone, e.g.
scp -r mfm-oled-display debian@beaglebone:/home/debian/

# On the BeagleBone:
cd ~/mfm-oled-display
sudo ./install.sh
```

## Check it

```bash
journalctl -u mfm-oled -f
systemctl status mfm-oled
```

Expected screen (autostart, single drive), FlashFloppy-style:

```
  /home/debian
  emufile_a        <- double-size font
  Running
```

Single disk only: the daemon tracks just the first file (EmuFN1 /
the first `--file` entry, "file A"). A second file (EmuFN2, "file B")
is ignored entirely and never appears on the display.

The layout follows FlashFloppy's 128x64 OLED default

- Line 1 (small): the folder containing the file (the full directory,
  truncated from the front with `...` if it exceeds 21 characters).
- Line 2 (large): just the filename (basename), at 2x the 5x7 font —
  10 characters fit across the 128-pixel width. Longer filenames
  scroll, FlashFloppy-style: 2 s pause at the start, 0.5 s per step
  through the name, 2 s pause at the end, then back to the start.
- Line 3 (small): status. `Running` when `mfm_emu` is running,
  `Idle` when the names came from `/etc/mfm_emu.conf` and the
  emulator is not currently running. The voltage is deliberately
  never shown on the display; on power loss the line becomes the
  static `POWER LOSS - SHUTDOWN` (see below).

Power loss: the board input voltage is read from the same
BeagleBone ADC node `powerfail` watches
(`/sys/bus/iio/devices/iio:device0/in_voltage0_raw`, voltage =
raw * 1.8 / 4096 / scale). The scale and threshold are parsed from
a running `powerfail` process's command line when one is present,
otherwise `powerfail`'s defaults apply (scale 0.125, threshold
11.5 V). The daemon monitors the voltage every tick but never
displays it: in the normal state the line is just `Running` /
`Idle`, and if the voltage falls below the threshold for two
consecutive 0.25 s ticks, line 3 becomes the static
`POWER LOSS - SHUTDOWN` and stays there — it overrides Running/Idle,
since `powerfail` will have SIGINTed `mfm_emu` by then and the conf
fallback would misleadingly say "Idle". The voltage at detection is
still logged to the journal. That is the window while `powerfail`
waits for `mfm_emu` to flush and exit and then runs its poweroff
command, until the board itself powers off. A transient dip shorter
than ~0.5 s does not trigger it, matching `powerfail`'s own
multi-sample requirement. If the ADC cannot be read at all, no
power-loss warning can be shown — the line stays plain
Running/Idle, no guessed value. Note: unlike FlashFloppy, there is
no track/head status here — `mfm_emu` does not expose that to
userspace, so it is not invented. With the single-disk focus, the
drive number is dropped from the status line entirely.

Two buttons — file selection, FlashFloppy-style:

- Wiring: two momentary pushbuttons, one side to a GPIO input, the
  other to ground, with a pull-up to 3.3V (external 10k resistor, or
  the BeagleBone's internal pull-up — never 5V). Defaults:
  GPIO2_22 = Next, GPIO2_23 = Prev - SW1 J7 pin 4, SW2 J7 pin 6.
- Operation: press Next/Prev to preview the next/previous valid
  emulation file in the SAME directory as the current file A
  (file B is never touched). Candidates are recognised by the
  8-byte MFM header magic, not by extension, and sorted
  alphabetically; the list wraps around at the ends. The preview
  shows on the OLED as `Preview i/n` with the candidate's name.
  Hold a button past ~0.5 s and it auto-repeats every ~0.25 s.
  Contact bounce is filtered (~100 ms of stable reads).
- Commit: 2 s after the last press (`MFM_OLED_BTN_IDLE`), the
  previewed file is committed — the candidate is re-validated,
  `EmuFN1` in `/etc/mfm_emu.conf` is rewritten (other lines
  untouched, write is atomic), and the `mfm_emu` service is
  stopped and started again (`MFM_OLED_EMU_SERVICE`, default
  `mfm_emu.service`). If the restarted emulator does not come up
  on the new file, the old conf is restored and the service is
  restarted on the old file, and line 3 shows `Swap failed` for
  5 s. Scrolling full circle back to the current file commits
  nothing.
- Guards: the swap is refused if the emulator service is not
  active — a CLI-run `mfm_emu` is never left behind by a conf-only
  change. Buttons are locked out entirely while power loss is
  latched. And the host caveat from the research still applies:
  a hard-disk host does not poll for disk changes, so a boot-image
  swap needs a host reboot and a data disk should only be swapped
  with nothing open — the daemon cannot know the host's state.
- Backup note: the restart goes through the service, so
  `start_mfm_emu` runs again — if `Backup=copy/rdiff/xdelta` is set
  in the conf, every swap also makes a backup copy of the old image
  (as on any service start). That is deliberate data safety, not
  something the daemon disables; if swaps feel slow, `Backup=""` is
  the lever.
- Fail-soft: if the GPIOs cannot be opened (not exported, wrong
  permissions, no sysfs GPIO), the daemon logs "buttons
  unavailable" once and keeps working as a display-only daemon.
  Buttons are polled in a separate thread at 50 ms; the thread
  never touches the display or the conf — only the main loop does.
  `--once` and `--dry-run` do not start the button thread.

Screen saver — burn-in protection, FlashFloppy-style:

FlashFloppy's `FF.CFG` has `display-off-secs` (default 60 s):
seconds of inactivity before the display turns off, and
`display-on-activity` (default `yes`): the display wakes again on
activity. This daemon follows the same shape, adapted to the
SSD1306 and to what `mfm_emu` exposes:

- After 120 s with no button press and no change in what the
  screen shows, the daemon sends the SSD1306 DISPLAY OFF command
  (`0xAE`). The panel goes dark but its framebuffer RAM is kept,
  so DISPLAY ON (`0xAF`) restores the exact same screen instantly
  with no redraw. (An SSD1306 has no backlight to switch, so the
  controller command is the equivalent of FlashFloppy's "backlight
  off".) While blanked, the daemon also skips framebuffer writes,
  so no I2C traffic reaches a dark panel.
- Wake: press either button and the display comes back. That
  press is consumed by the wake alone — it does NOT also step the
  file selection, so waking can never accidentally swap files.
  The next press after waking acts normally. A change in the
  screen's content (file swapped externally, emulator stopped,
  swap failed) also wakes it, and power loss always wakes it, so
  `POWER LOSS - SHUTDOWN` never sits on a dark screen.
- What does NOT count as activity: the scrolling of a long
  filename (otherwise a long name would keep the screen lit
  forever), and FlashFloppy's track/write wake, which has no
  equivalent here — `mfm_emu` exposes no track or write activity
  to userspace.
- The saver never blanks while browsing, while a swap error is
  shown, or while power loss is latched.
- Timeout: `MFM_OLED_SCREENSAVER_SECONDS`, default `120`. Set it
  to `0` to disable the saver entirely. The journal logs both
  "display blanked after … s idle (screensaver)" and
  "display woken by button" / "display woken by power loss".

Preview the layout without a display:

```bash
python3 mfm_oled.py --dry-run
```

## Troubleshooting

- Display blank, service running: check `journalctl -u mfm-oled` —
  "cannot open display" means the I2C address/bus is wrong or the
  module is unpowered. Re-run `sudo i2cdetect -y -r 2`.
- Display shows `(idle)` names but the emulator is running: the daemon
  could not find an `mfm_emu` process in `/proc`. Check
  `ps -C mfm_emu -o pid,args` and compare with what the daemon parses.
- Emulator unaffected: by design. Stopping the display service
  (`systemctl stop mfm-oled`) never touches `mfm_emu`.

## Status

Tested against 3 images in /opt/mfm/mfm folder, all 3 boot my CompuPro machine
