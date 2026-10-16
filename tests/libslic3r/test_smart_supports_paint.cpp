// Smart support painting: prism painting of the placements (plan §4, §7.1; WP1b).
//
// The projection checks use project_states() (WP1a), which projects the paint exactly as the slicer does
// (PrintObject::project_and_append_custom_facets()). R_k is recomputed here from the specification (plan §4, D17):
// R_k = (∪_{|j−k| ≤ k_d} P_j) ⊕ e, e = Params::edge_limit, k_d = max(1, ⌈e / h_min⌉).

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <atomic>
#include <cmath>
#include <functional>
#include <memory>

#include <tbb/global_control.h>

#include "smart_supports_meshes.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PrintBase.hpp"
#include "libslic3r/Support/SmartSupportsInternal.hpp"

using namespace Slic3r;
using namespace Slic3r::SmartSupports;
using namespace Slic3r::SmartSupports::Testing;
using Catch::Approx;

namespace {

double area_mm2(const ExPolygons &ex) { return area(ex) * SCALING_FACTOR * SCALING_FACTOR; }

float mm(double v) { return float(scaled<double>(v)); }

BoundingBox extents(const std::vector<ExPolygons> &layers)
{
    BoundingBox out;
    for (const ExPolygons &layer : layers)
        if (! layer.empty())
            out.merge(get_extents(layer));
    return out;
}

// L_k − L_{k−1}.
ExPolygons overhang(const AnalysisInput &in, size_t k)
{
    return k == 0 ? in.layers[0].lslices : diff_ex(in.layers[k].lslices, in.layers[k - 1].lslices);
}

// Layer onto which the slicer projects a downward facet at height z: slice_z_{k−1} ≤ z < slice_z_k.
size_t layer_of_z(const AnalysisInput &in, double z)
{
    size_t k = 0;
    while (k < in.layers.size() && float(in.layers[k].slice_z) <= float(z))
        ++ k;
    return k;
}

int k_d(const AnalysisInput &in)
{
    double h_min = 1e9;
    for (const LayerInput &layer : in.layers)
        h_min = std::min(h_min, layer.height);
    return std::max(1, int(std::ceil(in.params.edge_limit / h_min - 1e-9)));
}

Placement placement(uint32_t layer, ExPolygons area, Cause cause = Cause::Cantilever, Level level = Level::Must, bool paintable = true)
{
    static uint32_t next_id = 0;
    Placement p;
    p.id        = next_id ++;
    p.cause     = cause;
    p.level     = level;
    p.layer     = layer;
    p.area      = std::move(area);
    p.paintable = paintable;
    return p;
}

// Placements P_k = O_k ∩ clip on the layers with slice_z in [z0, z1].
std::vector<Placement> overhang_placements(const AnalysisInput &in, double z0, double z1, const ExPolygons &clip,
                                           Cause cause = Cause::Cantilever)
{
    std::vector<Placement> out;
    for (size_t k = 1; k < in.layers.size(); ++ k)
        if (in.layers[k].slice_z >= z0 && in.layers[k].slice_z <= z1) {
            ExPolygons area = intersection_ex(overhang(in, k), clip);
            if (! area.empty())
                out.push_back(placement(uint32_t(k), std::move(area), cause));
        }
    return out;
}

// R_k of the specification for the placements that paint_placements() is expected to paint.
std::vector<ExPolygons> prism_regions(const AnalysisInput &in, const std::vector<Placement> &placements, uint8_t causes, Level lowest)
{
    const int               kd = k_d(in);
    std::vector<ExPolygons> P(in.layers.size()), R(in.layers.size());
    for (const Placement &p : placements)
        if (p.paintable && (cause_bit(p.cause) & causes & (cbIsland | cbCantilever)) && p.level >= lowest)
            append(P[p.layer], p.area);
    for (int k = 0; k < int(in.layers.size()); ++ k) {
        ExPolygons src;
        for (int j = std::max(0, k - kd); j <= std::min(int(in.layers.size()) - 1, k + kd); ++ j)
            append(src, P[j]);
        if (! src.empty())
            R[k] = offset_ex(union_ex(src), mm(in.params.edge_limit));
    }
    return R;
}

// The slicer's projection of the paint (states passing `filter`) of all volumes, united per layer.
std::vector<ExPolygons> project(const AnalysisInput &in, const std::vector<VolumePaint> &paint,
                                const std::function<bool(TriangleStateType)> &filter = [](TriangleStateType s) { return is_auto_state(s); })
{
    std::vector<const TriangleSelector::TriangleSplittingData*> per_volume;
    for (const VolumePaint &vp : paint)
        per_volume.push_back(&vp.data);
    std::vector<ExPolygons> out;
    for (const Polygons &layer : project_states(in, per_volume, filter, [] {}))
        out.push_back(union_ex(layer));
    return out;
}

// Area (mm², PO frame) of the facets of the volume whose state passes `filter`.
double paint_area(const VolumeInput &volume, const TriangleSelector::TriangleSplittingData &data,
                  const std::function<bool(TriangleStateType)> &filter)
{
    TriangleSelector selector(*volume.mesh);
    selector.deserialize(data, false);
    std::vector<TriangleStateType> states;
    for (size_t s = 0; s < TRIANGLE_STATE_TYPE_COUNT; ++ s)
        if (filter(TriangleStateType(s)))
            states.push_back(TriangleStateType(s));
    const indexed_triangle_set its = selector.get_facets_strict(states);
    double out = 0.;
    for (const stl_triangle_vertex_indices &f : its.indices) {
        const Vec3d a = volume.trafo * its.vertices[f(0)].cast<double>();
        const Vec3d b = volume.trafo * its.vertices[f(1)].cast<double>();
        const Vec3d c = volume.trafo * its.vertices[f(2)].cast<double>();
        out += 0.5 * (b - a).cross(c - a).norm();
    }
    return out;
}

double state_area(const VolumeInput &volume, const TriangleSelector::TriangleSplittingData &data, TriangleStateType state)
{
    return paint_area(volume, data, [state](TriangleStateType s) { return s == state; });
}

bool has_auto(const TriangleSelector::TriangleSplittingData &data)
{
    for (uint8_t mask = 1; mask <= cbAll; ++ mask)
        if (TriangleSelector::has_facets(data, auto_state(mask)))
            return true;
    return false;
}

// Upper bound: the projection of the paint lies in R_k (⊕ 1 µm) on every layer.
// Lower bound: every placement P_k (clipped to O_k) is covered by the projection (⊕ 2 µm).
void check_bounds(const AnalysisInput &in, const std::vector<Placement> &placements, const std::vector<ExPolygons> &proj,
                  uint8_t causes = cbIsland | cbCantilever, Level lowest = Level::Should)
{
    REQUIRE(proj.size() == in.layers.size());
    const std::vector<ExPolygons> R = prism_regions(in, placements, causes, lowest);
    for (size_t k = 0; k < in.layers.size(); ++ k) {
        INFO("layer " << k << ", R " << area_mm2(R[k]) << " mm², projection " << area_mm2(proj[k]) << " mm²");
        CHECK(area_mm2(diff_ex(proj[k], offset_ex(R[k], mm(0.001)))) < 1e-4);
    }
    for (const Placement &p : placements) {
        if (! p.paintable || ! (cause_bit(p.cause) & causes & (cbIsland | cbCantilever)) || p.level < lowest)
            continue;
        const ExPolygons need = intersection_ex(p.area, overhang(in, p.layer));
        INFO("placement at layer " << p.layer << ", " << area_mm2(need) << " mm²");
        CHECK(area_mm2(diff_ex(need, offset_ex(proj[p.layer], mm(0.002)))) < 1e-3 + 1e-3 * area_mm2(need));
    }
}

// Original facet containing the point p (in the mesh frame), -1 if none.
int facet_at(const TriangleMesh &mesh, const Vec3d &p)
{
    for (size_t i = 0; i < mesh.its.indices.size(); ++ i) {
        const stl_triangle_vertex_indices &f = mesh.its.indices[i];
        const Vec3d a = mesh.its.vertices[f(0)].cast<double>(), b = mesh.its.vertices[f(1)].cast<double>(), c = mesh.its.vertices[f(2)].cast<double>();
        const Vec3d n = (b - a).cross(c - a);
        if (n.norm() == 0. || std::abs(n.normalized().dot(p - a)) > 1e-3)
            continue;
        const double s0 = n.dot((b - a).cross(p - a)), s1 = n.dot((c - b).cross(p - b)), s2 = n.dot((a - c).cross(p - c));
        if (s0 >= 0. && s1 >= 0. && s2 >= 0.)
            return int(i);
    }
    return -1;
}

// A stroke of the sphere brush of the paint gizmo (manual paint), volume with identity trafo.
void paint_manual(VolumeInput &volume, const Vec3d &center, float radius, TriangleStateType state)
{
    TriangleSelector selector(*volume.mesh);
    selector.deserialize(volume.paint, false);
    const int facet = facet_at(*volume.mesh, center);
    REQUIRE(facet >= 0);
    const Vec3f c = center.cast<float>();
    selector.select_patch(facet,
        TriangleSelector::SinglePointCursor::cursor_factory(c, c - Vec3f(0.f, 0.f, 10.f), radius,
            TriangleSelector::CursorType::SPHERE, volume.trafo, TriangleSelector::ClippingPlane()),
        state, volume.trafo, true);
    volume.paint = selector.serialize();
}

AnalysisInput floating_box_input()
{
    // Box [0, 10]² × [5, 7]: its bottom (z = 5) is projected onto one layer. A tower far away keeps the layers
    // going above the box, so that its top face lies inside the slicing range.
    return synthetic_input({ box({ 0., 0., 5. }, { 10., 10., 7. }), box({ 30., 0., 0. }, { 35., 5., 10. }) }, 0.2, 0.2, test_params());
}

ExPolygons rect(double x0, double y0, double x1, double y1) { return { rectangle_mm(x0, y0, x1, y1) }; }

} // namespace

TEST_CASE("Prism painting of overhang strips on a wedge", "[SmartSupports][sas-paint]")
{
    const double angle = GENERATE(30., 45.);
    const double h     = GENERATE(0.2, 0.1);
    INFO("wedge " << angle << "°, layer height " << h);
    const AnalysisInput in = synthetic_input({ wedge(angle) }, h, h, test_params());
    REQUIRE(in.layers.size() > 40);

    // Strips of the slope at z ∈ [5, 7], y ∈ [2, 8].
    const std::vector<Placement> placements = overhang_placements(in, 5., 7., rect(-1., 2., 30., 8.));
    REQUIRE(placements.size() >= 9);
    const std::vector<VolumePaint> paint = paint_placements(in, placements, cbIsland | cbCantilever, Level::Should, [] {});
    REQUIRE(paint.size() == 1);
    CHECK(paint[0].volume_id == in.volumes[0].volume_id);
    CHECK(paint[0].changed);

    const std::vector<ExPolygons> proj = project(in, paint);
    check_bounds(in, placements, proj);

    // Only the cantilever cause is painted.
    CHECK(paint_area(in.volumes[0], paint[0].data, [](TriangleStateType s) { return s != TriangleStateType::NONE && s != auto_state(cbCantilever); }) == 0.);
    // Not more than k_d layers beyond the placements, nothing beyond y ∈ [2 − e, 8 + e].
    const int kd = k_d(in);
    for (size_t k = 0; k < proj.size(); ++ k)
        if (int(k) < int(placements.front().layer) - kd || int(k) > int(placements.back().layer) + kd)
            CHECK(proj[k].empty());
    const BoundingBox bbox = extents(proj);
    CHECK(unscaled(bbox.min.y()) > 2. - in.params.edge_limit - 1e-3);
    CHECK(unscaled(bbox.max.y()) < 8. + in.params.edge_limit + 1e-3);

    SECTION("the predicted contact of the paint covers the planned contact") {
        Reachability reach(in);
        reach.compute_collisions([] {});
        double planned_total = 0.;
        for (const Placement &p : placements) {
            if (! reach.tips_possible(p.layer))
                continue;
            reach.ensure(reach.tip_layer(p.layer), [] {});
            const ExPolygons planned = reach.predict_F(p.area, p.layer);
            const ExPolygons actual  = reach.cover(reach.predict_F(proj[p.layer], p.layer), p.layer);
            planned_total += area_mm2(planned);
            INFO("layer " << p.layer << ": planned F " << area_mm2(planned) << " mm², covered by the paint " << area_mm2(actual) << " mm²");
            CHECK(area_mm2(diff_ex(shrink_ex(planned, mm(in.params.edge_limit)), actual)) < 0.01);
        }
        CHECK(planned_total > 1.);
    }
}

TEST_CASE("Prism painting is sound: holes and empty slabs stay unpainted", "[SmartSupports][sas-paint]")
{
    SECTION("hole in the prism over one big horizontal facet") {
        const AnalysisInput in = synthetic_input({ box({ 0., 0., 5. }, { 20., 20., 7. }) }, 0.2, 0.2, test_params());
        const size_t k = layer_of_z(in, 5.);
        REQUIRE(k < in.layers.size());
        REQUIRE(! overhang(in, k).empty());
        // Ring around the whole bottom; every vertex of the two bottom triangles is inside R_k.
        ExPolygon ring = rectangle_mm(-1., -1., 21., 21.);
        Polygon   hole = rectangle_mm(8., 8., 12., 12.).contour;
        hole.reverse();
        ring.holes.push_back(hole);
        const std::vector<Placement> placements { placement(uint32_t(k), { ring }) };
        const std::vector<VolumePaint> paint = paint_placements(in, placements, cbCantilever, Level::Must, [] {});
        const std::vector<ExPolygons>  proj  = project(in, paint);
        check_bounds(in, placements, proj, cbCantilever, Level::Must);
        // The hole of R (P's hole shrunk by e) is free, the rest of the bottom is painted.
        CHECK(area_mm2(intersection_ex(proj[k], rect(8.26, 8.26, 11.74, 11.74))) == 0.);
        CHECK(area_mm2(proj[k]) > 400. - 16. - 1e-3);
        CHECK(area_mm2(proj[k]) < 400. - 3.5 * 3.5 + 1e-3);
    }
    SECTION("steep facet through slabs without placements") {
        // Wedge 60° (length 5, height 8.66): the underside is two triangles from z = 0 to z = 8.66. A tower far away
        // keeps the layers going above the wedge, so that the top vertices of the underside lie inside a slab.
        const double len = 5., top = len * std::tan(60. * M_PI / 180.);
        const AnalysisInput in = synthetic_input({ wedge(60., len), box({ 20., 0., 0. }, { 25., 10., top + 2. }) }, 0.2, 0.2, test_params());
        const size_t k_top = layer_of_z(in, top);
        REQUIRE(k_top + 3 < in.layers.size());
        // Placements only around the vertices of the underside: at the bed and at its top edge.
        const std::vector<Placement> placements { placement(0, rect(-1., -1., 1., 11.)),
                                                  placement(uint32_t(k_top), rect(len - 1., -1., len + 1., 11.)) };
        const std::vector<VolumePaint> paint = paint_placements(in, placements, cbCantilever, Level::Should, [] {});
        const std::vector<ExPolygons>  proj  = project(in, paint);
        check_bounds(in, placements, proj, cbCantilever, Level::Should);
        const int kd = k_d(in);
        for (size_t k = size_t(kd) + 1; k + size_t(kd) < k_top; ++ k)
            CHECK(proj[k].empty());
        CHECK(! proj[0].empty());
        CHECK(! proj[k_top].empty());
        // The tower (second volume) has no downward facet inside a prism: it stays unpainted.
        CHECK(! paint[1].changed);
    }
}

TEST_CASE("Prism painting of a mirrored volume", "[SmartSupports][sas-paint]")
{
    // The wedge mirrored in x and moved: its overhang faces −x, x ∈ [10, 30].
    Transform3d mirror = Transform3d::Identity();
    mirror.translate(Vec3d(30., 0., 0.));
    mirror.scale(Vec3d(-1., 1., 1.));
    REQUIRE(mirror.linear().determinant() < 0.);
    const indexed_triangle_set original = wedge(45.);
    indexed_triangle_set       world    = original;
    its_transform(world, mirror);
    its_flip_triangles(world);   // outward orientation of the world mesh (reference volume)

    const AnalysisInput reference = synthetic_input({ world }, 0.2, 0.2, test_params());
    AnalysisInput       in        = reference;
    in.volumes[0].mesh  = std::make_shared<const TriangleMesh>(original);
    in.volumes[0].trafo = mirror;

    const std::vector<Placement> placements = overhang_placements(in, 5., 7., rect(-1., 2., 40., 8.));
    REQUIRE(placements.size() >= 9);
    const std::vector<VolumePaint> paint = paint_placements(in, placements, cbCantilever, Level::Should, [] {});
    REQUIRE(paint[0].changed);
    const std::vector<ExPolygons> proj = project(in, paint);
    check_bounds(in, placements, proj);
    const BoundingBox bbox = extents(proj);
    CHECK(unscaled(bbox.min.x()) > 10. - 1e-3);

    // Same projected paint as the unmirrored copy of the world mesh, up to the edge limit band at both strip ends
    // (2 · e · strip width 0.2 mm; the split patterns of the two meshes differ).
    const std::vector<ExPolygons> proj_ref = project(reference, paint_placements(reference, placements, cbCantilever, Level::Should, [] {}));
    for (const Placement &p : placements) {
        INFO("layer " << p.layer << ": " << area_mm2(proj[p.layer]) << " vs " << area_mm2(proj_ref[p.layer]) << " mm²");
        CHECK(std::abs(area_mm2(proj[p.layer]) - area_mm2(proj_ref[p.layer])) < 2. * in.params.edge_limit * 0.2 + 1e-3);
    }
}

TEST_CASE("Prism painting re-runs reproduce the paint and keep manual paint", "[SmartSupports][sas-paint]")
{
    AnalysisInput in = synthetic_input({ wedge(45.) }, 0.2, 0.2, test_params());
    // Manual enforcer and blocker spots on the slope inside the planned zone.
    paint_manual(in.volumes[0], Vec3d(5.5, 4., 5.5), 0.6f, TriangleStateType::ENFORCER);
    paint_manual(in.volumes[0], Vec3d(6.5, 6., 6.5), 0.6f, TriangleStateType::BLOCKER);
    const TriangleSelector::TriangleSplittingData manual = in.volumes[0].paint;
    const double enf_area = state_area(in.volumes[0], manual, TriangleStateType::ENFORCER);
    const double blk_area = state_area(in.volumes[0], manual, TriangleStateType::BLOCKER);
    REQUIRE(enf_area > 0.5);
    REQUIRE(blk_area > 0.5);

    const std::vector<Placement> placements = overhang_placements(in, 5., 7., rect(-1., 2., 30., 8.));
    std::vector<VolumePaint> first = paint_placements(in, placements, cbIsland | cbCantilever, Level::Should, [] {});
    REQUIRE(first[0].changed);
    CHECK(state_area(in.volumes[0], first[0].data, TriangleStateType::ENFORCER) == Approx(enf_area).margin(1e-6));
    CHECK(state_area(in.volumes[0], first[0].data, TriangleStateType::BLOCKER) == Approx(blk_area).margin(1e-6));
    CHECK(paint_area(in.volumes[0], first[0].data, is_auto_state) > 10.);

    for (int run = 2; run <= 5; ++ run) {
        INFO("run " << run);
        in.volumes[0].paint = paint_placements(in, placements, cbIsland | cbCantilever, Level::Should, [] {})[0].data;
        CHECK(FacetsAnnotation::same_paint(in.volumes[0].paint, first[0].data));
    }
    // Starting from its own result, a run changes nothing.
    const std::vector<VolumePaint> again = paint_placements(in, placements, cbIsland | cbCantilever, Level::Should, [] {});
    CHECK(! again[0].changed);
    CHECK(again[0].data.bitstream.size() == first[0].data.bitstream.size());

    // Without placements the smart paint is removed, the manual paint is restored exactly.
    const std::vector<VolumePaint> cleared = paint_placements(in, {}, cbIsland | cbCantilever, Level::Should, [] {});
    CHECK(cleared[0].changed);
    CHECK(! has_auto(cleared[0].data));
    CHECK(FacetsAnnotation::same_paint(cleared[0].data, manual));
}

TEST_CASE("Prism painting rules: causes, levels, downward facets, replacement", "[SmartSupports][sas-paint]")
{
    AnalysisInput in = floating_box_input();
    const size_t k_bottom = layer_of_z(in, 5.), k_top = layer_of_z(in, 7.) - 1;
    REQUIRE(k_bottom < in.layers.size());
    REQUIRE(k_top < in.layers.size());
    const ExPolygons all = rect(-1., -1., 11., 11.);
    auto auto_area = [&](const VolumePaint &vp) { return paint_area(in.volumes[0], vp.data, is_auto_state); };

    SECTION("only downward facets are painted") {
        // A prism around the top face (and the side walls) paints nothing.
        std::vector<VolumePaint> paint = paint_placements(in, { placement(uint32_t(k_top), all) }, cbAll, Level::May, [] {});
        CHECK(! paint[0].changed);
        CHECK(! has_auto(paint[0].data));
        // A prism at the bottom paints the whole bottom.
        paint = paint_placements(in, { placement(uint32_t(k_bottom), all) }, cbAll, Level::May, [] {});
        CHECK(paint[0].changed);
        CHECK(auto_area(paint[0]) == Approx(100.).margin(1e-4));
    }
    SECTION("filters: cause toggles, lowest level, paintable, long spans") {
        const std::vector<std::pair<Placement, uint8_t>> none {
            { placement(uint32_t(k_bottom), all, Cause::Cantilever, Level::Should), cbAll },          // below Must
            { placement(uint32_t(k_bottom), all, Cause::Cantilever, Level::Must, false), cbAll },     // not paintable
            { placement(uint32_t(k_bottom), all, Cause::LongSpan, Level::Must), cbAll },              // long span (v1)
            { placement(uint32_t(k_bottom), all, Cause::Island, Level::Must), cbCantilever },         // cause unchecked
        };
        for (const auto &[p, causes] : none) {
            INFO("cause " << int(p.cause) << ", level " << int(p.level) << ", paintable " << p.paintable);
            const std::vector<VolumePaint> paint = paint_placements(in, { p }, causes, Level::Must, [] {});
            CHECK(! paint[0].changed);
            CHECK(! has_auto(paint[0].data));
        }
        const std::vector<VolumePaint> paint = paint_placements(in, { placement(uint32_t(k_bottom), all, Cause::Island, Level::Must) },
                                                                cbIsland, Level::Must, [] {});
        CHECK(state_area(in.volumes[0], paint[0].data, auto_state(cbIsland)) == Approx(100.).margin(1e-4));
    }
    SECTION("overlapping causes are OR-ed, a run replaces all smart paint") {
        const std::vector<Placement> both { placement(uint32_t(k_bottom), rect(-1., -1., 6., 11.), Cause::Island),
                                            placement(uint32_t(k_bottom), rect(4., -1., 11., 11.), Cause::Cantilever) };
        const std::vector<VolumePaint> paint = paint_placements(in, both, cbIsland | cbCantilever, Level::Should, [] {});
        const double both_area = state_area(in.volumes[0], paint[0].data, auto_state(cbIsland | cbCantilever));
        const double island    = state_area(in.volumes[0], paint[0].data, auto_state(cbIsland));
        const double cant      = state_area(in.volumes[0], paint[0].data, auto_state(cbCantilever));
        // Islands paint x ≤ 6 + e, cantilevers x ≥ 4 − e (minus at most one edge limit at the prism border).
        CHECK(both_area >= 20. - 1e-4);
        CHECK(both_area <= 25. + 1e-4);
        CHECK(island >= 37.5 - 1e-4);
        CHECK(cant >= 37.5 - 1e-4);
        CHECK(both_area + island + cant == Approx(100.).margin(1e-4));

        // Earlier smart paint plus a manual blocker; the next run paints islands only.
        in.volumes[0].paint = paint[0].data;
        paint_manual(in.volumes[0], Vec3d(8., 8., 5.), 1.f, TriangleStateType::BLOCKER);
        const double blk = state_area(in.volumes[0], in.volumes[0].paint, TriangleStateType::BLOCKER);
        REQUIRE(blk > 1.);
        const std::vector<VolumePaint> islands = paint_placements(in, both, cbIsland, Level::Should, [] {});
        CHECK(islands[0].changed);
        CHECK(state_area(in.volumes[0], islands[0].data, auto_state(cbCantilever)) == 0.);
        CHECK(state_area(in.volumes[0], islands[0].data, auto_state(cbIsland | cbCantilever)) == 0.);
        CHECK(state_area(in.volumes[0], islands[0].data, auto_state(cbIsland)) >= 60. - 1e-4);
        CHECK(state_area(in.volumes[0], islands[0].data, auto_state(cbIsland)) <= 62.5 + 1e-4);
        CHECK(state_area(in.volumes[0], islands[0].data, TriangleStateType::BLOCKER) == Approx(blk).margin(1e-6));

        // No placements: all smart paint goes, the blocker stays; a second empty run changes nothing.
        in.volumes[0].paint = islands[0].data;
        const std::vector<VolumePaint> cleared = paint_placements(in, {}, cbIsland | cbCantilever, Level::Should, [] {});
        CHECK(cleared[0].changed);
        CHECK(! has_auto(cleared[0].data));
        CHECK(state_area(in.volumes[0], cleared[0].data, TriangleStateType::BLOCKER) == Approx(blk).margin(1e-6));
        in.volumes[0].paint = cleared[0].data;
        CHECK(! paint_placements(in, {}, cbIsland | cbCantilever, Level::Should, [] {})[0].changed);
    }
    SECTION("manual paint under a prism is kept") {
        paint_manual(in.volumes[0], Vec3d(3., 3., 5.), 1.5f, TriangleStateType::ENFORCER);
        paint_manual(in.volumes[0], Vec3d(7., 7., 5.), 1.5f, TriangleStateType::BLOCKER);
        const double enf = state_area(in.volumes[0], in.volumes[0].paint, TriangleStateType::ENFORCER);
        const double blk = state_area(in.volumes[0], in.volumes[0].paint, TriangleStateType::BLOCKER);
        const std::vector<VolumePaint> paint = paint_placements(in, { placement(uint32_t(k_bottom), all, Cause::Island) },
                                                                cbIsland, Level::Should, [] {});
        CHECK(state_area(in.volumes[0], paint[0].data, TriangleStateType::ENFORCER) == Approx(enf).margin(1e-6));
        CHECK(state_area(in.volumes[0], paint[0].data, TriangleStateType::BLOCKER) == Approx(blk).margin(1e-6));
        CHECK(auto_area(paint[0]) == Approx(100. - enf - blk).margin(1e-4));
    }
}

TEST_CASE("Prism painting: volumes, determinism, cancel", "[SmartSupports][sas-paint]")
{
    // Two parts: a wedge and a floating box far away; placements on both.
    const AnalysisInput in = synthetic_input({ wedge(45.), box({ 40., 0., 5. }, { 50., 10., 7. }), box({ 80., 0., 0. }, { 90., 10., 10. }) },
                                             0.1, 0.2, test_params());
    REQUIRE(in.volumes.size() == 3);
    std::vector<Placement> placements;
    // Comb of strips on the slope (many prism borders), the bottom of the floating box.
    ExPolygons comb;
    for (int i = 0; i < 10; ++ i)
        comb.push_back(rectangle_mm(-1., 0.5 + i, 30., 0.9 + i));
    placements = overhang_placements(in, 2., 18., comb);
    placements.push_back(placement(uint32_t(layer_of_z(in, 5.)), rect(39., -1., 51., 11.), Cause::Island));
    REQUIRE(placements.size() > 100);

    const std::vector<VolumePaint> paint = paint_placements(in, placements, cbIsland | cbCantilever, Level::Should, [] {});
    REQUIRE(paint.size() == 3);
    for (size_t i = 0; i < 3; ++ i)
        CHECK(paint[i].volume_id == in.volumes[i].volume_id);
    CHECK(paint[0].changed);
    CHECK(paint[1].changed);
    CHECK(! paint[2].changed);                       // nothing planned on the third part
    CHECK(paint[2].data.triangles_to_split.empty());
    check_bounds(in, placements, project(in, paint));

    SECTION("1 thread equals the default parallelism") {
        std::vector<VolumePaint> serial;
        {
            tbb::global_control control(tbb::global_control::max_allowed_parallelism, 1);
            serial = paint_placements(in, placements, cbIsland | cbCantilever, Level::Should, [] {});
        }
        for (size_t i = 0; i < 3; ++ i) {
            CHECK(serial[i].data.triangles_to_split == paint[i].data.triangles_to_split);
            CHECK(serial[i].data.bitstream == paint[i].data.bitstream);
        }
    }
    SECTION("cancel") {
        auto count_calls = [&in](const std::vector<Placement> &placements) {
            std::atomic<int> calls { 0 };
            paint_placements(in, placements, cbIsland | cbCantilever, Level::Should, [&calls] { ++ calls; });
            return int(calls);
        };
        const int total = count_calls(placements);
        // Full strips on the same layers: the same prism layers, volumes and causes, but far fewer triangles to
        // classify. More cancel calls for the comb come from the predicate (every 4096 triangles).
        const int strips = count_calls(overhang_placements(in, 2., 18., rect(-1., 0.5, 30., 9.9)));
        INFO("cancel calls: comb " << total << ", strips " << strips);
        CHECK(total > strips);
        for (const int at : { 1, total / 2, total }) {
            std::atomic<int> count { 0 };
            CHECK_THROWS_AS(paint_placements(in, placements, cbIsland | cbCantilever, Level::Should,
                                             [&count, at] { if (++ count >= at) throw CanceledException(); }),
                            CanceledException);
        }
    }
    SECTION("no layers, no placements") {
        AnalysisInput empty = in;
        empty.layers.clear();
        for (const VolumePaint &vp : paint_placements(empty, placements, cbAll, Level::May, [] {}))
            CHECK(! vp.changed);
        for (const VolumePaint &vp : paint_placements(in, {}, cbAll, Level::May, {}))
            CHECK(! vp.changed);
    }
}
