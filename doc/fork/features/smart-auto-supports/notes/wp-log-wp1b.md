# Work package log — WP1b (prism painting)

Scope (plan §8.2): `paint_placements` in `src/libslic3r/Support/SmartSupportsPaint.cpp` (plan §4, D17, theory §7) and
`tests/libslic3r/test_smart_supports_paint.cpp` (`[sas-paint]`, plan §7.1). No header was changed.

## What `paint_placements` does (contract for WP2c1b / WP5)

1. **Causes:** `causes ∩ {cbIsland, cbCantilever}` (long spans are never painted in v1). A placement is painted iff
   `paintable`, its cause bit is in that set, `level ≥ lowest` and `layer < in.layers.size()`.
2. **Prisms per cause:** `P_k` = the areas of those placements at layer k; `R_k = offset_ex(union_ex(∪_{|j−k|≤k_d} P_j), e)`
   (default jtMiter 3), `e = Params::edge_limit` (0.25 mm), `k_d = max(1, ⌈e / h_min⌉)`, `h_min` = smallest layer
   height of `in.layers`. Per non-empty slab: boundary lines, an AABB tree over them, bbox. Parallel over layers
   (indexed slots).
3. **Per volume** (parallel over volumes, one private `TriangleSelector` each): `deserialize(paint)`,
   `remap_states(clear_causes_map(cbAll))` (**a run replaces all smart paint**, also of unchecked causes; manual paint
   is kept), then per cause (island first, then cantilever) one `select_patch` with a `PredicateCursor`
   (`edge_limit = e`, the volume's full trafo, `use_world_coordinates`), `tool_paint_rule(bit)`, requested state
   `auto_state(bit)`, splitting on, 89.99° angle guard, rule cleared afterwards. Overlapping causes give OR-ed masks
   (state 5).
4. **Seeds:** original facets with a normalised world normal `(M⁻ᵀ·n).z < −1e-4` (correct for mirroring and
   non-uniform trafos) whose XY bbox overlaps the bbox of a non-empty `R_k` of a slab they cross. No seeds → no
   `select_patch`. A volume without paint and without seeds is returned as is (no selector built).
5. **Result:** `VolumePaint{volume_id, serialize(), changed = !FacetsAnnotation::same_paint(input, output)}`, in
   volume order. A re-run on its own output returns identical data with `changed = false`.

### The triangle predicate (sound, per slab)

- **Slabs mirror the slicer** (`slice_mesh_slabs()`, called by `project_and_append_custom_facets()`): slab k =
  `[slice_z_{k−1}, slice_z_k)` (slab 0 unbounded below), compared in float like the slicer; a horizontal facet
  exactly on a plane belongs to the slab above; the part above the topmost plane is not projected and is neutral
  (neither blocks nor touches).
- `classify(tri)`: the triangle is clipped to every slab it crosses (convex piece ≤ 5 vertices). A piece is
  *Inside* iff no boundary segment of `R_k` comes within 10 nm of it (exact segment / convex-piece test on the
  AABB candidates) and one of its points is inside (ray casting, boundary = not inside); *Partial* if a segment
  touches; *Outside* otherwise. Result **3** iff every piece is Inside; **1** if some piece is not Outside (the
  selector splits); **0** if none touches. `touches()` returns the cached result of the same triangle.
- Consequences (tested): the projection of the paint onto layer k lies in `R_k`; every point of `P_k` on a downward
  facet of slab k is painted (leaves touching `P_k` have a diameter ≤ e and cross at most `k_d` slab boundaries).
  Leaves crossing the border of `R_k` at the edge limit stay unpainted (painted area = `P_k` … `R_k`).

### Cancel, threading, determinism

- `cancel()` is called per prism layer (from TBB workers), twice per volume plus once per painted cause (from TBB
  workers), and every 4096 triangle classifications inside `select_patch`. It must be thread-safe (plan §2.3). A
  `CanceledException` propagates out of `paint_placements`; the private selectors are discarded. An empty
  `ThrowOnCancel` is accepted.
- No shared mutable state; output slots per volume / layer; identical bytes with 1 thread and default parallelism
  (tested).

## Deviations from plan.md (exact)

1. **Slab definition** (§3.3 "slab `[slice_z − h/2, slice_z + h/2)`, vertex z snapped +1e-4", §4 "slab(z) half-open +
   snapped"): implemented the slicer's projection slab `[slice_z_{k−1}, slice_z_k)` without a snap.
   `slice_facet_with_slabs<false>()` (`TriangleMeshSlicer.cpp:540-760`) projects a downward facet piece between two
   consecutive slicing planes onto the upper one; the plan's slab is shifted by half a layer and would test the upper
   half of every physical layer against the wrong `R_k`. Planning in `O_n = L_n − L_{n−1}` (slices at `slice_z`) is
   consistent with the slicer's slab, so nothing changes for the other WPs. The comparison uses the slicer's own
   float computation (the same float trafo applied to the same mesh-space vertices, against `float(slice_z)`), so no
   snap is needed; a mismatch could only concern facets within float rounding of a slicing plane.
2. **`classify` return value**: 1 instead of "#vertices inside ≤ 2" for a touching, not certainly inside triangle
   (`select_triangle_recursive()` only distinguishes 0 / 3 / other). `point_inside` is implemented as specified but
   not used by the selector while `classify` is set.
3. **Containment test**: an exact segment test against an AABB tree of the boundary of `R_k` instead of
   `diff(piece, clip(R_k, bbox(piece) ⊕ 1 µm)) = ∅` with Clipper (same result, conservative by 10 nm, much
   cheaper); no point memo (pieces are per triangle).
4. **Edge limit** clamped to ≥ 0.01 mm for the selector (0 would split forever); `e` itself is used unclamped.
5. **Size guard**: one `info` log line when the auto leaves of a volume exceed 20 × its seed facets (count over the
   whole volume).

## Notes for dependent work packages

- **WP2c1b (`run()`):**
  - `Result::painted_areas[cause]`: `paint_placements` cannot return it (frozen signature). Count the placements
    that pass the filter above (`paintable`, cause checked and not long span, `level ≥ lowest`), per cause; each of
    them is painted on all downward facets under its area (lower bound).
  - Progress: map 80 % before and 90 % after the call (no progress callback in the signature; painting is fast:
    the `[sas-paint]` cases take ≤ 60 ms each, 160 comb layers on three volumes included).
  - Verification (§3.10): `project_states(paint, filter)` sees exactly what the slicer projects. Expect the paint at
    layer k to be `⊂ R_k ∩ O_k`, i.e. up to `k_d` layers beyond a zone on slopes (D17's worst-case flank, by design),
    and slightly more than the zone itself (≤ e beyond its border).
  - Re-runs: pass the previous `VolumePaint::data` back in `VolumeInput::paint`; with unchanged placements
    `changed` is false for every volume (skip `commit_paint`).
- **WP3 / GUI:** the data are plain `serialize()` output (`used_states` set), auto states 3..9 only on downward
  facets; manual leaves are bit-identical to the input.
- **WP5:** the predicate's cost is `O(pieces × nearby boundary segments)`; measure the paint phase on `frog_legs` /
  `figurine_proxy` (§3.13 budget 1–3 s). Mutation check done in WP1b (below); keep the tests discriminating when
  changing the predicate.

## Tests (`[SmartSupports][sas-paint]`, libslic3r_tests, synthetic inputs)

| Test case | Plan §7.1 item / what fails it |
|---|---|
| overhang strips on a wedge (30° / 45° × h 0.2 / 0.1) | upper bound `proj_k ⊂ R_k ⊕ 1 µm` on every layer; lower bound `P_k ∩ O_k ⊂ proj_k ⊕ 2 µm`; nothing beyond `k_d` layers / `y ± e`; `cover(predict_F(proj_k)) ⊇ predict_F(P_k) ⊖ e` (Reachability, interface 0) |
| sound: holes and empty slabs | ring prism with a 4 mm hole over a 20 × 20 single-facet bottom (all vertices inside): hole stays free; 60° facet from the bed to z 8.66 with prisms only at both ends: no paint in the slabs between |
| mirrored volume | wedge with `scale(−1, 1, 1)` + offset: bounds hold, paint at x ≥ 10, same area per layer as the unmirrored world mesh (± 2 · e · strip) |
| re-runs and manual paint | manual enforcer / blocker spots on the slope unchanged (± 1e-6 mm²); 5 re-runs byte-identical; own result → `changed = false`; no placements → exactly the manual-only data |
| rules | only downward facets (top face / walls in a prism → nothing; bottom → 100 mm²); filters (below lowest, not paintable, long span, unchecked cause → nothing); OR-ed causes (state 5 in 20 … 25 mm²); a run with islands only removes all cantilever paint; empty runs; manual paint under a prism kept |
| volumes, determinism, cancel | three parts (painted / painted / untouched → `changed` false, empty data); 1 thread = default (bytes); cancel at the 1st, middle and last call → `CanceledException`; the comb causes more cancel calls than full strips (predicate checks); no layers / no placements |

**Mutation check** (temporary `SAS_MUT` switches, not committed; each mutant must fail `[sas-paint]`): vertex-only
predicate (3 vertices inside → paint) — fails (hole, empty slabs); no downward filter **and** no angle guard — fails
(rules); no clearing of earlier smart paint — fails; paint nothing — fails; normal from world vertices (ignores the
mirror) — fails; `k_d = 0` — fails (lower bound); no paint rule — fails (manual paint). The downward filter alone is
redundant with `select_patch`'s 89.99° guard (that mutant passes, by design both are kept).

## Results (sandbox, dev/2.9.6 merged in by `wp-build.sh`)

- `libslic3r_tests "[sas-paint]"`: 6 test cases / 2483 assertions passed.
- `libslic3r_tests "[SmartSupports]"`: 59 test cases / 28314 assertions passed; `fff_print_tests "[SmartSupports]"`:
  27 test cases / 6220 assertions passed.
- No compiler warnings in the WP1b files.
