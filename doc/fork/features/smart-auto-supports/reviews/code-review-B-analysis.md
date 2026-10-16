# Code review B — analysis modules (`src/libslic3r/Support/SmartSupports*`)

Reviewer B, 2026-10-16, on `feature/smart-auto-supports` @ 19ec04ce3d (`git diff version_2.9.6..HEAD`).
Scope: `SmartSupports.{hpp,cpp}`, `SmartSupportsInternal.hpp`, `Input`, `Base`, `Reachability`, `Raster`, `Reach`,
`Tension`, `Eval`, `Plan`, `Report`, and their tests.

Method: a full read against plan §2.3, §3.5–§3.12 and the wp-logs, mirror checks against `TreeSupport.cpp`,
`TreeModelVolumes.cpp`, `TreeSupportCommon.cpp`, `PrintObjectSlice.cpp` and `Layer.cpp`, and two throwaway probes.
The probes were linked in `/tmp` against the sandbox build (sources identical to HEAD) and removed afterwards:
- a verification probe (finding M1);
- a sweep of 9 configs × 4 shapes × 3 parameter sets with verification on. The configs were raft 3 + interface 2,
  build plate only + interface 2, contact 0, tip/branch 3 mm, tree angle 0, xy 0, 0.05 mm layers, and raft 1 +
  contact 0 + interface 3. The parameter sets included α 89, β 10, H_s 5 and K 0.5, B 5, B_ceil 0, H_s 0, PlateOnly.
  No crash or hang, all runs ≤ 2.6 s.

## Critical

None found. Checked and clean:
- **Indexing:** n−1 at n = 0, raft offsets (`slices(s)`, `tip_layer`, `tips_possible` match `TreeSupport.cpp:1299-1306`),
  1-layer and empty layers, empty fronts/ExPolygons, undefined bboxes (every `get_extents` use on a possibly empty set
  is guarded).
- **Arithmetic:** divisions by zero, inf/NaN into Clipper (user params are clamped by the GUI, tree angle ≤ 85°).
- **Lifetimes:** `LayerGeom::in`, `Progress::band`, the prefetch lambda and the evaluator references all outlive their
  users. `~Impl` joins the thread before any member dies.

## Major

### M1 — Verification hides RemainingAfterPaint under every placement that carries a note

`SmartSupportsPlan.cpp:861-864, 877-880` (with `accept()` `:554-562` and `may_merge()` `:766`).

**Code.** `verify()` builds `reported[n]` from all of `plan.residual`. A new need within 0.5 mm of it is "already
reported" and dropped. But the residual also holds the notes and flags of placements that were placed (and painted),
each with `area = pl.area`: `PartlyOnModel` (a note), `OnModelFallback`, `OnModelTooShort`, `MayMergeOntoModel`,
`LongSpanNotPainted`.

**Effect.** Verification exists to find needs left where the actual paint falls short of a placement (D13). Exactly
there the remaining need lies inside `pl.area ⊕ 0.5 mm`, so it is filtered out. `PartlyOnModel` is set for any plate
placement with ≥ A_min of tips over the model whenever `buildplate_only` is off, i.e. for any overhang that has a lower
part of the object a few mm below it.

**Evidence (probe).** `shelf_cube(10, 15)`, default `sas_config()`, verification on, all placements removed with
`test_mutate_before_paint`, so nothing is painted:

| Geometry | Plan | Verification |
|---|---|---|
| shelf over the plate | 1 placement | RemainingAfterPaint **102.0 mm²** (as #22) |
| same shelf + a block `box({22, 2.5, 0}, {30, 17.5, 15})` 3 mm under its outer half | 1 placement + PartlyOnModel 56.7 mm² | RemainingAfterPaint **31.1 mm²** |

The shelf layers are the same in both cases. About 71 mm² (≈ 70 %) of the missing support is reported nowhere; the
only row left there is a *note*. #22 does not catch this because its shelf stands over the plate.

**Fix.** Build `reported` only from reasons that describe unplaced needs: Unplaceable, BlockedByUser, WillBeDropped,
TailWithoutDownwardFacets and the folded-tree entries. Possibly add BelowLowestLevel, since its 0.5 mm band also masks
adjacent ≥-lowest needs. Exclude PartlyOnModel, OnModelFallback, OnModelTooShort, MayMergeOntoModel and
LongSpanNotPainted. Add a regression test with the shelf-over-block case above.

## Minor

### m1 — Long-span rows are anchors for everything above them, but are never painted and never reported as relied on

`SmartSupportsPlan.cpp:657-661, 835`; `SmartSupportsEval.cpp:903, 911-917`; `SmartSupportsReport.cpp:397`.

**Code.**
- An accepted LongSpan placement joins `E`.
- `commit(n)` then uses its contact as F⁺: `zero` for the carry (ĉ = 0) and `UG_n −= cover(F)`.
- In v1 the row is never painted (`paintable = false`).
- `add_cause_summaries` only checks `{Island, Cantilever}`, so painted placements that lean on a row (within H_s
  above, through ĉ) get no `ReliesOnDisabledCauses` row.
- `verify()` puts `zones_unpainted` into `E_act` as if painted, so no deficit appears.

**Theory.** §6 P3 requires that each painted placement lists the planned placements of disabled causes it relies on.
In v1 a long span is effectively a permanently disabled cause.

**Effect.** Limited by the H_s fade (default 1 mm), but silent.

**Fix.** Treat cbLongSpan as disabled in `add_cause_summaries` while `paintable = false`, or keep long-span rows out of
`E` for the carry/UG of later layers.

### m2 — No cancel checks inside the per-front classification and tension

`SmartSupportsTension.cpp:494-615` (`classify_front`), `:136-256` (`wide_tension`).

- `classify_front` samples up to `grid_max_pixels` (4 M) interior points plus the boundary. In grid mode each
  `reach.value()` is an AABB radius query (allocating a vector) plus 16 cells.
- `wide_tension` handles up to 20 000 scanlines × 8 θ.
- Neither has a `cancel` parameter. The latency is a whole front, e.g. a 200 × 200 mm bridge at 0.1 mm, run once per
  trial evaluation of its cluster.

Plan §2.3 promises checks "per cluster, every 64k Dijkstra pops"; these loops sit between those checks. **Fix:** pass
`ThrowOnCancel` and check it every N rows / scanlines.

### m3 — `wide_tension` runs a full Clipper intersection against the whole C′ for every long segment

`SmartSupportsTension.cpp:226-228`. `! intersection(Polygons{ rect }, cp).empty()` is evaluated for each scanline
segment with level May/Should/Must, against the complete `cp`, with no bbox clipping.

**Cost.** O(segments × |C′|). A 100 mm span over a C′ with a detailed outline gives ~444 lines × up to 8 θ ≈ 3.5 k
Clipper runs. It repeats on every re-evaluation of the cluster: each long-span trial changes its `Fc`.

**Fix.** A bbox prefilter, plus `ClipperUtils::clip_clipper_polygons_with_subject_bbox(cp, rect bbox)` before the
test (or a point-in-C′ test of the segment midpoint).

### m4 — Grid memory is retained beyond the plan's budget

`SmartSupportsReach.cpp:338-341, 412-439`; `SmartSupportsEval.cpp:217-219, 890-897`.

**Known.** `res` is clamped to ≤ `grid_res_max`, so a D component larger than 200 × 200 mm exceeds
`grid_max_pixels`: 300 × 300 mm → 9 M cells, about 117 MB transient in `build_grid` + `CellHeap`, per cluster in
parallel. This much is documented in wp-log-wp2b1.

**Not covered by plan §3.13** ("freed after the layer"):
- the full `v` (36 MB at 9 M cells) is held by `ClusterSlot::cur` *and* `prev` until `commit`;
- a front-bbox crop of nearly the same size stays in the carry for every layer of the H_s window. That is H_s/h
  layers: 5 at the defaults, 100 at the GUI limits H_s 5 mm / 0.05 mm layers.

It only bites on large flat grid-mode fronts on consecutive layers. **Fix:** drop `prev` fields after accept/restore;
crop the carry field to the part within R of ∂C (only boundary-near q are ever queried); or enforce `grid_max_pixels`
by raising `res`.

## Checked without findings (summary)

- **Threading.**
  - `tbb::parallel_invoke` + `call_once` + `this_task_arena::isolate`: no deadlock. Group cancellation leaves a
    partial `coll`, but it is discarded with the run.
  - Avoidance chain: one writer. Readers only read `avoid[k ≤ ready]` (acquire/release). `advance` releases the lock
    around `cancel()`; `start_prefetch` joins before reassigning the `std::thread`.
  - `verify()`'s parallel `predict_F` only hits layers ≤ the ensured layer.
  - Cluster tasks call only const `cover`/`c_hat`/`value`; the memo is per cluster slot.
  - Exceptions in TBB bodies propagate and the result is discarded.
- **Determinism.** Indexed slots everywhere. The memo is never iterated, and its values are computed at the snapped
  key. Orders are canonical (stable front sort, cluster tie-break by front index, `std::map` in θ and tails).
  Reductions are max-only.
- **Cluster cache.** `touched` (interface 0) plus the exact `Fc` comparison, and restore with the same `touched`,
  leave no rejected-trial state behind.
- **`build_input` on the GUI thread.**
  - After posSlice the slicer only writes `lslices_ex[].islands` (a separate member), and posSlice can only be
    invalidated by `Print::apply` on the GUI thread.
  - Instance / PrintObject matching follows `set_task_impl`.
  - `po.trafo_centered()` includes shrinkage and z (sinking); mirroring is handled by the projection.
  - `raft_support_layer_count` and the TreeSupportSettings mirrors match.
- **Reachability mirror.** Collision, avoidance steps (the zero-step case at tree angle 0 matches the slicer), outset
  loop, roof decision and buildplate-only: `roofs ∩ avoid = ∅` because forbid = avoid.
- **Units/frames.** All mm/scaled conversions in Base, Reach, Tension, Eval and Plan are consistent. The slab is
  `[slice_z ± h/2]`. `tip_layer` = n + raft − (z_top + 1).
