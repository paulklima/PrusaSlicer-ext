# Work package log — WP2a (Phase A + A2)

Scope (plan §8.2): `build_base_model` (Phase A §3.5, Phase A2 §3.6), `feret_diameter`, `bridge_directions` in
`src/libslic3r/Support/SmartSupportsBase.cpp`; tests `tests/libslic3r/test_smart_supports_geometry.cpp`
(`[sas-geometry]`) and `tests/fff_print/test_smart_supports_islands.cpp` (`[sas-islands]`). No header changes.

## Deviations from the plan (exact)

1. **`bridge_directions` keeps tied directions at least π / max_dirs apart** (22.5° for 8), picked greedily in
   `(cost, angle mod π)` order. The plan says "deduplicated modulo 180°, sorted, at most 8". Without the
   separation, the short arc / rounding edges that the opening of `B_C` creates next to every true candidate tie
   within 10 % (e.g. 2° next to 0°), and a round front returns 8 nearly identical directions instead of sampling
   the half circle. Exact rectangle ties (10 × 20 shelf: 0° and 90°) are unaffected.
2. **Tails per branch** (§3.6 says "per connected component of its slices", `while n < merge`): a branch starts
   at a root slice (a tree slice with no tree slice below it) and continues upwards through the linked tree slices
   whose lower neighbour was in the tail. It stops when the slice is stiff (`!shrink_ex(part, W_stiff/2).empty()`,
   also checked on the root: wide islands get no tail, their footprint is the island need), when
   `slice_z − root_z > H_tail`, or when the slice is grounded (per branch, not the tree's merge layer). Where
   branches join, the latest root z counts (longer remaining tail). For single-root trees this is exactly the plan's
   loop; for merged sibling islands every island gets its own tail (theory §3.1, test "Sibling tails").
3. **"tn empty"** (§3.6) means `opening_ex(tn, eps/2)` is empty: slicing slivers of vertical faces (collinear
   vertices rounded to nm) do not count as downward facets. Such a layer adds no `tail_needs` entry and sets
   `tail_without_downward`. Real thin downward rings (75° cone near the apex, ≈ 0.05 mm wide) survive.

## Decisions on ambiguities

- `U_n = diff_ex(L_n, offset(L_{n−1}, w_n, jtRound, arc 1 µm))`; `S_n` is not stored. `w_n = 0` → `L_n − L_{n−1}`.
- `unprintable = diff_ex(L, opening_ex(L, printable_width_factor·ext_width/2), ApplySafetyOffset::Yes)`:
  miter opening (default join) keeps right-angle corners (no corner slivers); the 10 nm safety offset removes
  rounding slivers along the outline. `ext_width` = `Params::ext_width` (max over regions). Layer 0 included.
- `wide` = `!shrink_ex({C}, ext_width).empty()` (≡ the opening is not empty).
- Front order: stable sort by `(bbox.min.y, bbox.min.x, area_mm2)`, Clipper's output order breaks ties;
  `FrontGeom::bbox` = bbox of the contour.
- Clusters: window `bbox(C) ⊕ (m + w)`, `m = cluster_margin + rho_exact + w`; windows are merged while they overlap
  **or touch**, so the final windows are strictly disjoint. Clusters sorted by `(win.min.y, win.min.x, first front)`,
  fronts ascending inside.
- Bands: `offset(∪C, m | m + w, jtRound, arc 5 µm)` ∩ (L | L_{n−1} clipped to the window). Clipper puts arc vertices
  on the circle, so a band is up to 5 µm smaller than the exact `∪C ⊕ m` (tests use a 10 µm tolerance).
- `B_C` = the components of `opening_ex(band − band_prev, 0.1·ext_width)` that intersect C; if none survives,
  `{C}`. So every wide front has ≥ 1 θ; thin fronts have none.
- Custom bridge angle (`custom_bridge_angle_deg > 0`): θ = `(cos a, sin a)`, the line direction as in
  `LayerRegion.cpp` (`bridge_angle = deg2rad(custom)`; auto: `π + atan2(dir)`).
- `bridge_directions(B, lower)`: floating edges = outline of `B − lower` outside `expand(lower, 2 µm)` (the slicer
  uses `SCALED_EPSILON`; `B_C` comes from an opening, hence 2 µm). Normals are keyed by `ceil(1000·angle mod π)`,
  edges of one key are summed (cost = Σ length·|u·n|, O(keys²)); ties: `cost ≤ min·(1 + tie_tol) + 1 nm`. No
  floating edge → the minor principal axis (`compute_principal_components().second`, as the slicer). All
  directions canonical: unit, angle in [0, π).
- Hoop (D26): checked for every front (thin or wide) with exactly one hole; `S_band = band − ∪C(cluster)`,
  "lies on" = inside `expand(S_band, 2 µm)`; `|H|` and the contour fraction are **lengths**; `hoop_hole = 0`
  (the only hole); `hoop_feret_mm` = Feret of the matching **slice** hole.
- Weak contact: `extent` = max side of the bbox of `slice ∩ L_{n−1}` (all overlap pieces together). A slice without
  any overlap below is weak too (`slice_bad = 1`), so "no bad slice" (Phase B fast path) implies every slice
  touches the layer below. Layer 0: never.
- `FrontGeom::slices`: slices whose bbox contains the front's bbox; overlap test only for nested slices.
- `manual_enf / manual_blk = union_ex(project_states(own paint, state == ENFORCER / BLOCKER)[n],
  slice_modifiers(enforcers / blockers)[n])`; not clipped to L. Two `project_states` calls.
- Island trees: `bed` as §3.6; union-find over non-bed slices along below links; tree ids in the order of their
  first slice (bottom up, slice index); `merge` = min n of a bed slice with a below link into the tree;
  `volume` = Σ area·h of the tree slices below `merge` (all if never); `tolerated` compares `slice_z(merge) −
  slice_z(first) ≤ H_merge`. `T_n` = all slices of tolerated trees at n (copied; slices are disjoint).
- `tail_needs[n]`: one entry per tree, ascending tree id; pieces of several branches appended; raw (no noise
  filter); at a root it is the whole slice. `tail_last` = last layer where the tail zone was active (also with empty
  tn); `tail_last = 0` means "no tail" (trees never start at layer 0).
- `last_layer_with_fronts` = max n with fronts, tail needs or manual enforcers (0 if none).

## Threading, cancel, progress (for WP2c1b)

- Per-layer Phase A runs in one `tbb::parallel_for`; each layer writes only its own `LayerGeom`. `cancel()` is called
  per layer **from the worker threads** (it must be thread-safe, plan §2.3) and every 64 layers in Phase A2;
  a `CanceledException` thrown in a task propagates out of `build_base_model`.
- `progress(p)`: p in 0..100 **local to Phase A** (0 start, 10 after the manual zones, 10–90 layers, 100 at the end),
  monotone, called only from the calling thread. `run()` maps it to its 0–15 % band.

## Notes for dependent work packages

- WP2c1a: `S_band = band − ∪C` (the cluster's fronts are the only part of `U_n` inside the band); `band` may be
  ≤ 5 µm smaller than the exact offset; `unprintable` is whole-layer (clip it to the window).
- WP2c1a/b: `tail_needs` and `tolerated` are unfiltered; tolerated trees have no tail entries.
- WP2b2: `thetas` are canonical unit vectors (angle in [0, π)), wide fronts only, ≤ `max_tie_dirs`.

## Tests

- `[sas-geometry]` (libslic3r_tests, synthetic inputs): Feret (square, circle, triangle, concave, hole,
  degenerate); bridge directions (outward / along / tie / 10 % tie tolerance / max_dirs / mirrored and rotated
  shelves n ≡ −n / anchored all around → minor axis / round front sampling / empty); fronts, clusters and θ of
  shelves 10 × 15, 10 × 30, 10 × 20, custom angle, thin shelf; weak contact (thin post → island tree that never
  merges, rib, 2 × 2 post, full base); hoop candidates (dome rings with Feret = hole diameter, lid with one hole yes,
  lid with four holes no, halo no, post inside the ring no); unprintable fin; determinism 1 thread vs default on
  `figurine_proxy`, progress monotone 0 → 100, cancel → `CanceledException`.
- `[sas-islands]` (fff_print_tests, sliced prints): pendant 60° cone (first, merge, volume, not tolerated, tail to
  apex + 2.6 ± 0.3, ring areas, slice_tree / slice_bad); tolerated trees (volume vs height vs tolerated, `T_n`);
  needle (tail without downward facets, tail up to the merge); sibling tails (two roots in one tree, separate
  tails); cluster invariants on T-shapes (two / one cluster), dome, cone, figurine proxy (canonical order,
  ownership, disjoint windows, bands ⊇ `(C ⊕ m) ∩ L`, band_prev ⊇ `(C ⊕ (m + w)) ∩ L_{n−1}`, bands ⊂ window).
- **Deferred** (projection is a stub in the WP2a tree): `manual_enf` / `manual_blk` from painted enforcers /
  blockers and enforcer / blocker volumes — add once WP1a's `project_states` / `slice_modifiers` are in.
