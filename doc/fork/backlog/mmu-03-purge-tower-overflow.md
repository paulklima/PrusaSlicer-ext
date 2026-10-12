# MMU Issue 3 — Wipe tower over-purges; purged filament sticks above the tower and is hit by the nozzle

Status: actively diagnosed. Root cause is partially hardware (HF
nozzle internal exit geometry) and partially slicer-side
(`wipe_tower_bridging` anchor failure). Settings-only mitigation is
available; two optional fork-side improvements documented below.

## Symptom

The purge tower accumulates filament strands and blobs above its
nominal top surface. On the next layer, the nozzle drags through them,
leaving marks on the tower and (worse) carrying contamination back
onto the model. In severe cases the blob hardens, the nozzle collides
with it, and the tower is knocked off or the print shifts (XY layer
offset).

This is the single most-reported MMU defect on Prusa hardware.
Upstream tracking issues:

- [PrusaSlicer #14784 — Wipe tower blob crash mitigation](https://github.com/prusa3d/PrusaSlicer/issues/14784) — open, no Prusa response, no PR.
- [PrusaSlicer #13897 — Bridged purge extrusions fail to anchor at default 10 mm `wipe_tower_bridging`](https://github.com/prusa3d/PrusaSlicer/issues/13897) — open, with a concrete mechanism documented in the thread.

## Empirical observation: brass works, HF fails

Confirmed in this fork's testing on MK4S MMU3: same printer, same
filament, same temperature, same ramming curve, same print profile.

| Nozzle | `multimaterial_purging` | Result |
|---|---|---|
| 0.4 mm brass MMU3 | 80 mm³ | tower stays flat; print completes cleanly |
| 0.4 HF MMU3 | 240 mm³ | tower drifts upward; nozzle eventually collides |

Both profiles inherit `*MK4IS_common*` and share
`wipe_tower_extra_flow = 250 %`, `wipe_tower_extra_spacing = 110 %`,
`wipe_tower_bridging = 10 mm`. The only profile-level differences are
`multimaterial_purging` (80 vs 240 mm³) and the `nozzle_high_flow`
flag.

## Root cause — Prusa acknowledges it

Prusa themselves state on their forum that the HF nozzle's internal
**4-channel split exit** impairs tip-shaping during ramming. The
HF MMU3 profile ships with 240 mm³ purge instead of 80 mm³ *because of*
this — more flushing is needed to remove the mis-formed tip residue
before the next colour is laid down. The blob/crash on the tower top
is a side-effect of the same root cause: the tip never emerges clean,
and residue accumulates.

References:
- Prusa forum — *High flow nozzle with MMU3*: <https://forum.prusa3d.com/forum/original-prusa-i3-mmu3-hardware-firmware-and-software-help/high-flow-nozzle-with-mmu3/>
- Prusa KB — *Prusa nozzle vs CHT nozzle*: <https://help.prusa3d.com/article/mmu3-prusa-nozzle-vs-cht-nozzle_779168>
- Prusa DE forum — user reports the wipe tower depth dropped from ~10 cm to ~1 cm after swapping HF → standard ObXidian on MMU3: <https://forum.prusa3d.com/forum/hilfe-zur-hardware-firmware-und-software-3/mmu3-und-e3d-prusa-nozzle-high-flow-obxidian-0-4-mm/>

## Two failure sources, both outside the slicer's Z budget

The bulk wipe fill geometry is **not** the cause — verified in code,
next section. The failures come from two sources the slicer does not
model in its per-layer Z accounting:

### Source A — Tip residue per toolchange (hardware-dominant)

Each ramming/retract on the HF nozzle leaves a non-conical,
multi-headed tip clinging to the 4-channel exit. On the next load,
residual melt mixes with new filament and emerges as a blob at the
**end of the next ramming line** — one specific X range per
toolchange, alternating direction. Brass has the same mechanism but
with a clean conical tip; residue per toolchange is much smaller.

Effect on the tower top: small bumps at the same X bands every layer.
Squish from the next layer absorbs perhaps 70 %; the rest persists
vertically. Rough magnitudes (estimated, not measured):

|  | Per-toolchange bump | After squish | 200-layer drift at 4 toolchanges/layer |
|---|---|---|---|
| Brass | ~0.05 mm | ~0.015 mm | ~12 mm — likely still below the nozzle plane |
| HF    | ~0.15 mm | ~0.045 mm | ~36 mm — collides well before print end |

The ~3× per-bump factor is qualitative (4-channel exit), independent
of the volume scaling in Source B.

### Source B — Volume-scaled side-effects

The HF profile dumps 3× the wipe volume per toolchange (240 vs
80 mm³) → 3× the number of wipe passes → 3× the number of:

- **Line endings with pressure-advance overshoot.** Each line end has
  a pressure spike. At `extra_flow = 250 %` the spike is fatter than
  at lower flows.
- **Bridge events between perimeter anchors.** At the default
  `wipe_tower_bridging = 10 mm`, bridged extrusions fail to anchor
  and lift up as loose loops (PrusaSlicer #13897 mechanism).

Brass has the same per-line failure modes but at 1/3 the rate.

## Code-verified wipe-tower geometry

A previous draft of this document claimed the slicer over-extrudes the
tower by ~2.27× because `extra_flow` and `extra_spacing` were
decoupled. That bug existed pre-2024 but was fixed in commit
[`ba37505`](https://github.com/prusa3d/PrusaSlicer/commit/ba37505)
(2024-01-31, *"Wipe tower: fixed depth calculation for nonzero extra
spacing; fixed issues with non-unity extra flow (incorrect wiping
volumes, overlaps)"*). The current code at
`src/libslic3r/GCode/WipeTower.cpp:550–552`:

```cpp
m_extra_flow(float(config.wipe_tower_extra_flow/100.)),
m_extra_spacing_wipe(float(config.wipe_tower_extra_spacing/100. * config.wipe_tower_extra_flow/100.)),
m_extra_spacing_ramming(float(config.wipe_tower_extra_spacing/100.)),
```

`m_extra_spacing_wipe` is the **product** `extra_spacing × extra_flow`.
At `WipeTower.cpp:1202` (non-first layer):

```cpp
float dy = (is_first_layer() ? m_extra_flow : m_extra_spacing_wipe) * m_perimeter_width;
```

So on non-first layers: `dy = extra_spacing × extra_flow × perimeter_width`.

### Numbers at defaults

For `perimeter_width = 0.5 mm`, `extra_flow = 250 %`,
`extra_spacing = 110 %`, `multimaterial_purging = 240 mm³` (HF),
`layer_height = 0.2 mm`, `inner_tower_width ≈ 30 mm`:

| Quantity | Value |
|---|---|
| Line cross-section width | 2.50 × 0.5 = **1.25 mm** |
| Y pitch between lines (non-first layer) | 2.75 × 0.5 = **1.375 mm** |
| Y pitch on first layer | 2.50 × 0.5 = 1.25 mm (lines touch exactly) |
| Gap between adjacent lines (non-first) | ~0.13 mm (~9 %) |
| Number of wipe passes per toolchange | 33 |
| Reserved depth | 33 × 0.5 × 2.75 = **45.4 mm** |
| Volume capacity at that depth | 30 × 45.4 × 0.2 = 272 mm³ |
| Material extruded | 240 mm³ |
| Fill ratio | **88 % (12 % air gaps)** |

The slicer already reserves 12 % more volume than the wipe deposits.
The bulk fill is geometrically clean and cannot be the cause of the
HF failure.

### Key files and lines

`src/libslic3r/GCode/WipeTower.cpp`:

| File:line | What |
|---|---|
| `:550–552` | Init: `m_extra_spacing_wipe = extra_spacing × extra_flow` |
| `:949–971` | Ramming loop — time-stepped extrusions while moving X |
| `:991–1000` | Retract / unload at start speed |
| `:1003–1059` | Cooling moves; stamping pushes filament back to spread blobs |
| `:1193–1202` | Wipe loop — line width, dy pitch, extrusion flow |
| `:1564–1569` | `get_wipe_depth()` — reserved depth per toolchange |
| `:1591–1601` | `plan_toolchange()` — ramming_depth + wiping_depth per layer |

### Profile defaults (`resources/profiles/PrusaResearch.ini`)

| Key | MK4S MMU3 brass | MK4S MMU3 HF |
|---|---|---|
| `multimaterial_purging` | **80 mm³** | **240 mm³** |
| `nozzle_high_flow[0]` | 0 | 1 |
| `wipe_tower_extra_flow` (print profile) | 250 % | 250 % |
| `wipe_tower_extra_spacing` (print profile) | 110 % | 110 % |
| `wipe_tower_bridging` (print profile) | 10 mm | 10 mm |

## What ramming does (background)

Ramming is a rapid extrusion burst with the heater still on, performed
just before the filament is retracted out of the hotend. Its purpose
is to shape the **tip** of the unloaded strand so it doesn't string,
doesn't form a blob, and fits cleanly back through the PTFE/heatbreak.

Per toolchange the slicer emits:

1. **Ram** (`WipeTower.cpp:949–971`) — push a small volume (≈10–15 mm³
   for PLA) out fast while moving in X. Internal melt pressure forms
   a clean conical tip on brass; a multi-headed clinging tip on HF
   (Source A above).
2. **Retract / unload at start speed** (`:991–1000`) — pull the strand
   back rapidly so the tip doesn't relax into a blob.
3. **Cooling moves** (`:1003–1049`) — bounce the filament
   forward/backward inside the cooling tube to solidify the tip
   thin.
4. **Stamping** (`:1049–`) — short push back into the nozzle that
   flattens residual ooze sideways onto the tower instead of letting
   it pile at one X. The HF tip resists this flattening because of
   step 1's poorer initial shape.
5. **Park** at cooling-tube position; load next filament with optional
   `extra_loading_distance`.

The ramming graph in *Filament Settings → Advanced → Ramming settings*
is a time → volumetric-speed curve. Area under it = total rammed
volume. Per-filament, not per-nozzle.

## Community-validated workarounds, by mechanism

Re-grouped by *what they actually address* rather than the original
"effectiveness order". The user-confirmed HF-vs-brass case is
hardware-tip-shape (Source A) plus volume-scaled bridging anchor
failures (Source B); workarounds that only address generic tip-shape
(temp, ramming graph, cooling moves) cannot explain why brass works
and HF fails on the same printer and filament.

### Group 1 — Address the HF tip-shape root cause

- **Use the standard brass nozzle for MMU3** (Prusa's own
  recommendation). Tower depth shrinks ~10× (German forum datapoint
  above).
- **Use the CHT nozzle.** Prusa's KB explicitly compares HF and CHT
  for MMU3; CHT keeps the volumetric advantage with a single annular
  exit channel that shapes the tip cleanly.

### Group 2 — Reduce per-line bridging failures (#13897)

- **`wipe_tower_bridging` 10 mm → 3–5 mm.** Single most concrete
  upstream-documented slicer-side mechanism. Bridged extrusions at
  ≤5 mm spans anchor reliably; at 10 mm they don't.

### Group 3 — Redistribute material so residue lands in gaps, not on lines

This is the spirit of the "make the tower geometrically larger than
necessary, with deliberate gaps" idea. The slicer's default 88 % fill
means the gaps exist *between* lines, not *under* them — they cannot
absorb residue that lands directly on a line. Widening the gaps so
they are comparable to the line width makes the next layer's wipe
pass more likely to land *on the gap* than on yesterday's blob.

- **`wipe_tower_extra_flow` 250 % → 100–150 %.** Narrower lines
  (0.5–0.75 mm vs 1.25 mm) bridge better (anchor fix per #13897) and
  carry less PA overshoot per line end.
- **`wipe_tower_extra_spacing` 110 % → 180–220 %.** Widens the gap
  between lines from ~0.13 mm (essentially fused) to ~0.7 mm
  (gap ≈ line width). The fill becomes "ventilated".
- **Wipe-into-infill** (right-click on a model → Wipe options) reduces
  the per-toolchange tower volume in the first place.

### Group 4 — Damage mitigation (after the tower has already grown too tall)

- **Park the nozzle off the tower during toolchange** via custom
  toolchange G-code (`G1 Z{layer_z + 10}` lift + small XY hop). Avoids
  collision without fixing growth.
- **Reduce travel acceleration / input-shaping speed** over the tower
  on MK4/Core One. Softens the impact, doesn't prevent it.
- **Move the tower** away from the model so traversal doesn't cross
  it. Delays damage to the model, doesn't prevent tower growth.

### Group 5 — Generic tip-shape tuning (orthogonal to brass-vs-HF)

These help when a specific filament rams poorly, but cannot explain
the brass-vs-HF asymmetry because both profiles use the same ramming
parameters:

- Lower hotend temp 5–10 °C for wet / off-brand filament.
- Re-tune the ramming graph (shorten total time, raise peak speed).
- Increase cooling moves; ensure `filament_stamping_distance > 0`.
- Set `extra_loading_distance` more negative (e.g. −18 mm) to remove
  the post-reload blob.

### Forum claims that don't survive scrutiny

- **"`filament_max_volumetric_speed` is the real over-flow source."**
  Volumetric speed caps mm³/s, not mm³ per toolchange. It cannot
  reduce per-layer deposited material. Slower wiping can dampen PA
  overshoot at line ends, but this is not a *volume* fix as several
  forum posts claim.

## Recommended settings combo (no code change)

Targets Sources A and B together. One combo, two severities:

| Knob | Default | Moderate | Aggressive |
|---|---|---|---|
| `wipe_tower_extra_flow` | 250 % | 150 % | 120 % |
| `wipe_tower_extra_spacing` | 110 % | 180 % | 220 % |
| `wipe_tower_bridging` | 10 mm | 4 mm | 4 mm |

Resulting tower geometry at 240 mm³ HF purge, 0.2 mm layer height,
0.5 mm perimeter width, 30 mm inner width:

| Setting | Line width | Pitch | Gap | Depth |
|---|---|---|---|---|
| Default | 1.25 mm | 1.375 mm | 0.13 mm | 45 mm |
| Moderate | 0.75 mm | 1.35 mm | 0.60 mm | ~73 mm |
| Aggressive | 0.60 mm | 1.32 mm | 0.72 mm | ~88 mm |

Trade-off: the moderate combo eats ~28 mm of additional Y bed depth;
the aggressive combo ~43 mm. Width is unchanged.

Optional additional experiment, untested in the community:

- **`multimaterial_purging` on the HF profile: 240 → 80 mm³.** Matches
  the brass value directly. Risk: visible colour bleed on dark→light
  transitions, which is exactly why Prusa picked 240 in the first
  place. Run on a colour-tolerant test print first.

## Fork-side proposals

Only if the settings combo isn't enough on a given filament/printer
pair.

### Option A — Iron the last toolchange of each layer

After the last toolchange of a layer, do one slow zero-extrusion pass
across the top of the freshly purged box to flatten any blob before
the nozzle leaves the tower.

Sketch:

- In `WipeTower::toolchange_Wipe` / `finish_layer`, detect "last
  toolchange of this layer" (`m_layer_info`, `tool_changes` count).
- Emit a single sweep at `m_perimeter_width / 2` above the tower
  surface, feedrate ~20 mm/s, `E = 0`. One pass across the wiped X
  range.
- Gate behind a new advanced bool `wipe_tower_iron_top`, default off.

Effort: ~40 lines in `WipeTower.cpp` plus a tooltip update.

Targets Source A residue. Does not address Source B bridging
failures. Caveat: hot nozzle at low Z may pull blobs into trails
rather than flatten them — validate empirically.

### Option B — Rolling pit for ramming residue

The Source A blob lands at the **end of the ramming line**, a
predictable X range per toolchange. Instead of flattening it after the
fact, carve a place for it to go *before* it forms. Each layer leaves
a small (~2×2 cm) un-purged patch on the tower top, rotating across
4 positions per layer cycle. The next toolchange's ramming
pre-positions over the patch and rams *into* the layer-height-deep
cavity. The rammed material sits below the surrounding purge surface;
the next layer's nozzle passes cleanly above it.

Forum prior art: a "spill hole within the wipe tower" suggestion in
the Prusa forum *What is ramming?* thread. No PR was ever filed.

Prusa developer pushback on the related *relocate ramming into model
infill* proposal:

> Ramming is time-critical and wouldn't work in infill. Top layers
> have no infill. And the infill often has less volume than required
> anyway.

Each of these applies to ram-into-infill, not to pit-in-tower:

| Dev objection | Why pit-in-tower is unaffected |
|---|---|
| Time-critical | Ramming curve, retract and cooling sequence run untouched. Only the X/Y of the existing pre-ramming `writer.travel(...)` changes. |
| Top layer has no infill | The pit lives on the tower, which prints at every model layer. Last-layer pit is harmless. |
| Infill volume insufficient | A 2×2 cm pit at 0.2 mm layer height holds 80 mm³ — more than one PLA ram (~25 mm³). |

Volume math (PLA defaults, 0.2 mm layer):

- Typical ram: ~25 mm³ → ~167 mm of ramming line at 0.75 mm width.
- 2×2 cm pit at 0.2 mm = 80 mm³ capacity → fits one ram easily.
- X-sweep shrinks from full tower width to ~20 mm → more turnarounds
  in the same Y span. Validate that turnarounds don't pile material
  at the pit edges.

Prototype sketch (`WipeTower.cpp`):

1. Add advanced bool `wipe_tower_ram_into_pit` (default off).
2. Add per-layer offset cycle (e.g. 4 quadrants of the tower top).
3. In `plan_tower()` / `toolchange_Wipe`: skip a ~2×2 cm patch from
   the previous-layer purge fill at the upcoming layer's pit
   position so it is layer-height-deep when ramming arrives.
4. Just after the FINDA wait at `:959–960`, before the ramming loop
   at `:964`, emit `writer.travel(...)` to the pit position.
5. Leave the ramming loop, retract, cooling, stamping unchanged.

Effort: ~100 lines, all in `WipeTower.cpp`. Targets Source A
directly; orthogonal to Sources B and the settings combo above.

### Why no Option C

An earlier draft proposed an adaptive "geometric purge-flow budget"
that would cap effective flow at per-layer geometric capacity. Its
premise — that the slicer over-extrudes by ~2.27× because flow and
spacing are decoupled — was disproven by code verification (see
*Code-verified wipe-tower geometry* above). The current slicer
already reserves 12 % more volume than the wipe deposits at defaults.
Option C is removed.

## Open questions

- Does the symptom depend on the *last* purge of the layer
  specifically? Determines whether Option A's single ironing pass is
  enough or every toolchange needs it.
- Is behaviour different between SEMM and true multi-tool? Temperature
  handling in `tool_change()` is gated on `m_semm`; ramming/purge is
  shared.
- Does ironing interact badly with `wipe_tower_no_sparse_layers`?
- **HF profile `multimaterial_purging` 240 → 80 mm³ — does colour
  bleed return and does the tower drift stop?** Single most
  informative untested experiment.
- Does the settings combo (moderate or aggressive) alone suffice for
  HF on PETG, or only on PLA?

## Next step

1. **Settings test on a known repro print.** Apply the moderate combo
   first (`extra_flow = 150 %`, `extra_spacing = 180 %`, `bridging
   = 4 mm`). Compare tower-top photos at layers 10 / 50 / 100. No
   code change.
2. **Brass-volume test.** Separately, drop HF profile
   `multimaterial_purging` 240 → 80 mm³; check both tower drift and
   colour bleed.
3. **Option A prototype**, only if 1 + 2 leave residual blob issues.
   Implement `wipe_tower_iron_top` behind a default-off flag, test on
   a ≥ 4-colour PETG print.
4. **Option B prototype**, only if A is insufficient. Rolling pit is
   ~100 lines; not warranted unless A fails.

## References

- [PrusaSlicer #14784 — Wipe tower blob crash mitigation](https://github.com/prusa3d/PrusaSlicer/issues/14784)
- [PrusaSlicer #13897 — Bridged purge extrusions don't anchor at 10 mm `wipe_tower_bridging`](https://github.com/prusa3d/PrusaSlicer/issues/13897)
- [PrusaSlicer commit `ba37505` (2024-01-31) — Wipe tower depth and overlap fix](https://github.com/prusa3d/PrusaSlicer/commit/ba37505)
- [Prusa forum — High flow nozzle with MMU3](https://forum.prusa3d.com/forum/original-prusa-i3-mmu3-hardware-firmware-and-software-help/high-flow-nozzle-with-mmu3/)
- [Prusa KB — Prusa nozzle vs CHT nozzle](https://help.prusa3d.com/article/mmu3-prusa-nozzle-vs-cht-nozzle_779168)
- [Prusa KB — Wipe tower](https://help.prusa3d.com/article/wipe-tower_125010)
- [Prusa KB — Purging volumes (MMU)](https://help.prusa3d.com/article/purging-volumes-mmu_125097)
- [Prusa forum (DE) — MMU3 und Prusa Nozzle High Flow Obxidian 0.4 mm](https://forum.prusa3d.com/forum/hilfe-zur-hardware-firmware-und-software-3/mmu3-und-e3d-prusa-nozzle-high-flow-obxidian-0-4-mm/)
- [Prusa forum — Blobs on purge Tower (MMU3)](https://forum.prusa3d.com/forum/original-prusa-i3-mmu3-general-discussion-announcements-and-releases/blobs-on-purge-tower/)
- [Prusa forum — MMU3/MK4 Wipe tower issues (too much purged)](https://forum.prusa3d.com/forum/original-prusa-i3-mmu3-hardware-firmware-and-software-help/mmu3-mk4-wipe-tower-issuestoo-much-purged/)
- [Prusa forum — What is ramming?](https://forum.prusa3d.com/forum/original-prusa-i3-mmu2s-mmu2-general-discussion-announcements-and-releases/what-is-ramming/)

## Appendix: revision history

- **2026-05-12.** Rewrite after user-confirmed empirical observation
  (brass 0.4 MMU3 works, HF 0.4 MMU3 fails on the same printer,
  filament and temperature) and code re-verification. Prior draft's
  central thesis — that the slicer over-extrudes the tower by ~2.27×
  because `wipe_tower_extra_flow` and `wipe_tower_extra_spacing` are
  decoupled on non-first layers — was disproven by inspection of
  `WipeTower.cpp:551`. The decoupling was a real bug pre-2024 and was
  fixed in upstream commit `ba37505` (2024-01-31). The current code
  multiplies the two and the default profile produces a ~9 % gap fill,
  not a 2.27× overlap. Root cause re-identified as the
  Prusa-acknowledged HF 4-channel exit tip mis-shaping (Source A),
  amplified by 3× wipe volume (Source B). Option C (geometric budget)
  removed; recommendation rebuilt around a settings combo plus
  Options A (ironing) and B (rolling pit).
