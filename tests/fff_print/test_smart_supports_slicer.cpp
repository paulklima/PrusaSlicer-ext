// Smart support painting: slicer integration — projection of smart paint as enforcers, organic support from smart
// paint, the _SUPPORTS_OFF warning, project_states / slice_modifiers (plan §5, §7.1; WP1a).

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <algorithm>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "smart_supports_test_utils.hpp"

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/Support/SmartSupportsInternal.hpp"

using namespace Slic3r;
using namespace Slic3r::SmartSupports;
using namespace Slic3r::SmartSupports::Testing;
using Catch::Approx;

namespace {

double area_mm2(const ExPolygons &expolys) { return area(expolys) * SCALING_FACTOR * SCALING_FACTOR; }
double area_mm2(const Polygons &polys) { return area_mm2(union_ex(polys)); }

// Area of the symmetric difference of the unions, mm².
double xor_area_mm2(const Polygons &a, const Polygons &b)
{
    const ExPolygons ua = union_ex(a), ub = union_ex(b);
    return area_mm2(diff_ex(ua, ub)) + area_mm2(diff_ex(ub, ua));
}

// Paints the volume's mesh with the selector operations of ops (mesh coordinates).
void paint(ModelVolume &mv, const std::function<void(TriangleSelector&, const TriangleMesh&)> &ops)
{
    TriangleSelector selector(mv.mesh());
    selector.deserialize(mv.supported_facets.get_data());
    ops(selector, mv.mesh());
    mv.supported_facets.set(selector);
}

// Downward facing facets (mesh coordinates, the volumes of these tests are not rotated).
std::vector<int> downward_facets(const TriangleMesh &mesh)
{
    std::vector<int> out;
    const std::vector<Vec3f> normals = its_face_normals(mesh.its);
    for (size_t i = 0; i < normals.size(); ++ i)
        if (normals[i].z() < -0.5f)
            out.push_back(int(i));
    return out;
}

// All downward facets of the volume in one state.
void paint_downward(ModelVolume &mv, TriangleStateType state)
{
    paint(mv, [state](TriangleSelector &s, const TriangleMesh &mesh) {
        for (int facet : downward_facets(mesh))
            s.set_facet(facet, state);
    });
}

// Print::apply() of the (changed) live model with the full config of the print.
void reapply(Print &print, Model &model, const DynamicPrintConfig &config)
{
    DynamicPrintConfig full_config = DynamicPrintConfig::full_print_config();
    full_config.apply(config);
    print.apply(model, full_config);
}

// Index of the layer whose slab starts at z (mm).
size_t layer_starting_at(const PrintObject &po, double z)
{
    for (size_t i = 0; i < po.layer_count(); ++ i)
        if (std::abs(po.get_layer(int(i))->print_z - po.get_layer(int(i))->height - z) < 1e-4)
            return i;
    FAIL("no layer starts at z = " << z);
    return 0;
}

} // namespace

TEST_CASE("Smart paint is projected as support enforcers", "[SmartSupports][sas-slicer]")
{
    const DynamicPrintConfig config = sas_config();
    auto sp = make_sliced_print(shelf_cube(10., 15.), config);

    std::vector<Polygons> before;
    sp->print_object()->project_and_append_custom_facets(false, TriangleStateType::ENFORCER, before);
    for (const Polygons &layer : before)
        CHECK(layer.empty());

    // Shelf (volume 1, x 19..30, y 2.5..17.5, z 18..20): smart paint (overhangs) on its bottom.
    paint_downward(*sp->model_object()->volumes[1], auto_state(cbCantilever));
    reapply(sp->print, sp->model, config);
    process_to_slices(sp->print);
    const PrintObject &po = *sp->print_object();

    std::vector<Polygons> enforcers, blockers;
    po.project_and_append_custom_facets(false, TriangleStateType::ENFORCER, enforcers);
    po.project_and_append_custom_facets(false, TriangleStateType::BLOCKER, blockers);
    REQUIRE(enforcers.size() == po.layer_count());
    // A downward horizontal face is projected onto the layer above it.
    const size_t shelf_layer = layer_starting_at(po, 18.);
    for (size_t i = 0; i < enforcers.size(); ++ i) {
        INFO("layer " << i);
        if (i == shelf_layer)
            CHECK(area_mm2(enforcers[i]) == Approx(11. * 15.).epsilon(0.01));
        else
            CHECK(enforcers[i].empty());
    }
    for (const Polygons &layer : blockers)
        CHECK(layer.empty());
}

TEST_CASE("_SUPPORTS_OFF warning with smart paint", "[SmartSupports][sas-slicer]")
{
    const DynamicPrintConfig config = sas_config({ { "support_material", false } });
    Model        model;
    Print        print;
    ModelObject *mo = init_print_parts(shelf_cube(10., 15.), print, model, config);
    auto supports_off_warning = [&]() {
        reapply(print, model, config);
        std::vector<std::string> warnings;
        CHECK(print.validate(&warnings).empty());
        return std::find(warnings.begin(), warnings.end(), "_SUPPORTS_OFF") != warnings.end();
    };
    CHECK(! supports_off_warning());
    paint_downward(*mo->volumes[1], TriangleStateType::BLOCKER);
    CHECK(! supports_off_warning());
    for (uint8_t mask = 1; mask <= cbAll; ++ mask) {
        paint_downward(*mo->volumes[1], auto_state(mask));
        CHECK(supports_off_warning());
    }
    // Stock behaviour.
    paint_downward(*mo->volumes[1], TriangleStateType::ENFORCER);
    CHECK(supports_off_warning());
}

TEST_CASE("project_states and slice_modifiers equal the slicer's projections", "[SmartSupports][sas-slicer]")
{
    const DynamicPrintConfig config = sas_config();
    auto         sp = make_sliced_print(shelf_cube(10., 15.), config);
    ModelObject &mo = *sp->model_object();

    // The shelf is mirrored (in place, the box is symmetric to its centre).
    mo.volumes[1]->set_mirror(Vec3d(-1., 1., 1.));
    // Cube: one of its bottom facets is blocked (on the bed, layer 0).
    paint(*mo.volumes[0], [](TriangleSelector &s, const TriangleMesh &mesh) { s.set_facet(downward_facets(mesh).front(), TriangleStateType::BLOCKER); });
    // Shelf bottom: a manual enforcer spot, a blocker spot and smart paint around them (T-joints).
    paint(*mo.volumes[1], [](TriangleSelector &s, const TriangleMesh &mesh) {
        const std::vector<int> bottom = downward_facets(mesh);
        REQUIRE(bottom.size() == 2);
        const Transform3d trafo = Transform3d::Identity();
        for (size_t i = 0; i < 2; ++ i) {
            const stl_triangle_vertex_indices &f = mesh.its.indices[bottom[i]];
            const Vec3f c = (mesh.its.vertices[f(0)] + mesh.its.vertices[f(1)] + mesh.its.vertices[f(2)]) / 3.f;
            s.select_patch(bottom[i], TriangleSelector::SinglePointCursor::cursor_factory(c, c - Vec3f(0.f, 0.f, 10.f), 2.f,
                               TriangleSelector::CursorType::SPHERE, trafo, TriangleSelector::ClippingPlane()),
                           i == 0 ? TriangleStateType::ENFORCER : TriangleStateType::BLOCKER, trafo, true);
        }
        const float z_min = mesh.bounding_box().min.cast<float>().z();
        TriangleSelector::PredicateCursor::Predicates predicates;
        predicates.point_inside = [z_min](const Vec3f &p) { return p.z() < z_min + 1e-3f && p.x() < 3.f; };
        predicates.seed_facets  = bottom;
        s.set_paint_rule(tool_paint_rule(cbIsland));
        s.select_patch(bottom.front(), std::make_unique<TriangleSelector::PredicateCursor>(predicates, 0.1f, trafo),
                       auto_state(cbIsland), trafo, true, 89.99f);
        s.clear_paint_rule();
    });
    REQUIRE(mo.volumes[1]->supported_facets.has_auto_states());
    // Support enforcers (one of them rotated and mirrored, two of them overlapping) and a blocker under the shelf.
    {
        ModelVolume *enf = mo.add_volume(TriangleMesh(box({ 21., 4., 6. }, { 28., 16., 12. })), ModelVolumeType::SUPPORT_ENFORCER);
        enf->set_rotation(Vec3d(0.4, 0., 0.2));
        enf->set_mirror(Vec3d(1., -1., 1.));
        mo.add_volume(TriangleMesh(box({ 25., 6., 2. }, { 29., 14., 9. })), ModelVolumeType::SUPPORT_ENFORCER);
        mo.add_volume(TriangleMesh(box({ 23., 9., 0. }, { 26., 11., 17. })), ModelVolumeType::SUPPORT_BLOCKER);
    }
    reapply(sp->print, sp->model, config);
    process_to_slices(sp->print);

    const PrintObject &po = *sp->print_object();
    REQUIRE(input_consistent(po, mo, 0));
    const AnalysisInput in = build_input(po, mo, 0, UserParams());
    REQUIRE(in.volumes.size() == 2);
    REQUIRE(in.modifiers.size() == 3);
    const std::vector<const TriangleSelector::TriangleSplittingData*> per_volume(in.volumes.size(), nullptr);
    auto cancel = []() {};

    // Each layer of ours equals the slicer's (the slicer's vector is empty if there is nothing at all).
    auto compare = [&in](const std::vector<Polygons> &ours, const std::vector<Polygons> &slicer) {
        REQUIRE(ours.size() == in.layers.size());
        REQUIRE((slicer.empty() || slicer.size() == ours.size()));
        double total = 0.;
        for (size_t i = 0; i < ours.size(); ++ i) {
            INFO("layer " << i);
            CHECK(xor_area_mm2(ours[i], slicer.empty() ? Polygons() : slicer[i]) <= 1e-3);
            total += area_mm2(ours[i]);
        }
        return total;
    };

    SECTION("painted facets") {
        std::vector<Polygons> slicer_enf, slicer_blk;
        po.project_and_append_custom_facets(false, TriangleStateType::ENFORCER, slicer_enf);
        po.project_and_append_custom_facets(false, TriangleStateType::BLOCKER, slicer_blk);
        const double enf = compare(project_states(in, per_volume, [](TriangleStateType s) { return acts_as_enforcer(s); }, cancel), slicer_enf);
        const double blk = compare(project_states(in, per_volume, [](TriangleStateType s) { return s == TriangleStateType::BLOCKER; }, cancel), slicer_blk);
        CHECK(enf > 10.);
        CHECK(blk > 50.);

        // Only the smart paint, only the manual enforcers.
        const double smart  = compare(project_states(in, per_volume, [](TriangleStateType s) { return is_auto_state(s); }, cancel),
                                      project_states(in, per_volume, [](TriangleStateType s) { return s == auto_state(cbIsland); }, cancel));
        const double manual = area_mm2(project_states(in, per_volume, [](TriangleStateType s) { return s == TriangleStateType::ENFORCER; }, cancel)[layer_starting_at(po, 18.)]);
        CHECK(smart > 1.);
        CHECK(manual > 1.);
        CHECK(smart + manual == Approx(enf).epsilon(0.01));

        // Paint passed by the caller replaces the volume's paint.
        const TriangleSelector::TriangleSplittingData none {};
        std::vector<const TriangleSelector::TriangleSplittingData*> replaced = per_volume;
        replaced[1] = &none;
        const std::vector<Polygons> empty(in.layers.size());
        compare(project_states(in, replaced, [](TriangleStateType s) { return acts_as_enforcer(s); }, cancel), empty);
    }

    SECTION("support volumes") {
        const double enf = compare(slice_modifiers(in, true, cancel), po.slice_support_enforcers());
        const double blk = compare(slice_modifiers(in, false, cancel), po.slice_support_blockers());
        CHECK(enf > 100.);
        CHECK(blk > 50.);
    }

    SECTION("cancel") {
        auto throwing = []() { throw CanceledException(); };
        CHECK_THROWS_AS(project_states(in, per_volume, [](TriangleStateType s) { return acts_as_enforcer(s); }, throwing), CanceledException);
        CHECK_THROWS_AS(slice_modifiers(in, true, throwing), CanceledException);
    }
}

TEST_CASE("Organic support from smart paint", "[SmartSupports][sas-slicer][slow]")
{
    const DynamicPrintConfig config = sas_config();
    for (const bool painted : { false, true }) {
        SECTION(painted ? "with smart paint" : "without paint") {
            Model        model;
            Print        print;
            ModelObject *mo = init_print_parts(shelf_cube(10., 15.), print, model, config);
            if (painted) {
                paint_downward(*mo->volumes[1], auto_state(cbCantilever));
                reapply(print, model, config);
            }
            print.process();

            const PrintObject &po   = *print.objects().front();
            const BoundingBox  cube = get_extents(po.get_layer(10)->lslices);
            ExPolygons         support;
            double             z_min = std::numeric_limits<double>::max(), z_max = 0.;
            for (const SupportLayer *layer : po.support_layers())
                if (! layer->support_fills.empty()) {
                    append(support, layer->support_islands);
                    z_min = std::min(z_min, layer->print_z);
                    z_max = std::max(z_max, layer->print_z);
                }
            if (! painted) {
                CHECK(support.empty());
            } else {
                REQUIRE(! support.empty());
                // Under the shelf only: right of the cube, which the support keeps clear of.
                for (const ExPolygon &island : support)
                    CHECK(get_extents(island).min.x() >= cube.max.x() - scaled<coord_t>(0.1));
                // From the bed up to the contact distance below the shelf (z = 18).
                CHECK(z_min < 0.5);
                CHECK(z_max > 17.);
            }
        }
    }
}
