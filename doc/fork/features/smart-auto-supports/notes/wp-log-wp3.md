# Work package log — WP3 (GUI)

Scope (plan §8.2): the "Paint where needed" GUI against the fake result — `TriangleSelectorFdmGui.{hpp,cpp}`,
`SmartSupportsPanel.{hpp,cpp}`, `GLGizmoFdmSupports.{hpp,cpp}`, the R1 fix in `GLGizmoPainterBase.cpp`.
No libslic3r header was changed; built on the feature tip with WP2c0 (real contract, fingerprint, report).

## Files

| File | Content |
|---|---|
| `GLGizmoPainterBase.cpp` | R1 fix: seed-fill bucket NONE → 0, ENFORCER → 1, any other state → 2 |
| `TriangleSelectorFdmGui.{hpp,cpp}` | renderer: one GLModel per paint class (manual enforcer/blocker, auto island/overhang/long span) plus one seed-fill model per class; colours of §6.5; unknown states 10..255 render like blockers (as stock) |
| `SmartSupportsPanel.{hpp,cpp}` | `SmartSupportsHost`, `SmartSupportsPanel` (section, settings, contract line, run state machine, UI job, finalize, staleness, "Remove smart paint", report window, marker, clip), `SmartSupportsDialog : MsgDialog`, fake result |
| `GLGizmoFdmSupports.{hpp,cpp}` | implements the host (private base), entry line, height clamp, section, report window, `render_3d`, busy handling, `TriangleSelectorFdmGui` selectors |

## Deviations from the plan (exact)

1. **Host interface (§6.9)** additionally has `GLCanvas3D &canvas()` (posts `EVT_GLCANVAS_FORCE_UPDATE` after
   "Fix…", as `GLGizmoHollow`). The panel additionally has `entry_line_width(autopaint_label)` and
   `required_width()` (the gizmo widens its window to fit the section rows and the entry line);
   `render_entry_line()` expects `ImGui::SameLine()` from the caller.
2. **Window clamp (§6.1):** `m_last_window_height` is the *desired* height of the last frame
   (`GetCursorPosY() + WindowPadding.y` after the content), not `GetWindowHeight()`, so the window moves up as far
   as needed before it starts to scroll (with the clamped height it would stay low and scroll early). The size
   constraint uses max x = `FLT_MAX` instead of −1: −1 keeps the previous width and disables ImGui's scrollbar
   width compensation of auto-resized windows.
3. **Busy mouse handling (D18):** `gizmo_event` consumes LeftDown/RightDown only when the click hits the object
   (raycast as the brush does), and then the drag and release of that click. Clicks beside the object still reach
   the canvas, so the view can be orbited and panned during a run. Consuming every event would block all camera
   movement except zoom.
4. **Stale texts (§6.1):** two variants — "Result is stale: the paint changed — run again." and "… settings changed
   …". Settings staleness = `config_fingerprint` differs **or** the panel's `UserParams` differ from the run's.
5. **Bed switch / validation:** the bed of the rule-1 instance (§6.7: selected if printable, else first printable)
   is activated **before** `validate()` (stock `auto_generate()` validates first, which fails for objects on another
   bed). The print is validated again right after the synchronous reslice (an "Apply and paint" can make it invalid;
   the reslice would then never start and the wait would only end by the 15-min timeout).
6. **Inconsistent input while waiting:** when posSlice is done but `input_consistent()` fails (model edited after the
   reslice), the reslice is re-issued, at most once per second.
7. **Entry glyph:** "Paint where needed »" instead of "▸" (Latin-1 is always in the ImGui font).

## Decisions on ambiguities

- **Gate (§6.4):** "Paint anyway" marks the object as answered for the session; Cancel asks again next time. Gate
  buttons: Apply and paint (`wxID_YES`) / Paint anyway (`wxID_NO`) / Cancel; "Fix…": Apply (`wxID_APPLY`) / Apply and
  paint (`wxID_YES`) / Cancel. Without any recommended change only Cancel (Fix) resp. Paint anyway + Cancel (gate).
  Body text: the issue texts of §6.4, "Set for this object only:" with "<option full label>: <value>" (bools on/off,
  enum labels, sidetext), and for the interface "Fix…" the three side effects of §6.4.
- **Contract:** real WP2c0 functions on the cached effective config (preset `full_config()` + object config,
  rebuilt on object-config timestamp / placement change and every 500 ms, forced after Apply and in finalize).
- **Report (WP2c0 notes):** group rule mirrored (`BlockedByUser` is a problem only for islands, else state ≤
  `MayMergeOntoModel`); totals = rows + `report_more`; rows with `area_mm2 == 0` have no Z column, no marker and no
  position lines in the tooltip; SeveralPrintObjects shows `analysed_instance_idx + 1`; CauseDisabled text by cause;
  the advisory `Raft` and `ReachBelowTipSpacing` issues of `in.contract` are listed at the top of the Notes group.
  Tables show ≤ 10 rows and scroll; the window is ≤ 60 % of the canvas height and ≤ the space to the bottom limit.
- **After-run interface line (D30):** count = Σ `count` of rows with `why == InterfaceGap`; shown only while the result
  is not stale.
- **Marker (§6.3):** ring (r = max(1 mm, √(area/π))) in the horizontal plane plus a downward tick, yellow, drawn
  without depth test (it sits under the model); only while the report window is open and a positioned row is
  selected. `flat` shader, `dashed_thick_lines` on core profiles (as `GLGizmoScale`).
- **"Clip at this height":** clipping normal +Z, offset = marker world z (everything above is hidden); the ratio is
  computed as `ObjectClipper::set_position_by_ratio()` does, so the clipping slider stays consistent.
- **Job (§2.3):** `queue_job` on the UI worker; the job captures only the input, a result slot and atomics; `is_canceled`
  = `ctl.was_canceled() || panel flag`; progress is monotone, `ctl.update_status` throttled to ≥ 100 ms (capped at 99)
  and a final `update_status(100)` always closes the progress notification. Cancel bumps the run id, so a late
  finalize is ignored; `Worker::cancel()` only while our `running` flag is set. A finalize after the panel died only
  clears `eptr` (lifetime token, no Plater access). Exceptions: one error line + PlaterJob's popup.
- **Finalize checks (§2.4):** gizmo open, same object, same model-part ids, mesh pointers and paint timestamps, sizes
  of selectors / volumes / paint; else discard with a one-line notice. Only volumes with `changed` are deserialized;
  no snapshot without a change.
- **LastRun** keeps only report data (no paint, no placements) per ModelObject id, for the session.
- **AppConfig (§6.8):** decimals with '.'; `overhang_angle` starts from the effective `support_material_threshold`
  and is persisted on first load; "Reset to defaults" resets all panel parameters (α again from the threshold).
  The β slider runs from 10° to α, for auto α to atan(layer_height / (0.5 · 0.45 mm)) (exact clamp in libslic3r).
- **Strings:** all translatable; the degree sign reuses the gizmo's msgid + context.

## Fake result (`[smart_auto_supports] debug_fake_result = 1`)

`process` skips `run()` and, per model part on a private selector: clears all smart paint, then paints auto(island)
on unsplit unpainted facets with world normal z < −0.95 and auto(overhang) for −0.95 ≤ z < −0.7, only for checked
causes (manual paint is kept, split facets are skipped). It then sleeps 3 s in 100 ms steps (progress, Cancel) and
emits one row per `ReportState` (problems first, `report_more = {2, 0}`), positioned on painted facets when there
are any; Unplaceable has why InterfaceGap and count 4 (→ after-run interface line), CauseDisabled is for islands,
SeveralPrintObjects / OtherObjectsNotConsidered have no position. The panel shows "(debug_fake_result)".

## Header change requests

None needed. Optional (WP2c0 suggestion): `inline bool is_problem(const ReportEntry &)` in `SmartSupports.hpp`;
the GUI would then drop its copy in `SmartSupportsPanel.cpp`.

## Build results (sandbox, lane 2: dev/2.9.6 merged in, wxWidgets 3.3)

- `libslic3r_gui`: OK, no warnings in the WP3 files.
- `PrusaSlicer` link: OK.
- The GUI cannot run in the sandbox; host checklist: plan §7.3.

## Notes for WP5 (switch from the fake to `run()`)

- No GUI change is needed: `run()` is used whenever `debug_fake_result` is not "1". Keep or delete the fake
  (`fake_run`, `FakeSelector`, the flag) in `SmartSupportsPanel.cpp`.
- What the GUI relies on: `run()` throws `CanceledException` on cancel (from any phase); `ProgressFn` may be called
  from any thread; `Result::paint` has one entry per `in.volumes` in order with a correct `changed`;
  `painted_areas[cause]` feeds the summary; the report order/grouping is WP2c0's; `Result::fingerprint` is unused
  (the GUI computes `config_fingerprint` itself).
