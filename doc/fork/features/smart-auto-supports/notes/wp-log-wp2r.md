# Work package log — WP2r (Reachability)

Scope: `src/libslic3r/Support/SmartSupportsReachability.cpp` complete (plan §2.3, §3.7) and
`tests/fff_print/test_smart_supports_reachability.cpp` (plan §7.1, `[sas-reachability]`). No header changed.

## What is mirrored (verified against 2.9.6)

| Item | Slicer source | Implementation |
|---|---|---|
| collision `coll_s` of the tip radius | `TreeModelVolumes.cpp:118-126, 425-541` | outlines simplified by the G-code resolution; `∪_{j=s−z_bot..s} L_j ⊕ (r + xy_min)` (offset per layer, jtMiter 1.2) `∪ ∪_{i=1..z_top} L_{s+i} ⊕ (r + range_i)`; `union_`, **not simplified, no blockers** (see deviations) |
| Fast avoidance to the plate | `TreeModelVolumes.cpp:618-715` | `avoid_0 = coll_0`; sub-steps `1.9·max(r, xy_min)`, last-step stretching, `jtRound` with arc tolerance = resolution, `polygons_simplify` per layer; `max_move = (coord_t)(tan(angle)·layer_height) − 2` |
| enforced overhang | `TreeSupport.cpp:284-299` | `F_grown = diff(offset(union_ex(E ∩ (L_n − L_{n−1})), tip_d), L_{n−1})` (default jtMiter 3) |
| forbidden area | `TreeSupport.cpp:1323-1328, 1132-1135` | `coll_t` (or `avoid_t` with `buildplate_only`, D35) `⊕ 5 µm`, jtMiter 1.2 |
| tip area T | `TreeSupport.cpp:1336-1366` | `T0 = F_grown − forbid`; the outset loop is copied literally (incl. a copy of the file-static `safe_offset_inc()` / `safe_union()`), radius-0 collision as clip |
| roof decision | `TreeSupport.cpp:1122-1150, 1414-1426` | per `union_ex(T0)` component K: roof iff `interface_layers > 0` and `area(K − forbid_{t−1} ⊕ 5 µm) ≥ 1 mm²`; roof contact = K; failed components and `T − roofs` take the tip path |
| tip status | `TreeSupport.cpp:381-418` | plate = centres outside `avoid_t`; model = inside (only if support may rest on the model) |
| short on-model branches | `TreeSupport.cpp:2708-2716`, `TreeSupportCommon.cpp:103` | tip-path model centres in `∪_{s=t−min_dtt..t−1} coll_s` are deleted (= plan's `(∪ L_{t−k..t}) ⊕ (r + xy)`, k = min_dtt + z_bottom); roofs exempt; with `force_tip_to_roof` (`:1273`) nothing is deleted |
| tip layers | `TreeSupport.cpp:1248-1306` | `tip_layer(n) = n + raft − (z_top + 1)`, `tips_possible(n) = tip_layer ≥ max(raft − (z_top + 1), 1)` and `n < layers` |

Contact of a tip-centre area X: `F_grown ∩ (X ⊕ r)` (jtRound, arc tolerance 5 µm), split plate / model by
the status above. `Contact::model` may overlap `Contact::plate` (`model_part` subtracts the plate part).
`cover(F) = F ∪ (L_n ∩ (F ⊕ G) ∩ (L_{n−1} ⊕ G))` with jtRound offsets; n = 0 → F.

## Deviations from plan.md (exact)

1. **Collisions in the raft region** (§3.2 header comment, §3.7 "coll_s = avoid_s = ∅ for s < raft_support_layers"):
   the slicer's collision of a raft layer s still holds the object layers within `z_top` above
   (`TreeModelVolumes.cpp:490-527`); mirrored, so `coll_s = ∅` only for `s < raft − z_top`. Tips of the cap's first layer
   over a 1-layer stem on a raft are kept `r + xy_min/2` from layer 0, as in the slicer (test "raft indexing" compares
   with `TreeModelVolumes` there).
2. **Collisions are not simplified** (§3.7 "polygons_simplify(gcode_resolution)") and contain no blockers: the slicer's
   `processing_last_mesh` (`TreeModelVolumes.cpp:480`) is never true for one object, so neither its simplification nor
   the anti-overhang union happens. The avoidance is simplified as in the slicer.
3. **Outset of the remainder** (§3.7 `T = T0 ∪ ((Rm ⊕ extra_outset) − forbid_n)`): replaced by the slicer's literal loop.
   It grows in steps of `lw/8` while `acc + lw/8 < extra_outset`, i.e. at most 0.15 mm for the defaults (extra_outset
   0.2 mm with line width 0.4), each step limited by the radius-0 collision.
4. **Radius-0 collision** of that loop is computed window-locally from the unsimplified slices (the slicer simplifies
   by the G-code resolution first; difference ≤ 12.5 µm, only in the outset).
5. **`below_model_too_short(Z, n)`** (the plan gives no formula): true iff on-model tip-path tips of `contact(Z, n)` are
   deleted as too short and that leaves `area(Z ∩ cover(F_short) − cover(contact(Z, n).all)) ≥ A_min`. False with
   `buildplate_only` or `force_tip_to_roof`.
6. **Queries without `ensure()`** do not fail: they advance the chain themselves (blocking, under the mutex, without a
   cancel callback; collisions are computed on first use inside `std::call_once` + `tbb::this_task_arena::isolate`).
   Callers should still `ensure()` first (cancel, plan §2.3).
7. **Raft first tree layer** (`finalize_raft_contact()`, `TreeSupport.cpp:1027-1042`: tips below
   `raft_layers() − 1` deleted) is not mirrored: `Params` has no `raft_layers()`; it only matters when the gap layers
   between raft and object are fewer than `z_top` (e.g. layer 0.1 with contact 0.2 on a raft).
8. **Real-contact cross-check, uncovered samples** (§7.1 "samples predicted uncovered (InterfaceGap) have none within
   r"): impossible by construction (the roof starts 0.5175 mm from `L_{n−1}`, the 40° front ends at 0.238 → 0.28 mm < r).
   The test uses isolated enforced layers and checks that **no real support is closer to a roof-gap sample than the
   predicted roof (− 0.05 mm)**; the real roof equals the predicted one (11.73 / 11.73 mm², 38.26 / 38.26 mm²).
9. **Avoidance cross-check without an accessor**: the frozen header exposes no `coll`/`avoid`. The test recovers them
   through `tips()`: layer `n = t + z_top + 1` of a copied input is replaced by a large square (coll_t only reads layers
   ≤ n − 1), then `T = tips().bp ∪ tips().model` is the complement of `coll_t ⊕ 5 µm` and `tips().model = T ∩ avoid_t`.
   No header change needed.

## Notes for dependent work packages

- **Threading.** `compute_collisions` is parallel (TBB, isolated), once (`call_once`). `start_prefetch` starts a
  `std::thread` (ClipperUtils only), single writer up to the target; `ensure(k)` waits in 50 ms slices calling `cancel`,
  for `k` beyond the target it waits for the thread to finish, joins it and advances under the mutex; prefetch
  exceptions are rethrown (sticky). The destructor sets `stop` and joins. All queries are const and thread-safe;
  results are identical whoever computed the chain (test "pull model").
- **Cost.** Every query clips to `bbox(arg) ⊕ (tip_d + extra_outset + r + max(xy, xy_min) + 1 mm)`; `plate_part` and
  `model_part` each call `contact` once (WP2c1b: call `contact` once yourself if you need both parts).
- **WP2c1a / InterfaceGap.** With roofs, `cover(all) = roof` near steep fronts (no gap band: K ⊕ G does not reach
  `L_{n−1} ⊕ G`), `cover(tip_path_all)` covers the front — the attribution rule of §3.8 works as planned.
- **WP5 / D34 (open question 4), measured** (default config, 0.2 mm layers, organic):
  - painted 40° wall / sphere ring, interface 0: 100 % of the covered samples have real support within
    `tip_spacing/2 + r` (90 % quantile ≈ 0.45 mm), real / predicted contact area ≈ 1.2;
  - an **isolated** enforced layer of a thin ring (sphere R 20, r ≈ 5.9): only 88 % within `tip_spacing/2 + r`
    (90 % at 2.1 mm, max 3.4 mm), real / predicted area 6.4 / 52 mm² — `ensure_maximum_distance_polyline()` keeps the
    tips of a chord ≥ `connect_length` apart (Euclidean), so gaps reach ≈ 2·tip_spacing. The test bounds that case
    by `tip_spacing + r`;
  - **interface 2 on painted slopes**: 60 % (wall) / 89 % (ring) of the roof-gap samples have real support within G,
    from the branches of the tips of the painted layers above that pass through the tip layer. `InterfaceGap` is
    therefore conservative on continuous painted slopes; it is exact for isolated layers.

## Results (sandbox, dev/2.9.6 merged in by `wp-build.sh`)

- `fff_print_tests "[sas-reachability]"`: 10 test cases / 241 assertions passed, incl. the `[slow]` cases
  (collision / avoidance vs `TreeModelVolumes` at 5 + 5 + 2 support layers: sym. diff ≈ 0, avoidance gain checked;
  real-contact cross-check: 8 full `process()` runs, ≈ 4.5 s).
- `fff_print_tests "[SmartSupports]"`: 20 test cases / 2499 assertions passed; `libslic3r_tests "[SmartSupports]"`:
  7 test cases / 262 assertions passed (the other WPs' stub smoke tests are unaffected).
- No compiler warnings in the WP2r files.

## Tests (`[SmartSupports][sas-reachability]`)

| Test case | Plan §7.1 row |
|---|---|
| plate coverage incl. cover, interface 0 (5 sections) | plate coverage: 40° wall front, sphere ring r 12.1, inverted 60° / 75° cone tail rings, 60° cone at 0.1 mm (z_top 2) → `plate_part` ≥ 90 % of the zone; also: the contact alone stays ≥ 0.05 mm from `L_{n−1}` (the gap band covers) |
| interface 2, roof gap on a 40° wall | interface 2 |
| roof decision on the union of the painted zones | roof merge (shelf, two 0.3 × 0.3 mm pieces 0.4 mm apart) |
| raft indexing | raft 2 (+ `TreeModelVolumes` comparison in support-layer indices) |
| build plate only | buildplate_only |
| T arm over an 80 × 80 base rests on the model | T arm; section: arm 0.6 mm above the base → `below_model_too_short` |
| first support layer | first support layer |
| pull model (5 sections) | pull model; plus "queries without ensure" |
| collision and Fast avoidance equal TreeModelVolumes `[slow]` | avoidance vs `TreeModelVolumes` Fast |
| real contact cross-check against process() `[slow]` | real contact cross-check (painted, interface 0 / 2) + isolated enforced layers (roof gap) |
