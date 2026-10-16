# Work package log — WP2c1b (joint plan, verification, run())

Scope (plan §8.2): `make_plan` and `verify` in `src/libslic3r/Support/SmartSupportsPlan.cpp` (plan §3.9, §3.10, theory §5.2,
§5.3, §6, §7), `run()` in `src/libslic3r/Support/SmartSupports.cpp` (plan §2.2, §2.3) and the reference cases of plan §7.2 in
`tests/fff_print/test_smart_supports.cpp` (`[SmartSupports][sas-reference]`). No header changed.

## What the modules do (contract for WP3 / WP5)

### `run(in, opt, is_canceled, progress)`
1. `cancel = [&]{ if (is_canceled && is_canceled()) throw CanceledException(); }`; an empty input (no layers) returns the
   unchanged paint.
2. `tbb::parallel_invoke(build_base_model, Reachability::compute_collisions)`, then
   `start_prefetch(tip_layer(last_layer_with_fronts), is_canceled)`.
3. `make_plan`; then `opt.test_mutate_before_paint` on a **copy** of the placements (only the paint sees the mutation;
   `Result::placements` and the report keep the plan).
4. `opt.paint`: `paint_placements(in, placements, up.paint_causes, up.lowest_level, cancel)`; else the unchanged paint
   (`changed = false`). `painted_areas[cause]` = placements passing WP1b's filter (paintable, cause checked and not long
   span, level ≥ lowest), counted only when painting.
5. Verification if `opt.verification` (−1 → `Params::verification`) **and** `opt.paint`.
6. `build_report(in, plan, verify_residual)`; `layer_stats = plan.stats` with `collect_stats`; `fingerprint` stays 0
   (WP2c0: the GUI computes `config_fingerprint`).
- **Progress**: Phase A 0–15 %, plan 15–80 %, 80 before / 90 after the paint, 97 after the verification, 100 at the end.
  Thread-safe (Phase A reports from a TBB worker): calls are serialised by a mutex, only increasing values are passed on.
- **Cancel**: `CanceledException` from every phase (tested at Phase A, plan, paint, verification); `~Reachability` stops and
  joins the prefetch thread during unwinding. `cancel` is called from TBB workers (Phase A, collisions, paint, verification).

### `make_plan` (per layer n = 1 … `last_layer_with_fronts`, serial)
- `E` = `manual_enf_n`; blocker zone `blk` = `∪_{|j−n|≤k_d} manual_blk_j ⊕ (tip_d + extra_outset + e)`.
- Manual paint the slicer drops → `WillBeDropped` (see deviations).
- `L = evaluate(n, E, {}, nullopt)`; `plan.stats[n] = L.stats` (needs of the layer before its own placements).
- Causes Island → Cantilever → LongSpan, ≤ `max_place_iterations` per cause; needs = `L.need[cause][≥ lowest]`; stages
  plate → widen → model (each falls through on nothing / no progress):
  - **plate**: `Z = needs` (long spans: `rows(needs)`) − `blk`; `P = Z ∩ cover(contact(E ∪ Z).plate)`; one placement per need
    component (level = the highest level in that component); long-span rows: one placement per component of `P`.
  - **widen** (islands, cantilevers): need components with < `A_min` plate part; `Zw = (C' ∩ (N ⊕ K_max)) − blk`; one
    placement per component of `plate_part(Zw)`, level = highest need level within `K_max`.
  - **model** (prefer plate only): `M = need[cause][Must] − blk` (long spans: their must rows at the interval centres),
    noise-filtered; per component kept if `area(M_c ∩ cover(contact(E ∪ M).all)) ≥ A_min`; `on_model = true`, level must.
  - trial: `E' = E ∪ new`, `overrides + (needs, cause)`; accepted iff the cause's needs fall by ≥ `A_min` (and see the roof
    check below); otherwise `evaluate(n, E, overrides, touched)` restores the evaluator (served from its cache).
- Accepted placements: ids in plan order; flags into the residual: long spans `LongSpanNotPainted`; on-model
  `OnModelTooShort` (`below_model_too_short`) else `OnModelFallback`; plate/widen placements with
  `area(tips(P).spill_model) ≥ A_min` → `PartlyOnModel` (not with `buildplate_only` or `test_assume_reachable`).
- **Residual reasons** per piece (plan §3.9): `∩ blk` → `BlockedByUser`; `∩` a known why (NoTipLayer, InterfaceGap) →
  `Unplaceable` with it; components with no candidate within `K_max` → `Unplaceable/NotReachable`; else
  `Unplaceable/NoProgress` (also when the iterations are exhausted). Known whys are collected from the layer evaluation, every
  trial evaluation and every stage contact (see deviations). Pieces < 0.01 mm² (boolean slivers) are dropped.
- Unplaced islands: the island tree of every residual island piece (`slice_tree` of the slice containing it) is marked
  unplaced, `L` is re-evaluated (folding applies from the same layer on); `L.folded` of the later layers goes into the
  residual with the tree's first reason/why (`Island`, `Must`) — the report merges them into one row per tree.
- Needs below the lowest level → `BelowLowestLevel` (any cause).
- `zones_paintable[n]` = placements that are painted (paintable, cause ∈ `paint_causes ∩ {I, C}`, level ≥ lowest),
  `zones_unpainted[n]` = the other placements (long spans, unchecked causes). The plan itself never reads the toggles (P3,
  test #18).
- After the loop: `TailWithoutDownwardFacets` per non-tolerated tree with `tail_without_downward` (area = the tree's slices at
  `tail_last`, the row's marker); `MayMergeOntoModel` (heuristic below).

### `verify`
1. `E_act = project_states(paint, s == ENFORCER || auto with an island/cantilever bit of paint_causes)` ∪ `manual_enf`
   (enforcer volumes) ∪ `zones_unpainted`; planned = `manual_enf ∪ zones_paintable ∪ zones_unpainted`.
2. Parallel over layers 1 … `last_layer_with_fronts`: `deficit_n = area((cover(F_plan) − cover(F_act)) ∩ O_n) ≥ A_min`,
   `O_n = L_n − L_{n−1}`.
3. No deficit → ∅. Else one `evaluate_pass(E_act)`; needs (levels ≥ lowest) minus the plan residual of the same layer
   ⊕ 0.5 mm, noise-filtered → `RemainingAfterPaint`.

## Deviations from the plan (exact)

1. **WillBeDropped** (§3.9 "tips(E) vs F_grown(E)"): the painted overhang `(E ∩ L_n) − L_{n−1}` (noise-filtered) minus
   `cover(contact(E).all)`; why `NoTipLayer` (no tip layer), `InterfaceGap` (inside `cover(tip_path_all)` with a roof),
   else `NotReachable`. Reported as `Cause::Cantilever`, `Level::Must` (manual paint has no cause).
2. **Blocker clearance** = `tip_d + extra_outset + e` (e = `edge_limit`, the paint prism inflation) and the blockers of the
   layers within `k_d`: the paint may exceed a zone by e and spans `k_d` layers; with the plan's `tip_d + extra_outset` the
   paint would come closer than the clearance (#19 checks paint ∩ (blocker ⊕ (tip_d + extra_outset)) = 0).
3. **InterfaceGap attribution of candidates**: the plate / widen / model stages compute the contact of `E ∪ Z` themselves
   (same formula as `plate_part`, one contact) and add `needs ∩ cover(tip_path_all) − cover(all)` to the known whys. Without
   this the 40° wall at interface 3 (#27) was `NotReachable`: the roof candidate has no plate part, so no trial is
   evaluated and the evaluator's own why (for `E` = manual only) has no roof.
4. **`touched`** (§3.9 `bbox(predict_F(E') Δ predict_F(E))`): `bbox(new) ⊕ (tip_d + extra_outset + r + xy + 1 mm + G)` with
   interface layers 0 (the contact is local and superadditive), `nullopt` with roofs (a merge can move contact anywhere);
   the evaluator decides by exact contact comparison anyway (WP2c1a). Saves two contact computations per trial.
5. **Long spans**: no widen stage (rows already take the plate position nearest the interval centre, theory §5.2); the
   model stage paints rows at the interval centres, not the whole must region; long-span model placements get only
   `LongSpanNotPainted` (no `OnModelFallback` row: nothing is painted).
6. **Placements per component** for rows, widen and model (the plan's "per need component" is kept for the plate stage).
7. **Verification deficit over `O_n` only** (§3.10 `area(cover(F_plan) − cover(F_act))`): the mirrored
   `F_grown = (E ∩ O_n) ⊕ tip_d` (miter) is not monotone in the outline of `E` — the paint's triangle outline loses offset
   corners in the air beyond the slice (sphere R 20: 0.22 mm² lost although the paint covers the zone), which triggered a
   needless pass (0.24 s). Contact outside `O_n` supports nothing.
8. **`plan.stats`** = the layer's evaluation before its own placements (with the manual paint and the plan below).
9. **`test_assume_reachable`** (not used by `Reachability`): the planner's plate part counts every contact
   (`cover(contact.all)`); no `PartlyOnModel` flags in that mode.
10. **Test hook**: `test_mutate_before_paint` mutates a copy; `Result::placements` and the report stay the plan's.

## Decisions on open points

- **Theory §11 #12 (roof merges across causes, WP4 gap)**: implemented the acceptance rule of theory §5.2 ("the needs of no
  other cause grow") where it can be violated: with interface layers > 0, if a trial grows the needs (≥ lowest) of another
  cause by ≥ `A_min`, `contact_lost` checks `area(cover(F(E)) − cover(F(E ∪ new))) ≥ A_min`; then the trial is rejected
  (no trace) and the cause's pieces that end as `NoProgress` get why `InterfaceGap`. Without roofs nothing is checked: other
  causes then change only by reclassification (e.g. cantilever must → long span must when support makes lines finite),
  which must not reject a placement. Cost: two contacts, only when another cause grew. Merges with zones of *other layers*
  cannot happen (contact is per layer); the verification compares paint and plan, which contain the same merges. Not
  covered by a dedicated test (constructing a cross-cause roof merge is fiddly; WP5 may add a synthetic one).
- **MayMergeOntoModel** (v1 heuristic, plan §3.9 leaves it open): a painted plate placement P and a painted on-model
  placement Q at a layer ≤ P's, ≤ 10 mm below, closer than `branch_d + (2·2 mm + Δz)·tan(tree_angle)` (both influence
  areas grow by tan(angle) per mm during a 2 mm descent). Constants to calibrate on the host.
- **Levels**: `lowest_level` is used as is (May would plan may needs; the GUI offers Should / Must).
- **`RunOptions::paint = false`**: plan and report only; no verification.

## Bugs and findings in other modules

- None blocking. Finding (WP2r, `SmartSupportsReachability.cpp`, `Impl::f_grown`): `offset(union_ex(E ∩ O_n), tip_d)` with
  the default miter join is not monotone in E's outline (above, deviation 7). It mirrors the slicer
  (`TreeSupport.cpp:289-299`), so no change is proposed; consumers comparing contacts of different paints should compare over
  `O_n` (or within the slice).
- `Params::test_assume_reachable` is documented in `SmartSupports.hpp` but implemented nowhere else; the planner's reading
  (deviation 9) is enough for #11 / #12.

## Header change requests

None.

## Reference cases (plan §7.2) — `fff_print_tests "[sas-reference]"`, α = 45°, interface 0 unless stated

| # | Result | Expected |
|---|---|---|
| 1 | no placements, max c 1.80 | none; 1.8 ± 0.1 ✓ |
| 2 | 1 placement, 1 layer (print_z 18.2), 102.0 mm², 3.2 mm from the cube, must | ✓ |
| 3 | 204.0 mm², 1 layer | 204 ± 10 % ✓ |
| 4 | 2 × 118.0 mm², 3.2 from the stem, must | ✓ |
| 5 | none, empty report | ✓ |
| 6 | 1 long-span row 9.9 mm², should, not paintable, mid-span ± 1 mm, `LongSpanNotPainted`, paint unchanged | ✓ |
| 7 | default B_ceil: none; B_ceil 0: first placement slice_z 19.1, hole r 5.93, should; none below 18.5 | ✓ (r ± 0.4, WP2c1a) |
| 8 | dome R 30 / 35, B_ceil 20: one ring at hole r 11.52, should (one layer, 14.7 mm²) | 11.9 ± 0.5 ✓ |
| 9 | Ø8 none; Ø20 none at 0.2 mm, should at 0.1 mm; Ø50: 4 should placements (z 58.4–59.8); with "critical only" 3 must placements + BelowLowestLevel rows | **deviation**: the plan's "must" holds without support or with "critical only"; with should planned the chain restarts at the should band |
| 10 | prefer plate: 2 on-model must placements 103 mm² each + 2 `OnModelFallback`; plate only: no placements, 4 `Unplaceable/NotReachable` rows (2 must 103, 2 should 15 mm²) | ✓ except the plan's "Unplaceable should bands (3, 3.7]" with prefer plate: none remain (the on-model support anchors them, theory-consistent) |
| 11 | sphere R 20, assume reachable: rings at inner r 5.67 and 12.06, should; all within (5.2, 14.6) | ✓ |
| 12 | elevated sphere: island 12.1 mm² at print_z 5.2, then the same rings; no Unplaceable | ✓ |
| 13 | first placement print_z 16.0; 3 placements (16.0 / ≈ 32 / 47.6), none below 14 | 15.8 ± 1.6 ✓ |
| 14 | 44°: none; 40° with H_s 0: none | ✓ |
| 15 | risers 0.4: placements on steps 2, 4, 6 (should); 1.2: none | ✓ |
| 16 | lean-to 30: none; 50: no placements, `BelowLowestLevel` long span may (z 4.4–11.6, 232 mm²) | ✓ |
| 17 | V_min 20: none; default: 1 island placement 25 mm² under the logo (print_z 9.6), nothing else | ✓ |
| 18 | {I,C}, {I}, {C}: identical placements; paint {I,C} = {I} ∪ {C} (difference 0 mm²); CauseDisabled / ReliesOnDisabledCauses rows | ✓ |
| 19 | shelf + blocker volume: placement 38.3 mm², `BlockedByUser` 6.75 mm² (note), paint inside blocker ⊕ (tip_d + extra_outset) 0 mm² | ✓ |
| 20 | 1 thread = default: paint bytes, placements, report identical | ✓ |
| 21 | 40° wall + enforcer volume: needs 131 → 69 mm², max growth per layer 1e-16 mm²; nothing dropped | ✓ |
| 22 | verification without mutation: no RemainingAfterPaint; placement dropped: RemainingAfterPaint 79.5 + 22.5 = 102 mm² | ✓ |
| 23 | empty input, 1 layer, grounded cube: nothing; shelf with first 0.3 / 0.15: 102.8; 0.1 mm: 103.5; raft 2: 102.0; scaled ×2: 102.0; mirrored: 102.0 (paint changed); X-rotated 90°: 13.6 (fin bottom 2 × 6.8); sinking 5 mm: 102.0 at print_z 13.2 | ✓ |
| 24 | `CanceledException` at progress ≥ 1 (Phase A), 16, 50 (plan), 80 (paint), 90 (verification); progress monotone to 100 | ✓ |
| 25 | `[.benchmark][sas-reference-bench]`, see timings | — |
| 26 | inverted 60° cone: island placements up to apex + 2.5 (11 placements, 6.4 mm²) plus 2 cantilever should placements under the top plate; no Unplaceable | 2.6 ± 0.3 ✓ |
| 26b | 75° cone: 22 island placements (z 6.4–10.6), no Unplaceable; 60° cone at 0.1 mm (z_top 2): 21 island + 2 cantilever placements, no Unplaceable | ✓ |
| 27 | interface 3: wall 40° → no placements, `Unplaceable/InterfaceGap` should (z 16–24.4, 30 mm²) and must (z 23.8–50, 101 mm²); `InterfaceLayers` advisory only; shelf → 102 mm² placed | ✓ |

Further tests: needle (tip placed, `TailWithoutDownwardFacets`), ball in a cup (prefer plate: island on the model,
`OnModelFallback`; plate only: one `Unplaceable/NotReachable` island row over 11 layers, ≥ 10 merged pieces), manual enforcer
under a T arm with build plate only (`WillBeDropped/NotReachable` 150 mm²), 60° cone at interface 3 (tail rings
`Unplaceable/InterfaceGap`), shelf over a block (plate + on-model placements, `OnModelFallback`, `MayMergeOntoModel`,
`PartlyOnModel`), re-run on its own paint (`changed = false`, identical data), `paint = false`, verification on ten
reference shapes (no `RemainingAfterPaint`).

## Timings (sandbox, 8 cores, Release; `[sas-reference-bench]`, verification on)

| Shape | Layers | Total | Phase A | Plan | Paint | Verification + report |
|---|---|---|---|---|---|---|
| figurine_proxy | 480 | 0.21 s | 0.05 | 0.16 | 0.00 | 0.00 |
| T on 80 × 80 | 110 | 0.02 s | 0.00 | 0.02 | 0.00 | 0.00 |
| hole Ø50 | 350 | 0.08 s | 0.00 | 0.07 | 0.00 | 0.00 |
| dome R 20 | 125 | 0.16 s | 0.05 | 0.11 | 0.00 | 0.00 |
| sphere R 20 | 200 | 0.36 s | 0.06 | 0.27 | 0.01 | 0.02 |
| wall 40° | 250 | 0.13 s | 0.00 | 0.12 | 0.00 | 0.00 |

All reference cases together ≈ 9 s (most of it slicing the cached prints).

## Notes for WP5

- The GUI can switch from the fake to `run()` as is (WP3 contract: `CanceledException`, progress from any thread, one
  `VolumePaint` per volume with a correct `changed`, `painted_areas`).
- Plan §7.2 expectations to settle: #9 Ø50 "must" (only with "critical only" in the joint plan), #10 should band (none),
  #8 hole r 11.52 (11.9 ± 0.5), #9 Ø20 needs 0.1 mm layers (WP2c1a), #26 tail top at apex + 2.5.
- Calibrate the `MayMergeOntoModel` constants and the `PartlyOnModel` threshold (`A_min` of spill tips) on the host; the
  sphere rings and shelves never flag it, shelves over model parts do.
- Real meshes (`frog_legs`, `extruder_idler` ×3) are not measured here; the benchmark case lists the synthetic shapes.
- Verification cost is ≈ 0 when the paint covers the plan; a deficit costs one `evaluate_pass` (sphere R 20: 0.24 s).
