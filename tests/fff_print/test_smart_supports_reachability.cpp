// Smart support painting: Reachability — plate coverage incl. cover, roof path, roof merge, raft indexing,
// build plate only, support on the model, first support layer, pull model, collision / avoidance against
// TreeModelVolumes and the real-contact cross-check against a full process() (plan §7.1; WP2r).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <sstream>
#include <string>

#include <oneapi/tbb/global_control.h>

#include "smart_supports_test_utils.hpp"

#include "libslic3r/AABBTreeLines.hpp"
#include "libslic3r/BuildVolume.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/PrintBase.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/Support/SmartSupportsInternal.hpp"
#include "libslic3r/Support/TreeModelVolumes.hpp"
#include "libslic3r/Support/TreeSupportCommon.hpp"

using namespace Slic3r;
using namespace Slic3r::SmartSupports;
using namespace Slic3r::SmartSupports::Testing;

namespace {

double area_mm2(const ExPolygons &e) { return area(e) * SCALING_FACTOR * SCALING_FACTOR; }

double sym_diff_mm2(const ExPolygons &a, const ExPolygons &b) { return area_mm2(diff_ex(a, b)) + area_mm2(diff_ex(b, a)); }

const ThrowOnCancel never_cancel = []() {};

// Shapes of this file (plan §7 rules: WP-specific shapes stay in the WP's test file).
Parts sphere(double r) { return { translate(its_make_sphere(r, M_PI / 45.), Vec3d(0., 0., r)) }; }

// A 5 × 5 stem of height stem_h under a 15 × 15 × 2 cap: the cap's first layer overhangs everywhere around the stem.
Parts mushroom(double stem_h)
{
    return { box({ 0., 0., 0. }, { 5., 5., stem_h }), box({ -5., -5., stem_h }, { 10., 10., stem_h + 2. }) };
}

// Index of the layer whose slab contains z (object frame).
uint32_t layer_at(const AnalysisInput &in, double z)
{
    uint32_t best = 0;
    for (uint32_t n = 0; n < in.layers.size(); ++ n)
        if (std::abs(in.layers[n].slice_z - z) < std::abs(in.layers[best].slice_z - z))
            best = n;
    return best;
}

// Overhang front with α = 45°: U_n = L_n − (L_{n−1} ⊕ h_{n−1}) (plan §3.5, w_n = h_{n−1} / tan α).
ExPolygons front_zone(const AnalysisInput &in, uint32_t n)
{
    return diff_ex(in.layers[n].lslices,
                   offset(in.layers[n - 1].lslices, scaled<float>(in.layers[n - 1].height), ClipperLib::jtRound, scaled<double>(0.001)));
}

// Tail ring (w = 0): O_n = L_n − L_{n−1} (plan §3.6).
ExPolygons tail_zone(const AnalysisInput &in, uint32_t n) { return diff_ex(in.layers[n].lslices, in.layers[n - 1].lslices); }

AnalysisInput input_of(const SasPrint &sp) { return build_input(*sp.print_object(), *sp.print_object()->model_object(), 0, UserParams()); }

// Collisions computed and the avoidance chain ensured up to the last tip layer.
void prepare(Reachability &reach, const AnalysisInput &in)
{
    reach.compute_collisions(never_cancel);
    reach.ensure(reach.tip_layer(uint32_t(in.layers.size() - 1)), never_cancel);
}

// The slicer's own collision and Fast avoidance to the build plate of the tip radius, precalculated as the
// slicer does it (TreeSupport.cpp:3482-3497).
struct SlicerVolumes {
    SlicerVolumes(const PrintObject &po, int max_layer) :
        mesh_group(po), settings(mesh_group, po.slicing_parameters()),
        volumes(po, BuildVolume(Pointfs{ Vec2d{ -300., -300. }, Vec2d{ -300., 300. }, Vec2d{ 300., 300. }, Vec2d{ 300., -300. } }, 0.),
                settings.maximum_move_distance, settings.maximum_move_distance_slow, 0)
    {
        volumes.precalculate(po, max_layer, []() {});
    }
    bool min_xy_dist() const { return settings.xy_distance > settings.xy_min_distance; }
    const Polygons& collision(int s) const { return volumes.getCollision(settings.getRadius(0), s, this->min_xy_dist()); }
    const Polygons& avoidance(int s) const
        { return volumes.getAvoidance(settings.getRadius(0), s, FFFTreeSupport::TreeModelVolumes::AvoidanceType::Fast, false, this->min_xy_dist()); }

    FFFTreeSupport::TreeSupportMeshGroupSettings mesh_group;
    FFFTreeSupport::TreeSupportSettings          settings;
    FFFTreeSupport::TreeModelVolumes             volumes;
};

// Our collision and avoidance of support layer t, recovered through the public queries: layer n = tip layer + z_top + 1
// is replaced by a large square E (coll_t only depends on the layers up to n − 1). Then the tip area T of tips(E, n) is
// the complement of coll_t ⊕ 5 µm, and tips(E, n).model = T ∩ avoid_t (support rests on the model).
struct Volumes { ExPolygons coll, avoid; };
Volumes recover_volumes(const AnalysisInput &in0, int t, const BoundingBox &region)
{
    AnalysisInput in = in0;
    in.params.buildplate_only = false;
    const int n = t - in.params.raft_support_layers + in.params.z_top_layers + 1;
    REQUIRE(n >= 1);
    REQUIRE(n < int(in.layers.size()));
    const ExPolygons big { rectangle_mm(unscaled(region.min.x()) - 5., unscaled(region.min.y()) - 5.,
                                        unscaled(region.max.x()) + 5., unscaled(region.max.y()) + 5.) };
    in.layers[size_t(n)].lslices = big;
    Reachability reach(in);
    reach.compute_collisions(never_cancel);
    reach.ensure(t, never_cancel);
    REQUIRE(reach.tip_layer(uint32_t(n)) == t);
    REQUIRE(reach.tips_possible(uint32_t(n)));
    const Reachability::Tips tips = reach.tips(big, uint32_t(n));
    const ExPolygons R { ExPolygon(region.polygon()) };
    Volumes out;
    out.coll  = diff_ex(R, union_ex(tips.bp, tips.model));
    out.avoid = union_ex(out.coll, intersection_ex(R, tips.model));
    return out;
}

// Compares the recovered collision and avoidance with TreeModelVolumes at the given support layers (≤ 5 % sym. diff).
void check_volumes(const SasPrint &sp, const std::vector<int> &tip_layers)
{
    const PrintObject   &po = *sp.print_object();
    const AnalysisInput  in = input_of(sp);
    const SlicerVolumes  slicer(po, *std::max_element(tip_layers.begin(), tip_layers.end()));
    BoundingBox region;
    for (const LayerInput &l : in.layers)
        for (const ExPolygon &ex : l.lslices)
            region.merge(get_extents(ex));
    region.offset(scaled<coord_t>(3.));
    const ExPolygons R { ExPolygon(region.polygon()) };
    for (int t : tip_layers) {
        const Volumes    ours   = recover_volumes(in, t, region);
        const ExPolygons coll   = intersection_ex(R, offset(union_ex(slicer.collision(t)), scaled<float>(0.005), ClipperLib::jtMiter, 1.2));
        const ExPolygons avoid  = union_ex(coll, intersection_ex(R, union_ex(slicer.avoidance(t))));
        INFO("support layer " << t << ": collision " << area_mm2(coll) << " mm², avoidance " << area_mm2(avoid) << " mm²");
        CHECK(area_mm2(coll) > 1.);
        CHECK(sym_diff_mm2(ours.coll, coll) <= 0.05 * area_mm2(coll) + 0.01);
        CHECK(sym_diff_mm2(ours.avoid, avoid) <= 0.05 * area_mm2(avoid) + 0.01);
    }
}

} // namespace

TEST_CASE("Reachability: plate coverage incl. cover, interface 0", "[SmartSupports][sas-reachability]")
{
    struct Case { std::string name; std::function<Parts()> parts; DynamicPrintConfig config; double z; bool tail; };
    const std::vector<Case> cases {
        { "40° wall front",                     [] { return slope_wall(40., 15., 5., 20.); }, sas_config(), 7.5, false },
        { "sphere ring r 12.1",                 [] { return sphere(20.); },                   sas_config(), 20. - std::sqrt(400. - 12.1 * 12.1), false },
        { "inverted 60° cone tail ring",        [] { return inverted_cone(60.); },            sas_config(), 6., true },
        { "inverted 75° cone tail ring",        [] { return inverted_cone(75.); },            sas_config(), 6., true },
        { "60° cone, 0.1 mm layers, z_top 2",   [] { return inverted_cone(60.); },            sas_config({ { "layer_height", 0.1 } }), 6., true },
    };
    for (const Case &c : cases)
        SECTION(c.name) {
            const SasPrint     &sp = cached_print("reach_" + c.name, c.parts, c.config);
            const AnalysisInput in = input_of(sp);
            REQUIRE(in.params.interface_layers == 0);
            if (c.config.opt_float("layer_height") < 0.15)
                REQUIRE(in.params.z_top_layers == 2);
            Reachability reach(in);
            prepare(reach, in);
            const uint32_t   n = layer_at(in, c.z);
            REQUIRE(reach.tips_possible(n));
            const ExPolygons Z = c.tail ? tail_zone(in, n) : front_zone(in, n);
            REQUIRE(area_mm2(Z) > 0.01);
            const ExPolygons plate = reach.plate_part(Z, {}, n);
            INFO("layer " << n << ": zone " << area_mm2(Z) << " mm², plate part " << area_mm2(plate) << " mm²");
            CHECK(area_mm2(plate) >= 0.9 * area_mm2(Z));
            CHECK(area_mm2(diff_ex(plate, Z)) < 1e-6);
            CHECK(area_mm2(reach.model_part(Z, {}, n)) < 0.01 * area_mm2(Z));
            // The contact alone (without the gap band) does not reach L_{n−1}: the band G is what covers the zone.
            const Reachability::Contact contact = reach.contact(Z, n);
            CHECK(! contact.roof_gap);
            CHECK(contact.all == contact.tip_path_all);
            CHECK(area_mm2(intersection_ex(contact.all, offset_ex(in.layers[n - 1].lslices, scaled<float>(0.05)))) < 1e-3);
        }
}

TEST_CASE("Reachability: interface 2, roof gap on a 40° wall", "[SmartSupports][sas-reachability]")
{
    const SasPrint &sp = cached_print("reach_40° wall front", [] { return slope_wall(40., 15., 5., 20.); },
                                      sas_config({ { "support_material_interface_layers", 2 } }));
    const AnalysisInput in = input_of(sp);
    REQUIRE(in.params.interface_layers == 2);
    Reachability reach(in);
    prepare(reach, in);
    const uint32_t   n = layer_at(in, 7.5);
    const ExPolygons Z = front_zone(in, n);
    REQUIRE(area_mm2(Z) > 0.1);
    const Reachability::Contact contact = reach.contact(Z, n);
    CHECK(contact.roof_gap);
    CHECK(! contact.all.empty());
    CHECK(area_mm2(reach.plate_part(Z, {}, n)) < 0.01 * area_mm2(Z));
    CHECK(area_mm2(reach.model_part(Z, {}, n)) < 0.01 * area_mm2(Z));
    // InterfaceGap attribution: the zone would be covered by the tip path.
    CHECK(area_mm2(diff_ex(Z, reach.cover(contact.tip_path_all, n))) < 0.01 * area_mm2(Z));
    // The roof starts r + xy_min/2 + 5 µm = 0.5175 mm from L_{n−1} (TreeSupport.cpp:1327, 1418).
    const ExPolygons near = offset_ex(in.layers[n - 1].lslices, scaled<float>(0.5));
    CHECK(area_mm2(intersection_ex(contact.all, near)) < 1e-3);
    CHECK(area_mm2(intersection_ex(contact.all, offset_ex(in.layers[n - 1].lslices, scaled<float>(0.55)))) > 0.1);
}

TEST_CASE("Reachability: roof decision on the union of the painted zones", "[SmartSupports][sas-reachability]")
{
    const SasPrint &sp = cached_print("shelf_10x15", [] { return shelf_cube(10., 15.); },
                                      sas_config({ { "support_material_interface_layers", 2 } }));
    const AnalysisInput in = input_of(sp);
    Reachability reach(in);
    prepare(reach, in);
    const uint32_t n = layer_at(in, 18.1);   // first shelf layer
    REQUIRE(area_mm2(tail_zone(in, n)) > 100.);
    // Two 0.3 × 0.3 mm pieces at the cube face, 0.4 mm apart: each alone has a roof candidate of ≈ 0.47 × 1.9 mm² beyond
    // forbid_{t−1}, below the minimum roof area of 1 mm²; together ≈ 0.47 × 2.6 mm².
    const BoundingBox cube = get_extents(in.layers[n - 1].lslices);
    const double      x0   = unscaled(cube.max.x());
    const double      yc   = unscaled(cube.center().y());
    const ExPolygons  A { rectangle_mm(x0, yc - 0.5, x0 + 0.3, yc - 0.2) };
    const ExPolygons  B { rectangle_mm(x0, yc + 0.2, x0 + 0.3, yc + 0.5) };
    const ExPolygons  AB = union_ex(A, B);
    const Reachability::Contact cA  = reach.contact(A, n);
    const Reachability::Contact cB  = reach.contact(B, n);
    const Reachability::Contact cAB = reach.contact(AB, n);
    CHECK(! cA.roof_gap);
    CHECK(! cB.roof_gap);
    CHECK(cAB.roof_gap);
    REQUIRE(! cA.all.empty());
    REQUIRE(! cAB.all.empty());
    // predict_F is not superadditive with roofs (review 7 M2): the separate predictions cover more than the union's.
    const ExPolygons separate = union_ex(reach.predict_F(A, n), reach.predict_F(B, n));
    const ExPolygons together = reach.predict_F(AB, n);
    INFO("separate " << area_mm2(separate) << " mm², together " << area_mm2(together) << " mm²");
    CHECK(sym_diff_mm2(separate, together) > 0.1);
    CHECK(area_mm2(diff_ex(separate, together)) > 0.1);
    // Without interface layers the union is the union of the parts (tip path everywhere).
    const SasPrint &sp0 = cached_print("shelf_10x15", [] { return shelf_cube(10., 15.); }, sas_config());
    const AnalysisInput in0 = input_of(sp0);
    Reachability reach0(in0);
    prepare(reach0, in0);
    CHECK(sym_diff_mm2(union_ex(reach0.predict_F(A, n), reach0.predict_F(B, n)), reach0.predict_F(AB, n)) < 0.01);
}

TEST_CASE("Reachability: raft indexing", "[SmartSupports][sas-reachability]")
{
    const SasPrint &sp = cached_print("reach_mushroom_0.2", [] { return mushroom(0.2); }, sas_config({ { "raft_layers", 2 } }));
    const AnalysisInput in = input_of(sp);
    const int R = in.params.raft_support_layers;
    REQUIRE(R >= 2);
    Reachability reach(in);
    reach.compute_collisions(never_cancel);
    // Support-layer space: tip_layer(n) = n + raft_support_layers − (z_top + 1); no negative index anywhere.
    CHECK(reach.tip_layer(1) == 1 + R - (in.params.z_top_layers + 1));
    CHECK(reach.tip_layer(0) >= 0);
    reach.ensure(-1, never_cancel);
    reach.ensure(reach.tip_layer(0), never_cancel);
    prepare(reach, in);
    // The cap's first layer n = 1: tips at a raft-region support layer, all reaching the plate (only raft below).
    REQUIRE(reach.tips_possible(1));
    const ExPolygons &L1 = in.layers[1].lslices;
    const Reachability::Tips tips = reach.tips(L1, 1);
    CHECK(! tips.bp.empty());
    CHECK(tips.model.empty());
    // The collision of the raft-region tip layer holds the object layer within z_top above (TreeModelVolumes.cpp:490-527).
    CHECK(area_mm2(intersection_ex(tips.bp, offset_ex(in.layers[0].lslices, scaled<float>(0.5)))) < 1e-3);
    CHECK(area_mm2(reach.plate_part(tail_zone(in, 1), {}, 1)) >= 0.9 * area_mm2(tail_zone(in, 1)));
    // Against the slicer's TreeModelVolumes in support-layer indices (raft layers without outlines).
    check_volumes(sp, { reach.tip_layer(1), reach.tip_layer(5) });
}

TEST_CASE("Reachability: build plate only", "[SmartSupports][sas-reachability]")
{
    const SasPrint &sp = cached_print("t_on_base_80", [] { return t_shape(15., 10., 10., 20., 2., 80., 2.); }, sas_config());
    AnalysisInput in = input_of(sp);
    in.params.buildplate_only = true;      // the slicer's setting decides, not the plate policy (D35)
    in.params.user.plate_policy = PlatePolicy::PreferPlate;
    Reachability reach(in);
    prepare(reach, in);
    const uint32_t   n = layer_at(in, 20.1);  // first arm layer
    const ExPolygons Z = diff_ex(tail_zone(in, n), offset(in.layers[n - 1].lslices, scaled<float>(3.)));
    REQUIRE(area_mm2(Z) > 100.);
    const Reachability::Tips tips = reach.tips(Z, n);
    CHECK(tips.model.empty());
    CHECK(tips.spill_model.empty());
    CHECK(tips.bp.empty());
    CHECK(reach.model_part(Z, {}, n).empty());
    CHECK(reach.plate_part(Z, {}, n).empty());
    CHECK(reach.contact(Z, n).all.empty());
    CHECK(! reach.below_model_too_short(Z, n));
    // A reachable overhang still gets plate tips.
    const SasPrint &sp2 = cached_print("shelf_10x15", [] { return shelf_cube(10., 15.); }, sas_config());
    AnalysisInput in2 = input_of(sp2);
    in2.params.buildplate_only = true;
    Reachability reach2(in2);
    prepare(reach2, in2);
    const uint32_t n2 = layer_at(in2, 18.1);
    const ExPolygons Z2 = tail_zone(in2, n2);
    CHECK(reach2.tips(Z2, n2).model.empty());
    CHECK(area_mm2(reach2.plate_part(Z2, {}, n2)) >= 0.9 * area_mm2(Z2));
}

TEST_CASE("Reachability: T arm over an 80 x 80 base rests on the model", "[SmartSupports][sas-reachability]")
{
    const SasPrint &sp = cached_print("t_on_base_80", [] { return t_shape(15., 10., 10., 20., 2., 80., 2.); }, sas_config());
    const AnalysisInput in = input_of(sp);
    REQUIRE(! in.params.buildplate_only);
    Reachability reach(in);
    prepare(reach, in);
    const uint32_t   n = layer_at(in, 20.1);
    const ExPolygons Z = diff_ex(tail_zone(in, n), offset(in.layers[n - 1].lslices, scaled<float>(3.)));
    REQUIRE(area_mm2(Z) > 100.);
    CHECK(area_mm2(reach.plate_part(Z, {}, n)) < 0.01 * area_mm2(Z));
    CHECK(area_mm2(reach.model_part(Z, {}, n)) >= 0.9 * area_mm2(Z));
    const Reachability::Tips tips = reach.tips(Z, n);
    CHECK(tips.bp.empty());
    CHECK(! tips.model.empty());
    CHECK(area_mm2(tips.spill_model) == area_mm2(tips.model));
    // The branches are 18 mm long: not too short.
    CHECK(! reach.below_model_too_short(Z, n));

    SECTION("arm 0.6 mm above the base: on-model branches too short, deleted (D23)") {
        const SasPrint &low = cached_print("t_on_base_40_low", [] { return t_shape(10., 10., 10., 2.6, 2., 40., 2.); }, sas_config());
        const AnalysisInput in2 = input_of(low);
        Reachability reach2(in2);
        prepare(reach2, in2);
        const uint32_t   n2 = layer_at(in2, 2.7);
        const ExPolygons Z2 = diff_ex(tail_zone(in2, n2), offset(in2.layers[n2 - 1].lslices, scaled<float>(2.)));
        REQUIRE(area_mm2(Z2) > 50.);
        CHECK(! reach2.tips(Z2, n2).model.empty());
        CHECK(area_mm2(reach2.model_part(Z2, {}, n2)) < 0.01 * area_mm2(Z2));
        CHECK(reach2.below_model_too_short(Z2, n2));
    }
}

TEST_CASE("Reachability: first support layer", "[SmartSupports][sas-reachability]")
{
    // Cap overhang at layer 2: tip layer 2 − (z_top + 1) = 0 < 1, the slicer places no tips (TreeSupport.cpp:1303).
    const SasPrint &sp = cached_print("reach_mushroom_0.4", [] { return mushroom(0.4); }, sas_config());
    const AnalysisInput in = input_of(sp);
    Reachability reach(in);
    prepare(reach, in);
    CHECK(reach.tip_layer(2) == 0);
    CHECK(! reach.tips_possible(2));
    CHECK(reach.tips_possible(3));
    const ExPolygons &L2 = in.layers[2].lslices;
    CHECK(! reach.F_grown(L2, 2).empty());
    CHECK(reach.tips(L2, 2).bp.empty());
    CHECK(reach.contact(L2, 2).all.empty());
    CHECK(reach.plate_part(tail_zone(in, 2), {}, 2).empty());

    const SasPrint &sp3 = cached_print("reach_mushroom_0.6", [] { return mushroom(0.6); }, sas_config());
    const AnalysisInput in3 = input_of(sp3);
    Reachability reach3(in3);
    prepare(reach3, in3);
    REQUIRE(reach3.tips_possible(3));
    CHECK(! reach3.contact(in3.layers[3].lslices, 3).all.empty());
}

TEST_CASE("Reachability: pull model", "[SmartSupports][sas-reachability]")
{
    // Stacked spheres: the upper spheres hang over the lower ones, so contacts and tip status depend on the avoidance.
    const SasPrint     &sp  = cached_print("reach_figurine_proxy", [] { return figurine_proxy(); }, sas_config());
    const AnalysisInput in  = input_of(sp);
    const auto          N   = uint32_t(in.layers.size());
    auto probe = [&in, N](const Reachability &reach) {
        std::vector<ExPolygons> out;
        for (uint32_t n = 3; n < N; n += 23) {
            out.push_back(reach.contact(in.layers[n].lslices, n).all);
            out.push_back(reach.tips(in.layers[n].lslices, n).model);
        }
        return out;
    };
    const int top = Reachability(in).tip_layer(N - 1);
    // Reference: the chain advanced by ensure() only, layer by layer (computed once for all sections).
    static const std::vector<ExPolygons> expected = [&in, &probe, top]() {
        Reachability ref(in);
        ref.compute_collisions(never_cancel);
        for (int k = 0; k <= top; ++ k)
            ref.ensure(k, never_cancel);
        return probe(ref);
    }();
    size_t non_empty_contact = 0, non_empty_model = 0;
    for (size_t i = 0; i < expected.size(); i += 2) {
        non_empty_contact += ! expected[i].empty();
        non_empty_model   += ! expected[i + 1].empty();
    }
    REQUIRE(non_empty_contact >= 4);
    REQUIRE(non_empty_model >= 2);
    const IsCanceled never = []() { return false; };

    SECTION("prefetch to the top with max_allowed_parallelism 1") {
        tbb::global_control gc(tbb::global_control::max_allowed_parallelism, 1);
        Reachability reach(in);
        reach.compute_collisions(never_cancel);
        reach.start_prefetch(top, never);
        reach.ensure(-1, never_cancel);      // no-op
        reach.ensure(top / 2, never_cancel); // waits for the prefetch thread
        reach.ensure(top, never_cancel);
        CHECK(probe(reach) == expected);     // identical whoever computed the chain (§3.12)
    }
    SECTION("ensure beyond the prefetch target waits for the thread, then advances") {
        Reachability reach(in);
        reach.compute_collisions(never_cancel);
        reach.start_prefetch(top / 3, never);
        reach.ensure(top, never_cancel);
        CHECK(probe(reach) == expected);
    }
    SECTION("queries without ensure advance the chain themselves") {
        Reachability reach(in);
        CHECK(probe(reach) == expected);
    }
    SECTION("cancel during the prefetch: CanceledException from ensure") {
        std::atomic<int> polls { 0 };
        Reachability reach(in);
        reach.compute_collisions(never_cancel);
        reach.start_prefetch(top, [&polls]() { return ++ polls > 10; });
        CHECK_THROWS_AS(reach.ensure(top, [&polls]() { if (polls.load() > 10) throw CanceledException(); }), CanceledException);
        CHECK(polls.load() > 10);
    }
    SECTION("the destructor stops and joins a running prefetch") {
        {
            Reachability reach(in);
            reach.compute_collisions(never_cancel);
            reach.start_prefetch(top, never);
        }
        SUCCEED();
    }
}

TEST_CASE("Reachability: collision and Fast avoidance equal TreeModelVolumes", "[SmartSupports][sas-reachability][slow]")
{
    SECTION("T on an 80 x 80 base") {
        const SasPrint &sp = cached_print("t_on_base_80", [] { return t_shape(15., 10., 10., 20., 2., 80., 2.); }, sas_config());
        check_volumes(sp, { 3, 20, 50, 80, 98 });
    }
    SECTION("sphere R 20 on the bed") {
        const SasPrint &sp = cached_print("reach_sphere_r20", [] { return sphere(20.); }, sas_config());
        check_volumes(sp, { 10, 60, 100, 140, 190 });
    }
}

namespace {

// A print of the parts with the facets selected by paint (object frame) painted as support enforcers, fully processed.
std::unique_ptr<SasPrint> make_painted_print(const Parts &parts, const DynamicPrintConfig &config,
                                             const std::function<bool(const Vec3d &centroid, const Vec3d &normal)> &paint)
{
    auto out = std::make_unique<SasPrint>();
    DynamicPrintConfig full_config = DynamicPrintConfig::full_print_config();
    full_config.apply(config);
    ModelObject *object = out->model.add_object();
    for (const indexed_triangle_set &its : parts)
        object->add_volume(TriangleMesh(its));
    object->add_instance();
    out->model.center_instances_around_point({ 100., 100. });
    object->ensure_on_bed();
    for (ModelVolume *mv : object->volumes) {
        const indexed_triangle_set &its   = mv->mesh().its;
        const Transform3d           trafo = mv->get_matrix();
        TriangleSelector            selector(mv->mesh());
        for (size_t i = 0; i < its.indices.size(); ++ i) {
            const Vec3d a = trafo * its.vertices[its.indices[i](0)].cast<double>();
            const Vec3d b = trafo * its.vertices[its.indices[i](1)].cast<double>();
            const Vec3d c = trafo * its.vertices[its.indices[i](2)].cast<double>();
            if (paint((a + b + c) / 3., (b - a).cross(c - a).normalized()))
                selector.set_facet(int(i), TriangleStateType::ENFORCER);
        }
        mv->supported_facets.set(selector);
    }
    out->print.auto_assign_extruders(object);
    out->print.apply(out->model, full_config);
    if (std::string err = out->print.validate(); ! err.empty())
        throw std::runtime_error("make_painted_print: " + err);
    out->print.set_status_silent();
    out->print.process();
    return out;
}

const SupportLayer* support_layer_at(const PrintObject &po, double print_z)
{
    for (const SupportLayer *sl : po.support_layers())
        if (std::abs(sl->print_z - print_z) < 1e-3)
            return sl;
    return nullptr;
}

struct CrossCheck {
    int    covered = 0, covered_ok = 0;   // samples predicted covered / with real support within tip_spacing/2 + r
    int    gap = 0, gap_near = 0;         // samples predicted uncovered by the roof gap / with real support within G
    double min_gap_distance = std::numeric_limits<double>::max();
    double predicted_mm2 = 0, real_mm2 = 0;
};

// Samples (0.05 mm grid) of the painted overhang E ∩ (L_n − L_{n−1}) of every layer, against the real support of the
// tip layer (SupportLayer::support_islands).
CrossCheck cross_check(const SasPrint &sp)
{
    const PrintObject  &po = *sp.print_object();
    const AnalysisInput in = input_of(sp);
    REQUIRE(in.params.raft_support_layers == 0);
    Reachability reach(in);
    prepare(reach, in);
    std::vector<Polygons> enforcers;
    po.project_and_append_custom_facets(false, TriangleStateType::ENFORCER, enforcers);
    REQUIRE(enforcers.size() == in.layers.size());
    const double d_ok  = scaled<double>(0.5 * in.params.tip_spacing + 0.5 * in.params.tip_diameter);
    const double d_gap = scaled<double>(reach.gap_band_mm());
    const coord_t step = scaled<coord_t>(0.05);
    CrossCheck out;
    for (uint32_t n = 1; n < in.layers.size(); ++ n) {
        if (enforcers[n].empty() || ! reach.tips_possible(n))
            continue;
        const ExPolygons E  = union_ex(enforcers[n]);
        const ExPolygons Zs = intersection_ex(E, tail_zone(in, n));
        if (Zs.empty())
            continue;
        const Reachability::Contact contact = reach.contact(E, n);
        const ExPolygons cov     = reach.cover(contact.all, n);
        const ExPolygons cov_tip = reach.cover(contact.tip_path_all, n);
        const int t = reach.tip_layer(n);
        const SupportLayer *sl = support_layer_at(po, po.get_layer(t)->print_z);
        const ExPolygons islands = sl != nullptr ? sl->support_islands : ExPolygons();
        const AABBTreeLines::LinesDistancer<Line> distancer(to_lines(islands));
        out.predicted_mm2 += area_mm2(contact.all);
        out.real_mm2      += area_mm2(intersection_ex(islands, offset_ex(reach.F_grown(E, n), scaled<float>(0.5 * in.params.tip_diameter))));
        const BoundingBox bbox = get_extents(Zs);
        for (coord_t y = bbox.min.y() + step / 2; y < bbox.max.y(); y += step)
            for (coord_t x = bbox.min.x() + step / 2; x < bbox.max.x(); x += step) {
                const Point p(x, y);
                if (std::none_of(Zs.begin(), Zs.end(), [&p](const ExPolygon &ex) { return ex.contains(p); }))
                    continue;
                const auto inside = [&p](const ExPolygons &ex) { return std::any_of(ex.begin(), ex.end(), [&p](const ExPolygon &e) { return e.contains(p); }); };
                const double d = islands.empty() ? std::numeric_limits<double>::max() :
                                 std::max(0., distancer.distance_from_lines<true>(p));
                if (inside(cov)) {
                    ++ out.covered;
                    out.covered_ok += d <= d_ok;
                } else if (inside(cov_tip)) {
                    ++ out.gap;
                    out.gap_near += d <= d_gap;
                    out.min_gap_distance = std::min(out.min_gap_distance, unscaled(d));
                }
            }
    }
    return out;
}

} // namespace

TEST_CASE("Reachability: real contact cross-check against process()", "[SmartSupports][sas-reachability][slow]")
{
    struct Case { std::string name; std::function<Parts()> parts; std::function<bool(const Vec3d&, const Vec3d&)> paint; };
    const std::vector<Case> cases {
        { "40° wall", [] { return slope_wall(40., 10., 5., 20.); },
          [](const Vec3d &c, const Vec3d &nrm) { return nrm.z() < -0.3 && c.z() > 0.3; } },
        { "sphere ring", [] { return sphere(10.); },
          [](const Vec3d &c, const Vec3d &nrm) { const double r = std::hypot(c.x(), c.y()); return nrm.z() < -0.2 && r > 2.5 && r < 6.5; } },
    };
    for (const Case &c : cases)
        for (int interface_layers : { 0, 2 })
            SECTION(c.name + ", interface layers " + std::to_string(interface_layers)) {
                auto sp = make_painted_print(c.parts(), sas_config({ { "support_material_interface_layers", interface_layers } }), c.paint);
                const CrossCheck r = cross_check(*sp);
                std::ostringstream log;
                log << c.name << ", interface " << interface_layers << ": covered " << r.covered_ok << " / " << r.covered
                    << " with real support within tip_spacing/2 + r; roof gap samples " << r.gap << ", within G " << r.gap_near
                    << ", min distance " << (r.gap > 0 ? r.min_gap_distance : 0.) << " mm; contact area real / predicted "
                    << r.real_mm2 << " / " << r.predicted_mm2 << " mm²";
                WARN(log.str());
                if (interface_layers == 0) {
                    CHECK(r.covered > 100);
                    CHECK(r.gap == 0);
                } else
                    CHECK(r.gap > 100);
                if (r.covered > 20)
                    CHECK(r.covered_ok >= 0.9 * r.covered);
                CHECK(r.gap_near == 0);
            }
}
