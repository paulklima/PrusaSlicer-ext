# Plan review 4: performance, numerics, determinism, testability

Reviewer #4, 2026-10-15. Reviewed: `notes/plan.md` (draft of 2026-10-15) against the 2.9.6 code.

**Verdict: not ready for WP0.** As specified, Phase B misses the 30 s budget by 3–15×. The cause
is serial work on whole layers, repeated for every evaluation and every front, and the WP0
interface freeze would lock it in. Two biases accumulate along chains, the prism predicate is not
sound, and several tests cannot fail for the bugs they are meant to catch.

Basis of the estimates (Release build):
- One Clipper operation on a whole figurine layer (5–10k vertices): 2–5 ms.
- The same on one front (≤ 500 vertices): 0.05–0.2 ms.
- One distancer query: 1–3 µs.
- Figurine: 1000 layers, about 50 fronts and 300 mm of anchor per layer.

## Blocker

### B1: The cost structure of Phase B (§3.2 signatures, §3.8–§3.11)

**Problem.**
- Every `evaluate_layer` recomputes 5–6 operations on the whole layer, serially: `offset(G_{n−1}, w_n)`,
  `∩ slices`, `∪ F`, `− T`, `∩ P`, `D = diff_ex(L, GP ∪ F)`. That is about 20 ms.
- The frozen per-front functions take whole-layer polygons:
  - `compute_anchors` expands `GP` and `diff(L, f.C) ∪ F`;
  - `bridge_directions` expands `Lp`;
  - the hoop test clips against `expand(S)`;
  - `tension_levels` probes endpoints with an O(V) `contains`.
- Taken literally, that is about 0.5 s per layer.

**Estimate even with bbox clipping.**
- One pass ≈ 1000 × (25 ms + 20–60 ms for ĉ and classification) = 45–85 s, serial.
- Re-evaluations multiply this by about 1.7. Tails alone put a placement on every layer of each
  finger or strand.
- D13 adds one more pass. Total: 120–250 s.

**Fix** (in the WP0 headers):
1. **Phase A (parallel) precomputes:** `S`, `P`, `S ∩ P`, `opening(O, 0.1·ext_w)`,
   `expand(Lp, ε)`, and LinesDistancers of the outline and of the grounded boundary.
2. **Fast path:**
   - if every slice of layer n−1 within `bbox ⊕ w` is fully grounded, then `GS_i = S ∩ slice_i`;
   - if the whole layer is grounded, `G_n = L_n` (a flag);
   - the result differs from the generic path only by nm rounding.
3. **A `LayerContext`** (precomputed data plus a spatial index of the fronts) replaces the raw
   layer parameters. Every per-front operation runs on
   `clip_clipper_polygons_with_subject_bbox(·, bbox(C) ⊕ (2 mm + ρ))`; `D` is computed per component.
4. **θ only for wide fronts.**
5. **Cache per D component.** After a placement, re-evaluate only the components that the new F
   touches. The result is identical.
6. **Verification by P4:**
   - compute `F^plan_n − F^act_n` in parallel;
   - layers below the first deficit ≥ `A_min` reuse the plan state;
   - above it, re-evaluate only components that are dirty, through an F deficit or a hang-from link.
7. **Make the risk-table early exits design rules:**
   - hoop-covered or fully tension-covered fronts skip `c`;
   - skip classification if `max ĉ + perimeter(C')/2 + width ≤ K_max`. The "diameter" bound in §9
     is wrong for non-convex fronts.
8. **Pipeline the serial avoidance chain with Phase B** (layer n needs only `avoid_{t(n)}`). Stop
   it at the highest tip layer that has fronts.

**Expected after the fix:**
- Phase A about 5 s.
- Avoidance hidden behind Phase A.
- Phase B 8–15 s.
- Paint 1–3 s.
- 2000 layers take about twice as long.

## Major

### M1: Volume of ĉ samples (§3.8)

**Problem.**
- Sample spacing `min(0.2 mm, res)` gives 0.05 mm on every front with a bbox under 20 mm, about
  6000 samples per layer.
- Each sample scans ⌈H_s/h⌉ layers of carry fronts linearly (11 by default, 101 at H_s = 5 and
  h = 0.05), plus 3 queries: about 7 µs per sample, so about 40 ms per evaluation. Classification
  costs the same again.
- Sphere-like layers have one component, so they do not parallelise.
- "res" is undefined in Direct mode.

**Fix.**
- Spacing 0.1 mm, plus every anchor vertex.
- An R-tree per carry layer.
- `parallel_for` over sample chunks, writing into indexed slots.
- Cache the ĉ of material anchors across the plan iterations of a layer.

### M2: Biases that accumulate along chains (§3.5, §3.8, D11)

**a) Miter corners make phantom zero anchors.**
- `S` uses the default jtMiter. At a convex corner of `L_{n−1}` with exterior angle φ, the anchor
  lies up to `w/cos(φ/2)` from C, but the reach is only `R = w + δ`.
- At 0.2 mm / 45° that is 0.283 against 0.245 mm at every corner with φ > 70°. The ~0.06 mm next
  to each miter vertex (itself always a sample) hangs from nothing, so ĉ = 0.
- The zero spreads sideways every layer: inverted frustums lose `c` within about `K` of each corner,
  exactly where the slope is flattest.
- Fix: S offset with jtRound (ArcTolerance ≤ 1 µm), or enlarge R by the miter excess.

**b) The segment minimum is not exact.**
- `ĉ_s(proj) + |p − proj|` ignores a gradient `g` of ĉ along the segment. The true minimum is
  `ĉ(proj) + d·√(1−g²)`.
- The overestimate per layer is up to half the increment for `g ≈ 0.9`: next to zones reset by
  support, and along bands fed from one end.
- Fix: the closed-form minimiser `s* = −sign(g)·d·|g|/√(1−g²)`, clamped to the segment.

**c) Grid mode.** q points of lower fronts in grid mode are farther than ρ from the seeds:
`min(D(v)+|q−v|)` overestimates by about 0.1–0.3·res per layer. The effect is small (short chains).

**d) The stated bound.**
- The 16-neighbour worst case is 2.75 % (`1/cos(½·atan ½) − 1`), not 2.7 %, and only in free space.
- Knight moves must check both cells they cross.
- The exact Euclidean zone shortcuts slots narrower than ρ.

**H1 test.** The 2 % tolerance is realistic (an exact Direct mode stays below 0.1 %), but straight
uniform bands cannot expose a)–c).

### M3: Robustness of the raster and Direct mode (§3.8)

**Direct-mode test.** `offset(seeds, ρ)` gets the polyline defaults, jtSquare and etOpenButt
(`ClipperUtils.hpp:51, 59`).
- Butt ends push nearly every front with free ends into Grid mode, about 200k pixels for each
  sphere-ring layer.
- Square joins reach about 8 % beyond ρ, where `all_lines_in_radius(ρ)` finds nothing: +∞, a false
  must-need.
- Fix: `jtRound`/`etOpenRound` at ρ − 1 µm with an explicit ArcTolerance; query at ρ + 1 µm; fall
  back to `distance_from_lines`.

**Pixel centres at res ≤ 0.25 mm** drop thin necks and most of the unprintable medium `L − P`
(< 0.22 mm wide), which the theory lets `c` cross. Components fall apart and give ∞. Fix:
- conservative rasterisation (a pixel is active if it intersects D);
- never return ∞ for a point of a grounded C' without a fallback search;
- shift the grid origin off round coordinates. Otherwise staircase edges hit the mesh vertices of
  CAD test meshes, and `outside()` returns 0 on the boundary.

### M4: The prism predicate is not sound; painting cost (§4, D17)

**Problem.** The fully-inside rule (vertices inside, no projected edge crosses any ∂R_k in the
z range, centroid inside) is:
- **insufficient:** slabs with empty `R_k` have no boundary to cross, and holes inside the triangle
  cross no edge. Long cone or cylinder flanks from `revolve` (apex in a tail band, rim in a ring
  band) get painted whole.
- **too strict:** parts of an edge outside slab k are tested against `R_k`, so multi-slab zones split
  down to `e` everywhere.

**Fix.**
- Clip the triangle to each slab band; require `R_k ≠ ∅` and the clipped part ⊂ `R_k`.
- Horizontal facets use the slab of `slice_mesh_slabs` (downward → the slab above,
  `TriangleMeshSlicer.hpp:74-75`).
- A z outside all slabs counts as outside, and so does `outside() == 0`.
- Mirrored volumes: seed normals are `M^{-T}n`, or use `sign(det)` (`TriangleMeshSlicer.cpp:2271`).

**Cost.** `ExPolygon::contains` is O(V) for each call on staircase polygons, and each vertex is
tested about 6 times. Use `LinesDistancer::outside()` per slab and a memo per vertex index.

**Dilation.** ±1 layer misses leaves whose z extent `e·sinθ` exceeds `h` (θ > 24° at 0.1 mm).
Dilate by `⌈e·sin α/h⌉` layers.

**Size (item 2), 100×100 mm painted:**
- **horizontal:** 10–20k triangles, about 1 MB in memory, 12 kB bitstream, 50 kB of 3MF text;
- **sloped across many slabs (current rule):** about 500k nodes, 25 MB, 300 kB per undo snapshot,
  1.2 MB of 3MF text.

### M5: Memory on large models (§3.2, §3.11)

**Estimate.**
- At 2000 layers with 5–20k points, one ExPolygons copy is 80–320 MB.
- The plan keeps 7–9 copies (input L, `L P S U O`, `plate_ok`, `raw`, `model_short`): 0.7–2.9 GB
  at peak.
- The carry at H_s = 5, h = 0.05 (101 layers × 50 fronts × 200 kB) is about 1 GB.
- Grids reach 666k pixels, not "≤ 160k".

**Fix.**
- LayerGeom references the input L.
- Store `L − P` instead of `P`.
- Store `avoid_t` once and derive the reachability sets lazily, clipped to each zone.
- Free the carry outside the H_s window.

### M6: No determinism guarantee (§2.3, test #20)

**Problem.** Clipper's output order follows its input order, and anchor resampling starts at the
first point of each polyline. A differently ordered union therefore shifts every sample.

**Rules to state in the plan:**
- Parallel results go into indexed slots and are combined serially in index order.
- Forbidden: `concurrent_vector`, appending under a mutex, `union_parallel_reduce`
  (`ClipperUtils.cpp:1031`), iterating unordered containers, parallel float sums.
- Sorts need total keys:
  - bridge candidates (cost, angle), deduplicated modulo 180°: `n` and `−n` tie and would take two
    of the four slots;
  - report entries (level, area, layer, x, y, id).

**Test #20:** compare the paint byte by byte, and run once with
`tbb::global_control(max_allowed_parallelism, 1)` and once with the default.

### M7: Tests that cannot fail, brittle tests, gaps (§7)

**Cannot fail:**
- "Exact straight anchor" and "geodesic L" use axis-aligned or diagonal paths; an 8-neighbour grid
  passes them. Add a rectangle rotated by 13.28° (≤ 2.75 %) and a path at 22.5° (8-neighbour: 8.2 %).
- The 45° wedge test checks only an upper bound, so painting nothing passes. Add a lower bound:
  `predict_F(project(paint))_k ⊇ planned F_k` minus an `e` band, at 0.2 and 0.1 mm.

**Brittle:**
- #18 ±1 %: the second pass reuses the first pass's splits, so the leaves differ by
  O(e × boundary length), which is ≫ 1 % on 1.2 mm rings. Compare the plans exactly and F within `e`.
- #11 depends on D10: the r 5.8 ring is about 0.3 mm outside the avoidance. Test H1 with
  reachability forced to "everywhere".
- #9 has an 8 % margin.
- #7 "≈ 14" has no tolerance.

**Missing tests:**
- cancel in each phase;
- P4 (a manual enforcer never adds needs);
- 5 re-runs without split growth;
- empty input, one layer, a grounded cube;
- volume matrices; mirrored, scaled and rotated instances; a sinking object;
- first layer 0.3 with layer height 0.15; a raft; 0.1 mm layers;
- an inverted frustum (M2a);
- ε at 9 vs 12 µm;
- `RemainingAfterPaint`, using a hook that drops one placement.

**Runtime.** `fff_print_tests` takes 28 s today. Spheres (7 runs × 3–5 s), domes R60 (2 runs ×
10–20 s), organic `process()` and TreeModelVolumes add 60–120 s. Fix: cache the sliced prints,
turn `full_verification` off where it is not tested, use a dome R30, tag `[slow]`.

### M8: Cancel and exceptions (§2.3, §6.2)

- **Error popup on every cancel.** `CanceledException` is a `std::exception` (`PrintBase.hpp:23`);
  `PlaterJob::finalize` shows "An unexpected error occured" for any eptr still set
  (`PlaterWorker.hpp:86-90`). Every cancel, including the `cancel_all()` on object removal, would
  pop an error. Clear the eptr for `CanceledException` in `finalize`.
- **No cancel checks** in `select_patch` (a private selector, so throwing out of it is safe) or the
  Dijkstra (check every 64k pops). Pass `throw_on_cancel` to `slice_mesh_slabs`.
- **Exceptions in parallel code.** Use `tbb::parallel_invoke` instead of a bare `task_group`.
- **Progress** from an atomic layer counter, monotone, and throttled.

## Minor

- **m1 jtRound without an ArcTolerance** passes 3 nm (`ClipperUtils.cpp:318, 480`), about 500
  vertices per circle. Pass `gcode_resolution` as `TreeModelVolumes.cpp:694` does.
- **m2 Hoop matching by "first vertex"** is fragile, because Clipper starts loops at an arbitrary
  vertex. Match by bbox and area.
- **m3 Direct mode samples only the boundary** (2·res squares). Fronts up to 0.5 mm wide get no
  samples in the middle.
- **m4 Linear ĉ interpolation** overestimates at V-shaped kinks by up to half the spacing.
- **m5 Two endpoint classifiers** (2 µm anchors, 4 µm probe) can disagree at junctions. Reuse the
  `compute_anchors` polylines for both. The 2 µm itself is enough: the deviations are ≤ 0.6 µm.
- **m6 Variable layer height (other styles):** the tip-layer and max_move arithmetic assumes a
  constant layer height. Use `h_k`, or add a contract issue.
- **m7 3MF strings are built by prepending** (`Model.cpp:1637`), which is O(L²). Make the new
  `triangle_as_string` linear.
- **m8 The bridge cost loop** is O(E·D). That is acceptable for wide fronts only (B1.4).
- **m9 `build_input`** copies up to 320 MB on the GUI thread. Show a busy cursor and measure.
- **m10 Correct in §3.8/§3.11:** "≤ 160k pixels", "≤ 2.7 %", "max ĉ + diameter".
