# Plan review 1 — theory conformance and algorithm correctness

Reviewer #1, 2026-10-15. Reviewed: `notes/plan.md` (draft 2026-10-15) against `notes/theory.md`
and research-algo §3/§4.

Assumptions for all numbers below:
- α = β = 45°, h = 0.2, K = 3, H_s = 1, B = B_ceil = 40, f_hard = 1.5;
- default widths: ext_w ≈ 0.45, support line ≈ 0.35;
- tip_d 0.8, xy 50 %, contact 0.2, so z_top = 1.

## Verdict

**Not ready, one Blocker.**

What is right:
- Base model, accumulated reach, levels, causes and the island rules follow the theory closely.
- Tie handling is equivalent to the theory, since `min(max_θ lt_θ, lc) = max_θ min(lt_θ, lc)`.
- Most §10 numbers recompute correctly.

What is wrong:
- **Blocker:** plate reachability is tested in the wrong space. No slope steeper than ≈ 12° ever
  gets a plate placement. That disables the main H1/H3 use case, and tests #11–#13 fail.
- **Four Majors:**
  - a bookkeeping bug in the plan loop;
  - an on-model fallback broader than §5.3 allows;
  - hoop detection that accepts too much;
  - one wrong test expectation (#7).

## Blocker

**B1 — Reachability is applied to the need zone, not to the slicer's tip candidates.**

*Location:* §3.9 `a = z ∩ plate_ok_n`; §3.7; D10 and the "Why D10" paragraph.

*Problem.*
- `plate_ok_n = complement(avoid_{t(n)} ⊕ clearance)`.
- The collision at the tip layer n−2 contains `L_{n−1} ⊕ (r + xy/2)`. Source:
  `TreeModelVolumes.cpp:490-526`, `required_range_x = xy/2` for z_top = 1; the plan's own §3.7
  formula says the same.
- A cantilever zone lies inside its front, so it is never more than one step `0.2/tanθ` beyond
  `L_{n−1}`.
- With the D10 clearance, `plate_ok` excludes `L_{n−1} ⊕ (0.4 + 0.11 + 0.425) ≈ 0.94 mm`.
- So `z ∩ plate_ok = ∅` for every slope steeper than atan(0.2/0.94) ≈ **12°**. The same limit is
  ≈ 21° without clearance and 7.4° with the theory's clearance.

*Cases hit* (step per layer in brackets):
- sphere rings (0.69 at r 5.9; 0.27 at r 12.1);
- 40° wall (0.24);
- 30° arm and mushroom cap (0.35);
- 15° beam (0.75);
- tops of horizontal holes, domes without hoop credit;
- cone-tail rings (0.115).

*What happens.*
- Strict mode: nothing is painted on slopes.
- Plate-preferred mode:
  - should needs are only reported;
  - must needs are painted only at c > 4.5, labelled "on the model" with a false warning.
- What the slicer really does (buildplate_only off):
  - it places tips outside the *collision* (`TreeSupport.cpp:1323` uses `getCollision`), in the
    tip_d spill-over beyond L_n (`:297`);
  - it classifies them `TO_BP` (`:392-405`).
- `predict_F` (`∩ buildable ⊕ r_tip`) counts those same tips, so placement and prediction
  contradict each other.
- Tests that fail: #11 and #12 (rings), #13 (placement at 15.6 mm).

*"Why D10" is wrong in four ways.*
- It measures only from `L_{n−2}` (`r_n − r_{n−2} ≈ 1.3`) and omits the `L_{n−1}` term.
- `r + xy + tip_d + extra_outset ≈ 0.4 + 0.225 + 0.8 + 0.225 = 1.65`, not ≥ 1.8.
- The direction is inverted: it is the *steeper* strips that never get a tip.
- D10 leaves the effective limit at ≈ 12°, the very value it claims to remove.

The theory has the same category error. §5.2 writes "Z ∩ R_n", but R_n is in tip space and Z is in
paint space.

*Fix.*
- Define feasibility in paint space, with one function shared with `predict_F`:
  `tips(z) = ((z ∩ O_n) ⊕ tip_d − L_{n−1}) ⊕ extra_outset − coll_{t(n)}`.
  Strict mode also subtracts `avoid_{t(n)}`.
- Plate-supported part of z: `z ∩ ((tips(z) ∩ plate_ok_raw) ⊕ r_tip)`.
- Use the clearance only to flag "may land on the model" (`tips(z) ⊄ plate_ok_raw`), never to
  reject a placement.
- Drop D10. Amend theory §5.1/§5.2.
- Add unit tests for plate feasibility of: the 40° wall, the sphere ring at r 12, the cone tail.

## Major

**M1 — The break path keeps the evaluation of discarded placements.**

*Location:* §3.9, termination line.

*Problem.*
- On insufficient progress the loop does `ev = ev'; break` without `placements_n += new`.
- `ev'` was evaluated with F including `new`, but `new` is never painted.
- So `G_n`, the carry (c* = 0 under F) and the residual all reflect support that does not exist.
- Result: spurious chain resets above, and under-reported needs.

*Evidence:* §5.2 says such an iteration fails, so its placements must not count.

*Fix:* on that branch keep the previous `ev` and discard `new`.

**M2 — The on-model fallback is broader than §5.3 allows.**

*Location:* §3.9 component loop; D12.

*Problems.*
1. `z − a` goes on the model in the same iteration as the plate part `a`, before any
   re-evaluation. Support anchors pass ĉ = 0, so support at `a` usually brings c in `z − a` back
   under K. The on-model part is then not needed.
2. `level = max level inside z` puts the *should* parts of a must component on the model. §5.3
   allows on-model only for must needs; should and may needs go to the report.
3. With `Z = need zone`, the theory's cantilever freedom is never tried: rows in
   `[D − K, min(D, B)]`, partly inside the covered region (§3.3, §5.2). A reachable row next to an
   unreachable need zone is missed, and the need falls back to the model.

*Fix.*
1. Place the plate parts first and re-evaluate.
2. If a need has no plate position, widen Z within the front to `C' ∩ (need ⊕ K_max)`.
3. Then place on the model only the **must-level** points of what remains.
4. Add the loss of freedom to §3.12 row 1.

**M3 — Hoop detection accepts too much.**

*Location:* §3.5 hoop candidates, `hoop_feret = max Feret over its matched holes`.

*Problems.*
- **Several holes.** A lid or grille anchored at its rim with bolt or vent holes qualifies: every
  hole is fully free and the contour lies 100 % on S. The whole front becomes a covered hoop band.
  Theory §2.4 excludes "a front that merely contains holes"; a hoop band must lie *between* H and
  the outer boundary.
- **No band-width limit.** A Ø100 flat ceiling with one Ø10 hole is fully covered, although its
  bridge lines are ~100 mm (t/B = 2.5, must). The H2 argument — the free inner edge would have to
  stretch — is about ring bands. This gap is inherited from the theory.

*Fix.*
- Grant hoop credit only if C's set of holes is exactly {H}.
- Limit the credit to narrow bands, e.g. `max dist(p, H)` ≤ a hoop-width limit (B_max/2 as a
  placeholder). Judge the rest of the front by t and c.
- Record this as an open theory question.
- Add tests:
  - lid Ø30 with 4 × Ø5 holes → not a hoop band;
  - Ø100 ceiling with a Ø10 hole → long-span or cantilever needs.

**M4 — Test #7 expectation is wrong.**

*Location:* §7.2 #7, "with `B_ceil = 0`: cantilever must needs above z ≈ 14".

*Derivation.* For inner radius R and hole radius x ≤ R/√2:
`c(x) = R√2 − x − √(R² − x²)`. With R = 20:

| c | hole radius x | z |
|---|---|---|
| 0 | 14.1 | 14.1 |
| 3 (first need, should) | 6.3 | 18.98 |
| 4.5 (must) | 4.24 | 19.55 |

With D12 the first need is *should* and gets placed. Must appears only if the need stays unplaced.

*Fix:* expect "first cantilever need at z 19.0 ± 0.3 (hole radius 6.3 ± 0.3), level should; no
need below z 18.5". If the mesh has outer R 20, use the inner radius instead (R 18 → z 17.2).

## Minor

1. **Tip layers.** §3.7 says "t(n) < 0 → avoid = ∅".
   - Without a raft the slicer builds no tips for overhangs on object layers ≤ z_top + 1, because
     `first_support_layer = max(raft − z_delta, 1)` (`TreeSupport.cpp:1300-1305`).
   - The plan treats layer 1 as reachable and layer 2 as normal, so it predicts F where nothing is
     built (islands below z ≈ 0.6 mm).
   - Fix: buildable = ∅ on those layers; report "will be dropped".
2. **Front filter.** §3.5 keeps only fronts with area > 1e-4 mm², which contradicts §2.1 "U_n is
   not filtered".
   - On a continuous slope, a dropped front makes `c_hat` fall through to layer m−1 with factor 0.8.
   - Fix: carry every front; filter only the needs.
3. **`c_hat` skips ungrounded fronts** and keeps searching downward, so it picks an older, decayed
   c*. The theory stops at the topmost layer that has a front in reach.
   - Fix: stop at that layer; if it holds only ungrounded fronts, ĉ = 0 (folded into the island).
4. **Exact zone uses only the projection** (§3.8).
   - The minimum over a of `|p − a| + ĉ(a)` is not at the projection when `|dĉ/ds| > 1`, e.g.
     when ĉ is interpolated across a reset border (3 → 0 within 0.2 mm).
   - This overestimates c by up to the jump and creates spurious needs along reset borders.
   - Fix: do not interpolate across zero-region borders; also evaluate segment endpoints.
5. **Ties.**
   - "Worst over tied θ" is equivalent to the theory's union of needs.
   - The cap of 4 directions under-samples round fronts (theory §10 flange: "ties in every
     direction").
   - The carry's zero region must be "Covered under **all** tied θ", not the union of per-θ
     strips. State this explicitly.
6. **End-closure test** (§3.8, thin fronts). This is a needed change to the theory, not an
   implementation detail.
   - Read literally, §2.4 gives tension to every thin strip anchored along its long side, because
     its U-shaped free outline stays within one width of its chord. That includes hole strips.
   - List it in §3.12 and in the §10 doc changes.
   - Require both endpoints to touch *anchors*, not "neither" boundaries.
   - Add a test: lean-to with one open end → no tension.
7. **Blocker clearance.** The reduced D10 clearance is also used in `manual_blk_n ⊕ clearance`.
   - The slicer grows enforced areas by tip_d, unclipped by blockers (`TreeSupport.cpp:289-299`).
     Support therefore reaches ~0.4 mm into blocked areas, against P5.
   - Fix: keep `tip_d + extra_outset` for blockers.
8. **Long-span rows only at the midpoint** (§3.8), instead of anywhere in the interval
   `[L − B, B]` (§3.2, §5.2).
   - The theory row "bridge 60 mm, model below the middle" cannot be reproduced.
   - These rows are anchors under P3, so their position matters.
   - Fix: sample the interval and keep the reachable positions.
9. **D9 "not planned"** changes the §6 composition: the theory plans every need and filters only
   the output.
   - D9 does not break P3 (it is not toggle-driven) or P4 (it adds no anchors).
   - Add it to the §10 doc changes.
   - Also state which report reason applies when `max_place_iterations` runs out.
10. **Tail** (§3.6). `part` is the union of all tree slices, so one wide branch ends the tail of a
    thin sibling. Check the shrink per component.
11. **Numbers and test setup.**
    - 40° excess per layer is `0.2·(1/tan 40° − 1) = 0.03835`, not 0.03838 (80 layers → 3.068).
    - #13: the first need is at layer 79 above the start, i.e. 15.8 mm (within tolerance).
    - #11 with D12 resets: the reset is at r 5.93 on layer 4, so the second ring is at r 12.1.
      That is still inside 11.9 ± 1.0.
    - #3 needs a cube at least 30 mm wide; otherwise 204 mm² is wrong.
    - Unit test "exact straight anchor ± 0.01" holds in grid mode only for axis-aligned anchors.
      The 16-neighbour Dijkstra error is ≤ 2.7 % beyond ρ, i.e. 0.12 mm at 5 mm.
    - "2-layer riser" must mean Δz = 0.4 between treads to give the factor 0.8.
    - #9 must state the block depth: whether the inside of the hole is reachable depends on it.

## Checked and consistent (no finding)

- **Base model and reach:**
  - w_n, including α = 0;
  - S, U and fronts; printable opening;
  - per-point groundedness, including F and T_n; weak contact;
  - anchor / free / neither classification; D as the medium for c and t;
  - β < α via Δ; ĉ decay over H_s.
- **Stairs:** `2.3 + 0.8·2.3 = 4.14` → placements at steps 2, 4, 6.
- **Levels and causes:** level table; cause on ties; tail override.
- **Islands:** tree construction and tolerance.
- **Plan loop:** overrides and iterations keep P3 and P4 (apart from M1).
- **Verification** against the actual paint.

Recomputed and correct:
- Shelves:
  - shelf 2: c = 1.8;
  - shelf 10 × 15: zone starts 3.2 from the wall, area 102 mm²;
  - shelf 10 × 30: 204 mm².
- T-shape: 2 × 118 mm², c = 14.8.
- Bridge directions for 10 × 15, 10 × 30 and 10 × 20 (tie).
- Sphere ring 1: r 5.80–5.93, layer 4, c = 3.14.
- 44° wall: c reaches 3 mm after 84.4 mm.
- Horizontal holes Ø8 / Ø20 / Ø50: c = 1.66 / 4.14 / 10.36.
- Domes: R20 Feret 28.3; R60 needs from hole radius 29.7.
- Flat bridge 80: t/B = 2.0 → should.
- Cone tail: 2.6 mm.
