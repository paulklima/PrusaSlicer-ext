# Work package log — smart support painting

One section per work package: deviations from `plan.md` (exact), decisions on ambiguities and
what later work packages must know. The plan stays authoritative where this log is silent.

## WP0

Scope: headers of §3.1/§3.2, stubs, the real input functions, the TriangleSelector / FacetsAnnotation
additions, GUI placeholders, CMake entries, test infrastructure and the input test (§8.2 WP0 row).

### Files

| File | Content |
|---|---|
| `src/libslic3r/Support/SmartSupportStates.hpp` | complete (inline) |
| `src/libslic3r/Support/SmartSupports.hpp`, `SmartSupportsInternal.hpp` | frozen headers |
| `src/libslic3r/Support/SmartSupportsInput.cpp` | real `derive_params`, `choose_print_object`, `input_consistent`, `build_input` |
| `src/libslic3r/Support/SmartSupports{,Base,Eval,Paint,Plan,Projection,Raster,Reach,Reachability,Report,Tension}.cpp` | stubs (below) |
| `src/libslic3r/TriangleSelector.{hpp,cpp}` | `PaintRule` + setters, `PredicateCursor` (real), `get_facets_strict(states)` (real) |
| `src/libslic3r/Model.{hpp,cpp}` | the six `FacetsAnnotation` helpers (real), `reset()` resets `used_states` |
| `src/slic3r/GUI/Gizmos/{SmartSupportsPanel,TriangleSelectorFdmGui}.{hpp,cpp}` | empty placeholders (namespace only) |
| `tests/smart_supports_meshes.hpp`, `tests/fff_print/smart_supports_test_utils.hpp` | test infrastructure |
| `tests/fff_print/test_smart_supports_input.cpp` | complete, `[sas-input]` |
| 12 further test files | one WP0 smoke test each (below) |
| 4 CMakeLists | libslic3r Support block, GUI Gizmos block, both test lists |

### Header deviations from §3.1 / §3.2 (exact)

1. **Private state via pimpl** (the plan lists no private members; the headers are frozen, so each
   owner needs freedom in its `.cpp`):
   - `Reachability`, `ReachCarry`, `LayerEvaluator`: `private: struct Impl; std::unique_ptr<Impl> m_impl;`,
     an out-of-line destructor, copy deleted. `ReachCarry` additionally has a move constructor and move
     assignment (`noexcept`, defaulted in the `.cpp`). The owner defines `struct X::Impl` and the special
     members in its own `.cpp` (the WP0 stubs show the pattern).
   - `ReachField`: `struct Impl;` (public, opaque), `~ReachField() override;`,
     `private: explicit ReachField(std::shared_ptr<const Impl> impl); std::shared_ptr<const Impl> m_impl;`
     (`cropped()` can build a new `ReachField` around a new `Impl`, or return any other `ReachQuery`).
2. **Default member initialisers** added to plain structs (no API change, aggregate init still works):
   `ContractIssue::key = SupportOff`, `LayerInput` scalars = 0, `VolumeInput/ModifierInput::trafo` =
   identity, `AnalysisInput::po_trafo_centered` = identity, `instance_shift_mm` = 0, all `Placement`,
   `ReportEntry` fields, `VolumePaint::changed = false`, `FrontGeom`, `LayerGeom` (`in = nullptr`, scalars 0),
   `IslandTree` (`id = -1`, `merge = UINT32_MAX`), `Grid` scalars, `Reachability::Contact::roof_gap = false`,
   `Need` (Island / Must / None / 0 / Unplaceable), `FrontNeeds::max_c = 0`. Lines with several
   declarations were split; types, names and order are unchanged.
3. **Additions:** `inline constexpr uint8_t cause_bit(Cause)` in `SmartSupports.hpp`;
   `PredicateCursor() = delete;` and `~PredicateCursor() override = default;`; `#include <functional>` in
   `TriangleSelector.hpp`. `TriangleSelector::PaintRule` is a public type of `TriangleSelector`.
4. **Grid layout** (not in the plan, now part of the header): cell `(ix, iy)` ↔ `v/m[iy * cols + ix]`,
   centre `origin + ((ix + ½)·res, (iy + ½)·res)`; `make_grid` covers the bbox with
   `origin = bbox.min − (0.37, 0.61)·res`.
5. **`Params` defaults corrected** to what 2.9.6 really derives for its default config with nozzle 0.4
   (verified against `TreeSupportSettings` by `[sas-input]`): `support_line_width` 0.42 → **0.4**,
   `tip_spacing` 3.2 → **0.4·100/15 + 0.4 ≈ 3.07**. All other defaults of §3.1 matched. (The "≈ 3.2 mm
   stock" in §3.7 / D34 is therefore ≈ 3.07 mm.)

### Decisions on ambiguities

- **`AnalysisInput::volumes`** = model parts only (`is_model_part()`), in `ModelObject::volumes` order —
  only their paint is projected by the slicer (`PrintObject.cpp:3431`). **`modifiers`** = support
  enforcer / blocker volumes only; other modifiers and negative volumes are already in `lslices`.
- **`num_print_objects`** = PrintObjects of this ModelObject in the active print (≥ 1);
  **`other_objects_on_bed`** = any PrintObject of another ModelObject in the active print.
- **`build_input`**: `instance_shift_mm = unscale(PrintInstance::shift)` of the analysed instance (bed-local);
  `ext_width_avg_mm` = average of `LayerRegion::flow(frExternalPerimeter).width()` over `Layer::regions()`
  (divided by `region_count()`, as `TreeSupport.cpp:259-266`), `params.ext_width` without regions;
  `contract = check_contract(po.config(), up.plate_policy, &params)` (empty until WP2c0).
- **`derive_params`**: `first_layer_height = SlicingParameters::first_object_layer_height`;
  `custom_bridge_angle_deg` = the first printing region with `bridge_angle > 0`; the soluble adjustment of
  `TreeSupportSettings` (`xy_min ≥ 0.1`, `xy ≥ xy_min`) is applied when **this** object's contact distance is
  0 (the slicer's flag is global over all objects); `tree_angle_deg` clamped to [0, 90];
  `z_top/bottom_layers` from `SlicingParameters::gap_support_object / gap_object_support` (0 with soluble
  interface); `tip_spacing` in mm without the slicer's integer truncation; `raft_support_layers` mirrors the
  raft loop of the `TreeSupportSettings` constructor including the layers between raft and object.
- **`choose_print_object`**: background processing on and no PrintObject prints the analysed instance →
  `po = nullptr`, `NotInPrint`; background processing off with a fallback whose instances are not in the live
  object → `NotInPrint` (Print::apply pending). "First printable instance" of the fallback PO = the first of
  its `instances()` that is printable in the live object.
- **`input_consistent`** additionally requires that `po.instances()` contains the analysed instance and
  `analysed_instance < live_mo.instances.size()`; z offsets equal within `EPSILON`; instance and volume
  matrices compared with `isApprox`.
- **`FacetsAnnotation`**: `has_auto_states()` = up to 7 `TriangleSelector::has_facets(m_data, s)` scans
  (s = 3..9), short-cut on empty data; `has_enforcer_like_facets()` = ENFORCER scan or `has_auto_states()`;
  `get_data_auto_baked()` = deserialize, `remap_states(bake_map())`, serialize (merges children);
  `get_triangle_as_string()` now calls the static `triangle_as_string()` (same output, linear).
- **`clear_causes_map(causes)`** contains all 7 auto states (identity entries included).
- **`tool_paint_rule(bit)`** ignores the requested state.

### TriangleSelector state after WP0 (WP1a owns the file)

- `set_paint_rule()` / `clear_paint_rule()` store the rule, but **`select_patch()` ignores it until WP1a**
  adds the hook. `apply_paint_rule_recursive()` is declared only (no definition, never called).
- `PredicateCursor`: base `Cursor(0, 0, trafo, no clipping)`, then `use_world_coordinates = true`,
  `trafo_normal` set, `m_edge_limit = edge_limit_world`. `is_mesh_point_inside(p) = point_inside(trafo·p)`;
  `vertices_inside` = `classify(world triangle)`, or the default vertex count of `point_inside` when
  `classify` is empty; `is_any_edge_inside_cursor` = `touches(world triangle)` (false when empty; only asked
  when `vertices_inside` returned 0); `get_facets_to_select` returns `seed_facets`; `is_facet_visible` false
  (no BFS).
- `get_facets_strict(states)`: one traversal with a 256-entry state mask.

### Stub behaviour (what isolated WP trees see)

| Module | Stub |
|---|---|
| Projection | `project_states`, `slice_modifiers`: `in.layers.size()` empty `Polygons` |
| Phase A | `build_base_model`: layers sized, `in`, `z_bot/z_top/h`, `w` (0 at layer 0; §3.4 rule incl. α = 0 auto), `w_beta` (β clamped to (0, α_eff]), `slice_bad` = 0 / `slice_tree` = −1 per slice; no fronts, clusters, trees; `last_layer_with_fronts = 0`. `feret_diameter` = 0, `bridge_directions` = ∅ |
| Reachability | `tip_layer` real formula; `tips_possible` true; `gap_band_mm` = `xy_min_distance`; `F_grown` real formula `(E ∩ (L_n − L_{n−1})) ⊕ tip_d − L_{n−1}` (∅ at n = 0, no window); `tips.bp = F_grown`, model / spill ∅; `contact.all = plate = tip_path_all = F_grown`, `roof_gap` false; `cover(F) = F`; `plate_part = Z`; `model_part = ∅`; `below_model_too_short` false; collisions / prefetch / ensure no-ops |
| Raster | `make_grid` real (see above, `v` = 0, `m` = 0); `rasterize` no-op; `cells_to_expolygons` ∅ |
| Reach | `ReachField::value` = min over seeds of (Euclidean distance to the segment + linearly interpolated ĉ), +∞ without seeds; `res` 0.1 mm; `direct_mode` true; `cropped` shares the field; `ReachCarry::push` no-op, `c_hat` 0; `segment_min` = min of f at 0, len and the foot point |
| Tension | `compute_anchors`: material = all of ∂C' (`to_polylines`), support / free ∅; `domain_edges`: two empty (non-null) distancers; `tension_levels`: one region (C', `TLevel::None`); `hoop_credit` ∅; `classify_front`: pixel centres at `reach.res()` (coarsened to ≤ 1e6 samples), c ≤ K covered, ≤ f_hard·K should, else must, cantilever only, squares ∩ C'; `zero = covered_all ∪ hoop_band`; `max_c` over samples |
| Evaluator | `evaluate` returns an empty `LayerNeeds` (owned by the evaluator); `commit`, `mark_tree_unplaced` no-ops; `evaluate_pass`: `layers` sized, empty |
| Plan | `make_plan`: `stats`, `zones_paintable`, `zones_unpainted` sized, nothing else; `verify` ∅ |
| Report | `check_contract`, `recommended_changes` ∅; `apply_recommended` no-op; `config_fingerprint` 0; `build_report` ∅ with `report_more_out = {0, 0}` |
| Paint | `paint_placements`: `{volume_id, paint, changed = false}` per volume |
| `run()` | `paint` as above, `layer_stats` sized only with `collect_stats`, everything else empty; never throws |

### Test infrastructure (WP0, shared; WP-specific helpers go into the WP's own test file)

Namespace `Slic3r::SmartSupports::Testing`.

- `tests/smart_supports_meshes.hpp` (libslic3r only): `Parts = std::vector<indexed_triangle_set>`;
  2D helpers `polygon_mm`, `rectangle_mm`, `circle_mm`; meshes `translate`, `rotate_x` (degrees), `box(min, max)`,
  `extrude(ExPolygon, h, z0)`, `extrude_xz(profile, width)` (profile in the x/z plane, extruded along +y),
  `revolve(rz, segments)` (closed CCW profile, r = 0 on the axis), `arc_rz`, `wedge(angle, length, width)`;
  shapes `stairs`, `shelf_cube(depth, length, cube, thickness)`, `t_shape(arm, width, stem, height, thickness,
  base_size, base_h)`, `dome(r_in, r_out)`, `hole_block(hole_d, w, depth, h)`, `slope_wall(angle, height,
  thickness, length)`, `inverted_cone(flank, cone_h, apex_z)` (apex_z > 0: hangs from a plate on a post),
  `figurine_proxy()`; `test_params()` (Params defaults + α 45, interface layers 0, verification off; checked
  against `derive_params` of `sas_config()` by `[sas-input]`); `synthetic_input(parts, layer_h, first_h,
  params)` — model object id 999, volume ids 1000 + i, closing radius 0.049, links from 1 µm-shrunk slices,
  sets `params.layer_height/first_layer_height`.
- `tests/fff_print/smart_supports_test_utils.hpp`: `sas_config(overrides)` (§7 default config),
  `init_print_parts(parts, print, model, config)` (one object, one part per mesh, returns the live
  `ModelObject*`, throws on validation errors), `process_to_slices(print)` (posSlice only),
  `SasPrint{model, print}`, `make_sliced_print(parts, config)`, `cached_print(key, make_parts, config)` (key +
  full config; never modify), `SasRun{input, result}`, `run_sas(print, up, opts, adjust)` (first PO, its own
  model copy as live object, instance 0, `verification = false` then `adjust(params)`).

### Tags

All files use `[SmartSupports]` plus: `[sas-input]` (complete), WP0 smoke tests `[sas-selector]`,
`[sas-3mf]`, `[sas-paint]`, `[sas-geometry]`, `[sas-reach]`, `[sas-tension]`, `[sas-report]` (libslic3r_tests),
`[sas-slicer]`, `[sas-islands]`, `[sas-reachability]`, `[sas-eval]`, `[sas-reference]` (fff_print_tests). The
smoke tests assert stub behaviour and are replaced by the owning WP.

### Notes for later WPs

- libslic3r and the tests compile with **`-std=gnu++17`** (no C++20), Release with `NDEBUG` (asserts off).
- `SmartSupports.hpp` includes `TriangleSelector.hpp` and `SmartSupportStates.hpp`; `Model.cpp` includes
  `SmartSupportStates.hpp` (not `SmartSupports.hpp`).
- The fff_print print cache is shared by all fff test files of one binary run (same key + config → same print).

### Results (sandbox, dev/2.9.6 merged in by `wp-build.sh`)

- Full build of all targets incl. `PrusaSlicer` (GUI link) and all test binaries: OK, no warnings in
  new or changed files.
- `fff_print_tests "[SmartSupports]"`: 11 test cases / 2263 assertions passed (6 `[sas-input]` cases);
  `libslic3r_tests "[SmartSupports]"`: 7 test cases / 262 assertions passed.
- Full suites: `libslic3r_tests` 237 test cases (236 passed, 1 failed as expected — the existing
  `[!shouldfail]` arc welder case), `fff_print_tests` 138 test cases / 214135 assertions passed.
