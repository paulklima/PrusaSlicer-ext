# Research report B — libslic3r algorithm base (2.9.6)

theory.md line refs mostly verified; three corrections marked [CORRECTION].

## 1. Layer data after posSlice

- `LayerSlice` (Layer.hpp:135-161): `bbox`, `overlaps_above`, `overlaps_below` (`Link{int32_t slice_idx; float area}`), `islands`. `Layer::id()` (168-170) includes raft layers → index `po.layers()` with `id() − raft_layers`. `upper_layer`/`lower_layer` (174-175); `slice_z`, `print_z`, `height`, `bottom_z()` (177-180); `lslices` (193), `lslices_ex` (195); `zs_from_layers()` (310) = slice_z values.
- posSlice (`PrintObject::slice()`, PrintObjectSlice.cpp:528-579): `lslices_ex` bbox in parallel (553-566); `Layer::build_up_down_graph(n−1,n)` (567-575); `set_done(posSlice)` (578). Overlaps on slices shrunk by 1 µm (Layer.cpp:105-107, graph 542-566) → touching-only contacts not linked; layer 0 `overlaps_below` empty. `Link::area` in scaled² (Layer.cpp:298-300). `lslices` = union of region slices (Layer::make_slices, Layer.cpp:66-83); nothing reassigns it after posSlice.
- Layer 0: with `raft_layers==0`, layer-0 `lslices` replaced by the uncompensated slice (PrintObjectSlice.cpp:962-1031), includes negative `xy_size_compensation` (996); region slices stay compensated.
- Available after posSlice: `lslices`, `lslices_ex`, `LayerRegion::slices()` (untyped), `LayerRegion::flow()`. Not yet: perimeters, surface types (`detect_surfaces_type` PrintObject.cpp:306, posPrepareInfill), bridge angles (`process_external_surfaces` 344), support layers.
- Iterate `for (const Layer *l : po.layers())` (Print.hpp:299) / `get_layer(i)` (332). `po.slicing_parameters()` (Print.hpp:361; Slicing.hpp:36-120): `first_object_layer_height`, `layer_height`, `object_print_z_min`, `raft_layers()`.
- Coordinates: `new_layers()` (PrintObjectSlice.cpp:48-70): `print_z = hi + object_print_z_min`, `slice_z = ½(lo+hi)`, lo/hi object-frame z → object-frame slab of layer n = `[print_z − height − z_min, print_z − z_min]`. `PrintObject::trafo()` = instance matrix (with shrinkage compensation) with XY translation zeroed (PrintApply.cpp:171-187), Z translation + rotation kept. `center_offset` = XY bbox centre of the rotated object (PrintObject.cpp:110-135); `trafo_centered()` subtracts it (Print.hpp:303-304). `PrintInstance::shift` = instance offset + center_offset (PrintObject.cpp:141-144); bed XY = layer point + shift.
- Volume → layer frame: `(trafo_centered() * mv->get_matrix()) * v_mesh` (unscaled mm) — same chain as slicing (PrintObjectSlice.cpp:85) and enforcer projection (PrintObject.cpp:3443-3445). Layer → mesh: inverse of that matrix on `(unscale(p), z_obj)` (gizmo does this at GLGizmoFdmSupports.cpp:424-433; its `obj_transform` adds the first layer's `bottom_z`, PrintObject.cpp:506-509, because SupportSpots positions are print_z based — not needed for us).
- One ModelObject can give several PrintObjects differing by Z rotation; find ours by `po->model_object()->id()` (GLGizmoFdmSupports.cpp:418-419).

## 2. Geometry toolbox

- ClipperUtils.hpp: `offset`/`offset_ex` (356-372), `expand`/`expand_ex`/`shrink`/`shrink_ex` (389-402), `offset2_ex` (408), `closing`/`closing_ex` (412-420), `opening`/`opening_ex` (425-436), `diff`/`diff_ex` (442-465), `intersection`/`intersection_ex` (480-501), `union_`/`union_ex`/`union_safety_offset_ex` (378-386, 523-536), `diff_pl` (466-472), `intersection_pl` (502-509), `*_ln` (511-516). `ClipperSafetyOffset`=10 (47), `DefaultJoinType`=jtMiter (49).
- ExPolygon.hpp: `contains(Point)` (50), `on_boundary` (52), `area`, `lines`, `medial_axis(min,max,ThickPolylines*)` (98-101); Geometry/MedialAxis.hpp:20.
- `AABBTreeLines::LinesDistancer<Line>` (AABBTreeLines.hpp:333-398): `distance_from_lines_extra<signed>`, `outside()`, `all_lines_in_radius`, `intersections_with_line<sorted>`.
- `EdgeGrid::Grid` (EdgeGrid.hpp:110-191): `create(ExPolygons,res)`, `calculate_sdf`, `signed_distance`, `closest_point_signed_distance`.
- ClipperZUtils.hpp: tag result edges with source polygon index (anchor/free-edge provenance).
- Algorithm/RegionExpansion.hpp:20-121: `propagate_waves[_ex](src, boundary, expansion, step, max_steps)`, `expand_expolygons` — geodesic-limited growth, returns only the final area → write an own loop on that pattern for iso-bands. `MarchingSquares.hpp` (`marchsq::execute`) raster → polygons.
- Sampling: `sample_expolygon` (SLA/SupportIslands/UniformSupportIsland.cpp:1324) is in an anonymous namespace (not reusable); public `sla::uniform_support_island/peninsula` (UniformSupportIsland.hpp:18-28) Voronoi-based, heavy. Simplest: scanlines + `intersection_pl`.
- Shape: `compute_principal_components(Polygons)` (PrincipalComponents2D.hpp:27); `MinAreaBoundigBox` [sic] (MinAreaBoundingBox.hpp:23); `Geometry::convex_hull` (Geometry/ConvexHull.hpp:23-28). **No Feret helper** → convex hull + rotating calipers / O(h²). Inscribed width: `shrink_ex(ex, W/2).empty()` ⇒ width < W; MedialAxis/EdgeGrid for per-point widths.

## 3. Bridge direction

`BridgeDetector.hpp:88-141`, two inline pure functions, callable standalone:
```cpp
std::tuple<Vec2d,double> detect_bridging_direction(const Lines &floating_edges, const Polygons &overhang_area);
std::tuple<Vec2d,double> detect_bridging_direction(const Polygons &to_cover, const Polygons &anchors_area); // overhang=diff(to_cover,anchors); floating=diff_pl(overhang, expand(anchors,SCALED_EPSILON))
```
- No floating edges → PCA minor axis `pc2` (91-97). Otherwise candidates = normals of floating edges, quantised `ceil(atan2*1000)`, `std::unordered_map` (101-106); cost(n)=Σ|edge·n|; min with strict `<`; returns the perpendicular (lines parallel to dominant free edges) + min cost (110-128). Only the winner returned → copy the ~25-line cost loop to return all candidates within ±10 %.
- Active slicer path: `LayerRegion.cpp:320` in `merge_bridges` via `expand_bridges_detect_orientations` (375-413) from `process_external_surfaces` (513-516); expansion zones 495-505. Dead paths: LayerRegion.cpp:756-773 (`#else` of `#if 1`); PerimeterGenerator.cpp:846 (extra perimeters on overhangs). `BridgeDetector` class only used by tests/fff_print/test_bridges.cpp. `bridge_angle = π + atan2(dir)`; config `bridge_angle` > 0 overrides (LayerRegion.cpp:515, PrintConfig.hpp:741).
- **[CORRECTION] theory §2.4:** the slicer's bridge region is `opening_ex(L_n − L_{n−1}, ext_width/10)` with zero angle (PrintObject.cpp:1104-1110), per LayerRegion, inside the perimeters (fill surface). Anchors are only the **wave expansions into the region's fill surfaces** (internal solid, sparse, top), ≈ `shell_width·√2` (LayerRegion.cpp:470-505). Bridge edges touching **perimeters** count as floating → walls too thin for infill do not anchor the direction.

## 4. Organic reachability

- `TreeModelVolumes(const PrintObject&, const BuildVolume&, coord_t max_move, coord_t max_move_slow, size_t current_mesh_idx, ..., const std::vector<Polygons>& additional_excluded_areas = {})` (TreeModelVolumes.hpp:55, .cpp:65-165): reads `lslices` of all layers simplified to `gcode_resolution`; support-layer indices with empty raft layers in front (117-126); slices blocker volumes (106); builds `TreeSupportMeshGroupSettings(po)` which asserts support on + tree/organic style (TreeSupportCommon.cpp:30-32, debug only) and needs print config. Production: `TreeModelVolumes{po, BuildVolume(±300 mm dummy), config.maximum_move_distance, config.maximum_move_distance_slow, idx, {}}` (TreeSupport.cpp:3469-3473, 3655-3657). Constructible standalone after posSlice; query `getAvoidance(config.getRadius(0), tip_layer, AvoidanceType::Fast, false, min_xy_dist)` (.hpp:138).
- **[CORRECTION] theory §5.1:** `calculateMachineBorderCollision()` returns `{}` ("FIXME Machine border is currently ignored", .cpp:49-63). `processing_last_mesh = outline_idx == layer_outline_indices.size()` (.cpp:480) is never true → `m_anti_overhang` (blocker volumes) never added to collision (528-537): painted blockers are not considered at all.
- Collision (.cpp:425-585): radius r → `offset(union(L_j), r + xy)` over `z_distance_bottom` layers below; layers above tapered xy (521-526). Avoidance (618-715) serial bottom-up: `avoid_n = union(collision_n, offset(avoid_{n−1}, −max_move, jtRound))`, sub-steps `1.9·max(r, min_xy)` (670-694), then `polygons_simplify`. Radii snapped by `ceilRadius` (834+; 0.5 mm then exponential). Layer 0 returns collision (340-341).
- Cost: lazy `getAvoidance(Fast)` → `calculateAvoidance({r,L}, to_bp=true, to_model=false)` runs Slow, FastSafe, Fast chains in parallel (636-652) + collisions, layers 0..L: ~3–4× one chain. Caches mutex-guarded; `getAvoidance` const via const_cast; duplicate computation possible → warm once then query in parallel.
- TreeSupportCommon.cpp: `maximum_move_distance = tan(support_tree_angle)·layer_height` (96, scaled; TreeModelVolumes stores max_move−2, .cpp:74). `min_radius = support_tree_tip_diameter/2` (95), `getRadius(0) = min_radius` (TreeSupportCommon.hpp:427-434). `support_xy_distance = support_material_xy_spacing.get_abs_value(max ext-perimeter width)` (58); `xy_min_distance = min(xy, min(xy, 0.5·epw))` (108); `min_xy_dist = xy_distance > xy_min_distance` (TreeSupport.cpp:1252; false with default 50 %). `z_distance_top_layers = round(gap_support_object / layer_height)` (113); `gap_support_object` = `support_material_contact_distance` (Slicing.cpp:124-128, 0 if soluble). Tip layer = overhang layer − (z_distance_top_layers+1) (TreeSupport.cpp:1249, 1306); overhang index = object layer + `raft_layers.size()` (314); raft heights from `TreeSupportSettings::raft_layers` (.cpp:147-175). `support_rests_on_model = !support_material_buildplate_only` (106). `extra_outset = max(0, min_radius − line_w/2) (+ line_w/2 if min_xy_dist)` (TreeSupport.cpp:1270). Enforcers grown by `support_tree_tip_diameter` without clipping (239, 284-312); `dont_support_bridges` applied at 277. Defaults: angle 40°, slow 25°, tip 0.8, branch 2, xy 50 %, contact 0.2, threshold 0 (= auto), buildplate_only false.
- Own simplified avoidance is easy (~60 LOC): mirror collision 485-527 (no blockers/border) and Fast avoidance 683-698 at one radius `r = ceil(tip_r + xy delta)`; ~3× cheaper; cross-check in a unit test against `TreeModelVolumes::getAvoidance(..., Fast, false, ...)`.

## 5. Config access

- `po.config()` = PrintObjectConfig (PrintConfig.hpp:648+): `layer_height` (662), `raft_layers` (669), `support_material_*` (685-713: auto, buildplate_only, contact_distance, bottom_contact_distance, enforce_layers, style, threshold, xy_spacing), `support_tree_*` (715-722), `dont_support_bridges` (654), `elefant_foot_compensation` (655).
- `po.print()->config()` = PrintConfig (958+): `first_layer_height` (998), `nozzle_diameter`, `gcode_resolution`. PrintRegionConfig (736+): `external_perimeter_extrusion_width` (750), `extra_perimeters_on_overhangs` (759), `bridge_angle` (741).
- Extrusion width: `po.printing_region(i).flow(po, frExternalPerimeter, layer_h[, first_layer]).scaled_width()` (PrintRegion.cpp:36-65, Print.hpp:364-365); after posSlice `layer->get_region(k)->flow(frExternalPerimeter)` (LayerRegion.cpp:44). α=0 rule = TreeSupport.cpp:259-266 (0.5 × average ext-perimeter width of the lower layer's regions). Max over regions as tree support: TreeSupportCommon.cpp:36-40. `support_material_flow(po,h)` Flow.hpp:148.

## 6. Mesh side and painting

- `its_face_normals` (TriangleMesh.hpp:362), `its_face_normal` (365-368), `its_face_neighbors` (249). `AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set` (AABBTreeIndirect.hpp:676) + `traverse(tree, intersecting(AlignedBox3f), cb)` (~911-990). `AABBMesh` (AABBMesh.hpp:36-140). Slicer "down" facet = projected 2D orientation (TriangleMeshSlicer.cpp:2272-2284), only Down/Degenerate feed bottom slabs (914) → use `normal.z < 0` in the PrintObject frame.
- Cursor (TriangleSelector.hpp:102-267): protected ctor `(source, radius_world, trafo, clip)`; virtuals `is_mesh_point_inside`, `is_pointer_in_triangle`, `vertices_inside` (115), `is_any_edge_inside_cursor`, `is_facet_visible`, `get_facets_to_select`. `select_patch` (.cpp:243-277) BFS from `get_facets_to_select()`, `highlight_by_angle_deg` filters by `vec_down·n` (enforce_spot uses 89.99, TriangleSelectorWrapper.cpp:27), neighbours enqueued only if `is_facet_visible`. `select_triangle_recursive` (1066-1115): 3 vertices inside ⇒ whole triangle; else split until edges ≤ `m_edge_limit` (`split_triangle` 1147-1201); still-partial leaves are **not** selected (1189-1192). `m_edge_limit` default `radius/5` (2187).
- Prism cursor fits: copy `HeightRange` (.hpp:249-267, .cpp:2410-2469); `use_world_coordinates = true` (tests in PrintObject frame); explicit `m_edge_limit` ≈ 0.2–0.3 mm; `get_facets_to_select` via AABB tree; `is_mesh_point_inside` = z in slab && XY in polygon; `is_facet_visible` = world normal.z < 0; `is_pointer_in_triangle` false; override `vertices_inside`. A single "layered-region" cursor (ExPolygons per slab, looked up by z) paints all prisms in one `select_patch`. Sequence: `TriangleSelector sel(mv->mesh()); sel.deserialize(mv->supported_facets.get_data()); select_patch(seed, cursor, state, trafo_no_translate, true, 89.99f); serialize()` (or `mv->supported_facets.set(sel)`, Model.hpp:695). `TriangleSelectorWrapper` (TriangleSelectorWrapper.hpp:21-33) — an `enforce_prism()` would fit there.
- Verification: `po.project_and_append_custom_facets(false, ENFORCER, out)` (PrintObject.cpp:3429-3457) or `slice_mesh_slabs` (TriangleMeshSlicer.cpp:2215).

## 7. Reusable existing code

- SupportSpotsGenerator: extrusion-based, after posInfill; only `Integrals`/`compute_second_moment`/`PartialObject` (.hpp:147-231) relevant, for stability later.
- SLA (SLA/SupportPointGenerator.cpp, namespace sla, in libslic3r): `prepare_generator_data` (963-1082) links parts (lslices_ex already does); `create_peninsulas` (567-682) closest analogue of U_n with free/anchor edges (`below_self_supported = offset(below, w)`, `diff_ex`, per-edge `is_outline` by parallel-line matching); `get_small_parts`/`create_small_part` (698-898) bounding-sphere island trees; structs `LayerPart`, `Peninsula`, `PartLinks` (.hpp:50-120) as templates.

## 8. Unit tests

- Catch2 3.8 (tests/CMakeLists.txt:4); `#include <catch2/catch_test_macros.hpp>`, `catch_approx.hpp`; `TEST_CASE`, `SCENARIO/GIVEN/WHEN/THEN`, `REQUIRE`, `CHECK`, `Approx`; `CATCH_CONFIG_FAST_COMPILE`.
- Register: `tests/fff_print/CMakeLists.txt` `add_executable(${_TEST_NAME}_tests …)` list (links `test_common`, `slic3r-arrange-wrapper`; needed for test_data). Pure geometry: `tests/libslic3r/CMakeLists.txt` (links libslic3r). New sources: `src/libslic3r/CMakeLists.txt` Support block (355-372).
- Meshes: `its_make_cube/prism/cylinder/cone/frustum/pyramid/sphere(r, fa)` + `make_*` (TriangleMesh.hpp:370-388); `TriangleMesh::translate/rotate_x/scale/merge` (149-169); `its_translate`, `its_transform`, `its_rotate_x/y/z` (bundled_deps/admesh/admesh/stl.h:260-300); `its_merge` (TriangleMesh.hpp:356-360); `MeshBoolean::cgal::plus/minus/intersect` (MeshBoolean.hpp:59-65).
- Print: `Test::init_print(std::vector<TriangleMesh>&&, Print&, Model&, const DynamicPrintConfig&)` (tests/fff_print/test_data.cpp:241-274) — one ModelObject per mesh, arranged, centred at (100,100), apply + validate (merge meshes for one object). `init_and_process_print` (308-320) full `process()`. Stop after slicing: `Print::TaskParams tp; tp.to_object_step = posSlice; print.set_task(tp); print.process(); print.finalize();` (PrintBase.hpp:444-453, 629-716; Print.hpp:650-654) — no existing test does this; plain `process()` also works.
- Example (tests/fff_print/test_printobject.cpp:14-31):
```cpp
Slic3r::Print print;
Slic3r::Test::init_and_process_print({TestMesh::cube_20x20x20}, print, {{"first_layer_height",2},{"layer_height",2},{"nozzle_diameter",3}});
SpanOfConstPtrs<Layer> layers = print.objects().front()->layers();
REQUIRE(layers.size() == 10);
```
  Set `support_material=1`, `support_material_auto=0`, `support_material_style=organic` for valid tree settings.

## 9. Threading, cancel, progress

- `tbb::parallel_for(tbb::blocked_range<size_t>(0,n), [&](auto &r){ for(i…){ throw_on_cancel(); … } })` (PrintObjectSlice.cpp:553-575; TreeSupport.cpp:243-316). SLA: `execution::for_each(ex_tbb, from, to, fn, grain)` (Execution/Execution.hpp:50), `if (i % 32 == 0) throw_on_cancel()`. `tbb::task_group` (TreeModelVolumes.cpp:251-256).
- Cancel: `PrintBase::throw_if_canceled()`/`make_try_cancel()` are protected (PrintBase.hpp:553-555, PrintTryCancel 385-395) → take `std::function<void()> throw_on_cancel` param like `fff_tree_support_generate` (TreeSupport.cpp:3647). Progress: `Print::set_status(percent,msg)` public (PrintBase.hpp:501); SLA uses `std::function<void(int)>`.

## Researcher's recommended module design

Files: `src/libslic3r/Support/SmartSupports.{hpp,cpp}` (API, Params, orchestration, report), `SmartSupportsBase` (§2.1-2.2), `SmartSupportsReach` (c, θ, t, hoop), `SmartSupportsIslands` (§3.1), `SmartSupportsReachability` (R_n), `SmartSupportsPlan` (placements); prism cursor + painting in TriangleSelector / `TriangleSelectorWrapper`. Tests: `tests/fff_print/test_smart_supports.cpp` (§10 cases on sliced meshes), `tests/libslic3r/test_smart_supports_geometry.cpp` (waves, Feret, θ ties, avoidance).

Core types (namespace `Slic3r::SmartSupports`):
```cpp
enum class Cause:uint8_t{Island,Cantilever,LongSpan}; enum class Level:uint8_t{May,Should,Must};
struct Params{ double alpha,beta,K_max,H_s,B_max,B_ceil,f_hard,kappa,a_min,eps,A_min,delta,V_min,H_merge,W_stiff,H_tail,margin,wave_step; bool strict; static Params from(const PrintObject&); };
struct LayerModel{ double z_lo,z_hi,h; coord_t w; ExPolygons L,printable,S,U,grounded,F; };
struct Front{ uint32_t layer; ExPolygon area; BoundingBox bbox; Polylines anchors, free_edges; bool grounded; std::vector<Vec2d> thetas; std::vector<ExPolygons> c_bands; ExPolygons covered; };
struct ReachCarry{ std::vector<std::pair<ExPolygons,float>> bands; }; // decayed c* carried upward
struct Need{Cause c; Level l; uint32_t layer; ExPolygons area;};  struct Placement{Cause c; Level l; uint32_t layer; ExPolygons footprint; bool on_model;};
struct Result{ std::vector<Placement> placements; std::vector<Need> unresolved; };
Result analyze(const PrintObject&, const Params&, std::function<void()> throw_on_cancel);
```

Concept → helpers:
- U_n/S_n: `S = intersection_ex(L_n, offset(L_{n−1}, w_n))`, `U = diff_ex(L_n, S)`; components = fronts.
- Printable: `opening_ex(L, min_w/2)`.
- Groundedness/islands: bottom-up; `overlaps_below`; weak contact = `Link::area / area() < κ` then extent of `intersection_ex` < a_min; grounded = `intersection(L_n, offset(grounded_{n−1}, w_n)) ∪ F_n` + grounded fronts.
- Anchors/free edges: `intersection_pl`/`diff_pl(to_polylines(front), expand(grounded∩printable∪F, ε))`; free edges = rest on ∂L_n (pattern BridgeDetector.hpp:136-141).
- c(p): own clipped-wave loop (RegionExpansion style, step ≈0.2 mm, capped at f_hard·K_max+step); multi-source: anchors binned by ĉ start at wave ⌊ĉ/s⌋; keep iso-bands per front.
- ĉ inheritance: carry a decayed band field (`ReachCarry`) up layer by layer instead of searching the topmost layer within H_s: reset to 0 on covered / support-resting / plain-stacked material, scaled by `1 − h/H_s` per plain layer; look up anchors with `offset(δ)` tolerance.
- θ(C): own copy of the `detect_bridging_direction` cost loop returning all candidates within 10 %.
- t(p): scanlines along θ every ~1 extrusion width, `intersection_pl` with `C' ∪ ungrounded/unprintable`; endpoints classified via `LinesDistancer` against anchor/free-edge lines; strips with t > B_max offset into polygons.
- Hoop: `ExPolygon::holes` fully inside the front; outer boundary on S_n ≥ 90 % length (`intersection_pl` lengths); Feret by convex hull + calipers.
- Island trees: union-find over (layer, slice) via `overlaps_above/below`; bed connectivity bottom-up; volume Σ area·h; tail via `shrink_ex(W_stiff/2).empty()`.
- R_n: own Fast avoidance (task_group parallel to base model); clearance = `shrink(complement, tip_d + extra_outset + margin)`.
- Painting: one layered prism cursor; prisms inflated by the edge limit.

Hard parts / v1 simplifications:
1. Bottom-up base pass is serial (causality); parallelise per-layer geometry first (S, U, fronts, θ, hoop candidates, weak contact), then fronts within a layer.
2. Geodesic c with varying ĉ: binned waves O(cap/step) offsets per front; saturate above f_hard·K_max.
3. θ prediction won't match the slicer's regions exactly (see correction); v1 evaluates ties, accepts mismatch.
4. Thin-front straight-edge tension: v1 t = ∞ unless the free edge is a straight chord (≤ one width deviation).
5. Planner v1: islands footprint (+tail) ∩ R; cantilevers zone `{c > K_max}` ∩ R (not rows); long spans report only; one bottom-up place-then-re-evaluate pass per layer (monotone, P4).
6. Closed-cavity detection (3D flood fill) postponed (R_n empty inside cavities anyway).
7. Own avoidance, not TreeModelVolumes (cost, debug asserts, three avoidance types).
8. Paint: edge limit ≈0.2–0.3 mm; smaller explodes triangle count along prism borders.
9. Researcher B suggested running inside the background process (step like posSupportSpotsSearch); researcher A recommends a UI job on a GUI-thread copy (R5) — to be decided by the plan.
