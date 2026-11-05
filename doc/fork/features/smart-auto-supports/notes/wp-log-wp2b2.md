# Work package log — WP2b2 (tension, chord rule, hoop credit, classification)

Scope: `src/libslic3r/Support/SmartSupportsTension.cpp` (`compute_anchors`, `domain_edges`, `tension_levels`,
`hoop_credit`, `classify_front`) and `tests/libslic3r/test_smart_supports_tension.cpp` (`[sas-tension]`).
No header changes. The plan (§3.8, D26, D27) stays authoritative where this log is silent.

## Ownership note

The WP brief also listed "bridge directions with ties (copy of the slicer's cost loop)" and "Feret diameter".
Both are declared in the Phase A block of `SmartSupportsInternal.hpp` and have their WP0 stubs in
`SmartSupportsBase.cpp`, which WP2a owns (plan §2.1, §3.5, §7.1 geometry tests). Defining them here would give
duplicate symbols, so WP2b2 does **not** implement them. The tension code consumes their results through
`FrontGeom::thetas` (all tied directions, evaluated worst-over-θ here) and `FrontGeom::hoop_hole / hoop_feret_mm`.

## Decisions and deviations (exact)

### `compute_anchors`
- `support = ∂C' ∩ expand(F, 2 µm)`; **`material = ∂C' ∩ (expand(GP, 2 µm) − expand(F, 2 µm))`**. The plan formula
  lets both contain the boundary along `L ∩ F` (GP ⊇ L ∩ Fcov); here support wins, so the two lists are disjoint
  (the ĉ minimum is the same either way: support anchors seed ĉ = 0).
- `free = ∂C' − expand((L − C) ∪ F, 2 µm)` as in the plan.
- GP, F, L are clipped to `bbox(C') ⊕ 50 µm` first (band-wide inputs).
- Canonical output (§3.12): open polylines first < last, closed loops (whole contour anchored) start at the
  lexicographically smallest point (Clipper does not keep the orientation of open paths, so the loop orientation is
  whatever Clipper returns), each list sorted by (first point, last point, size).
- Boundary pieces carry 2 µm stubs at junctions (the tolerance band): a free piece and the adjacent anchor meet
  2 µm away from the geometric corner.

### `domain_edges`
- `anchor = ∂D ∩ expand(stop, 2 µm)`, `free_ = ∂D − expand(stop, 2 µm)`; the band border is free automatically.
  Both distancers are always non-null (empty for an empty D: distance = +∞).

### `tension_levels` — wide fronts (`FrontGeom::wide`)
- Scanlines along every θ of `front.thetas`, on a **global lattice**: line j at `n·p = (j + 0.37)·s`,
  `s = max(½·ext_width, 0.05 mm)` (n = θ rotated +90°), so lines never depend on bbox(D) and never coincide with
  axis-aligned boundaries at round coordinates. Only lines whose strip reaches bbox(C') are cast (≤ 20000 per θ;
  the spacing grows beyond that). Lines are clipped to the D component (`intersection_ln`).
- Span end classification: anchor iff `d(anchor edges) ≤ d(free edges)` (and finite). Both ends anchored → finite
  t, else None.
- Levels: `t ≤ B + 1 µm` Covered, `≤ f·B + 1 µm` May, `≤ f²·B + 1 µm` Should, else Must (the 1 µm keeps
  theory §10's 90 mm bridge at should).
- A span's level applies to its **strip**: the scanline band (width s, +1 µm overlap with the neighbours), extended
  along θ towards the midpoint of the gap to the next segment on the same line, at most 2·s beyond the segment ends
  (C' next to slanted boundaries takes the level of its own segment). Overlaps: worst wins.
- A lattice line just outside D leaves a band of up to ½·s along boundaries parallel to θ: C' not in any strip but
  within s of one takes that strip's level (worst wins). C' farther from every strip of a θ counts as None for
  that θ.
- Ties: the level of a point is the **worst over θ**; `covered_all` = covered under all θ (review 1 m5).
- No θ (empty `thetas`, e.g. the WP2a stub) or empty D → None everywhere.
- `regions`: a partition of C', at most one entry per level, in `TLevel` order (Covered, May, Should, Must, None),
  empty levels omitted.

### Cuts (long-span row candidates)
- One set **per scanline segment** (per tied θ) whose span is finite with t > B and whose strip meets C':
  `k = ⌈t/B⌉ − 1` cuts (1 for t ≤ 2B); cut i (1-based) has `[lo, hi] = [max(0, t − (k + 1 − i)·B), min(t, i·B)]`.
  The interval centres are B apart and keep every sub-span ≤ B (WP2c1b's "nearest the interval centre" works).
- `Cut::p` = the span start (end point with the smaller coordinate along θ), `dir` = θ normalised, `t` in mm,
  `lo/hi` in mm from p along dir. Order: θ order, line index, position along θ.
- Thin fronts emit cuts for their qualifying chords too (p = chord start, dir = chord direction).

### `tension_levels` — thin fronts: the chord rule (D27, review 1 m6)
A free piece Π (from `Anchors::free_edges`) gets `t = |chord|` iff
1. it is open and **both ends lie within 5 µm of an anchor** (material or support), i.e. not at a "neither"
   boundary;
2. `|chord| > ext_width`;
3. every vertex of Π lies within `ext_width (+2 µm)` of the chord segment;
4. **end closure ≥ 45°** at both ends: walking along the anchor polyline that touches the end, away from Π, the
   direction to the first anchor vertex at least 10 µm from the end makes ≥ 45° with the chord line (this skips
   the 2 µm stub). A side wall gives ≈ 90°; a strip anchored only along its long side (hole strip, lean-to with an
   open end) gives ≈ 0° at the bad end. A smooth (tangent) anchor/free junction fails the test (conservative).

`C' ∩ (Π ⊕ 2·ext_width)` takes the level of the chord; where several pieces qualify the **best** counts (each free
edge is printed as its own perimeter). The rest of C' is None. `covered_all` = the Covered part.

### `hoop_credit` (D26)
Credit only if `grounded`, `C.holes.size() == 1`, `hoop_hole == 0` and `hoop_feret_mm ≤ B_ceil`; band =
`C' ∩ (H ⊕ B_max·hoop_width_factor)` (round offset of the hole region, arc tolerance 5 µm). The other D26
conditions (fully free slice hole, ≥ 90 % of the outer contour on S) are Phase A's (WP2a sets `hoop_hole`).

### `classify_front`
- `zero = covered_all ∪ hoop_band` (the evaluator adds `C_f ∩ Fcov`, plan §3.8).
- Samples: pixel centres `((i + ½)·res, (j + ½)·res)` of a **global lattice** inside C' (scanline fill, even-odd)
  plus ∂C' every res (each contour from its first point); `res = max(reach.res(), 10 µm)`, coarsened so that
  bbox(C')/res² ≤ `grid_max_pixels`. Each sample stands for a res × res square; row runs of equal level are merged;
  where squares of both levels overlap **must wins**; squares are clipped to C'.
- Bending level: `c ≤ K` covered, `≤ f_hard·K` should, else must; **a non-finite c counts as must**
  (the stub skipped it). Callers should only pass C' parts that lie in a seeded field.
- Combination (theory §2.5): covered if bending covered, tension Covered, in `covered_all` or in the hoop band;
  otherwise level = min(tension, bending), cause = the mechanism giving it, **tie → long span**. C' without a finite
  tension region counts as t = ∞. Cantilever needs exist only at should/must; long-span needs at may/should/must.
- `max_c` = max of the finite sample values.
- **Early exit** (plan §3.8 "classification skipped"): applied inside `classify_front` when C' is a single
  ExPolygon without holes, `max_chat` is finite and `max_chat + perimeter(C')/2 + width(C') ≤ K_max`, with
  width = the smaller bbox extent of C'. Then there are no needs and `max_c` is the maximum over the ∂C' samples
  only (cheap: the perimeter is < 2·K_max). The bound assumes that C' has an anchor on its own contour:
  **pass `max_chat = +∞` to disable it** whenever that is not guaranteed.

## Notes for dependent WPs (WP2c1a, WP2c1b)

- Call order per front (plan §3.8): `A = compute_anchors(C', C_f, GPc, Fcov, Lc)`; for wide fronts
  `edges = domain_edges(Dcomp, GPc ∪ Fcov)` and `tension_levels(C' ∩ Dcomp, front, Dcomp, edges, A, params)`;
  thin fronts ignore D and the edges (any values are fine, e.g. empty). D must contain the C' passed with it.
- `TensionMap::regions` partitions the C' passed in; `covered_all ⊇ C'` can be tested with
  `diff_ex(C', covered_all).empty()` (strips overlap by 1 µm, so a fully covered C' leaves no slivers).
- `max_tie_dirs`, `tie_tolerance` are used by WP2a's `bridge_directions`, not here.
- Cuts can be numerous (one set per scanline per θ, ≈ width/(½·ext_w)); they are plain data, sorted
  deterministically.
- All functions are pure, serial and deterministic (no TBB inside): safe in the per-cluster `parallel_for`.

## Tests (`tests/libslic3r/test_smart_supports_tension.cpp`, `[SmartSupports][sas-tension]`)

All synthetic (rectangles / circles, `ReachFn` = c as a function of the position), 12 test cases:
anchors (material / support / free / neither, support wins, closed loop, empty C'); domain edges; bridge
30/50/70/80/90/100 mm (levels, `covered_all`, cut intervals per scanline); one free end; no θ; support contact
splitting a span (P4); spans through ungrounded material; rotated (30°) and slanted (63°) bridges; ties
(`covered_all` = covered under all θ, worst direction wins); chord rule (lean-to 30 → covered, 50 → may with one
cut, open end → none, hole strip → none, thin beam → covered, curved edge → none); hoop credit (band, not
grounded, Feret vs B_ceil, two holes, band width, C' only); `classify_front` (None/May/Should/Must/Covered
tension, tie rules, hoop, mixed map, strip thinner than res, non-finite c, early exit, determinism); tension +
classification of a 50 mm bridge.

## Results (sandbox, dev/2.9.6 merged in by `wp-build.sh`)

- `libslic3r_tests "[sas-tension]"`: 12 test cases, all passed.
- `libslic3r_tests "[SmartSupports]"`: 18 test cases / 2377 assertions passed (incl. the other WPs' WP0 smoke
  tests in this tree). No warnings in the WP2b2 files.
