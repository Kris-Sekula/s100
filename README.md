<div align="center">

# CompuPro CP/M Variants

**Godbout · CompuPro · Viasyn — S-100 / IEEE-696**

*Every CompuPro-branded CP/M, sorted out: what was sold, what hardware it needed, and which boot disks survive today.*

## Headlines

- CompuPro genuinely sold **8 CP/M-family operating systems** for S-100: CP/M-80, CP/M-86, CP/M 8-16, MP/M-86, MP/M 8-16, Concurrent CP/M 8-16, Concurrent DOS 8-16, and CP/M-68K.
- **7 of the 8** have a bootable CompuPro image archived today. Only standalone MP/M-86 has no standalone image found — it survives in its MP/M 8-16 build form.
- **No** CompuPro-branded CP/M Plus / CP/M 3.0 product, and **no** standalone MP/M-80 product, was found in the sources searched. See [The two ghosts](#the-two-ghosts).
- For CP/M-68K, only **1.1** is archived as a bootable CompuPro set. Revision **1.3** is the latest version documented anywhere, but no CompuPro 1.2 or 1.3 boot image exists in a public archive checked.
- Correction on 8-16: its standard CPU is the single **CPU 8085/88** board — an 8085 and an 8088 on one card — not a CPU-Z + CPU 86/87 pair.

Researched 2026-10-23 from CompuPro manuals, period sources including the February 1984 *Data Decisions* System 816 profile and *S-100 Journal* (Spring 1986), and the bitsavers, Dunfield and SIMH / cpmarchives collections. Links were re-verified 2026-10-23 where noted in the source catalogue.

## The one thing that makes a CompuPro disk a CompuPro disk

The CompuPro part was never the BDOS — it is the **boot / BIOS layer**:

- Disk 1 / Disk 1A and Disk 2 / Disk 3 drivers
- Interfacer / System Support 1 console support
- M-Drive/H RAM disk as drive `M:`
- The boot-ROM path, BOOT loader and CompuPro CBIOS — later TMXBIOS on the x86 line, XIOS under Concurrent

A generic Digital Research distribution has none of that and **will not boot CompuPro hardware as-is**. That distinction decides almost every “does it survive?” answer below.

## At a glance

<!-- Embedded HTML table: GitHub renders this, including the alignment attributes. Markdown tables cannot centre a heading block or merge the title row like this. -->

<table>
  <thead>
    <tr>
      <th align="left">OS</th>
      <th align="left">Versions seen</th>
      <th align="left">CPU / hardware</th>
      <th align="right">Min RAM</th>
      <th align="left">Users / tasks</th>
      <th align="left">Bootable archive?</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><strong>CP/M-80</strong></td>
      <td>2.2H / K / N / Q / NZ / LD / LM</td>
      <td>CPU-Z or CPU 8085/88; SS1 / Interfacer 3/4 console; Disk 1/1A at C0h</td>
      <td align="right">64K</td>
      <td>1 user, 1 task</td>
      <td>Yes — Dunfield, SIMH</td>
    </tr>
    <tr>
      <td><strong>CP/M-86</strong></td>
      <td>1.1PA/PAA, 1.1PB, 1.1PD, physical “1R” master</td>
      <td>CPU 86/87 (8086 + opt. 8087) or 8088 side of 8085/88; GO86 EPROM on SS1; Disk 1 rev F+ / 1A</td>
      <td align="right">128K</td>
      <td>1 user, 1 task</td>
      <td>Yes — Dunfield, SIMH</td>
    </tr>
    <tr>
      <td><strong>CP/M 8-16</strong></td>
      <td>1.1R</td>
      <td>CPU <strong>8085/88</strong> single board (8085 + 8088 on one card)</td>
      <td align="right">128K</td>
      <td>1 user, 1 task</td>
      <td>Yes — SIMH, bitsavers</td>
    </tr>
    <tr>
      <td><strong>MP/M-86</strong></td>
      <td>2.0 / 2.1</td>
      <td>System 816/D (8086), /F (286)</td>
      <td align="right">—</td>
      <td>Multi-user</td>
      <td>No standalone CompuPro image found</td>
    </tr>
    <tr>
      <td><strong>MP/M 8-16</strong></td>
      <td>V2.1F, by G&amp;G Engineering</td>
      <td>CPU 8085/88</td>
      <td align="right">192K</td>
      <td>Up to 8 users; mixed 8- and 16-bit programs</td>
      <td>Yes — SIMH</td>
    </tr>
    <tr>
      <td><strong>Concurrent CP/M 8-16</strong></td>
      <td>3.1</td>
      <td>CPU 8085/88</td>
      <td align="right">—</td>
      <td>Multi-user <em>and</em> multi-tasking, virtual consoles</td>
      <td>Yes — SIMH</td>
    </tr>
    <tr>
      <td><strong>Concurrent DOS 8-16</strong></td>
      <td>3.1x – 5.0</td>
      <td>CPU 8085/88 or 8086/286; Disk 1/1A + Disk 2/3; up to 14 users via Interfacer 3/4</td>
      <td align="right">256K</td>
      <td>Multi-user, multi-tasking; runs DOS programs (PC module); SPUZ Z80 slaves (up to 16)</td>
      <td>Yes — bitsavers (rich)</td>
    </tr>
    <tr>
      <td><strong>CP/M-68K</strong></td>
      <td>1.0 / 1.1 / <strong>1.3</strong></td>
      <td>CPU 68K (8/10 MHz, opt. 68451 MMU)</td>
      <td align="right">—</td>
      <td>1 user, 1 task</td>
      <td><strong>1.1 only</strong> — bitsavers</td>
    </tr>
  </tbody>
</table>

RAM minimums are per the Disk 1A manual: 64K for CP/M-80, 128K for CP/M-86 and 8-16, 192K for MP/M 8-16, and 256K for Concurrent DOS 8-16.

## The variants, one by one

Click a heading to expand it — this uses GitHub-supported `<details>` / `<summary>` HTML, no JavaScript required.

<details>
<summary><strong>1. CP/M-80 2.2 — the Z80 baseline · boot image survives</strong></summary>

Single-user, single-tasking. Runs on CPU-Z or the 8085 side of the CPU 8085/88, with System Support 1 or Interfacer 3/4 as console, Disk 1/1A at C0h, and a 64K minimum.

Revisions seen: 2.2H, 2.2K, 2.2N, 2.2Q, 2.2NZ, 2.2LD and 2.2LM, differing mostly in BIOS and M-Drive/H support. Best survivor of the family.

**Archived**

- Dunfield — CompuPro CP/M 2.2 for Disk1A (285K IMD): [dunfield.classiccmp.org/img42841/system.htm](http://dunfield.classiccmp.org/img42841/system.htm)
- cpmarchives SIMH “CompuPro Collection” — 64K CP/M 2.2Q (85/88) by VIASYN Corp, and 64K CP/M 2.2NZ with 8 MB M-Drive/H + Disk2/M20: [cpmarchives.classiccmp.org](http://cpmarchives.classiccmp.org/cpm/mirrors/www.schorn.ch/cpm/intro.php)

</details>

<details>
<summary><strong>2. CP/M-86 1.1 — pure 16-bit, single-user · boot image survives</strong></summary>

For CPU 86/87 (8086 + optional 8087) or the 8088 side of the 8085/88. 128K minimum, needs the GO86 EPROM on the SS1 and Disk 1 rev F or later / 1A.

Versions seen: 1.1PA/PAA, 1.1PB, 1.1PD, plus a physical 1.1 “1R” system master.

**Archived**

- Dunfield — CompuPro CP/M 86 for Disk1A (two images, 330K/335K), plus CompuPro CPU-8085/86 System Disks (850K) and CPU-8086/87 System Disks (940K): [dunfield.classiccmp.org/img42841/system.htm](http://dunfield.classiccmp.org/img42841/system.htm)
- cpmarchives SIMH “CompuPro Collection” — CP/M 86 1.1PB (Digital Research / Sorcim / CompuPro): [cpmarchives.classiccmp.org](http://cpmarchives.classiccmp.org/cpm/mirrors/www.schorn.ch/cpm/intro.php)

</details>

<details>
<summary><strong>3. CP/M 8-16 1.1R — the dual-processor one-OS trick · boot image survives</strong></summary>

> [!IMPORTANT]
> The standard CPU is the single **CPU 8085/88** board (8085 and 8088 together on one card), not a CPU-Z + CPU 86/87 pair.

“8-16” is a CompuPro marketing name, not a DRI product — CompuPro called it a “proprietary version of CP/M-86.” One OS, and the file type picks the CPU: `.COM` runs on the 8085, `.CMD` runs on the 8088. Single-user, single-tasking, 128K minimum.

**Archived**

- cpmarchives SIMH “CP/M 86” package — literally CompuPro CP/M 8-16 version 1.1R: [cpmarchives.classiccmp.org](http://cpmarchives.classiccmp.org/cpm/mirrors/www.schorn.ch/cpm/intro.php)
- bitsavers — `CPM8-16_T-2.zip` (1.3 MB) in the CompuPro 5.25″ set: [bitsavers.org/bits/Compupro/5inch/](https://bitsavers.org/bits/Compupro/5inch/)

</details>

<details>
<summary><strong>4. MP/M-86 2.0 / 2.1 — pure 16-bit multi-user · no standalone image found</strong></summary>

Multi-user on the System 816/D (8086) and /F (286).

No standalone CompuPro boot image was found — it survives only as the 8-16 build below. Generic DRI MP/M-86 file sets are not a substitute: they lack the CompuPro XIOS/BIOS.

</details>

<details>
<summary><strong>5. MP/M 8-16 V2.1F — the dual-processor multi-user one · boot image survives</strong></summary>

Implemented by G&G Engineering: up to 8 users running any mix of CP/M-80 and CP/M-86 programs, 62K TPA per user vs 48K under plain MP/M, 192K minimum. The packaged System 816/C was 3 users base, 7 max recommended.

**Archived**

- cpmarchives SIMH “Concurrent CP/M 86 and MP/M 86” package — MP/M-86 2.1 [20 July 82], identified as CompuPro MP/M 8-16 V2.1F: [cpmarchives.classiccmp.org](http://cpmarchives.classiccmp.org/cpm/mirrors/www.schorn.ch/cpm/intro.php)

</details>

<details>
<summary><strong>6. Concurrent CP/M 8-16 3.1 — multi-user and multi-tasking · boot image survives</strong></summary>

CompuPro’s build of Concurrent CP/M-86 3.1, with virtual consoles: multi-user *and* multi-tasking.

**Archived**

- Same cpmarchives SIMH package — Concurrent CP/M-86 3.1 (1983), identified as CompuPro Concurrent CP/M 8-16: [cpmarchives.classiccmp.org](http://cpmarchives.classiccmp.org/cpm/mirrors/www.schorn.ch/cpm/intro.php)

</details>

<details>
<summary><strong>7. Concurrent DOS 8-16 3.1x – 5.0 — the longest-lived line · boot image survives</strong></summary>

A DRI + Viasyn compound trademark. Loaders for both the 8085/88 (`.88`) and the 8086/286 (`.86`); Disk 1/1A plus Disk 2/3; up to 14 users via Interfacer 3/4; 256K minimum. Runs DOS programs via its PC module and supports SPUZ Z80 slave processors — up to 16, one processor per user.

**Archived — richly, on bitsavers**

All in [bitsavers.org/bits/Compupro/5inch/](https://bitsavers.org/bits/Compupro/5inch/):

- `CCPM1.IMD.gz` through `CCPM6.IMD.gz` — Concurrent CP/M disk set
- `CDOS_4.1_Bkup_A_1` through `_A_5.IMD.gz` + label photos (`.jpg`) — Concurrent DOS 4.1 backup set
- `CDOS_8-16_SRC_4.1C.zip` (962 KB) — Concurrent DOS 8-16 4.1C source
- `XIOS_4.1_SRC_1/2.IMD.gz` — XIOS source

> [!WARNING]
> bitsavers also holds `CDOS_version_5A-3_for_the_CompuPro_10.zip` — that is for the non-S-100 CompuPro 10/10+, not the 816. Don’t chase it for an S-100 bus.

</details>

<details>
<summary><strong>8. CP/M-68K 1.0 / 1.1 / 1.3 — the 68000 line · 1.1 only survives</strong></summary>

For the CPU 68K (8/10 MHz, optional 68451 MMU), single-user, single-tasking.

1.3 is the latest CompuPro release documented anywhere — *S-100 Journal*, Spring 1986, lists it as a $125 upgrade (old disks returned for RMA) including a full C compiler with floating point and SPUZ Z-80 slave support — but no source explicitly calls it final, and **no CompuPro 1.2 or 1.3 boot image exists in any public archive checked** (bitsavers, archive.org, deramp, s100computers, Dunfield, retroarchive, GitHub). A physical 1.3 8″ floppy is attested in private hands (VCFed “Help with Compupro 68K”, Feb 2026) but has never been imaged or shared.

**Archived — 1.1 only**

- bitsavers — [CompuPro_68K_System_Masters.zip](https://bitsavers.org/bits/Compupro/CompuPro_68K_System_Masters.zip) (2.9 MB, added 2022-11-17). 3-disk 8″ single-sided 1024 B/S System Master set, labels read VERSION 1.1E, serial C68-0272-000408. Direct strings inspection of all 3 IMDs: “Version 1.1” 84× total, zero “Version 1.2” / “Version 1.3”; Disk 1 is bootable (`CPM.SYS`, `CPMLDR.SYS`, `PUTBOOT.68K`); Disk 2 embeds the CompuPro CBIOS source for CPU-68K + Disk 1.
- Mirror on deramp.com as `CompuPro_CPM68K_System_Masters.zip` (verified 2026-10-21).

> [!CAUTION]
> The generic DRI CP/M-68K 1.2 / 1.3 binaries floating around are DRI distributions, not CompuPro System Masters — no CompuPro CBIOS, so they will not boot as-is on CompuPro hardware.

</details>

## The two ghosts

These names come up constantly in listings and forum posts. The evidence supports “no product found in sources searched,” not an absolute proof that nothing ever existed.

### CP/M Plus (CP/M 3.0)

Only a third-party CP/M 3 Disk-1 BIOS driver by Dick Lieber (22 May 1983). No CompuPro manual, catalogue entry, or System Master was found.

### Standalone MP/M-80 / MP/M II

A Feb–Mar 1982 CompuPro ad claims the 85/88 “runs MP/M II,” but no CompuPro manual or master turned up. Multi-user 8-bit was folded into MP/M 8-16 instead.

## Verification notes

- bitsavers Compupro directories were re-listed live on 2026-10-23; file names above come from those listings. Note the case: `bits/Compupro/` (lowercase `p`); `bits/CompuPro/` 404s.
- Dunfield and cpmarchives contents come from their published index pages (crawled indexes verified 2026-10-23); individual disk contents were not re-downloaded in that pass.
- The MP/M-86 standalone-image absence, and the two ghosts, are “no source found” claims, not proofs of non-existence.

---

<div align="center">

*CompuPro CP/M Variants — researched 2026-10-23. S-100 / IEEE-696.*

</div>
