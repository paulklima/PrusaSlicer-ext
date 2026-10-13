# Theory — where FDM support is really needed

Theory document for `feature/smart-auto-supports`. It refines §2–§4 of `concept.md` with:
- a physical failure model;
- one geometric base model shared by all detectors;
- the detector classes defined on it;
- plate reachability for organic supports;
- the rules that let each detector be used alone or together.

Implementation decisions beyond what the theory needs go into `notes/plan.md`.

Status: **draft, reviewed** (2026-10-13; three part reviews and four full review rounds).
Code references are against `version_2.9.6`; §12 compares with the PrusaSlicer 3.0 alphas.

Target setup: **organic supports, enforcers only** (`support_material_auto` off; "Manual
Only" in 3.0), with support standing on the build plate wherever possible.

## 0. Summary

**When support is needed.** Fresh material needs support where:
- it has **nothing** to hold on to (islands);
- it hangs **by bending** from one side further than a short reach (cantilevers, including
  long shallow slopes);
- it hangs **in tension** between anchors over a span that is too long (long spans).

Everything else prints without support.

**Two principles.**
- **Causality:** every layer is judged only by what exists when it is printed.
- **Grounding:** only **grounded** material can be an anchor.

**One base model (§2) for all detectors.** Each unsupported point is either *covered*
(printable), *exempt* (noise, tolerated islands) or a *need* with exactly one cause and a
need level.

**Independence.** One joint plan is computed for all causes, regardless of the toggles. The
toggles only choose which causes are painted.
- Switching a detector on or off never changes another detector's paint (§6).
- A detector used alone gives a **consistent** result for its own problem, not a
  **sufficient** one for the whole print. The report lists the rest.

**Concept 2.1–2.3 are the right core, with four corrections.**
- Bridges follow the slicer's **single bridge direction per region**. Ring bands around a
  hole (domes, funnels, counterbores) get separate **hoop** credit.
- Cantilever reach **accumulates the excess overhang over layers**.
- Islands need **tolerance and tail** rules.
- Anchors must be **grounded**.

**Missing and critical parts.**
- **Causality** (§1, §2.2): closures cannot hold earlier layers.
- **Plate reachability and placement** (§5): organic support silently drops tips that cannot
  reach the bed.
- The **composition model** (§6).

**Stability** of the partial print is real but of a different nature. It is a separate,
later class (§3.4).

**Plate policy (default: plate-preferred).** Support goes on the plate wherever possible.
Support on the model is only a fallback for *must* needs, matching the request "wherever it
works out". A strict "never on the model" mode is optional.

**Hypotheses.** Test prints must confirm H1 (accumulation) and H2 (hoop bands); H3 (where
the demand lies) only orders the work (§1.1).

**Upstream.** The PrusaSlicer 3.0 alphas (up to alpha12) add nothing that overlaps with
this theory (§12).

## 1. Physical model — how unsupported material survives or fails

Fresh extrusion **holds** if:
- it **rests** on the layer below; or
- it hangs **in tension** between two anchors (the stretched extrudate solidifies as a taut
  line).

It **fails** if:
- it hangs **by bending** from one side beyond a short reach; or
- it has **nothing** to hold on to.

Independently of this, the partly printed object must stay mechanically **stable**.

| Mode | Example | Holds by | Fails as | Need level when over the limit |
|---|---|---|---|---|
| rests | wall steeper than the overhang angle α | layer below | — | — |
| tension | bridge between two walls | anchors at both ends of each line | sag growing with the span; very long spans fail outright | may → must |
| hoop (H2) | ring band around a round hole: dome ceiling, funnel, counterbore step | the closed ring itself | sag at large openings | judged as bending |
| bending | shelf, T-arm, mushroom cap, horizontal through-hole before it closes, outstretched arm | stiffness of a few fresh layers | droop and curl-up, then nozzle hits | should → must |
| nothing | lowest point of a part (local minimum) | — | spaghetti, dragged blob | must |
| stability | heavy part on a thin neck, tall slender part | — | breaks, tips over, leaves the bed | must (separate class) |

**Need levels** used later:
- **must**: the print fails;
- **should**: quality failure that can escalate;
- **may**: cosmetic.

**Causality.** A closure many layers higher cannot hold the layers printed before it. The
closing gap of a horizontal hole is a bridge, but the converging fronts before it hang by
bending.

### 1.1 Hypotheses (to be confirmed by test prints)

**H1 — accumulation.** For slopes flatter than α, the risk of failure grows with the
accumulated excess overhang along a continuous front, not only with the angle. Short shallow
chamfers print; long shallow slopes droop and curl.
- Common overhang tests vary the angle, not the extent, so H1 needs its own test.
- Without H1, `H_s = 0` gives the per-layer reach that OrcaSlicer uses.

**H2 — hoop bands.** A band that runs all the way around a compact hole, anchored on its
outside, cannot droop without stretching its free inner edge.
- An outward ring (mushroom cap) can droop or curl by shrinking, which cooling encourages.
- So domes print better than mushroom caps of the same angle, up to some opening `B_ceil`.
- Straight sides do not have this effect, so long slots get no hoop credit.
- This is also why a curvature-based limit misjudges dome tops. `SupportSpotsGenerator`
  allows `max(3 mm, 16 mm / (1+|curvature|)^3)` (`SupportSpotsGenerator.cpp:368-371`,
  `432-435`).
- Without H2, dome tops are cantilevers and get support, which is what the original
  complaint was about.

**H3 — demand.** In organic / figurine prints most real support demand is cantilevers, and
long-span support is rare. This affects the order of work (§8), not the rules.

## 2. Base model (per layer, shared by all detectors)

### 2.1 Unsupported area

**Slices.**
- `L_n` = `layer->lslices` (ExPolygons).
- `L_{−1}` is the bed, so layer 0 is never unsupported.
- Layer-0 slices are stored without elephant-foot compensation
  (`PrintObjectSlice.cpp:1022-1031`).

**Self-supporting step** `w_n = h_{n−1} / tan α`.
- α is taken from `support_material_threshold`, measured from horizontal. α = 0 means
  `w_n = 0.5 · external perimeter width`, as in the slicer.
- The slicer's own automatic detection adds `+1°` (`SupportMaterial.cpp:1171-1234`,
  `TreeSupport.cpp:234-270`). It does not matter here: in enforcers-only mode the slicer tests
  enforced areas with a zero angle, so α is purely this tool's criterion.

**Parts of a layer.**
- `S_n = L_n ∩ offset(L_{n−1}, w_n)` is self-supported.
- `U_n = L_n − S_n` is unsupported.
- A **front** `C` is a connected component of `U_n`.
- `U_n` is **not** filtered. The noise filter (`ε`, `A_min`) applies only to the final
  needs (§2.5). A filter on `U_n` would create a dead band near α that depends on layer height,
  and would break chains at random.

**Printable material** is `L_n` after a morphological opening by half the minimum feature
width. The rest (e.g. a 0.15 mm rib under a bridge) is never an anchor.

**Scope.** Only object slices are examined. Internal bridges over sparse infill are the
slicer's own business.

### 2.2 Groundedness, anchors, chains

**Grounded material** is decided per point, layer by layer:
- layer 0 is grounded (bed);
- a point of `S_n` is grounded if it lies in `offset(grounded part of L_{n−1}, w_n)` or on
  the predicted support contact `F_n`;
- a point of a front is grounded if its front has a grounded anchor, whether the point is
  covered or not, because it is attached;
- nothing else makes material grounded. In particular, connectivity inside a layer does not.

**Weak contact.** A slice counts as having **no** contact below if both hold:
- its overlap with `L_{n−1}` is smaller than `κ` × its own area;
- the extent (largest dimension) of that overlap is smaller than `a_min`.

Example: a sphere resting on an edge. Thin inclined rods and fingers overlap most of their
own area and keep their contact.

**Material anchors** of a front `C` are the parts of its boundary that touch grounded,
printable material of `S_n`.

**Support anchors** come from the predicted support contact `F_n` under the layer:

    C' = C − F_n      anchors(C') = ∂C' ∩ (grounded printable S_n  ∪  F_n)

`F_n` is produced by one prediction function that mirrors the slicer (§7):

    F_n = ((paint ∩ zero-threshold overhang) ⊕ tip diameter) − L_{n−1}

- `paint` covers manual enforcers and the planned placements of all causes (§6).
- Everything the slicer will drop is removed from `F_n` (§5.3).

**Free edges** are the parts of `∂C'` that face open air, i.e. lie on the outline of `L_n`.
Boundary against ungrounded or unprintable material is neither an anchor nor a free edge:
`t` and `c` run through such material.

**Hanging and chains.**
- **What an anchor point hangs from.** Take an anchor point `a` at layer `n`.
  - Find the **topmost** earlier layer `m` with `z_n − z_m − h_n < H_s` that has a front
    within horizontal reach `Σ_{k=m+1..n} w_k + δ` of `a`.
  - In that layer, take from each front in reach the point `q` nearest to `a`.
  - `a` hangs from these points, one per front.
  - `δ` only decides whether a front is in reach. It never shifts `q` deeper into the front
    or into lower layers, so nothing is lost per layer.
- A front hangs from whatever its anchor points hang from.
- Fronts linked by this relation form a **chain**. On a continuous slope, each front hangs
  from the front of the layer directly below.

### 2.3 Accumulated cantilever reach `c(p)`

    c_n(p) = min over anchors a of ( dist(p, a) + ĉ(a) )            (β = α)
    c_n(p) = min over anchors a of ( max(0, dist(p, a) − (w_β − w_n)) + ĉ(a) )   (β < α)

**Distance** is measured inside the material of the layer (geodesic), not straight across
voids.

**Inherited reach.**

    ĉ(a) = min over the points q that a hangs from of  c*_m(q) · max(0, 1 − (z_n − z_m − h_n) / H_s)

where `c*_m(q) = 0` if `q` was covered by tension or hoop (§2.4) or rests on support, and
`c*_m(q) = c_m(q)` otherwise.
- The factor is 1 for the layer directly below, so a continuous slope keeps its full reach.
- It decays only over plain material stacked in between, e.g. stair risers.
- `ĉ(a) = 0` if `a` hangs from nothing, if `a` is a support anchor, or if `H_s = 0`.

So only material that hangs by bending passes on its reach. Where chains merge, the better
anchored one wins.

**Excess (H1).** The anchor lies on `∂S_n`, at distance `w_n` outside `L_{n−1}`. Each layer
therefore adds only the part of its step that goes *beyond* the self-supporting step.
- `c` is the **accumulated excess overhang**.
- Per unit of horizontal extent, a slope θ adds `1 − tanθ / tanβ`. β ≤ α is the
  accumulation angle (default β = α).
- If β < α, each layer adds `max(0, step − w_β)` with `w_β = h / tanβ`. Slopes between β and
  α add nothing.

| Slope θ (β = α = 45°) | 0° (shelf) | 15° | 30° | 40° | 44° | ≥ 45° |
|---|---|---|---|---|---|---|
| rate | 1.00 | 0.73 | 0.42 | 0.16 | 0.03 | 0 |

- **Independent of layer height.** The excess per layer is `h·(1/tanθ − 1/tanβ)`, so the
  excess per millimetre of extent does not depend on `h`.
- **Never stricter than classic support at α** for the `c` rule. Classic support would
  support every one of these slopes completely.
- **β < α** lets users tolerate long slopes just below α without changing α.

**Stiffening height `H_s`.**
- Plain material stacked on a fresh overhang makes it stiff. Its inherited reach fades
  linearly over `H_s` of that material.
- Stairs therefore chain while the risers are low, and stop chaining once a riser exceeds
  `h + H_s`, without a hard flip in between.
- `H_s = 0` gives per-layer reach. That is OrcaSlicer's cantilever test ("critical regions
  only", 3 mm), which never flags long shallow slopes.

### 2.4 Tension: bridge span `t(p)` and hoop credit

**Wide fronts** (wider than about 2 extrusion widths) are printed as bridge infill in one
direction per region (`detect_bridging_direction`, `BridgeDetector.hpp:88-140`, called from
`LayerRegion.cpp:316-320`).

How the slicer picks that direction:
- It takes the direction **parallel** to the free edges: the one that minimises
  `Σ|edge · normal|` over the free edges, i.e. the fewest line ends in the air.
- A shelf of depth `D` and length `L > 2D` therefore gets lines **along the wall**; a shorter
  shelf gets lines **outward**.
- A front with no free edge (fully surrounded) gets lines along its short principal axis
  (`BridgeDetector.hpp:88-96`).

How `θ(C)` is predicted here:
- Once per front, from all adjacent `L_{n−1}` material, whether grounded or not. The slicer
  never treats support as an anchor, so placements cannot change `θ`.
- If several directions cost within ~10 % of each other, the prediction cannot tell which one
  the slicer picks: its regions differ slightly, and exact ties follow `std::unordered_map`
  order. All tied directions are then evaluated and their needs united. Examples: a 10 × 20
  shelf, nearly round fronts.
- The slicer's real regions come from its own surface expansions, so this is an
  approximation.

The span itself:
- Follow the line through `p` in direction `θ(C)`. It runs through ungrounded or
  unprintable material and stops only at grounded printable material, at `F_n`, or at a free
  edge.
- `t(p)` is the distance between its two stops. It is finite only if **both** stops are
  anchors; otherwise `t(p) = ∞`.

**Thin fronts** are printed as perimeters along their free edge.
- A thin front gets tension only if its free edge between two anchors is **straight**: it
  deviates from its chord by no more than one extrusion width. Then `t` = chord length.
  Example: the band of a sloped ceiling between two side walls.
- A curved or closed free edge gives `t = ∞`.

**Hoop credit (H2).** A front, or part of a front, is a **hoop band** if:
- it lies between a hole contour `H` of its ExPolygon at layer `n` and an outer boundary that
  lies on `S_n` (material below, grounded or not) along at least ~90 % of its length; and
- all of `H` belongs to the front, i.e. no part of `H` lies on `S_n`. Support cannot change
  this.

The grounded-anchor condition of §2.5 still applies.

The band is covered if `Feret(H) ≤ B_ceil`, where `Feret(H)` is the longest chord of `H`.
- Nested slices inside the hole are ignored.
- The rule uses only the current layer, so it is causal, and support cannot change it.
- What does **not** qualify:
  - a front that merely contains holes (a flange with bolt holes);
  - a ring with a free outer edge (a halo);
  - an elongated hole (a hip roof over a long slot, whose Feret diameter exceeds `B_ceil`).

**Converging fronts without a hoop band** have no tension until their last layer.
- Examples: a horizontal through-hole, whose slice splits in two; a gable roof over an open
  slot; a dome whose door reaches into the shallow top.
- They are chains under the `c` rule. Only the final closing gap is a bridge.

### 2.5 Coverage, causes, need levels

**Coverage.** A point `p ∈ C'` is **covered** (prints without support) iff

    C' has a grounded anchor   and   ( t(p) ≤ B_max   or   p in a covered hoop band   or   c(p) ≤ K_max )

**Levels.** For an uncovered point, each available mechanism gives a level, by ratio to its
limit:

| Mechanism | may | should | must |
|---|---|---|---|
| tension, `t/B_max` (only if `t` is finite) | ≤ `f_hard` | ≤ `f_hard²` | above |
| bending, `c/K_max` | — | ≤ `f_hard` | above |

- The point's **level is the lower one**, because the material is held by the better
  mechanism.
- Its **cause** is the mechanism that gives that level: *long span* for tension, *cantilever*
  for bending. On a tie it is long span, since a row across the span (a cut, §3.2) usually
  resolves it more cheaply.
- Needs that a placement leaves behind or creates keep that placement's cause (§3.3).
- A point of a front without any grounded anchor is an *island* need, level must.
- Both ratios can only fall when anchors are added, so the level never rises (P4).

**Filter.** The needs of a layer get a morphological opening by `ε`, and components smaller
than `A_min` are dropped.

**Not every point is covered or a need.** Tolerated islands and filtered slivers are
*exempt*. Tail points are needs regardless of coverage (§3.1).

`f_hard` defaults to 1.5.

## 3. The classes

### 3.1 Islands (local minima) — must

**An island need** is any front without a grounded anchor (§2.5). Typical cases per slice:
- a slice with no overlap below (layer > 0);
- a slice with only weak contact (§2.2);
- a slice that touches only ungrounded material.

**Detection.**
- It catches every local minimum, including downward cone tips that have no horizontal facet,
  which an angle test misses.
- Dome ceilings are never islands.
- The test is free in 2.9.6: `Layer::lslices_ex[i].overlaps_below` is empty for
  `layer > 0`. It is available right after `posSlice` (`Layer.hpp:135-161`, `195`;
  `PrintObjectSlice.cpp:556-577`; `Layer.cpp:542-566`).

**Tolerated islands** (decided before any planning, on geometry only):
- **Island tree.** An island is tracked upward via `overlaps_above`. Island trees that merge
  form one tree: their volumes add, and the lowest start counts.
- **End of the tree.** The tree ends when it merges with material that is connected to the bed
  through `overlaps_below`. Support is ignored for this.
- **Tolerance.** If the tree ends within `H_merge` with a volume below `V_min`, it is
  tolerated. Examples: scan noise, small downward bumps.
- **Effect.** A tolerated tree and the fronts hanging from it up to the merge are exempt:
  they are neither needs nor anchors.
- **Why over height.** The volume is accumulated over height, not per layer. A cone tip
  starts at ~0 mm², so a single-layer area filter would wrongly drop it.

**Unplaced islands.** Material hanging from an island that cannot be placed becomes **one
report entry** for that island, not a cascade of new island needs.

**Tail (stabilisation zone).**
- A thin island stays a need until its inscribed width reaches `W_stiff` or it merges, for at
  most `H_tail` of height.
- This **overrides** coverage: tail points are needs even when `c` is small. Inside the zone,
  `w = 0`.
- Only downward facets can be painted:
  - a tail with downward facets, such as cone flanks, gets tips along its underside;
  - a vertical needle or fin has none, so it is **reported**.
- OrcaSlicer's "sharp tails" are the same idea, measured by bounding box (< 6 mm, up to 16 mm
  high). Those numbers are not directly comparable.

**Minimal support** is the footprint plus the tail. There is no freedom outside footprint
and tail (§5.2).

### 3.2 Tension regions: bridges and hoop bands — mostly an exemption

**Members:**
- flat bridges;
- straight bands of ceilings between side walls;
- annular ceilings between two concentric walls, where lines in direction `θ` end on both
  walls;
- hoop bands: dome ceilings, funnels, counterbore steps.

This answers concept question 3 (curved and ring bridges).

**Main value.** Tension regions *remove* support that angle-based tools add. That is the
original complaint: Automatic painting paints dome tops.

**Long spans** (`t > B_max`) need a **cut, not an area**.
- **Placement:** a row that **crosses** `θ(C)`, at a position where both sub-spans are
  ≤ `B_max`.
- **Freedom:** the interval `[L − B_max, B_max]` along a span of length `L`. More rows if
  `L > 2·B_max`.
- **Tips:** their density inside the painted row is the slicer's choice (§7).

**Hoop bands get no placement inside the void.**
- The void holds no material until the last layers, so support there would only appear above
  the rings it should hold.
- An uncovered hoop band is judged by `c` like any front, and supported under its band.

**Slicer facts:**
- Support is not a bridge anchor, so the slicer keeps `θ` and the row must cross it.
- With contact distance > 0 the surface stays a bridge (`PrintObject.cpp:1061-1063`). With
  contact distance 0 there are no bridges at all (P7).
- Enforced support is not removed by `dont_support_bridges`: that filter runs at
  `TreeSupport.cpp:277-281`, before the enforcers are added at `284-312`.

### 3.3 Cantilevers / overhang fronts — should / must

**Examples:** shelves, T-arms, mushroom caps, chins, outstretched arms, the underside of a
sphere above its tip, horizontal through-holes and open-slot gables before they close,
inclined beams, and hoop bands with `Feret(H) > B_ceil`.

**Measure.** `c(p)` (§2.3) against `K_max`. Above `f_hard · K_max` bending gives must;
tension may still lower the level (§2.5).

**Placement principle.**
- A row helps through `t` only if it **crosses** `θ(C)`.
- A row parallel to `θ(C)` helps only through `c`, covering `K_max` on each side.
- Each placement iteration aims to resolve its need **completely**, e.g. all rows of a deep
  shelf at once. Needs that a placement leaves or
  creates keep the placement's cause.

**Shelf of depth `D`, lines outward** (length `< 2D`):
- One row parallel to the wall near the free edge, at `d ∈ [D − K_max, min(D, B_max)]`.
- The part from the wall to the row becomes a bridge; the rest is a short lip.
- If `D > B_max + K_max`, add further rows at most `B_max` apart, in the same placement
  iteration.

**Shelf of depth `D`, lines along the wall** (length `> 2D`). Use the cheaper of:
- rows **across** the wall direction, at most `B_max` apart, the outermost at most `K_max`
  from the shelf's ends;
- rows parallel to the wall, at most `2·K_max` apart.

**Slope.**
- Support anchors pass on `ĉ = 0`, so support resets `c`.
- A slope therefore needs rows about every `K_max` of excess, or simply the whole zone beyond
  `K_max`.

**Composes through anchors.** Supporting a cantilever turns parts of it into bridges. The
placement rules keep those bridges ≤ `B_max`.

**Slicer influence.** These raise the practical `K_max`, and its default should follow them:
- `extra_perimeters_on_overhangs`:
  - grows concentric loops from the anchor band, with no reach limit;
  - active only with `overhangs` on and perimeters > 0;
  - skipped when the area counts as bridgeable;
  - `PerimeterGenerator.cpp:790-986`, condition `864-866`, `1158-1159`.
- Overhang speed and fan.
- Nozzle size: `K_max` and `B_max` scale with extrusion width.

### 3.4 Stability — different nature, separate and later

**Examples:** a heavy part hanging on a few thin tips, weak necks, separation from the bed,
tall slender parts.

- **Detection** needs mass and centroid tracking over `lslices_ex`. Comparing two layers does
  not show it.
- **Existing model.** `SupportSpotsGenerator` has one (`WeakObjectPart`, `SeparationFromBed`,
  `UnstableFloatingPart`).
  - It runs on extrusions, i.e. after `posInfill`, on every slice (`Print.cpp:908-923`).
  - Its layer driver is file-local (`SupportSpotsGenerator.cpp:1096-1127`).
- **What paint can do.** Enforcers act only on downward facets. Stability problems can
  therefore only be eased with more tips under downward faces above the weak part. Otherwise
  they must be **reported**, with brim or orientation as the remedy.
- **Plan.** A separate detector, reporting first.

## 4. Where support is not needed

The exemption list follows from §2. It is what makes the result "only where really needed".

**No support:**
- slopes at least as steep as α, which produce no unsupported area;
- shelves, lips, chamfers and slopes whose accumulated excess stays ≤ `K_max`;
- slopes just below α, which accumulate very slowly (44° wall: c reaches 3 mm only after
  ≈ 84 mm of height);
- bridges along the slicer's direction with `t ≤ B_max`;
- hoop bands (domes, funnels, counterbore steps) with `Feret(H) ≤ B_ceil` (H2);
- tolerated islands, and needs below the noise filter.

**Not placeable, reported instead of painted:**
- under a user blocker: "suppressed by user". Islands among them are warnings.
- inside a **closed cavity**: support could never be removed. A closed cavity is a 3D void
  that is not connected to the outside, or only through openings smaller than `d_min`.
- not reachable from the plate: *should* and *may* needs always; *must* needs only in
  strict mode (§5.3).

## 5. Plate reachability and placement

### 5.1 Reachable set `R_n`

`R_n` is the set of tip positions at layer `n` from which an organic branch can reach the
plate.

**Query.** It is the complement of the slicer's own query:

    getAvoidance(getRadius(0), tip_layer, Fast, to_model=false, min_xy_dist)

(`TreeModelVolumes.hpp:138`, used at `TreeSupport.cpp:392-405`, `1323-1326`).

**How the avoidance is built.**
- It is built bottom-up: `avoidance(n) = collision(n) ∪ shrink(avoidance(n−1), max_move)`
  (`TreeModelVolumes.cpp:618-715`).
- `max_move = tan(support_tree_angle) · layer_height`, using the configured layer height
  (`TreeSupportCommon.cpp:94-97`).
- Organic support refuses variable layer height (`Print.cpp:547-555`), so this is exact.
- The collision includes the machine border.

**Indexing.**
- Tip layer = `n − (z_distance_top_layers + 1)` (`TreeSupport.cpp:1248-1249`, `1306`).
- Layers are counted in support-layer space: raft layers are added.

**Margin.**
- Propagation re-checks branches at their growing radius (`TreeSupport.cpp:1612-1613`).
- A **margin** of about one branch radius is therefore only a tolerance for lost or crowded
  branches.

**Not modelled** (reported as risks):
- crowding between branches;
- other objects on the plate. `TreeModelVolumes` uses only the object's own slices, so a
  "reachable" tip can still hit another object.

**Instances.** Instances with different rotations become separate PrintObjects. Reachability
is computed per PrintObject. One painted volume serving several PrintObjects gets a warning.

### 5.2 Freedom zones and placement

Each need has a **freedom zone** `Z`: the support positions that resolve it.

| Cause | Freedom zone `Z` |
|---|---|
| island | footprint (+ tail) |
| long span | the cut interval across `θ(C)` |
| cantilever | the rows of §3.3 (chosen by `θ(C)`), or rows / the zone along a slope or ring band |

**Allowed positions** = `Z ∩ R_n ∩ not blocked`. A placement is chosen inside them.

**Clearance.** Plate placements keep a distance of `tip diameter + extra_outset` from the
edge of `R_n` and from blocker projections. The clearance includes the margin of §5.1.
- `extra_outset` is how far the slicer pulls tips outward, ≈ tip radius − line width/2
  (`TreeSupport.cpp:1264-1270`).
- The distance is needed because the slicer grows enforced areas by one tip diameter without
  clipping (`TreeSupport.cpp:289-299`).

**Room by cause.** Long spans and cantilevers usually have room for a plate position;
islands have the least.

**Termination.** Every placement iteration must reduce the need area by at least `A_min`.
Otherwise the need is marked *unplaceable*.

### 5.3 Plate policy, slicer setting, silent drops

**Policy.**
- **Plate-preferred** (default): a placement may stand on the model only as a fallback, and
  only for a *must* need that has no plate placement.
- **Strict** (option): never on the model.

**`support_material_buildplate_only` is fixed by the policy before planning.** The plan then
accounts for it.
- If it depended on the plan instead, the plan and the setting would feed back into each
  other without a fixed point, for example through unreachable manual enforcers.

**Strict → `buildplate_only` on.**
- Nothing can leak onto the model.
- With organic support the slicer drops every tip that cannot reach the bed, enforced tips
  included, and tells nobody (`TreeSupport.cpp:392-405`, `1323-1336`; errors only go to
  `printf`, `TreeSupportCommon.cpp:178-201`).
- The tool's report is the only place where the user sees what will be missing.

**Plate-preferred → `buildplate_only` off.**
- On-model placements and manual enforcers on the model are built.
- **Risks:**
  - **Prediction miss.** A tip predicted as reachable that the slicer finds unreachable
    lands on the model instead of being dropped. The clearance in §5.2 makes this rare.
  - **Merging.** A plate branch that merges with an on-model branch ends on the model
    (`to_buildplate = a && b`, `TreeSupport.cpp:2057`, merge check `2113-2129`). Plate
    placements are kept apart from on-model ones, or reported as "may merge onto the model".
  - **Short on-model branches.** Branches on the model shorter than 1 mm are deleted
    silently unless they carry a roof (`support_tree_min_height_to_model`, a hard-coded
    constant at `TreeSupportCommon.hpp:205`; `TreeSupport.cpp:2708-2716`). On-model
    placements are checked against this height and against the placeable areas.

**Tip status order:** `TO_BP_SAFE → TO_BP → TO_MODEL_GRACIOUS_SAFE → TO_MODEL_GRACIOUS →
TO_MODEL → dropped`. The three `TO_MODEL_*` states apply only when support may rest on the model
(`TreeSupport.cpp:390-401`).

**What happens to needs without a plate placement:**

| Need | Plate-preferred (default) | Strict |
|---|---|---|
| island (must) | on the model + warning | warning |
| cantilever / long span at *must* level | on the model + warning | warning |
| cantilever / long span at *should* / *may* | report | report |
| inside a closed cavity | report, never painted | report, never painted |

**Classic support differs.** Classic (non-tree) support lets enforced support rest on the
model even with build-plate-only (`SupportMaterial.cpp:2039`, `2072-2092`). The settings
logic above is therefore specific to tree and organic support.

## 6. Composition — each detector alone, all together without hindering

**Pipeline:**

    base model + tolerated islands (geometry only)
    → one joint plan for ALL causes, independent of the toggles
      (deterministic: layers bottom-up; within a layer islands → cantilevers → long spans;
       place, re-evaluate)
    → each planned placement tagged with its cause
    → paint only the placements of causes the user enabled
    → verification against the actual paint → report

**Rules:**

- **P1 — shared base.** All detectors use the same `U_n`, groundedness, anchors, α and
  limits, so their results cannot contradict each other.
- **P2 — partition.** Every need has exactly one cause and one level. Toggles filter the
  **output**, never the coverage rule. Otherwise switching "long spans" off would turn every
  bridge into a cantilever need.
- **P3 — independence.**
  - The joint plan is computed once, for all causes, whatever the toggles say. The toggles
    only select which tagged placements become paint.
  - Enabling, disabling, clearing or re-running one cause therefore never changes another
    cause's paint. Any combination paints exactly the union of the single runs.
  - Because the plan covers all causes, each cause effectively relies on the *planned*
    support of the others, not on their paint.
  - **"Used alone" means consistent, not sufficient.** The report lists the needs of
    disabled causes. For each painted placement it also lists the planned placements of
    disabled causes that it relies on.
  - Example: with only cantilevers enabled, an elevated sphere gets the band beyond `K_max`, and its
    island tip is reported. The cantilever detector does not quietly cover the tip.
  - Example: a shelf with a drip lip gets nothing from the cantilever detector, because the
    plan supports the lip island. The report says so.
  - Rejected alternative: detectors see only the real paint. Results would then depend on
    the toggles and the run order, and clearing one cause would silently invalidate others.
  - Planned placements that the slicer will drop are anchors for no one (§5.3).
- **P4 — monotonicity.** Adding anchors never adds needs and never raises a level, so the
  plan loop converges:
  - `θ(C)` is fixed from material, so support only splits lines and `t` only falls.
  - Hoop credit does not depend on anchors.
  - `c` is a minimum over anchors, and support passes on `ĉ = 0`.
  - The level is the minimum of two ratios that can only fall (§2.5).
  - Grounding only grows: tolerance is decided on geometry before planning, and planned
    island support only grounds more material.
  - Tail needs are fixed and are resolved by their own placement.
  - The `A_min` progress rule bounds the number of iterations.
- **P5 — user paint wins.**
  - Never paint over user blockers. The slicer itself lets enforcers override blockers in
    classic and tree/organic support (`SupportMaterial.cpp:1355-1376`,
    `TreeSupport.cpp:272-310`), so the tool has to enforce this.
  - Manual enforcers are kept. They are anchors where the slicer will really build them.
    In strict mode, unreachable manual enforcers are reported as "will be dropped".
  - Manual paint is never erased. Today's Automatic painting erases all paint, enforcers and
    blockers alike (`GLGizmoFdmSupports.cpp:546-564`; still true in 3.0, §12).
- **P6 — provenance.** Auto paint is stored per cause, separately from the `TriangleSelector`
  states, which carry no source tag (`TriangleSelector.hpp:36-60`; 3.0 offers 255 states,
  §12).
  - **Storage.** It is kept with the volume, so it survives undo/redo and 3MF.
  - **Manual erase.** Erasing auto paint by hand is stored as a suppression mask, so a re-run
    does not paint it back.
  - **Existing button.** The existing Automatic painting must clear the store, or be blocked
    while the store is not empty.
  - **Mesh edits** invalidate the store.
  - **Fingerprint.** A fingerprint marks stale paint. It covers α, layer heights, contact
    distance, tree angle, tip diameter, z/xy distance, build-plate-only, raft, bed position,
    the manual paint and the suppression mask.
  - **Painting rule.** Tool paint ignores the "highlight overhang by angle" filter and uses
    the downward-facing rule of `enforce_spot`.
- **P7 — slicer contract.**
  - `support_material` on, `support_material_auto` off (3.0: mode "Manual Only").
  - Organic style.
  - `support_material_enforce_layers` = 0.
  - `support_material_contact_distance` > 0. With 0, every bottom surface is non-bridge
    (`PrintObject.cpp:1061-1063`), and the bridge rule would not match the print.
  - `buildplate_only` as fixed by the policy.
  - Enforcer modifier volumes and a raft count as manual support.
  - Inside an enforcer the slicer uses a zero-angle overhang test, grown by
    `support_tree_tip_diameter` (`TreeSupport.cpp:284-312`). This is the reason for the
    clearance in §5.2.
- **P8 — report.** Unsatisfied needs are listed per cause and level, with layer and position.
  Possible states:
  - unplaceable;
  - suppressed by user;
  - cause disabled;
  - relies on a disabled cause;
  - will be dropped;
  - may merge onto the model;
  - on the model (fallback);
  - below the lowest painted level;
  - one painted volume serves several PrintObjects;
  - inside a closed cavity;
  - tail without downward facets;
  - crowding or other-object risk.

  The slicer itself reports none of this.

**User view.** One checkbox per cause (islands, cantilevers, long spans; stability later),
one "Paint" action, and "Clear" per cause. Any combination gives the same per-cause paint
(P3).

## 7. Output contract (paint)

**Facets.**
- Paint only downward-facing facets. The slicer ignores enforcer paint on all other facets
  (`TriangleMeshSlicer.cpp:914`, orientation `2272-2284`).
- The painted facets are projected onto the layer slabs without offset
  (`PrintObject.cpp:3429-3457`).

**Layers.**
- A spot on a horizontal facet affects exactly one layer (`TriangleMeshSlicer.cpp:574-647`).
- A sloped facet gives one strip of width ≈ `h / tanθ` per layer.

**How to paint.**
- Paint **prisms**: an XY polygon × a z interval aligned to the `slice_z` slabs.
- `TriangleSelector` paints only triangles that lie fully inside the cursor, so the painted
  area comes out slightly smaller than planned. Prisms must therefore be at least 2 × the
  TriangleSelector edge-length limit wide, and at least one layer strip `h/tanθ`. They may
  span several layers.

**Verification.**
- After painting, `F_n` is re-derived from the **actual** paint through the same projection
  (`project_and_append_custom_facets`).
- "Actual paint" means manual paint plus the paint of enabled causes; disabled causes stay
  planned (P3).
- Remaining needs smaller than `A_min` are ignored. The rest are reported, not repainted in a
  loop.

**Spill-over.** The enforced area grows by one tip diameter and is not clipped to the paint,
so support reaches that far beyond it. The clearance in §5.2 accounts for this.

**Tips.**
- The slicer samples tips along lines `support_tree_branch_distance` apart, resampled about
  every `connect_length` (`TreeSupport.cpp:1181-1201`, `1258`).
- A painted strip must be wide enough to get tips. Very small areas get their outline sampled
  instead.

**What to paint per cause:**
- island: footprint (+ tail);
- long span: row strips;
- cantilever: rows, or the zone beyond `K_max`.

## 8. Verdict — are concept 2.1–2.3 all?

**2.1 Islands — critical.** Add:
- geometry-only tolerance, accumulated over height;
- weak contact, judged relative to the slice;
- groundedness;
- the tail override.

**2.2 Bridges — keep, but change two things:**
- Follow the slicer's single direction, including what that means for shelves and for ties.
- Add hoop credit for compact ring bands: domes, funnels, counterbores (H2).

Their main value is the exemption. Long-span support is rare (H3), but long-span **planning**
is needed anyway: the joint plan contains it.

**2.3 Cantilevers — critical.** Reach must be the **accumulated excess along chains** (H1).
- Otherwise long shallow slopes are missed (OrcaSlicer's trade-off), or slopes near α are
  over-flagged.
- If H3 holds, most real support demand of organic / figurine prints lies here, including
  horizontal through-holes and open gables.

**Missing and critical:**
- **causality** (§2.2): closures cannot hold earlier layers;
- **plate reachability and placement**: the stated goal, and organic support drops tips
  silently;
- **the composition model**: joint plan, toggles on the output, monotonicity, provenance.

**Real but separate:** stability (§3.4).

**Smallest usable capability:**
- base model;
- islands;
- cantilevers;
- bridge and hoop exemption;
- long-span planning (report only);
- reachability, policy and report.

Long-span painting and stability are follow-ups.

## 9. Parameters (defaults are placeholders until test prints)

| Parameter | Meaning | Starting point |
|---|---|---|
| α | overhang threshold, read from the print config | `support_material_threshold` |
| β | accumulation angle (≤ α) | = α |
| `K_max` | longest accumulated excess reach | 3 mm (OrcaSlicer, per layer); more with extra perimeters on overhangs; scales with extrusion width |
| `H_s` | stiffening height; 0 = per layer (OrcaSlicer-like) | 1 mm |
| `B_max` | longest bridge span | 40 mm; scales with extrusion width |
| `B_ceil` | longest chord of a hoop hole (H2) | 40 mm |
| `f_hard` | step between need levels | 1.5 |
| `κ`, `a_min` | weak contact: relative overlap, overlap extent | 10 %, 2 extrusion widths |
| `ε`, `A_min` | noise filter on needs; minimal progress per placement iteration | 0.01 mm; ~0.1 mm² |
| `δ` | tolerance for hanging (§2.2) | 0.1 × extrusion width |
| `V_min`, `H_merge` | tolerated islands | to calibrate |
| `W_stiff`, `H_tail` | island tail zone (inscribed width) | 3 mm / 16 mm |
| `d_min` | opening below which a cavity counts as closed | a few branch diameters |
| margin | reachability tolerance | about one branch radius |
| plate policy | plate-preferred / strict | plate-preferred |
| lowest painted level | may / should / must; needs below are only reported | to decide |

## 10. Reference cases

**Assumptions:** α = β = 45°, h = 0.2 mm (`w` = 0.2 mm), `K_max` = 3 mm, `H_s` = 1 mm,
`B_max` = `B_ceil` = 40 mm, `f_hard` = 1.5, `W_stiff` = 3 mm.

**Derived numbers:**
- Rates (§2.3): 30° → 0.42, 40° → 0.16, 44° → 0.034, 15° → 0.73.
- One side of a circular ceiling of radius R accumulates up to `0.41·R` before its top.

**How to read the table.** Every row follows from §2–§5. The column **Hyp.** names the
hypothesis the row depends on. The table becomes the test catalogue.

| Case | Expected | Reason | Hyp. |
|---|---|---|---|
| Hemisphere dome ceiling, R 20 | nothing | hoop bands, `Feret(H)` ≤ 28.3 mm | H2 |
| Dome R 20, door reaching the apex | cantilever near the top, must | no hoop band, `c` up to 8.3 mm | H1 |
| Dome R 20, door ending below the 45° level | rings above the door: nothing | there they are hoop bands again, and `ĉ` resets | H2 |
| Dome R 40 | nothing | rings with opening > 40 mm reach only `c` ≈ 1.9 mm | H1, H2 |
| Dome R 60 | upper rings without hoop credit (opening > 40 mm), must | `c` reaches 3 mm partway, 8.3 mm at opening 40 | H1, H2 |
| Inward funnel narrowing to a Ø 5 hole that stays open | nothing | hoop bands | H2 |
| Counterbore Ø 10 → Ø 5.5, head pocket facing down | nothing | hoop band, `Feret(H)` = 5.5 mm; also `c` ≈ 2.05 mm | — |
| Flat flange OD 70 on a tube OD 30, with 6 × Ø 6 bolt holes | rim ring plus a ring / zone across the bolt circle, must | not a hoop band (free outer edge); `c` ≈ 19.8; hole edges are free, so lines that meet a hole give `t = ∞`; after the rim ring `c` still reaches ≈ 8.9 around the bolt circle | — |
| Same flange without bolt holes | rim ring (must); remaining spans across the annulus at may level (cause stays cantilever, §2.5) | round free edges tie in every direction (all united); after the rim ring `t` ≤ ≈ 59 (`t/B_max` ≤ 1.48) | — |
| Flat halo ring Ø 30 / Ø 24 on one thin post | rows or zone, must | free outer edge, no hoop; reach around the ring from the post | — |
| Hip-roof ceiling 30 × 200 mm at 30° over a slot open below | cantilever along the sides, must | `Feret(H)` ≈ 200 > 40, no hoop; `c` up to ≈ 6.3 | H1 |
| Horizontal through-hole Ø 8 | nothing | chains, `c` ≤ 1.7; the closing gap is a short bridge | H1 |
| Horizontal through-hole Ø 20 | cantilever near the top, should | `c` up to 4.1 | H1 |
| Horizontal through-hole Ø 50 | cantilever near the top, must; inside the hole usually unreachable → on the model + warning (strict: warning) | `c` up to 10.4 | H1 |
| Flat bridge 30 mm | nothing | `t` = 30 | — |
| Flat bridge 90 mm | two rows across `θ`, should | `t/B_max` = 2.25 = `f_hard²` | — |
| Bridge 60 mm between towers, model part below the middle | one row in the reachable part of [20, 40] mm, may | freedom zone ∩ `R_n` | — |
| Bridge 30 mm with a tolerated 0.6 mm logo below it | nothing | lines pass through ungrounded material | — |
| Wall at 40° standing on a covered 30 mm bridge | need above ≈ 15.6 mm of height, as on any 40° wall | bridge-covered material passes on `ĉ` = 0 | H1 |
| Shelf 2 mm | nothing | `c` = 1.8 | — |
| Shelf 10 mm deep, 15 mm long | row 7–10 mm from the wall, must | lines outward; the row turns the rest into bridge + lip | — |
| Shelf 10 mm deep, 30 mm long | rows across the wall direction (≤ 40 mm apart, outermost ≤ 3 mm from the ends), or parallel rows ≤ 6 mm apart; must | lines along the wall (30 > 2·10); `c` = 9.8 | — |
| Shelf 10 × 20 mm (tie) | union of both orientations' needs | the costs tie | — |
| Shelf 10 × 30 mm, thin post under the middle of its edge | as for 10 × 30; the post helps only within `K_max` of itself | its line ends free at the shelf end | — |
| Shelf 30 mm deep, 20 mm long, 1 mm drip lip along its edge | island support under the lip only | lines outward; planned lip grounds the line end → 30 mm bridge | — |
| Same, lip tolerated | rows near the edge, must | lip ungrounded → lines end free → `c` ≈ 29.8 | — |
| Shelf 4.2 × 100 mm with two pendant knobs under its edge corners | knobs as islands; shelf edge at most should | `c`/`K_max` = 1.33; the knobs add anchors, and the level cannot rise (P4) | — |
| T-shape, arms 15 mm long and 10 mm wide | rows 12–15 mm from the stem, must | lines outward (10 < 2·15), `c` = 14.8 | — |
| Mushroom cap, 30° underside | zone beyond ≈ 7.1 mm from the stem; `H_s` = 0: nothing | accumulated excess | H1 |
| Wall at 40°, 50 mm tall | need above ≈ 15.6 mm of height (classic support: whole wall); lower β if it prints fine | slow accumulation | H1 |
| Wall at 44°, 50 mm tall | nothing, at any layer height | reaches 3 mm only after ≈ 84 mm of height | H1 |
| Thin rod Ø 1 mm, 30° from horizontal, 20 mm long | no island; cantilever beyond ≈ 7.1 mm of horizontal extent | real overlap below; excess accumulates | H1 |
| Lean-to ceiling at 30° in a closed box 30 mm wide | nothing | straight thin bands, side wall to side wall, `t` = 30 | — |
| Same in a box 50 mm wide | long span inside a closed cavity → report only | `t` = 50 > 40; trapped | — |
| Gable roof over an open slot | cantilever where `c` > 3 | no side walls, no hoop band | H1 |
| Inclined 15° beam between towers 30 mm apart | zone beyond ≈ 4.1 mm from the lower tower, must | chain | H1 |
| Stairs, treads 2.5 mm; risers 0.4 mm / 1.2 mm | flagged from the 2nd step (2.3 + 0.8·2.3 = 4.1) / nothing | `ĉ` fades over `H_s` | H1 |
| Cone tip pointing down, 60° flanks | tip, plus the flanks up to ≈ 2.6 mm (tail) | island; tail until 3 mm width; above it self-supporting | — |
| Vertical needle hanging down | tip; tail reported | no downward facets along the tail | — |
| Sphere R 20 on the bed | band from ≈ 5.8 mm radius to the 45° slope (≈ 14.1 mm); must in its outer part (`c` up to 6.4) | first layer rests on the bed; chain | H1 |
| Sphere R 20, elevated | as above, plus the island footprint (small first-layer disk) | island, then chain | H1 |
| Small downward bump (R 1.5 hemisphere) on a steep wall | nothing | tolerated tree, its fronts exempt up to the merge | — |
| Outstretched figurine arm, 30° underside | zone beyond ≈ 7.1 mm, branches to the plate | cantilever, reachable | H1 |
| Chin over chest | should: report; must: on the model + warning (strict: warning) | cantilever, unreachable | H1 |
| Closed cavity with a 60 mm flat ceiling | report only | trapped support | — |
| Ball floating inside a cup | on the model + warning if the gap is ≥ 1 mm, else reported as dropped (strict: warning); the hemisphere above is one report entry if unplaced | island, unreachable | — |

## 11. Open questions

1. **Test prints:** H1, H2 and the defaults (`K_max`, `B_ceil`, `H_s`, `B_max`).
2. **Accumulation:**
   - β = α (the strict reading of α) or β < α (tolerate long slopes just below α);
   - the default for `H_s`;
   - in short: quality through accumulation, or minimal support through OrcaSlicer-like
     per-layer reach.
3. **Scope of the limits:** per filament `B_max` / `K_max`, or global.
4. **Stability:** in this feature or a later one.
5. **Where it lives:** gizmo only, or also as a print-setting support mode.
6. **Disabled causes:** assume them handled (P3, recommended) or treat them as absent.

## 12. Upstream status — PrusaSlicer 3.0 alphas (checked up to `version_3.0.0-alpha12`)

The 3.0 tree is restructured. libslic3r lives in `src/libslic3r/src/libslic3r/`; the gizmo is
`src/slic3r-shared/src/Slic3r/App/Plater/PaintOnSupportsGizmo.cpp`. Nothing in it overlaps
with this theory.

**Automatic painting.** Ported to `PaintOnSupportsGizmo` without a change in substance
(`5e2ef10a0d`, SPE-3807):
- It still says "will erase all currently painted areas" and replaces the paint.
  - Difference: the old paint is replaced only when the result arrives. A cancelled run
    therefore keeps the user's paint.
- Causes are discarded: `GeneratedSupportPoints` holds only position and radius.
- Painting is unchanged: a ray up from z = 0 and a sphere on downward facets, with the same
  squared-distance comparison.
- There are no parameters and no critical-only or smart mode.

**`SupportSpotsGenerator`.** Unchanged: 16 mm bridge distance, curvature penalty, causes,
stability model. Only its acceleration input changed (`5c25799d0f`).

**Enforcers and blockers in tree/organic support.** Unchanged:
- zero threshold;
- tip-diameter offset (still marked "FIXME … fudge constant");
- blockers do not affect enforced areas;
- unreachable tips are dropped with only a log warning; there is no user warning.

**Config.** `support_material` and `support_material_auto` are merged into one
`SupportMode` enum: Off / **Manual Only** (`EnforcersOnly`) / On (`dd5c1fb7cb`).
- "Manual Only" is the **default** (`ConfigDefsFDM.cpp:3906-3912`), so painted enforcers
  produce support without further switches.
- New keys: `support_material_first_layer_density` and
  `support_material_first_layer_expansion`.
- No options for critical regions, sharp tails, bridge length or overhang reach.

**Painting infrastructure.**
- `TriangleSelector` can hold 255 states (`1d72759173`) and remap them (`150603df7c`).
- Facet annotations in 3MF are versioned (`6fe1ff0831`).

This makes provenance (P6) easier after a move to 3.0: auto paint per cause could be a state
of its own.

**Algorithm fixes worth knowing:**
- `7221a5cadb`: organic support handles low overhangs better (`recommendedMinRadius`). This
  affects reachability margins close to the bed.
- `ee15861d7e`: narrow overlaps are no longer bottom bridges. This affects the bridge
  prediction.
- `3588389352`: missing layers under floating objects.

**Bridge direction and extra perimeters on overhangs.** Refactored only;
`dont_support_bridges` still defaults to true.

**Consequence.** The theory holds unchanged for 3.0. When the fork moves to 3.0:
- the contract (P7) maps to "Manual Only";
- provenance can use a TriangleSelector state of its own;
- the reference cases should be re-checked after `ee15861d7e`.

## Appendix — code facts used (2.9.6)

| Fact | Where |
|---|---|
| Island test via `overlaps_below`, built in `PrintObject::slice()` before `posSlice` is done; overlap on slices shrunk by 1 µm; layer 0 stored uncompensated | `Layer.hpp:135-161`, `195`; `PrintObjectSlice.cpp:556-577`, `1022-1031`; `Layer.cpp:105-107`, `542-566` |
| Enforcer paint: downward facets only, no offset; a horizontal spot hits one layer | `PrintObject.cpp:3429-3457`; `TriangleMeshSlicer.cpp:574-647`, `914`, `2272-2284` |
| Tree/organic enforcers: zero threshold, grown by tip diameter (not clipped); blockers and `dont_support_bridges` act only before | `TreeSupport.cpp:272-281`, `284-312` |
| Classic enforcers: blockers not applied; rest on the model even with build-plate-only | `SupportMaterial.cpp:1355-1376`, `2039`, `2072-2092` |
| Organic + build-plate-only: unreachable tips dropped silently | `TreeSupport.cpp:392-405`, `1323-1336`; `TreeSupportCommon.cpp:106`, `178-201` |
| On-model branches below 1 mm deleted (hard-coded); merged branches lose `to_buildplate` | `TreeSupportCommon.hpp:205`; `TreeSupport.cpp:2708-2716`, `2057`, `2113-2129` |
| Tip status order | `TreeSupport.cpp:390-401` |
| Avoidance to plate, max move per layer, tip layer, extra outset | `TreeModelVolumes.hpp:138`; `TreeModelVolumes.cpp:618-715`; `TreeSupportCommon.cpp:94-97`, `113`; `TreeSupport.cpp:1248-1249`, `1264-1270`, `1306` |
| Organic support refuses variable layer height | `Print.cpp:547-555` |
| Slicer's automatic overhang threshold (`+1°`, 0 = auto, lower layer height) | `SupportMaterial.cpp:1171-1234`; `TreeSupport.cpp:234-270` |
| Bridge direction: `detect_bridging_direction` (parallel to the free edges, `unordered_map` tie order); support never an anchor; `BridgeDetector` class unused | `BridgeDetector.hpp:88-140`; `LayerRegion.cpp:316-320`, `375-413`, `513-516` |
| Bridge flow kept when contact distance > 0; none at 0 | `PrintObject.cpp:1061-1063` |
| Extra perimeters on overhangs: no reach limit; conditions | `PerimeterGenerator.cpp:790-986`, `864-866`, `1158-1159` |
| Automatic painting erases all paint; no source tag in paint | `GLGizmoFdmSupports.cpp:546-564`; `TriangleSelector.hpp:36-60` |
| `SupportSpotsGenerator`: curvature limit; runs on extrusions, every slice | `SupportSpotsGenerator.cpp:368-371`, `432-435`, `1096-1127`; `Print.cpp:908-923` |
| Tip sampling along lines, `connect_length` | `TreeSupport.cpp:1181-1201`, `1258` |
| OrcaSlicer: cantilever > 3 mm per layer; sharp tails < 6 mm bounding box, ≤ 16 mm; critical-only drops all other overhangs | OrcaSlicer `src/libslic3r/Support/TreeSupport.cpp` (`detect_overhangs`) |
