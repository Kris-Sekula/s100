#!/usr/bin/env python3
"""MFM emulator filename display for a 0.96" SSD1306 OLED.

Standalone daemon: it never touches mfm_emu, the PRUs, or the emulation
files. It only *reads* what file mfm_emu is using and draws it on the
display, so a missing or dead display cannot affect emulation.

Hardware (verified on Kris's board, 2026-10-24):
  * 128x64 SSD1306 module, I2C, powered from 3.3V (NOT 5V - J7 I/O is
    3.3V only, and most cheap modules pull SDA/SCL up to their VCC).
  * Wired to the MFM board's J7 I2C2_SCL / I2C2_SDA nets (same nets as
    BBB P9_19 / P9_20). On his BeagleBone image I2C2 is /dev/i2c-2
    (older images used /dev/i2c-1 - do not assume), and the display
    answers at 0x3c (verified with i2cdetect -y -r 2).

Dependencies: NONE beyond python3 itself (standard library only).
There is deliberately no luma.oled, no Pillow, no pip install: the
SSD1306 is driven with raw /dev/i2c-* ioctls and a built-in 5x7 font.
This matters on the BeagleBone's Debian 11 (bullseye) image, whose
bullseye-security apt index is stale - `apt install python3-pip`
fails with 404s for superseded python3.9 packages, and pulling a
python interpreter upgrade onto an emulator board is undesirable
anyway. This daemon sidesteps apt entirely.

Filename sources, in priority order:
  1. The command line of a running mfm_emu process, read from
     /proc/<pid>/cmdline. This covers both autostart (via
     start_mfm_emu / powerfail) and manual launches, and it is the
     exact file mfm_emu opened.
  2. /etc/mfm_emu.conf (EmuFN1 only, since the daemon tracks a single
     disk), used when mfm_emu is not currently running - the status
     line then says "Idle" instead of "Running".

Single disk only: only the first file is tracked (EmuFN1 / the first
--file entry, "file A"). A second file (EmuFN2, "file B") is ignored
entirely, by request.

Power status (see the "Power status" section below): the daemon also
reads the board input voltage from the same BeagleBone ADC node that
powerfail watches. The voltage is never shown on the display --
the status line is just "Running" / "Idle" normally, and the static
line "POWER LOSS - SHUTDOWN" once that voltage falls below
powerfail's threshold. The static line stays up while powerfail
SIGINTs mfm_emu and runs its poweroff command, until the board
itself powers off.

mfm_emu never swaps images mid-run, so the filename itself only
changes at start/restart; the daemon ticks every 0.25 s mainly to
advance the long-filename scroll, and redraws only when the visible
content changes.

Layout (FlashFloppy-style, see build_layout below): small path line,
large (double-size) filename, small status line, centered on screen.

Two buttons (GPIO2_22 Next, GPIO2_23 Prev, active-low to ground
with a 3.3V pull-up) browse the valid emulation files in the same
directory as the current file A: the previewed file shows as
"Preview i/n", and 2 s after the last press it is committed by
rewriting EmuFN1 in /etc/mfm_emu.conf and restarting the mfm_emu
service (with rollback if the new file does not come up). See the
"Two-button file selection" section below for the full rules,
including the service-only guard and the power-loss lockout.

Screen saver (FlashFloppy-style, see the "Screen saver" section
below): after 120 s with no button press and no content change,
the display is blanked with the SSD1306 DISPLAY OFF command
(0xAE) -- the panel goes dark but its framebuffer is kept, so
DISPLAY ON (0xAF) restores the screen instantly. Any button press
wakes it (that press is consumed by the wake, it does not also
step the selection); a content change or a power loss also wakes
it, so a POWER LOSS warning never sits on a dark screen.
MFM_OLED_SCREENSAVER_SECONDS changes the timeout, 0 disables it.

Usage:
  python3 mfm_oled.py                # daemon loop (what the service runs)
  python3 mfm_oled.py --once         # draw the current screen once, exit
  python3 mfm_oled.py --dry-run      # print the screen as ASCII, no I2C

Run as a systemd service - see mfm-oled.service and install.sh.
"""

import fcntl
import os
import queue
import re
import shlex
import signal
import struct
import subprocess
import sys
import threading
import time

# ---- Configuration: change these if your wiring/image differs ---------
I2C_BUS = int(os.environ.get("MFM_OLED_BUS", "2"))       # /dev/i2c-2
I2C_ADDR = int(os.environ.get("MFM_OLED_ADDR", "0x3c"), 0)
I2C_DEV = os.environ.get("MFM_OLED_I2C_DEV") or ("/dev/i2c-%d" % I2C_BUS)
WIDTH = 128
HEIGHT = 64
RETRY_SECONDS = 10.0
CONF_PATH = os.environ.get("MFM_OLED_CONF", "/etc/mfm_emu.conf")
PROC_ROOT = os.environ.get("MFM_OLED_PROC", "/proc")

CHAR_WIDTH = 6   # 5 px glyph + 1 px spacing -> 21 chars per 128 px line
CHAR_HEIGHT = 8  # 7 px glyph + 1 px spacing
CHARS_PER_LINE = WIDTH // CHAR_WIDTH

# Large (double-size) font for the filename, as in FlashFloppy's
# double-height filename row: 5x7 glyph scaled 2x, 10 chars per line.
LARGE_SCALE = 2
LARGE_CHAR_WIDTH = 5 * LARGE_SCALE + 2   # 12 px -> 10 chars per line
LARGE_CHAR_HEIGHT = 7 * LARGE_SCALE + 2  # 16 px
LARGE_CHARS = WIDTH // LARGE_CHAR_WIDTH

# Three-line block geometry (small / large / small, with gaps):
#   path    (small,  8 px)  at y = 12
#   filename (large, 16 px) at y = 24
#   status  (small,  8 px)  at y = 44
# Total 40 px, centered in the 64 px height (12 px margin top/bottom).
LAYOUT_GAP = 4
LAYOUT_HEIGHT = CHAR_HEIGHT + LAYOUT_GAP + LARGE_CHAR_HEIGHT \
    + LAYOUT_GAP + CHAR_HEIGHT
LAYOUT_Y0 = (HEIGHT - LAYOUT_HEIGHT) // 2

# Long-filename scrolling, following FlashFloppy's pattern (pause at
# each end, step through the name in between). Timings are adapted to
# this daemon's 0.25 s tick: 2 s pauses, 0.5 s per step.
TICK_SECONDS = 0.25
SCROLL_HOLD_TICKS = 8   # 2 s
SCROLL_STEP_TICKS = 2   # 0.5 s

I2C_SLAVE = 0x0703  # ioctl: set slave address


# ---- Filename sources ---------------------------------------------------

def parse_file_args(argv):
    """Return [(drive, file), ...] from an mfm_emu argument list, or None.

    Handles both '--file a,b --drive 1,2' and '--file=a,b' forms.
    Files and drives are positional: the first file belongs to the
    first drive listed, and so on.
    """
    files = None
    drives = None
    i = 0
    while i < len(argv):
        arg = argv[i]
        if arg == "--file" and i + 1 < len(argv):
            files = argv[i + 1].split(",")
            i += 2
        elif arg.startswith("--file="):
            files = arg[len("--file="):].split(",")
            i += 1
        elif arg == "--drive" and i + 1 < len(argv):
            drives = argv[i + 1].split(",")
            i += 2
        elif arg.startswith("--drive="):
            drives = arg[len("--drive="):].split(",")
            i += 1
        else:
            i += 1
    if not files:
        return None
    files = [f for f in files if f]
    if not files:
        return None
    if not drives or len(drives) != len(files):
        # mfm_emu defaults to drive 1, then 2, ... in file order.
        drives = [str(n + 1) for n in range(len(files))]
    return list(zip(drives, files))


def find_running_mfm_emu(proc_root=PROC_ROOT):
    """Scan /proc for an mfm_emu process.

    Return (pairs, cwd) where pairs is [(drive, file), ...] or None,
    and cwd is the process's working directory (from /proc/<pid>/cwd)
    or None if it cannot be read. The cwd matters for relative
    filenames: mfm_emu is often launched from the image directory
    with a bare filename, and without the cwd the directory line
    would degrade to "." (see build_layout).
    Also catches the case where mfm_emu is launched through a shell
    wrapper (powerfail --command "... mfm_emu --file ..."), whose
    cmdline is a single shell string containing both names.
    """
    try:
        pids = [p for p in os.listdir(proc_root) if p.isdigit()]
    except OSError:
        return None, None
    for pid in pids:
        try:
            with open(os.path.join(proc_root, pid, "cmdline"), "rb") as fh:
                raw = fh.read()
        except (OSError, IOError):
            continue
        if not raw:
            continue
        args = [a.decode("utf-8", "replace") for a in raw.split(b"\0") if a]
        if not args:
            continue
        parsed = None
        if os.path.basename(args[0]) == "mfm_emu":
            parsed = parse_file_args(args[1:])
        else:
            # Wrapper case: one shell argument holding the whole command.
            joined = " ".join(args)
            if "mfm_emu" in joined and "--file" in joined:
                try:
                    parsed = parse_file_args(shlex.split(joined))
                except ValueError:
                    parsed = None
        if parsed:
            try:
                cwd = os.readlink(os.path.join(proc_root, pid, "cwd"))
            except OSError:
                cwd = None
            return parsed, cwd
    return None, None


def read_conf_files(conf_path=CONF_PATH):
    """Return ([(drive, file)], emudir_or_None) from /etc/mfm_emu.conf.

    EmuDir matters for the same reason as a running process's cwd:
    start_mfm_emu does `cd $EmuDir` before launching mfm_emu, so a
    relative EmuFN1 is relative to EmuDir, not to this daemon.
    """
    values = {}
    try:
        with open(conf_path, "r") as fh:
            for line in fh:
                line = line.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                key, val = line.split("=", 1)
                values[key.strip()] = val.strip().strip('"').strip("'")
    except OSError:
        return None, None
    files = []
    drives = []
    for num in ("1", "2"):
        name = values.get("EmuFN" + num, "")
        if name:
            files.append(name)
            drives.append(num)
    if not files:
        return None, None
    return list(zip(drives, files)), values.get("EmuDir") or None


def current_files():
    """Return (drive_file_pairs, running_flag, base_dir_or_None).

    base_dir is the directory relative filenames are relative to:
    the running mfm_emu's cwd, or EmuDir from the conf fallback.
    """
    running, cwd = find_running_mfm_emu()
    if running:
        return running, True, cwd
    conf, emudir = read_conf_files()
    if conf:
        return conf, False, emudir
    return [], False, None


# ---- Power status -------------------------------------------------------
# powerfail (dgesswein/mfm, powerfail/powerfail.c) watches the board's
# input voltage through the BeagleBone ADC and, on loss, SIGINTs
# mfm_emu, waits for it to flush and exit, then runs its powercmd
# (typically poweroff). The OLED daemon reads the SAME ADC node
# directly, so it sees the loss at the same time as powerfail and does
# not depend on powerfail signalling it anything:
#   device:  /sys/bus/iio/devices/iio:device0/in_voltage0_raw
#   voltage: raw * 1.8 / 4096 / scale   (powerfail's own formula)
#   defaults (powerfail/parse_cmdline.c): scale 0.125, threshold 11.5 V,
#   wait 0.1 s, 2 under-threshold samples to trigger.
# If a powerfail process is running, its --scale/--threshold arguments
# are parsed from its /proc cmdline so a non-default threshold in the
# user's setup is honoured automatically; otherwise the defaults apply.
# If the ADC node cannot be read (overlay not loaded, non-BBB test
# machine), power status is simply unknown and the status line falls
# back to plain Running/Idle -- never an error, never a guessed value.
ADC_DEV = os.environ.get("MFM_OLED_ADC_DEV",
                         "/sys/bus/iio/devices/iio:device0/in_voltage0_raw")
ADC_DEFAULT_SCALE = 0.125
ADC_DEFAULT_THRESHOLD = 11.5
# Consecutive below-threshold ticks (0.25 s each) before the power-loss
# warning latches on. powerfail itself needs 2 samples at 50 ms, so a
# 2-tick confirmation here still leaves the warning up for the whole
# mfm_emu shutdown window, while filtering out a transient dip that
# powerfail would ignore.
POWER_LOSS_TICKS = 2


def _parse_powerfail_args(argv):
    """Extract (scale, threshold) from a powerfail argument list."""
    scale = ADC_DEFAULT_SCALE
    threshold = ADC_DEFAULT_THRESHOLD
    i = 0
    while i < len(argv):
        arg = argv[i]
        if arg == "--scale" and i + 1 < len(argv):
            scale = float(argv[i + 1])
            i += 2
        elif arg.startswith("--scale="):
            scale = float(arg[len("--scale="):])
            i += 1
        elif arg == "--threshold" and i + 1 < len(argv):
            threshold = float(argv[i + 1])
            i += 2
        elif arg.startswith("--threshold="):
            threshold = float(arg[len("--threshold="):])
            i += 1
        elif arg == "-s" and i + 1 < len(argv):
            scale = float(argv[i + 1])
            i += 2
        elif arg == "-t" and i + 1 < len(argv):
            threshold = float(argv[i + 1])
            i += 2
        else:
            i += 1
    return scale, threshold


def find_powerfail_params(proc_root=PROC_ROOT):
    """Return (scale, threshold) from a running powerfail, or defaults."""
    try:
        pids = [p for p in os.listdir(proc_root) if p.isdigit()]
    except OSError:
        return ADC_DEFAULT_SCALE, ADC_DEFAULT_THRESHOLD
    for pid in pids:
        try:
            with open(os.path.join(proc_root, pid, "cmdline"), "rb") as fh:
                raw = fh.read()
        except (OSError, IOError):
            continue
        if not raw:
            continue
        args = [a.decode("utf-8", "replace") for a in raw.split(b"\0") if a]
        if not args:
            continue
        if os.path.basename(args[0]) == "powerfail":
            try:
                return _parse_powerfail_args(args[1:])
            except ValueError:
                return ADC_DEFAULT_SCALE, ADC_DEFAULT_THRESHOLD
    return ADC_DEFAULT_SCALE, ADC_DEFAULT_THRESHOLD


def read_input_voltage(scale, adc_dev=None):
    """Read the board input voltage from the ADC, or None if unreadable."""
    if adc_dev is None:
        adc_dev = ADC_DEV
    try:
        with open(adc_dev, "r") as fh:
            raw = int(fh.read().strip())
    except (OSError, ValueError):
        return None
    return raw * 1.8 / 4096.0 / scale


def power_status(proc_root=PROC_ROOT):
    """Return (voltage_or_None, threshold, is_below_threshold)."""
    scale, threshold = find_powerfail_params(proc_root)
    voltage = read_input_voltage(scale)
    if voltage is None:
        return None, threshold, False
    return voltage, threshold, voltage < threshold


# ---- Built-in 5x7 font ---------------------------------------------------
# Each glyph is 7 rows of 5 bits, written as strings so the shapes are
# readable in the source. Row 0 is the top of the glyph. Lowercase
# letters are x-height (rows 2-6) except ascenders (b, d, f, h, k, l, t)
# and descenders (g, j, p, q, y), which use the full height.

def _glyph(*rows):
    return rows


FONT = {
    " ": _glyph("00000", "00000", "00000", "00000", "00000", "00000", "00000"),
    "!": _glyph("00100", "00100", "00100", "00100", "00100", "00000", "00100"),
    '"': _glyph("01010", "01010", "01010", "00000", "00000", "00000", "00000"),
    "#": _glyph("01010", "01010", "11111", "01010", "11111", "01010", "01010"),
    "$": _glyph("00100", "01111", "10100", "01110", "00101", "11110", "00100"),
    "%": _glyph("11001", "11010", "00010", "00100", "01000", "01011", "10011"),
    "&": _glyph("01100", "10010", "10100", "01000", "10101", "10010", "01101"),
    "'": _glyph("00100", "00100", "01000", "00000", "00000", "00000", "00000"),
    "(": _glyph("00010", "00100", "01000", "01000", "01000", "00100", "00010"),
    ")": _glyph("01000", "00100", "00010", "00010", "00010", "00100", "01000"),
    "*": _glyph("00000", "10101", "01110", "11111", "01110", "10101", "00000"),
    "+": _glyph("00000", "00100", "00100", "11111", "00100", "00100", "00000"),
    ",": _glyph("00000", "00000", "00000", "00000", "00100", "00100", "01000"),
    "-": _glyph("00000", "00000", "00000", "11111", "00000", "00000", "00000"),
    ".": _glyph("00000", "00000", "00000", "00000", "00000", "01100", "01100"),
    "/": _glyph("00001", "00001", "00010", "00100", "01000", "10000", "10000"),
    "0": _glyph("01110", "10001", "10011", "10101", "11001", "10001", "01110"),
    "1": _glyph("00100", "01100", "00100", "00100", "00100", "00100", "01110"),
    "2": _glyph("01110", "10001", "00001", "00010", "00100", "01000", "11111"),
    "3": _glyph("11111", "00010", "00100", "00010", "00001", "10001", "01110"),
    "4": _glyph("00010", "00110", "01010", "10010", "11111", "00010", "00010"),
    "5": _glyph("11111", "10000", "11110", "00001", "00001", "10001", "01110"),
    "6": _glyph("00110", "01000", "10000", "11110", "10001", "10001", "01110"),
    "7": _glyph("11111", "00001", "00010", "00100", "01000", "01000", "01000"),
    "8": _glyph("01110", "10001", "10001", "01110", "10001", "10001", "01110"),
    "9": _glyph("01110", "10001", "10001", "01111", "00001", "00010", "01100"),
    ":": _glyph("00000", "01100", "01100", "00000", "01100", "01100", "00000"),
    ";": _glyph("00000", "01100", "01100", "00000", "01100", "00100", "01000"),
    "<": _glyph("00010", "00100", "01000", "10000", "01000", "00100", "00010"),
    "=": _glyph("00000", "00000", "11111", "00000", "11111", "00000", "00000"),
    ">": _glyph("01000", "00100", "00010", "00001", "00010", "00100", "01000"),
    "?": _glyph("01110", "10001", "00001", "00110", "00100", "00000", "00100"),
    "@": _glyph("01110", "10001", "10111", "10101", "10110", "10000", "01110"),
    "A": _glyph("01110", "10001", "10001", "11111", "10001", "10001", "10001"),
    "B": _glyph("11110", "10001", "10001", "11110", "10001", "10001", "11110"),
    "C": _glyph("01110", "10001", "10000", "10000", "10000", "10001", "01110"),
    "D": _glyph("11100", "10010", "10001", "10001", "10001", "10010", "11100"),
    "E": _glyph("11111", "10000", "10000", "11110", "10000", "10000", "11111"),
    "F": _glyph("11111", "10000", "10000", "11110", "10000", "10000", "10000"),
    "G": _glyph("01110", "10001", "10000", "10111", "10001", "10001", "01111"),
    "H": _glyph("10001", "10001", "10001", "11111", "10001", "10001", "10001"),
    "I": _glyph("01110", "00100", "00100", "00100", "00100", "00100", "01110"),
    "J": _glyph("00111", "00010", "00010", "00010", "00010", "10010", "01100"),
    "K": _glyph("10001", "10010", "10100", "11000", "10100", "10010", "10001"),
    "L": _glyph("10000", "10000", "10000", "10000", "10000", "10000", "11111"),
    "M": _glyph("10001", "11011", "10101", "10101", "10001", "10001", "10001"),
    "N": _glyph("10001", "10001", "11001", "10101", "10011", "10001", "10001"),
    "O": _glyph("01110", "10001", "10001", "10001", "10001", "10001", "01110"),
    "P": _glyph("11110", "10001", "10001", "11110", "10000", "10000", "10000"),
    "Q": _glyph("01110", "10001", "10001", "10001", "10101", "10010", "01101"),
    "R": _glyph("11110", "10001", "10001", "11110", "10100", "10010", "10001"),
    "S": _glyph("01111", "10000", "10000", "01110", "00001", "00001", "11110"),
    "T": _glyph("11111", "00100", "00100", "00100", "00100", "00100", "00100"),
    "U": _glyph("10001", "10001", "10001", "10001", "10001", "10001", "01110"),
    "V": _glyph("10001", "10001", "10001", "10001", "10001", "01010", "00100"),
    "W": _glyph("10001", "10001", "10001", "10101", "10101", "10101", "01010"),
    "X": _glyph("10001", "10001", "01010", "00100", "01010", "10001", "10001"),
    "Y": _glyph("10001", "10001", "01010", "00100", "00100", "00100", "00100"),
    "Z": _glyph("11111", "00001", "00010", "00100", "01000", "10000", "11111"),
    "[": _glyph("01110", "01000", "01000", "01000", "01000", "01000", "01110"),
    "\\": _glyph("10000", "10000", "01000", "00100", "00010", "00001", "00001"),
    "]": _glyph("01110", "00010", "00010", "00010", "00010", "00010", "01110"),
    "^": _glyph("00100", "01010", "10001", "00000", "00000", "00000", "00000"),
    "_": _glyph("00000", "00000", "00000", "00000", "00000", "00000", "11111"),
    "`": _glyph("01000", "00100", "00010", "00000", "00000", "00000", "00000"),
    "a": _glyph("00000", "00000", "01110", "00001", "01111", "10001", "01111"),
    "b": _glyph("10000", "10000", "11110", "10001", "10001", "10001", "11110"),
    "c": _glyph("00000", "00000", "01110", "10000", "10000", "10001", "01110"),
    "d": _glyph("00001", "00001", "01111", "10001", "10001", "10001", "01111"),
    "e": _glyph("00000", "00000", "01110", "10001", "11111", "10000", "01110"),
    "f": _glyph("00110", "01000", "01000", "11110", "01000", "01000", "01000"),
    "g": _glyph("00000", "01111", "10001", "10001", "01111", "00001", "01110"),
    "h": _glyph("10000", "10000", "11110", "10001", "10001", "10001", "10001"),
    "i": _glyph("00100", "00000", "01100", "00100", "00100", "00100", "01110"),
    "j": _glyph("00010", "00000", "00110", "00010", "00010", "10010", "01100"),
    "k": _glyph("10000", "10000", "10010", "10100", "11000", "10100", "10010"),
    "l": _glyph("01100", "00100", "00100", "00100", "00100", "00100", "01110"),
    "m": _glyph("00000", "00000", "11010", "10101", "10101", "10101", "10101"),
    "n": _glyph("00000", "00000", "11110", "10001", "10001", "10001", "10001"),
    "o": _glyph("00000", "00000", "01110", "10001", "10001", "10001", "01110"),
    "p": _glyph("00000", "11110", "10001", "10001", "11110", "10000", "10000"),
    "q": _glyph("00000", "01111", "10001", "10001", "01111", "00001", "00001"),
    "r": _glyph("00000", "00000", "10110", "11001", "10000", "10000", "10000"),
    "s": _glyph("00000", "00000", "01111", "10000", "01110", "00001", "11110"),
    "t": _glyph("01000", "01000", "11100", "01000", "01000", "01000", "00110"),
    "u": _glyph("00000", "00000", "10001", "10001", "10001", "10011", "01101"),
    "v": _glyph("00000", "00000", "10001", "10001", "10001", "01010", "00100"),
    "w": _glyph("00000", "00000", "10001", "10101", "10101", "10101", "01010"),
    "x": _glyph("00000", "00000", "10001", "01010", "00100", "01010", "10001"),
    "y": _glyph("00000", "10001", "10001", "10001", "01111", "00001", "01110"),
    "z": _glyph("00000", "00000", "11111", "00010", "00100", "01000", "11111"),
    "{": _glyph("00010", "00100", "00100", "01100", "00100", "00100", "00010"),
    "|": _glyph("00100", "00100", "00100", "00100", "00100", "00100", "00100"),
    "}": _glyph("01000", "00100", "00100", "00110", "00100", "00100", "01000"),
    "~": _glyph("00000", "00000", "01001", "10110", "00000", "00000", "00000"),
}
FONT["?"] = _glyph("01110", "10001", "00001", "00110", "00100", "00000", "00100")


# ---- Screen layout: FlashFloppy-style three lines --------------------
# Borrowed from FlashFloppy (Keir Fraser's Gotek firmware), whose
# 128x64 OLED default is display-order = 3,0d,1, i.e.:
#   content 3: current subfolder name   (small)
#   content 0: current image name, 'd' = double height (large)
#   content 1: status line              (small)
# FlashFloppy's content codes are 0 = image name, 1 = status (track /
# write indicator / slot), 2 = image/volume info, 3 = subfolder name.
# We keep the same row structure and the same double-height filename,
# but with our own status content: mfm_emu exposes no track/head, so
# the status line carries running/idle state plus the board input
# voltage, and becomes a power-loss warning on mains loss (see the
# "Power status" section). Single disk only: file B is ignored.
# The previous centered alignment is kept: the three-line block is
# centered on the screen.
# Long filenames scroll like FlashFloppy's (pause at each end, step
# through the name), instead of wrapping onto extra lines.

def _truncate_small(text, max_chars=CHARS_PER_LINE):
    """Fit text to the small font's width, keeping the informative tail."""
    if len(text) <= max_chars:
        return text
    return "..." + text[-(max_chars - 3):]


def resolve_path(path, base_dir):
    """Return an absolute, normalised path for a possibly relative one."""
    if not os.path.isabs(path) and base_dir:
        path = os.path.normpath(os.path.join(base_dir, path))
    return path


def build_layout(pairs, running, voltage=None, power_loss=False,
                 base_dir=None, status_override=None):
    """Return (path_line, filename, status_line) for the three rows.

    Single-disk only, by request: only the FIRST file is tracked
    (EmuFN1 / the first --file entry, "file A"). Any second file
    (EmuFN2 / "file B") is ignored entirely -- it never appears on
    any line, in any state.

    Path line: the directory containing the file, truncated from the
    front with "..." if over 21 chars (the small font's width -- that
    is already the most that fits on one line, keeping the leaf
    directory, which is the distinctive part). A relative filename
    is resolved against base_dir (the running mfm_emu's cwd, or
    EmuDir from the conf) before taking the directory, so a file
    launched as a bare name from its image directory shows that
    directory instead of ".". Only when no base directory is known
    at all does a bare filename still fall back to "." -- that is
    os.path.dirname("") and means "location unknown", not a
    truncation of a long path: absolute long paths truncate to
    their tail, never to ".".

    Line 3 is the status line. In the normal state it is just
    "Running" / "Idle" / "No file" -- the voltage is never shown on
    the display, by request: a falling voltage during power loss
    (down to 0.1, then 0.0 as the input collapses while the board
    runs on capacitors) looked like a sensor fault. A latched power
    loss instead shows the static line "POWER LOSS - SHUTDOWN", which
    overrides everything -- including the file lines' state, since
    mfm_emu will have been SIGINTed by powerfail by then and the
    conf fallback would misleadingly say "Idle". The voltage is
    still monitored every tick (that is how the loss is detected)
    and still logged to the journal at detection; it is just not
    drawn on the screen.
    """
    if not pairs:
        if power_loss:
            return ("-", "no file", "POWER LOSS - SHUTDOWN")
        if status_override is not None:
            return ("-", "no file", status_override)
        return ("-", "no file", "No file")
    drive, path = pairs[0]
    path = resolve_path(path, base_dir)
    directory = os.path.dirname(path) or "."
    path_line = _truncate_small(directory)
    filename = os.path.basename(path)
    if power_loss:
        return (path_line, filename, "POWER LOSS - SHUTDOWN")
    if status_override is not None:
        return (path_line, filename, status_override)
    state = "Running" if running else "Idle"
    return (path_line, filename, state)


# ---- Framebuffer rendering ----------------------------------------------

def _set_pixel(fb, x, y):
    if 0 <= x < WIDTH and 0 <= y < HEIGHT:
        fb[(y // 8) * WIDTH + x] |= (1 << (y % 8))


def _draw_text(fb, x0, y0, text, scale):
    """Draw text at (x0, y0) with the 5x7 font scaled by `scale`."""
    for ch_no, ch in enumerate(text):
        glyph = FONT.get(ch, FONT["?"])
        cx = x0 + ch_no * (5 * scale + scale)
        for col in range(5):
            for row in range(7):
                if glyph[row][col] == "1":
                    for dx in range(scale):
                        for dy in range(scale):
                            _set_pixel(fb, cx + col * scale + dx,
                                       y0 + row * scale + dy)


def _draw_centered(fb, y0, text, scale):
    """Draw one line centered horizontally."""
    width = len(text) * (5 * scale + scale)
    _draw_text(fb, max(0, (WIDTH - width) // 2), y0, text, scale)


def render_layout(path_line, filename, status_line, name_offset=0):
    """Render the three-line layout to an SSD1306 framebuffer.

    Bit (y % 8) of byte [page * 128 + x] is pixel (x, y).
    `name_offset` is the scroll window into a long filename.
    """
    fb = bytearray(WIDTH * (HEIGHT // 8))
    y_path = LAYOUT_Y0
    y_name = y_path + CHAR_HEIGHT + LAYOUT_GAP
    y_status = y_name + LARGE_CHAR_HEIGHT + LAYOUT_GAP
    _draw_centered(fb, y_path, path_line, 1)
    window = filename[name_offset:name_offset + LARGE_CHARS]
    _draw_centered(fb, y_name, window, LARGE_SCALE)
    _draw_centered(fb, y_status, status_line, 1)
    return fb


def render_screen(pairs, running, name_offset=0, voltage=None,
                  power_loss=False, base_dir=None, status_override=None):
    """Convenience wrapper: layout + render in one call."""
    path_line, filename, status_line = build_layout(
        pairs, running, voltage, power_loss, base_dir, status_override)
    return render_layout(path_line, filename, status_line, name_offset)


def framebuffer_to_ascii(fb):
    """Render a framebuffer as '#' / ' ' rows, trimmed to used width."""
    used = 0
    for y in range(HEIGHT):
        for x in range(WIDTH):
            if fb[(y // 8) * WIDTH + x] & (1 << (y % 8)):
                used = max(used, x + 1)
    out = []
    for y in range(HEIGHT):
        out.append("".join(
            "#" if fb[(y // 8) * WIDTH + x] & (1 << (y % 8)) else " "
            for x in range(used)).rstrip())
    # Drop trailing blank rows for a compact preview.
    while out and not out[-1].strip():
        out.pop()
    return "\n".join(out)


# ---- SSD1306 over raw I2C -----------------------------------------------

# Standard 128x64 init sequence (charge pump on, so it runs from 3.3V).
INIT_COMMANDS = [
    0xAE,             # display off
    0xD5, 0x80,       # clock divide / oscillator frequency
    0xA8, 0x3F,       # multiplex ratio 64
    0xD3, 0x00,       # display offset 0
    0x40,             # display start line 0
    0x8D, 0x14,       # charge pump on
    0x20, 0x02,       # page addressing mode
    0xA1,             # segment remap (column 127 -> SEG0)
    0xC8,             # COM scan direction remapped
    0xDA, 0x12,       # COM pins hardware configuration
    0x81, 0xCF,       # contrast
    0xD9, 0xF1,       # pre-charge period
    0xDB, 0x40,       # VCOMH deselect level
    0xA4,             # display follows RAM content
    0xA6,             # normal (not inverted) display
    0xAF,             # display on
]


class SSD1306(object):
    """Minimal SSD1306 driver on /dev/i2c-*, standard library only."""

    def __init__(self, dev_path=I2C_DEV, addr=I2C_ADDR):
        self.fd = os.open(dev_path, os.O_RDWR)
        try:
            fcntl.ioctl(self.fd, I2C_SLAVE, addr)
            self._commands(INIT_COMMANDS)
        except Exception:
            os.close(self.fd)
            raise

    def _commands(self, cmds):
        os.write(self.fd, bytes([0x00] + list(cmds)))

    def _data(self, data):
        # Chunked so no single I2C write is needlessly large.
        for i in range(0, len(data), 32):
            os.write(self.fd, bytes([0x40]) + bytes(data[i:i + 32]))

    def display(self, fb):
        for page in range(HEIGHT // 8):
            self._commands([0xB0 + page, 0x00, 0x10])  # page, column 0
            self._data(fb[page * WIDTH:(page + 1) * WIDTH])

    def display_off(self):
        # SSD1306 DISPLAY OFF (0xAE): the panel goes dark but the
        # framebuffer RAM is kept, so display_on() restores the
        # exact same screen instantly, with no redraw needed.
        self._commands([0xAE])

    def display_on(self):
        self._commands([0xAF])

    def clear(self):
        self.display(bytearray(WIDTH * (HEIGHT // 8)))

    def close(self):
        try:
            os.close(self.fd)
        except OSError:
            pass


def open_display():
    """Open the SSD1306. Raises on failure (caller retries)."""
    return SSD1306()


def draw(device, pairs, running, name_offset=0, voltage=None,
         power_loss=False, base_dir=None, status_override=None):
    device.display(render_screen(pairs, running, name_offset, voltage,
                                 power_loss, base_dir, status_override))


# ---- Two-button file selection (FlashFloppy-style) --------------------
#
# Two momentary pushbuttons, one side to a GPIO input, the other to
# ground, with a pull-up to 3.3V (an external 10k resistor, or the
# BeagleBone's internal pull-up -- never 5V). Active-low: pressed
# reads 0, released reads 1.
#
# Defaults are the GPIOs Kris specified: GPIO2_22 = Next, GPIO2_23 =
# Prev (Linux sysfs numbers 86 and 87). If the two buttons turn out
# to be swapped on the bench, swap the two env vars instead of
# rewiring. Spec forms: "2_22", "86", or "gpio86".
#
# A running mfm_emu cannot hot-swap its file, so selection is
# restart-based: pressing a button previews the next/previous valid
# emulation file in the SAME directory as the current file A
# (Kris's choice), on the OLED, wrapping around at the ends. If no
# button is pressed for BTN_IDLE seconds (2.0, his FlashFloppy
# adaptation), the previewed file is committed: EmuFN1 in
# /etc/mfm_emu.conf is rewritten and the mfm_emu service is stopped
# and started again. If the previewed file is the current file
# (scrolled full circle), nothing is restarted. Candidates are
# validated by the 8-byte MFM header magic, not by extension, and
# re-validated at commit time; if the restarted emulator does not
# come up on the new file, the conf is rolled back and the service
# is restarted on the old file.
#
# Safety: this only works for a service-managed mfm_emu. If the
# service is not active (e.g. mfm_emu running from the CLI), the
# swap is refused rather than writing a conf the running process
# will never read. And a hard-disk host does not poll for disk
# changes like a floppy host: a boot-image swap needs a host reboot,
# and a data disk should only be swapped with nothing open -- the
# daemon cannot know the host's state, so that judgement stays with
# the operator. Buttons are locked out entirely while power loss is
# latched, so a swap can never fire mid-shutdown.
#
# Buttons are fail-soft by design: if the GPIOs cannot be opened
# (not exported, wrong permissions, sysfs GPIO gone), the daemon
# logs once and keeps working as a display-only daemon. The button
# thread never touches the display; it only queues press events
# for the main loop, which owns all browsing, commit and display
# state.
#
# Note on the service restart and backups: the restart goes through
# the service, so start_mfm_emu runs again -- if Backup=copy/rdiff/
# xdelta is set in the conf, every swap also makes a backup copy of
# the old image (as on any service start). That is Kris's data
# safety net, not something this daemon disables; if swaps feel
# slow, Backup="" in the conf is the lever, taken deliberately.

BTN_NEXT_SPEC = os.environ.get("MFM_OLED_BTN_NEXT", "2_22")
BTN_PREV_SPEC = os.environ.get("MFM_OLED_BTN_PREV", "2_23")
EMU_SERVICE = os.environ.get("MFM_OLED_EMU_SERVICE", "mfm_emu.service")
BTN_IDLE = float(os.environ.get("MFM_OLED_BTN_IDLE", "2.0"))
BTN_DIR_OVERRIDE = os.environ.get("MFM_OLED_BTN_DIR") or None
BTN_POLL_SECONDS = 0.05
BTN_HOLD_SECONDS = 0.5    # hold before auto-repeat starts
BTN_REPEAT_SECONDS = 0.25  # repeat interval while held
BTN_DEBOUNCE_POLLS = 2    # ~100 ms of stable reads before accepting
EMU_FILE_MAGIC = b"\xEE\x4D\x46\x4D\x0D\x0A\x1A\x00"
EMU_FILE_VERSION = 0x02020200  # from emu/emu_tran_file.h


def parse_gpio(spec):
    """Parse a GPIO spec into a Linux sysfs GPIO number.

    Accepts "2_22" (chip_line), "86", or "gpio86".
    """
    spec = str(spec).strip().lower()
    if spec.startswith("gpio"):
        return int(spec[4:])
    if "_" in spec:
        chip, line = spec.split("_", 1)
        return int(chip) * 32 + int(line)
    return int(spec)


BTN_NEXT_GPIO = parse_gpio(BTN_NEXT_SPEC)
BTN_PREV_GPIO = parse_gpio(BTN_PREV_SPEC)


class GpioInput(object):
    """One sysfs GPIO input (active-low button). Raises on open."""

    def __init__(self, number):
        self.path = "/sys/class/gpio/gpio%d" % number
        if not os.path.isdir(self.path):
            with open("/sys/class/gpio/export", "w") as f:
                f.write(str(number))
            deadline = time.time() + 1.0
            while not os.path.isdir(self.path) and time.time() < deadline:
                time.sleep(0.05)
            if not os.path.isdir(self.path):
                raise OSError("gpio%d did not appear after export" % number)
        with open(os.path.join(self.path, "direction"), "w") as f:
            f.write("in")
        self.fd = os.open(os.path.join(self.path, "value"), os.O_RDONLY)

    def read_pressed(self):
        """True if the button is pressed (value 0), False if released.

        Returns None if the value cannot be read (sample ignored).
        """
        try:
            return os.pread(self.fd, 1, 0) == b"0"
        except OSError:
            return None

    def close(self):
        try:
            os.close(self.fd)
        except OSError:
            pass


class ButtonDebouncer(object):
    """Debounce + hold-repeat for one button. Pure logic, testable.

    update(pressed, now) returns "press", "repeat", or None.
    A raw level must be stable for BTN_DEBOUNCE_POLLS consecutive
    polls before it is accepted (contact bounce is filtered, not
    counted as presses). After BTN_HOLD_SECONDS held, "repeat"
    events fire every BTN_REPEAT_SECONDS.
    """

    def __init__(self):
        self._stable = False  # released
        self._pending = False
        self._pending_count = 1
        self._pressed_since = None
        self._next_repeat = None

    def update(self, pressed, now):
        if pressed != self._pending:
            self._pending = pressed
            self._pending_count = 1
        else:
            self._pending_count += 1
        if self._pending_count >= BTN_DEBOUNCE_POLLS \
                and self._pending != self._stable:
            self._stable = self._pending
            if self._stable:
                self._pressed_since = now
                self._next_repeat = now + BTN_HOLD_SECONDS
                return "press"
            self._pressed_since = None
            self._next_repeat = None
            return None
        if self._stable and self._next_repeat is not None \
                and now >= self._next_repeat:
            self._next_repeat = now + BTN_REPEAT_SECONDS
            return "repeat"
        return None


class BrowseSession(object):
    """Preview state for one browsing session. Main-loop only.

    direction is +1 (Next) or -1 (Prev). `current_path` is the
    resolved absolute path of the current file A; `browse_dir` is
    the directory candidates come from (same dir as file A).
    """

    def __init__(self):
        self.active = False
        self.candidates = []
        self.index = 0
        self.last_press = 0.0
        self.anchor_path = None
        self.browse_dir = None

    def candidate(self):
        if self.active and self.candidates:
            return self.candidates[self.index]
        return None

    def preview_status(self):
        if not self.active:
            return None
        return "Preview %d/%d" % (self.index + 1, len(self.candidates))

    def commit_due(self, now):
        return self.active and (now - self.last_press) >= BTN_IDLE

    def anchor_changed(self, current_path):
        return self.active and self.anchor_path is not None \
            and current_path is not None \
            and current_path != self.anchor_path

    def cancel(self):
        self.active = False
        self.candidates = []
        self.index = 0
        self.anchor_path = None
        self.browse_dir = None

    def press(self, direction, current_path, browse_dir, now):
        """Start browsing on the first press, step on later ones.

        Returns True if a preview is now active.
        """
        if not self.active:
            if not browse_dir:
                return False
            candidates = list_candidate_files(browse_dir)
            if not candidates:
                return False
            if len(candidates) == 1 \
                    and current_path is not None \
                    and candidates[0] == current_path:
                return False  # nothing else to select
            self.candidates = candidates
            self.browse_dir = browse_dir
            self.anchor_path = current_path
            if current_path in candidates:
                pos = candidates.index(current_path)
                self.index = (pos + direction) % len(candidates)
            else:
                self.index = 0 if direction > 0 else len(candidates) - 1
            self.active = True
        else:
            self.index = (self.index + direction) % len(self.candidates)
        self.last_press = now
        return True


def list_candidate_files(directory):
    """Sorted absolute paths of valid emulation files in a directory.

    Valid = a regular file whose first 8 bytes are the MFM header
    magic. No extension filter: MFM emulation files have no fixed
    extension. Sorted case-insensitively by basename.
    """
    out = []
    try:
        names = os.listdir(directory)
    except OSError:
        return out
    for name in names:
        path = os.path.join(directory, name)
        try:
            if not os.path.isfile(path):
                continue
            with open(path, "rb") as f:
                if f.read(8) == EMU_FILE_MAGIC:
                    out.append(path)
        except OSError:
            continue
    out.sort(key=lambda p: (os.path.basename(p).lower(),
                            os.path.basename(p)))
    return out


def validate_emu_file(path):
    """Check a candidate the way mfm_emu's header read would.

    Returns (True, "") or (False, reason). Checks the 8-byte magic
    and the version word's type byte (0x02 = emulation file; 0x01
    is a transfer file, which mfm_emu rejects) and major version
    (must not exceed the daemon's known 0x02). The full header
    (sample rate, track sizes) is re-checked by mfm_emu itself at
    restart; a bad file there triggers the rollback path.
    """
    try:
        with open(path, "rb") as f:
            header = f.read(12)
    except OSError as exc:
        return False, "cannot read: %s" % exc
    if len(header) < 12 or header[:8] != EMU_FILE_MAGIC:
        return False, "not an MFM emulation file (bad magic)"
    version = struct.unpack("<I", header[8:12])[0]
    if (version & 0xFF000000) != (EMU_FILE_VERSION & 0xFF000000):
        return False, "wrong file type (version 0x%08x)" % version
    if (version & 0x00FF0000) > (EMU_FILE_VERSION & 0x00FF0000):
        return False, "unsupported major version (0x%08x)" % version
    return True, ""


def rewrite_conf_emu_fn1(conf_path, new_path):
    """Rewrite EmuFN1 in the conf, preserving every other line.

    Returns the old file text (for rollback). The write is atomic
    (temp file in the same directory + os.replace) and keeps the
    original file's mode. If no EmuFN1 line exists, one is appended.
    """
    try:
        with open(conf_path, "r") as f:
            old_text = f.read()
    except OSError:
        old_text = ""
    lines = old_text.splitlines(keepends=True)
    replaced = False
    for i, line in enumerate(lines):
        if re.match(r"\s*EmuFN1\s*=", line):
            lines[i] = 'EmuFN1="%s"\n' % new_path
            replaced = True
            break
    if not replaced:
        if lines and not lines[-1].endswith("\n"):
            lines[-1] += "\n"
        lines.append('EmuFN1="%s"\n' % new_path)
    new_text = "".join(lines)
    tmp_path = conf_path + ".oled-tmp"
    with open(tmp_path, "w") as f:
        f.write(new_text)
    try:
        os.chmod(tmp_path, os.stat(conf_path).st_mode)
    except OSError:
        pass
    os.replace(tmp_path, conf_path)
    return old_text


def _default_runner(args, timeout):
    return subprocess.run(args, capture_output=True, text=True,
                          timeout=timeout)


def commit_swap(candidate, conf_path=CONF_PATH, service=EMU_SERVICE,
                runner=_default_runner, proc_root=PROC_ROOT,
                verify_seconds=15.0):
    """Commit a previewed file: conf rewrite + service restart.

    Returns (True, "") on success, (False, reason) on failure. On
    any failure after the conf was written, the old conf is restored
    and the service is restarted on the old file (best effort).
    Refuses outright if the emulator service is not active, so a
    CLI-run instance is never left behind by a conf-only change.
    """
    ok, reason = validate_emu_file(candidate)
    if not ok:
        return False, reason
    try:
        result = runner(["systemctl", "is-active", "--quiet", service],
                        timeout=10)
    except Exception as exc:
        return False, "cannot query service: %s" % exc
    if result.returncode != 0:
        return False, ("emulator service %s is not active -- "
                       "refusing to swap a CLI-run instance" % service)
    try:
        old_text = rewrite_conf_emu_fn1(conf_path, candidate)
    except OSError as exc:
        return False, "cannot write conf: %s" % exc

    def _rollback():
        try:
            with open(conf_path + ".oled-tmp", "w") as f:
                f.write(old_text)
            os.replace(conf_path + ".oled-tmp", conf_path)
        except OSError:
            pass
        try:
            runner(["systemctl", "start", service], timeout=60)
        except Exception:
            pass

    try:
        result = runner(["systemctl", "stop", service], timeout=60)
        if result.returncode != 0:
            _rollback()
            return False, "service stop failed"
        result = runner(["systemctl", "start", service], timeout=60)
        if result.returncode != 0:
            _rollback()
            return False, "service start failed"
    except Exception as exc:
        _rollback()
        return False, "service restart failed: %s" % exc
    deadline = time.time() + verify_seconds
    while time.time() < deadline:
        running_pairs, cwd = find_running_mfm_emu(proc_root=proc_root)
        if running_pairs:
            pairs, base_dir = running_pairs, cwd
        else:
            pairs, base_dir = read_conf_files(conf_path=conf_path)
        if pairs and resolve_path(pairs[0][1], base_dir) == candidate:
            return True, ""
        time.sleep(0.5)
    _rollback()
    return False, "restarted emulator did not come up on the new file"


def _button_thread(events, stop_check):
    """Poll both buttons; queue +1 (Next) / -1 (Prev) on press/repeat."""
    try:
        next_gpio = GpioInput(BTN_NEXT_GPIO)
        prev_gpio = GpioInput(BTN_PREV_GPIO)
    except Exception as exc:
        print("mfm-oled: buttons unavailable (%s) -- display only"
              % exc, flush=True)
        return
    next_db = ButtonDebouncer()
    prev_db = ButtonDebouncer()
    try:
        while not stop_check():
            now = time.monotonic()
            pressed = next_gpio.read_pressed()
            if pressed is not None and next_db.update(pressed, now):
                events.put(1)
            pressed = prev_gpio.read_pressed()
            if pressed is not None and prev_db.update(pressed, now):
                events.put(-1)
            time.sleep(BTN_POLL_SECONDS)
    finally:
        next_gpio.close()
        prev_gpio.close()


def start_button_thread(events, stop_check):
    """Start the button poller thread. Returns it (daemon thread)."""
    thread = threading.Thread(target=_button_thread,
                              args=(events, stop_check), daemon=True)
    thread.start()
    print("mfm-oled: buttons on GPIO2_22 (Next, sysfs %d) / "
          "GPIO2_23 (Prev, sysfs %d)" % (BTN_NEXT_GPIO, BTN_PREV_GPIO),
          flush=True)
    return thread


# ---- Screen saver (FlashFloppy-style) -----------------------------------
#
# FlashFloppy's FF.CFG has display-off-secs (default 60): seconds of
# inactivity before the display turns off, and display-on-activity
# (default yes): the display wakes again on activity (track changes
# and disk writes, or drive-select only with "sel"). The mechanism
# there is described as turning off the LCD/OLED backlight; an
# SSD1306 OLED has no backlight, so the equivalent here is the
# controller's own DISPLAY OFF command (0xAE) -- the panel goes dark
# while the framebuffer RAM is kept, and DISPLAY ON (0xAF) restores
# the screen instantly with no redraw. That is what "blanking" means
# in this daemon, not clearing the framebuffer.
#
# Differences from FlashFloppy, all deliberate:
# - Timeout default is 120 s ("a couple of minutes", Kris's words),
#   not FlashFloppy's 60 s. Env: MFM_OLED_SCREENSAVER_SECONDS, 0
#   disables the saver entirely.
# - Wake sources: any button press, any change in what the screen
#   shows (file, Running/Idle, preview, swap result), and power
#   loss, which always wakes the display -- a POWER LOSS warning
#   must never sit on a dark screen. FlashFloppy's track/write
#   wake has no equivalent here: mfm_emu exposes no track or write
#   activity to userspace, so it cannot be a wake source, and the
#   scrolling of a long filename is deliberately NOT activity
#   (otherwise a long name would keep the screen lit forever).
# - A button press that wakes the display is consumed by the wake
#   alone: it does NOT also step the file selection. The next press
#   after waking acts normally. Waking and accidentally swapping
#   files in one press would be a nasty surprise.
#
# The saver never blanks while browsing, while a swap error is
# shown, or while power loss is latched.

SCREENSAVER_SECONDS = float(
    os.environ.get("MFM_OLED_SCREENSAVER_SECONDS", "120"))


class Screensaver(object):
    """Idle-blank state machine. Pure logic, testable.

    Times are monotonic seconds, supplied by the caller (the main
    loop), so the class never touches the clock or the display.
    """

    def __init__(self, timeout):
        self.timeout = timeout
        self.blanked = False
        self.last_activity = None

    def start(self, now):
        self.last_activity = now

    def note_activity(self, now):
        self.last_activity = now

    def on_button(self, now):
        """A button event arrived. True if it was consumed as a wake
        (display was blanked); False if it is a normal press."""
        if self.blanked:
            self.blanked = False
            self.last_activity = now
            return True
        self.last_activity = now
        return False

    def wake(self, now):
        """Wake for a non-button reason (content change, power loss).
        Returns True if the display was blanked."""
        if self.blanked:
            self.blanked = False
            self.last_activity = now
            return True
        return False

    def check_blank(self, now, inhibited):
        """True when the display should be blanked right now."""
        if self.timeout <= 0 or self.blanked or inhibited:
            return False
        if self.last_activity is None:
            return False
        if now - self.last_activity >= self.timeout:
            self.blanked = True
            return True
        return False


# ---- Main loop ----------------------------------------------------------

_stop = False


def _on_signal(signum, frame):
    global _stop
    _stop = True


def main(argv):
    if "--dry-run" in argv:
        pairs, running, base_dir = current_files()
        voltage, _threshold, below = power_status()
        # A single snapshot has no tick history, so one below-threshold
        # reading is enough to preview the warning.
        print(framebuffer_to_ascii(render_screen(pairs, running, 0,
                                                 voltage, below,
                                                 base_dir)))
        return 0
    once = "--once" in argv
    signal.signal(signal.SIGTERM, _on_signal)
    signal.signal(signal.SIGINT, _on_signal)
    device = None
    last_key = None
    last_layout = None
    # Filename scroll state (see SCROLL_* constants above).
    scroll_offset = 0
    scroll_phase = "hold_start"
    scroll_ticks = 0
    # Power-loss latch state (see POWER_LOSS_TICKS above).
    below_ticks = 0
    power_loss = False
    # Button browsing state. The poller thread only queues events;
    # all session/commit/display decisions happen here, in the main
    # loop, so no display or conf state is ever touched from a thread.
    # --once draws a single snapshot, so it skips the poller.
    browse_session = BrowseSession()
    button_events = queue.Queue()
    if not once:
        start_button_thread(button_events, lambda: _stop)
    swap_error_until = 0.0
    swap_error_reason = None
    # Screen saver: starts counting from daemon start; any button
    # press, content change, or power-loss wake resets it.
    screensaver = Screensaver(SCREENSAVER_SECONDS)
    screensaver.start(time.monotonic())
    wake_pending = False  # a wake happened; send DISPLAY ON on draw
    while not _stop:
        pairs, running, base_dir = current_files()
        voltage, _threshold, below = power_status()
        if below:
            below_ticks += 1
            if below_ticks >= POWER_LOSS_TICKS:
                if not power_loss:
                    print("mfm-oled: input voltage %.1fV below threshold "
                          "-- power loss" % voltage, flush=True)
                power_loss = True
        else:
            # Voltage back above threshold (or ADC unreadable and not
            # below): clear the latch. A genuine loss never recovers,
            # so the warning stays up until the board powers off.
            below_ticks = 0
            power_loss = False
        # ---- Button browsing / commit (main loop only) ---------------
        now = time.monotonic()
        current_resolved = None
        if pairs:
            current_resolved = resolve_path(pairs[0][1], base_dir)
        browse_dir = BTN_DIR_OVERRIDE
        if browse_dir is None:
            if current_resolved:
                browse_dir = os.path.dirname(current_resolved) or None
            elif base_dir:
                browse_dir = base_dir
        events_this_tick = []
        while True:
            try:
                events_this_tick.append(button_events.get_nowait())
            except queue.Empty:
                break
        if events_this_tick:
            if screensaver.on_button(now):
                # Display was blanked: this press only wakes it.
                # The events are discarded, NOT fed to browsing,
                # so waking can never accidentally swap files.
                wake_pending = True
                print("mfm-oled: display woken by button", flush=True)
            elif not power_loss:
                for direction in events_this_tick:
                    browse_session.press(direction, current_resolved,
                                         browse_dir, now)
        if power_loss:
            # Never browse or commit while the board is shutting down.
            browse_session.cancel()
            if screensaver.wake(now):
                # Power loss must never sit on a dark screen.
                wake_pending = True
                print("mfm-oled: display woken by power loss",
                      flush=True)
        elif browse_session.anchor_changed(current_resolved):
            # The current file changed behind our back (external
            # restart, conf edit): drop the stale preview.
            browse_session.cancel()
        elif browse_session.commit_due(now):
            candidate = browse_session.candidate()
            if candidate is not None \
                    and candidate == current_resolved:
                browse_session.cancel()  # scrolled full circle: no-op
            elif candidate is not None:
                # Blocking by design (service stop/start + verify,
                # seconds): the display holds the preview frame until
                # the loop resumes and shows the new file.
                print("mfm-oled: committing %s" % candidate, flush=True)
                browse_session.cancel()
                ok, reason = commit_swap(candidate)
                if ok:
                    print("mfm-oled: now emulating %s" % candidate,
                          flush=True)
                    last_layout = None  # force a refresh next tick
                else:
                    print("mfm-oled: swap failed: %s" % reason,
                          flush=True)
                    swap_error_until = now + 5.0
                    swap_error_reason = reason
        # ---- What to draw this tick -----------------------------------
        display_pairs = pairs
        status_override = None
        if browse_session.active and not power_loss:
            display_pairs = [("1", browse_session.candidate())]
            status_override = browse_session.preview_status()
        elif now < swap_error_until and not power_loss:
            status_override = "Swap failed"
        path_line, filename, status_line = build_layout(
            display_pairs, running, voltage, power_loss, base_dir,
            status_override)
        layout = (path_line, filename, status_line)
        layout_changed = (layout != last_layout)
        if layout_changed:
            # New file / state: restart the scroll from the beginning.
            last_layout = layout
            scroll_offset = 0
            scroll_phase = "hold_start"
            scroll_ticks = 0
            last_key = None
            screensaver.note_activity(now)
        if screensaver.blanked and layout_changed:
            # Content changed under a blank screen (file swapped
            # externally, emulator stopped, swap failed): wake and
            # redraw, so the new state is actually visible.
            screensaver.wake(now)
            wake_pending = True
            last_key = None
        max_offset = max(0, len(filename) - LARGE_CHARS)
        if max_offset == 0:
            scroll_offset = 0
        else:
            scroll_ticks += 1
            if scroll_phase == "hold_start" \
                    and scroll_ticks >= SCROLL_HOLD_TICKS:
                scroll_phase = "scroll"
                scroll_ticks = 0
            elif scroll_phase == "scroll" \
                    and scroll_ticks >= SCROLL_STEP_TICKS:
                scroll_ticks = 0
                if scroll_offset < max_offset:
                    scroll_offset += 1
                else:
                    scroll_phase = "hold_end"
            elif scroll_phase == "hold_end" \
                    and scroll_ticks >= SCROLL_HOLD_TICKS:
                scroll_phase = "hold_start"
                scroll_ticks = 0
                scroll_offset = 0
        key = layout + (scroll_offset,)
        if device is None:
            try:
                device = open_display()
                last_key = None  # force a redraw after reconnect
                # Init leaves the display on: the saver restarts
                # from here, and any pending wake is satisfied.
                screensaver.blanked = False
                screensaver.note_activity(now)
                wake_pending = False
                print("mfm-oled: display opened on %s at 0x%02x"
                      % (I2C_DEV, I2C_ADDR), flush=True)
            except Exception as exc:  # display absent/unplugged: keep trying
                print("mfm-oled: cannot open display: %s" % exc, flush=True)
                if once:
                    return 1
                time.sleep(RETRY_SECONDS)
                continue
        if not screensaver.blanked and (wake_pending or key != last_key):
            try:
                if wake_pending:
                    # DISPLAY ON first: without it the panel stays
                    # dark even though the framebuffer is written.
                    device.display_on()
                    wake_pending = False
                draw(device, display_pairs, running, scroll_offset,
                     voltage, power_loss, base_dir, status_override)
                last_key = key
                if layout != getattr(main, "_logged_layout", None):
                    main._logged_layout = layout
                    print("mfm-oled: showing %s (running=%s, %s)"
                          % (display_pairs[0][1] if display_pairs
                             else "no file", running, status_line),
                          flush=True)
            except Exception as exc:
                print("mfm-oled: display write failed: %s" % exc, flush=True)
                device = None
                wake_pending = False
                if once:
                    return 1
                time.sleep(RETRY_SECONDS)
                continue
        # Screen saver: blank after SCREENSAVER_SECONDS of no button
        # press and no content change, unless inhibited (browsing,
        # swap error, or power loss). While blanked, the draw block
        # above is skipped, so no I2C traffic reaches a dark panel.
        inhibited = power_loss or browse_session.active \
            or now < swap_error_until
        if screensaver.check_blank(now, inhibited):
            if device is not None:
                try:
                    device.display_off()
                    print("mfm-oled: display blanked after %.0f s idle "
                          "(screensaver)" % SCREENSAVER_SECONDS, flush=True)
                except Exception as exc:
                    print("mfm-oled: display blank failed: %s" % exc,
                          flush=True)
                    device = None
                    screensaver.blanked = False
        if once:
            break
        time.sleep(TICK_SECONDS)
    if device is not None:
        try:
            device.clear()
        except Exception:
            pass
        device.close()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
