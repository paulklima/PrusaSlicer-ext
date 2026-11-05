# Plan review 5 — algorithm, round 2

Reviewer #5, 2026-10-15. Reviewed: `notes/plan.md` revision 2 against `notes/theory.md`, reviews 1 and 4,
and `TreeSupport.cpp` 225-312, 1118-1426, 2696-2716, `TreeModelVolumes.cpp` 302-364, 425-715, 834-860.

Assumptions: h = contact = 0.2 (z_top = 1), tip_d 0.8 (r 0.4), xy 50 % → xy_min 0.225, α = 45°.
Then coll_{n−2} ⊇ L_{n−1} ⊕ (r + g) with **g = xy_min/2 = 0.1125** (`required_range_x`, :497-526).

## Verdict

**Not ready for WP0: 1 Blocker, 3 Majors.** All of them are plan-text fixes. Two of them change
headers that WP0 freezes (`ContractIssue::Key`, `LayerGeom`, `ReachCarry::c_hat`).

Round-1 status:
- **1-B1** is resolved for fronts with w ≥ g. Sphere rings (r 5.9: bp ⊕ r from 5.36 covers [5.80, 5.93];
  r 12.1), the 40° wall and 30° arms get plate placements. The T-arm over the 80×80 base correctly gets
  none: at arm height the avoidance has shrunk to a half-width of 40.6 − 0.168·100 ≈ 24, which is more
  than the arm tip at 20. `plate_part` and `predict_F` share `tips()`. `predict_F` is superadditive, so
  `F ∪= predict_F(new)` is conservative. **Not resolved for tails and roofs** (B1, M1).
- **4-B1**: the structure is correct. Cluster windows are disjoint, so F_grown (≤ tip_d) never leaves
  its cluster. A cluster's result depends only on UG_{n−1}, the carry and F ∩ win, so re-evaluating only
  the touched clusters is exact, and causality, P3 and P4 hold. The budget is not met for some shapes (M3).
- The other Majors of reviews 1 and 4 are resolved: termination, must-only fallback, hoop (D26), #7, ĉ
  sampling, jtRound S, raster fallbacks, prism soundness, cancel. The segment minimiser is correct:
  `s* = sp − g·d/√(1−g²)`. Remaining details are under Minor.

Reference cases recomputed and consistent:

| Case | Result |
|---|---|
| #2 | 102 mm², 3.2 mm |
| #4 | 2 × 118 mm² |
| #6 | cut at 39.6 of 79.6 |
| #7 | x 6.30, z 18.98, should |
| #8 | need at x 11.88; credit from x ≤ 10; c from the reset reaches only 1.16 → one ring |
| #9 | the D27 end-closure removes tension from hole strips; c 1.66 / 4.14 / 10.36 |
| #11 | ring at layer 4 [5.80, 5.93]; next ring at r 12.12; then max c 0.26 |
| #13 | 79 layers = 15.8 mm |
| #15 | 4.14 |

## Blocker

**B1 — Tails can never be resolved (what is left of 1-B1).**
*Where:* §3.8 "Layer needs" (tail override), §3.7 `predict_F`/`plate_part` and "Checks", §7.1 plate
feasibility.

*Problem (a): pseudocode.*
- Tail needs are `tail_needs[n] − T_n`; F never reduces them.
- At an island's first layer, `tn = part` whenever the first slice is narrower than W_stiff. That is
  almost every point-like island: cone tips, sphere bottoms, fingertips.
- The island need is replaced by a tail need that no placement reduces. Progress is 0, so the loop
  takes "discard new → Unplaceable", and these islands are never painted.
- #12 passes only because its first slice happens to be ≥ 3 mm wide.

*Problem (b): geometry, even with `− F`.*
- Tips lie outside coll ⊇ L_{n−1} ⊕ (r + g). So `bp ⊕ r_tip`, and with it `predict_F`, never reach
  the band L_{n−1} ⊕ g.
- A tail ring at w = 0 keeps a residual of [0, g] × perimeter on every layer. That is ≥ A_min for any
  ring wider than Ø0.3, so every tail layer ends as "no progress → Unplaceable".
- Inverted 60° cone (s = 0.115): `plate_part` = [0.1125, 0.115], about 2 % of the ring. §3.7 Checks and
  §7.1 expect "whole zone ±10 %".
- The same happens to fronts when w_n < g (α ≳ 61° at 0.2 mm). The band [w, g] is never covered, so c
  never resets, and every layer above the first need is Unplaceable.

*Fix.*
- Define covered-by-support once, in `Reachability`:
  `F^cov = F ∪ (L_n ∩ (F ⊕ g) ∩ (L_{n−1} ⊕ g))`, with g = xy_min/2 if z_top = 1, else 0. This is the
  band that the slicer leaves free on purpose.
- Use F^cov for C', for the tail residual (`tn − T_n − F^cov_n`), for `plate_part` and for verification.
- Correct the §3.7/§7.1 expectation.
- Add a reference case: inverted 60° cone with a floating apex → tail painted, no Unplaceable.
- Record the band in theory §5.2.

## Major

**M1 — The roof path is not mirrored.**
*Where:* §3.7, D23, the §6.4 contract table.

*Problem.*
- With `support_material_interface_layers` > 0 (PrintConfig default 2), the contact is
  `overhang_roofs = F_grown − forbidden` (`TreeSupport.cpp:1414-1426`).
- `sample_overhang_area` inserts the roof polygon itself whenever the next roof layer has
  ≥ minimum_roof_area = 1 mm² (`:1122-1170`, `TreeSupportCommon.hpp:131`). There is no `⊕ r_tip` and no
  extra_outset.
- The contact therefore starts at r + g = 0.51 from L_{n−1}:
  - the §3.7 40° wall check fails, because the front [0.2, 0.238] gets no contact; the same holds for
    the sphere rings;
  - `predict_F` over-claims on every slope steeper than ≈ 21°;
  - no test in §7 notices.

*Fix* (before WP0, because `ContractIssue::Key` is frozen):
- Either add a contract key `InterfaceLayers` (recommend 0, which is what D23 already assumes) or mirror
  the roof path.
- Add a test that compares `predict_F` with the contact layer of a real `process()` run, on the 40° wall
  and the sphere ring, with interface layers 0 and 2.

**M2 — Avoidance pipelining contradicts itself and can deadlock.**
*Where:* §3.9 line 744 vs §2.3 and §3.13.

*Problem.*
- §3.9 runs `parallel_invoke{Phase A, chain}` *before* the layer loop, so nothing overlaps. Phase A
  also appears twice there: `build_base_model()` and "Phase A".
- §2.3 instead lets Phase B spin on `avoid_ready` inside TBB. Implemented that way, it can hang:
  - with `max_allowed_parallelism = 1` (test #20), the Phase B branch can run first on the only thread
    and spin forever;
  - if the chain throws (cancel), Phase B also spins forever, because `parallel_invoke` rethrows only
    after both branches have finished.

*Fix.*
- Use a pull model: `Reachability::ensure(k)` advances the chain under a mutex on the calling thread,
  with an optional background prefetch, and checks cancel in the wait.
- Specify where the chain stops. The highest tip layer is unknown while Phase A runs.

**M3 — The Phase B budget is not plausible for ring-front layers.**
*Where:* §3.8, §3.13, resolution 4-B1.1 "C".

*Problem.*
- Clusters use bbox windows. A ring front has bbox ≈ the whole layer. Ring fronts appear on the lower
  halves of spheres, domes and torsos, i.e. on most layers of round figurines.
- Each evaluation then runs about 8 whole-layer operations:
  - `Lc − Uc`;
  - the opening (2 offsets);
  - `expand(GP)`;
  - `expand((L−C) ∪ F)`;
  - the D diff;
  - the clips.
- That is 20–40 ms per evaluation, serial with one cluster, so 20–40 s per pass before ĉ,
  classification and re-evaluation.
- The avoidance chain is itself 8–25 s serial (≥ 3 offsets, a union and a simplify per layer). It is
  not "hidden behind Phase A ≈ 5 s", and Phase B waits on it.

*Fix* (this changes the frozen `LayerGeom`):
- Phase A computes in parallel:
  - the small `L − opening(L, ¼·ext_w)` per layer;
  - per cluster a polygon band subject `clip(L, ∪C ⊕ m)` instead of a bbox;
  - the collision offsets `L_k ⊕ (r+xy)` and `L_k ⊕ (r+g)`.
- Choose the grid resolution per D component.
- The WP5 benchmark adds a model dominated by spheres or torsos.

## Minor

1. **`c_hat` lacks w_n.** R = Σ_{m+1..n} w_k includes w_n, and α = 0 needs ext_w of layer n−1. Add the
   parameter (frozen header).
2. **Stage 3** uses `M − blk` without the clearance. P5 and D10 require `blk ⊕ (tip_d + extra_outset)`.
   Also, a stage that fails the progress rule should fall through to the next stage, as "until one
   makes progress" says. The loop currently breaks.
3. **§3.2 window.** The `Reachability` comment omits `r + xy` (§3.7 has it). Without it, collision from L
   just outside the window is lost.
4. **ĉ memo.** The value is computed at whichever unquantised point comes first in the parallel chunks,
   so it depends on scheduling. Compute it at the snapped point (D29, #20).
5. **§3.14 #14 bound is wrong.** A slit closed by conservative rasterisation can shortcut an arbitrarily
   long detour, not just ≤ res. Cap res at 0.1 mm, or keep slits narrower than res open.
6. **Verification step 5.** M and the ĉ of the first rebuilt layer are undefined. Keep a snapshot of the
   carry, or re-run one pass from layer 0 with F_act.
7. **§7.1 "Grid within 2.75 %" at 13.28°.** That angle is the theoretical worst case (2.73 %), and
   discretisation comes on top, so the test is flaky. Use 3.5 %, or 2.75 % + res/d.
8. **Carry memory is unbounded.** Fields with up to 250k pixels are kept for ⌈H_s/h⌉+1 layers (101 at
   H_s 5, h 0.05). Crop carried fields to bbox(C).
9. **#10, "Prefer build plate".**
   - The must-only on-model placement covers c > 4.5.
   - F_grown reaches only 0.8 inward from it.
   - A should band with c ∈ (3, 3.7] therefore remains on each arm and is reported as an Unplaceable
     problem. That matches the theory.
   - Add it to the expectation, or show it as a note.
10. **k_d from sin α** under-dilates tails and on-model flanks steeper than α. For a 60° cone k_d should
    be 2. Use the steepest painted flank.
11. **Report reasons.** The rule that chooses BlockedByUser over Unplaceable is undefined.
