# MMU Issue 4 — Filament ejected mid-print (not during toolchange)

Status: known firmware/hardware issue. Maps to Prusa error
[**#04507 "Filament ejected"**](https://help.prusa3d.com/article/filament-ejected-04507-mmu_480424).
Primarily MMU3 firmware / hardware calibration, not a PrusaSlicer bug — but
slicer-side mitigations exist.

## Symptom

During a print (not during a planned toolchange) the MMU3:

1. Pulls the active filament fully out, back through the MMU3.
2. Moves the **selector out of the way**, leaving the filament protruding
   from the front of the unit.
3. Pauses and prompts the user to push the filament back in.

The firmware gives a very short time window to push the filament past the
loader before the recovery times out, which makes manual rescue stressful.

This is the documented behavior of error #04507 — designed for Spooljoin
(automatic end-of-spool runout) — but it is being triggered without an
actual runout.

## Why this happens (community + KB consensus)

Error #04507 is fired when the MMU3 firmware concludes that the spool is
empty and ejects so the user can attach the next spool. The eject itself is
correct firmware behavior; the bug is the **false trigger**. Root causes,
ranked by frequency:

1. **FINDA mis-calibration** — FINDA sits too high or too low; vibration or
   filament dust causes the magnetic ball to drop briefly during a print,
   the firmware reads "no filament," and triggers a Spooljoin-style eject.
   This is by far the most common cause.
2. **Debris under FINDA** — fragments of old filament tip lodged under the
   ball stop it dropping cleanly, producing intermittent state flips.
3. **Selector / output PTFE residue** — a short stub of filament left in
   the output tube holds FINDA in a stuck state.
4. **Spool/buffer friction** — when the filament feeder can't keep up, the
   FINDA momentarily reads empty.
5. **PTFE tube alignment** at the MMU input — tubes not perfectly aligned
   cause the filament to occasionally slip back past FINDA.
6. **Implausible SuperFINDA reading** — sensor detects filament in the
   selector when firmware has no record of it being loaded.
7. **FINDA hardware failure** — rare, but documented.

## Where it lives in code

This is **not** a PrusaSlicer issue per se. The eject is decided in the
MMU3 firmware (`Prusa-Firmware-MMU`) on the basis of FINDA + filament
sensor state. PrusaSlicer's only contribution is the G-code: an unwanted
`M600` or a toolchange that fires when it shouldn't could *look* like
this, but the description (selector parks out of the way, user must push
filament back) is specifically the #04507 eject sequence, not a normal
toolchange.

For our fork, the only slicer-side angle is making sure we don't emit
G-code that triggers the false runout path (e.g. unintended `M600` from
custom G-code, or extremely fast retracts that briefly empty FINDA).

## Slicer-side things to verify

- Grep generated G-code for `M600` outside of expected toolchanges.
- Check `filament_load_speed` / `filament_unload_speed` are not so high
  that FINDA briefly flips during the toolchange.
- Confirm no custom G-code (start, layer-change, before/after toolchange)
  injects loader commands that could be misread.

## Hardware fixes (these are what actually resolve the issue)

1. **Recalibrate FINDA**: push test filament in, lower FINDA until it
   touches the ball, raise in small increments until it correctly signals
   filament-present / filament-absent. Test many push/pull cycles.
   Tighten only when it never false-triggers.
2. **Clean under FINDA**: unscrew the Festo fitting, push debris out with
   a 1.5 mm Allen key, blow out with compressed air.
3. **Inspect output PTFE** for residual filament stubs.
4. **Reduce vibration** to the MMU (paving stone / sponge feet under the
   printer) — vibration-induced FINDA drops are common.
5. **Check spool tension** and buffer routing — no kinks, no high friction.
6. **Re-align input PTFE tubes** to the MMU.
7. **Replace FINDA** if recalibration and cleaning don't fix it.

## Recovery-window usability complaint

A separate, valid complaint in your report: the firmware gives "very short
time to push the filament past the loader" before recovery times out.
This is firmware-side (MMU3 FW), not slicer. The current implementation
has no setting to extend the timeout; community workaround is to be
physically present at the printer whenever the issue is known to trigger.

If we want to push upstream on this, the change would be in
`Prusa-Firmware-MMU` recovery state machine to make the manual-load
timeout configurable — out of scope for this slicer fork.

### Related UX gap: FINDA hidden during recovery prompt

Buddy firmware's status footer (configurable to show nozzle temp, bed
temp, filament sensor, FINDA) is **hidden** during the #04507 recovery
modal where the user is prompted to push filament back past the loader.
This is precisely the moment FINDA state matters most — the user needs
to know when the filament has cleared the sensor — and it isn't shown.

Workaround: watch the green FINDA LED on the MMU3 board itself instead
of the printer screen.

Upstream fix would be in `Prusa-Firmware-Buddy` GUI code (the recovery
screen should preserve at least the FINDA indicator from the footer).
Out of scope for this slicer fork; worth filing as a Buddy issue.

## Open questions

- Always the same filament slot, or random? (Same slot → FINDA / selector
  alignment for that slot. Random → FINDA itself or vibration.)
- Always at roughly the same layer height / time into the print?
  ("Filament ejected over and over after approx 10 cm height" thread
  suggests there's a class of issue tied to print progress / heat.)
- Firmware version on the printer and MMU3? Newer FW (6.4.0+) has
  unrelated regressions (see MMU-05) but no known regression here.

## Next step

1. Confirm error number on the printer screen when it happens — should
   read **04507**. If it's a different code (04101/04102/04104), this is
   a different failure mode and belongs with MMU-05.
2. Recalibrate FINDA and clean under it before anything else.
3. Capture the G-code section around the eject point and grep for
   `M600` / `M702` / unexpected toolchanges as a sanity check.

## References

- [Prusa KB #04507 — Filament ejected (MMU)](https://help.prusa3d.com/article/filament-ejected-04507-mmu_480424)
- [Prusa KB #04506 — Unload manually (MMU)](https://help.prusa3d.com/article/unload-manually-04506-mmu_480414)
- [Prusa KB #04101 — FINDA didn't trigger](https://help.prusa3d.com/article/finda-didnt-trigger-04101-mmu_393894)
- [Forum — Filament ejected over and over after approx 10 cm height](https://forum.prusa3d.com/forum/original-prusa-i3-mmu3-general-discussion-announcements-and-releases/filament-ejected-over-and-over-after-approx-10cm-height/)
- [Forum — MMU3 filament unload slips back too far](https://forum.prusa3d.com/forum/original-prusa-i3-mmu3-hardware-firmware-and-software-help/mmu3-filament-unload-filament-slips-back-to-far-and-cannot-be-grabbed-anymore/)
- [Forum — MK4S MMU3 constantly signaling end of filament roll](https://forum.prusa3d.com/forum/english-forum-original-prusa-i3-mk4s-hardware-firmware-and-software-help/mk4s-mmu3-constantly-signaling-end-of-filament-roll/)
- [Prusa-Firmware #1905 — MK3S/MMU2S randomly ejects filament at start of print](https://github.com/prusa3d/Prusa-Firmware/issues/1905)
- [MMU3 Printing Handbook — FINDA inspection (ManualsLib p. 51)](https://www.manualslib.com/manual/3197600/Prusa-Research-Prusa-Mmu3.html?page=51)
