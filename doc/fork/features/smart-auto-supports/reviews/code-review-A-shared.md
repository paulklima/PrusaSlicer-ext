# Code review A: shared upstream code paths

Branch `feature/smart-auto-supports` at `19ec04ce3d`, diff `version_2.9.6..HEAD`, limited to the files that run
for every user: `TriangleSelector.{hpp,cpp}`, `Model.{hpp,cpp}`, `PrintObject.cpp`, `Print.cpp`, `Format/3mf.cpp`,
`GLGizmoPainterBase.cpp`, plus `SmartSupportStates.hpp`, `SmartSupportsProjection.cpp`, `SmartSupportsPaint.cpp` and
their tests. I checked everything by reading the code against the upstream callers (`TriangleMeshSlicer.cpp`
`slice_facet_with_slabs<false>()`, `SupportMaterial.cpp`, `TreeSupport.cpp`, `PrintApply.cpp`, `Sidebar.cpp`,
`FullSpectrum*`). I did not run anything: `build/` belongs to another checkout (`/home/paul/dev/Tests/PrusaSlicer`).

## Critical

None found.

## Major

None found.

## Minor

### M1: Every 3MF load holds one more per-triangle `std::string` vector

`Format/3mf.cpp:387` and `:2156`. `custom_supports_ext` gets a `push_back` for every `<triangle>` of every file,
including stock files that never contain the attribute.

- **Cost.** A `std::string` is 32 bytes in libstdc++, and the vector doubles its capacity as it grows. A
  2 M-triangle project therefore needs about 64 MB more, and up to about 100 MB for a moment while the vector
  reallocates. This happens on top of the four upstream vectors of the same kind. It is the only cost this feature
  adds to all users, on a path every user runs.
- **Related robustness issue.** At `:2667-2676` a volume uses its fork attribute for all triangles as soon as any
  one triangle has it. If a triangle has only the standard attribute, its paint is silently dropped. Fork-written
  files cannot contain such a triangle, but a 3MF edited by an external tool can.
- **Fix.** Store the attribute sparsely, for example `std::vector<std::pair<int, std::string>>` keyed by the global
  triangle index and appended only when the attribute is present. In `_generate_volumes()`, choose per triangle:
  the fork string if present, otherwise the standard one. For every file the fork writes, this gives the same result
  as today.

### M2: `has_facets_of_states()` can disagree with what is actually loaded

`Model.cpp:1610-1644`, used by `Print.cpp:712` (the `_SUPPORTS_OFF` warning) and `3mf.cpp:3359` (whether to bake on
export). The upstream `TriangleSelector::has_facets(data, ENFORCER)` walks each triangle's tree from its
`bitstream_start_idx` and stops at the end of the tree. The new scan reads the whole bitstream linearly and treats
every nibble after a split code as a node.

- **Failure.** A `custom_supports` string with data after the end of its tree, for example `"48"`. Deserialization
  reads only the leaf `8` (BLOCKER) and ignores the trailing `4`. The linear scan also reports an ENFORCER leaf. The
  result is a `_SUPPORTS_OFF` warning for an object that has only blockers, which stock 2.9.6 does not show.
  `has_auto_states()` can also turn true in the same way. That only causes a redundant bake and fork attribute, so
  it is harmless.
- **Who is affected.** Only malformed or hand-made files. `serialize()` and `set_triangle_from_string()` never produce
  trailing data.
- **Fix.** Iterate `triangles_to_split` and walk each tree with the same depth counter that
  `TriangleSelector::has_facets()` uses, testing against the mask. The cost is the same and the result is identical
  to deserialization. Or simply call the upstream `has_facets()` once per state for the `validate()` call, so the
  stock warning stays exactly as before.

### M3: The paint rule (and the predicate cursor) is not cleared when `select_patch()` throws

`SmartSupportsPaint.cpp:474-478`, `TriangleSelector.hpp:441-442`.

- **Failure.** `PrismPredicate::classify()` calls `cancel()`, which can throw `CanceledException` in the middle of
  `select_patch()`. `clear_paint_rule()` is then skipped. `m_cursor` also keeps the `PredicateCursor`, whose closures
  refer to `planes`, `regions[r]` and `cancel`: locals of `paint_placements()` that go away during unwinding.
- **Today this is harmless.** The selector is a local in `paint_volume()` and is destroyed during unwinding, and the
  test at `test_smart_supports_selector.cpp:395` clears the rule by hand.
- **Latent risk.** Any future caller that paints a live GUI selector this way (for example a fast path in
  `SmartSupportsPanel`) would be left with `tool_paint_rule` still set after a cancel. Every later manual brush stroke
  would then go through the rule: NONE becomes auto(bit), the requested ENFORCER, BLOCKER or NONE is ignored, and the
  erase stroke would paint auto states instead of erasing.
- **Fix.** Clear the rule in an RAII guard: either a small scope guard in `paint_volume()`, or better, take the rule
  as an argument of `select_patch()` (or a `PaintRuleScope` helper) so it can never outlive the call. Also call
  `m_cursor.reset()` on exit from `select_patch()` when the cursor is a `PredicateCursor`.

### M4: `FacetsAnnotation::reset()` now clears `used_states` (not byte-identical, benign)

`Model.cpp:1687-1693`. This is deliberate (plan §5 and plan-review-2 N3) and applies to all four annotations,
including `mm_segmentation_facets`, whose `used_states` are extruder ids.

- **Readers are safe.** Every reader of `used_states` is guarded by `is_mm_painted()` / `!empty()`:
  `PrintApply.cpp:1611`, `Model.cpp:1530`, `Sidebar.cpp:124/522`, `FullSpectrumDialog.cpp:1793`,
  `VirtualExtruder.cpp:1166`. So there is no change for MMU users. The `minimum_required_painting_version()` written
  to the 3MF metadata even gets more accurate: a reset volume that once used states ≥ 17 no longer forces version 2.
- **Observable difference.** `FacetsAnnotation::set()` compares `used_states` too. Take a volume loaded from a 3MF
  (`used_states[NONE] = true`) whose paint was then removed from the object list. On the next stroke that changes
  nothing, `set()` now returns true and calls `touch()`, which reschedules the support step. A volume that was never
  painted already behaves this way in stock.
- **Action.** None is required. If strict identity is wanted, `reset()` could set `used_states[NONE] = true` instead
  of clearing it.

## Checked, no finding

- **TriangleSelector without a rule.** `select_triangle_recursive()` (`TriangleSelector.cpp:1082-1123`) behaves
  exactly as 2.9.6: every changed expression falls back to `type` when `m_paint_rule` is empty. Nothing in the GUI
  sets a rule (`grep set_paint_rule`: only `SmartSupportsPaint.cpp` and tests). Brush, seed fill, bucket fill, smart
  fill, `set_facet()`, `remap_states()`, `serialize()`/`deserialize()` and undo/redo (cereal of
  `TriangleSplittingData`) are unchanged.
- **Rule path.**
  - Protected or already-painted leaves return before `split_triangle()`.
  - A fully covered split triangle keeps its subdivision and is merged by `remove_useless_children()` in
    `select_triangle()`.
  - Neighbour midpoints and T-joints go through the unchanged `perform_split()`/`triangle_midpoint_or_allocate()`.
  - `apply_paint_rule_recursive()` allocates nothing, so its reference stays valid, and it recurses only as deep as
    the tree already is.
  - The causes are OR-ed through `tool_paint_rule`, and `clear_causes_map()`/`bake_map()` are correct for all masks
    1..7.
  - After a throw the tree is consistent: splits are complete and only the merge is missing.
- **Slicer.** The ENFORCER branch of `project_and_append_custom_facets()` (`PrintObject.cpp:3437-3440`) is reached
  only from `SupportMaterial.cpp:1128` and `TreeSupport.cpp:232`. `seam` is tested first, so seam projection is
  untouched, and BLOCKER is unchanged.
  - With states 0..2 only, `get_facets_strict(states)` filters exactly the same leaves as `get_facets_strict(ENFORCER)`
    in the same traversal, so the output is identical.
  - `has_enforcer_like_facets()` equals `has_facets(ENFORCER)` for well-formed data.
  - The `SmartSupports` helpers are applied only to `supported_facets`; MMU, seam and fuzzy skin never see them.
- **GUI base.** `GLGizmoPainterBase.cpp:1045` maps states 0..2 to the same buckets as before. The MMU gizmo has its own
  `update_render_data()`.
- **3MF without auto states.** When `has_auto_states()` is false the same code runs, and `get_triangle_as_string()`
  (now `push_back` + `reverse`) gives the same digits, so the output is byte-identical. The fork attribute is never
  written. The painting version stays 1. Files without the attribute load through the standard attribute as before.
- **3MF with auto states.** Baking never creates NONE, so the baked and fork strings cover the same triangles.
  `triangles_to_split` from `serialize()` is sorted, as `triangle_as_string()`'s `lower_bound` requires. Stock
  ignores the unknown attribute.
- **Slab conventions.** `slab_of()`/`slab_range()` in `SmartSupportsPaint.cpp` match `slice_facet_with_slabs<false>()`:
  - a piece in (z_{k-1}, z_k] goes to layer k;
  - a horizontal facet on plane k goes to layer k+1;
  - nothing above the top plane is projected.

  `to_slicing_frame()` reproduces `transform_mesh_vertices_for_slicing()` bit for bit in z. The XY rounding changes
  the analysis by at most one scaled unit and does not touch the slicer.
- **Threads.** The new `const` methods (`get_facets_strict(states)`, `has_*`, `get_data_auto_baked()`) each use a
  local selector or a local mask and touch no shared mutable state. `paint_placements()` and `project_states()` give
  each worker its own selector, and their inputs are copies (`VolumeInput::paint`, `shared_ptr` mesh).
