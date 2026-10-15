# Plan review 3 — GUI, user experience, scope, work packages

Reviewer #3, 2026-10-15, on `notes/plan.md` (draft 2026-10-15). Code checked against the
feature branch (= `version_2.9.6`).

## Verdict

The analysis scope is the right "one usable capability". The GUI as drawn has four problems:
- it does not fit the gizmo window on common screens;
- it uses theory jargon;
- two of its semantics surprise the user: unchecked causes keep stale paint, and α can no longer
  be edited once auto supports are off.

§8 cannot run in parallel as written: builds go through the shared branch, the GUI seam is
under-specified, and four WPs are too big. Fix B1 and M1–M7 in the plan before WP0. None of
them needs a theory change.

## Blocker

**B1 — §6.1 panel height / placement.**
- The existing window is ≈ 26 em (`approx_height` 26.3; 1 em = 1.5 · em_unit = 15 px at
  100 %). The section as drawn adds ≈ 20 rows, up to 6 contract lines and a 10-row table,
  which is ≈ 45–50 em more. The expanded window is then ≈ 1100 px tall; a 1080p canvas is
  ≈ 850–900 px. Even without the table it is ≈ 900 px.
- The window is `AlwaysAutoResize | NoMove` with no maximum size. `y = min(y, bottom_limit − h)`
  becomes negative, so the top of the window (captions, "Automatic painting") leaves the
  canvas, and nothing scrolls.
- Inserting the section after the autopaint separator also pushes the brush controls (the
  main use of this gizmo) down.

Fix:
1. Call `ImGui::SetNextWindowSizeConstraints({0,0}, {-1, bottom_limit − top_margin})` on the
   gizmo window, as in `GCodeViewer.cpp:2069-2072`, so it scrolls instead of overflowing.
2. Move the section to the end of the window (after "Remove all selection"). For
   discoverability, put only a one-line "Paint where needed ▸" next to "Automatic painting".
3. Move the report into its own ImGui window ("Smart painting report", own size limit,
   ScrollY), opened from the summary line.
4. Keep the main section to ≤ ~12 rows. `H_s`, `B_ceil` and β go to "Advanced" (collapsed).

GUI acceptance: the panel can be fully reached on 1366×768 and 1920×1080 at 100 % and 150 %
scaling.

## Major

**M1 — §6.1 "Paint from level: should".**
- may/should/must are theory words.
- In v1, *may* needs exist only for tension, i.e. long spans (theory §2.5; bending has no
  *may*). Long spans are never painted (D7), so "may" paints exactly what "should" paints.

Fix: a two-item combo, "Support: Critical only / Critical and likely (recommended)" (= must /
should), with plain tooltips. Keep `Level::May` internal.

**M2 — §4 "re-run replaces own cause only" vs the §6.1 checkboxes.**
- Unchecking "Cantilevers" and pressing Paint leaves the previous run's cantilever paint on
  the model. The checkbox reads as "the result contains this".

Fix:
- A run replaces **all** auto states with the checked causes (clear all bits before painting).
  P3 still holds: any combination still equals the union of the single runs.
- Replace the three clear buttons with one "Remove smart paint" (own paint kept).

**M3 — §6.1 "Overhang threshold: 45° (print settings)", §3.4 α.**
- The contract requires `support_material_auto` off.
- With auto off, the Print tab greys out `support_material_threshold`
  (`ConfigManipulation.cpp:362`), and organic support no longer uses it: `TreeSupport.cpp:256`
  uses it only with auto or enforced layers, and `TreeSupportSettings::support_angle` is never
  read.
- So after "Apply recommended" the user can no longer change the angle that drives the
  analysis, and the setting does nothing else.

Fix:
- Make α a panel parameter, "Overhang angle". Default: the config value; 0 is shown as
  "auto".
- Store it in AppConfig, `UserParams` and the fingerprint. Drop the read-only line.

**M4 — §3.10 / §6.1 report.**
- 7 columns do not fit the panel width.
- Bed X/Y and layer ranges do not help anyone find a spot on a figurine.
- A cap of 500 rows is not usable.

Fix:
- **Group by state, in two tiers.**
  - *Problems*, expanded: Unplaceable, BlockedByUser (islands), WillBeDropped,
    RemainingAfterPaint, OnModelFallback / MayMergeOntoModel / OnModelTooShort.
  - *Info*, collapsed with counts: BelowLowestLevel, LongSpanNotPainted, CauseDisabled,
    ReliesOnDisabledCauses, TailWithoutDownwardFacets, SeveralPrintObjects,
    OtherObjectsNotConsidered.
- **Rows:** level colour, short text, Z; layers, X/Y, area and "what you can do" in the
  tooltip. No ContractMismatch rows (already shown above the button). Cap ~50 per group.
- **Marker required in v1**: it is the only way to locate an entry. Cheap extra: on click, set
  the clipping plane to the entry's Z (`ObjectClipper::set_range_and_pos`).

**M5 — §6.6 / §8: the seam between WP3a and WP3b.**
- `SmartSupportsHost` covers only model, selectors, commit and redraw.
- The panel also needs:
  - the call site and the width in `on_render_input_window`;
  - marker drawing in `render_painter_gizmo`;
  - `data_changed`, `on_shutdown` and selection-change hooks;
  - a busy flag for `gizmo_event`;
  - the colour mode for `TriangleSelectorFdmGui`;
  - the window-size rule (B1).
- Neither WP can be tested in the sandbox.

Fix: merge WP3a and WP3b into one GUI WP (≈ 1200 lines). Otherwise freeze a panel API in WP0:
`render_imgui(float width)`, `render_3d(const Selection&)`, `on_data_changed()`,
`on_shutdown()`, `bool busy() const`.

**M6 — §8 build and commit rules.**
- `build.sh sync` merges the **committed** head of `feature/smart-auto-supports` into
  `/home/nhp/sas-build`. An agent can compile only after committing, and every build contains
  every other agent's latest commit.
- Seven agents committing to one branch from one checkout: `index.lock` races, mixed commits,
  and one broken commit blocks all builds. "Every commit keeps the tree compiling" cannot be
  checked before the commit.

Fix:
- One worktree and branch per WP (`wip/sas-<wp>`).
- A build.sh variant that, under the flock, resets the sandbox worktree to
  `dev/2.9.6 + feature + wip/<wp>` and builds.
- The orchestrator merges a WP into the feature branch only when it is green.
- Batch the post-WP0 header changes: `Model.hpp` and `TriangleSelector.hpp` force near-full
  rebuilds.

**M7 — §8 WP sizes, coupling, order.**
- **Too big for one agent session:**
  - WP1: TriangleSelector rule and cursor, Model, slicer, 3MF, `SmartSupportsPaint`, 3 test
    files.
  - WP2a: input, Phase A, Phase A2, reachability.
  - WP2b: raster, ReachField, carry, tension/ties/rows/chord rule, hoop, classification.
- **WP2c:** cannot run anything until WP1, WP2a and WP2b land, yet it holds the hardest
  debugging (all §7.2 cases).
- **WP5:** "fixes routed to owners" routes them to sessions that have already ended.
- **Hidden coupling:**
  - `build_reachability(…, const BaseModel&, …)` contradicts "concurrent with Phase A" (§2.3,
    §3.7). Make it take `AnalysisInput` only.
  - WP2a's manual-paint inputs and reference case #19 need WP1's `project_states`.

Proposed schedule:
- **Phase 0:** WP0, including the panel API and the per-WP build script.
- **Phase 1, in parallel:**
  - WP1a: TriangleSelector, Model, PrintObject, Print, 3MF.
  - WP2a: base model, Phase A2.
  - WP2r: reachability.
  - WP2b1: raster, ReachField, carry (H1 tests).
  - WP2b2: tension, hoop, `classify_front`.
  - WP2c0: contract, `apply_recommended`, fingerprint, `build_report` on synthetic plans.
  - WP3: the GUI against a fake `Result` (debug switch that paints a fixed placement list), so
    the user can smoke-test the workflow early.
  - WP4: docs.
- **Phase 2:**
  - WP1b: `SmartSupportsPaint` (needs PredicateCursor).
  - WP2c1: evaluator, plan loop, verify, `run()`.
- **Phase 3:** WP5 with **one** owner for the whole analysis (calibration, performance), then
  the host test.

## Minor

- **m1 — naming (§6.1).** "Smart painting" sits next to the existing "Smart fill".
  - Header: "Paint where needed (geometric)"; its tooltip contrasts it with "Automatic
    painting".
  - Use PrusaSlicer wording: "Prefer build plate" / "Build plate only", and "Max" without a
    period.
  - In the summary, "placements" → "areas".
  - "Stiffening height" → "Reach fade height"; "Max. cantilever reach" → "Max overhang
    reach".
- **m2 — long spans checkbox.** Remove the disabled "Long spans (report only)" checkbox; the
  report entries are enough.
- **m3 — colours (§6.4).** The "Show causes off" colour (0.60, 0.60, 1.00) is
  indistinguishable from manual enforcers (0.47, 0.47, 1.00), and violet against blue is weak
  for protan users. Drop the toggle and check the palette with a colour-blindness simulator.
- **m4 — busy state (§6.2).** Blocking only `gizmo_event` is not enough. During
  WaitingForSlices and Analysing, also disable:
  - "Remove all selection", the angle "Enforce" button and "Automatic painting";
  - "Remove smart paint" and "Apply recommended".
  Otherwise `finalize` discards the result.
- **m5 — snapshot type (§2.4 vs §6.3).** §2.4 says "apply settings" is a GizmoAction; §6.3
  uses the default type. Use `Action`: GizmoAction snapshots are collapsed when the gizmo is
  left (`Plater.cpp:4278-4284`), which hides the config change in the history.
- **m6 — D15 edge cases.**
  - **Background processing off:** `reslice_until_step` processes only the **first**
    PrintObject of the ModelObject (`PrintBase.hpp:640-681`). A selected instance with
    another rotation therefore never gets sliced and the run ends in Failed. Detect this and
    tell the user.
  - **Bed:** switch to the selected instance's bed, not instance 0's (the copy of
    `GLGizmoFdmSupports.cpp:538`).
  - **Non-printable selected instance:** disable the button and say why in the tooltip.
- **m7 — result visibility.** Paint lands only on downward facets, so the default camera shows
  nothing.
  - Summary: "Painted N areas (islands a, overhangs b) on downward faces — press 2 for the
    bottom view".
  - Empty result: "Nothing needs support with these settings".
- **m8 — per-frame cost (§2.4, D20).** `full_config()`, the fingerprint and the contract run
  on every ImGui frame, and frames are continuous while painting. Recompute them on
  preset/object-config change, or at most every 0.5 s.
- **m9 — no report after reopening (D14).** After a 3MF is reopened, show "Smart paint
  present — run again for the report" instead of an empty area.
- **m10 — brush over smart paint (D8).** Painting over smart paint turns it into manual paint
  for good. Say so in the Paint tooltip, next to the blocker hint.
- **m11 — defaults.** The placeholder defaults persist in AppConfig. Add "Reset to defaults".
- **m12 — translations.** About 40 new strings and no .po updates, so localized UIs show
  English. Use `format` placeholders, not concatenation.
- **m13 — "Apply recommended" (D16).** The tooltip lists the exact keys and values it sets.
  When issues exist, Paint offers "Apply and paint / Paint anyway / Cancel", once per object.
- **m14 — verification runtime (D13).** Verification doubles the run time. Measure it in WP5;
  if the benchmark takes more than ~10 s, make it an Advanced option.

Scope:
- **Cut:** the "Show causes" toggle, per-cause clear, the dead long-span checkbox, the
  7-column table.
- **Add:** the marker (M4), editable α (M3), result and empty-state messages (m7), "Reset to
  defaults", "Apply and paint".
- **Not needed:** a separate preview-before-commit step. One undo step already serves as the
  preview.
- **Follow-up:** "Paint all objects", since users print several figurines per plate.

## Host checklist (§7.3) — missing checks

1. Background processing on **and** off. A validation error ends in Failed with the message.
2. 1366×768 and 1080p canvases at 100 % and 150 %: the whole panel can be reached and
   scrolling works (B1).
3. Object on bed 2. Switching objects, deleting the object, loading a project and quitting
   during analysis.
4. Multi-part object, modifiers, negative volume, enforcer/blocker modifiers; a scaled,
   mirrored or X/Y-rotated object.
5. Raft, variable layer height, first layer ≠ layer height.
6. Settings change after a run → stale label. "Apply recommended" → object list "Settings"
   item; deleting it brings the warnings back.
7. Existing "Automatic painting" after a smart run: dialog, all paint erased, panel stale.
8. Copy/paste, split, Cut gizmo, Simplify on smart-painted objects.
9. Preview: organic tips on the plate under the paint; on-model tips only where reported
   (sphere on bed, T-arm; D10).
10. Language switch and app restart: parameters persist; missing LastRun handled (m9).
11. A ~1M-triangle figurine: paint size, 3MF size, undo memory, FPS; brush frame rate with
    the panel open (m8).

## Recommendations for the open questions

- **D7 long spans:** keep report-only in v1 (rare on figurines, `B_max` uncalibrated). Remove
  the dead checkbox and give entries a marker. Painting the rows is the first follow-up.
- **D8 suppression:** accept blockers as the suppression tool. Add a one-line hint: "Paint a
  blocker to keep an area free; erased smart paint returns on the next run".
- **D9 default level:** *should* (safer while `K_max` is uncalibrated), but offer only two
  choices (M1). Revisit after test prints.
- **D10 clearance:** accept `extra_outset + 0.2 mm` for v1 and calibrate it with the
  sphere-on-bed and T-arm prints (checklist 9).
- **D15 instances:** accept analysing the selected instance. Show the warning in the panel
  before the run, and handle background processing off (m6).
- **D16 apply settings:** accept object-level overrides:
  - other objects that rely on auto support are not affected;
  - the overrides are visible in the object list and undoable;
  - the SLA gizmo density slider already writes `mo->config`.
  List the changes, use an `Action` snapshot, and offer "Apply and paint" (m13).
