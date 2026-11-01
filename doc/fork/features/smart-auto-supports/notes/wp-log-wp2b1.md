# Work package log — WP2b1 (raster utilities, accumulated reach)

Scope (plan §8.2): `SmartSupportsRaster.cpp` (`make_grid`, `rasterize`, `cells_to_expolygons`),
`SmartSupportsReach.cpp` (`segment_min`, `ReachField`, `ReachCarry`), `tests/libslic3r/test_smart_supports_reach.cpp`
(`[sas-reach]`). No header changes.

## Semantics (contract for WP2b2, WP2c1a)

### Raster

- `make_grid`: unchanged from WP0 (origin = bbox.min − (0.37, 0.61)·res, covers the bbox, `v` = 0, `m` = 0).
- `rasterize(expolys, grid, value, conservative)`: sets `m = value` for the cells whose **centre** is inside; all
  other cells are left as they are (several rasterisations with different values can be layered; `v` is never
  touched). Non-zero winding over all contours and holes, so overlapping ExPolygons fill as their union. Boundary
  rule: scanline y counts an edge if `min(a.y, b.y) ≤ y < max(a.y, b.y)`, a cell is inside if its centre x lies in
  `[x_enter, x_leave)`. `conservative = true` rasterises `offset_ex(expolys, res/2, jtRound, arc tolerance 1 µm)`:
  cells touching the polygons (disc kernel, review 4 M3).
- `cells_to_expolygons(grid, pred)`: exact union of the cell squares with `pred(iy·cols + ix)`; row runs are merged
  with identical runs of the following rows into rectangles, then `union_ex`.

### ReachField

`ReachField(cprime, domain, seeds, delta_beta_mm, params, cancel)` — `delta_beta_mm` = Δ = `w_β − w_n` in mm (negative
values are clamped to 0, so the caller may pass `w_β − w_n` directly).

- **Mode.** Direct iff the seeds are non-empty and `area(C' − offset(chained seeds, ρ − 1 µm, jtRound, etOpenRound,
  arc tolerance 1 µm)) < 1e-6 mm²` (an empty C' is Direct). Consecutive seeds (`b == next.a`) are chained into
  polylines first. **No seeds: `value()` = +∞ everywhere**, `direct_mode()` true (ungrounded component).
- **Direct `value(p)`** = exact Euclidean minimum over *all* seeds of `max(0, |p − a| − Δ) + ĉ(a)` (closed-form
  segment minimum, bounded radius search: after the first hit only seeds within `best − min ĉ + Δ` are checked).
- **Grid.** `res = clamp(√(area(bbox(D ∪ C' ∪ seeds)) / grid_max_pixels), grid_res_min, grid_res_max)` (plan rule;
  with `grid_res_max` = 0.1 mm a D component larger than 200 × 200 mm exceeds 4 M cells — not capped, as the plan
  says). Conservative raster of D **and** C'. Cells within ρ_eff of a seed start with the exact value over those seeds;
  16-neighbour Dijkstra (steps 1, √2, √5; a knight step needs both cells its segment crosses, a diagonal step only its
  target cell), indexed binary heap with decrease-key (8 bytes per cell, deterministic `(value, index)` order).
- **Grid `value(p)`** = min(exact minimum over the seeds within ρ_eff + 1 µm, min over the 4×4 cell centres c around
  p of `v(c) + |p − c|`); both +∞ → Euclidean minimum over all seeds. A grounded C' therefore never gets +∞; points
  outside the domain get a finite (Euclidean) value too.
- `res()`: grid resolution; Direct: `grid_res_max` (0.1 mm by default).
- `cropped(bbox)`: Grid → copy of the cells within bbox ± 3 cells with a whole-cell origin shift, so values inside
  the bbox are **bit-identical** to the field; seeds are shared (a cropped field keeps all seeds of its component).
  Direct → shares the field.
- Cancel: `cancel()` every 65 536 Dijkstra pops and every ~65 536 exact-zone cell evaluations; an empty
  `ThrowOnCancel` is allowed.
- Immutable after construction, thread-safe.

### ReachCarry

- `push(layer, z_top, w, fronts)`: call for **every** committed layer in increasing order, also for layers without
  fronts — its `w` is part of R for the layers below. Pushing a layer ≤ the newest replaces it and everything above
  (re-commit). `H_s ≤ 0`: nothing is stored. Window: layers with `z_top_newest − z_top_m ≥ H_s + 1e-4` are dropped.
- `FrontCarry` as the evaluator should fill it: `C` = the whole front C (not C'); `bbox` (computed when undefined);
  `grounded`; `field` = `field->cropped(bbox(C))`, `nullptr` → c* = 0 (early exits without a field); `zero` = scaled
  `covered_all ∪ hoop band ∪ (C ∩ Fcov)` — grown by 2 µm internally, so a q on ∂C on a zero border counts as zero.
  **`boundary` is not used and may be `nullptr`**: the carry builds its own double-precision edge index (see below).
- `c_hat(a, n, z_top_n, h_n, w_n)` = plan §3.8 exactly: layers with index ≥ n are skipped; per layer m (newest
  first, while `z_top_n − z_top_m − h_n < H_s`) the fronts within R = `w_n + Σ w_k (pushed layers between) + δ`,
  δ = `delta_widths · ext_width`; the first layer with any front in reach decides: only its grounded fronts count
  (none grounded → 0, §3.14 #11), ĉ = min c*(q)·max(0, 1 − Δz/H_s), q = a inside C, else the nearest point of ∂C.
  Non-finite field values are ignored (→ 0 if nothing else). Front lookup: 2 mm uniform grid per layer (CSR).
  Pure and thread-safe.

### Left to WP2c1a (not in this module)

Anchor resampling (0.1 mm + vertices, canonical start, 1 µm snapping), the cluster-owned ĉ memo, splitting a segment
between a zero and a non-zero sample in the middle (constant halves; `segment_min` is exact for any linear ĉ, so this
only controls how the zero spreads), Δ per layer, `cropped(bbox(C))` before `push`.

## Deviations from the plan (all inside the module)

1. **Direct mode beyond ρ.** The plan queries Direct fields only within ρ. Direct `value()` searches all seeds
   (bounded, Euclidean): near a reset border the minimising seed (ĉ = 0) can be farther than ρ while a high-ĉ seed is
   close (review 1 m4). Within ρ the result is the plan's.
2. **Grid query inside ρ** takes min(exact zone, 4×4 cells) instead of the exact zone alone — same reason: the cell
   values carry low-ĉ seeds beyond ρ geodesically.
3. **Exact radius ρ_eff = max(ρ, Δ + 1.5·res)** in Grid mode. With β ≪ α Δ can exceed ρ (β 10°, h 0.2: Δ = 0.93); the
   Dijkstra is exact only from cells in the linear part of `max(0, dist − Δ)`.
4. **Conservative raster with a disc** (`⊕ res/2`, round), not the square kernel: cells that touch D only near a
   square corner may stay inactive. Thin necks stay connected (tested).
5. **Diagonal steps are not checked** (only knight steps): thin 45° necks stay connected; a one-cell diagonal wall can
   be crossed (same class as §3.14 #14).
6. **Double-precision AABB trees.** Seeds and carry edges use `LinesDistancer<Linef>`. Measured: radius queries of
   `LinesDistancer<Line>` (integer boxes) visit every line (≈ 20 µs per query on 1800 lines, also for a straight line
   and a far point) against 0.25 µs with `Linef`; its nearest-line query additionally descends into every node whose
   box contains the point. With `Line` the frustum chain test took 14.8 s, now 0.43 s. Other modules using
   `LinesDistancer<Line>` for many queries (e.g. `DomainEdges`, `FrontCarry::boundary`) should prefer `Linef` in their
   own code; the frozen header types stay as they are.

## Tests (`[sas-reach]`, libslic3r_tests)

16 test cases: `segment_min` (closed form incl. kink and |g| ≥ 1, 300 random segments × 3 Δ against brute force);
raster (layout, hole, conservative neck, union, untouched cells); straight anchor at 13.28° (16-neighbour worst case)
and 22.5° (8-neighbour worst case) — exact within ρ ± 0.01, ≤ 3.5 % beyond; geodesic L at 22.5° ± 3.5 %; one-cell
wall vs knight steps; thin neck + Euclidean fallback + no seeds; reset border (Grid and Direct); Direct with free ends;
`cropped` bit-identical; Δ > ρ; cancel + determinism; carry (hanging rule, reach R, zero border, ungrounded topmost
layer, H_s = 0, window, 4 threads = serial); H1 chains with a mini evaluator: 40° wall 3.068 ± 2 % at 0.2 mm (also
rotated 30°) and 0.1 mm, inverted frustum (sides 3.068, corners 80·(√2·step − w) ± 2 %, no side value below 98 %),
β < α (40° wall with β 35° → 0; 30° wall with β 40°), stairs with H_s decay (2.3·0.8 + 2.3, 2.3·0.2 + 2.3, none).

Mutation checks (temporary, reverted): without the knight crossing check the one-cell-wall test fails (0 of 8
positions go around); with 8 neighbours the 22.5° straight anchor (7.5 % off), the 22.5° L path and the reset border
fail (27 assertions). The 13.28° case alone does not separate 8 from 16 neighbours: from a straight anchor both are
2.7 % off there.

## Results (sandbox, dev/2.9.6 merged by `wp-build.sh`)

`libslic3r_tests "[sas-reach]"`: 16 test cases / 16 382 assertions, all passed (≈ 0.9 s);
`libslic3r_tests "[SmartSupports]"`: 22 test cases / 16 636 assertions passed. No warnings in the WP2b1 files.
