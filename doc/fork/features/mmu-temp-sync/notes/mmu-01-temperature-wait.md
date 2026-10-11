# MMU Issue 1 — New filament loaded before nozzle reaches new target temperature

Status: research complete (2026-05-10). **Implemented** since `v2.9.6+ext-v1.0.0` on
`feature/mmu-temp-sync` (see ../README.md); the text below is the original research.

This document explains the bug, surveys every workaround known at the time
of writing, and answers the question *"is `mmuGcodeParser` the right
solution?"*. It deliberately stops short of proposing an implementation —
that belongs in a follow-up.

## Symptom

On a tool change between filaments with different print temperatures, the
slicer does **not** synchronize the nozzle to the new target before the new
filament is loaded and purged.

- **Cool → hot** (e.g. PLA 200 °C → PETG 240 °C, PC 270 °C): new filament
  is fed into a still-too-cold nozzle. Result: under-extrusion at the
  start of the next object, audible extruder clicking, jammed heatbreak.
  Canonical failure case in MMU3 reports.
- **Hot → cool** (e.g. PETG 250 °C → PLA 215 °C): load, purge and the
  first object moves happen at the previous (too-hot) temperature.
  Result: oozing, stringing, poor surface finish until the nozzle
  catches up — typically several centimetres into the next object.

## Real problem (root cause)

The bug is **not** "the slicer forgets to wait". The slicer *does* emit a
temperature command — it emits exactly one, in the wrong place, with the
wait flag hard-coded off:

`src/libslic3r/GCode/WipeTower.cpp:1002`
```cpp
writer.set_extruder_temp(new_temperature, false);
```

This single `M104 S<new>` is written **mid-unload**, between ramming and
the cooling moves, and is the *only* temperature command for the whole
toolchange. After it, the slicer:

1. Finishes the cooling moves of the *old* filament.
2. Emits the `T<n>` tool-select.
3. Loads the new filament.
4. Purges on the wipe tower.
5. Resumes the model.

Steps 3–5 all run at whatever temperature the nozzle happens to be —
which, for any non-trivial Δ, is somewhere between the two targets and
not the right one for either filament. The wipe tower exists *to absorb*
the transition, but the slicer never tells the firmware to wait *at* the
tower; it tells it to start changing temperature halfway through the
unload and then never synchronizes again.

There is a second instance of the same call at `WipeTower.cpp:1047` for
the cold-ramming branch — same problem.

The underlying writer API already supports both M104 and M109:

`src/libslic3r/GCode/WipeTower.cpp:354`
```cpp
WipeTowerWriter& set_extruder_temp(int temperature, bool wait = false)
{
    m_gcode += "G4 S0\n"; // flush planner queue
    m_gcode += "M" + std::to_string(wait ? 109 : 104) + " S"
             + std::to_string(temperature) + "\n";
    ...
}
```

So this is a **placement and wait-flag** bug, not missing infrastructure.

## Where in the slicer

Phase markers in `tool_change()` (the same markers `mmuGcodeParser` and
PR #9737 hook on to):

| File:line | Marker / call | Phase |
|-----------|---------------|-------|
| `WipeTower.cpp:824` | `; CP TOOLCHANGE START` | start of toolchange |
| `WipeTower.cpp:891` | `; CP TOOLCHANGE UNLOAD` | ramming + cooling begin |
| **`WipeTower.cpp:1002`** | `set_extruder_temp(new, false)` | **bug site 1** — M104, no wait |
| **`WipeTower.cpp:1047`** | same, cold-ramming branch | **bug site 2** |
| `WipeTower.cpp:847` | `toolchange_Change()` → `T<n>` | tool select |
| `WipeTower.cpp:1115` / L1128 | `toolchange_Load()` / `; CP TOOLCHANGE LOAD` | load new filament |
| `WipeTower.cpp:1144` / L1151 | `toolchange_Wipe()` / `; CP TOOLCHANGE WIPE` | purge on tower |
| `WipeTower.cpp:863` | `; CP TOOLCHANGE END` | hand back to print |

## Versions checked (2026-05-10)

| Component | Latest | Date | Bug fixed? |
|-----------|--------|------|------------|
| PrusaSlicer (stable) | **2.9.4** | 2025-11-07 | No |
| PrusaSlicer (next) | 3.0 pre-release (Spring 2026) | — | No (no PR merged) |
| This fork | 2.9.5-beta2 (internal) | — | No |
| Prusa-Firmware-Buddy | **6.5.3** | 2026-03-24 | No (focused on MMU *speed*, not temp timing) |
| MMU3 firmware | 3.0.4 (required by 6.5.3) | — | No |
| Prusa-Firmware (MK3 8-bit) | legacy | — | No |

The 6.5.x firmware line parallelized unload/idler/selector for ~9 s saved
per change but does nothing for temperature timing.

## Upstream status

All issues below are open on `prusa3d/PrusaSlicer` as of 2026-05-10.

| # | Title | Opened | Notes |
|---|-------|--------|-------|
| [#1339](https://github.com/prusa3d/PrusaSlicer/issues/1339) | Temperature-commands while changing filament with MMU | 2018-10 | Canonical issue; describes the M104+M109 sequencing this doc proposes. |
| [#1207](https://github.com/prusa3d/PrusaSlicer/issues/1207) | MMU – Change Temperature/Material Purge Order | 2018 | #8201 and #8702 closed as duplicates. |
| [#1300](https://github.com/prusa3d/PrusaSlicer/issues/1300) | Ramming temperature different from print temperature | 2018 | Cold-ramming feature partly addresses; load side still wrong. |
| [#2385](https://github.com/prusa3d/PrusaSlicer/issues/2385) | Integrate Skinnydip into PrusaSlicer | 2019 | By the skinnydip author. |
| [#10254](https://github.com/prusa3d/PrusaSlicer/issues/10254) | Add Ramming temperature to filament settings | 2023 | 0.8 mm nozzle MMU jam case. |
| [#12289](https://github.com/prusa3d/PrusaSlicer/issues/12289) | Inconsistent M104/M109 issuing | 2024 | Most recent restatement. |

**[PR #9737](https://github.com/prusa3d/PrusaSlicer/pull/9737)** by
`amatulic` (Alex Matulich) fixes #1207 by editing `WipeTower.cpp`.
**Open and unmerged since 2023-02-16; last rebased 2026-04-23; 32 commits
(mostly merge-master rebases); +32 / −9 lines across `WipeTower.cpp` +
`WipeTower.hpp`.** Zero Prusa-staff comments in 94 community comments.
Author has rebased every few months and says he will close and reopen
against 2.9.6 / 3.0 once Prusa engages with outside PRs. Mergeability:
`MERGEABLE` but `BLOCKED` (no approval, no required CI).

The PR's strategy matches `mmuGcodeParser` on **cool→hot** (M104 inside
`toolchange_Unload`, before `T<n>`; blocking M109 at the very start of
the wipe) but **deliberately diverges on hot→cool** as a result of
three years of community testing — see "Field-tested deviation" below.
It is always-on, gated only on `m_semm`; the author has explicitly
refused to add a config flag.

## Solution survey

| Solution | Type | Link | Last activity | Approach | Verdict |
|----------|------|------|---------------|----------|---------|
| `mmuGcodeParser` | Python post-proc | [github](https://github.com/workinghard/mmuGcodeParser) | 2019-04 (Win binary); logic stable | Comments out slicer M104; re-inserts M104 before T (cool→hot) and M109 at purge start; mirror for hot→cool with M109 before TOOLCHANGE END. | Reference implementation. Correct semantics, dated. |
| `johnnyruz/PrusaScripts` (`MMU_Temp_Fix`) | Python post-proc | [github](https://github.com/johnnyruz/PrusaScripts) | maintained through 2024 | Same idea, simpler; places temp commands after cooling moves. | **Recommended stopgap** — more recently maintained than mmuGcodeParser. |
| `cjbaar/prusa-slicer-post-processing` | Python post-proc | [github](https://github.com/cjbaar/prusa-slicer-post-processing) | sporadic | Wipe-tower cleanup; not focused on temperature. | Skip. |
| `skinnydip` | Perl post-proc | [github](https://github.com/domesticatedviking/skinnydip) | author marks superseded | Toolchange temp + post-cool dip; author redirects users to SuperSlicer's built-in. | Historical. |
| `domesticatedviking/PrusaSlicer` fork | Slicer fork | [github](https://github.com/domesticatedviking/PrusaSlicer) | years stale | Native skinnydip + per-filament toolchange temp. | Don't use — unmaintained. |
| Community "CE" PrusaSlicer fork | Slicer fork | (Prusa forum thread) | inactive | Adds per-filament "Change Tool temperature". | Skip — inactive. |
| `mkudzia84/toolchanger-pspp` | Python post-proc | [github](https://github.com/mkudzia84/toolchanger-pspp) | active | IDEX/Duet, not MMU. | Not applicable. |
| Built-in G-code Substitutions | regex find/replace | help.prusa3d.com | built-in | Slicer-side regex on output. | Too coarse for asymmetric cool/hot logic. |
| Custom "Tool change G-code" textbox | manual | n/a | n/a | Append `M109 S[nozzle_temperature]` in the printer profile. | Side effect: stalls motion *on* the wipe tower; ooze/collapse risk (Orca explicitly documents this). |
| **PrusaSlicer PR #9737** | upstream PR | [github](https://github.com/prusa3d/PrusaSlicer/pull/9737) | rebased 2026-04-23, no approval | In-slicer fix in `WipeTower.cpp` (+32/−9). Cool→hot matches parser; hot→cool emits M104 mid-wipe (30 % into band), **no terminal M109**. Always-on, `m_semm`-gated. | **Reference for an in-slicer fix — but note its hot→cool tail differs from the parser.** |

## How other slicers handle it

**SuperSlicer** ships skinnydip natively (Filament Settings → Advanced:
toolchange temperature, dip distance). Per-filament toolchange-temp
plumbing already exists in the same Slic3r-derived codebase, which makes
it useful as a code reference for an in-slicer fix in PrusaSlicer.
Recurring bugs ([#195](https://github.com/supermerill/SuperSlicer/issues/195),
[#122](https://github.com/supermerill/SuperSlicer/issues/122)) report
that the *restored* post-toolchange print temp picks the wrong filament;
upstream activity slowed in 2024–25.

**OrcaSlicer** explicitly chose a different strategy: **non-blocking
early preheat**. The 2.3.2 cycle moved preheat G-code outside the
toolchange block and injects boost-preheat as early as possible after the
*previous* tool switch, *without* M109, to avoid stalling on the prime
tower. Per-filament `filament_tower_interface_print_temp` exists. Issues
[#4337](https://github.com/OrcaSlicer/OrcaSlicer/issues/4337) and
[#2334](https://github.com/SoftFever/OrcaSlicer/issues/2334) discuss
remaining gaps. This is the strongest argument for *not* simply copying
the M109 approach — early preheat avoids the wipe-tower stall entirely
but is harder to time when the previous tool's last extrusion is short.

**Bambu Studio / AMS** uses a hardware-side approach: flush at a fixed
~250 °C regardless of filament, then apply per-filament target with a
*firmware-side* M109 wait before resuming the model. Bambu recommends
keeping paired materials within ~40 °C. Architecturally different from
PrusaSlicer's wipe-tower model, but confirms "wait at end of toolchange"
is industry practice.

## Cold-ramming overlap

PrusaSlicer's `filament_enable_toolchange_temp` /
`filament_toolchange_temp` ("cold ramming") drops the nozzle a few
degrees during ramming so the filament tip forms cleanly. It addresses a
*tip-quality / unload* problem and uses M104 (no wait) in the same
pattern as the bug. It does **not** target the new filament's print
temperature for load/purge. Field reports (#1300, #10254, MMU3 Tip
Tuning forum threads) treat cold ramming as **complementary to**
`mmuGcodeParser`, not a replacement.

## Is `mmuGcodeParser` the right solution?

**As a stopgap: yes, with a caveat.** Its strategy (asymmetric M104/M109
placement based on transition direction) is correct and is the same
strategy PR #9737 implements in C++. It's the most-cited workaround in
the MMU community and works on the comment markers PrusaSlicer still
emits.

**Caveats**:
- Last meaningful update 2019 (workinghard/mmuGcodeParser); a more
  actively maintained alternative is **`johnnyruz/PrusaScripts
  MMU_Temp_Fix`**.
- Python-based post-processor: requires a Python install on the
  slicing host, won't run on Prusa Connect upload paths or
  direct-to-printer slicing scenarios.
- Brittle to slicer comment-marker drift. PrusaSlicer has been stable
  on these markers since pre-2018 (`; CP TOOLCHANGE WIPE` etc.), but
  there is no contract.
- Drops a fixed 10 °C during ramming regardless of filament; not
  configurable per-material.

**As a long-term answer: no.** The right fix is in
`src/libslic3r/GCode/WipeTower.cpp` itself — same semantics, no Python
dependency, runs everywhere PrusaSlicer's output is consumed, and the
writer already supports M109. PR #9737 is a working reference,
unmerged only for organizational reasons (author wants to redo on 3.0).

**Practical recommendation for this fork (2.9.5-beta2-wayland)**:
1. Use `johnnyruz/PrusaScripts MMU_Temp_Fix` (or `mmuGcodeParser`) as a
   post-processing script today, configured in Print Settings → Output
   options → Post-processing scripts. Zero code change required.
2. Plan an in-slicer fix as a separate branch off the Wayland branch,
   using PR #9737 as the reference and `mmuGcodeParser`'s state machine
   as the spec. Implementation details are out of scope for this
   document.

## Recommended approach: hybrid (early non-blocking M104 + blocking M109 at the right barrier)

A natural design question is whether to follow PrusaSlicer/SuperSlicer
"M109 wait" semantics or OrcaSlicer "non-blocking early preheat"
semantics. The answer is **both, combined** — and on closer reading
this is exactly what `mmuGcodeParser` already does. The hybrid is the
right design; it just hasn't been ported into the slicer.

### Underlying principle

Each phase of the toolchange has a physically correct temperature.
Issue commands so that *each phase runs at its right temperature*, and
let the thermal transition overlap with phases where it does no harm.
Use M109 only as a final barrier before a phase where wrong temperature
would cause visible damage.

| Direction | Ramming/unload should be at… | Load should be at… | Purge should be at… | Model resume should be at… |
|-----------|------------------------------|-------------------|---------------------|----------------------------|
| Cool → hot | Old (cool) — clean tip | New (or ramping toward new) — smooth feed | **New** — clean purge | New — already there |
| Hot → cool | Old (hot) — clean melt-through | Old (hot) — smooth feed | New (or ramping toward new) — cooling overlap | **New** — already there |

The bold entries identify where an M109 belongs: the first phase whose
correctness *depends* on the new target being reached.

### Concrete sequence (matches `mmuGcodeParser` semantics)

**Cool → hot** (e.g. PLA 200 °C → PETG 240 °C)
1. At start of unload: optional small Δ drop (`M104 S<old−Δ>`) for a
   cleaner tip — reuse `filament_enable_toolchange_temp` rather than a
   new constant.
2. **Just before `T<n>`** (`WipeTower.cpp:847`): `M104 S<new>` —
   non-blocking. Heating ramps during cooling-moves tail, tool select,
   load, and start of wipe.
3. **Just after `; CP TOOLCHANGE WIPE`** (`WipeTower.cpp:1151`):
   `M109 S<new>` — blocking. Guarantees purge happens at correct temp.
4. At `; CP TOOLCHANGE END`: nothing — already stable.

**Hot → cool** (e.g. PETG 250 °C → PLA 215 °C)
1. At start of unload: optional small Δ drop as above.
2. Just before `T<n>`: restore old temp if a Δ drop was applied.
3. **Just after `; CP TOOLCHANGE WIPE`** (`WipeTower.cpp:1151`):
   `M104 S<new>` — non-blocking. Nozzle cools during the entire purge.
4. **Just before `; CP TOOLCHANGE END`** (`WipeTower.cpp:863`):
   `M109 S<new>` — blocking. Guarantees model resumes at correct temp.

**No change**: emit nothing — the slicer's existing skip at
`WipeTower.cpp:996` already handles this.

In all cases the slicer's current mid-unload `M104` at L1002 / L1047 is
replaced rather than supplemented.

### Why this dominates either pure approach

- **vs. pure M109-wait** (a single blocking command in the wrong place):
  the early M104 means the M109 typically resolves in milliseconds
  because most of the Δ has already been absorbed during overlapping
  mechanical work. The wait only stalls on extreme Δ (e.g. PLA→PC,
  70 °C cool→hot).
- **vs. pure non-blocking preheat** (no M109 at all): correctness no
  longer depends on hoping the purge volume + cooling moves are enough.
  Worst-case behavior is "slower" instead of "jammed" or "stringy".
- **vs. mmuGcodeParser as a post-processor**: same semantics, but
  in-slicer, with no Python dependency, runs on every output path
  (including Prusa Connect / direct-to-printer), not brittle to
  comment-marker drift, and the Δ drop reuses the existing cold-ramming
  config instead of a hard-coded 10 °C.

### Field-tested deviation on hot→cool (from PR #9737)

The strategy table above describes what *should* work in theory and
matches `mmuGcodeParser` exactly. PR #9737 started life implementing
that exact strategy, then iterated for three years against real prints.
The current state of the PR diverges from the parser on the hot→cool
path for two empirical reasons:

1. **Fast-cooling hotends jammed.** Setting M104 to the lower target at
   the *start* of the wipe caused the nozzle to drop below the
   filament's melt point before the wipe was finished on
   fast-cooling assemblies (Voron CHC Pro, Dragon, K1 UHF). PR author
   moved the M104 emission to mid-wipe — specifically
   `y_temp_change = 0.30f * (cleaning_box.lu.y() - writer.y()) + writer.y();`
   which is ~30 % into the *remaining* wipe band, ≈40–50 % of the
   tower top surface visually.
2. **Soluble-support ooze.** With a terminal M109 before
   `; CP TOOLCHANGE END`, slow-cooling stock hotends sat over the
   tower for many seconds waiting for the temperature to drop, and
   BVOH (195 °C support after PLA at 205 °C) oozed a "gigantic blob"
   onto the tower top. The author **removed the terminal M109
   entirely** in commit `266b48df` (2024-06-04).

The result: hot→cool transitions are non-blocking in PR #9737. They
rely on the model's first moves being far enough from the tool change
that the temperature has settled by the time it matters, and on the
mid-wipe M104 starting cool-down early enough that the remaining wipe
finishes the transition.

**Implications for the design choice:**

- If portability across hotend types matters more than strict
  correctness, copy PR #9737's hot→cool exactly: mid-wipe M104, no
  terminal M109.
- If correctness matters more (especially on big Δ down, e.g. PC →
  PLA, 55 °C), use the parser's strategy: M104 at start of wipe,
  M109 before model resume — and accept the ooze risk on slow-cool
  hotends, or mitigate with a parking move during the wait.
- A **gated** implementation could expose this as a printer- or
  filament-level choice. The PR author rejected gating as too
  complex; that judgement is debatable.

The cool→hot path has no equivalent field-tested deviation — both PR
#9737 and `mmuGcodeParser` converged on the same placement (M104 before
`T<n>`, blocking M109 at start of wipe) and no regressions have been
reported on that path.

### Optional further refinement (Orca-style)

OrcaSlicer goes one step beyond what `mmuGcodeParser` does: it emits a
*preheat boost* during the **previous** filament's last print moves,
before the current toolchange even starts. This shaves additional
seconds off the cool→hot case at the cost of running the previous
filament's tail slightly off-target (Orca exposes
`filament_tower_interface_print_temp` to control this).

This is a **separable enhancement**: it requires UI plumbing (per-pair
or per-filament transition temperature), the planning layer needs to
know the next tool's target before the current toolchange is generated
(already available — `m_plan` in `WipeTower.cpp` carries the full
schedule), and the user must accept some quality trade-off on the
previous filament's last layer. Recommended path: ship the
parser-equivalent hybrid first; consider Orca-style pre-shift as a
follow-up.

## Open questions

- **PR #9737 forward-port**: GitHub reports `MERGEABLE` against current
  master as of the 2026-04-23 rebase; mechanically clean. One subtle
  latent risk: the PR moves the `m_old_temperature` assignment outside
  the conditional so it now stores *the current tool's nominal config
  temperature* on every entry, instead of *the temperature most recently
  written to G-code*. For the MK4-MMU3 cold-ramming path this means
  comparisons against `m_old_temperature` now reference the nominal
  temperature, never the −20 °C ramming temperature. Test against an
  MK4-MMU3 cold-ramming profile before adopting.
- **Hot→cool strategy choice**: parser-style (M104 at start of wipe +
  terminal M109) vs PR #9737-style (M104 mid-wipe, no terminal M109).
  See "Field-tested deviation" section. This is the single biggest
  design question for the in-slicer port.
- **Cold-ramming interaction**: with `filament_enable_toolchange_temp`
  enabled, the Δ-drop in step 1 should reuse the existing knob rather
  than introducing a new one. Read the cold-ramming branch at
  `WipeTower.cpp:1045–1049` (`change_temp_later` flag) when wiring this
  up — that branch already delays the M104 emission and must be
  reconciled with the new placement rules.
- **Last-toolchange branch** (`WipeTower.cpp:853`): the orchestrator
  calls `toolchange_Unload` only, with no `Change`/`Load`/`Wipe`.
  Likely needs no temperature treatment (no new filament loaded), but
  confirm by reading the call site.
- **Δ for the M109 wait**: on extreme cool→hot (PLA → PC, 70 °C+) the
  M109 at start-of-purge will block for several seconds with the new
  filament parked in the cooling tube. Verify ooze behavior in a real
  print before accepting; if problematic, consider parking the head off
  the tower during the wait.
- **Multi-tool (non-SEMM) printers**: the existing M104 emission at
  L1002 is gated on `m_semm`. Any new emissions should be gated the
  same way — multi-tool printers handle their own temperature
  scheduling per extruder.

## Sources

- PrusaSlicer issues: [#1339](https://github.com/prusa3d/PrusaSlicer/issues/1339), [#1207](https://github.com/prusa3d/PrusaSlicer/issues/1207), [#1300](https://github.com/prusa3d/PrusaSlicer/issues/1300), [#2385](https://github.com/prusa3d/PrusaSlicer/issues/2385), [#10254](https://github.com/prusa3d/PrusaSlicer/issues/10254), [#12289](https://github.com/prusa3d/PrusaSlicer/issues/12289)
- PrusaSlicer PR: [#9737](https://github.com/prusa3d/PrusaSlicer/pull/9737)
- Post-processors: [mmuGcodeParser](https://github.com/workinghard/mmuGcodeParser), [johnnyruz/PrusaScripts](https://github.com/johnnyruz/PrusaScripts), [skinnydip](https://github.com/domesticatedviking/skinnydip)
- Other slicers: [SuperSlicer #195](https://github.com/supermerill/SuperSlicer/issues/195), [SuperSlicer #122](https://github.com/supermerill/SuperSlicer/issues/122), [OrcaSlicer #4337](https://github.com/OrcaSlicer/OrcaSlicer/issues/4337)
- Releases: [PrusaSlicer releases](https://github.com/prusa3d/PrusaSlicer/releases), [Buddy 6.5.3](https://github.com/prusa3d/Prusa-Firmware-Buddy/releases/tag/v6.5.3)
