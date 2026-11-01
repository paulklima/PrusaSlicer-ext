# Work package log — WP2c1a (cluster evaluator, evaluate_pass)

Scope (plan §8.2): `LayerEvaluator` and `evaluate_pass` in `src/libslic3r/Support/SmartSupportsEval.cpp` (plan §3.8,
theory §2.2–§2.5, §3.1) and `tests/fff_print/test_smart_supports_eval.cpp` (`[sas-eval]`). No header changed.

## Contract (what WP2c1b can rely on)

### `evaluate(n, E, overrides, touched)`
- Precondition: layers `1 … n − 1` committed (`n = last committed + 1`, asserted in debug builds) and
  `reach.ensure(tip_layer(n))` called (otherwise `contact()` advances the chain itself, blocking, without cancel).
  `n = 0` or `n ≥ layers` → empty needs.
- `contact = reach.contact(E, n)` once per call (cached for the last two `E` of the layer, exact equality);
  `F = contact.all`, `F⁺ = cover(F)`. Nothing is computed for layers without clusters, tails, ungrounded or tolerated
  material (fast path; `E` is then ignored).
- **Clusters**: results are cached per cluster for the last two contacts of the layer, keyed by `F` cropped to the
  cluster window ⊕ (G + 20 µm). A cluster is recomputed only if that crop changed. `touched` is only a prefilter
  (clusters whose window ⊕ (G + 20 µm) misses it are reused unseen); `nullopt` is always safe. The planner's
  "restore" call (`evaluate(n, E, overrides, touched)` after a rejected trial) is served from the cache (measured
  0.05 ms on the T-shape). The ĉ memo per cluster lives until `commit(n)`.
- `overrides` are applied on every call (they never touch the cluster cache).
- Returned reference: valid until the next `evaluate()` / `commit()`.

### `LayerNeeds`
- `need[Island][Must]` = C' of the non-grounded fronts + tail override; `need[Cantilever][Should|Must]`,
  `need[LongSpan][May|Should|Must]` from `classify_front`. Overrides may move pieces to another cause at the same
  level (so `need[Island][Should]` etc. can occur after an override).
- Order of the layer steps: cluster needs → **cause overrides** → **tail override** (removed from all classes, added to
  Island/Must; tails therefore stay island needs even inside an override region) → **noise filter** (opening ε/2 of
  the union, components < A_min dropped, every class intersected with the result) → **folding** → `why` → stats.
- `folded`: `(tree, area)` sorted by tree id; an island-need component is folded into the unplaced tree whose source
  region (C' of its non-grounded fronts, its tail zones) covers most of it, if that is ≥ 50 % of the component. Folded
  pieces are removed from `need` (so `need − folded` in §3.9 is a no-op).
- `why`: at most one entry. `!tips_possible(n)` → all needs, `NoTipLayer`. Else, if `contact.roof_gap`:
  `needs ∩ cover(tip_path_all) − cover(all)` → `InterfaceGap`. **The why belongs to the `E` of that call**: with
  `E` = manual paint only there is usually no roof, so take the InterfaceGap pieces from the trial evaluation `L'`.
- `cprime` = C' of all fronts (grounded or not); `cuts` = all tension cuts of the grounded fronts, cluster then front
  order; `stats.max_c` = max over `classify_front` (incl. its early exit); `stats.need_area` after filter and folding.

### Grounding (theory §2.2)
- A front is grounded iff `compute_anchors` returns a material or support anchor, or C' = ∅ and C ∩ F⁺ ≠ ∅ (fully
  supported). Non-grounded fronts with C' ≠ ∅ are island needs.
- GP = (band − ∪C − ungrounded S − T − unprintable) ∪ (band ∩ F⁺ − T).
- **Ungrounded S** (once per layer, plan formula): `((L − U) ∩ W) − ((L_{n−1} − UG_{n−1}) ⊕ (w + 2 µm))` with
  `W = bbox(UG_{n−1}) ⊕ (w + 1 mm)`, plus `(L − U)` of the weak-contact slices. The 2 µm absorb the 1 µm arc tolerance of
  the S_n offset (no slivers along ∂S_n). Fast path: `UG_{n−1} = ∅` and no bad slice.
- **UG_n** (`commit`) = (ungrounded S ∪ C of non-grounded fronts ∪ tolerated slices) − F⁺.
- **Tags** (needed for folding, not specified by the plan): every UG piece carries an island tree id — bad slices and
  fronts in tree slices: `slice_tree`; ungrounded S: the lowest tree of the UG_{n−1} pieces within w + 4 µm; fronts in
  bed slices: the lowest tree of the touching ungrounded S. So material that hangs from an unplaced island above its
  merge layer folds into that island too.

### Reach (theory §2.3)
- Domain D = band − GP − F⁺; one `ReachField` per D component that holds C' of a grounded front not fully covered by
  tension or hoop (§3.8 early exit: no field otherwise). C' pieces are assigned to components by bbox +
  intersection, anchor polylines to the C' piece with the nearest boundary.
- Seeds: material anchors resampled (vertices + ≤ 0.1 mm), ĉ = `carry.c_hat(snap_1µm(p), n, z_top, h, w)` from the
  cluster memo (misses deduplicated, evaluated in parallel from 512 on, inserted serially), zero/non-zero neighbours
  split in the middle into constant halves; support anchors: segments with ĉ = 0. Δ = `w_β − w` (mm).
- `classify_front` per (front, D component) group with `max_chat` = max seed ĉ of that front in that component, only
  if the group is one ExPolygon without holes and has seeds (else +∞, WP2b2 note).
- Carry (`commit`): every front of every cluster, `grounded`, `field->cropped(bbox(C))` (a small composite query if a
  front's C' lies in several D components), `zero = covered_all ∪ hoop ∪ (C ∩ F⁺)`; `boundary` = nullptr (unused by
  the carry). Layer 0 is pushed by the constructor; every committed layer is pushed (also without fronts).

### `evaluate_pass`
For n = 1 … `last_layer_with_fronts`: `ensure(tip_layer(n))`, `evaluate(n, E[n], {}, nullopt)`, `commit(n)`; layers above
have no fronts or tails and stay empty. `need`, `folded`, `why`, `stats` are copied, `cprime`/`cuts` left empty.
Progress 0 … 100, monotone, from the calling thread. Cancel per layer and inside (clusters, fields, memo).

## Deviations from the plan (exact)

1. **Cluster cache by contact crop** (§3.8 says "only touched clusters are re-evaluated"): `touched` is a prefilter;
   the decision is the exact comparison of the cropped contact; two results per cluster are kept (restore is free).
2. **Contact cache**: the last two `E` per layer.
3. **Band recomputation** for clusters whose Phase A band misses fronts (WP2a bug below): `L ∩ ((∪C ⊕ 0.01·m) ⊕ 0.99·m)`.
4. **Tail override after the cause overrides** (§3.8 lists it before): tail points stay island musts.
5. **Folding** component-wise with island-tree tags on UG (mechanism not in the plan).
6. **evaluate_pass** stops at `last_layer_with_fronts`.
7. Ungrounded-S tolerance 2 µm (above).

## Bugs and findings in other modules / the plan (for the orchestrator)

1. **WP2a, `SmartSupportsBase.cpp`, `build_layer_geometry`**: `cl.band` and `cl.band_prev` are **empty** for fronts
   thinner than ≈ 0.005·m (m = 2.5 mm + w → ≈ 14 µm). `ClipperOffset::ShortestEdgeLength = |δ|·ClipperOffsetShortestEdgeFactor`
   (ClipperUtils.cpp:321, 483) merges the end vertices of the thin strip before offsetting, the strip degenerates and
   `offset(Cs, m)` is empty. Symptom (without the WP2c1a workaround): every front of a 40° wall with auto α (w 0.225,
   fronts 13 µm) and of any slope within ≈ 2° of α (44° wall at α 45: 7 µm) has no GP → island need on every layer.
   Proposed fix: offset in two steps, `offset(offset(Cs, 0.01·m), 0.99·m)` (same for band_prev and the hoop `S_on`).
   WP2c1a recomputes such bands itself; band_prev and the hoop test for such thin fronts stay affected (hoop credit
   lost on rings < 14 µm wide — negligible, their c increments are < 0.03 per mm).
   The same Clipper effect in **WP2r `f_grown`** (`offset(E ∩ O_n, tip_d)`): fronts thinner than 0.005·0.8 = 4 µm get
   no F_grown (slopes within ≈ 0.6° of α; negligible, same fix possible).
2. **`ClipperUtils::clip_clipper_polygons_with_subject_bbox` is not a crop** (keeps vertices outside the box); fine as a
   clip operand, wrong as a subject. WP2c1a uses its own exact `crop()` for subjects (this was a bug in an
   intermediate WP2c1a version). Other modules use it as an operand only (checked: Reachability, Tension, Base).
3. **Reference expectations at h 0.2** (plan §7.2, for WP2c1b / WP5):
   - **α**: `UserParams::alpha_deg` defaults to 0 = auto (w = 0.5·0.45 = 0.225 mm), while theory §10 assumes
     α = 45° (w = 0.2). `run_sas()` passes `UserParams()` → reference tests must set `up.alpha_deg = 45`
     (with auto α the 44° wall has no fronts at all and the 40° wall accumulates only 0.013 per layer).
   - **#9 Ø20**: the sliced maximum is c(x_last) = 2.83 at the last open layer (half chord 1.41 mm); the closing
     layer is a 2.4 mm bridge (covered). So **no need** at 0.2 mm layers (theory's 4.1 / Ø8's 1.7 are the limits
     x → 0); at 0.1 mm layers c(1.0) = 3.19 → should. Ø50 gives must (max c 8.29).
   - **#7**: first need at slice_z 19.1 (theory 18.98), hole r 5.93: r jumps 6.54 → 5.93 between the layers, so the
     plan's "hole r 6.3 ± 0.3" needs ± 0.4.
   - **Cone apex** (#26): the noise filter drops the apex slice and the first tail ring of a 60° cone (0.01 and
     0.08 mm² < A_min); the tail needs start at apex + 0.4. Paint prisms (k_d = 1) reach the layer below.
4. **WP2b1 perf caveat (DomainEdges uses `LinesDistancer<Line>`)**: not a hotspot here — `domain_edges` ≤ 0.5 ms,
   `tension_levels` ≤ 19 ms (dome ring, 6 θ) on the largest wide fronts of the benchmark shapes.

## Notes for WP2c1b (plan loop) and WP5

- Call `ensure(tip_layer(n))` before the first `evaluate(n)`; copy `LayerNeeds` before the next call.
- `touched` may be `nullopt` (correct, slightly more work); with `touched` the cache also covers restores.
- `mark_tree_unplaced(t)` affects every later `evaluate()`, also of the current layer. For the tree of a residual
  island need use `base.layers[n].slice_tree` of the slice containing it (the tree's first layer is a tree slice).
- `why` is per call (see above); `NoTipLayer` layers are 1 … z_top + 1 without a raft.
- Island needs of a fully unsupported tree continue on every layer with fronts above it (UG propagates; lateral
  grounding spreads by w per layer next to grounded material) until folded.
- Timings (sandbox, 8 cores, `[.benchmark][sas-eval-bench]`): evaluate_pass figurine_proxy 480 layers 0.19 s, sphere
  R 20 0.35 s (1 thread 0.38 s), dome R 20/25 0.12 s (B_ceil 0: 0.29 s), hole Ø50 0.11 s, 40° wall 0.24 s, T on
  80 × 80 0.017 s; Phase A 0.0005–0.04 s for these.

## Tests (`tests/fff_print/test_smart_supports_eval.cpp`, `[SmartSupports][sas-eval]`, α = 45 unless stated)

| Test case | Checks |
|---|---|
| #1 shelf 2 mm | no needs, max c 1.8 ± 0.1; 4 mm shelf: should band 0.8 × 15 ± 10 %, 3.2 mm from the cube (± 0.1), no must |
| #5 bridge 30 | no needs; B_max 20: long span may (29.6 − 6) × 10 ± 10 %, no cantilever, cuts t 29.6 / [9.6, 20] |
| #14 walls | 44°: none, max c 1.78 ± 10 % (exercises the thin-front band fix: fronts 7 µm); 40° with H_s 0: none, c 0.038 |
| #13 wall 40° | first need print_z 16.0 (15.8 ± 1.6, > 14), should first, must at 23.5 ± 2, a need on every front layer above |
| cone tail | 60° inverted cone: island needs up to apex + 2.5 (2.6 ± 0.3), with the tail painted 0 mm² (≤ 5 %) |
| islands / folding | floating sphere: first slice = island; marked unplaced → island needs = 0, folded = unmarked areas (1e-6) |
| chains | stairs 0.4: need from step 2 (c 4.14, should), step 3 must; 1.2: none; holes Ø8 / Ø20: none, max c = c(x_last); dome: none; B_ceil 0: first need z 19.1, c = R√2 − r − z |
| ungrounded material | slab on a grounded box: none; on a floating box: gap cantilever from the post (should 15, must 51 mm²), box bottom island 100 mm² |
| touched clusters | T-shape (2 clusters): partial re-evaluation = full one (exact), restore = original, untouched arm identical |
| cause overrides | shelf needs move to LongSpan at the same level; an override elsewhere changes nothing |
| known reasons | NoTipLayer on layer 1; 40° wall painted at a need layer: interface 0 → needs 0, interface 2 → needs stay, InterfaceGap ≥ 90 % |
| determinism, cancel | sphere R 20: 1 thread = default (exact), max c 6.4 ± 15 %, needs between r 5.9 and 13.6 (5.8 / 14.1 ± 0.6); cancel → CanceledException; progress monotone to 100 |
| timings (hidden) | `[.benchmark][sas-eval-bench]` |

## Results (sandbox, dev/2.9.6 merged in by `wp-build.sh`)

- `fff_print_tests "[sas-eval]"`: 12 test cases / 903 assertions passed, ≈ 3.6 s total (slowest: determinism 1.2 s,
  dome section 0.5 s, InterfaceGap 0.34 s, #13 0.24 s, #14 0.22 s).
- `fff_print_tests "[SmartSupports]"`: 38 test cases / 7121 assertions passed;
  `libslic3r_tests "[SmartSupports]"`: 54 test cases / 25837 assertions passed.
- No warnings in the WP2c1a files.
