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
