# Implementation plan — smart support painting (v1)

Plan for `feature/smart-auto-supports`. Specification: `notes/theory.md` (cited as §x.y);
background: `notes/concept.md`. Code references are against `version_2.9.6` and were re-checked.

Status: **revision 4, final for WP0** (2026-10-15). Reviewed in three rounds
(`reviews/plan-review-{1..7}-*.md`); the resolution of every finding is in §10. The orchestrator's
rulings are applied. §3.1/§3.2 and the WP0 row of §8.2 are final.

Process: plan → reviews → implementation by agents (§8) → sandbox build + unit tests → the user
tests the GUI on the host. Nothing is merged into `dev/2.9.6` or `fork/2.9.6` until the user says so.

## 0. Summary

v1 adds a **"Paint where needed"** section at the end of the paint-on supports gizmo window. One
button runs a geometric analysis of the object's layer slices (theory §2–§6) as a background job.
It paints **support enforcers only where support is really needed**:
- islands (with tolerance and tail);
- overhangs (theory: cantilevers) by accumulated reach (§2.3).

Bridges and hoop bands (domes, funnels, counterbores) are exemptions. Long spans are planned and
reported, not painted.

A placement counts as supported only where the slicer's own contact would cover it:
- the tip path or the roof path of tree support, mirrored from `TreeSupport.cpp:1084-1426`;
- plus the narrow band that the slicer leaves free on purpose next to the layer below (§3.7).

A separate report window lists everything that is not painted, with a 3D marker.

Auto paint is stored in `ModelVolume::supported_facets` as TriangleSelector states 3–9, which
encode the cause bitmask. The slicer treats them as enforcers. 3MF export bakes them to plain
enforcers for stock PrusaSlicer and keeps the true states in a fork-only attribute. Manual paint
is never overwritten. A run replaces all smart paint with the checked causes.

The analysis is a pure libslic3r module (`Slic3r::SmartSupports`). Its per-layer phase works on
precomputed polygon bands around each front cluster (§3.8), with the avoidance chain pulled on
demand (§3.7). It is unit-tested with Catch2 on sliced test prints and on synthetic inputs. The
GUI is a thin layer: compile- and link-checked in the sandbox, developed against a fake result,
tested on the host.

## 1. Decisions

D1–D6 come from the orchestrator, D7+ from this plan. Orchestrator rulings are marked (O). Each
decision can be vetoed.

| # | Decision | Reason |
|---|---|---|
| D1 | GUI lives in `GLGizmoFdmSupports`:<br>• a collapsible section "Paint where needed" at the **end** of the window;<br>• a one-line entry next to "Automatic painting" opens it;<br>• the window gets a height limit and scrolls (§6.1, clamp from the last frame's height);<br>• the report opens in its own window to the right of the gizmo window. | Review 3 B1, review 6 M1. |
| D2 | Pure libslic3r analysis on an input struct copied on the GUI thread after `posSlice`. It runs as a UI job; `finalize` applies the paint with one undo snapshot. | Research R5. |
| D3 | Auto paint = state `2 + mask` (bits: island 1, cantilever 2, long span 4 → states 3–9). Paint-rule hook in `select_triangle_recursive`. The slicer treats 3–9 as enforcers. R1 fixed. 3MF bake plus a fork attribute. | Undo/redo, copy/split, mesh reset and invalidation come for free; painting version stays 1. |
| D4 | v1 scope = theory §8 "smallest usable capability", with the §3.14 simplifications. | One usable capability. |
| D5 (O) | User parameters live in the panel and in AppConfig. **α is a panel field**. Other theory parameters are constants in `Params`. P7 warnings come with "Fix…". | `support_material_threshold` is greyed out when auto support is off and unused by organic support. |
| D6 | Catch2 tests, tagged `[SmartSupports]` plus a module tag; synthetic inputs for `tests/libslic3r` (§7). | Tag-filtered runs; `tests/libslic3r` links no Print helpers. |
| D7 (O) | Long spans: report only. No checkbox. Report entries have a marker. Rows are planned, never painted. Row painting is the first follow-up. | Theory §8; rare on figurines (H3). |
| D8 (O) | No "suppressed" state. A blocker keeps an area free. A one-line hint says that erased smart paint returns on the next run. | Brush semantics unchanged. |
| D9 (O) | Levels: "Critical only" (must) or "Critical and likely (recommended)" (must + should), default the latter. Needs below the chosen level are not planned, only reported. | Review 3 M1. |
| D10 (O) | Support is decided in **paint space**, through one function set in `Reachability` that mirrors the slicer:<br>• `tips` and `predict_F` (tip path **and roof path**, §3.7), always evaluated on the **union of all painted zones of the layer**;<br>• `cover` = contact plus the slicer's deliberate gap band of width **G = xy_min** next to `L_{n−1}`.<br>The clearance only raises flags. Blockers keep the clearance `tip_d + extra_outset`. | Reviews 1 B1, 5 B1/M1, 7 M1/M2. |
| D11 | `c(p)` uses a raster Dijkstra plus an exact near-anchor zone (ρ = 0.5 mm), a closed-form segment minimiser and exact sub-pixel queries. | H1 accuracy on thin slope fronts. |
| D12 | Overhang placement = the need zone, plate part first. Then widen within `C' ∩ (need ⊕ K)`. Then must-only on the model, in "prefer plate" mode only. Each stage falls through on "no progress". | Theory §3.3, §5.3; reviews 1 M2, 5 m2. |
| D13 | Verification uses the actual paint. It is skipped without an F deficit; otherwise one evaluation pass runs from layer 0 with the actual F. | §7 at near-zero typical cost; review 5 m6. |
| D14 | Staleness = config fingerprint + paint timestamps, in session memory. After a reopen the panel shows "Smart paint present — run again for the report". | No format change. |
| D15 (O) | Instances: analyse the selected instance. With background processing off, use the PrintObject the slicer actually processes (`choose_print_object`, §6.7). Warn before the run. | Reviews 2 M2, 6 M5i. |
| D16 (O) | "Fix…" writes object-level overrides with an `Action` snapshot. It lists the exact changes and offers "Apply", "Apply and paint", "Cancel" (custom `MsgDialog`). | Reviews 3 m13, 6 m1. |
| D17 | Prism painting through `TriangleSelector::PredicateCursor`. `R_k = (∪_{\|j−k\|≤k_d} P_j) ⊕ e`, e = 0.25 mm, `k_d = max(1, ⌈e/h_min⌉)` (worst-case flank). The fully-inside test clips per slab. | Reviews 4 M4, 5 m10. |
| D18 | While busy:<br>• brush mouse events are consumed;<br>• "Automatic painting", "Remove all", angle "Enforce", "Remove smart paint" and "Fix…" are disabled;<br>• finalize discards the result on change. | Reviews 2 N5, 3 m4. |
| D19 | 3MF fork attribute `slic3rpe:ext_custom_supports`. It is written only for volumes with auto states; the loader prefers it per volume. | Stock ignores unknown attributes. |
| D20 | Contract warnings use a cached effective config, rebuilt on change and forced after "Fix…". The analysis uses `po->config()` after a synchronous `Print::apply` (§2.2). | Reviews 2 N8, 6 M3. |
| D21 | Noise filter: ε is a minimal width (opening radius ε/2), applied to the union of the layer's needs. Components < `A_min` are dropped. | No dead band beyond ~1° below α. |
| D22 | `V_min` = 1 mm³, `H_merge` = 1 mm (placeholder constants). | Theory §9. |
| D23 | On-model tips count as built unless the branch is shorter than 1 mm **and** the tip does not carry a roof. With interface layers > 0, roof components are exempt; tip-path contacts are not. | `TreeSupport.cpp:2708-2716` (`supports_roof` from `dtt_roof`); review 7 m2. |
| D24 | `S_n` is offset with jtRound (ArcTolerance 1 µm). | Miter corners give phantom ĉ = 0 (review 4 M2a). |
| D25 | Phase B evaluates each front cluster on **polygon bands precomputed in Phase A**. Grounding is stored as the ungrounded complement `UG_n`. Only touched clusters are re-evaluated. | Reviews 4 B1, 5 M3. |
| D26 | Hoop credit applies only to a front with exactly one hole that is a fully free slice hole, and only within `B_max/2` of that hole. | Review 1 M3; open theory question. |
| D27 | Thin-front chord rule: anchors at both ends plus end closure (≥ 45°). | Review 1 m6; theory amendment. |
| D28 | The GUI WP works against a fake result (hidden AppConfig switch) until the analysis lands. | Review 3 M7. |
| D29 | The determinism rules of §3.12 are binding. | Review 4 M6. |
| D30 | **Interface layers.** The run always uses the **user's** interface setting:<br>• `predict_F` mirrors the roof path (contact = roof polygon, no tip radius, starts `r + xy_min/2` from `L_{n−1}`);<br>• flat overhangs get roofs as usual;<br>• needs that only the roof gap blocks (steep slope fronts, tail rings) get `Why::InterfaceGap`.<br>Only **after** a run with such needs does the report show one problem row with "Fix…", which sets Top interface layers to 0 and lists the side effects (§6.4). The `InterfaceLayers` contract issue is **advisory**: not in the pre-run warning, not in the "Apply and paint" gate. | Reviews 5 M1, 7 M3. Verified `TreeSupport.cpp:1414-1426, 1122-1170`. PrintConfig default is **3** (`PrintConfig.cpp:3436`; "2 (default)" is only the enum label). |
| D31 | The avoidance chain is a **pull model**:<br>• `Reachability::ensure(k)` under a mutex, called by the serial layer loop;<br>• an optional prefetch `std::thread` with exception capture;<br>• cancel-checked waits. | Reviews 5 M2, 6 M4: no TBB task spins on another. |
| D32 | WP0 implements `build_input`, `derive_params`, `choose_print_object` and `input_consistent` for real, plus `synthetic_input()` / `test_params()` for tests. | Review 6 B1: phase-1 WPs must be testable alone. |
| D33 | Paint always goes through `CallAfter{reslice_FFF_until_step(posSlice)}` (synchronous `Print::apply`). The Print is resolved via `plater->active_fff_print()`; the PrintObject is re-found on every poll. | Review 6 M3. |
| D34 | **Tip spacing assumption.** The slicer places tips at intervals (`support_tree_branch_distance`, resampled every `connect_length`, `TreeSupport.cpp:1181-1201, 1258`). The plan models contact as an area. That is valid when `K_max ≥ tip_spacing/2`; an advisory contract note is shown otherwise. The cross-check test is distance-based. | Review 7 M4. |
| D35 | `Reachability` follows the slicer's **`support_material_buildplate_only`** (`Params::buildplate_only`) for forbid and tip status. `PlatePolicy` only selects the planner stages. | Review 7 m4: "Paint anyway" with a mismatch must predict what the slicer does. |

## 2. Architecture

### 2.1 Files

New files (namespace `Slic3r::SmartSupports`; GUI `Slic3r::GUI`). Owners: §8. "WP0 (impl)" means
WP0 delivers the real implementation, not a stub.

| File | Purpose | WP |
|---|---|---|
| `src/libslic3r/Support/SmartSupportStates.hpp` | header-only, `inline`: state encoding, remap maps, tool paint rule | WP0 (impl) |
| `src/libslic3r/Support/SmartSupports.hpp` | public API (§3.1) | WP0 |
| `src/libslic3r/Support/SmartSupportsInternal.hpp` | internal types and functions of all modules (§3.2) | WP0 |
| `src/libslic3r/Support/SmartSupportsInput.cpp` | `derive_params`, `build_input`, `choose_print_object`, `input_consistent` | WP0 (impl) |
| `src/libslic3r/Support/SmartSupportsProjection.cpp` | `project_states`, `slice_modifiers` | WP1a |
| `src/libslic3r/Support/SmartSupportsPaint.cpp` | `paint_placements` | WP1b |
| `src/libslic3r/Support/SmartSupportsBase.cpp` | Phase A (fronts, clusters, bands, θ, hoop candidates, weak contact, unprintable), Phase A2 (trees, tolerance, tail), Feret | WP2a |
| `src/libslic3r/Support/SmartSupportsReachability.cpp` | `Reachability`: collisions, pulled avoidance chain, tips, roof/tip contact, cover | WP2r |
| `src/libslic3r/Support/SmartSupportsRaster.cpp` | `Grid`, rasterise, cells → polygons | WP2b1 |
| `src/libslic3r/Support/SmartSupportsReach.cpp` | `ReachField`, `ReachCarry`, segment minimiser | WP2b1 |
| `src/libslic3r/Support/SmartSupportsTension.cpp` | anchors, domain edges, tension (ties, cuts), chord rule, hoop credit, `classify_front` | WP2b2 |
| `src/libslic3r/Support/SmartSupportsReport.cpp` | `build_report`, contract, `recommended_changes`, `apply_recommended`, fingerprint | WP2c0 |
| `src/libslic3r/Support/SmartSupportsEval.cpp` | `LayerEvaluator`, `evaluate_pass` | WP2c1a |
| `src/libslic3r/Support/SmartSupportsPlan.cpp` | `make_plan`, `verify` | WP2c1b |
| `src/libslic3r/Support/SmartSupports.cpp` | `run()` orchestration, progress, cancel | WP2c1b |
| `src/slic3r/GUI/Gizmos/TriangleSelectorFdmGui.{hpp,cpp}` | renderer with cause colours | WP3 (WP0 adds empty placeholders + CMake) |
| `src/slic3r/GUI/Gizmos/SmartSupportsPanel.{hpp,cpp}` | section, report window, marker, state machine, job, dialogs, fake result | WP3 (WP0 adds empty placeholders + CMake) |
| `tests/smart_supports_meshes.hpp` | header-only, no Print: meshes, `synthetic_input`, `test_params` | WP0 (impl) |
| `tests/fff_print/smart_supports_test_utils.hpp` | multi-part prints, posSlice processing, print cache, `run_sas` | WP0 (impl) |
| test files (§7) | one per module | per module |

Modified files:

| File | Change | WP | dev/2.9.6 conflict risk |
|---|---|---|---|
| `src/libslic3r/TriangleSelector.hpp` / `.cpp` | `PaintRule`, `PredicateCursor`, `get_facets_strict(states)` | WP0 (decl + link-complete bodies, §3.1) / then WP1a (rule hook) | none |
| `src/libslic3r/Model.hpp` / `.cpp` | `FacetsAnnotation` helpers | WP0 (decl + real bodies) / then WP1a | none |
| `src/libslic3r/PrintObject.cpp` | projection ENFORCER ⇒ {1, 3..9} | WP1a | none |
| `src/libslic3r/Print.cpp` | `_SUPPORTS_OFF` warning for auto paint | WP1a | none |
| `src/libslic3r/Format/3mf.cpp` | bake on export, fork attribute, load preference | WP1a | none |
| `src/libslic3r/CMakeLists.txt` | Support source list (lines 355–373) | WP0 | low (separate hunk) |
| `src/slic3r/CMakeLists.txt` | GUI sources (line 82) | WP0 | none |
| `src/slic3r/GUI/Gizmos/GLGizmoFdmSupports.{hpp,cpp}` | host, panel calls, window clamp, busy handling, selector subclass | WP3 | none |
| `src/slic3r/GUI/Gizmos/GLGizmoPainterBase.cpp` | R1 fix, defensive (lines 1044, 1047) | WP3 | none |
| test CMakeLists (2) | new test files | WP0 | none |
| `doc/fork/features/smart-auto-supports/{README.md, notes/theory.md}` | §11 doc changes | WP4 | none |

Not touched: `Plater.cpp`, `ImGuiWrapper.cpp`, `GUI_ObjectList.cpp`, `MsgDialog.hpp` (dev-touched).
Only their public APIs are used. GUI code compiles with wx 3.2 and 3.3: no `char + wxString`, no
`std::string + wxString`, slider tooltips via `_L`.

### 2.2 Data flow (D33)

```
[Paint where needed] (GUI; disabled while busy or the UI worker is not idle)
  ├─ non-advisory contract issues? → once per object: "Apply and paint / Paint anyway / Cancel" (D16)
  ├─ validate print (GLGizmoFdmSupports.cpp:523-528); choice = choose_print_object(active print, mo,
  │  selected instance, background_processing) → switch active bed to the analysed instance's bed
  ├─ CallAfter{ reslice_FFF_until_step(posSlice, mo);   // runs Print::apply synchronously (Plater.cpp:6566)
  │            state = WaitingForSlices }
  ├─ every poll (schedule_extra_frame(100)): print = plater->active_fff_print();
  │     choice = choose_print_object(...)    (never cache the PrintObject*: apply may delete it)
  │     if choice.ready && input_consistent(*choice.po, mo, choice.analysed_instance):
  │         in = build_input(*choice.po, mo, choice.analysed_instance, user_params)   (wxBusyCursor)
  │         queue_job(ui_worker, process, finalize)
[worker] run(in, opt, is_canceled, progress):
           Reachability reach(in); base = parallel_invoke{ build_base_model, reach.compute_collisions }
           reach.start_prefetch(reach.tip_layer(base.last_layer_with_fronts), is_canceled)
           plan = make_plan(in, base, reach)   (pulls avoidance via ensure)
           → test hook → paint (private selectors) → verification (only with deficits) → report
[GUI] finalize: superseded/cancelled → nothing (no plater access in this branch; eptr cleared for CanceledException);
      object/meshes/paint changed → discard + message; no VolumePaint.changed → only LastRun/report;
      else commit_paint("Smart support painting") = TakeSnapshot(GizmoAction) + deserialize + update_model_object()
[slicer] project_and_append_custom_facets(ENFORCER) collects states {1, 3..9}
```

### 2.3 Threading, cancel, progress

**GUI thread.** `build_input` reads:
- `lslices`;
- `lslices_ex[].{bbox, overlaps_below, overlaps_above}`;
- `slice_z`, `print_z`, `height`;
- region flows and configs;
- the live ModelObject: `mesh_ptr()`, paint, matrices.

It does not slice. Modifier volumes are copied as `{mesh_ptr, trafo, enforcer}` and sliced in the worker. After `set_done(posSlice)` only `lslices_ex[].islands` is written, and it is not read.

**Worker.**
- The job owns `shared_ptr<const AnalysisInput>`. It never touches Print, Model or wx.
- Status strings are translated before the job starts.
- Execution order, done by `run()` (review 7 m6):
  - `tbb::parallel_invoke(build_base_model, Reachability::compute_collisions)`;
  - then `reach.start_prefetch(target, is_canceled)`, a `std::thread`;
  - then `make_plan`: the serial Phase B loop calls `reach.ensure(tip_layer(n))` before its `parallel_for` over clusters (D31).

**Avoidance chain.**
- The prefetch thread is the **single writer** up to its target. It uses ClipperUtils only, no TBB; it polls `is_canceled()` per layer and captures any exception.
- `ensure(k)`:
  - `k < 0` → no-op;
  - k ≤ the prefetch target → wait on the condition variable in 50 ms slices, calling `cancel()` between slices and rethrowing a captured exception;
  - k > the target → first wait for the thread to finish, then advance the chain itself under the mutex;
  - without a prefetch → advance under the mutex.
- No TBB task ever waits on another, so `max_allowed_parallelism = 1` cannot deadlock.
- `~Reachability` sets a stop flag and joins the thread.

**Background processing on:** the slicer may run alongside (CPU only).

**Cancel.**
```
is_canceled = [ctl = &ctl, flag = panel_cancel_flag]{ return ctl->was_canceled() || flag->load(); }  // thread-safe
cancel      = [&]{ if (is_canceled()) throw CanceledException(); }       // built inside run() from is_canceled
```
- `panel_cancel_flag` is a `shared_ptr<atomic<bool>>`.
- `Ctl::was_canceled` is atomic (`BoostThreadWorker.hpp:152`), so the prefetch thread may call `is_canceled` (review 7 m5).
- `Worker::cancel()` is called only while our `running` flag is set.
- Checks run per layer, per cluster, every 64k Dijkstra pops, every 4096 cursor predicate calls, in the prefetch loop, and in `slice_mesh_slabs` / `slice_mesh_ex`.

`finalize` (the cancelled branch touches no `wxGetApp().plater()`, which is nullptr at shutdown, review 6 m6):
```
if (run_id != current) { eptr = nullptr; return; }
if (canceled || eptr holds CanceledException) { eptr = nullptr; state = Idle; return; }
if (eptr) { state = Failed; return; }        // PlaterJob shows it
```

**Progress.** A monotone atomic layer counter, throttled to ≥ 100 ms. Split: Phase A 0–15 %, plan 15–80 %, paint 80–90 %, verification 90–100 %.

### 2.4 Consistency, staleness, undo/redo, existing tools

**`input_consistent(po, live_mo, instance)`** (WP0; reviews 2 M1, 6 M2/M3):
- `po->is_step_done(posSlice)`.
- For the analysed instance, the Print copy `mi_copy` is found by id in `po->model_object()->instances`:
  `mi_copy->get_matrix_no_offset().isApprox(live->get_matrix_no_offset())` and offset z equal.
  - This is bed-shift safe: `with_single_bed_model_fff` shifts XY (`Plater.cpp:2486-2489`).
  - It is shrinkage safe.
- **All** volumes (model parts, negative volumes, modifiers, support volumes) match in count, ids, `type()`, `mesh_ptr()` and `get_matrix()`.

`instance_shift_mm` is bed-local.

**Finalize checks:** gizmo `On`, same ModelObject id, same model-part ids and mesh pointers, equal `supported_facets.timestamp()`s. Otherwise discard.

**Undo/redo:**
- `GizmoAction` per run and per "Remove smart paint";
- `Action` per "Fix…";
- undo reloads the selectors; the panel shows stale.

**Staleness:** `LastRun{mo_id, timestamps after apply, config_fp, result}` per object. Checked ≤ 2×/s and on events.

**Existing tools** overwrite auto states (the user wins; tooltip). "Automatic painting" erases all paint → stale. Mesh edits reset paint.

## 3. libslic3r analysis design

### 3.1 Public API (`SmartSupports.hpp`, `SmartSupportStates.hpp`; frozen by WP0)

Conventions for §3.1 and §3.2:
- every per-layer vector has exactly `in.layers.size()` entries;
- polygons are scaled `coord_t` in the PO frame (§3.3);
- functions are pure and thread-safe unless stated otherwise;
- **stub behaviour** (WP0) returns correctly sized, empty, non-throwing results.

```cpp
// SmartSupportStates.hpp — header-only, all inline; WP0 implements completely
namespace Slic3r::SmartSupports {
enum CauseBit : uint8_t { cbIsland = 1, cbCantilever = 2, cbLongSpan = 4, cbAll = 7 };
inline constexpr bool    is_auto_state(TriangleStateType s) { return uint8_t(s) >= 3 && uint8_t(s) <= 9; }
inline constexpr uint8_t auto_mask(TriangleStateType s)     { return is_auto_state(s) ? uint8_t(uint8_t(s) - 2) : 0; }
inline constexpr TriangleStateType auto_state(uint8_t mask) { return mask == 0 ? TriangleStateType::NONE : TriangleStateType(uint8_t(mask + 2)); }
inline constexpr bool    acts_as_enforcer(TriangleStateType s) { return s == TriangleStateType::ENFORCER || is_auto_state(s); }
inline std::vector<TriangleStateType> enforcer_like_states();                       // {1, 3..9}
inline std::map<TriangleStateType, TriangleStateType> clear_causes_map(uint8_t causes); // auto(m) → auto(m & ~causes)
inline std::map<TriangleStateType, TriangleStateType> bake_map();                   // auto(m) → ENFORCER
inline TriangleSelector::PaintRule tool_paint_rule(uint8_t cause_bit);  // NONE→auto(bit); auto(m)→auto(m|bit); else unchanged
}
```

```cpp
// SmartSupports.hpp
namespace Slic3r::SmartSupports {
enum class Cause : uint8_t { Island, Cantilever, LongSpan };          // bit = 1 << int(cause)
enum class Level : uint8_t { May, Should, Must };
enum class PlatePolicy : uint8_t { PreferPlate, PlateOnly };

struct UserParams {                                     // panel + AppConfig (§6.8)
    uint8_t     paint_causes = cbIsland | cbCantilever; // cbLongSpan ignored in v1
    int         alpha_deg    = 0;                       // 0 = auto
    double      K_max = 3.0, B_max = 40.0, B_ceil = 40.0, H_s = 1.0;   // mm
    bool        beta_equals_alpha = true;  double beta_deg = 45.0;
    PlatePolicy plate_policy = PlatePolicy::PreferPlate;
    Level       lowest_level = Level::Should;           // Should or Must
};

struct Params {
    UserParams user;
    // ---- derived by derive_params() (WP0, impl). Defaults: PrintConfig defaults of 2.9.6, nozzle 0.4
    //      (interface_layers = 3 is the PrintConfig default; tests use test_params(), which sets 0).
    double ext_width = 0.45, support_line_width = 0.42;          // mm
    double tip_diameter = 0.8, branch_diameter = 2.0, tree_angle_deg = 40.0;
    double branch_distance = 1.0;                                 // support_tree_branch_distance
    double tip_spacing = 3.2;                                     // connect_length = lw·100/top_rate + max(2r − lw, 0), TreeSupport.cpp:1258
    double xy_distance = 0.225, xy_min_distance = 0.225; bool min_xy_dist = false;
    double layer_height = 0.2, first_layer_height = 0.2, gcode_resolution = 0.0125;
    double contact_distance = 0.2;                                // top contact Z distance
    int    z_top_layers = 1, z_bottom_layers = 1, interface_layers = 3;
    int    raft_support_layers = 0;                               // = TreeSupportSettings::raft_layers.size() (TreeSupportCommon.cpp:147-176)
    bool   buildplate_only = false;                               // support_material_buildplate_only (D35)
    double custom_bridge_angle_deg = 0.0;                         // > 0: θ fixed
    // ---- constants (theory §9 placeholders; mm, mm², mm³)
    double f_hard = 1.5, kappa = 0.10, a_min_widths = 2.0, eps = 0.01, A_min = 0.1, delta_widths = 0.1;
    double V_min = 1.0, H_merge = 1.0, W_stiff = 3.0, H_tail = 16.0;
    double hoop_width_factor = 0.5, hoop_outer_fraction = 0.90, tie_tolerance = 0.10;
    double flag_clearance_extra = 0.2, min_height_to_model = 1.0, minimum_roof_area = 1.0;   // TreeSupportCommon.hpp:131
    double printable_width_factor = 0.5, edge_limit = 0.25, rho_exact = 0.5, cluster_margin = 2.0;
    double grid_res_min = 0.05, grid_res_max = 0.1;  size_t grid_max_pixels = 4'000'000;
    int    max_place_iterations = 4, max_tie_dirs = 8;
    bool   verification = true;
    bool   test_assume_reachable = false;                         // tests only
};

struct ContractIssue {
    enum Key : uint8_t { SupportOff, AutoOn, NotOrganic, EnforceLayers, ContactZero, BuildplateMismatch,
                         InterfaceLayers, Raft, ReachBelowTipSpacing } key;
    bool advisory = false;                              // InterfaceLayers, Raft, ReachBelowTipSpacing: never in the
                                                        // pre-run warning / "Apply and paint" gate (D30, D34)
    std::string current, recommended;                   // serialised option values ("" for info keys)
};
struct LayerInput {
    double slice_z, print_z, height;                    // mm; slab [slice_z − h/2, slice_z + h/2)
    ExPolygons lslices;
    struct Link { int32_t idx; float area; };           // scaled²
    struct SliceInfo { BoundingBox bbox; std::vector<Link> below, above; };
    std::vector<SliceInfo> slices;                      // parallel to lslices
    double ext_width_avg_mm;                            // auto α: w = 0.5·this of layer n−1
};
struct VolumeInput {
    ObjectID volume_id; std::shared_ptr<const TriangleMesh> mesh;
    Transform3d trafo;                                  // po.trafo_centered() * mv->get_matrix()
    TriangleSelector::TriangleSplittingData paint; uint64_t paint_timestamp;
};
struct ModifierInput { std::shared_ptr<const TriangleMesh> mesh; Transform3d trafo; bool enforcer; };
struct AnalysisInput {                                  // immutable after build; shared read-only by all threads
    ObjectID model_object_id; size_t analysed_instance_idx;
    Transform3d po_trafo_centered;                      // marker: world = live_instance · po_trafo_centered⁻¹ · p
    Vec2d instance_shift_mm;                            // bed-local
    std::vector<LayerInput> layers; std::vector<VolumeInput> volumes; std::vector<ModifierInput> modifiers;
    int num_print_objects = 1; bool other_objects_on_bed = false;
    std::vector<ContractIssue> contract;                // check_contract(po->config(), policy, &params) — empty until WP2c0
    Params params;
};

struct Placement {
    uint32_t id; Cause cause; Level level; uint32_t layer;
    ExPolygons area; bool on_model; bool paintable;     // paintable = false: LongSpan (v1)
};
enum class ReportState : uint8_t {
    Unplaceable, BlockedByUser, WillBeDropped, RemainingAfterPaint, OnModelFallback, OnModelTooShort,
    MayMergeOntoModel,                                                          // ← problems
    PartlyOnModel, BelowLowestLevel, LongSpanNotPainted, CauseDisabled, ReliesOnDisabledCauses,
    TailWithoutDownwardFacets, SeveralPrintObjects, OtherObjectsNotConsidered }; // ← notes
enum class Why : uint8_t { None, NotReachable, NoProgress, InterfaceGap, NoTipLayer };
struct ReportEntry {
    ReportState state; Why why; Cause cause; Level level;
    uint32_t layer_first, layer_last; double z_first, z_last;   // print_z
    Point centroid; double slice_z, area_mm2; int count = 1;
};
struct VolumePaint { ObjectID volume_id; TriangleSelector::TriangleSplittingData data; bool changed; };
struct LayerStats  { float max_c = 0; float need_area[3][3] = {}; };   // [cause][level] mm²
struct Result {
    std::vector<Placement> placements; std::vector<VolumePaint> paint;   // paint.size() == in.volumes.size()
    std::vector<ReportEntry> report;                    // §3.11 order, ≤ 50 per group + counts
    int report_more[2] = {};                            // hidden entries per group
    std::vector<LayerStats> layer_stats;                // only with collect_stats
    uint64_t fingerprint = 0; int painted_areas[3] = {};
};
struct RunOptions {
    bool paint = true, collect_stats = false; int verification = -1;   // −1: Params::verification
    std::function<void(std::vector<Placement>&)> test_mutate_before_paint;   // tests (#22)
};
using ThrowOnCancel = std::function<void()>;            // throws CanceledException
using IsCanceled    = std::function<bool()>;            // thread-safe predicate (prefetch thread may call it)
using ProgressFn    = std::function<void(int)>;

// ---- GUI thread (WP0 impl) — all read live Print/Model; never call from a worker
// derive_params mirrors the TreeSupport formulas directly from po.config() / print config / regions; it
// never constructs TreeSupportMeshGroupSettings (asserts when support is off or not tree/organic, review 7 m9).
Params derive_params(const PrintObject &po, const UserParams &up);
struct PrintObjectChoice {
    const PrintObject *po = nullptr; size_t analysed_instance = 0;
    bool fallback = false, ready = false;               // fallback: not the selected instance's PrintObject
    enum class Null : uint8_t { None, NoPrintableInstance, NotInPrint } why_null = Null::None;
};
PrintObjectChoice choose_print_object(const Print &active, const ModelObject &live_mo,
                                      size_t selected_instance, bool background_processing);   // rules §6.7
bool          input_consistent(const PrintObject &po, const ModelObject &live_mo, size_t analysed_instance); // §2.4
AnalysisInput build_input(const PrintObject &po, const ModelObject &live_mo, size_t analysed_instance,
                          const UserParams &up);        // precondition: input_consistent; copies only

// ---- WP2c0 (stubs: empty / 0 / no-op)
// derived == nullptr (GUI, before slicing): config keys only; with derived params also ReachBelowTipSpacing.
std::vector<ContractIssue> check_contract(const PrintObjectConfig &cfg, PlatePolicy policy, const Params *derived = nullptr);
// include_advisory = false: pre-run "Fix…"; true: the after-run InterfaceGap "Fix…" (adds Top interface layers = 0)
std::vector<std::pair<std::string, std::string>> recommended_changes(const PrintObjectConfig &cfg, PlatePolicy policy,
                                                                     bool include_advisory); // (option key, new value)
void     apply_recommended(ModelConfig &object_config, const PrintObjectConfig &effective, PlatePolicy policy,
                           bool include_advisory);
uint64_t config_fingerprint(const DynamicPrintConfig &effective, const ModelObject &live_mo, size_t instance,
                            const UserParams &up);      // §6.8 keys

// ---- worker (WP2c1b; stub: paint = unchanged volumes, changed = false, rest empty).
// Builds cancel = [&]{ if (is_canceled()) throw CanceledException(); } internally; does parallel_invoke,
// start_prefetch, make_plan, paint, verify, build_report (§2.2).
Result run(const AnalysisInput &in, const RunOptions &opt, const IsCanceled &is_canceled, const ProgressFn &progress);
}
```

`TriangleSelector.hpp`. WP0 declares and adds **link-complete bodies** in `TriangleSelector.cpp`, so isolated WP trees link and `PredicateCursor` gets its vtable (review 7 m7):
- `get_facets_strict(states)` is real: the template with a state-mask lambda;
- the `PredicateCursor` ctor and overrides are real: they call the predicates;
- the rule setters are inline;
- the rule **hook** in `select_triangle_recursive` is WP1a.

WP1a owns the file after WP0.

```cpp
using PaintRule = std::function<TriangleStateType(TriangleStateType old_state, TriangleStateType requested)>;
void set_paint_rule(PaintRule rule) { m_paint_rule = std::move(rule); }
void clear_paint_rule()             { m_paint_rule = nullptr; }
indexed_triangle_set get_facets_strict(const std::vector<TriangleStateType> &states) const;
class PredicateCursor : public Cursor {
public:
    struct Predicates {                                 // world frame (trafo applied); called from one thread
        std::function<bool(const Vec3f &p)>                  point_inside;
        std::function<int (const std::array<Vec3f, 3> &tri)> classify;   // 3 = certainly inside, else #inside ≤ 2
        std::function<bool(const std::array<Vec3f, 3> &tri)> touches;
        std::vector<int>                                     seed_facets; // original facets; caller guarantees non-empty
    };
    PredicateCursor(Predicates p, float edge_limit_world, const Transform3d &trafo);   // use_world_coordinates = true
    bool is_mesh_point_inside(const Vec3f &p) const override;
    bool is_pointer_in_triangle(const Vec3f &, const Vec3f &, const Vec3f &) const override { return false; }
    int  vertices_inside(const Triangle &tr, const std::vector<Vertex> &v) const override;
    bool is_any_edge_inside_cursor(const Triangle &tr, const std::vector<Vertex> &v) const override;
    bool is_facet_visible(int, const std::vector<Vec3f> &) const override { return false; }
    std::vector<int> get_facets_to_select(int, const std::vector<Vertex> &, const std::vector<Triangle> &, int, int) const override { return m_pred.seed_facets; }
private: Predicates m_pred;
};
private: PaintRule m_paint_rule; void apply_paint_rule_recursive(int facet_idx, TriangleStateType type);
```

`Model.hpp` (`FacetsAnnotation`). WP0 declares and implements all of them **for real** in `Model.cpp`; they are small:
- `has_*` via `TriangleSelector::has_facets(m_data, s)`;
- `get_data_auto_baked` via `remap_states(bake_map())`;
- `triangle_as_string` is the refactored loop, made linear;
- `reset()` also resets `used_states`.

WP1a owns the file afterwards and only adds the projection/3MF use.

```cpp
indexed_triangle_set get_facets_strict(const ModelVolume &mv, const std::vector<TriangleStateType> &states) const;
bool has_enforcer_like_facets() const;                  // bitstream scan
bool has_auto_states() const;                           // bitstream scan, states 3..9
TriangleSelector::TriangleSplittingData get_data_auto_baked(const ModelVolume &mv) const;
static std::string triangle_as_string(const TriangleSelector::TriangleSplittingData &, int triangle_idx);  // linear
static bool same_paint(const TriangleSelector::TriangleSplittingData &, const TriangleSelector::TriangleSplittingData &); // ignores used_states
```

### 3.2 Internal interfaces (`SmartSupportsInternal.hpp`, frozen by WP0)

```cpp
namespace Slic3r::SmartSupports {
// ===== Projection (WP1a). Result size = in.layers.size(); stub: all empty.
// per_volume.size() == in.volumes.size(); nullptr = use VolumeInput::paint. Parallel over volumes inside.
std::vector<Polygons> project_states(const AnalysisInput &, const std::vector<const TriangleSelector::TriangleSplittingData*> &per_volume,
                                     const std::function<bool(TriangleStateType)> &filter, const ThrowOnCancel &);
std::vector<Polygons> slice_modifiers(const AnalysisInput &, bool enforcers, const ThrowOnCancel &);  // flips if det < 0

// ===== Phase A (WP2a). Stub: layers sized, z/h/w filled, no fronts, no trees.
struct FrontGeom {
    ExPolygon C; BoundingBox bbox; double area_mm2; bool wide;
    std::vector<Vec2d> thetas;            // wide only; ≤ max_tie_dirs, canonical order
    int hoop_hole = -1; double hoop_feret_mm = 0;     // D26
    std::vector<int> slices;
};
struct Cluster {
    BoundingBox win; std::vector<uint32_t> fronts;    // canonical order
    ExPolygons band;                      // L_n ∩ (∪C ⊕ m),  m = cluster_margin + rho_exact + w
    ExPolygons band_prev;                 // L_{n−1} ∩ (∪C ⊕ (m + w))
};
struct LayerGeom {
    const LayerInput *in; double z_bot, z_top, h; coord_t w, w_beta;
    std::vector<FrontGeom> fronts;        // all components of U_n (no area filter), canonical order
    std::vector<Cluster> clusters;        // disjoint windows
    ExPolygons unprintable;               // L − opening(L, ¼·ext_w) (small)
    std::vector<uint8_t> slice_bad; std::vector<int32_t> slice_tree;
    ExPolygons tolerated;                 // T_n
    std::vector<std::pair<int32_t, ExPolygons>> tail_needs;   // per tree (component-wise, w = 0)
    ExPolygons manual_enf, manual_blk;
};
struct IslandTree { int32_t id; uint32_t first, merge; double volume_mm3; bool tolerated;
                    uint32_t tail_last; bool tail_without_downward; };   // merge = UINT32_MAX: never
struct BaseModel { std::vector<LayerGeom> layers; std::vector<IslandTree> trees; uint32_t last_layer_with_fronts = 0; };
BaseModel build_base_model(const AnalysisInput &, const ThrowOnCancel &, const ProgressFn &);   // parallel inside
double feret_diameter(const Polygon &);                                              // scaled
std::vector<Vec2d> bridge_directions(const ExPolygons &bridge_region, const ExPolygons &lower_local, double tie_tol, int max_dirs);

// ===== Reachability (WP2r). Stub: tips_possible = true; tips.bp = F_grown; predict_F = F_grown;
//       cover = F; plate_part = Z; model_part = ∅; ensure/prefetch no-op.
// Indices: coll/avoid are indexed by SUPPORT layer s (object layer + raft_support_layers); s < raft_support_layers
// → ∅ (no object there). tip_layer(n) returns the support-layer index (review 7 m3).
// E arguments are ALWAYS the union of all painted zones of layer n (manual + accepted + trial): the roof
// decision is per union component (review 7 M2).
class Reachability {
public:
    explicit Reachability(const AnalysisInput &);
    ~Reachability();                                                 // sets stop, joins the prefetch thread
    void   compute_collisions(const ThrowOnCancel &);                // parallel; once, before prefetch/ensure
    void   start_prefetch(int target_support_layer, IsCanceled);     // std::thread, single writer up to target
    void   ensure(int support_layer, const ThrowOnCancel &);         // serial caller; k < 0 no-op; k > target: join first,
                                                                     // then advance under the mutex; rethrows prefetch errors
    int    tip_layer(uint32_t n) const;                              // n + raft_support_layers − (z_top + 1)
    bool   tips_possible(uint32_t n) const;                          // tip_layer(n) ≥ max(raft_support_layers − (z_top+1), 1)
    double gap_band_mm() const;                                      // G = xy_min (review 7 M1)
    // const queries; thread-safe for tip_layer(n) ≤ last ensure();
    // window = bbox(arg) ⊕ (tip_d + extra_outset + r + xy + 1 mm)
    ExPolygons F_grown(const ExPolygons &E, uint32_t n) const;
    struct Tips { ExPolygons bp, model, spill_model; };              // status by Params::buildplate_only (D35)
    Tips       tips(const ExPolygons &E, uint32_t n) const;
    struct Contact {
        ExPolygons all, plate, model;                                // the slicer's contact (roof or tip path per component)
        ExPolygons tip_path_all;                                     // contact if every component took the tip path
        bool roof_gap;                                               // some component took the roof path (D30)
    };
    Contact    contact(const ExPolygons &E, uint32_t n) const;
    ExPolygons predict_F(const ExPolygons &E, uint32_t n) const { return contact(E, n).all; }
    ExPolygons cover(const ExPolygons &F, uint32_t n) const;         // F ∪ (L_n ∩ (F ⊕ G) ∩ (L_{n−1} ⊕ G))
    ExPolygons plate_part(const ExPolygons &Z, const ExPolygons &E_layer, uint32_t n) const;
        // Z ∩ cover(contact(E_layer ∪ Z).plate)
    ExPolygons model_part(const ExPolygons &Z, const ExPolygons &E_layer, uint32_t n) const;
        // Z ∩ cover(contact(E_layer ∪ Z).model) − plate_part
    bool       below_model_too_short(const ExPolygons &Z, uint32_t n) const;   // tip-path contacts only (D23)
};

// ===== Raster + reach (WP2b1). Stubs: ReachField = Euclidean nearest seed + ĉ, Direct, res 0.1 mm; c_hat = 0.
struct Grid { Point origin; coord_t res; int cols, rows; std::vector<float> v; std::vector<uint8_t> m; };
Grid       make_grid(const BoundingBox &, coord_t res);             // origin shifted (0.37, 0.61)·res
void       rasterize(const ExPolygons &, Grid &, uint8_t value, bool conservative);
ExPolygons cells_to_expolygons(const Grid &, const std::function<bool(int idx)> &pred);
struct AnchorSeg { Line l; float c0, c1; };                         // ĉ at endpoints, mm
class ReachQuery {                                                   // query interface (synthetic in WP2b2 tests)
public:
    virtual ~ReachQuery() = default;
    virtual float   value(const Point &p) const = 0;                 // mm; finite inside a grounded C'
    virtual coord_t res() const = 0;                                 // sample spacing for classification
    virtual bool    direct_mode() const = 0;
};
class ReachField final : public ReachQuery {                         // immutable after construction
public:
    ReachField(const ExPolygons &cprime /* C' of the component */, const ExPolygons &domain /* D component */,
               std::vector<AnchorSeg> seeds, double delta_beta_mm, const Params &, const ThrowOnCancel &);
    float value(const Point &) const override; coord_t res() const override; bool direct_mode() const override;
    std::shared_ptr<const ReachQuery> cropped(const BoundingBox &bbox) const;   // for the carry (review 5 m8)
};
struct FrontCarry { ExPolygon C; BoundingBox bbox; bool grounded; std::shared_ptr<const ReachQuery> field;
                    ExPolygons zero; std::shared_ptr<const AABBTreeLines::LinesDistancer<Line>> boundary; };
class ReachCarry {
public:
    explicit ReachCarry(const Params &);
    void  push(uint32_t layer, double z_top, coord_t w, std::vector<FrontCarry> &&all_fronts);  // serial; keeps the H_s window
    float c_hat(const Point &a, uint32_t n, double z_top_n, double h_n, coord_t w_n) const;     // pure; thread-safe
};
float segment_min(const AnchorSeg &, const Point &p, double delta_beta_mm);   // §3.8 closed form

// ===== Tension, hoop, classification (WP2b2). Stubs: anchors = material ∂C'; tension None; hoop ∅;
//       classify by c only.
struct Anchors { Polylines material, support, free_edges; };
Anchors compute_anchors(const ExPolygons &cprime, const ExPolygon &C, const ExPolygons &grounded_printable,
                        const ExPolygons &F, const ExPolygons &L);  // all band-local
struct DomainEdges { std::shared_ptr<const AABBTreeLines::LinesDistancer<Line>> anchor, free_; };
DomainEdges domain_edges(const ExPolygons &D_component, const ExPolygons &stop /* GP ∪ F */);
enum class TLevel : uint8_t { Covered, May, Should, Must, None };
struct TensionMap {
    std::vector<std::pair<ExPolygons, TLevel>> regions;   // worst over tied θ
    ExPolygons covered_all;                               // Covered under all θ
    struct Cut { Point p; Vec2d dir; double t; double lo, hi; };   // cut interval [lo, hi] along the span
    std::vector<Cut> cuts;
};
TensionMap tension_levels(const ExPolygons &cprime, const FrontGeom &, const ExPolygons &D_component,
                          const DomainEdges &, const Anchors &, const Params &);
ExPolygons hoop_credit(const ExPolygons &cprime, const FrontGeom &, bool grounded, const Params &);
struct FrontNeeds { ExPolygons need[2][3]; ExPolygons zero; float max_c; };   // [0 cantilever, 1 long span][level]
FrontNeeds classify_front(const ExPolygons &cprime, const ReachQuery &, const TensionMap &,
                          const ExPolygons &hoop_band, float max_chat, const Params &);

// ===== Evaluator (WP2c1a). Stub: evaluate returns empty needs; commit no-op; evaluate_pass empty.
struct LayerNeeds {
    ExPolygons need[3][3];                                // [cause][level] after tail, overrides, filter, folding
    std::vector<std::pair<int32_t, ExPolygons>> folded;   // per unplaced island tree
    std::vector<std::pair<ExPolygons, Why>> why;          // pieces with a known blocking reason (InterfaceGap, NoTipLayer)
    ExPolygons cprime;                                    // ∪ C' (for widening)
    std::vector<TensionMap::Cut> cuts;                    // long-span cut candidates
    LayerStats stats;
};
class LayerEvaluator {                                    // one caller thread; parallel over clusters inside
public:
    LayerEvaluator(const AnalysisInput &, const BaseModel &, const Reachability &, const ThrowOnCancel &);
    // n = last committed layer + 1. E = union of ALL painted zones of layer n (manual + accepted + trial);
    // the evaluator computes contact = reach.contact(E, n) itself (F = contact.all, tip_path_all for
    // InterfaceGap). overrides = (need region, cause) of accepted placements at n. touched = bbox of the
    // contact change since the previous evaluate(n) (nullopt = all clusters). The returned reference is
    // invalidated by the next evaluate()/commit(): callers comparing evaluations copy (review 7 m6).
    const LayerNeeds &evaluate(uint32_t n, const ExPolygons &E,
                               const std::vector<std::pair<ExPolygons, Cause>> &overrides,
                               std::optional<BoundingBox> touched);
    void commit(uint32_t n);                              // UG_n, carry, stats of the last evaluate(n)
    void mark_tree_unplaced(int32_t tree);
};
struct PassResult { std::vector<LayerNeeds> layers; };   // size = in.layers.size(); cprime/cuts cleared
PassResult evaluate_pass(const AnalysisInput &, const BaseModel &, Reachability &,
                         const std::vector<ExPolygons> &E_per_layer /* painted zones */, const ThrowOnCancel &, const ProgressFn &);

// ===== Plan (WP2c1b), report (WP2c0), paint (WP1b)
struct Need { Cause cause; Level level; Why why; uint32_t layer; ExPolygons area; ReportState reason; };
struct Plan {
    std::vector<Placement> placements;
    std::vector<Need> residual;            // EVERYTHING to report: incl. folded trees, tails without downward
                                           // facets (area ∅), PartlyOnModel / MayMerge flags (WP2c1b fills)
    std::vector<LayerStats> stats;         // size layers
    std::vector<ExPolygons> zones_paintable;   // size layers; areas this run paints (paintable && cause checked)
    std::vector<ExPolygons> zones_unpainted;   // size layers; planned but not painted (long spans, unchecked causes)
                                               // the plan itself stays toggle-independent (P3); only this split reads the toggles
};
// The BaseModel is built and the prefetch started by run() before this call (review 7 m6).
Plan make_plan(const AnalysisInput &, const BaseModel &, Reachability &, const ThrowOnCancel &, const ProgressFn &);
std::vector<Need> verify(const AnalysisInput &, const BaseModel &, Reachability &, const Plan &,
                         const std::vector<VolumePaint> &, const ThrowOnCancel &);
std::vector<ReportEntry> build_report(const AnalysisInput &, const Plan &, const std::vector<Need> &verify_residual,
                                      int report_more_out[2]);
// size = in.volumes.size(), volume order; parallel over volumes; stub: unchanged, changed = false
std::vector<VolumePaint> paint_placements(const AnalysisInput &, const std::vector<Placement> &, uint8_t causes,
                                          Level lowest, const ThrowOnCancel &);
}
```

### 3.3 Units and frames

**PO frame.** `po.trafo_centered() * mv->get_matrix()` maps mesh → PO frame in mm. It is the same chain as slicing and the enforcer projection. Polygons are scaled `coord_t` (1 unit = 1 nm); z is in mm in the `slice_z` frame.

**Slabs.** Layer n slab = `[slice_z − h/2, slice_z + h/2)`, half-open. Vertex z is snapped +1e-4 mm, so a downward horizontal facet lands in the slab above (`TriangleMeshSlicer.hpp:74-75`).

**Report.** z as `print_z` + layer number. X/Y bed-local = `unscale(p) + instance_shift_mm`. Marker world position = `live_instance_matrix · po_trafo_centered⁻¹ · (p, slice_z)`.

### 3.4 Parameters

| Name | Source | Default / rule |
|---|---|---|
| α | panel (AppConfig `overhang_angle`; first value from the effective `support_material_threshold`) | 0 = auto: `w_n = 0.5·ext_width_avg(n−1)`; else `w_n = h_{n−1}/tan α` |
| β | Advanced | = α; else `w_β = h_{n−1}/tan β`; β ∈ (0, α_eff], α_eff = α or `atan(h/(0.5·ext_w))` |
| K_max, B_max | panel | 3, 40 mm |
| B_ceil, H_s | Advanced | 40, 1 mm |
| level, policy, causes | panel | Critical and likely; Prefer build plate; islands + overhangs |
| ext_width | max external perimeter width over regions | `TreeSupportCommon.cpp:36-40` |
| support line width | `support_material_flow(&po, layer_height).width()` | |
| tip_d, branch_d, tree angle | `support_tree_*` | |
| xy, xy_min, min_xy_dist | as `TreeSupportCommon.cpp:58, 108`, `TreeSupport.cpp:1252` | |
| z_top / z_bottom | `round(gap / layer_height)` | `TreeSupportCommon.cpp:113` |
| interface layers | `support_material_interface_layers` | contract key (D30) |
| extra_outset | `max(0, tip_r − line_w/2) + (min_xy_dist ? line_w/2 : 0)` | `TreeSupport.cpp:1264-1270` |
| G (gap band) | `xy_min`, for every `z_top`: the largest gap the collision can impose next to `L_{n−1}` (`coll_t ⊇ L_{t−z_bot..t} ⊕ (r + xy_min)`, `TreeModelVolumes.cpp:465, 487-489, 521-526`) | review 7 M1 |
| tip spacing | `connect_length = lw·100/top_rate + max(2r − lw, 0)`; `branch_distance` | `TreeSupport.cpp:1181-1201, 1258` (D34) |
| raft support layers | `TreeSupportSettings::raft_layers.size()` mirrored from `slicing_parameters()` | `TreeSupportCommon.cpp:147-176` |
| constants | `Params` defaults | §3.1 |

### 3.5 Phase A — per-layer geometry (parallel, plan-independent; WP2a)

Whole-layer operations happen only here, in parallel (`L = in.lslices`, `Lp = L_{n−1}`):

```
S = intersection_ex(L, offset(Lp, w_n, jtRound, arcTol 1 µm));  U = diff_ex(L, S)       // n = 0: U = ∅
unprintable = diff_ex(L, opening_ex(L, 0.25·ext_w))
fronts = all components of U (area == 0 dropped only), canonical order
clusters: windows bbox(C) ⊕ (m + w) merged while overlapping (sweep over sorted bboxes);
   band = intersection_ex(L, offset(∪C, m, jtRound)); band_prev = intersection_ex(Lp, offset(∪C, m + w, jtRound))
per front (band-local):
   wide = !opening_ex({C}, ext_w).empty()
   wide: B_C = components of opening_ex(band − band_prev, 0.1·ext_w) intersecting C
         thetas = custom ? {angle} : bridge_directions(B_C, band_prev, 0.10, 8)
   hoop (D26): C has exactly one hole H′; a hole H of the containing slice matches by bbox (±2 µm) and
         area (±1 %); H fully free (≤ 1 % of |H| within expand(S_band, 2 µm)); ≥ 90 % of C.contour within
         expand(S_band, 2 µm) → hoop_hole, hoop_feret = feret_diameter(H)
weak contact per slice (n ≥ 1): Σ below.area < κ·area and the max bbox extent of slice ∩ clip(Lp, bbox) < a_min
manual_enf / manual_blk = project_states(1 / 2)[n] ∪ slice_modifiers(enf / blk)[n]
```

`bridge_directions`: the cost loop of `BridgeDetector.hpp:88-128`, keeping candidates within 10 %,
deduplicated modulo 180°, sorted by `(cost, angle mod π)`, at most 8. PCA minor axis without
floating edges. `feret_diameter`: convex hull + O(h²).

### 3.6 Phase A2 — island trees, tolerance, tail (serial; WP2a)

```
bed[0][*] = true; bed[n][i] = !weak && ∃ below link j with bed[n−1][j]
union-find over non-bed slices (below links) → trees; first; merge = min n of a bed slice linking into the tree
volume = Σ area·h below merge; tolerated = merge exists && z(merge) − z(first) ≤ H_merge && volume < V_min
T_n = union of tolerated tree slices at n
tail per non-tolerated tree, per connected component of its slices:
   for n = first … while n < merge && z_n − z_first ≤ H_tail:
       part = component at n; stop if !shrink_ex(part, W_stiff/2).empty()
       tn = (n == first) ? part : diff_ex(part, clip(L_{n−1}, bbox(part)))   // w = 0
       tn empty (n > first) → tail_without_downward = true
       tail_needs[n] += (tree, tn)
last_layer_with_fronts = max n with fronts, tails or manual enforcers
```

### 3.7 Reachability, contact and cover (§5; D10, D23, D30, D31; WP2r)

```
r = tip_d/2; xy = xy_min; max_move = tan(tree_angle)·layer_height − 2 (scaled)
compute_collisions (parallel over k):
    coll_k = union( offset(union(L_{k−z_bot..k}), r + xy, jtMiter 1.2),
                    ∪_{i=1..z_top} offset(L_{k+i}, r + xy_i) ),  xy_i = xy − (i − (z_top==1 ? 0.5 : 0))·xy/z_top
             stored per k (one global copy), polygons_simplify(gcode_resolution)
avoidance chain (serial, pulled; ensure(k) / prefetch thread):
    avoid_0 = coll_0; avoid_k = union(coll_k, offset(avoid_{k−1}, −step, jtRound, arcTol = gcode_resolution))
    sub-steps 1.9·max(r, xy), last-step stretching (TreeModelVolumes.cpp:670-681), polygons_simplify
    prefetch target = tip_layer(last_layer_with_fronts)
indices: support layer s = object layer + raft_support_layers; coll_s = avoid_s = ∅ for s < raft_support_layers
tip_layer(n) = n + raft_support_layers − (z_top + 1);  tips_possible(n) = tip_layer(n) ≥ max(raft_support_layers − (z_top+1), 1)
forbid_n = buildplate_only ? avoid_t : coll_t      (Params::buildplate_only, D35; relevant_forbidden, TreeSupport.cpp:1323-1328)
E = union of ALL painted zones of layer n (manual + accepted + trial)                     (review 7 M2)
F_grown(E) = offset_ex(union_ex(E ∩ O_n), tip_d) − L_{n−1}                // :289-299   (O_n = L_n − L_{n−1})
tips(E):  !tips_possible → ∅
          T0 = F_grown − (forbid_n ⊕ 5 µm)                                 // overhang_regular, :1336
          Rm = (F_grown ∩ forbid_n) − (T0 ⊕ lw/2)                          // remaining overhang, :1340-1345
          T  = T0 ∪ ((Rm ⊕ extra_outset) − forbid_n)                       // outset grows only the remainder, :1346-1366 (review 7 m1)
          bp = T − avoid_t;  model = buildplate_only ? ∅ : T ∩ avoid_t;  spill_model = model
contact(E)  (per union_ex component K of the roof candidate R = T0, :1414-1426):
   roof path  iff interface_layers > 0 && area(K − forbid_{t−1}) ≥ minimum_roof_area (:1122-1170):
        contact_K = K itself (no ⊕ r, no outset); plate/model split by avoid_t; roof_gap = true;
        model_short does NOT apply (the tip carries a roof)
   else tip path:  contact_K = F_grown ∩ ((tips ∩ K-area) ⊕ r); model part minus model_short (D23, review 7 m2)
   + tip path for the outset extension T − T0
   all = ∪ contact_K; plate / model accordingly; tip_path_all = the tip-path contact for ALL components
model_short_n = (∪_{m=t−k..t} L_m) ⊕ (r + xy), k = ⌈(1 mm + bottom gap)/h⌉      // applied to tip-path contacts only
G = xy_min;  cover(F) = F ∪ (L_n ∩ (F ⊕ G) ∩ (L_{n−1} ⊕ G))             // deliberate gap band (reviews 5 B1, 7 M1)
plate_part(Z, E_layer) = Z ∩ cover(contact(E_layer ∪ Z).plate)
model_part(Z, E_layer) = Z ∩ cover(contact(E_layer ∪ Z).model) − plate_part
flags: area(tips(P).spill_model) ≥ A_min → PartlyOnModel (note)
InterfaceGap attribution (WP2c1a): need points ⊂ cover(tip_path_all) but ⊄ cover(all)
```

**Tip-spacing assumption (D34).** The slicer samples tips along lines `branch_distance` apart,
every `tip_spacing` (= `connect_length`, ≈ 3.2 mm stock / ≈ 1.6 mm Prusa profile). Thin
overhangs fall back to their outline (`TreeSupport.cpp:1181-1201`). The area model "contact
covers ⇒ supported" is valid when the supported material spans gaps of `tip_spacing/2`, i.e.
`K_max ≥ tip_spacing/2`. Otherwise an advisory contract note is shown (`ReachBelowTipSpacing`).
The cross-check test is distance-based (§7.1).

**Window.** Every query is clipped to `bbox(arg) ⊕ (tip_d + extra_outset + r + xy + 1 mm)` (review 5 m3).

**Thread-safety.** `coll` and `avoid` are preallocated vectors. Elements are written before `ready` advances (mutex + release store); queries read only `k ≤ ready`.

**Checks** (interface 0, h 0.2, z_top 1, xy_min 0.225 → G = 0.225, `min_xy_dist` false → extra_outset 0.19; distances from `L_{n−1}`):

| Case | Tips / contact | Cover result |
|---|---|---|
| 40° wall, front [0.2, 0.238] | T = [0.51, 1.04]; contact from 0.11 | the whole front |
| inverted 60° cone, tail ring [0, 0.115] | contact from max(0.1125, 0.225 − s) ⊕ … | G band → whole ring |
| inverted 75° cone, ring [0, 0.054] (s small) | contact from ≈ 0.17 | G band ⊇ [0, 0.054] when adjacent to F ⊕ G → whole ring (#26b) |
| sphere rings r 5.9 / 12.1 | — | whole ring |

With interface layers > 0, roof components start the contact at `r + xy_min/2 + 5 µm` = 0.5175
(`TreeSupport.cpp:1327, 1418`). The 40° front and tail rings stay uncovered → `Why::InterfaceGap`
→ after-run "Fix…" (D30).

### 3.8 Phase B — cluster evaluator (§2.2–§2.5; D25; WP2c1a)

**State.** `UG_{n−1}` (usually ∅), `ReachCarry`, E (painted zones: manual + accepted + trial). Per `evaluate` the evaluator computes `contact = reach.contact(E, n)` once; F = `contact.all`.

**Ungrounded S.**
- Fast path: if `UG_{n−1} = ∅` and the layer has no bad slice → none.
- Else, in the window `W = bbox(UG_{n−1}) ⊕ (w + 1 mm)`:
  `ungroundedS = ((L − U) ∩ W) − offset(clip(L_{n−1}, W ⊕ w) − UG_{n−1}, w, jtRound)` ∪ `(L − U) ∩ bad slices`.
- Computed once per layer, outside the clusters; it is band-local for clusters.

**Per cluster c** (parallel; result in slot c; all band-local):

```
Lc = band; Lpc = band_prev; Fc = F ∩ win; Fcov = reach.cover(Fc, n); Tc = T_n ∩ win
Sc = Lc − ∪C;  GPc = (Sc − ungroundedS − Tc − unprintable) ∪ (Lc ∩ Fcov − Tc)
per front f: C'_f = C_f − Fcov − Tc;  A_f = compute_anchors(C'_f, C_f, GPc, Fcov, Lc)
     material = ∂C' ∩ expand(GPc, 2 µm); support = ∂C' ∩ expand(Fcov, 2 µm); free = ∂C' − expand((Lc − C_f) ∪ Fcov, 2 µm)
     grounded_f = material ∪ support non-empty
Dc = Lc − GPc − Fcov → components containing a grounded C'; DomainEdges per component
ĉ: material anchors resampled every 0.1 mm + every vertex, canonical start; points SNAPPED to a 1 µm grid
   before evaluation; the cluster-owned memo (snapped point → ĉ), reused across evaluate() calls of the layer:
   look up serially → evaluate misses in parallel chunks into indexed slots → insert serially (review 5 m4, review 6 M5b)
   segments between a zero and a non-zero sample split in the middle (constant halves)
ReachField per D component (D11): exact zone min over seed segments within ρ via segment_min;
   Direct iff diff(C', offset(seeds, ρ − 1 µm, jtRound, etOpenRound, arcTol 1 µm)) area < 1e-6 mm²;
   Grid: res = clamp(√(area(bbox(Dcomp))/grid_max_pixels), grid_res_min, grid_res_max) per D component;
         conservative raster (Dcomp ⊕ res/2 at centres); 16-neighbour Dijkstra (knight needs both cells);
         query exact within ρ, else min over 4×4 neighbours; +∞ fallback = Euclidean min over seeds
tension (wide): scanlines per θ (spacing 0.5·ext_w) ∩ Dcomp; endpoint anchor/free via DomainEdges (2 µm);
   band border → free; t, levels, worst over θ; covered_all; cuts with interval [t − B, B] (⌈t/B⌉−1 cuts if t > 2B)
thin (D27): free piece Π with anchors at both ends, max chord deviation ≤ ext_w, end closure ≥ 45° → t = chord
hoop: hoop_credit(C', f, grounded, params) = C' ∩ (H ⊕ B_max·hoop_width_factor) if hoop_feret ≤ B_ceil
early exits: hoop band ⊇ C' or covered_all ⊇ C' → no field (zero = C');
   max ĉ + perimeter(C')/2 + width(C') ≤ K → classification skipped, field kept for the carry
classify_front: samples = pixel centres inside C' (Direct: res 0.1 mm) + ∂C' every res;
   lt, lc, covered, level = min, cause (tie → long span); need = C' ∩ squares; zero = covered_all ∪ hoop ∪ (C_f ∩ Fcov)
carry: all fronts (grounded flag), field cropped to bbox(C) (review 5 m8), zero, boundary distancer
```

**Segment minimiser.** For `f(s) = c0 + g·s + max(0, √(d² + (s − sp)²) − Δ)`, take the minimum over the candidates `{0, len, sp, sp − sign(g)·d·|g|/√(1−g²) (if |g|<1)}`, each clamped to the segment.

**`ReachCarry::c_hat(a, n, z_top_n, h_n, w_n)`:**

```
H_s == 0 → 0
for m = n−1 down while z_top_n − z_top_m − h_n < H_s:
    R = w_n + Σ_{k=m+1..n−1} w_k + δ                      // w_n passed in (review 5 m1)
    near = fronts of layer m (2 mm uniform grid) with dist(a, C) ≤ R
    if near: grounded ⊂ near; none grounded → 0
             else min c*_f(q_f)·max(0, 1 − (z_top_n − z_top_m − h_n)/H_s); stop
return 0
```

**Layer needs** (a `LayerNeeds` object):
- islands: non-grounded `C' − T_n` → (Island, Must);
- tail override: `tail_needs[n] − T_n − Fcov` → (Island, Must), removed from other needs (review 5 B1);
- cause overrides from accepted placements at n;
- `why` pieces:
  - need points inside `cover(contact(E).tip_path_all)` but outside `cover(contact(E).all)` → InterfaceGap (review 7 m8). E = the layer's painted zones; the evaluator receives them with F;
  - layers with `!tips_possible` → NoTipLayer;
- noise filter: opening `ε/2` on the union, drop components < `A_min`;
- folding of unplaced island trees.

**`commit(n)`:**
- `UG_n = ungroundedS ∪ non-grounded fronts' C ∪ T_n − (L ∩ Fcov)`;
- carry push;
- stats.

**`evaluate_pass`** = for n = 1…: `ensure(tip_layer(n))`; `evaluate(n, E[n], {}, nullopt)`; `commit(n)`.

### 3.9 Joint plan (§6, P3/P4, §5.2, P5; WP2c1b)

```
// called by run() after parallel_invoke{build_base_model, compute_collisions} and start_prefetch (review 7 m6)
ev = LayerEvaluator(in, base, reach, cancel)
for n = 1 .. N−1:
    reach.ensure(tip_layer(n))
    E = manual_enf_n                                                // painted zones of the layer (review 7 M2)
    dropped manual tips (tips(E) vs F_grown(E)) → WillBeDropped (why NotReachable / NoTipLayer)
    overrides = []; L = copy(ev.evaluate(n, E, overrides, nullopt))   // copy: the reference is invalidated later
    for cause in [Island, Cantilever, LongSpan]:
      for it in 0 .. max_place_iterations−1:
        needs = L.need[cause][≥ lowest] − folded
        if area(needs) < A_min: break
        progressed = false
        for stage in [plate, widen, model]:                         // fall through on no progress (review 5 m2)
            new = place(stage, cause, needs, L, E)
              plate: Z = cause==LongSpan ? rows(L.cuts, needs) : needs; Z −= blk ⊕ (tip_d + extra_outset)
                     P = plate_part(Z, E) per need component (level = max), keep area ≥ A_min
              widen: components without plate part: Zw = (L.cprime ∩ (N ⊕ K)) − blk ⊕ …; P = plate_part(Zw, E)
              model: PreferPlate only: M = must-level points − blk ⊕ (tip_d + extra_outset); paint area P = M,
                     kept only if area(model_part(M, E) ∪ plate_part(M, E)) ≥ A_min;
                     flag OnModelTooShort if below_model_too_short(M), else OnModelFallback
            if new.empty(): continue
            E' = E ∪ ∪new                                           // the union, never F ∪ predict_F(new)
            touched = bbox(predict_F(E') Δ predict_F(E))
            L' = copy(ev.evaluate(n, E', overrides + (needs, cause), touched))
            if area(L.need[cause]) − area(L'.need[cause]) ≥ A_min:
                accept: E = E'; overrides += (needs, cause); L = L'; flags (PartlyOnModel); progressed = true; break
            else: ev.evaluate(n, E, overrides, touched)             // restore cluster state; discard new (review 1 M1)
        if !progressed: residual(needs) with reason/why (below); break
      (iterations exhausted → residual(rest, Unplaceable, NoProgress))
    residual += L.need below level (BelowLowestLevel); long-span needs → LongSpanNotPainted via their placements
    ev.commit(n); zones_paintable[n] / zones_unpainted[n] from the accepted placements of n
plan.residual += folded tree entries; TailWithoutDownwardFacets per tree (base.trees); MayMergeOntoModel heuristic
```

**Residual reasons** (review 5 m11), applied per piece:
- part inside `blk ⊕ (tip_d + extra_outset)` → BlockedByUser;
- else a known `why` (InterfaceGap, NoTipLayer) → Unplaceable with that why;
- else no stage found a position → Unplaceable / NotReachable;
- else the progress rule failed → Unplaceable / NoProgress.

**`rows(cuts, needs)`:** per cut inside the needs, take the position in `[lo, hi]` nearest the interval centre that lies in `plate_part(cprime, E)`; if none, the centre. Then squares (spacing × 1 mm) → `closing_ex(0.5 mm)`; `paintable = false`.

**P4:**
- Placements only add anchors; θ, hoop and tolerance are fixed; discarded placements leave no trace.
- The progress rule is checked on the true union `predict_F(E ∪ new)`, so every accepted step really reduces the needs.
- Superadditivity of `predict_F` ("more paint never removes contact") holds only for **interface layers 0**. With roofs, merging zones can turn a tip-path component into a roof and push its contact out to `r + xy_min/2` (review 7 M2).
- With interface layers > 0, needs can therefore reappear when a later zone merges with an earlier one. The union evaluation catches this for the zones of the same layer. Verification catches the rest.

**#10 note.** With "Prefer build plate", the must-only on-model zone (c > 4.5) reduces neighbouring c only by `tip_d`. A should band `c ∈ (3, 3.7]` therefore remains and is reported as Unplaceable. This matches the theory (review 5 m9).
*Settled by WP5 (wp-log-wp5.md): wrong. The on-model contact is a support anchor with ĉ = 0 (theory §2.2, §2.3), so the band between stem and contact has c ≤ 1.9 < K_max; nothing remains.*

### 3.10 Verification (D13; WP2c1b)

1. `E_act = project_states(new paint, ENFORCER or auto with a painted bit)`. This already includes manual enforcer paint; enforcer volumes are added from `manual_enf`.
2. `F_plan_n = predict_F(manual_enf_n ∪ zones_paintable_n ∪ zones_unpainted_n)` and `F_act_n = predict_F(E_act_n ∪ manual_enf_n ∪ zones_unpainted_n)`. Both are unions (review 7 M2); unpainted planned zones stay planned (P3).
3. In parallel: `deficit_n = area(cover(F_plan_n) − cover(F_act_n)) ≥ A_min`.
4. No deficit → done.
5. Otherwise `evaluate_pass(E_act ∪ manual_enf ∪ zones_unpainted)` from layer 0. Needs not in the plan residual ⊕ 0.5 mm → `RemainingAfterPaint`.

### 3.11 Report (P8 subset; WP2c0)

**Input.** `build_report` uses `Plan.residual` (complete, see §3.2) and the verify residual. It needs no `BaseModel`.

**Merge rule.** Pieces merge when they share (state, why, cause, level), lie on consecutive layers and have overlapping bboxes ⊕ 1 mm.

**Added entries:**
- `CauseDisabled` per unchecked cause;
- one `ReliesOnDisabledCauses`;
- `SeveralPrintObjects`;
- `OtherObjectsNotConsidered`.

**Groups:**
- Problems: Unplaceable, BlockedByUser (islands), WillBeDropped, RemainingAfterPaint, OnModelFallback, OnModelTooShort, MayMergeOntoModel.
- Notes: the rest.

**Order:** `(group, level desc, area desc, layer, x, y, id)`; ≤ 50 per group, plus counts in `report_more`.

### 3.12 Determinism (D29)

**Parallel output.** Indexed slots, combined serially in index order. Forbidden:
- `concurrent_vector`;
- appends under a mutex;
- `union_parallel_reduce`;
- unordered iteration;
- parallel float sums.

**Canonical orders:**
- fronts and clusters by `(bbox.min.y, bbox.min.x, area)`;
- closed anchor loops start at the lexicographically smallest point;
- open polylines run first < last;
- bridge candidates by `(cost, angle mod π)`;
- report as in §3.11.

**Memo.** Values are computed at the snapped key point, so they are independent of scheduling.

**Avoidance chain.** Identical whether the prefetch thread or `ensure` computes it.

**Test #20:** byte-equal paint, 1 thread vs default.

### 3.13 Performance and memory budget

| Item | Target (1000-layer figurine / sphere-heavy model, 8 cores) |
|---|---|
| Phase A incl. bands, unprintable, θ, hoop | 4–8 s parallel |
| collisions | parallel with Phase A |
| avoidance chain (union + shrink + simplify) | 3–8 s, on the prefetch thread alongside Phase B |
| Phase B | 8–15 s (band-local ops, Direct mode on rings, memo, touched clusters) |
| paint | 1–3 s |
| verification | ≈ 0 without deficits |
| total | ≤ 30 s; WP5 measures `frog_legs`, `extruder_idler` ×3 and a sphere/torso proxy |

**Memory:**
- `L` once;
- `coll` and `avoid` one copy each;
- bands ≈ fronts' perimeter × 2m;
- the carry window with cropped fields;
- grids ≤ 4M pixels (16 MB) per D component, only for wide fronts, freed after the layer.

### 3.14 v1 simplifications

| # | Simplification | Consequence |
|---|---|---|
| 1 | Overhang placement = zone (widened only if unreachable) | More support than rows on deep shelves and T-arms. |
| 2 | Long spans planned, not painted | Long bridges print unsupported; the report lists them. |
| 3 | θ from all adjacent `L_{n−1}`, not the slicer's fill-surface expansions | θ may differ next to walls that carry only perimeters. |
| 4 | Thin/wide decided per front | Thin appendages of wide fronts get scanline tension. |
| 5 | No closed-cavity detection | Prefer-plate may put must-level support inside closed cavities. |
| 6 | No stability class | Separate feature. |
| 7 | "Relies on disabled causes" is one summary | No per-placement list. |
| 8 | Contact modelled as an area (tips are discrete, D34); no crowding, no other objects | Valid for `K_max ≥ tip_spacing/2` (advisory note otherwise); misses covered by verification and report notes. |
| 9 | ε filter + `A_min` per layer | Walls within ~1° of α never produce needs. |
| 10 | Tolerance constants not editable | Calibration later. |
| 11 | ĉ = 0 when hanging only from ungrounded material | Folded into the island's report entry. |
| 12 | Verification re-runs from layer 0 with the actual F | Costs one pass when paint fails somewhere. |
| 13 | D components truncated at the cluster band; band border = free | Geodesics leaving the band (> m) are lost. |
| 14 | Conservative raster at res ≤ 0.1 mm | Slits < res count as connected and can shortcut a detour. Slices already close gaps < 2 × `slice_closing_radius` (≈ 0.1 mm), so such slits hardly exist in `lslices`. |
| 15 | With interface layers > 0, steep slope fronts and tail rings are uncoverable (roof contact starts 0.5175 mm out) | Reported with why InterfaceGap; after-run "Fix…" offers 0 with its side effects (D30). |
| 16 | The roof-line check (`TreeSupport.cpp:1162-1170`: narrow roofs that generate no lines fall back to tips) is not mirrored | Conservative: may report a false InterfaceGap for very narrow roof components (review 7 m10). |
| 17 | The per-component `minimum_roof_area` rule is not exploited to keep roofs on flat areas while narrow slope or tail pieces get tips | Open question (§11); needs a slicer check first. |

## 4. Painting design (§7, P6; WP1b)

**Paint rule** (`TriangleSelector.cpp:1066-1116`, WP1a). It applies only while a rule is set:
- fully inside: leaf → `rule(old, type)`; split → recurse without `undivide`;
- partial leaf with an unchanged rule result → keep, do not split;
- the `!triangle_splitting` branch applies the rule;
- `seed_fill_apply_*` is unchanged.

**Prism cursor:**

```
causes = paint_causes ∩ {Island, Cantilever}
P_k = union of placements (cause, paintable, level ≥ lowest) at layer k; R_k = offset_ex(∪_{|j−k|≤k_d} P_j, e),
      k_d = max(1, ⌈e / h_min⌉)  (worst-case flank, review 5 m10); per slab: bbox_k, LinesDistancer_k
per volume (parallel over volumes; own selector each):
    sel.deserialize(paint); sel.remap_states(clear_causes_map(cbAll))          // a run replaces all smart paint
    per cause with any R_k:
        seeds = facets with (M⁻ᵀ·n).z < −1e-4 whose world bbox overlaps a non-empty slab region; empty → skip
        point_inside: slab(z) half-open + snapped; R_k ≠ ∅; bbox; LinesDistancer_k.outside(p) < 0 (0 = outside); memo
        classify: 3 only if all vertices inside and, for every overlapped slab k, the XY piece of the triangle
                  clipped to [z_bot_k, z_top_k) satisfies R_k ≠ ∅ and diff(piece, clip(R_k, bbox(piece) ⊕ 1 µm)) = ∅
        touches:  any vertex inside or some piece intersects R_k
        rule = tool_paint_rule(bit); select_patch(seeds[0], PredicateCursor, auto_state(bit),
                 linear(trafo), splitting = true, 89.99°); clear rule
    VolumePaint{id, serialize(), changed = !same_paint(...)}
```

**Size guard.** Painted leaves > 20× the original facets of the region → log. The re-run test checks that the tree does not grow.

## 5. Slicer and persistence (WP1a)

**Projection** (`PrintObject.cpp:3429-3457`): ENFORCER ⇒ `get_facets_strict(*mv, enforcer_like_states())`.

**Warning.** `Print.cpp:711` uses `has_enforcer_like_facets()`.

**3MF export** (`3mf.cpp:3353-3359`):
- `bake = has_auto_states()` → `baked = get_data_auto_baked()` once per volume;
- standard attribute from `baked`;
- `slic3rpe:ext_custom_supports` = true string, written only if non-empty;
- painting version stays 1.

**3MF import:**
- `Geometry::custom_supports_ext` is cleared in `reset()`; index assert;
- `use_ext` per volume over `[first_id, last_id]`.

**Stock compatibility.** Stock sees states 0–2 only. A stock re-save turns auto paint into manual enforcers.

**Undo/redo:** no change.

**R1 fix** (defensive): seed-fill bucket `NONE → 0, ENFORCER → 1, else → 2`.

## 6. GUI design (WP3)

WP3 owns all GUI files, including the two new headers. The boundary in §6.9 is the contract
between the gizmo and the panel; WP3 may add private members.

### 6.1 Main window

```
 [ Automatic painting ]   Paint where needed ▸            ← entry line: opens the section + SetScrollHereY
 … existing tools, clipping, [ Remove all selection ] …   (unchanged)
 ─────────────────────────────────────────────────────────
  ▼ Paint where needed (geometric)
     Paint           [x] Islands   [x] Overhangs
     Overhang angle      [=====|=====]  45°   (0 = auto)
     Max overhang reach  [=====|=====]  3.0 mm
     Max bridge span     [=====|=====]  40 mm
     Support             [ Critical and likely (recommended) v]
     Placement           (o) Prefer build plate   ( ) Build plate only
     > Advanced          Reach fade height · Max dome opening · Accumulation angle · [Reset to defaults]
     ! 2 support settings do not match              [Fix…]      (non-advisory issues only)
     ! Instances differ in rotation: computed for instance 2
     [   Paint where needed   ]  [ Remove smart paint ]
     Paint a blocker to keep an area free; erased smart paint returns on the next run.
     Painted 6 areas (islands 1, overhangs 5) on downward faces — press 2 for the bottom view.
     ! 4 areas (steep slopes, hanging tips) cannot be supported while Top interface layers > 0  [Fix…]
                                                    (only after a run with InterfaceGap needs, D30)
     Report: 2 problems, 5 notes   [Show report]
```

**Window clamp** (review 6 M1), in `on_render_input_window`:
```
h = max(m_imgui->scaled(26.3f), m_last_window_height)
y = max(top_margin, min(y, bottom_limit − h))
ImGui::SetNextWindowSizeConstraints({0, 0}, {-1, bottom_limit − y})
... after the content: m_last_window_height = ImGui::GetWindowHeight()
```

**Summary texts:** "Nothing needs support with these settings." / "Smart paint present — run again for the report." / "Result is stale: settings changed — run again."

**Sliders and strings.**
- α 0–89 (0 = "auto"); K 0.5–20; B 5–200; B_ceil 0–200; H_s 0–5; β 10–α_eff (§3.4).
- Format placeholders; tooltips `_L`.

**Tooltips:**

| Item | Tooltip |
|---|---|
| Header | "Paints support only where it is really needed, from the layer slices. Unlike Automatic painting it keeps domes and short bridges free and keeps your own paint." |
| Islands | "Parts that start in mid-air: lowest points, floating parts, downward tips. They always need support." |
| Overhangs | "Overhangs that hang from one side further than the maximum reach, including long shallow slopes (the reach adds up over the layers)." |
| Overhang angle | "Slopes steeper than this angle (from horizontal) print without support. 0 = automatic (half an extrusion width per layer)." |
| Max overhang reach | "Longest unsupported overhang that still prints. Larger values give less support." |
| Max bridge span | "Longest bridge that prints without support, along the slicer's bridge direction. Longer bridges are only reported in this version." |
| Reach fade height | "Height of plain material above an overhang after which it no longer passes its reach on (stairs). 0 = judge each layer alone." |
| Max dome opening | "Largest opening of a dome, funnel or counterbore ceiling that holds itself." |
| Accumulation angle | "Slopes steeper than this do not add to the reach. Lower it to tolerate long slopes just below the overhang angle." |
| Critical only | "Only where the print would fail." |
| Critical and likely | "Also where the print is likely to droop or curl." |
| Prefer build plate | "Support stands on the build plate wherever possible; on the model only where a part would otherwise fail." |
| Build plate only | "Never put support on the model. Areas that cannot reach the plate are only reported; the slicer would drop them." |
| Paint where needed | "Slices the object if needed and paints enforcers where support is needed. Replaces earlier smart paint; your own paint is kept. Painting over smart paint by hand turns it into your own paint." |
| Remove smart paint | "Removes all smart paint. Your own paint is kept." |
| Fix… | The exact change list (§6.4). |

### 6.2 Report window

- `render_report_window(gizmo_x + gizmo_width + gap, y, bottom_limit)` — to the **right** of the gizmo window (review 6 M1).
- Size constraints: max 60 % of the canvas height.
- **Problems** (open) and **Notes** (collapsed), each with a count and "+N more".
- Row = level dot + short text + "Z 22.6 mm". The tooltip shows layers, bed X/Y, area and what you can do, depending on `why`:
  - InterfaceGap → "Top interface layers keep support 0.5 mm away from this area. Fix… lists what changes if you set them to 0." All InterfaceGap entries are also summarised in the panel's after-run line (§6.1).
  - NoTipLayer → "Too close to the bed for tree support."
- Table: `BeginTable(id, 3, BordersH|ScrollY|SizingFixedFit, {width, rows·h})` with `WidthFixed` columns.
- Row click → marker. "Clip at this height" → `ObjectClipper::set_range_and_pos` (z normal, offset = z).

| State | Short text |
|---|---|
| Unplaceable | "Cannot be supported from the plate" |
| BlockedByUser | "Kept free by your blocker" |
| WillBeDropped | "Your paint here will be dropped by the slicer" |
| RemainingAfterPaint | "Paint does not fully cover this area" |
| OnModelFallback | "Support on the model (no plate position)" |
| OnModelTooShort | "Support on the model may be removed (branch < 1 mm)" |
| MayMergeOntoModel | "Plate support may merge onto the model" |
| PartlyOnModel | "Some tips of this area stand on the model" |
| BelowLowestLevel | "Likely droop, not painted (Critical only)" |
| LongSpanNotPainted | "Long bridge, not painted in this version" |
| CauseDisabled | "Islands/Overhangs not painted (unchecked)" |
| ReliesOnDisabledCauses | "Painted areas assume the unchecked ones are supported" |
| TailWithoutDownwardFacets | "Thin hanging part without downward faces" |
| SeveralPrintObjects | "Computed for instance N only" |
| OtherObjectsNotConsidered | "Other objects on the plate are not considered" |

### 6.3 Marker

`render_3d` is called from `render_painter_gizmo`. It draws a ring (radius `max(1 mm, √(area/π))`) plus a tick at the world position of §3.3, with the `flat` shader as a line loop.

### 6.4 Contract, "Fix…", "Apply and paint" (D16, D20, D30)

**Effective config.** Preset `full_config()` + `mo->config`. It is cached and rebuilt when the edited preset or `mo->config.timestamp()` changes, polled ≤ 2×/s. It is **force-rebuilt** after Apply.

| Key | Condition | Short text | Change |
|---|---|---|---|
| SupportOff | `!support_material` | "Supports are off" | Generate support material: on |
| AutoOn | `support_material_auto` | "Auto-generated supports are on" | Auto-generated supports: off |
| NotOrganic | style ≠ organic | "Support style is not Organic" | Style: Organic |
| EnforceLayers | > 0 | "Enforced support layers are set" | Enforce support for the first: 0 |
| ContactZero | contact = 0 | "Top contact Z distance is 0 (no bridges)" | Top contact Z distance: 0.2 mm |
| BuildplateMismatch | ≠ (policy == PlateOnly) | "'Support on build plate only' does not match Placement" | per policy |
| InterfaceLayers (**advisory**) | `support_material_interface_layers > 0` | shown only after a run with InterfaceGap needs (§6.1) | Top interface layers: 0 — side effects listed in the dialog: "no dense interface under any flat overhang", "bottom interfaces also switch off" (`TreeSupportCommon.cpp:51, 67`), "short on-model branches are removed (more 'Support on the model may be removed' entries)" (`TreeSupport.cpp:2708`) |
| Raft (advisory) | `raft_layers > 0` | info "A raft counts as manual support" | — |
| ReachBelowTipSpacing (advisory) | `K_max < tip_spacing/2` (derived params only) | note in the report: "Max overhang reach is below half the support tip spacing; painted areas may get too few tips" | — |

**Dialog** (review 6 m1). `SmartSupportsDialog : MsgDialog` (in `SmartSupportsPanel.cpp`), identical on all platforms. It lists "Set for this object only: • …" and its buttons are created with `add_button(wxID_APPLY, true, _L("Apply"))`, `add_button(wxID_YES, false, _L("Apply and paint"))`, `add_button(wxID_CANCEL, false, _L("Cancel"))`.

**Apply:**
1. `TakeSnapshot(_L("Apply smart support settings"))` (`Action`);
2. `apply_recommended(…, include_advisory)` (false pre-run, true from the after-run InterfaceGap line);
3. `obj_list()->update_and_show_object_settings_item()`;
4. schedule background processing (pattern `GLGizmoHollow.cpp:610-670`);
5. force the cache rebuild.

"Apply and paint" then follows §2.2, which reslices synchronously, so the new config is used.

**"Paint" with open non-advisory issues** asks once per object per session: "Apply and paint / Paint anyway / Cancel" (same dialog class).

**Advisory issues** are never part of this gate or of the pre-run warning line.
- The after-run InterfaceGap line opens the same dialog with `recommended_changes(…, include_advisory = true)`, listing "Top interface layers: 0" and its side effects.
- "Apply and paint" there re-runs with the new setting.

### 6.5 Rendering and colours

`TriangleSelectorFdmGui : TriangleSelectorGUI` overrides `render()`, owns its GLModels and the seed-fill variants, keeps the paint contour and uses only protected helpers.

| Class | RGBA |
|---|---|
| manual enforcer | 0.47, 0.47, 1.00 |
| manual blocker | 1.00, 0.44, 0.44 |
| auto with island bit | 0.95, 0.65, 0.10 (amber) |
| auto, overhang | 0.10, 0.80, 0.85 (cyan) |
| auto, long span only | 0.60, 0.90, 0.30 (unused v1) |

The palette is checked with a colour-blindness simulator (luminance carries the difference).

### 6.6 State machine

| State | Panel | Leaves on |
|---|---|---|
| Idle | Paint enabled if the worker is idle and a printable instance exists | Paint → (dialog) → `CallAfter` → WaitingForSlices |
| WaitingForSlices | "Slicing… Ns" + Cancel | every poll: re-resolve print and choice (§2.2); ready & consistent → Analysing. Failed on validation error, `why_null` or a 15-min timeout. Cancel, gizmo close or object change → Idle |
| Analysing | "Analysing… x %" + Cancel | finalize: ok → Applied; cancelled → Idle; exception → Failed |
| Applied | summary + report | Paint → …; stale check |
| Failed | one error line | Paint → … |

**Busy** (D18): `gizmo_event` is a public override; while busy it consumes LeftDown/RightDown/Dragging/LeftUp/RightUp. The listed buttons are disabled.

**Gizmo close / object change:** set the cancel flag, cancel the worker only if our job runs, keep LastRun.

**finalize** (review 6 m6): skips `commit_paint` when no volume changed; the cancel branch touches no Plater.

### 6.7 Instances (D15) — `choose_print_object` rules (WP0)

1. **Analysed instance:** the selected one if printable, else the first printable one. None → `po = nullptr`, `NoPrintableInstance` (Paint disabled with a tooltip).
2. **Matching POs:** `active.objects()` with `model_object()->id() == live_mo.id()`. None → `nullptr`, `NotInPrint` (e.g. outside the bed).
3. **preferred** = the matching PO whose `instances()` contain `pi.model_instance->id()` of the analysed instance.
4. **Background processing on** → choice = preferred (no fallback).
5. **Background processing off** → choice = the first matching PO in `active.objects()` order, the one `set_task_impl` processes (`PrintBase.hpp:640-662`).
   - If it ≠ preferred: `fallback = true` and `analysed_instance` = the live index of that PO's first printable instance.
6. `ready = choice->is_step_done(posSlice)`.

**Notes:**
- GUI thread only; call again on every poll (`Print::apply` may delete POs).
- Before slicing, the function already predicts the fallback, so the panel warns before the run.
- The bed is switched to the analysed instance's bed **before** reslicing.

### 6.8 AppConfig and fingerprint

**AppConfig keys** (section `smart_auto_supports`):
- `section_open`, `cause_islands`, `cause_overhangs`;
- `overhang_angle`, `k_max`, `b_max`, `b_ceil`, `h_s`, `beta_equals_alpha`, `beta`;
- `plate_policy` (`prefer_plate|plate_only`), `level` (`critical|critical_likely`);
- `debug_fake_result` (hidden).

**`config_fingerprint`** covers:
- user params;
- support keys incl. interface layers;
- `layer_height`, `first_layer_height`, layer-height profile timestamp;
- ext/perimeter widths, nozzle, `bridge_angle`, `gcode_resolution`, shrinkage;
- instance matrix without offset;
- volume ids, types, matrices, mesh pointers.

### 6.9 Gizmo ↔ panel boundary (contract inside WP3)

```cpp
struct SmartSupportsHost {
    virtual ModelObject *model_object() = 0;          virtual int selected_instance() const = 0;
    virtual std::vector<std::unique_ptr<TriangleSelectorGUI>> &selectors() = 0;
    virtual void commit_paint(const wxString &snapshot_name) = 0;   // GizmoAction + update_model_object()
    virtual void request_redraw(int delay_ms = 0) = 0;  virtual bool is_open() const = 0;
    virtual ObjectClipper *clipper() = 0;
};
class SmartSupportsPanel {
public:
    explicit SmartSupportsPanel(SmartSupportsHost &host);
    void  render_entry_line();
    void  render_section(float window_width);
    void  render_report_window(float x, float y, float bottom_limit);
    void  render_3d();
    void  on_data_changed();   void on_shutdown();   bool busy() const;
    float last_window_height() const;  void set_last_window_height(float);
};
```

**Fake result** (D28): if `debug_fake_result = 1`, `process` skips `run()`. It paints auto(cantilever) on facets with world normal z < −0.7 (via `set_facet` on a private selector) and emits one entry of each `ReportState`. It works with WP0's real `build_input`.

## 7. Test plan

**Infrastructure** (WP0, complete):

`tests/smart_supports_meshes.hpp` (header-only, libslic3r only; usable from `tests/libslic3r` and `tests/fff_print`):
- meshes: `box`, `extrude(ExPolygon, h)` (caps from `triangulate_expolygon_3d` soup → `its_merge` + `its_merge_vertices`), `revolve(rz_profile, segments)`, `wedge`, `translate`, `rotate_x`;
- named shapes: `stairs`, `shelf_cube`, `t_shape`, `dome`, `hole_block`, `slope_wall`, `inverted_cone`, `figurine_proxy` (stacked spheres R 20/15/10 on a cylinder);
- `Params test_params()` — §7 defaults, interface layers 0;
- `AnalysisInput synthetic_input(const std::vector<indexed_triangle_set> &parts, double layer_h, double first_h, const Params &)`:
  - slices with `slice_mesh_ex` at layer centres;
  - computes `below`/`above` links by bbox-filtered `intersection` areas;
  - one `VolumeInput` per part with identity trafo and empty paint;
  - `ext_width_avg = params.ext_width`.

`tests/fff_print/smart_supports_test_utils.hpp`:
- `init_print_parts` (one object, one volume per part);
- posSlice-only processing;
- a static print cache;
- `run_sas(print, up, opts)` via the real `build_input`.

**Rules:**
- WP-specific mesh helpers stay in the WP's own test file (review 6 m5).
- Default config: layer 0.2, first 0.2, nozzle 0.4, perimeters 2, support on, auto off, organic, threshold 45, contact 0.2, enforce 0, buildplate_only 0, raft 0, **interface layers 0** unless the test says otherwise; `verification = false` unless tested.

**Tags:** `[SmartSupports]` + `[sas-input] [sas-selector] [sas-3mf] [sas-slicer] [sas-paint] [sas-geometry] [sas-islands] [sas-reachability] [sas-reach] [sas-tension] [sas-report] [sas-eval] [sas-reference] [slow] [.benchmark]`.

**Tolerances:** ±0.3 mm, areas ±10 %, heights ±10 %, unless stated.

### 7.1 Module tests

| File (WP) | Test | Expected |
|---|---|---|
| fff_print/test_smart_supports_input (0) | `derive_params` | values equal the TreeSupport settings of the same PO (organic, support on); with **support off** and with grid style no assert/crash, values from config (review 7 m9); `tip_spacing` = `connect_length` (top rate 15 % and 30 %); `raft_support_layers` = `TreeSupportSettings::raft_layers.size()` for raft 2 |
| ″ | `build_input` | layer count, links, trafo `trafo_centered()*matrix`; modifiers copied, not sliced |
| ″ | `choose_print_object` | 2 instances with different Z rotations, BP on/off: rules of §6.7; non-printable selected → first printable |
| ″ | `input_consistent` | true after slicing; false after a live rotation; true after a live XY move (bed-safe) |
| libslic3r/test_smart_supports_selector (1a) | encoding | round trip; `acts_as_enforcer` only for 1, 3–9 |
| ″ | rule keeps manual paint (R2) | cube: ENF/BLK spots unchanged (±1e-6 mm²), rest of bottom = auto(1), top untouched |
| ″ | OR / clear | overlap = state 5; `clear_causes_map(cbAll)` leaves manual only |
| ″ | `get_facets_strict(states)` | area = Σ single-state areas |
| ″ | `PredicateCursor` | seeds only (no BFS); empty seeds guarded; a cancel thrown from a predicate leaves the selector valid |
| ″ | `has_auto_states` after `reset()` | false |
| libslic3r/test_smart_supports_3mf (1a) | round trip | states 0,1,2,3,5,9 identical (`same_paint`) |
| ″ | bake | strip `slic3rpe:ext_custom_supports` from `3D/3dmodel.model` (miniz_extension), reload → states ≤ 2, ENF area = manual + auto, BLK unchanged |
| ″ | legacy | stock-style file loads as before |
| fff_print/test_smart_supports_slicer (1a) | projection | auto(2) on a shelf bottom → `project_and_append_custom_facets(ENFORCER)` non-empty at that layer |
| ″ | organic support from auto paint `[slow]` | full `process()`: support under the shelf only with paint |
| ″ | `_SUPPORTS_OFF` | warning with auto paint only |
| ″ | `project_states`, `slice_modifiers` | equal to `po.project_and_append_custom_facets` / `slice_support_enforcers` (sym. diff ≤ 1e-3 mm²) |
| libslic3r/test_smart_supports_paint (1b) | wedge 30°/45° at h 0.2/0.1 (synthetic_input): upper bound ⊂ R ⊕ e; lower bound `cover(predict_F(project(paint)))_k ⊇ planned F_k` minus an e band; soundness (hole / empty slab); mirrored volume; 5 re-runs identical | |
| libslic3r/test_smart_supports_geometry (2a) | Feret; bridge directions (outward / along / tie, n ≡ −n); weak contact; hoop candidates (annulus yes; lid with 4 holes, halo no) | |
| fff_print/test_smart_supports_islands (2a) | island tree; tolerance; cone tail to apex + 2.6 ± 0.3; needle; sibling tails; bands contain all fronts ⊕ m | |
| fff_print/test_smart_supports_reachability (2r) | avoidance vs `TreeModelVolumes` Fast | ≤ 5 % sym. diff `[slow]` |
| ″ | plate coverage incl. `cover` (interface 0) | 40° wall front, sphere ring r 12.1, inverted 60° cone tail ring, **inverted 75° cone** ring, **60° cone at 0.1 mm layers (contact 0.2, z_top 2)** → `plate_part` = whole zone ± 10 % (review 7 M1) |
| ″ | interface 2 | 40° wall front → `plate_part` = ∅, `roof_gap` true, zone ⊂ `cover(tip_path_all)` (InterfaceGap attribution) |
| ″ | roof merge (union) | interface 2: two small pieces whose separate `F_grown − forbid` components are < 1 mm² → tip path each; their union ≥ 1 mm² → roof path; `predict_F(A ∪ B) ≠ predict_F(A) ∪ predict_F(B)` is detected (review 7 M2) |
| ″ | **real contact cross-check** `[slow]` | 40° wall + sphere ring painted, full `process()`, interface 0 and 2: for samples predicted covered, real support exists at the tip layer (`support_islands`) within `tip_spacing/2 + r` for ≥ 90 % of samples; samples predicted uncovered (InterfaceGap) have none within `r`; the area ratio is only logged (review 7 M4) |
| ″ | raft indexing | raft 2: `tip_layer` in support-layer space, coll/avoid ∅ below the raft offset, no negative index (review 7 m3) |
| ″ | buildplate_only | `Params::buildplate_only = true` with policy PreferPlate: `tips().model = ∅`, model stage finds nothing (review 7 m4) |
| ″ | T arm over 80×80 base | `plate_part` = ∅, `model_part` ≠ ∅ |
| ″ | first support layer | layer 2 → `tips_possible` false |
| ″ | pull model | `ensure` under `global_control(max_allowed_parallelism, 1)` completes; `ensure(−1)` no-op; `ensure(k > target)` waits for the thread, then advances; cancel during prefetch → `CanceledException` from `ensure`; destructor joins |
| libslic3r/test_smart_supports_reach (2b1) | rotated rectangle 13.28°: exact within ρ ± 0.01; Grid beyond ρ ≤ **3.5 %**; 22.5° L path ± 3.5 %; H1 80 bands 3.068 ± 2 %; inverted frustum ± 2 %; H_s 0.8; β < α; reset border; Direct with free ends; `cropped` equals the field inside the bbox | |
| libslic3r/test_smart_supports_tension (2b2) | tension both/one free end; ties `covered_all`; chord rule (lean-to / hole strip / one open end); hoop credit band; all with synthetic `ReachQuery` | |
| libslic3r/test_smart_supports_report (2c0) | contract keys incl. InterfaceLayers; `recommended_changes`; merge/sort/cap/`report_more`; fingerprint sensitivity | |
| fff_print/test_smart_supports_eval (2c1a) | `evaluate_pass` with F = manual only: #1 max c 1.8 ± 0.1; #5 none; #14 none; #13 first need at 15.8 ± 1.6 mm; tail needs on the inverted cone exist and vanish with F = its tail paint | |

### 7.2 Reference cases (`fff_print/test_smart_supports.cpp`, WP2c1b; theory §10)

| # | Case | Expected |
|---|---|---|
| 1 | Shelf 2 mm | none; max c 1.8 ± 0.1 |
| 2 | Shelf 10 × 15 | 1 layer; 102 ± 10 mm²; 3.2 ± 0.3 from the cube; must present |
| 3 | Shelf 10 × 30 (cube 30) | 204 ± 10 % mm² |
| 4 | T-shape 15 × 10 | zone ≥ 3.2 from the stem, 2 × 118 ± 10 % mm², must |
| 5 | Bridge 30 | none |
| 6 | Bridge 80 | 1 long-span placement ± 1 mm of mid-span, should, not paintable; `LongSpanNotPainted` |
| 7 | Dome R20 (outer R25, open), `B_ceil = 0` | first need z 19.0 ± 0.3 (hole r 6.3 ± 0.3), should; none below 18.5; default `B_ceil` → none |
| 8 | Dome R30, `B_ceil = 20` | one ring at hole r 11.9 ± 0.5, should; none at hole r < 9.8 (WP5: the first layer whose hole edge passes c = K_max, 11.52 at 0.2 mm) |
| 9 | Holes Ø8 / Ø20 / Ø50 (blocks 20×20×20 / 40×20×40 / 70×20×70, depth 20) | none / max should / must (WP5: Ø20 should only at 0.1 mm layers; Ø50 must only with "critical only" — with should planned, the first should band resets c, as on the sphere #11) |
| 10 | T on 80×80 base | prefer plate: on-model (must points) + `OnModelFallback`, ~~plus Unplaceable should bands `c ∈ (3, 3.7]`~~ (WP5: none remain, see the §3.9 note); plate only: no placements, Unplaceable |
| 11 | Sphere R20 on bed, `test_assume_reachable` | rings r 5.9 ± 0.6 and 12.1 ± 1.0, should; none < 5.2 or > 14.6 |
| 12 | Sphere elevated | island placement at its first layer; then as #11 |
| 13 | Wall 40°, 50 tall | first placement at 15.8 ± 1.6; none below 14.0 |
| 14 | Wall 44°; wall 40° with `H_s = 0` | none |
| 15 | Stairs 2.5, risers Δz 0.4 / 1.2 | steps 2, 4, 6 / none |
| 16 | Lean-to 30 / 50 apart | none / long-span may only (`BelowLowestLevel`) |
| 17 | Bridge 30 + logo, `V_min = 20` / default | none / island under the logo only |
| 18 | P3: #12 with {I,C}, {I}, {C} | identical plans; F within e |
| 19 | P5: #2 + blocker | no paint inside blocker ⊕ clearance; `BlockedByUser` note |
| 20 | Determinism | byte-equal paint, 1 thread vs default |
| 21 | P4 | a manual enforcer never adds needs |
| 22 | Verification | `test_mutate_before_paint` drops one placement → `RemainingAfterPaint` |
| 23 | Edge cases | empty, 1 layer, grounded cube; first layer 0.3 / layer 0.15; raft 2; 0.1 mm layers; scaled, mirrored, X-rotated, sinking |
| 24 | Cancel | each phase → `CanceledException`, no partial result |
| 25 | Benchmark `[.benchmark]` | `frog_legs.obj`, `extruder_idler.obj` ×3, `figurine_proxy` (sphere-heavy) — phase timings (WP5: `notes/timings.md`) |
| 26 | Inverted 60° cone, floating apex | tail placements up to apex + 2.6 ± 0.3; no Unplaceable (review 5 B1) |
| 26b | Inverted 75° cone; 60° cone at 0.1 mm layers with contact 0.2 | tail painted, no Unplaceable (review 7 M1) |
| 27 | Interface layers 3 (PrintConfig default): #13 (40° wall) and #2 (shelf) | wall: Unplaceable with why `InterfaceGap`; `InterfaceLayers` advisory issue present and **not** in the non-advisory list. Shelf: placement accepted (roof covers the flat zone) |

### 7.3 Manual host checklist (after the sandbox build passes)

1. The section, its fields and the collapse state persist across restarts; "Reset to defaults" works.
2. 1366×768 and 1920×1080 at 100 % and 150 %: the gizmo window never leaves the canvas (top or bottom) and scrolls; the report window sits right of it.
3. T-shape:
   - Paint → "Slicing…" → "Analysing… %" → cyan paint beyond ~3 mm;
   - the summary shows the bottom-view hint;
   - one undo removes the paint, redo restores it.
4. Manual enforcers and blockers stay; nothing is painted in a blocker; the report shows "Kept free by your blocker".
5. Uncheck "Overhangs", then Paint → earlier overhang paint is gone; "Remove smart paint" keeps manual paint.
6. Hollow dome: nothing is painted (compare: "Automatic painting" paints its top).
7. T on wide base: Prefer build plate → paint + "Support on the model"; Build plate only → no paint, problems listed.
8. Contract:
   - pre-run warnings for support off / auto on / grid style; **no** pre-run warning for interface layers;
   - with interface layers 3, a T-shape gets roofs under the arms; a 40° wall shows the after-run InterfaceGap line; its "Fix…" lists "Top interface layers: 0" plus the three side effects;
   - "Fix…" lists the changes;
   - "Apply and paint" runs with the new config, without a second prompt;
   - the object list shows the settings item; undo reverts;
   - deleting the item brings the warnings back.
9. Preview: organic tips on the plate under smart paint; on-model tips only where reported.
10. 3MF:
    - saved and reopened in the fork → colours kept, "run again for the report" shown;
    - opened in stock PrusaSlicer 2.9.x → blue enforcers, same support, no crash.
11. R1: smart fill or Triangles brush over smart paint → no crash.
12. Cancel while slicing and while analysing: no error popup. Close the gizmo, switch objects, delete the object, load a project or quit during analysis: nothing applied, no crash.
13. While busy: the brush and the listed buttons are disabled.
14. Background processing on and off.
15. Object on bed 2. Instances with different rotations → warning before the run. A non-printable selected instance.
16. Multi-part object, modifier, negative volume, enforcer/blocker modifiers; scaled, mirrored, X/Y-rotated objects; raft; variable layer height; first layer ≠ layer height.
17. "Automatic painting" after a smart run: its dialog appears, all paint is erased, the panel shows stale.
18. Copy/paste, split, Cut gizmo, Simplify on smart-painted objects.
19. Report row → marker at the right place; "Clip at this height" works.
20. A ~1M-triangle figurine: run time, paint size, 3MF size, undo memory, brush FPS with the panel open.
21. Language switch: no crash; strings appear (English until translated).
22. Palette check with a colour-blindness simulator.
23. dev build (wx 3.3, Wayland): the panel renders and behaves the same.

## 8. Work packages

### 8.1 Execution model (set up by the orchestrator)

**Spec first.** The orchestrator commits `plan.md` and `reviews/` on `feature/smart-auto-supports` before creating worktrees (review 6 m4).

**WP0** runs alone in `/home/nhp/project` on `feature/smart-auto-supports` and commits there. It builds with:
```
/home/nhp/sas-deps/wp-build.sh /home/nhp/project …
```

**Every later WP:**
- works in `/home/nhp/sas-wt/<wp>` with a **detached HEAD** at the feature tip; no branches;
- commits there;
- builds with:
  ```
  /home/nhp/sas-deps/wp-build.sh /home/nhp/sas-wt/<wp> --test '<cmd in build dir>' <targets>
  ```
  This builds the current working tree, including uncommitted and new files, merged in memory with dev/2.9.6. It is serialised by a lock, incremental, and prints the log path.
- **Rule:** the target list must include every test binary that `--test` runs (`libslic3r_tests` and/or `fff_print_tests`), otherwise a binary from another WP's tree may run. Example:
  ```
  wp-build.sh /home/nhp/sas-wt/wp2b1 --test 'tests/libslic3r/libslic3r_tests "[sas-reach]"' libslic3r_tests
  ```

**Integration.**
- The orchestrator cherry-picks finished WPs onto the feature branch.
- Running WPs rebase onto the new tip before their final build.
- Commits: `feat(smart-auto-supports): <wp>: …`, plus the required attribution line.
- No pushes; no merges into `dev/*` or `fork/*`.

**Ownership.** Strict per file (§2.1).
- libslic3r headers are **frozen after WP0**. Changes go through the orchestrator, batched in one commit (`TriangleSelector.hpp` and `Model.hpp` trigger near-full rebuilds).
- GUI headers belong to WP3.

**CPU.** One build at a time. Only the needed targets: `libslic3r_tests`, `fff_print_tests`, `libslic3r_gui`, and `PrusaSlicer` for the GUI link check. Tests run filtered by tag; full suites only in WP1a (once) and WP5. ccache is recommended (orchestrator).

**Size.** Each WP fits one agent session (≈ 300–1500 lines incl. tests).

**Host smoke test.** The user builds the detached merge commit printed by `wp-build.sh` — never a dev merge (review 6 m10).

### 8.2 Phases

**Phase 0**

| WP | Content | Owned files | Size | Acceptance |
|---|---|---|---|---|
| WP0 | See the deliverables list below the table. | all new libslic3r headers + stub `.cpp`s, `SmartSupportsInput.cpp`, the new parts of `TriangleSelector.{hpp,cpp}` and `Model.{hpp,cpp}` (WP1a owns those files afterwards), GUI placeholders, 4 CMakeLists, test utils, input test | ≈ 1500 | see below |

**WP0 deliverables:**
1. `SmartSupportStates.hpp` complete (inline).
2. `SmartSupports.hpp` and `SmartSupportsInternal.hpp` exactly as §3.1/§3.2, with stub bodies per the stub rules.
   - Every function returns correctly sized, non-throwing results.
   - `Reachability` and `ReachField` stubs are usable (the documented stub semantics).
3. **Real** implementations in `SmartSupportsInput.cpp`:
   - `derive_params` (no `TreeSupportMeshGroupSettings`; `tip_spacing`, `branch_distance`, `raft_support_layers`, `buildplate_only`);
   - `choose_print_object`;
   - `input_consistent`;
   - `build_input`.
4. `TriangleSelector.hpp` declarations + **link-complete bodies** in `TriangleSelector.cpp`:
   - `get_facets_strict(states)` real;
   - `PredicateCursor` real, so its vtable exists;
   - the rule setters inline;
   - the hook stays for WP1a.
5. `Model.hpp` declarations + **real** `FacetsAnnotation` helpers in `Model.cpp`.
6. CMake entries for the lib, the GUI placeholders (empty `.hpp`/`.cpp`) and the tests.
7. Test utilities complete:
   - `tests/smart_supports_meshes.hpp` with `synthetic_input`, `test_params`;
   - `tests/fff_print/smart_supports_test_utils.hpp`.
8. `fff_print/test_smart_supports_input.cpp` complete; one stub `TEST_CASE` per other test file.

**WP0 acceptance:**
- full build of all targets including `PrusaSlicer` (link check of the declarations, review 7 m7);
- `[sas-input]` green;
- every test binary runs `[SmartSupports]`;
- the orchestrator reviews and freezes the libslic3r headers.

**Phase 1** — in parallel after WP0

| WP | Content | Owned files | Size | Depends | Acceptance |
|---|---|---|---|---|---|
| WP1a | TriangleSelector rule hook (fully-inside recursion, partial-leaf check, no-splitting branch); projection; warning; 3MF; `project_states`, `slice_modifiers`; tests of the WP0-implemented selector/FacetsAnnotation helpers | `TriangleSelector.cpp`, `Model.cpp`, `PrintObject.cpp`, `Print.cpp`, `Format/3mf.cpp`, `SmartSupportsProjection.cpp`; tests selector, 3mf, slicer | ≈ 1100 | WP0 | own tags green; full `libslic3r_tests` + `fff_print_tests` once |
| WP2a | Phase A + A2 | `SmartSupportsBase.cpp`; tests geometry, islands | ≈ 1100 | WP0 (projection stub ok) | `[sas-geometry] [sas-islands]` |
| WP2r | `Reachability` complete | `SmartSupportsReachability.cpp`; test reachability | ≈ 900 | WP0 | `[sas-reachability]` incl. cross-checks |
| WP2b1 | raster, `ReachField`, `ReachCarry`, `segment_min` | `SmartSupportsRaster.cpp`, `SmartSupportsReach.cpp`; test reach | ≈ 1200 | WP0 | `[sas-reach]` |
| WP2b2 | anchors, domain edges, tension, chord rule, hoop, `classify_front` | `SmartSupportsTension.cpp`; test tension | ≈ 1000 | WP0 | `[sas-tension]` |
| WP2c0 | report, contract, `recommended_changes`, `apply_recommended`, fingerprint | `SmartSupportsReport.cpp`; test report | ≈ 600 | WP0 | `[sas-report]` |
| WP3 | GUI complete against the fake result | `TriangleSelectorFdmGui.{hpp,cpp}`, `SmartSupportsPanel.{hpp,cpp}`, `GLGizmoFdmSupports.{hpp,cpp}`, `GLGizmoPainterBase.cpp` | ≈ 1500 | WP0 | `libslic3r_gui` + `PrusaSlicer` link OK; host smoke test with `debug_fake_result` |
| WP4 | docs (§11) | `README.md`, `notes/theory.md`, `notes/concept.md` (pointer) | — | plan approval | reviewed |

**Phase 2**

| WP | Content | Owned files | Size | Depends | Acceptance |
|---|---|---|---|---|---|
| WP1b | `paint_placements` | `SmartSupportsPaint.cpp`; test paint | ≈ 700 | WP1a, WP2r | `[sas-paint]` |
| WP2c1a | `LayerEvaluator`, `evaluate_pass` | `SmartSupportsEval.cpp`; `fff_print/test_smart_supports_eval.cpp` | ≈ 1100 | WP2a, WP2r, WP2b1, WP2b2, WP1a | `[sas-eval]` (#1, #5, #14, #13 first need, cone tail) |

**Phase 3**

| WP | Content | Owned files | Size | Depends | Acceptance |
|---|---|---|---|---|---|
| WP2c1b | `make_plan` (stages, reasons, rows), `verify`, `run()` | `SmartSupportsPlan.cpp`, `SmartSupports.cpp`; `fff_print/test_smart_supports.cpp` | ≈ 1100 | WP2c1a, WP2c0, WP1b | build OK; #2, #13, #15, #26 green |

**Phase 4**

| WP | Content | Owned files | Depends | Acceptance |
|---|---|---|---|---|
| WP5 | Single integration owner: all of §7.2; calibration; benchmark vs §3.13; verification-as-option decision (> 10 s); full suites; portability build (§8.3); one assert-enabled run of `[sas-selector]` if the orchestrator provides a Debug build (review 6 m8); switch the GUI from the fake to `run()`; record timings in `notes/` | every analysis `.cpp`/test (exclusive while active) | all | all `[SmartSupports]` green; full suites green; budget met or documented |

Then the orchestrator builds and hands over to the user for §7.3.

### 8.3 Portability check

WP5 builds the bare feature branch on `version_2.9.6` without dev, with `-DSLIC3R_GUI=OFF`, and runs the `[SmartSupports]` tests (separate build dir from the orchestrator).

## 9. Risks and mitigations

| Risk | Mitigation |
|---|---|
| Prediction ≠ slicer | tip + roof paths mirrored; real-contact cross-check test; verification; report |
| Interface layers > 0 | contract key + why `InterfaceGap`; reference #27 |
| H1 accuracy | exact zone, segment minimiser, round S; 2 % tests |
| Performance (ring fronts, big models) | precomputed bands, prefetch chain, Direct mode, memo; sphere-heavy benchmark |
| Memory | L once; coll/avoid one copy each; cropped carry fields; grid cap |
| Deadlock / cancel | pull model (no task waits on a task); cancel-checked waits; exceptions rethrown |
| Spill tips on the model | PartlyOnModel note; calibration |
| Closed cavities | §3.14 #5 |
| Paint size | sound predicate; edge limit; re-run test |
| Merge conflicts | only `src/libslic3r/CMakeLists.txt` shared (separate hunk) |
| Stale Print / multi-bed | synchronous apply via reslice; bed-safe consistency; re-resolve every poll |
| GUI untestable in sandbox | fake result; link check; host checklist |
| Port to 3.0 | changes localised |

## 10. Review resolution

A = accepted as proposed, C = accepted with a change, R = rejected. (O) = orchestrator ruling.

**Round 1** (reviews 1–4). The per-finding table of revision 2 is condensed here into groups; every finding was accepted, accepted with a change (C) or ruled by the orchestrator (O). Rows that round 2 changed further are marked →:

| Finding | Res. | Where / reason |
|---|---|---|
| 1-B1 reachability in tip space | A → | §3.7, completed by 5-B1/5-M1 |
| 1-M1 discard path | A | §3.9 |
| 1-M2 fallback scope | A | D12, §3.9 stages |
| 1-M3 hoop | A | D26 |
| 1-M4 test #7 | A | §7.2 #7 |
| 1-m1 … m11 | A | §3.5–§3.9, §7 |
| 2-M1 … M4, N1 … N15 | A → | §2.3, §2.4 (bed-safe, 6-M2), §3.1, §4, §5, §6, §7, §8.3 |
| 3-B1 panel height | A → | §6.1 (clamp fixed, 6-M1) |
| 3-M1, M2, M3, m2, m5, m13 | A (O) | D9, §4, D5, D16 |
| 3-M4, M5, M7, m1, m3, m4, m6–m11, m14 | A | §6, §8 |
| 3-M6 build model | C | orchestrator's worktree model §8.1 |
| 3-m12 translations | C | placeholders; `.po` out of scope |
| 4-B1.1 precompute | C → | now bands + unprintable precomputed in Phase A (5-M3) |
| 4-B1.2 … B1.5, B1.8 | A | §3.8, §3.7 |
| 4-B1.6 verification | C → | from layer 0 with F_act (5-m6) |
| 4-B1.7 early exits | C | field kept for the carry |
| 4-M1 … M8, m1 … m10 | A (4-m6 C) | §3.5–§3.13, §4, §7 |

**Round 2** (reviews 5 and 6):

| Finding | Res. | Where / reason |
|---|---|---|
| 5-B1 tails never resolved; uncoverable g band | A | `Reachability::cover` (§3.7); used for C', tail residual, `plate_part`, verification; §7.2 #26; theory §5.2 amendment |
| 5-M1 roof path not mirrored | A (both) | Verified `TreeSupport.cpp:1414-1426, 1122-1170`: `contact()` mirrors the roof path **and** new contract key `InterfaceLayers` (D30); why `InterfaceGap`; real-contact cross-check test; #27 |
| 5-M2 pipelining contradiction / deadlock | A | D31 pull model; §2.3, §3.7; pull-model test |
| 5-M3 Phase B budget for ring fronts | A | D25: bands and unprintable precomputed in Phase A; per-D-component grid; collisions parallel; sphere-heavy benchmark |
| 5-m1 `c_hat` lacks w_n | A | §3.2 signature |
| 5-m2 stage-3 clearance; fall-through | A | §3.9 |
| 5-m3 Reachability window | A | §3.2 comment |
| 5-m4 memo at snapped point | A | §3.8 |
| 5-m5 conservative raster bound | A | res ≤ 0.1 mm cap; corrected bound §3.14 #14 (slices close gaps < ≈ 0.1 mm) |
| 5-m6 verification carry undefined | A | §3.10 from layer 0 |
| 5-m7 grid tolerance | A | 3.5 % |
| 5-m8 carry memory | A | `ReachField::cropped` |
| 5-m9 #10 should band | A | §3.9 note; §7.2 #10 |
| 5-m10 k_d | A | worst-case flank `⌈e/h_min⌉` |
| 5-m11 report reasons | A | §3.9 precedence |
| 6-B1 phase-1 WPs need `build_input` | A | D32; WP0 implements input/params/choice/consistency; `synthetic_input`, `test_params`; dependency columns updated |
| 6-M1 window clamp / report position | A | §6.1, §6.2 |
| 6-M2 bed shift in consistency | A | §2.4 `get_matrix_no_offset` + z |
| 6-M3 stale Print after Apply / bed switch | A | D33, §2.2, §6.4 forced cache rebuild, all-volume check |
| 6-M4 avoidance deadlock | A | D31 |
| 6-M5a `c_hat` w_n | A | §3.2 |
| 6-M5b memo ownership | A | cluster-owned memo in `LayerEvaluator`; `c_hat` pure |
| 6-M5c ReachField ctor / res | A | §3.2 |
| 6-M5d synthetic fields | A | `ReachQuery` interface |
| 6-M5e report inputs | A | `Plan.residual` complete (WP2c1b) |
| 6-M5f `compute_anchors` C_f | A | §3.2 |
| 6-M5g test hook | A | `RunOptions::test_mutate_before_paint` |
| 6-M5h result sizes | A | §3.1 conventions + per-function notes |
| 6-M5i `choose_print_object` rules | A | `PrintObjectChoice`, §6.7 |
| 6-M5j `Params` defaults | A | §3.1 |
| 6-M5k GUI headers | A | WP3 owns them; boundary §6.9 |
| 6-M5l naming | A | `slice_modifiers` everywhere |
| 6-M6 split WP2c1 | A | WP2c1a / WP2c1b |
| 6-m1 dialog platforms | A | `SmartSupportsDialog : MsgDialog` with `add_button` |
| 6-m2 link check | A | `PrusaSlicer` target |
| 6-m3 `--test` binary | A | §8.1 rule (the orchestrator fixes the script) |
| 6-m4 commit spec first | A | §8.1 |
| 6-m5 shared test headers | A | §7 rule |
| 6-m6 finalize details | A | §2.2, §6.6 |
| 6-m7 β range | A | α_eff (§3.4) |
| 6-m8 asserts off | C | WP5, if the orchestrator provides a Debug build; otherwise a listed risk |
| 6-m9 ccache | C | orchestrator recommendation (§8.1) |
| 6-m10 smoke-test tree | A | §8.1 |

No round-2 finding was rejected. Round 2 rows that round 3 changed further:
- 5-B1 band width is now `G = xy_min` (7-M1);
- 5-M1 interface layers are run with the user's setting, with only an after-run, advisory "Fix…" (7-M3);
- 5-m10 / 6-M5i are unchanged.

**Round 3** (review 7):

| Finding | Res. | Where / reason |
|---|---|---|
| 7-M1 g band too narrow next to steep lower layers | A | `G = xy_min` for every `z_top` (§3.4, §3.7 `cover`, `gap_band_mm`); check table; tests #26b, reachability rows; theory amendment |
| 7-M2 `predict_F` not superadditive with roofs | A | `E` = union of all painted zones per layer: `contact`/`plate_part`/`model_part`/`evaluate`/`evaluate_pass` take the union (§3.2, §3.7, §3.9); `Plan.zones_*` replace `planned_F`/`manual_F`; verification on unions (§3.10); P4 superadditivity claim limited to interface 0; roof-merge test |
| 7-M3 do not recommend 0 interface layers up front | A | D30: run with the user's setting; `ContractIssue::advisory`; no pre-run warning or gate; after-run InterfaceGap line + "Fix…" with three side effects (§6.1, §6.4); `recommended_changes`/`apply_recommended(include_advisory)`; PrintConfig default 3 corrected; #27, §7.3 #8 adapted |
| 7-M3(d) exploit per-component `minimum_roof_area` | C | not v1: §3.14 #17, §11 open question |
| 7-M4 tips are discrete | A | D34; `Params::tip_spacing`, `branch_distance`; advisory `ReachBelowTipSpacing`; §3.7 assumption; distance-based cross-check (≥ 90 % within `tip_spacing/2 + r`) |
| 7-m1 T over-approximated | A | §3.7: outset only on the forbidden remainder; check table T = [0.51, 1.04], extra_outset 0.19 |
| 7-m2 `model_short` scope | A | D23: applies to tip-path contacts; roof components exempt |
| 7-m3 raft indexing | A | support-layer indices; `Params::raft_support_layers`; ∅ below the offset; test |
| 7-m4 Reachability models the policy | A | D35, `Params::buildplate_only`; test |
| 7-m5 prefetch cancel source; `ensure` semantics | A | `IsCanceled` (thread-safe) for `run()` and `start_prefetch`; `ensure(k<0)` no-op, `k > target` joins first (§2.3, §3.2) |
| 7-m6 who runs what; reference invalidation | A | `run()` does `parallel_invoke`, `start_prefetch`, `make_plan` (§2.2, §3.9); `evaluate` reference note; copies in §3.9 |
| 7-m7 link stubs | A | WP0 adds link-complete bodies (`PredicateCursor`, `get_facets_strict`) and real FacetsAnnotation helpers; WP0 acceptance includes the `PrusaSlicer` link |
| 7-m8 InterfaceGap attribution | A | `Contact::tip_path_all`; attribution rule in §3.7/§3.8 |
| 7-m9 `derive_params` with support off | A | no `TreeSupportMeshGroupSettings`; test |
| 7-m10 roof-line check | A | §3.14 #16 (conservative) |
| 7-m11 default comments | A | `Params` comment; `interface_layers = 3`; D30 corrected |

No round-3 finding was rejected.

## 11. Open questions and doc changes

**Open questions** (do not block v1):
1. Hoop band width `B_max/2` (D26): a placeholder; the theory is silent.
2. Spill tips in Prefer-plate mode (`PartlyOnModel`): acceptable, or trim the paint? Decide after host tests.
3. Interface layers (D30): can the slicer's per-component `minimum_roof_area` rule keep roofs on flat areas while narrow slope or tail pieces get tips (e.g. painting them as separate small zones)? This needs a slicer check first; not v1 (§3.14 #17).
4. Tip spacing (D34): is `K_max ≥ tip_spacing/2` enough in practice? Calibrate with the distance-based cross-check and test prints.

**Doc changes (WP4):**
- **theory P6:** storage in TriangleSelector states 3–9; blockers instead of suppression (+ panel hint); fingerprint in session memory; 3MF bake + `slic3rpe:ext_custom_supports`.
- **theory P3 / §6 user view:** a run replaces all smart paint with the checked causes; one "Remove smart paint". The joint plan stays toggle-independent.
- **theory §2.1:** round offset for `S_n`.
- **theory §2.4:** the research correction on the slicer's bridge region and anchors; the chord rule needs anchors at both ends + end closure; hoop credit only for exactly one hole, limited to a band width.
- **theory §5.1** (research correction): no machine border; blockers are not in the collision.
- **theory §5.1–§5.3:** feasibility in paint space via the slicer's tip/roof contact:
  - `R_n` applies to tips;
  - the clearance only flags;
  - blockers keep `tip_d + extra_outset`;
  - no tips below the first support layer;
  - a **gap band of width xy_min** next to `L_{n−1}` counts as covered;
  - contact is predicted on the union of all painted zones of a layer (the roof decision is per union component);
  - roofs keep contact `r + xy_min/2` away; the run uses the user's interface setting, and an after-run advisory offers 0 with its side effects;
  - tips are discrete: the area model assumes `K_max ≥ tip_spacing/2`;
  - the slicer's `buildplate_only`, not the policy, decides forbid and tip status;
  - short on-model branches are deleted only without a roof.
- **theory §2.5 / §9:** ε is a width; D9 (needs below the painted level are not planned).
- **README.md:** status, "How to test" (§7.3 + tag commands), notes list with `plan.md` and `reviews/`, a decision-log row.
