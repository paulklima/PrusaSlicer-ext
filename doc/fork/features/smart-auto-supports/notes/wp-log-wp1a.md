# Work package log — WP1a

TriangleSelector paint rule, FacetsAnnotation scans, slicer projection, `_SUPPORTS_OFF` warning, 3MF bake
and fork attribute, `project_states` / `slice_modifiers`, tests `[sas-selector] [sas-3mf] [sas-slicer]`.
`plan.md` stays authoritative where this log is silent. No header was changed.

## Implementation

| File | Change |
|---|---|
| `src/libslic3r/TriangleSelector.cpp` | rule hook in `select_triangle_recursive`, `apply_paint_rule_recursive` |
| `src/libslic3r/Model.cpp` | `has_auto_states()` / `has_enforcer_like_facets()`: one linear bitstream pass with a state mask |
| `src/libslic3r/PrintObject.cpp` | `project_and_append_custom_facets(ENFORCER)` uses `get_facets_strict(*mv, enforcer_like_states())` |
| `src/libslic3r/Print.cpp` | `_SUPPORTS_OFF` uses `has_enforcer_like_facets()` |
| `src/libslic3r/Format/3mf.cpp` | export bake + `slic3rpe:ext_custom_supports`, import preference per volume |
| `src/libslic3r/Support/SmartSupportsProjection.cpp` | `project_states`, `slice_modifiers` |

### Paint rule (plan §4)

- Fully covered triangle with a rule: the rule is applied to every leaf below it, the subdivision is kept
  (no `undivide_triangle`). `select_triangle()` then calls `remove_useless_children()` as before.
- Partially covered leaf: returns without splitting if `rule(old, requested) == old` (manual paint, a leaf
  that already has the cause bit). This is the stock "leaf already has the requested state" check with the
  rule result, so protected leaves are never split.
- `!triangle_splitting` branch: the leaf gets `rule(old, requested)`.
- Without a rule every branch is the 2.9.6 code (all changes are guarded by `m_paint_rule`). The test
  "an identity rule equals no rule" checks byte-equal serialization.
- `seed_fill_*`, `bucket_fill_*` and `set_facet()` ignore the rule (unchanged; tested).

### 3MF (plan §5, D19)

- Export: per volume `has_auto_states()` → `get_data_auto_baked()` once; the standard attribute is written
  from the baked data, `slic3rpe:ext_custom_supports` from the true data, only where non-empty and only for
  volumes with auto states. The painting version metadata is computed from the unbaked data as before
  (states ≤ 9 → version 1).
- Import: `Geometry::custom_supports_ext` (cleared in `reset()`, one entry per triangle). A volume uses the
  fork strings for all its triangles if any triangle in `[first_triangle_id, last_triangle_id]` has a
  non-empty fork string (baked and true strings are empty for the same triangles).

## Decisions and deviations

1. **Single-pass scans** (`Model.cpp`): the WP0 bodies did up to 8 `TriangleSelector::has_facets()` passes.
   Now one linear pass over the bitstream (a plain sequence of prefix codes, as
   `TriangleSplittingData::update_used_states()` parses it). Tested against the per-state scans, including
   states 16, 17, 20 and 255.
2. **`slice_modifiers` flips on the true determinant** `trafo.linear().determinant() < 0` (frozen header
   comment). The slicer's `slice_volume()` tests `trafo.rotation().determinant() < 0`, which is never true
   (Eigen's polar decomposition returns a proper rotation), so the slicer never flips. Both give the same
   slices because `slice_mesh_ex()` unites with the non-zero fill rule; the test compares a rotated and
   mirrored enforcer volume with `slice_support_enforcers()` (sym. diff ≤ 1e-3 mm² per layer).
3. **`project_states`** evaluates `filter` for all 256 states and projects through
   `get_facets_strict(states)` + `slice_mesh_slabs()` with `VolumeInput::trafo` (identical to the slicer). An
   unpainted volume is skipped unless NONE passes the filter. Per layer the polygons of all volumes are
   concatenated in volume order, **not united** (as the header says); `slice_modifiers` likewise (the slicer
   unites overlapping support volumes per layer).
4. Both projection functions call `cancel` per volume and pass it to `slice_mesh_slabs` / `slice_mesh_ex`; an
   empty `ThrowOnCancel` is accepted (no-op). Parallel over volumes with `tbb::parallel_for`, results in
   indexed slots; exceptions (`CanceledException`) propagate to the caller.

## Notes for dependent work packages

- **WP1b (paint):**
  - A `CanceledException` thrown from a predicate leaves the tree valid (tested: serialize/deserialize round
    trip, manual paint intact, painting again works). It skips `remove_useless_children()` and the garbage
    collection for the interrupted facet, and **the paint rule stays set** — clear it or discard the selector
    (private selectors per volume: discard).
  - `select_patch()` with an empty `seed_facets` list is a no-op, but `facet_start` must still be a valid
    original facet (asserted); guard `seeds.empty()` as the plan says.
  - The edge limit is in world units (`use_world_coordinates`); leaves still crossing the region border at
    the edge limit stay unpainted (under-approximation by at most one edge limit). With a non-convex region,
    `classify` must return 3 only when the triangle is certainly inside (plan §4).
  - Re-runs: "clear all smart paint (`remap_states(clear_causes_map(cbAll))`) + paint the same region"
    reproduces the same tree (tested 3×); the rule never splits manual leaves, so manual paint is preserved
    exactly (areas ± 1e-6 mm², leaf counts equal).
- **WP2a:** `manual_enf` (plan §3.5) = `project_states` with `[](s){ return s == ENFORCER; }` (state 1 only:
  smart paint of an earlier run is not manual) united with `slice_modifiers(true)`; `manual_blk` likewise with
  BLOCKER / `false`. The outputs are not united internally (polygons may overlap): unite them.
- **WP3:** the R1 fix in `GLGizmoPainterBase.cpp` is still needed (`iva_seed_fills_data[tr_state]` with states
  3..9); libslic3r seed fill works with auto states (tested).
- **Slicer:** only `project_and_append_custom_facets(false, ENFORCER, …)` changed; BLOCKER and seams are
  unchanged.

## Results (sandbox, dev/2.9.6 merged in by `wp-build.sh`)

- `libslic3r_tests "[sas-selector],[sas-3mf]"`: 9 test cases / 2156 assertions passed.
- `fff_print_tests "[sas-slicer]"`: 4 test cases / 1069 assertions passed (incl. the `[slow]` organic
  support case).
- Full suites (once): `libslic3r_tests` 244 test cases / 97312 assertions (243 passed, 1 failed as expected —
  the existing `[!shouldfail]` arc welder case); `fff_print_tests` 141 test cases / 215201 assertions passed.
- No compiler warnings in the changed or new files.
