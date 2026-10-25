# True-activity LED patch for dgesswein/mfm `mfm_emu` (PRU0)

## What changes

Stock behaviour: the J7 LED is a *drive-selected* light — on for as long
as the host holds the emulated drive selected, off otherwise. On
controllers that latch select, that is a steady light, not activity.

Patched behaviour: select no longer holds the LED on. Instead, host
*events* fire a retriggerable timed pulse on the selected drive's LED:

| Event (PRU0 hook) | Effect |
|---|---|
| (Re)selection / head change (`selected:` in both Rev C and Rev A/B paths) | LED on, hold timer restarted |
| Step pulse (`step:`) | LED on, hold timer restarted, per pulse |
| Write gate (`write:`) | LED on; stays on for the whole write (see below) |
| Deselect (`notsel:`, both paths) | LED off immediately, state cleared |

Hold timer: `ACT_HOLD_TICKS` = **6 revolutions** (≈100 ms at 3600 RPM,
≈120 ms at 3000 RPM), counted down once per revolution at `end_track:`.
While a seek is waiting on the ARM cylinder fetch, or a write is in
progress, `end_track:` is not reached, so the LED stays on for the
duration and only starts its 6-rev countdown when streaming resumes.

## How to install

### Download LED patch:

```
cd /opt/mfm
wget https://raw.githubusercontent.com/Kris-Sekula/s100/refs/heads/main/MFM_EMU_HDD_LED/true-activity-led.patch
```

### Check if the patch works:

```
debian@BeagleBone:/opt/mfm$ git apply --check true-activity-led.patch
debian@BeagleBone:/opt/mfm$ git apply --stat  true-activity-led.patch
 emu/prucode0.p |   88 ++++++++++++++++++++++++++++++++++++++++++++++++++------
 1 file changed, 79 insertions(+), 9 deletions(-)
debian@BeagleBone:/opt/mfm$
```

Expect no errors after the --check 


### Apply and check:

```
git apply true-activity-led.patch
grep -n ACT_HOLD_TICKS emu/prucode0.p | head -3

debian@BeagleBone:/opt/mfm$ grep -n ACT_HOLD_TICKS emu/prucode0.p | head -3
293:#define ACT_HOLD_TICKS 6
1384:   MOV      ACT_TICKS, ACT_HOLD_TICKS        // (Re)start the hold timer
debian@BeagleBone:/opt/mfm$
```

### Build the PRU binaries:

```
debian@BeagleBone:/opt/mfm$ cd emu && make pru
pasm -V2 -b -L prucode0.p prucode0_reva
```

Expect output like this:

```
PRU Assembler Version 0.87
Copyright (C) 2005-2013 by Texas Instruments Inc.

prucode0.p:1655: Warning: Resetting .origin value after use

Pass 2 : 0 Error(s), 1 Warning(s)

Writing Code Image of 1027 word(s)

pasm -V2 -b -L -DREVB prucode0.p prucode0_revb


PRU Assembler Version 0.87
Copyright (C) 2005-2013 by Texas Instruments Inc.

prucode0.p:1655: Warning: Resetting .origin value after use

Pass 2 : 0 Error(s), 1 Warning(s)

Writing Code Image of 1027 word(s)

pasm -V2 -b -L -DREVC prucode0.p prucode0_revc


PRU Assembler Version 0.87
Copyright (C) 2005-2013 by Texas Instruments Inc.

prucode0.p:1655: Warning: Resetting .origin value after use

Pass 2 : 0 Error(s), 1 Warning(s)

Writing Code Image of 1027 word(s)

debian@BeagleBone:/opt/mfm/emu$
```

At this point, the LED should flash every time there is activity on the HDD 
