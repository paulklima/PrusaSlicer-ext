# Plan review 7 (round 3): §3.7 contact model and the WP0 interfaces

Reviewed: `notes/plan.md` revision 3, §3.7 and §3.1/§3.2/§8 WP0. Code: `version_2.9.6` tree on the
feature branch.

## Verdict

**Not ready for WP0 yet.** There is no Blocker. M3 and M4 change frozen headers (`ContractIssue`,
`Params`), and so do the decisions in m4, m5 and m8. Make those edits before the freeze. M1 and M2
need no header change but must be fixed in the plan before WP2r and WP2c1b start.

Verified as correct:
- roof offset `r + xy_min/2 + 5 µm` = 0.5175 (`TreeSupport.cpp:1327, 1418`);
- `extra_outset` (`:1270`);
- avoidance steps and stretching (`TreeModelVolumes.cpp:670-697`);
- `tips_possible` (`TreeSupport.cpp:1300-1307`);
- tip status (`:392-405`);
- `F_grown` (`:289-298`).

The pull model, as specified, is thread-safe, deterministic and free of deadlocks:
- the prefetch path is ClipperUtils only, with no TBB;
- `global_control(1)` limits only TBB;
- elements are preallocated, with release/acquire on `ready`;
- exceptions are captured.

The remaining gaps are listed in m5.

## Major

**M1 — The g band is too narrow next to steep lower layers, so steep tails are never resolved.**
(§3.4 row g, §3.7 `cover`, §3.7 checks, §7.2 #26)
- Evidence:
  - `coll_t` contains `L_{t−z_bot..t} ⊕ (r + xy_min)` (`TreeModelVolumes.cpp:487-489, 465`) besides the layers above (`:521-526`). The final organic trim uses the same layers at radius 0 (`OrganicSupport.cpp:1241`).
  - With `z_top = 1`, the contact therefore starts at `max(xy/2, xy − s)` from `L_{n−1}`. Here `s` is how far `L_{n−1}` extends beyond `L_{n−2}`.
  - Tails use `w = 0`, so their ring width is `s`. The band covers only `[xy/2 − s, s]`.
  - Results at h 0.2 and xy_min 0.225:

    | Cone | s (mm) | Uncovered |
    |---|---|---|
    | 68° | 0.08 | `[0, 0.0325]` |
    | ≥ 74° | ≤ 0.056 | the whole ring |

  - With `z_top = 2`, the plan sets g = 0, but the real gap is `xy − 2s`. At 0.1 mm layers even the 60° cone fails.
  - The result: `plate_part = ∅` → no stage progresses → `Unplaceable`. Nothing is painted above the island's first layer.
  - #26 (60°, s = 0.1155 > 0.1125) passes by 3 µm and hides the problem.
- Fix:
  - Band width `G = xy_min` for every `z_top`. This is the largest gap the collision can impose next to `L_{n−1}` (a vertical wall below). On fronts (`s ≥ w`) it adds nothing beyond the contact.
  - Update §3.4, `gap_g_mm()`, the §3.7 table and the theory amendment.
  - Add #26b: an inverted 75° cone, and the 60° cone at 0.1 mm layers with contact 0.2.

**M2 — `predict_F` is not superadditive with roofs.** (§3.9 accept line, "P4 holds" paragraph, §3.10)
- Evidence:
  - The roof decision is made per `union_ex` component of the whole layer's overhang (`TreeSupport.cpp:1418-1424`). Its area test runs at `t−1` (`:1140-1153`). `remove_small` never runs because `minimum_support_area = 0` (`:1419`, `TreeSupportCommon.hpp:159`).
  - A small piece alone stays below 1 mm² and gets tips, with contact from g. Merged with a neighbouring placement or with manual paint, it becomes a roof, with contact from `r + g`.
  - Therefore `F ∪= predict_F(new)` over-predicts, and so does `F_man`. Verification catches this only as `RemainingAfterPaint`.
- Fix:
  - `make_plan` keeps the painted zones `E_n` (manual plus accepted).
  - Every trial evaluates `predict_F(E_n ∪ new)`, limited to the window.
  - `planned_F = predict_F(E_n) − F_man`.
  - Restrict the superadditivity / P4 claim to interface 0.

**M3 — Recommending `InterfaceLayers = 0` up front is not necessary and costs surface quality.**
(D30, §6.4 table, §3.14 #15, §11 Q3, #27, §7.3 #8)
- The roof path is mirrored, so the prediction is correct for any interface count.
- Roofs only block the `r + g` band (0.51 mm):
  - steep slope fronts;
  - **every tail ring**: its roof component is an annulus about 0.3 mm wide around `R + 0.66` mm, so it is ≥ 1.2 mm² and always becomes a roof.
- Flat overhangs (shelves, T arms, first layers of islands) are covered by roofs and get the dense interface that users want.
- Setting 0 has side effects the plan does not list:
  - no top interface on any flat overhang;
  - bottom interfaces also switch off (`TreeSupportCommon.cpp:51, 67`);
  - short on-model branches get deleted (`supports_roof` is false, `TreeSupport.cpp:2708`), so more `OnModelTooShort`.
- Fact correction: the PrintConfig default is **3**, not 2 (`PrintConfig.cpp:3436`; "2 (default)" is only the enum label). The PrusaResearch common profile uses 2. So nearly every user would get the warning and the pre-run prompt.
- Fix:
  - (a) Run with the user's interface setting. Remove `InterfaceLayers` from the pre-run warning line and the "Apply and paint" gate.
  - (b) Header: add `bool advisory` (or a severity) to `ContractIssue`.
  - (c) After a run with `InterfaceGap` residuals, show one problem row: "N areas (steep slopes, hanging tips) cannot be supported while Top interface layers > 0". Its Fix… list states the three side effects.
  - (d) Open question, not v1: the per-component `minimum_roof_area` rule could be used to keep roofs on flat areas while narrow slope or tail pieces whose `F_grown − forbid` component is < 1 mm² get tips. This needs a slicer check first.
  - (e) Adapt #27 and §7.3 #8.

**M4 — The contact is modelled as a solid area, but the slicer places tips at intervals.**
(§3.7 `contact`, §3.14 #8, §7.1 cross-check row, WP2r acceptance)
- Evidence:
  - Tips are points on support lines `support_tree_branch_distance` (1 mm) apart, resampled every `connect_length` (`TreeSupport.cpp:1181-1184`).
  - `connect_length = lw·100/top_rate + max(2r − lw, 0)` (`:1258`): ≈ 3.2 mm with stock settings (top rate 15 %), ≈ 1.6 mm with the Prusa profile (30 %).
  - Thin overhangs fall back to the outline (`:1189-1201`).
  - On the 40° front, one tip disc covers about 0.55 mm of front. The tips of that layer alone therefore cover 17–35 % of its area, and the plan predicts 100 %. "Within 15 %" fails by construction.
  - Semantically, "covered ⇒ supported" holds only if `K_max ≥ ~spacing/2`. The slider minimum is 0.5.
- Fix:
  - Add `Params::tip_spacing` (`connect_length`) and `branch_distance`, derived in WP0.
  - State the assumption in §3.7 and theory §5. Add a contract note when `K_max < tip_spacing/2`.
  - Make the cross-check distance-based: predicted-covered samples must have real layer-t support within `tip_spacing/2 + r`, for ≥ 90 % of samples. Log the area ratio only for calibration.

## Minor

- **m1 — T is over-approximated on the outer side** (§3.7 `tips`).
  - The outset grows only the forbidden remainder (`TreeSupport.cpp:1340-1366`).
  - The real T is `(F_grown − forbid) ∪ ((((F_grown ∩ forbid) − T0⊕lw/2) ⊕ extra_outset) − forbid)`. For the 40° front that is [0.51, 1.04], not [0.51, 1.44].
  - The 1.44 also needs `min_xy_dist = true`, while the `Params` default and the §7 config give false (xy 50 % = xy_min), i.e. an outset of 0.19.
  - The over-extension can mark tips as `bp` near avoidance edges. Mirror the remainder rule and fix the table.
- **m2 — `model_short`** (§3.7, D23): branches are deleted whenever `supports_roof` is false (`:2708`; set from `dtt_roof`, `:917/944/1179`). That includes tip-path contacts with interface > 0. Apply `model_short` to tip-path contacts and use ∅ only for roof components.
- **m3 — Raft indexing.**
  - Theory §5.1 counts in support-layer space. `tip_layer(n) = n − (z_top+1)` omits the offset, while `tips_possible` includes it. With raft 2 (#23) the index into coll/avoid becomes negative.
  - The offset is `TreeSupportSettings::raft_layers.size()` (`TreeSupportCommon.cpp:147-176`), not the config value.
  - Use support-layer indices and treat coll/avoid as ∅ below the offset.
- **m4 — Reachability models the policy, not the slicer.**
  - The slicer chooses forbid and tip status by `support_material_buildplate_only` (`TreeSupport.cpp:1323-1325, 392-405`).
  - After "Paint anyway" with `BuildplateMismatch`, `OnModelFallback` placements are silently dropped by the slicer.
  - Header: add `Params::buildplate_only` (derived) for Reachability. `PlatePolicy` then only selects stages.
- **m5 — Prefetch cancel flag.**
  - `start_prefetch` takes `shared_ptr<atomic<bool>>`, but `run()`, `RunOptions` and `make_plan` carry only `ThrowOnCancel`. Either drop the parameter (destructor stop plus the cancel in `ensure` suffice) or add a thread-safe `std::function<bool()> is_canceled` (`ctl.was_canceled` is atomic, `BoostThreadWorker.hpp:152`).
  - Also specify that `ensure(k)` beyond the prefetch target first waits for the thread to finish (single writer), and that `ensure(k < 0)` is a no-op.
- **m6 — Who runs what.**
  - `make_plan` takes a built `BaseModel`, yet the §3.9 pseudo-code builds it and starts the prefetch. State that `run()` does `parallel_invoke`, `start_prefetch`, then `make_plan`.
  - `evaluate()` returns a reference. Document that the next `evaluate`/`commit` invalidates it; the L/L′ comparison needs copies.
- **m7 — Link stubs for the declarations in `TriangleSelector.hpp`/`Model.hpp`.**
  - They have no bodies until WP1a. WP3 (phase 1) needs auto-state detection for D14, so `has_auto_states()` fails WP3's PrusaSlicer link in its isolated tree.
  - `PredicateCursor` has no key function, so it has no vtable.
  - Either WP0 adds stub bodies, or the plan tells WP3 to use `TriangleSelector::has_facets(data, state)` (`TriangleSelector.hpp:365`).
- **m8 — `InterfaceGap` attribution.** `Contact::roof_gap` is a single bool per call, but "blocked only by the roof gap" needs the tip-path contact. Either add `ExPolygons tip_path_all` to `Contact` (header), or specify that WP2c1a derives it from `tips()`.
- **m9 — `derive_params` and the support-off state.** It must not construct `TreeSupportMeshGroupSettings` when support is off or the style is not tree/organic: that asserts (`TreeSupportCommon.cpp:32-33`), and support off is the usual state before Fix….
- **m10 — Roof-line check not mirrored.** The check at `TreeSupport.cpp:1162-1170` is not mirrored: narrow roofs that generate no lines fall back to tips. The error is conservative (false `InterfaceGap`). Note it in §3.14.
- **m11 — Default comments.** The `Params` comment "defaults = 2.9.6 defaults" contradicts `interface_layers = 0` (the stock default is 3). D30 repeats the "2".
