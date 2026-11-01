# Code review C: GUI (feature/smart-auto-supports @ 19ec04ce3d)

Scope: `SmartSupportsPanel.{hpp,cpp}`, `TriangleSelectorFdmGui.{hpp,cpp}`, `GLGizmoFdmSupports.{hpp,cpp}`, the R1 hunk in
`GLGizmoPainterBase.cpp`. The review was done by reading the code against the 2.9.6 sources in the tree (ImGui 1.83,
BoostThreadWorker/PlaterWorker, Plater, ObjectList, MsgDialog). Nothing was run, because the sandbox has no display.

## Critical

None found.

## Major

### M1. A run after a settings or orientation change is committed and shown as up to date

- **Where:** `SmartSupportsPanel.cpp:862-884` (discard checks), `:921-922` (`lr.config_fp` computed in finalize), `:561-562`
  (stale check), `:751-753` (start without checking config).
- **Scenario A:** the user clicks "Fix… → Apply and paint", then presses Ctrl+Z while the analysis runs. The undo
  restores the object config. The ModelObject id, volume ids, shared meshes and paint timestamps all survive the undo
  unchanged. Finalize therefore commits paint that was computed for the applied config.
- **Scenario B:** the user changes layer height or a support option in the sidebar, or rotates the instance, during
  Analysing.
- **Scenario C:** the same change happens during WaitingForSlices before the 100 ms `background_process_timer` applies
  it (`Plater.cpp:2271`). `input_consistent()` ignores config, so `start_job()` builds the input from the stale
  PrintObject.
- **Result in all cases:** `LastRun::config_fp` is taken from the config *after* the run. `poll()` recomputes the same
  value, so "Result is stale: settings changed" never appears.
- **Evidence:** `config_fingerprint()` already covers the instance and volume matrices and all relevant keys
  (`SmartSupportsReport.cpp:183-196`), but it is evaluated at the wrong time. Finalize checks only ids, meshes and paint
  timestamps.
- **Fix:**
  - In `poll_waiting()`, return while `plater->is_background_process_update_scheduled()`.
  - Compute the fingerprint in `start_job()` after a forced `update_effective_config(true)`, capture it in the finalize
    closure and store it in `LastRun`. A later change then shows as stale.
  - Also discard in finalize, as for paint changes, when the analysed instance's `get_matrix_no_offset()` or any volume
    matrix differs from the input.

### M2. "Slicing… N s" can hang for up to 15 min when posSlice is invalidated or fails (background processing off, the default)

- **Where:** `SmartSupportsPanel.cpp:751-769`.
- **Scenario:** background processing is off. The user clicks Paint where needed, then changes a print setting or
  rotates the object while it slices. Alternatively, the object fails in posSlice.
- **What happens:**
  - `update_background_process()` kills the job and does not restart it (`Plater.cpp:2618-2623`).
  - `choice.ready` stays false. The retry (WP3 deviation 6) only runs when `ready && !consistent`, so it never fires.
  - The panel waits for the `WaitTimeout` of 15 minutes. Meanwhile clicks on the object are swallowed and Automatic
    painting, Remove all and Enforce stay disabled.
- **Evidence:** stock Automatic painting handles the same case by giving up when nothing is scheduled
  (`GLGizmoFdmSupports.cpp:553-555`).
- **Fix:** when `!choice.ready` and the print is idle (`!plater->is_background_process_update_scheduled() &&
  s_print_statuses[active_bed] != PrintStatus::running`), re-issue the reslice once (throttled). On the second idle
  observation, or on `PrintStatus::invalid`, fail with a message.

## Minor

### m1. Possible crash at shutdown in the normal (not cancelled) finalize branch

- **Where:** `SmartSupportsPanel.cpp:822-829`. The lambda only checks `alive`.
- **Scenario:**
  - The job finishes, and its Finalize message is queued (with `canceled=false`) before `MainFrame::shutdown()` runs.
    Shutdown calls `cancel_all()` (`MainFrame.cpp:508`) and then sets `plater_ = nullptr` (`:553`).
  - `~BoostThreadWorker` → `wait_for_idle()` (`BoostThreadWorker.cpp:107-115`) delivers that message during
    `~Plater::priv`. The member `m_worker` (`Plater.cpp:364`) is destroyed while the canvas, gizmo and panel still
    exist.
  - `finalize_job()` → `commit_paint()` → `Plater::TakeSnapshot(nullptr, …)` (`GLGizmoFdmSupports.cpp:676`) crashes, as
    does `poll(true)` (`:569`).
- **Note:** plan §2.3 guards only the cancelled branch.
- **Fix:** `if (!alive.lock() || wxGetApp().plater() == nullptr) { eptr = nullptr; return; }`.

### m2. A queued job can be dropped silently and leave the panel stuck at "Analysing… 0 %"

- **Where:** `SmartSupportsPanel.cpp:831`. `queue_job()` does not re-check `is_idle()`; `can_start()` only checked it at
  click time, before slicing.
- **Scenario:** if the user starts arrange or fill bed while the run waits for slices, our job is queued behind it.
  Any `cancel_all()` or `replace_job()` before it starts clears the entry without calling finalize
  (`BoostThreadWorker.hpp:194`, `Plater.cpp:2097, 7354`). The panel stays busy until Cancel is pressed or the gizmo
  closes.
- **Related race:**
  - `cancel_run()` reads `running == true`.
  - Our job then ends and a queued foreign job starts.
  - The following `Worker::cancel()` (`:937-939`) cancels that foreign job.
- **Fix:**
  - Queue only when the worker is idle; otherwise stay in WaitingForSlices and retry.
  - Drop `Worker::cancel()` from `cancel_run()`. `is_canceled` already polls the panel flag, and the bumped run id makes
    finalize ignore the result.

### m3. Paint where needed is allowed while stock Automatic painting is still pending

- **Where:** `can_start()` (`:627-644`) checks only the UI worker. The stock feature runs on the background process
  (`waiting_for_autogenerated_supports`).
- **Background processing off:** our `set_task(to_object_step = posSlice)` (`Plater.cpp:6581-6585`) replaces its
  posSupportSpotsSearch task. Automatic painting then gives up silently, after it has already erased all paint.
- **Background processing on:** the late backend result (`GLGizmoFdmSupports.cpp:437-438` → `apply_data_from_backend`)
  overwrites the smart paint that was just committed.
- **Fix:** expose the waiting flag through the host and disable Paint, with a tooltip, while it is set. This mirrors the
  existing `smart_busy` guard on the Automatic painting button.

### m4. Marker and clip are wrong in the instance-fallback case

- **Where:** `:1576`, `:1590-1595`.
- **Scenario:** the marker is drawn at `instances[lr.analysed_instance]`, but InstancesHider shows only the selected
  instance. When the two differ (fallback, or several rotated instances), the ring floats where nothing is visible.
  "Clip at this height" also mixes the selected instance's offset and radius with the analysed instance's z.
- **Fix:** if analysed ≠ selected, disable marker and clip with a tooltip. Alternatively, map through the selected
  instance when the two matrices without offset are equal.

### m5. Empty undo steps

- **Where:** "Remove smart paint" is enabled from a poll up to 500 ms old (`:1368`), and `commit_paint()` always takes a
  snapshot (`GLGizmoFdmSupports.cpp:676`).
- **Scenario:** the user paints over the last smart facet by hand and clicks Remove within that window. The result is
  a "Remove smart paint" undo step that changes nothing.
- **Fix:** in `remove_smart_paint()`, check the selectors for auto states first, or call `poll(true)` before
  committing.

### m6. A stuck consumed-click flag swallows a later normal stroke

- **Where:** `GLGizmoFdmSupports.cpp:645-661`.
- **Scenario:** if the release of a consumed click never reaches the gizmo (for example on focus loss), the next normal
  stroke loses its Dragging and LeftUp events. The base class keeps `m_button_down` set and does not commit that stroke
  (no snapshot, no `update_model_object()`) until a later stroke.
- **Fix:** clear `m_smart_consumed_click` on any LeftDown/RightDown while not busy.

### m7. Users who never open the section still pay for it

- **Where:** `render_section()` calls `tick()` before the open check (`:1137`, `:1157`). Twice per second this runs
  `full_config()`, `check_contract()` and `choose_print_object()`. `load_settings()` also writes the AppConfig section
  the first time the gizmo opens (`:461`).
- **Fix:** gate `poll()` on `m_section_open || busy() || current_last_run()`.
- **Also:** the window widening (`:176-177`) is intended but visible for every user.

## Checked, no defect found

- **ImGui balance and IDs:** Begin/End and push/pop are balanced on every path, including early returns, disabled
  blocks, CollapsingHeader, TreeNodeEx with NoTreePushOnOpen, BeginCombo and BeginTable/EndTable. There is no nested
  `disabled_begin` (it asserts on nesting, `ImGuiWrapper.cpp:885`). IDs are unique. The table and scroll flags exist in
  1.83. SizeConstraints with AlwaysAutoResize scroll as intended.
- **R1 fix:** the bucket mapping is identical for states 0/1/2, and the base renderer no longer runs for FDM selectors.
  Manual colours, the contour and the seed-fill colours match stock (`GLGizmoPainterBase.cpp:973-1012`).
- **Index mapping:** selectors and volumes map over model parts only. Raycasters use the same order
  (`GLGizmosCommon.cpp:276-279`). Finalize checks the sizes and ids before indexing. There are no out-of-bounds
  accesses in the class buckets.
- **Threading:** the worker touches only its copied input, the result slot and atomics. Status updates are serialised.
  The superseded and cancelled branches clear `eptr`. Errors are shown once by PlaterJob (`PlaterWorker.hpp:86-91`).
  Deferred calls are guarded by `alive`.
- **Undo/redo and settings:** GizmoAction snapshots are used for paint and Action snapshots for settings, as in stock.
  Undo rebuilds the selectors via `on_load` → `m_schedule_update` (`GLGizmoPainterBase.cpp:939-946`).
  `update_and_show_object_settings_item()` goes through `select_item()` with `m_prevent_list_events`, so the canvas
  selection does not change and the gizmo stays open.
- **Dialogs:** the MsgDialog protected API (`add_button`, `content_sizer`, `finalize`) is the 2.9.6 one. Esc and close
  map to `wxID_CANCEL`.
- **Portability:** no implicit conversions between `std::string` and `wxString`. All UI strings are translatable except
  the debug tag.
- **Marker math:** the marker transform matches §3.3. "Clip above z" matches the ClippingPlane semantics
  (`MeshUtils.hpp:48-53`).
