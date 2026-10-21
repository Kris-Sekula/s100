# CompuPro CP/M-68K 1.3J

This is CP/M-68K 1.3 built for the CompuPro 68000 S-100 system:

- **Operating system:** Digital Research's 1.3 core and command processor (`CPMLIB`, 08/05/85)
- **BIOS:** CompuPro BIOS revision 9 with the S3-1 console change; "J" marks this build
- **Loader:** the 1.3 loader with the CompuPro loader BIOS
- **Built with:** Digital Research's own 1.3 tools (`AS68`, `LO68`, `RELOC`), running under CP/M-68K in the 68000 emulator

```
CP/M-68K(tm) Version 1.3  08/05/85
Copyright (c) 1985 Digital Research, Inc.

CompuPro CP/M-68K version 1.3J
Copyright (c) 1983 CompuPro

A>
```

## Disks

Both disks are bootable: 8" single-sided, track 0 FM 26×128 and tracks 1–76 MFM 8×1024, the same format as the CompuPro master disk.

| Image | Contents | Free |
|---|---|---|
| `CPM68K_1.3J_Disk1_System.imd` | `CPM.SYS`, `CPMLDR.SYS`, PIP, STAT, ED, DUMP, DDT (+DDT68000), SIZE68, and the CompuPro utilities listed below | 294K |
| `CPM68K_1.3J_Disk2_Development.imd` | `CPM.SYS`, `CPMLDR.SYS`, AS68 (+AS68SYMB.DAT), LO68, RELOC, SIZE68, PIP, CPMLIB, LDRLIB, BIOS.S, LBIOS.S, BOOT.S, MFORM.S, PUTBOOT.S, MAKESYS.SUB, MAKELDR.SUB | 134K |

- **Utilities:** these are the Digital Research 1.3 versions, in relocatable form, so they load anywhere in the TPA.
- **CompuPro utilities:** these come unchanged from CompuPro 68K System Master Disks 1 and 2:

  | Program | Use |
  |---|---|
  | `FORMAT` | DISK 1A floppy formatter 1.1. Asks for the drive and density (0–3). |
  | `DISK3` | DISK 3 hard disk formatter and tester (CompuPro 10/13/83). `DISK3 HELP` lists the options: format, verify, seek, data, all; drive types cmi5619, st506, st412, q540 (default Q540), or the drive parameters entered by hand. It writes the header the BIOS reads at cold boot. |
  | `MFORM` | M-Drive/H formatter, run as `MFORM M`. |
  | `COPY` | Disk copier 1.1, with the modes ALL, BOOT (boot tracks only) and FILES. |

  The master disks have no separate M-Drive/H control program; `MFORM` is the only M-Drive/H utility on the set.
- **C compiler:** it doesn't fit on these disks. The 1.1 C compiler on the CompuPro master disk should also run under 1.3, but that hasn't been tested.

## Memory

| Address | Use |
|---|---|
| 000400–0168FF | TPA (90K) |
| 016900–01FF77 | CP/M-68K 1.3J (operating system, BIOS, buffers) |

The system is built for 128K of RAM. With more RAM, rebuild with a higher base address so the TPA grows (see *Rebuilding*).

## Drives

| Drive | Device |
|---|---|
| A–D | DISK 1A floppies 0–3 (density and sides detected when the disk is logged in) |
| E–I | DISK 3, unit 0, as five logical drives |
| M | M-Drive/H (size found at cold boot) |

**DISK 3**
- **Layout:** the build uses CompuPro's 40 MB (Q540) layout (`d3m40`). E: is the same as the 5 MB layout. F:–I: are the higher partitions, so on a smaller drive only the partitions that fit on it can be used.
- **Cold boot:** the BIOS resets the DISK 3. It reads the drive geometry from the CompuPro header on cylinder 0, which CompuPro's DISK 3 format program writes.
- **No DISK 3:** it prints `DISK3 controller does not respond` and continues. If the controller answers but the drive isn't formatted, it prints `DISK3 unit 0 is not formatted`.

**M-Drive/H**
- **Cold boot:** the BIOS counts the M-Drive/H boards and sizes drive M: to match.
- **Format:** `MFORM M` formats the M-Drive/H.

**Changes in 1.3J for missing hardware.** If the DISK 3 or the M-Drive/H wasn't found at cold boot, selecting its drives now gives a normal `select error`. In the original BIOS, selecting E: with no DISK 3 hung forever, and M: with no M-Drive crashed with a divide-by-zero exception.

**Bus echo.** On a bus that echoes data back from empty addresses, the M-Drive check can be fooled into finding a board that isn't there. M: would then appear but give errors.

## Console

The console follows DISK 1A switch **S3-1**, as in the patched 1.1I:

| S3-1 | Console |
|---|---|
| ON | System Support 1 |
| OFF | Interfacer 3/4 user 7 |

This applies to the loader banner, CP/M and all programs. The printer (LST:) is Interfacer user 4, or user 5 when the I/O byte selects UL1.

## Changes from CompuPro's BIOS revision 9

`BIOS.S` and `LBIOS.S` here are the full sources. The changes are marked "1.3J" or commented.

- **Version:** `make = 3`, `patch = 10`, so the sign-on reads "1.3J".
- **Devices:** M-Drive/H enabled (`hmdrive = 0`, as in CompuPro's source); DISK 3 enabled with the 40 MB layout (`d3m40 = 0`).
- **Console:** chosen by S3-1 at run time (`csel`), instead of the `syssup` build switch.
- **Memory:** `himem = $16900`, the top of the TPA and the system's load address.
- **Missing hardware:** `d3ok` and `mdok` flags, so missing hardware gives a select error.
- **Loader BIOS:** console follows S3-1; the serial set-up is unchanged.

## Rebuilding (on Disk 2, under CP/M)

```
A>SUBMIT MAKESYS      (BIOS.S -> CPM.SYS)
A>SUBMIT MAKELDR      (BOOT.S + LBIOS.S -> CPMLDR.SYS)
```

- **Checked:** both were run in the emulator on Disk 2. They rebuilt `CPM.SYS` and `CPMLDR.SYS` byte for byte identical to the ones supplied.
- **More RAM:** to use more memory, change `himem` in `BIOS.S` and the `-b` value in `MAKESYS.SUB` to the same new base address. Keep base + 9678h within your RAM.
- **Boot tracks:** the boot tracks on these images already hold the new loader. `PUTBOOT.S` is CompuPro's generic source and hasn't been tested with this disk format, so make new boot disks by copying an image.

## Testing done (68000 emulator)

- **Full boot, S3-1 ON and OFF:**
  - path: Boot ROM 2.5, the DISK 1A boot routine, the 1.3 loader from the boot tracks, then `CPM.SYS`
  - result: banner and sign-on on the selected port; `DIR`, `STAT`, `PIP` copies, `DUMP`, `DDT` and running a copied program all worked
- **Missing hardware:** `DIR M:` and `DIR E:` give select errors.
- **CompuPro utilities:** `FORMAT`, `DISK3 HELP`, `COPY` and `MFORM` start and show their prompts and help under 1.3J. Formatting and copying weren't run, because the emulator has only one floppy and no DISK 3.
- **Rebuild:** `SUBMIT MAKESYS` and `SUBMIT MAKELDR` reproduce the supplied files.

**Not tested:** the emulator has no DISK 3 or M-Drive/H model, so drives E:–I: and M: still need testing on the real hardware.

## Sources

- **Digital Research CP/M-68K 1.3** (CPMLIB, LDRLIB, tools): taken from the 1.3 distribution files in Damian Wildie's cpm-68k project (github.com/dwildie/cpm-68k).
- **CompuPro sources:** BIOS, loader BIOS and BOOT.S from CompuPro 68K System Master Disk 2 of 3.
