# Plan review 6 — code integration, GUI, work packages (round 2)

Reviewer #6, 2026-10-15, on `notes/plan.md` revision 2. Code checked in `version_2.9.6`
(checkout) and `dev/2.9.6` (`git diff`), plus `/home/nhp/sas-deps/wp-build.sh`.

## Verdict

**Ready for WP0: no.** There is one blocker in the WP structure, and several header-level gaps
would force mid-flight header changes. Three round-1 GUI/job fixes are wrong or incomplete
against the real code (window clamp, multi-bed consistency, stale Print state). All of them can
be fixed in the plan text in one pass; no new review round is needed except for M4.

These round-1 items are verified as correctly resolved:
- 2-M2: id mapping. `set_task_impl` slices only the first matching PO (`PrintBase.hpp:640-662`).
- 2-M3: modifiers are sliced in the worker.
- 2-M4: clearing `eptr` suppresses the error dialog (`PlaterWorker.hpp:86-90`). The worker resets `m_canceled` per job (`BoostThreadWorker.cpp:58`).
- 2-N5: `gizmo_event` is reached virtually from `on_mouse`.
- 3-M1, 3-M2, 3-M3, 3-M5, 3-M7.

## Blocker

**B1 — Phase-1 WPs cannot meet their acceptance without WP2a's `build_input` (§2.1, §8.2).**
- `Params` are "filled by build_input()" (§3.1), and `build_input` belongs to WP2a.
- **WP2r:** `Reachability(const AnalysisInput&)` needs real layers plus tip_d, xy, z_top, raft and line width. Its acceptance (cross-check ≤ 5 %) is impossible until WP2a merges.
- **WP1a:** the `project_states`/`slice_modifiers` tests compare against a PO through an `AnalysisInput`.
- **WP1b:** the lower-bound test needs `predict_F` (WP2r) and the input (WP2a), but its dependency column lists only WP1a.
- **WP3:** the fake result paints `VolumeInput`s. The stub `build_input` returns none, so there is nothing to smoke-test.
- **Paint test location:** `tests/libslic3r` links only `test_common libslic3r` (`tests/libslic3r/CMakeLists.txt:57`). The paint test there cannot use the fff_print helpers, because `test_data.cpp` is compiled only into `fff_print_tests`.

**Fix:**
- WP0 implements `build_input`, `choose_print_object` and the `Params` derivation for real (~250 lines, copy only).
- `tests/smart_supports_meshes.hpp` gets `AnalysisInput synthetic_input(const indexed_triangle_set&, double layer_h, const Params&)` (`slice_mesh_ex` plus overlap links) and `Params test_params()`.
- WP2a keeps Phase A/A2.
- Update the dependency columns (WP1b: +WP2r).

## Major

**M1 — §6.1 "Window": the 3-B1 fix does not work as written.**
- The clamp at `GLGizmoFdmSupports.cpp:107-108` uses the constant `scaled(26.3f)`, not the constrained height.
- `y` is the icon row (`GLGizmosManager.cpp:858, 865`), roughly mid-canvas.
- With max height `bottom_limit − top_margin`, the window bottom (new section, Paint button) runs off the canvas. ImGui only clamps auto-fit to the viewport (`imgui.cpp:5254`), so that part cannot be scrolled to.
- **Fix:** keep last frame's `GetWindowHeight()` as `h`. Set `y = max(top_margin, min(y, bottom_limit − h))` and the max height to `bottom_limit − y`.
- §6.2 "left of the gizmo window" puts the report on the gizmo toolbar (`x` = toolbar width, `GLGizmosManager.cpp:865`). Place it right of the gizmo window and pass the gizmo width to `render_report_window`.

**M2 — §2.4: the consistency check never passes on bed 2+ (new, from the 2-M1 fix).**
- `Print::apply` runs inside `with_single_bed_model_fff` (`Plater.cpp:2486-2489`). That shifts instances by `−get_bed_translation(bed)` (`MultipleBeds.cpp:225-238, 326`).
- As a result, `mi_copy->get_matrix().isApprox(live)` fails for every object not on the first bed. The panel waits for the 15-min timeout (checklist 15).
- **Fix:** compare `get_matrix_no_offset()` and offset z. Alternatively use review 2's variant: live `get_matrix_with_applied_shrinkage_compensation()` with XY zeroed vs `po->trafo()`.
- Note that `instance_shift_mm` is then bed-local.

**M3 — §2.2: Paint can analyse a stale Print ("Apply and paint", bed switch).**
- The flow reslices only if posSlice is not done or the input is inconsistent. `po->config()` and the Print copy change only in `Print::apply`.
- "Apply" merely schedules that through a 100 ms timer (`Plater.cpp:2267-2271`). "Apply and paint" therefore runs with the old z gap, style and buildplate_only. The effective-config fingerprint then marks the result fresh.
- After `set_active_bed`, `m_parent.fff_print()` still returns the previous bed's Print until `update_background_process` runs (`Plater.cpp:2423-2425`, `GLCanvas3D.cpp:7652`).
- Negative volumes and modifiers are not part of the consistency check.
- **Fix:**
  - Paint always goes through `CallAfter{reslice_FFF_until_step(posSlice, mo)}`. It runs `Print::apply` synchronously (`Plater.cpp:6566`). Then resolve the Print via `plater->active_fff_print()`.
  - Re-find the PO by ids on every poll; never cache the pointer, because apply deletes POs.
  - Keep the consistency check as a guard.
  - Force the config-cache rebuild after Apply. Otherwise the ≤ 2×/s poll asks "Apply and paint" again.

**M4 — §2.3 vs §3.9: the avoidance pipelining contradicts itself and can deadlock.**
- §2.3: the chain is a `parallel_invoke` task, and Phase B spins (yield) until `avoid_ready ≥ t(n)`.
- §3.9: `base = build_base_model()` (= Phase A), then `parallel_invoke{Phase A, chain}`. Phase A runs twice and nothing is pipelined into Phase B.
- If §2.3 is implemented, a TBB task yield-spinning on another TBB task deadlocks under `global_control(max_allowed_parallelism, 1)` (test #20). A non-cancel exception in the chain also leaves Phase B spinning forever.
- **Fix before WP0:** a pull model. `void ensure_avoidance(int k)` is serial and mutex-guarded, and the serial layer loop calls it before its `parallel_for`. Optionally prefetch on a `std::thread` with exception capture.
- Replace `run_avoidance_chain(int, std::atomic<int>&)` and add `int tip_layer(uint32_t n) const` (t(n)).
- State which `Reachability` methods are thread-safe.

**M5 — Header gaps that will force changes after the freeze (§3.1, §3.2, §6.9).**
- (a) **`ReachCarry::c_hat`** has no `w_n`, but `R = Σ_{k=m+1..n} w_k + δ` includes the current layer, and `push` only stores carried layers.
- (b) **`c_hat` memo:** `c_hat` is `const` but holds the ĉ memo (§3.8), and the parallel cluster loop calls it. Memo ownership and thread-safety are unspecified; per-cluster memos would avoid locks.
- (c) **`ReachField` constructor** lacks C′ (the Direct-mode test is on C′) and the window (`res` comes from `area(win)`). `classify_front` needs `res()`, which has no accessor.
- (d) **WP2b2 synthetic fields** ("synthetic fields in tests") are impossible: `ReachField` is a concrete class whose constructor is a WP2b1 stub. Make the query side an interface (`value`, `res`, `direct_mode`).
- (e) **`build_report(in, plan, residual)`** gets no `BaseModel`, but `TailWithoutDownwardFacets` is per `IslandTree`. Island-tree folding (§3.8 last bullet) has no owner. Either WP2c1 emits both into `Plan.residual`, or pass `BaseModel`.
- (f) **`compute_anchors`** lacks `C_f`: free edges use `Lc − C_f` (§3.8), not `Lc − C′`.
- (g) **`RunOptions`** has no hook for test #22 ("drop one placement before painting").
- (h) **`project_states` / `slice_modifiers`:** the result size is unspecified. A stub returning `{}` makes Phase A's `[n]` overflow. Require `in.layers.size()` entries.
- (i) **`choose_print_object`:**
  - behaviour before slicing (pre-run warning) is undefined;
  - BP on must wait for the selected PO, while BP off falls back. As written, it falls back to any done PO immediately;
  - which bed's Print is used is unspecified.
  - Add a `bool background_processing` parameter and nullptr rules.
- (j) **`Params` defaults:** the config-derived fields have no initialisers, so synthetic tests read garbage. Give them defaults.
- (k) **GUI headers:** `SmartSupportsPanel.hpp` and `TriangleSelectorFdmGui.hpp` are frozen by WP0. Nothing outside WP3 includes them, and WP3 must add private members (state machine, LastRun, config cache, GLModels, min width). Give both headers to WP3, or use a pimpl.
- (l) **Naming:** §2.1 says `slice_support_volumes`, §3.2 says `slice_modifiers`. "Paint state queries" is undefined.

**M6 — WP2c1 does not fit one session (§8.2).**
- WP2c1 holds all of §3.8–§3.10 plus `run()` and the reference test file, in "≈1500" lines.
- It is also the first time five modules meet. The first-subset acceptance needs all of them correct at once.
- **Fix:** split it.
  - **WP2c1a:** cluster evaluator, layer needs and carry, run without placements. Acceptance: #1 max c, #5, #14, and the first-need height of #13.
  - **WP2c1b:** plan loop, stages, verification, `run()`. Acceptance: #2, #13, #15.

## Minor

- **m1 — §6.4 dialog platforms are reversed.**
  - On `_WIN32`, `MessageDialog` is the `MsgDialog` (`SetButtonLabel`). Elsewhere it is `wxMessageDialog` (`MsgDialog.hpp:262-338`), which needs `SetYesNoCancelLabels`. The Windows branch is never compiled in the sandbox.
  - Use a small `MsgDialog` subclass with `add_button(id, focus, label)`, which is identical on all platforms.
- **m2 — WP3 has no link check.** Its acceptance builds only `libslic3r_gui` (a static lib), so missing definitions surface only at link time. Add the `PrusaSlicer` target.
- **m3 — `wp-build.sh --test` can run another WP's binary.** It runs whatever binary is in the shared build dir. If that binary's target is not in the list, it was built from another WP's tree. Rule: targets must include the tested binary, or the script derives them from the test command.
- **m4 — Commit the spec first.** `plan.md` and `reviews/` are untracked. Commit them before creating worktrees, otherwise worktrees at the tip lack the spec.
- **m5 — Shared test headers.** They are WP0-owned. State that WP-specific mesh helpers stay in the WP's own test file, so cherry-picks do not conflict on a shared header.
- **m6 — finalize details.**
  - Skip `commit_paint` when no `VolumePaint.changed` (no empty undo step, no reslice).
  - The cancel branch must not call `wxGetApp().plater()`: it is nullptr when `~BoostThreadWorker` finalizes at shutdown (`MainFrame.cpp:508, 553`).
- **m7 — β slider range.** The range `10–α` is undefined for α = 0 (auto). Use an estimated α_eff.
- **m8 — Asserts are off in the sandbox.** The sandbox is Release (`NDEBUG`), so the TriangleSelector invariant asserts that the no-undivide rule relies on are disabled. Run `[sas-selector]` once in an assert-enabled build.
- **m9 — Rebuild cost when switching worktrees.** ccache is not installed. Every switch between WP trees recompiles WP1a's `PrintObject.cpp`, `3mf.cpp` and `Model.cpp` and relinks the tests. Install ccache, or batch the builds.
- **m10 — Host smoke test of WP3.** Name the tree the user builds: the detached merge commit printed by `wp-build.sh`. Do not merge into dev before the feature is accepted.
