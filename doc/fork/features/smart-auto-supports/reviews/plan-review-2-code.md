# Plan review 2 — integration with the PrusaSlicer 2.9.6 code

Reviewer #2 (code integration). Plan: `notes/plan.md` (draft 2026-10-15). Every reference below
was read in `version_2.9.6` (checked-out tree) or in `dev/2.9.6` via `git diff`.

## Verdict

The integration is feasible as planned and needs **no blocker fix**. The TriangleSelector hook,
the slicer projection, the 3MF bake and load, and the GUI and job APIs all fit the real code.
**Four Major findings** must be fixed in the plan before WP0 freezes the headers. All four are on
the GUI/job side (consistency check, instance → PrintObject mapping, cancellation, a GUI-thread
throw). The Minor findings are mostly precision fixes for WP0/WP1/WP3.

Checked and found correct (no action needed):
- `Cursor` virtual signatures. The protected ctor, `use_world_coordinates`, `m_edge_limit`, nested
  access to protected `Triangle`/`Vertex` and the `HeightRange` precedent match
  (`TriangleSelector.hpp:102-142, 249-267`, `.cpp:2168-2188, 2409-2419`).
- `select_patch` processes every entry of `get_facets_to_select()` even when `is_facet_visible`
  returns false. Only neighbour expansion is gated (`.cpp:258-276`), so a complete seed list needs
  no BFS.
- Applying the rule without `undivide_triangle` keeps the tree invariants:
  - Non-leaf `state` is never read (`get_state()` asserts `!is_split()`, `.hpp:459`).
  - `number_of_splits` and `m_invalid_triangles` stay untouched.
  - T-joints from unsplit protected leaves are already handled by `get_facets_split_by_tjoints`
    (`.cpp:1672+`) and by midpoint sharing in `perform_split` (`.cpp:1498-1500`).
- States 3–9 serialize as 8-bit leaves and stay at painting version 1 (`.cpp:1876-1892, 2061-2065`).
- The ENFORCER consumers are complete: `SupportMaterial.cpp:1128` and `TreeSupport.cpp:232`, both
  via `PrintObject.cpp:3429-3457`.
  - `Print.cpp:711` is the only enforcer-based warning.
  - Nothing else gates support generation on painted enforcers. `TreeSupport.cpp:241` uses only
    `enforcers_layers.size()`, and `SupportMaterial.cpp:1196` uses only `enforcers_layers`.
  - `GUI_ObjectList.cpp:2947` and `Model.hpp:933` use `empty()`.
- 3MF:
  - Attribute parsing happens only at `3mf.cpp:2149`.
  - Volume creation happens only in `_generate_volumes` (`2576-2676`, called at 929 and 999).
  - Export happens only at `3353-3359`.
  - Per-volume `use_ext` can be implemented over `[first_triangle_id, last_triangle_id]`.
  - AMF stores no paint.
- Jobs:
  - `queue_job(Worker&, ProcessFn, FinishFn)` / `replace_job` exist (`Worker.hpp:58-103`).
  - `Job::process(Ctl&)` and `finalize(bool, std::exception_ptr&)` exist (`Job.hpp:46-56`).
  - `Plater::get_ui_job_worker()` exists (`Plater.hpp:179`).
  - `reslice_FFF_until_step(PrintObjectStep, const ModelObject&, bool)` exists (`Plater.hpp:240`).
- ImGui 1.83 (`imgui.h:63`) has `BeginTable`, `ImGuiTableFlags_BordersH/ScrollY` and
  `CollapsingHeader`.
- The wrapper API exists: `ImGuiPureWrap::combo/checkbox/radio_button/button` and the
  `slider_float(..., const wxString& tooltip)` signature.
- `ObjectList::update_and_show_object_settings_item()` exists (`GUI_ObjectList.hpp:381`).
- AppConfig `get/set/get_bool` exist, and AppConfig keeps arbitrary sections
  (`AppConfig.cpp:380-402`).
- The thread-safety claim holds: after `set_done(posSlice)` only `lslices_ex[].islands` is written
  (`PrintObjectSlice.cpp:553-578`, `Layer.cpp:1011-1014`). Meshes are immutable
  `shared_ptr<const TriangleMesh>` (`Model.hpp:807-816`).
- `dev/2.9.6` touches none of the modified files except `src/libslic3r/CMakeLists.txt`, in separate
  hunks (top, after `add_library`, and a new `-Wno-overloaded-virtual` block). It also does not
  touch `src/slic3r/CMakeLists.txt`.

---

## Blocker

None.

## Major

### M1 — §2.4 input consistency check never passes when shrinkage compensation is set
- **Problem:** the plan requires that the linear part of `po->trafo()` equals the linear part of
  the live instance matrix.
  - `po->trafo()` is `get_matrix_with_applied_shrinkage_compensation(shrinkage_compensation)`
    (`PrintApply.cpp:177-183`, `Geometry.cpp:642-650`).
  - With `filament_shrinkage_compensation_xy/z ≠ 0` the two are never equal.
  - The result is an endless "Mismatch → reslice and keep waiting" loop.
- **Fix:** compare the Print copy's instance with the live one by id:
  - find `mi_copy` in `po->model_object()->instances` with `mi_copy->id() == live_mi->id()`;
  - require `mi_copy->get_matrix().isApprox(live_mi->get_matrix())`;
  - keep the volume id, mesh pointer and volume matrix checks.
  - Alternatively, compare against
    `live_mi->get_transformation().get_matrix_with_applied_shrinkage_compensation(print.shrinkage_compensation())`
    with XY translation zeroed.
  - `Print::shrinkage_compensation()` is public (`Print.hpp:745`).

### M2 — §2.2 / D15: finding "the PrintObject of the selected instance" is wrong in two ways
- **Problem (a), pointer comparison:** `PrintInstance::model_instance` points into the Print's
  private model copy (`PrintApply.cpp:185`, built from `m_model.objects` at `:1518-1520`). It is
  never equal to the live `mo->instances[i]`.
- **Problem (b), the PrintObject may never be sliced:**
  - With background processing **off** (the default), `reslice_until_step_inner` sets
    `single_model_instance_only = true` and `to_object_step` (`Plater.cpp:6582-6583`).
  - `set_task_impl` then enables steps only for the **first** PrintObject whose
    `model_object()->id()` matches (`PrintBase.hpp:639-679`).
  - PrintObjects come from a `std::set` ordered by trafo (`PrintApply.cpp:171-187`), so for
    instances with different rotations the selected instance's PrintObject may never reach posSlice.
  - Result: WaitingForSlices → Failed every time (checklist #14).
  - In addition, a selected **non-printable** instance has no `PrintInstance` at all
    (`PrintApply.cpp:176`).
- **Fix:**
  - Map the instance by `pi.model_instance->id() == live_mo->instances[idx]->id()`.
  - If that PrintObject is not `is_step_done(posSlice)` but another PrintObject of the same
    ModelObject is, analyse that one. Report `SeveralPrintObjects` with the analysed instance index.
  - If the selected instance is not printable, use the first printable one.
  - Switch the active bed for the **analysed** instance, not for `instances.front()` (the stock code
    does the latter at `GLGizmoFdmSupports.cpp:538-544`).
  - `set_task_impl` asserts that the object is on the active bed's print (`PrintBase.hpp:649`).

### M3 — §3.1 `build_input`: `po.slice_support_enforcers()/blockers()` can throw on the GUI thread
- **Problem:**
  - `slice_support_volumes` slices with `print->throw_if_canceled()` as its cancel callback
    (`PrintObjectSlice.cpp:1049-1055`).
  - Between a background stop (`cancel()`) and the next restart, the Print is in canceled state.
  - In that state, an object with enforcer or blocker **modifier volumes** throws
    `CanceledException` inside `build_input` on the UI thread. That exception is uncaught.
  - It also slices meshes on the GUI thread.
- **Fix:**
  - Copy those volumes into `AnalysisInput` as
    `{shared_ptr<const TriangleMesh>, trafo_centered()*mv->get_matrix(), type}`.
  - Slice them in the worker with
    `slice_mesh_ex(mesh.its, zs, MeshSlicingParamsEx{trafo}, cancel)` (`TriangleMeshSlicer.hpp`).
  - Replace `enforcer_volumes`/`blocker_volumes` in the frozen struct accordingly.

### M4 — §6.2 cancel path shows an error dialog, and Cancel can hit someone else's job
- **Problem (a), error dialog on cancel:**
  - A cancelled run throws `CanceledException`. `BoostThreadWorker` stores it in `eptr` and sets
    `canceled` (`BoostThreadWorker.cpp:58-66`).
  - After our `finalize`, `PlaterJob::finalize` shows "An unexpected error occured: Background
    processing has been canceled" whenever `eptr` is still set (`PlaterWorker.hpp:86-90`).
  - The plan says "canceled → Idle" but does not clear `eptr`, so every cancel shows the dialog
    (checklist #11).
- **Problem (b), cancelling the wrong job:**
  - `Worker::cancel()` cancels whatever job is **current** (`Worker.hpp:28-32`). The UI worker is
    shared (arrange, rotate-optimize, emboss…).
  - Cancelling from `on_shutdown` or from the panel while our job is merely queued kills an
    unrelated job.
- **Fix:**
  - In `finalize`: `if (canceled || is CanceledException) { eptr = nullptr; state = Idle; return; }`.
  - Keep a panel-owned `std::shared_ptr<std::atomic<bool>> cancel_flag` that `throw_on_cancel`
    checks together with `ctl.was_canceled()`. Set it on Cancel or close.
  - Call `get_ui_job_worker().cancel()` only while a `running` flag (set at the start of `process`)
    says our job is current.

## Minor

### N1 — §4 `classify(tri) == 3` is not a sufficient "fully inside" test
- **Problem:** "all vertices inside + no projected edge crosses ∂R_k + centroid inside" misses a
  hole or island of `R_k` lying strictly inside a large projected triangle.
  - That case is realistic on 2-triangle shelf bottoms, where R contains holes from `blocker ⊕
    clearance`. The whole triangle then gets painted.
  - Rule protection covers only existing blocker leaves, not the clearance ring.
- **Fix:** additionally require that no vertex of `R_k` (any contour or hole) of a spanned slab
  lies inside the projected triangle. Equivalent: `diff(tri_xy, R_k)` is empty for each spanned
  slab.

### N2 — §4 hook details for WP1
- The `!triangle_splitting` branch (`TriangleSelector.cpp:1096-1099`) must also call `rule(old, type)`.
- `PredicateCursor` methods must be defined in `TriangleSelector.cpp`, because
  `Cursor::transform_triangle` is `inline` there (`.cpp:1234`).
- Guard `seeds.empty()` before `select_patch(seeds[0], …)`, which asserts
  `is_original_triangle(facet_start)` (`.cpp:245`).
- The rule hook in `seed_fill_apply_on_*` is unnecessary (GUI never sets a rule); drop it or keep
  it trivial.

### N3 — `has_auto_states()` based on `used_states` is unreliable
- **Problem:** `FacetsAnnotation::reset()` clears the bitstream but not `used_states`
  (`Model.cpp:1609-1614`). After "Remove all", "Automatic painting" or a mesh repair it stays true.
  - The 3MF bake then runs needlessly.
  - The GUI would enable "Clear smart paint" wrongly.
- **Fix:** scan the bitstream (`TriangleSelector::has_facets(m_data, s)` for s = 3..9, or a
  single-pass variant). Optionally also call `m_data.reset_used_states()` in `reset()`; WP1 owns
  `Model.cpp`.

### N4 — `VolumePaint::changed = (data != v.paint)` compares `used_states` too
- **Problem:** `operator==` includes `used_states` (`TriangleSelector.hpp:297-301`). A
  deserialize → serialize round trip can differ only there, because `set_triangle_from_string`
  flags NONE for empty triangles (`Model.cpp:1647-1651`).
  - The result is a spurious "changed", a needless snapshot, and posSupportMaterial invalidation.
- **Fix:** compare `triangles_to_split` and `bitstream` only.

### N5 — `gizmo_event` override (D18, §6.2)
- **Problem (a), access:** it must be **public** in `GLGizmoFdmSupports`. `GLGizmosManager.cpp:296`
  calls it through `GLGizmoFdmSupports*`.
- **Problem (b), returning false:** it hands LeftDown to the canvas selection and move logic
  (`GLCanvas3D.cpp:3933-3980`, sets `move_volume_idx`).
- **Fix:** while busy, return **true** for LeftDown/RightDown/Dragging/LeftUp/RightUp and pass
  wheel and clipping events to the base.
- Also disable the existing "Automatic painting", "Remove all" and "Enforce by angle" buttons while
  busy. Otherwise `auto_generate`'s reslice is silently dropped by `restart_background_process`
  while the UI worker is busy (`Plater.cpp:2663-2666`).

### N6 — WaitingForSlices failure rule copies a racy stock heuristic
- **Problem:** "neither step done nor update scheduled" (`GLGizmoFdmSupports.cpp:514-516`) is also
  true while the background thread is running.
  - `data_changed` fires mid-slicing via `RELOAD_SCENE` → `update_fff_scene` (`Plater.cpp:3339-3343`).
  - With several objects this can fail falsely before posSlice of our object.
- **Fix:** enter Failed only from the polling path after N consecutive checks, or on a timeout.
  Enter WaitingForSlices inside the `CallAfter`, after `reslice_FFF_until_step` returned, as stock
  does at `:567-570`.

### N7 — §2.3 "no CPU fight" holds only with background processing off
- With background processing on, `reslice_until_step_inner` sets no `to_object_step`
  (`Plater.cpp:6581-6584`), so the full pipeline keeps running next to the job.
- `restart_background_process` only refuses to *start* (`:2663-2666`).
- Document this. It is a performance issue only.

### N8 — Fingerprint and contract inputs (§2.4, §6.3)
- `config_fingerprint(PrintObjectConfig, …)` misses region and print keys the analysis uses:
  `external_perimeter_extrusion_width`, `perimeter_extrusion_width`, `nozzle_diameter`,
  `bridge_angle` (`LayerRegion.cpp:515`), `gcode_resolution`. Include them.
- Building the "effective config" from `preset_bundle->full_config()` every ImGui frame is
  expensive. Cache it and rebuild when the preset or `mo->config` timestamp changes.

### N9 — Header-only `SmartSupportStates.hpp`
- `enforcer_like_states`, `clear_causes_map`, `bake_map` and `tool_paint_rule` are declared without
  bodies.
- In a header-only file they must be `inline` definitions, or the WP split breaks (ODR or link
  errors).

### N10 — Ownership and interface gaps (§2.1 vs §8)
- `TriangleSelectorFdmGui.hpp` and `SmartSupportsPanel.hpp` are listed under WP3a/WP3b in §2.1 but
  under WP0 ("all new headers") in §8. Assign them to WP0 explicitly.
- `SmartSupportsHost` (§6.6) lacks members the panel needs:
  - "Show causes" toggle on the renderer (`set_show_causes(bool)` on `TriangleSelectorFdmGui`);
  - busy query for the gizmo (`bool is_busy() const` on the panel);
  - canvas for `schedule_extra_frame` (`GLCanvas3D&` or `schedule_poll(int ms)`);
  - selected report row for the marker drawn in `render_painter_gizmo`
    (`std::optional<Vec3d> marker_world()`).
- Add these now, so the header freeze holds.
- Marker world position: PO frame → bed needs `+ shift`. In Z, the trafo also contains shrinkage
  scaling, so use the inverse of `po.trafo_centered()` composed with the live instance matrix, not
  plain addition.

### N11 — `TriangleSelectorFdmGui` needs no change to `GLGizmoPainterBase.hpp`; keep it that way
- Override `render(ImGuiWrapper*, const Transform3d&)` and own all GLModels. Use only the
  protected members `m_update_render_data`, `get_seed_fill_color`, `update_paint_contour` and
  `render_paint_contour` (`GLGizmoPainterBase.hpp:56-74`), as `TriangleSelectorMmGui` does.
- The R1 fix in the base (`GLGizmoPainterBase.cpp:1044, 1047`, both index expressions) is then
  defensive only; the FDM gizmo no longer uses the base `update_render_data`.

### N12 — ImGui details (§6.1)
- A `ScrollY` table inside an `AlwaysAutoResize` window needs an explicit
  `outer_size = {window_width, rows*h}` and `ImGuiTableColumnFlags_WidthFixed` columns.
  Otherwise the width feeds back frame to frame.
- Restore the collapse state with `ImGui::SetNextItemOpen(open, ImGuiCond_Once)` before
  `CollapsingHeader`.
- Slider tooltips are `wxString` (`ImGuiWrapper.hpp:85`), so use `_L`, not `_u8L`.
- For wx 3.3 (dev), avoid `char + wxString` and `std::string + wxString`.

### N13 — Tests infrastructure (§7)
- `tests/libslic3r` links only `test_common libslic3r` (`tests/libslic3r/CMakeLists.txt:57`), not
  `test_data.cpp` or arrange.
  - Split mesh helpers (`extrude`, `revolve`, `box`, wedge) into a header with no Print
    dependency, e.g. `tests/smart_supports_meshes.hpp`, which is reachable through the
    `test_common` include dir `tests/`.
  - Keep `init_print_parts` / `run_sas` in `tests/fff_print/`.
- `triangulate_expolygon_3d` returns a triangle soup `std::vector<Vec3d>` (`Tesselate.hpp:21`), not
  an indexed set. The helper must index it.
- `tests/data` contains no figurine. Name an existing model for #21, e.g. `frog_legs.obj` or
  `extruder_idler.obj`, or add one to `tests/data`.
- Confirmed to work:
  - posSlice-only processing: `TaskParams` + `set_task` + `process` + `finalize`
    (`Print.hpp:650-654`, `PrintBase.hpp:666-715`). `Print::process` runs no step-gated work
    besides the conflict checker.
  - `TreeModelVolumes` cross-check, provided the config has support on + organic, because of the
    debug asserts at `TreeSupportCommon.cpp:32-33`.
  - Using miniz in tests: `libslic3r` links `miniz` PUBLIC (`src/libslic3r/CMakeLists.txt:654`).
- Use distinct file names for the two `test_smart_supports_base.cpp` files to avoid confusion.

### N14 — 3MF import details (§5)
- `Geometry::reset()` (`3mf.cpp:389-396`) must clear `custom_supports_ext`.
- Add an `assert(index < custom_supports_ext.size())` next to `2667-2669`.
- Write the ext attribute only when the string is non-empty, as `3353-3359` does.
- Keep the `slic3rpe:` prefix. A new undeclared XML prefix would make the model XML not
  namespace-well-formed for strict 3MF readers.

### N15 — Portability check of the bare feature branch
- The sandbox builds `dev + feature` (wx 3.3, patched `FindwxWidgets.cmake`). The bare feature
  branch cannot be GUI-built with those deps.
- Add a WP5 step: `-DSLIC3R_GUI=OFF` build and tests of `feature/smart-auto-supports` alone on
  `version_2.9.6`. This catches accidental dependencies on dev in libslic3r and the tests.
