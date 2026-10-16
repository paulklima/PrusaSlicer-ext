# Work package log — WP5 (integration owner)

Scope (plan §8.2 WP5): all of §7.2, calibration, benchmark vs §3.13, verification-as-option decision, full suites,
portability and assert build (§8.3), the GUI switch from the fake to `run()`, timings in `notes/`. WP5 owned every file
of the feature, including the formerly frozen headers (one header batch, below).

## Commits

| Commit | What |
|---|---|
| `fix: bands keep fronts thinner than 0.5 % of the margin` | `offset_keep_thin()`; Phase A bands; planner offsets; evaluator workaround removed; `[sas-geometry]` regression |
| `fix: project paint with vertices on slicing planes` | `project_states()` feeds `slice_mesh_slabs()` XY-rounded vertices; `[sas-slicer]` regression |
| `feat: shared is_problem(), Params::tree_angle_slow_deg` | header batch (`SmartSupports.hpp`), GUI uses the shared rule |
| `test: settle the disputed reference expectations #8, #9, #10` | tests check the settled expectations (below) |
| `feat: MayMergeOntoModel calibrated against the slicer` | slow angle, Δz ≤ 6 mm; hidden `[sas-merge-calibration]`; new reference case |
| `test: phase benchmark on real meshes and 1M triangles` | hidden `[sas-reference-bench]`, `notes/timings.md` |
| `test: robustness cases` | seven new cases (below) |
| `docs: WP5 ...` | this log, `timings.md`, README status / how to test, plan §3.9 / §7.2 marks |
| fix round (6 commits after `4583bac22d`) | review findings A, B, C — §9 |

## 1. Thin fronts (WP2c1a finding, fixed at the source)

- **Cause.** `ClipperUtils` sets `ShortestEdgeLength = 0.005·|delta|` (`ClipperUtils.cpp:321, 483`): `AddPath()` drops
  vertices closer than that to the last kept one, so a strip thinner than 0.5 % of the offset degenerates (fewer than
  three vertices) and its offset is empty. Fronts of slopes within ≈ 2° of α (13 µm at 40° with auto α, 7 µm at 44°
  with α 45) lost their cluster band.
- **Fix.** `offset_keep_thin(src, delta, join, limit)` (declared in `SmartSupportsInternal.hpp`, defined in
  `SmartSupportsBase.cpp`): two steps, 1 % of delta first. Strips survive down to 0.005 % of delta (0.14 µm at 2.8 mm).
  Used for `Cluster::band`, `band_prev` (and so the hoop `S_on`) and in the planner for every large offset of needs or
  placements: widen zone `N ⊕ K`, the NotReachable / NoProgress test `tried ⊕ K`, `level_near`, the blocker clearance,
  MayMergeOntoModel, the verification margin. With 13 µm needs the old widen zone was empty.
- **Clipper's own round-offset bound.** Debugging the band invariant showed that Clipper rounds the number of arc
  steps per join (`steps = max(Round(StepsPerRad·|a|), 1)`), so a join whose turn is ≈ 1.49 step angles is one chord:
  a round offset lies up to 2.25 × the arc tolerance inside the exact one (11 µm at the 5 µm band tolerance, also for
  single-step offsets). The band arc tolerance is now 2 µm; documented bound for `offset_keep_thin`:
  `src ⊕ (delta − 2.25·limit − 0.0008·delta) ⊂ result ⊂ src ⊕ delta` (tested).
- **Evaluator workaround removed** (`check_bands` / `band_fix` in `SmartSupportsEval.cpp`): Phase A now guarantees
  band ⊇ fronts, and the per-cluster area check on every layer is gone.
- **Other offsets checked.** Reachability (`f_grown`, collisions, avoidance, forbid, tip contact `X ⊕ r`, `cover`)
  stays single-step on purpose: those offsets mirror the slicer (`TreeSupport.cpp:289-299`, `TreeModelVolumes`), and
  the slicer loses a 4 µm painted strip the same way. `cover`'s `F ⊕ G` (shortest edge 1 µm), the raster (≤ 0.25 µm),
  the paint prisms (`R ⊕ e`, 1.25 µm; placements are ≥ ε = 10 µm wide after the noise filter), tension strips and the
  hoop band (slice holes are > 0.1 mm wide after the slicer's closing) are not affected.
- Regression: `[sas-geometry] "Bands contain thin fronts (two-step offset)"`: the helper's bounds on a 10 µm strip and
  a square, and 44° / α 45 and 40° / auto-α walls, where every cluster band holds its fronts while a plain offset would
  be empty on every cluster.

## 2. Assert found by the bare build: paint vertices on slicing planes

- **Symptom** (orchestrator's assert build): `TriangleMeshSlicer.cpp:1125 chain_lines_by_triangle_connectivity:
  last_line->b == next_line->a` in "Verification adds nothing ..." on the inverted 60° cone.
- **Root cause** (instrumented, not committed): `slice_mesh_slabs()` converts the vertices of slab and horizontal edges
  with `to_2d(v).cast<coord_t>()` (truncation) but those of edges on a slicing plane with
  `v3f_scaled_to_contour_point()` (rounding). A vertex exactly on a plane then has two positions 1 nm apart. Here
  TriangleSelector's midpoint splits of the flank z 5…15 put vertices at z 7.5 = slice_z of layer 37 (on the patch
  boundary). Release: the chain still closes with a 1 nm jog (no open polylines; the result is unchanged).
- **Upstream?** Yes. `PrintObject::project_and_append_custom_facets()` makes the same call on the same data, so a
  Debug build of the slicer asserts when it slices this paint. A stock sphere brush on the same cone did not hit it
  (its boundary missed the on-plane vertices), so manual paint is affected only in principle.
- **Workaround in our code.** `project_states()` transforms the facets exactly as `transform_mesh_vertices_for_slicing()`
  does (same float z, same slabs), rounds XY to whole scaled units (both conversions agree there) and flips the
  triangles of a mirroring trafo (the slicer's face orientation). The projection is unchanged: the symmetric
  difference to the old one is ≤ 2e-5 mm² per layer, and the WP1a comparison with the slicer (incl. a mirrored part)
  still holds. Regression: `[sas-slicer] "project_states with paint vertices on slicing planes"` (585 on-plane
  vertices).
- **Second upstream assert** (same bare run): `MutablePolygon.cpp:244 clip_narrow_corner: dfar2 >= shortcut_length2`
  in `smooth_outward()` of the organic base polygons (`OrganicSupport.cpp:1412`), in WP2r's "real contact cross-check
  against process()". That test paints whole facets with `set_facet(ENFORCER)` (state 1 only, so the fork's projection
  change is a no-op) and runs the stock organic generator: stock 2.9.6 code on stock paint. In Release the branch
  computes a square root that may be NaN (`assert(u > 0.)` follows) — a possible wild support vertex, upstream. The
  assert run excludes this one case (README "How to test").

## 3. Reference cases (§7.2) — all run under `[SmartSupports]` (#25 is the hidden benchmark), all pass

Settled expectations (implementation vs plan, decided from the theory):

- **#8 dome R 30, B_ceil 20 — the expectation stands; the test is now exact.** Theory's 11.9 is where the continuous
  reach c(r) = R√2 − r − √(R² − r²) (from the 45° ring inwards; no hoop credit while Feret(H) = 2r > B_ceil) reaches
  K_max: r* = 11.88. The slices step the hole radius by ≈ 0.46 mm per layer. The first need is therefore on the first
  layer whose hole edge passes r*: hole r 11.52 (c 3.21) after 11.98 (c 2.94). The test checks c(first) > K_max ≥
  c(previous layer), one ring, should, and the plan's 11.9 ± 0.5 (= one layer step).
- **#9 Ø50 — the expectation was wrong; the implementation is right.** "must" is the level of the need without support
  (c up to 10.4, sliced 8.3). The joint plan works bottom up (theory §6). The first layer with c > K_max is a should
  band; its placement resets c (support anchors pass ĉ = 0, §2.3) and the chain restarts. With "critical and likely",
  every placement is therefore should and c never reaches f_hard·K_max. This is what the plan itself already expects on
  the sphere (#11: should rings, although theory says "must in its outer part"). With "critical only", the should needs
  are reported (BelowLowestLevel), add no anchors, and the must zone is placed. Theory's "usually unreachable": this
  block is 20 mm deep, and the branches leave the hole through its open ends (no problems). Test: default → only
  should placements, no problems; critical only → must placements + BelowLowestLevel rows. Ø20 (should only at 0.1 mm
  layers) and Ø8 (none) as settled by WP2c1a (the theory's values are the limits h → 0).
- **#10 T on 80 × 80, prefer plate — the expectation was wrong; the implementation is right.** The plan's "plus
  Unplaceable should bands c ∈ (3, 3.7]" (§3.9 note, review 5 m9) measures c from the stem only. The on-model must zone
  (c > 4.5) is built (branches 18 mm long), its contact reaches tip_d = 0.8 towards the stem (c 3.7), and it is a
  support anchor with ĉ = 0 (theory §2.2, §2.3). The band between stem and contact has c = min(distance to the stem,
  distance to the contact) ≤ 1.9 < K_max. Test: prefer plate → no Unplaceable row. The plan text is marked.
- #7 (± 0.4 on the hole radius) and #26 (tail top at apex + 2.5) as settled by WP2c1a/b (within tolerance).

## 4. Calibration without test prints

- **MayMergeOntoModel**, argued from the slicer and measured with it:
  - Influence areas grow at the **slow** move first (`AvoidanceType::Slow`, `TreeSupport.cpp:1881`); the fast angle is
    used only when the slow one fails.
  - `merge_influence_areas_two_elements()` merges a "to plate" into a "to model" branch (`to_buildplate = a && b`,
    `:2057`) only while the plate branch is at most `max_to_model_radius_increase` = 0.5 mm thicker
    (`TreeSupportCommon.hpp:201`). Its radius grows by tan(5°) per mm, so Δz ≤ 0.5 / tan 5° ≈ 5.7 mm.
  - New rule: reach `branch_d + (2·2 mm + Δz)·tan(support_tree_angle_slow)`, Δz ≤ 6 mm (was: the 40° tree angle,
    Δz ≤ 10). That is 3.9 mm at Δz 0 and 5.7 mm at Δz 4 (was 5.4 / 8.7 mm). `Params::tree_angle_slow_deg` is new
    (header batch).
  - Measurement (hidden `[sas-merge-calibration]`): an arm over a block (on-model branches 3 / 6 mm long) beside an arm
    over the plate (0 / 4 mm higher), gaps 1–12 mm, full `process()`, support area resting on the block with and
    without the plate arm's tips.
  - Result: plate branches landed on the model only with the plate arm 4 mm higher and gaps ≤ 3 mm (+10 to +20 %),
    never at Δz 0. The new rule is still conservative by ≈ 2–3 mm; the old one by ≈ 5 mm.
  - (A first metric, first-layer support under the plate arm, was dominated by ±35 % noise and by harmless merges
    with plate branches of the other arm; not used.)
  - Reference tests: the shelf over a block (plate zone 4.8 mm from the on-model zone, same layer) is no longer flagged.
    For same-layer pairs the planner always leaves ≈ f_hard·K_max between them. New case: a plate placement 4 mm above
    an on-model one, flagged at 4 mm, not at 10 mm.
  - Not checked: an on-model placement *above* a plate one whose branch passes it (needs the on-model branch length;
    rare).
- **PartlyOnModel** threshold (spill tip area ≥ A_min) kept. The slicer samples thin overhangs along their outline, so
  an area threshold cannot predict "at least one tip" for strips. The row is a note, so over-reporting costs little.
- **Defaults sanity-checked** against 2.9.6:
  - `min_height_to_model` 1 mm (`TreeSupportCommon.hpp:205`), `minimum_roof_area` 1 mm² (`:131`) and the
    `derive_params` mirrors (`[sas-input]`, now including the slow angle);
  - `K_max ≥ tip_spacing/2` holds at the defaults (3 ≥ 1.53, no ReachBelowTipSpacing note);
  - α auto (threshold 0) = ½ external width per layer, as the slicer.
  - V_min / H_merge, W_hoop, f_hard, B_max / B_ceil, K_max and H_s are physical placeholders: they need prints.

## 5. Performance (`notes/timings.md`)

- `run()` takes 1.1–1.9 s for the ≈ 1000-layer meshes (frog_legs ×2 and extruder_idler ×3 at 0.1 mm) and ≤ 3.1 s for
  1.3M triangles. Budget ≤ 30 s: met by an order of magnitude; no hotspot to fix. The plan (Phase B) dominates.
- Verification ≤ 0.17 s without a deficit and 1.2 s with its one evaluation pass. It stays on by default (the plan's
  "verification as an option" threshold was 10 s).

## 6. Robustness tests added (`test_smart_supports.cpp`, `[sas-reference]`)

The cases the reviews asked for that did not exist yet:
- repeated runs on their own paint (×3, islands and overhangs, verification): identical paint, placements and report,
  `changed = false`;
- a canceled run (at 5 / 30 / 60 / 85 %) leaves nothing behind: the next run is identical to one before the cancels;
- degenerate input: no volumes, an empty middle layer, a volume without triangles;
- a multi-volume object (a part with a non-identity volume matrix, a negative volume cutting the zone, a parameter
  modifier): 94 mm² planned, paint only on the shelf part and within the zone ⊕ e;
- a non-uniformly scaled instance (2, 1, 1);
- determinism 1 thread vs default on frog_legs with real reachability and verification (`[slow]`);
- P4 with brush-painted manual enforcers: needs never grow on any layer (102 → 11.9 mm²), nothing dropped.

Already present and kept: cancel per phase (#24), P4 with an enforcer volume (#21), one re-run, empty input / one
layer, scaled / mirrored / rotated / sinking (#23), raft 2, first layer ≠ layer height, 0.1 mm layers, determinism with
`test_assume_reachable` (#20).

## 7. GUI switch (WP3 contract)

- The GUI runs `SS::run()` unless AppConfig `[smart_auto_supports] debug_fake_result = 1` (`fake_mode()`). The default
  is off, so no change was needed. The fake stays for GUI-only host checks.
- `run()` meets the contract:
  - `CanceledException` from every phase (#24 and the new cancel case);
  - the progress callback is called from TBB workers, serialised and monotone in `run()`, and the panel's callback is
    thread-safe;
  - one `VolumePaint` per `in.volumes` with a correct `changed` (re-run tests);
  - `painted_areas` filled.
- Header batch: `inline bool is_problem(const ReportEntry &)` in `SmartSupports.hpp`, used by `build_report` and the
  panel (its copy is gone). The report test checks it against the documented rule for all states and causes.

## 8. Verification results (sandbox)

| Check | Result |
|---|---|
| `libslic3r_tests "[SmartSupports]"` (lane 1, dev merged) | 60 test cases / 28 605 assertions passed |
| `fff_print_tests "[SmartSupports]"` (lane 1) | 77 test cases / 7 704 assertions passed |
| full `libslic3r_tests` | 290 test cases: 289 passed, 1 failed as expected (arc welder `[!shouldfail]`) |
| full `fff_print_tests` | 204 test cases / 219 576 assertions passed |
| lane 2 `PrusaSlicer` (wx 3.3, dev merged) | built and linked |
| bare feature branch on plain 2.9.6, asserts on, GUI off (`bare-build.sh 576ca2a185`) | `libslic3r_tests "[SmartSupports]"` 60 / 28 605 passed; `fff_print_tests "[SmartSupports]"` without the upstream-asserting cross-check: 76 / 7 668 passed |
| warnings in feature code | none (the four in the assert build are on upstream lines of `3mf.cpp`, `Print.cpp`, `TriangleSelector.cpp`) |

## 9. Code review fix round (reviews A, B, C in `reviews/code-review-*.md`)

| Finding | Status | What |
|---|---|---|
| B M1 verify() hides missing paint under placement notes | fixed | only unplaced residual (Unplaceable, BlockedByUser, WillBeDropped, TailWithoutDownwardFacets, folded trees) masks verification needs; regression: shelf over a block, nothing painted → RemainingAfterPaint 102 mm² (was 31) |
| C M1 stale run committed as fresh | fixed | fingerprint taken at `start_job()`, finalize discards on any change of settings / instance orientation / z offset / volume matrices, `LastRun` keeps the start fingerprint; the wait never uses slices while `is_background_process_update_scheduled()` |
| C M2 "Slicing…" hangs up to 15 min | fixed | after a 2 s grace with nothing running and posSlice not done: one reslice, then a failure; a slicer error fails at once. The decision is the pure `SmartSupportsWait::decide()` (header-only), tested in `slic3rutils_tests "[SmartSupports]"` |
| A M1 3MF load memory, per-volume choice | fixed | sparse `(triangle, value)` storage of the fork attribute; per-triangle choice; test with an edited file |
| A M2 scan disagrees with deserialization | fixed | tree walk with a depth counter as `TriangleSelector::has_facets()`; test with trailing data ("48", "0C8") |
| A M3 rule / cursor survive a throw | fixed | `SmartSupports::PaintRuleScope` (RAII, `SmartSupportStates.hpp`), `select_patch()` drops a `PredicateCursor` on exit; test: a manual stroke after a cancelled smart stroke paints what it asks for |
| A M4 `reset()` clears used_states | fixed (trivial) | stock `reset()` again; the smart scans never read used_states (plan §3.1 superseded) |
| B m1 unpainted long-span rows as support | fixed (P3) | rows stay planned anchors and planned zones in the verification (theory §7: disabled causes stay planned); painted placements after the first row get `ReliesOnDisabledCauses`, GUI text "…(unchecked causes, long bridges)"; test |
| B m2 no cancel in classification / tension | fixed | optional `ThrowOnCancel` in `tension_levels()` / `classify_front()` (per θ, 256 segments, 64 rows, 64k boundary samples) |
| B m3 Clipper per long segment | fixed | bbox prefilter + C′ clipped to the strip |
| B m4 grid memory | fixed (cap) | `grid_max_pixels` wins over `grid_res_max` (coarser grid beyond ≈ 200 × 200 mm); test. Not changed: the cluster cache keeps the last two results per cluster and the carry keeps cropped fields for the H_s window (≤ 4 M cells each now) |
| C m1 shutdown crash | fixed | finalize and deferred calls return when `wxGetApp().plater()` is null |
| C m2 dropped job / foreign cancel | fixed | queue only on an idle worker (`decide()` waits otherwise); no `Worker::cancel()` in `cancel_run()` |
| C m3 stock Automatic painting pending | fixed | host `stock_autopaint_pending()`; Paint disabled with a reason |
| C m4 marker / clip on the wrong instance | fixed (trivial) | drawn on the selected instance if it has the analysed orientation and z offset, else disabled with a tooltip |
| C m5 empty undo step | fixed | no snapshot without smart paint in the selectors |
| C m6 stuck consumed-click flag | fixed | cleared on every press while not busy |
| C m7 cost for users who never open the section | fixed | no tick / config lookup / AppConfig write while closed and idle; α derived on first opening; no window widening while closed |

GUI behaviour is compile-, link- and logic-tested only (no display); the host checklist (plan §7.3) covers the rest.

Verification of the fix round:

| Check | Result |
|---|---|
| `libslic3r_tests "[SmartSupports]"` | 62 test cases / 28 671 assertions passed |
| `fff_print_tests "[SmartSupports]"` | 78 test cases / 7 708 assertions passed |
| `slic3rutils_tests "[SmartSupports]"` (lane 2) | 1 test case / 15 assertions passed |
| lane 2 `PrusaSlicer` | built and linked, no warnings in feature files |
| full `libslic3r_tests` | 292 test cases: 291 passed, 1 failed as expected (arc welder `[!shouldfail]`) |
| full `fff_print_tests` | 205 test cases / 219 580 assertions passed |
| bare plain 2.9.6, asserts on (`bare-build.sh 61a8059ab3`) | `libslic3r_tests "[SmartSupports]"` 62 / 28 671 passed; `fff_print_tests "[SmartSupports]"` without the upstream-asserting cross-check 77 / 7 672 passed; no warnings in feature files |

## 10. Host test 1: foot model

**Report.** On a foot skeleton standing on its sole (one object, 1.25M triangles, 100 × 279 × 95 mm, 475 layers; plain 3MF
from another program, not in the repository) many islands got no island paint: "Painted 141 areas (islands 53, overhangs
88)", "32 problems, 117 notes" (auto α, reach 3, critical only, prefer plate).

**Diagnosis** (hidden debug harness, uncommitted; 0.4 nozzle, 0.2 layers, organic, interface 0 matches the user's counts
best). Every downward local minimum of the mesh (43 with a prominence ≥ 0.3 mm) was detected as an island tree slice;
nothing was wrongly grounded, filtered or silently lost. Two rules folded the islands away:
1. **Near-bed starts.** Three toe tips float 0.12–0.35 mm above the bed: they miss layer 0 and start on layers 1–2, where
   `tips_possible()` is false. Their `NoTipLayer` residual marked the **whole island tree** unplaced, and each tree was a
   whole toe ray (≈ 7000–8100 mm³, merging with the foot only at z 35–41 mm). Every later island of the toe (16 knuckle
   minima at z 0.4–34 mm) folded into it unpainted; 14 of the 18 "Too close to the bed" rows started at z 1.4–34 mm.
2. **Per-tree folding.** With interface layers > 0, any leftover island piece (typically an `InterfaceGap` tail ring) marked
   the whole tree, so later sibling islands folded as well (4 more minima at interface 3).
Folded rows also inherited the first row's state and reason.

**Fixes** (`fix(smart-auto-supports): …`):
- **Plate-resting near-bed starts** (`SmartSupportsBase.cpp`): below `first_tip_layer()` = `max(0, z_top + 2 − raft)`
  (shared with `Reachability::tips_possible()`, `SmartSupportsInternal.hpp`) a slice without (strong) contact below hangs
  at most `z_top + 1` layers above the plate and nothing can be placed under it; it is laid onto the plate like layer 0:
  its weak contact is cleared, its fronts are dropped and it is grounded (`bed`). Weak contact applies again from the
  first tip layer on (a tiny tip still makes the next layer an island start there).
- **Island starts** (`IslandStart`, `LayerGeom::slice_start`, `tail_starts`): every bad tree slice is a start. Tags of
  ungrounded material, island sources and tails are start ids; `LayerEvaluator::mark_unplaced(pieces)` marks the start
  whose source region covers most of a residual piece (`mark_tree_unplaced` remains for tests); the planner keeps the
  marking layer per start. Only material hanging from that start folds.
- **Own report row** (`ReportState::HangsFromUnplacedIsland`, a note): "Rests on an island that cannot be supported",
  advice "This part grows from an island listed under Problems; it is supported only if that island is." The
  verification treats it as reported (like the other unplaced states).
- Fix A (do not mark for `NoTipLayer`) is not needed: near-bed starts no longer exist.

**Foot model, before → after** (debug harness, same settings):

| | interface 0 | interface 3 |
|---|---|---|
| painted areas (islands / overhangs) | 156 (71 / 85) → 176 (93 / 83) | 115 (39 / 76) → 137 (66 / 71) |
| report problems / notes | 31 / 113 → 12 / 119 | 93 / 102 → 74 / 121 |
| downward minima in island trees without paint | 16 of 43 → 0 of 38 | 20 of 43 → 0 of 38 |
| island need left after painting (re-evaluated with the actual paint) | 499 mm² → 2.1 mm² | 643 mm² → 143 mm² (InterfaceGap rings) |

After the fix the 4 toe-tip minima rest on the plate (no need) and 1 is a tolerated bump; trees 32 → 41 (the knuckles
are their own trees once the toes are grounded). Run time unchanged (≈ 12–27 s for `run()` on this model).

**Tests** (all `[SmartSupports]`):
- `[sas-islands]` "Island starts: …": toe with the tip at z 1 → one tree, four starts with their own tails; tips 0.15 /
  0.35 above the bed → no start or tree below the first tip layer, the tip's slices grounded without fronts, the knuckles
  their own trees.
- `[sas-reference]` "Host test 1: …": tips 0.15 / 0.35 → no island `NoTipLayer`, no folded row, no island problem, every
  knuckle's first slice ≥ 90 % under island paint; tip at z 1 with interface 3 → `InterfaceGap` rows for the tip, the
  knuckles still painted, folded rows are notes without a why.
- `[sas-geometry]` `first_tip_layer()` equals `Reachability::tips_possible()` for z_top 0–3 and rafts 0–8.
- Mutation check (temporary switches, not committed): without the plate rule the near-bed section fails (NoTipLayer and
  folded rows), with per-tree marking the interface-3 section fails (knuckles unpainted).
- Updated with reason: ball in a cup, plate only (the cap hanging from the unplaceable ball is now one
  `HangsFromUnplacedIsland` note over 10 layers next to the 1-layer `Unplaceable/NotReachable` row, was one merged
  `Unplaceable` row); the floating-sphere folding test compares with the start id; the report group test covers the new
  state.

| Check | Result |
|---|---|
| `libslic3r_tests "[SmartSupports]"` | 63 test cases / 29 250 assertions passed |
| `fff_print_tests "[SmartSupports]"` | 80 test cases / 7 886 assertions passed |
| `slic3rutils_tests "[SmartSupports]"` (lane 2) | 1 test case / 15 assertions passed |
| lane 2 `PrusaSlicer` | built and linked |
| full `libslic3r_tests` | 293 test cases: 292 passed, 1 failed as expected (arc welder `[!shouldfail]`) |
| full `fff_print_tests` | 207 test cases / 219 758 assertions passed |

## Remaining known limitations and risks

- **Upstream asserts (Debug builds of the slicer itself, not our code).** Both are possible `fix/<id>` branches for
  the user to decide:
  - the slab projection's truncation vs rounding (a 3-line fix: use `v3f_scaled_to_contour_point()` for slab and
    horizontal edges);
  - `clip_narrow_corner`'s precondition in organic support smoothing.
- **Host tests** (plan §7.3): host test 1 (foot model, §10) found and fixed the near-bed / per-tree folding issue; the
  rest of the checklist is pending. The GUI is only compile- and link-checked here.
- **MayMergeOntoModel** is a heuristic, calibrated on one geometry family. It is conservative by ≈ 2–3 mm and does not
  check on-model placements above plate ones.
- **Paint under-approximation at prism borders.** E.g. the cone's tail rings next to the apex are painted only partly,
  because the apex layer's prism is empty after the noise filter. The contact's tip_d growth covers them, and
  verification re-checks.
- **Not mirrored / not modelled** (plan §3.14): closed cavities, the roof-line check, crowding, other objects, discrete
  tip gaps on isolated thin rings (WP2r: up to ≈ 2·tip_spacing), and long spans (planned, not painted).
- **Theory §11 #12** (roof merges across causes): the acceptance check exists (WP2c1b) but has no dedicated test.
- Strips thinner than 0.005 % of an offset distance still vanish (≤ 0.14 µm; no practical effect).
- Physical constants (K_max, B_max, B_ceil, H_s, f_hard, V_min, H_merge, W_hoop) remain placeholders until test prints.
