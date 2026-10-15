# Work package log — WP2c0 (report, contract, fingerprint)

Scope (plan §8.2): `src/libslic3r/Support/SmartSupportsReport.cpp` — `build_report` (§3.11), `check_contract`,
`recommended_changes`, `apply_recommended` (§6.4), `config_fingerprint` (§6.8); tests
`tests/libslic3r/test_smart_supports_report.cpp` (`[SmartSupports][sas-report]`, §7.1). No header changes.

## Contract (`check_contract`)

Issues in `ContractIssue::Key` order; every key is checked on its own (support off also reports AutoOn etc.,
so one "Fix…" sets everything).

| Key | Condition | `current` | `recommended` | advisory |
|---|---|---|---|---|
| SupportOff | `!support_material` | `opt_serialize` | `1` | no |
| AutoOn | `support_material_auto` | ″ | `0` | no |
| NotOrganic | style ≠ `smsOrganic` (grid, snug, legacy tree) | ″ | `organic` | no |
| EnforceLayers | `support_material_enforce_layers > 0` | ″ | `0` | no |
| ContactZero | `support_material_contact_distance < EPSILON` (as `derive_params`) | ″ | `0.2` | no |
| BuildplateMismatch | `support_material_buildplate_only ≠ (policy == PlateOnly)` | ″ | `1` / `0` | no |
| InterfaceLayers | `support_material_interface_layers > 0` | ″ | `0` | yes |
| Raft | `raft_layers > 0` | `""` | `""` | yes (info) |
| ReachBelowTipSpacing | `derived && K_max < tip_spacing / 2` | `""` | `""` | yes (info) |

`recommended_changes(cfg, policy, include_advisory)` = `(option key, recommended)` of the non-info issues;
the advisory InterfaceLayers only with `include_advisory` (then appended after the non-advisory ones). Raft and
ReachBelowTipSpacing are never changed. `apply_recommended` calls `ModelConfig::set_deserialize_strict` once per
change, so the object config (and its timestamp) is untouched when nothing has to change.

## Fingerprint (`config_fingerprint`)

64-bit FNV-1a over a canonical byte stream; it hashes mesh pointers, so it is comparable within one session only
(D14). It covers:
- all `UserParams` fields; `beta_deg` only when `!beta_equals_alpha`;
- these options of the effective config, of every volume config and of every layer range config (absent keys
  hashed as absent): `bridge_angle`, `elefant_foot_compensation`, `external_perimeter_extrusion_width`, `extruder`,
  `extrusion_width`, `filament_shrinkage_compensation_xy/z`, `first_layer_extrusion_width`, `first_layer_height`,
  `gcode_resolution`, `layer_height`, `nozzle_diameter`, `perimeter_extruder`, `perimeter_extrusion_width`,
  `raft_contact_distance`, `raft_layers`, `slice_closing_radius`, `slicing_mode`, `support_material`,
  `support_material_auto`, `support_material_bottom_contact_distance`, `support_material_bottom_interface_layers`,
  `support_material_buildplate_only`, `support_material_contact_distance`, `support_material_enforce_layers`,
  `support_material_extruder`, `support_material_extrusion_width`, `support_material_interface_layers`,
  `support_material_style`, `support_material_xy_spacing`, `support_tree_angle`, `support_tree_branch_diameter`,
  `support_tree_branch_distance`, `support_tree_tip_diameter`, `support_tree_top_rate`, `xy_size_compensation`;
- the layer height profile and the layer config ranges **by content**;
- the object id; the analysed instance's matrix without offset **and its z offset** (sinking); an out-of-range
  instance index hashes as "no instance";
- every volume: id, type, matrix, mesh pointer, its config (the key list above).

Not covered: the paint (LastRun keeps the timestamps), instance XY (bed-local), which instance is analysed (only
its matrix), speeds, patterns, `support_material_threshold` (α is a panel field) and other options that do not
change the analysis.

## Report (`build_report`)

- **Pieces.** Each ExPolygon of each `Need::area` is one piece (plan residual first, then the verify residual).
  A need with an empty area is one piece without geometry: never merged, `area_mm2 = 0`, `centroid = (0, 0)`.
- **Merge rule** (§3.11): same `(state, why, cause, level)`, same **or** adjacent layer, bounding boxes at most
  1 mm apart (one box inflated by 1 mm); transitive (union-find over the canonical piece order
  `(key, layer, bbox.min.y, bbox.min.x, area, input order)`).
- **Row.** `layer_first/last` and `z_first/z_last` (`print_z`) over the pieces, `area_mm2` = sum,
  `count` = pieces. Marker: the largest piece (first in canonical order on ties); `centroid` = its area centroid,
  or the projection onto its boundary when the centroid lies outside (rings); `slice_z` = that piece's layer.
- **Groups.** Problems = states up to `MayMergeOntoModel`, except `BlockedByUser`: a problem only for
  `Cause::Island`, a note otherwise (§3.11 "BlockedByUser (islands)"; §7.2 #19 expects a note for a blocked
  shelf). Notes = the rest. The returned vector holds the problems first.
- **Order** within a group: **deviation** — the summary rows (CauseDisabled, ReliesOnDisabledCauses,
  SeveralPrintObjects, OtherObjectsNotConsidered; at most 5) come first in the notes group, in enum then cause
  order, so the cap never hides them; then `(level desc, area desc, layer_first, centroid x, y, id)` as in §3.11,
  id = canonical index of the row's first piece.
- **Cap** 50 rows per group (summary rows included); `report_more_out` = hidden rows per group.
- **Summary rows:**
  - `CauseDisabled` per unchecked cause (Island, Cantilever) that has placements this run would paint
    (`paintable && level ≥ lowest_level`): `count` = placements, area, layer range, max level, marker on the
    largest component. No row if nothing of that cause was planned. Long spans are never a disabled cause in v1
    (they are `LongSpanNotPainted` rows from the residual).
  - one `ReliesOnDisabledCauses` if a CauseDisabled row exists and painted placements of checked causes come after
    the first disabled placement in plan order (layer, then Island < Cantilever, §3.9): `count` = those
    placements, `cause` = cause of the largest one, max level, marker.
  - `SeveralPrintObjects` if `num_print_objects > 1`: `count = num_print_objects`, no position (area 0), level May.
  - `OtherObjectsNotConsidered` if `other_objects_on_bed`: no position, level May.

## Notes for dependent work packages

- **WP2c1b:** `Plan::residual` (+ the verify residual, reason `RemainingAfterPaint`) is authoritative for every
  per-piece row; `build_report` derives nothing from `Plan::placements` except the summary rows above. So put
  **`LongSpanNotPainted`** rows into the residual yourself (area = the row placement's area, level of the placement),
  as well as PartlyOnModel, OnModelFallback, OnModelTooShort, MayMergeOntoModel, folded trees and
  TailWithoutDownwardFacets. For a tail, an area (e.g. the tail's slice at its last layer) gives the row a marker;
  with ∅ it has none. `Need::layer` must be `< in.layers.size()` (else z = 0). `Result::fingerprint` cannot be
  computed in `run()` (it needs the live Model): leave it 0, the GUI computes `config_fingerprint`.
- **WP3:**
  - Group of a row (also for "Problems"/"Notes" counts): `BlockedByUser` → problem iff `cause == Island`; else
    `state ≤ MayMergeOntoModel`. Totals = shown rows + `report_more`.
  - Rows with `area_mm2 == 0` (SeveralPrintObjects, OtherObjectsNotConsidered, tails without area) have no
    marker position. SeveralPrintObjects: N = `in.analysed_instance_idx + 1`. CauseDisabled: `cause` selects
    "Islands" / "Overhangs".
  - The advisory notes `ReachBelowTipSpacing` and `Raft` have no `ReportState`: show them from `in.contract`
    (filled by `build_input`) in the report window.
  - Pre-run warning / gate = non-advisory issues of `check_contract(effective, policy, nullptr)`.
- **Optional header request** (not needed, not made): an `inline bool is_problem(const ReportEntry &)` in
  `SmartSupports.hpp` would let the GUI share the group rule instead of copying it.
