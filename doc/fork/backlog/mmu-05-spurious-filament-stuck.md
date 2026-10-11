# MMU Issue 5 — Spurious "filament stuck" events, fixed by unload+reload on touchscreen

Status: known firmware regression. Most likely maps to MK4S/MK3.9 buddy
firmware **6.4.0+** regression. Reverting to **6.2.6** has fixed it for many
users.

## Symptom

The print pauses with a "filament stuck" prompt. The filament is **not
actually stuck** — using the touchscreen to unload and then reload works
immediately, no manual intervention, and the print resumes normally. The
event has no obvious correlation with layer, model, or filament.

Error codes commonly displayed:

- **#04102 — "FINDA: Filament Stuck"** (MMU3 FINDA state-machine)
- **#04104 — "Filament sensor: Fil. stuck"** (Nextruder filament sensor)

## Why this happens — the prevailing diagnosis

**Buddy firmware 6.4.0 regression on MK4S / MK3.9 / Core One with MMU3.**
After updating to 6.4.0 (and later patch releases on that branch), a large
number of users report one in every two or three prints stops after the
purge with the "filament stuck" prompt, despite the filament extruding fine
the moment they hit unload→load. Reverting to 6.2.6 resolves it for the
majority of reporters. Auto-retract toggling does **not** help.

There are at least two distinct sub-failures bundled under this symptom:

1. **False filament-sensor trigger (Nextruder).** The Nextruder filament
   sensor reports "no filament" briefly when the idler moves away from the
   drive gear — newer firmware made this more sensitive. Sensor
   recalibration (with attention to idler position) resolves it for some
   users.
2. **Tip deformation before retract.** On Core One specifically, the
   filament deforms inside the Nextruder before being pulled back to the
   MMU, leaving either a bulb at the top of the load cell or a fine
   string. That genuinely jams the unload, and the firmware reports
   "stuck."

## Where it lives in code

Not in PrusaSlicer. The detection logic is in the printer buddy firmware
(`Prusa-Firmware-Buddy`) and the MMU3 firmware (`Prusa-Firmware-MMU`).
PrusaSlicer's only relevant outputs are retract length/speed and the
toolchange sequence; neither has changed materially.

For this fork, there is nothing to fix in `WipeTower.cpp` — the slicer
is producing correct G-code. The issue is consumed by the printer.

## Recommended action (most → least effective)

1. **Check buddy firmware version on the printer.** If 6.4.0 or newer on
   that branch, revert to **6.2.6**. This is the single most-cited fix.
2. **Recalibrate the filament sensor** (Nextruder), paying attention to
   the idler — the sensor triggers when the idler swings away from the
   drive gear, not when the gear sees filament.
3. **Inspect the top of the heatsink / load cell** for filament bulbs or
   strings after a "stuck" event; if present, this is sub-failure #2 and
   tip-shaping (see MMU-03) needs tuning.
4. **Check the nozzle.** Some users resolved it only after replacing the
   nozzle (especially brass HF nozzles on MK4S — repeated bed probing
   deforms the tip slightly, narrowing the orifice).
5. **Cut filament tip clean before reload**, free of damage over the
   first ~40 cm.
6. **Check Bondtech idler door alignment** and gear cleanliness.
7. **Replace extruder motor** — last-resort, reported by users who
   exhausted everything else.

## Slicer-side things to check (for completeness)

- Retract length unusually high for the filament? Excess retract can pull
  the tip above the heat zone and into the cold area, causing a partial
  jam that reads as "stuck."
- Stringing during retract leaving deposits on the heatsink? See MMU-03
  tip-shaping advice (ramming volume, cooling moves).
- No unintended `M702` (unload) in custom G-code.

## Open questions

- What buddy firmware version is the user actually on? Decides whether
  this is the 6.4.0 regression or hardware.
- Does it happen at the same point in the print, or random? (Random +
  recent firmware update → regression. Same point → mechanical.)
- Is the Nextruder idler clicking back into place audibly when the event
  happens? That confirms sub-failure #1.

## Next step

1. Read the printer firmware version. If on the 6.4.0 branch, downgrade
   to 6.2.6 and re-run the same print.
2. If symptom persists, recalibrate the Nextruder filament sensor and
   inspect for filament deposits / bulbs.
3. Only after both, look at slicer-side retract settings.

## Related to other issues in this folder

- **MMU-03** (purge tower overflow) and this issue share a root cause
  when tip-shaping is wrong: poor tips both build blobs on the tower
  *and* cause "stuck" reports on retract. Tuning ramming/cooling fixes
  both.
- **MMU-04** (mid-print eject) is a distinct failure mode (error 04507,
  FINDA-based) — do not confuse with 04102/04104.

## References

- [Prusa KB #04102 — FINDA: Filament Stuck (MMU)](https://help.prusa3d.com/article/finda-filament-stuck-04102-mmu_394505)
- [Prusa KB #04104 — Filament sensor: Fil. stuck (MMU)](https://help.prusa3d.com/article/filament-sensor-fil-stuck-04104-mmu_394535)
- [Forum — Filament 'stuck', but not really after latest 6.4.0](https://forum.prusa3d.com/forum/english-forum-original-prusa-i3-mk4s-hardware-firmware-and-software-help/filament-stuck-but-not-really-after-latest-6-4-0/)
- [Forum — Filament stuck error when not stuck](https://forum.prusa3d.com/forum/english-forum-original-prusa-i3-mk4s-hardware-firmware-and-software-help/filament-stuck-error-when-not-stuck/)
- [Forum — Keep getting a filament stuck message on my MK4S](https://forum.prusa3d.com/forum/english-forum-original-prusa-i3-mk4s-hardware-firmware-and-software-help/keep-getting-a-filament-stuck-message-on-my-mk4s/)
- [Forum — Core One MMU3 Filament unloading getting stuck](https://forum.prusa3d.com/forum/prusa-core-one-hardware-firmware-and-software-help/core-one-mmu3-filament-unloading-getting-stuck/paged/2/)
- [Prusa-Firmware-Buddy #5008 — MMU3/MK3.5 won't unload on firmware filament eject](https://github.com/prusa3d/Prusa-Firmware-Buddy/issues/5008)
- [PrusaSlicer #15057 — MK4S with MMU3 gets hung on "Reloading Filament"](https://github.com/prusa3d/PrusaSlicer/issues/15057)
- [Prusa-Firmware #4849 — MMU3 fail to unload makes firmware stuck in unloading phase](https://github.com/prusa3d/Prusa-Firmware/issues/4849)
