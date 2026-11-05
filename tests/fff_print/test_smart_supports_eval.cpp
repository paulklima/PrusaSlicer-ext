// Smart support painting: Phase B evaluator — evaluate_pass on reference cases #1, #5, #13, #14 and cone tails
// (plan §7.1), plus the evaluator mechanics: islands and folding, touched clusters, cause overrides, known reasons
// (NoTipLayer, InterfaceGap), determinism and cancel (WP2c1a).

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>

#include <oneapi/tbb/global_control.h>

#include "smart_supports_test_utils.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/PrintBase.hpp"
#include "libslic3r/Support/SmartSupportsInternal.hpp"

using namespace Slic3r;
using namespace Slic3r::SmartSupports;
using namespace Slic3r::SmartSupports::Testing;
using Catch::Approx;

namespace {

double mm2(const ExPolygons &ex) { return area(ex) * SCALING_FACTOR * SCALING_FACTOR; }

double need_area(const LayerNeeds &ln, Cause cause, Level lowest = Level::May)
{
    double a = 0.;
    for (int l = int(lowest); l < 3; ++ l)
        a += mm2(ln.need[int(cause)][l]);
    return a;
}

double need_area(const LayerNeeds &ln)
{
    return need_area(ln, Cause::Island) + need_area(ln, Cause::Cantilever) + need_area(ln, Cause::LongSpan);
}

ExPolygons need_union(const LayerNeeds &ln)
{
    ExPolygons all;
    for (int c = 0; c < 3; ++ c)
        for (int l = 0; l < 3; ++ l)
            append(all, ln.need[c][l]);
    return union_ex(all);
}

double total_need_area(const PassResult &pass)
{
    double a = 0.;
    for (const LayerNeeds &ln : pass.layers)
        a += need_area(ln);
    return a;
}

float max_c(const PassResult &pass)
{
    float m = 0.f;
    for (const LayerNeeds &ln : pass.layers)
        m = std::max(m, ln.stats.max_c);
    return m;
}

// Lowest layer with a need of the cause at or above the level; −1 if none.
int first_need_layer(const PassResult &pass, Cause cause, Level lowest)
{
    for (size_t n = 0; n < pass.layers.size(); ++ n)
        if (need_area(pass.layers[n], cause, lowest) > 0.)
            return int(n);
    return -1;
}

// Painted zones of a pass with "F = manual only" (plan §7.1): the manual enforcers of each layer.
std::vector<ExPolygons> manual_zones(const BaseModel &base)
{
    std::vector<ExPolygons> E;
    for (const LayerGeom &lg : base.layers)
        E.push_back(lg.manual_enf);
    return E;
}

// The reference cases assume the theory values: α = β = 45° (theory §10: w = 0.2 mm at h 0.2), K_max 3, B_max 40,
// B_ceil 40, H_s 1 (§9), independent of the MK4S product defaults of UserParams.
UserParams ref_params() { return theory_user_params(); }

AnalysisInput input_of(const SasPrint &sp, const UserParams &up = ref_params())
{
    return build_input(*sp.print_object(), *sp.print_object()->model_object(), 0, up);
}

// One evaluate_pass on a fresh Reachability; prints the time.
PassResult run_pass(const char *name, const AnalysisInput &in, const BaseModel &base, const std::vector<ExPolygons> &E)
{
    const auto t0 = std::chrono::steady_clock::now();
    Reachability reach(in);
    reach.compute_collisions({});
    PassResult pass = evaluate_pass(in, base, reach, E, {}, {});
    std::cout << "evaluate_pass " << name << ": " << in.layers.size() << " layers, "
              << std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() << " s" << std::endl;
    return pass;
}

// Evaluates and commits the layers 1 … n − 1 (painted zones E, may be empty), as the planner does.
void commit_below(LayerEvaluator &ev, Reachability &reach, uint32_t n, const std::vector<ExPolygons> &E = {})
{
    const ExPolygons none;
    for (uint32_t k = 1; k < n; ++ k) {
        reach.ensure(reach.tip_layer(k), {});
        ev.evaluate(k, k < E.size() ? E[k] : none, {}, std::nullopt);
        ev.commit(k);
    }
    reach.ensure(reach.tip_layer(n), {});
}

bool same_needs(const LayerNeeds &a, const LayerNeeds &b)
{
    for (int c = 0; c < 3; ++ c)
        for (int l = 0; l < 3; ++ l)
            if (a.need[c][l] != b.need[c][l])
                return false;
    if (a.folded.size() != b.folded.size() || a.why.size() != b.why.size() || a.cprime != b.cprime || a.cuts.size() != b.cuts.size())
        return false;
    for (size_t i = 0; i < a.folded.size(); ++ i)
        if (a.folded[i] != b.folded[i])
            return false;
    for (size_t i = 0; i < a.why.size(); ++ i)
        if (a.why[i].first != b.why[i].first || a.why[i].second != b.why[i].second)
            return false;
    return a.stats.max_c == b.stats.max_c;
}

// Layer with the given print_z (± 1 µm); −1 if none.
int layer_at_print_z(const AnalysisInput &in, double z)
{
    for (size_t n = 0; n < in.layers.size(); ++ n)
        if (std::abs(in.layers[n].print_z - z) < 0.001)
            return int(n);
    return -1;
}

// Bridge between two pillars (case #5): pillars pillar × width × height, slab of the given thickness on top, span
// between the pillars' inner faces.
Parts bridge(double span, double width = 10., double pillar = 5., double height = 10., double thickness = 2.)
{
    const double x1 = pillar + span;
    return { box({ 0., 0., 0. }, { pillar, width, height }), box({ x1, 0., 0. }, { x1 + pillar, width, height }),
             box({ 0., 0., height }, { x1 + pillar, width, height + thickness }) };
}

} // namespace

TEST_CASE("Reference #1: shelf 2 mm has no needs, max c 1.8", "[SmartSupports][sas-eval]")
{
    const SasPrint     &sp   = cached_print("shelf_2x15", [] { return shelf_cube(2., 15.); }, sas_config());
    const AnalysisInput in   = input_of(sp);
    const BaseModel     base = build_base_model(in, {}, {});
    const PassResult    pass = run_pass("shelf 2 mm", in, base, manual_zones(base));
    REQUIRE(pass.layers.size() == in.layers.size());
    CHECK(total_need_area(pass) == 0.);
    CHECK(max_c(pass) == Approx(1.8).margin(0.1));

    SECTION("a 4 mm shelf needs a should band beyond 3.2 mm from the cube") {
        const SasPrint     &sp4   = cached_print("shelf_4x15", [] { return shelf_cube(4., 15.); }, sas_config());
        const AnalysisInput in4   = input_of(sp4);
        const BaseModel     base4 = build_base_model(in4, {}, {});
        const PassResult    pass4 = run_pass("shelf 4 mm", in4, base4, manual_zones(base4));
        const int n = first_need_layer(pass4, Cause::Cantilever, Level::Should);
        REQUIRE(n == layer_at_print_z(in4, 18.2));
        const LayerNeeds &ln = pass4.layers[size_t(n)];
        CHECK(ln.stats.max_c == Approx(3.8).margin(0.15));
        CHECK(need_area(ln, Cause::Cantilever, Level::Must) == 0.);
        CHECK(need_area(ln, Cause::Island) == 0.);
        CHECK(need_area(ln, Cause::LongSpan) == 0.);
        CHECK(mm2(ln.need[int(Cause::Cantilever)][int(Level::Should)]) == Approx(0.8 * 15.).epsilon(0.1));
        // Distance from L_{n−1} (the cube): c = d − w, the need starts at c = 3.
        const ExPolygons &Lp   = in4.layers[size_t(n) - 1].lslices;
        const ExPolygons &L    = in4.layers[size_t(n)].lslices;
        const ExPolygons &need = ln.need[int(Cause::Cantilever)][int(Level::Should)];
        CHECK(mm2(intersection_ex(need, offset(Lp, scaled<float>(3.1), ClipperLib::jtRound))) < 0.05);
        CHECK(mm2(diff_ex(diff_ex(L, offset(Lp, scaled<float>(3.3), ClipperLib::jtRound)), need)) < 0.05);
        // Nothing on the other layers.
        CHECK(total_need_area(pass4) == Approx(need_area(ln)));
    }
}

TEST_CASE("Reference #5: bridge 30 has no needs; B_max 20 gives a long span at may", "[SmartSupports][sas-eval]")
{
    const SasPrint &sp = cached_print("bridge_30", [] { return bridge(30.); }, sas_config());
    {
        const AnalysisInput in   = input_of(sp);
        const BaseModel     base = build_base_model(in, {}, {});
        const PassResult    pass = run_pass("bridge 30", in, base, manual_zones(base));
        CHECK(total_need_area(pass) == 0.);
    }
    // Discrimination: the same bridge with B_max = 20 (t = 29.6, t/B = 1.48 ≤ f_hard → may). Bending is covered
    // within 3 mm of both pillars, the rest takes the lower level: long span may.
    UserParams up = ref_params();
    up.B_max = 20.;
    const AnalysisInput in   = input_of(sp, up);
    const BaseModel     base = build_base_model(in, {}, {});
    const PassResult    pass = run_pass("bridge 30, B_max 20", in, base, manual_zones(base));
    const int n = layer_at_print_z(in, 10.2);
    REQUIRE(n > 0);
    const LayerNeeds &ln = pass.layers[size_t(n)];
    CHECK(mm2(ln.need[int(Cause::LongSpan)][int(Level::May)]) == Approx((29.6 - 6.) * 10.).epsilon(0.1));
    CHECK(need_area(ln, Cause::Cantilever) == 0.);
    CHECK(need_area(ln, Cause::LongSpan, Level::Should) == 0.);
    CHECK(total_need_area(pass) == Approx(need_area(ln)));

    // The cut candidates (cleared in a pass): one per scanline, interval [t − B, B] from the span start.
    Reachability reach(in);
    LayerEvaluator ev(in, base, reach, {});
    commit_below(ev, reach, uint32_t(n));
    const LayerNeeds &direct = ev.evaluate(uint32_t(n), {}, {}, std::nullopt);
    REQUIRE(! direct.cuts.empty());
    for (const TensionMap::Cut &cut : direct.cuts) {
        CHECK(cut.t == Approx(29.6).margin(0.05));
        CHECK(cut.lo == Approx(9.6).margin(0.05));
        CHECK(cut.hi == Approx(20.).margin(0.05));
    }
    CHECK(mm2(direct.cprime) == Approx(29.6 * 10.).epsilon(0.02));
}

TEST_CASE("Reference #14: wall 44° and wall 40° with H_s = 0 have no needs", "[SmartSupports][sas-eval]")
{
    {
        const SasPrint     &sp   = cached_print("slope_wall_44", [] { return slope_wall(44.); }, sas_config());
        const AnalysisInput in   = input_of(sp);
        const BaseModel     base = build_base_model(in, {}, {});
        const PassResult    pass = run_pass("wall 44°", in, base, manual_zones(base));
        CHECK(total_need_area(pass) == 0.);
        // The excess accumulates at 0.2·(1/tan 44° − 1) per layer: ≈ 1.78 mm after 50 mm (theory §10: 3 mm at ≈ 84 mm).
        CHECK(max_c(pass) == Approx(250. * 0.2 * (1. / std::tan(44. * M_PI / 180.) - 1.)).epsilon(0.1));
    }
    {
        UserParams up = ref_params();
        up.H_s = 0.;
        const SasPrint     &sp   = cached_print("slope_wall_40", [] { return slope_wall(40.); }, sas_config());
        const AnalysisInput in   = input_of(sp, up);
        const BaseModel     base = build_base_model(in, {}, {});
        const PassResult    pass = run_pass("wall 40°, H_s 0", in, base, manual_zones(base));
        CHECK(total_need_area(pass) == 0.);
        // Per-layer reach only: one step beyond w.
        CHECK(max_c(pass) == Approx(0.2 / std::tan(40. * M_PI / 180.) - 0.2).margin(0.01));
    }
}

TEST_CASE("Reference #13: wall 40°, 50 tall, first need at 15.8 mm", "[SmartSupports][sas-eval]")
{
    const SasPrint     &sp   = cached_print("slope_wall_40", [] { return slope_wall(40.); }, sas_config());
    const AnalysisInput in   = input_of(sp);
    const BaseModel     base = build_base_model(in, {}, {});
    const PassResult    pass = run_pass("wall 40°", in, base, manual_zones(base));
    const int first = first_need_layer(pass, Cause::Cantilever, Level::Should);
    REQUIRE(first > 0);
    const double z_first = in.layers[size_t(first)].print_z;
    std::cout << "wall 40°: first need at print_z " << z_first << ", max c " << max_c(pass) << std::endl;
    CHECK(z_first == Approx(15.8).margin(1.6));
    CHECK(z_first > 14.0);
    // Should first; must once c > f_hard·K = 4.5 (rate 0.2/tan 40° − 0.2 = 0.0384 per layer → ≈ 23.5 mm).
    CHECK(need_area(pass.layers[size_t(first)], Cause::Cantilever, Level::Must) == 0.);
    const int first_must = first_need_layer(pass, Cause::Cantilever, Level::Must);
    REQUIRE(first_must > first);
    CHECK(in.layers[size_t(first_must)].print_z == Approx(4.5 / (0.2 / std::tan(40. * M_PI / 180.) - 0.2) * 0.2).margin(2.));
    // A chain: every layer above the first need has a need (up to the last front layer), no islands, no long spans.
    int with_needs = 0, fronts = 0;
    for (size_t n = size_t(first); n < in.layers.size(); ++ n)
        if (! base.layers[n].fronts.empty()) {
            ++ fronts;
            with_needs += need_area(pass.layers[n]) > 0.;
        }
    CHECK(with_needs == fronts);
    for (const LayerNeeds &ln : pass.layers) {
        CHECK(need_area(ln, Cause::Island) == 0.);
        CHECK(need_area(ln, Cause::LongSpan) == 0.);
    }
}

TEST_CASE("Cone tail: tail needs exist and vanish with the tail painted", "[SmartSupports][sas-eval]")
{
    // Inverted 60° cone, apex at z 5, hanging from a plate (theory §10 "Cone tip pointing down"; plan #26).
    const SasPrint     &sp   = cached_print("inverted_cone_60", [] { return inverted_cone(60., 10., 5.); }, sas_config());
    const AnalysisInput in   = input_of(sp);
    const BaseModel     base = build_base_model(in, {}, {});
    REQUIRE(! base.trees.empty());
    const IslandTree &tree = base.trees.front();
    const double      apex = in.layers[tree.first].slice_z - 0.5 * in.layers[tree.first].height;
    const double      z_plate = apex + 10. - 0.5;     // the cone merges into the plate here

    const PassResult bare = run_pass("cone, no paint", in, base, manual_zones(base));
    double bare_island = 0.;
    int    top_island  = -1;
    for (size_t n = tree.first; n < in.layers.size() && in.layers[n].slice_z < z_plate; ++ n) {
        const double a = need_area(bare.layers[n], Cause::Island);
        bare_island += a;
        if (a > 0.)
            top_island = int(n);
        // Above the apex nothing but the tail is a need (60° flanks are self-supporting).
        CHECK(need_area(bare.layers[n], Cause::Cantilever) == 0.);
    }
    REQUIRE(top_island > 0);
    std::cout << "cone tail: island needs " << bare_island << " mm², top at slice_z − apex = "
              << in.layers[size_t(top_island)].slice_z - apex << std::endl;
    CHECK(bare_island > 1.);
    CHECK(in.layers[size_t(top_island)].slice_z - apex == Approx(2.6).margin(0.3));
    // The tail is a must of the island cause on every layer from the third layer (smaller rings are below A_min).
    for (size_t n = tree.first + 2; n <= size_t(top_island); ++ n)
        CHECK(mm2(bare.layers[n].need[int(Cause::Island)][int(Level::Must)]) > 0.);

    // Paint the tail zones: the contact plus the gap band resolve them (plan §3.7, review 5 B1).
    std::vector<ExPolygons> E = manual_zones(base);
    for (size_t n = 0; n < base.layers.size(); ++ n)
        for (const auto &[t, area] : base.layers[n].tail_needs)
            append(E[n], area);
    const PassResult painted = run_pass("cone, tail painted", in, base, E);
    double painted_island = 0.;
    for (size_t n = tree.first; n < in.layers.size() && in.layers[n].slice_z < z_plate; ++ n) {
        painted_island += need_area(painted.layers[n], Cause::Island);
        CHECK(need_area(painted.layers[n], Cause::Cantilever) == 0.);
    }
    std::cout << "cone tail painted: island needs " << painted_island << " mm²" << std::endl;
    CHECK(painted_island <= 0.05 * bare_island);
}

TEST_CASE("Islands: a floating sphere is one island; unplaced islands fold", "[SmartSupports][sas-eval]")
{
    // A sphere R 5 floating 5 mm above the bed next to a base block.
    const SasPrint &sp = cached_print("floating_sphere", [] {
        return Parts{ box({ 0., 0., 0. }, { 10., 10., 2. }), translate(its_make_sphere(5., M_PI / 45.), Vec3d(25., 5., 10.)) };
    }, sas_config());
    const AnalysisInput in   = input_of(sp);
    const BaseModel     base = build_base_model(in, {}, {});
    REQUIRE(base.trees.size() == 1);
    REQUIRE(base.starts.size() == 1);                // folding is per island start (theory §3.1, host test 1)
    const IslandTree &tree  = base.trees.front();
    const int32_t     start = base.starts.front().id;
    const uint32_t    first = tree.first;
    const uint32_t    upto  = first + 10;          // 2 mm of the lower hemisphere: fronts on every layer
    REQUIRE(upto < in.layers.size());

    Reachability   reach(in);
    LayerEvaluator plain(in, base, reach, {});
    LayerEvaluator folding(in, base, reach, {});
    commit_below(plain, reach, first);
    commit_below(folding, reach, first);

    // The first layer: the whole slice is an island need.
    const LayerNeeds at_first = plain.evaluate(first, {}, {}, std::nullopt);
    CHECK(mm2(at_first.need[int(Cause::Island)][int(Level::Must)]) == Approx(mm2(in.layers[first].lslices)).epsilon(0.02));
    CHECK(at_first.folded.empty());
    plain.commit(first);

    folding.evaluate(first, {}, {}, std::nullopt);
    folding.mark_tree_unplaced(tree.id);
    {
        // Folding applies to every evaluation after the call, also of the same layer.
        const LayerNeeds &again = folding.evaluate(first, {}, {}, std::nullopt);
        CHECK(need_area(again, Cause::Island) == 0.);
        REQUIRE(again.folded.size() == 1);
        CHECK(again.folded.front().first == start);
        CHECK(mm2(again.folded.front().second) == Approx(need_area(at_first, Cause::Island)).epsilon(1e-6));
    }
    folding.commit(first);

    double plain_total = 0., folded_total = 0.;
    for (uint32_t n = first + 1; n <= upto; ++ n) {
        reach.ensure(reach.tip_layer(n), {});
        const LayerNeeds a = plain.evaluate(n, {}, {}, std::nullopt);
        const LayerNeeds b = folding.evaluate(n, {}, {}, std::nullopt);
        plain.commit(n);
        folding.commit(n);
        // The material above the island stays ungrounded: its fronts are island needs, not cantilevers.
        CHECK(need_area(a, Cause::Cantilever) == 0.);
        CHECK(a.folded.empty());
        CHECK(need_area(b, Cause::Island) == 0.);
        double fb = 0.;
        for (const auto &[t, area] : b.folded) {
            CHECK(t == start);
            fb += mm2(area);
        }
        CHECK(fb == Approx(need_area(a, Cause::Island)).epsilon(1e-6));
        plain_total  += need_area(a, Cause::Island);
        folded_total += fb;
    }
    CHECK(plain_total > 10.);
    CHECK(folded_total == Approx(plain_total).epsilon(1e-6));
}

TEST_CASE("Chains: stairs, horizontal holes, dome rings", "[SmartSupports][sas-eval]")
{
    SECTION("stairs, treads 2.5: risers 0.4 chain from the 2nd step, risers 1.2 never") {
        // c = 2.3 per tread; the next tread inherits c·max(0, 1 − (Δz − h)/H_s) (theory §10: 2.3 + 0.8·2.3 = 4.14).
        const SasPrint     &sp   = cached_print("stairs_0.4", [] { return stairs(2.5, 0.4); }, sas_config());
        const AnalysisInput in   = input_of(sp);
        const BaseModel     base = build_base_model(in, {}, {});
        const PassResult    pass = run_pass("stairs 0.4", in, base, manual_zones(base));
        const int step2 = layer_at_print_z(in, 5.6);
        REQUIRE(step2 > 0);
        CHECK(first_need_layer(pass, Cause::Cantilever, Level::Should) == step2);
        CHECK(need_area(pass.layers[size_t(step2)], Cause::Cantilever, Level::Must) == 0.);
        CHECK(pass.layers[size_t(step2)].stats.max_c == Approx(2.3 + 0.8 * 2.3).epsilon(0.03));
        CHECK(pass.layers[size_t(layer_at_print_z(in, 5.2))].stats.max_c == Approx(2.3).epsilon(0.03));
        // Step 3 inherits 0.8·4.14: c = 5.6 > f_hard·K → must.
        CHECK(need_area(pass.layers[size_t(layer_at_print_z(in, 6.0))], Cause::Cantilever, Level::Must) > 0.);

        const SasPrint     &sp12   = cached_print("stairs_1.2", [] { return stairs(2.5, 1.2); }, sas_config());
        const AnalysisInput in12   = input_of(sp12);
        const BaseModel     base12 = build_base_model(in12, {}, {});
        const PassResult    pass12 = run_pass("stairs 1.2", in12, base12, manual_zones(base12));
        CHECK(total_need_area(pass12) == 0.);
        CHECK(max_c(pass12) == Approx(2.3).epsilon(0.03));
    }
    SECTION("horizontal holes Ø8 / Ø20: chains up to the last open layer, the closing gap is a bridge") {
        // Ceiling of radius R: c(x) = R√2 − x − √(R² − x²) (theory §10, review 1 M4). The last open layer (h 0.2) has the
        // half chord x_last = √(R² − (R − 0.1)²); the closing layer above is a short bridge (tension covered). So the
        // sliced maximum is c(x_last): 0.87 for Ø8, 2.83 for Ø20 (the theory's 1.7 / 4.1 are the limits x → 0).
        auto c_last = [](double R) {
            const double z = R - 0.1, x = std::sqrt(R * R - z * z);
            return R * std::sqrt(2.) - x - z;
        };
        const SasPrint     &sp8   = cached_print("hole_8", [] { return hole_block(8.); }, sas_config());
        const AnalysisInput in8   = input_of(sp8);
        const BaseModel     base8 = build_base_model(in8, {}, {});
        const PassResult    pass8 = run_pass("hole Ø8", in8, base8, manual_zones(base8));
        CHECK(total_need_area(pass8) == 0.);
        CHECK(max_c(pass8) == Approx(c_last(4.)).margin(0.1));

        const SasPrint     &sp20   = cached_print("hole_20", [] { return hole_block(20., 40., 20., 40.); }, sas_config());
        const AnalysisInput in20   = input_of(sp20);
        const BaseModel     base20 = build_base_model(in20, {}, {});
        const PassResult    pass20 = run_pass("hole Ø20", in20, base20, manual_zones(base20));
        CHECK(total_need_area(pass20) == 0.);
        CHECK(max_c(pass20) == Approx(c_last(10.)).margin(0.15));
    }
    SECTION("dome R 20 / 25: hoop bands cover all rings; B_ceil 0: first need at z 19.0, hole r 6.3") {
        const SasPrint &sp = cached_print("dome_20_25", [] { return dome(20., 25.); }, sas_config());
        {
            const AnalysisInput in   = input_of(sp);
            const BaseModel     base = build_base_model(in, {}, {});
            const PassResult    pass = run_pass("dome", in, base, manual_zones(base));
            CHECK(total_need_area(pass) == 0.);
        }
        UserParams up = ref_params();
        up.B_ceil = 0.;
        const AnalysisInput in   = input_of(sp, up);
        const BaseModel     base = build_base_model(in, {}, {});
        const PassResult    pass = run_pass("dome, B_ceil 0", in, base, manual_zones(base));
        const int n = first_need_layer(pass, Cause::Cantilever, Level::May);
        REQUIRE(n > 0);
        CHECK(first_need_layer(pass, Cause::Island, Level::May) < 0);
        CHECK(first_need_layer(pass, Cause::LongSpan, Level::May) < 0);
        CHECK(need_area(pass.layers[size_t(n)], Cause::Cantilever, Level::Must) == 0.);
        const double z = in.layers[size_t(n)].slice_z;
        REQUIRE(in.layers[size_t(n)].lslices.size() == 1);
        REQUIRE(in.layers[size_t(n)].lslices.front().holes.size() == 1);
        const double r_hole = std::sqrt(std::abs(in.layers[size_t(n)].lslices.front().holes.front().area()) / M_PI) * SCALING_FACTOR;
        std::cout << "dome, B_ceil 0: first need at slice_z " << z << ", hole r " << r_hole << ", max c " << max_c(pass) << std::endl;
        // Theory: c = 3 at hole r 6.3 (z 18.98); the slices are 0.2 mm apart (r 6.54 at z 18.9, 5.93 at z 19.1).
        CHECK(z == Approx(19.0).margin(0.3));
        CHECK(r_hole == Approx(6.3).margin(0.4));
        // c at the hole edge = R√2 − r − z (the continuous formula holds layer by layer).
        CHECK(pass.layers[size_t(n)].stats.max_c == Approx(20. * std::sqrt(2.) - r_hole - z).margin(0.15));
        CHECK(pass.layers[size_t(n) - 1].stats.max_c < 3.f);
    }
}

TEST_CASE("Ungrounded material: a slab resting on a floating box hangs from the post only", "[SmartSupports][sas-eval]")
{
    // Post 5 × 10 × 10 and a 10 × 10 box 10 mm away, a slab 25 × 10 × 2 on top of both. With the box on the bed the
    // gap is a 9.6 mm bridge (covered); with the box floating (an island, z 5..10) its top is ungrounded material:
    // no anchor, the lines run through it to the free end of the slab (t = ∞) and the gap is a cantilever from the post.
    auto parts = [](double box_z0) {
        return Parts{ box({ 0., 0., 0. }, { 5., 10., 10. }), box({ 15., 0., box_z0 }, { 25., 10., 10. }),
                      box({ 0., 0., 10. }, { 25., 10., 12. }) };
    };
    const SasPrint &grounded = cached_print("slab_on_box_grounded", [&] { return parts(0.); }, sas_config());
    const SasPrint &floating = cached_print("slab_on_box_floating", [&] { return parts(5.); }, sas_config());
    {
        const AnalysisInput in   = input_of(grounded);
        const BaseModel     base = build_base_model(in, {}, {});
        CHECK(base.trees.empty());
        const PassResult pass = run_pass("slab on a grounded box", in, base, manual_zones(base));
        CHECK(total_need_area(pass) == 0.);
    }
    const AnalysisInput in   = input_of(floating);
    const BaseModel     base = build_base_model(in, {}, {});
    REQUIRE(base.trees.size() == 1);
    const PassResult pass = run_pass("slab on a floating box", in, base, manual_zones(base));
    const int first = int(base.trees.front().first);
    const int slab  = layer_at_print_z(in, 10.2);
    REQUIRE(slab > first);
    // The box bottom is the island.
    CHECK(mm2(pass.layers[size_t(first)].need[int(Cause::Island)][int(Level::Must)]) == Approx(100.).epsilon(0.02));
    // The gap: c = d − w from the post side; should beyond 3.2, must beyond 4.7 mm from the post (gap 5.2 … 14.8).
    const LayerNeeds &ln = pass.layers[size_t(slab)];
    CHECK(need_area(ln, Cause::Island) == 0.);
    CHECK(need_area(ln, Cause::LongSpan) == 0.);
    CHECK(mm2(ln.need[int(Cause::Cantilever)][int(Level::Should)]) == Approx(1.5 * 10.).epsilon(0.1));
    CHECK(mm2(ln.need[int(Cause::Cantilever)][int(Level::Must)]) == Approx((14.8 - 9.7) * 10.).epsilon(0.1));
    CHECK(ln.stats.max_c == Approx(9.6).epsilon(0.05));
    // Nothing else in between (vertical box walls).
    CHECK(total_need_area(pass) == Approx(need_area(pass.layers[size_t(first)]) + need_area(ln)).epsilon(1e-9));
}

TEST_CASE("Touched clusters: a partial re-evaluation equals a full one", "[SmartSupports][sas-eval]")
{
    const SasPrint     &sp   = cached_print("t_shape_15", [] { return t_shape(); }, sas_config());
    const AnalysisInput in   = input_of(sp);
    const BaseModel     base = build_base_model(in, {}, {});
    const int n = layer_at_print_z(in, 18.2);
    REQUIRE(n > 0);
    const LayerGeom &lg = base.layers[size_t(n)];
    REQUIRE(lg.clusters.size() == 2);                 // one per arm

    // A zone under the outer 5 mm of the second arm.
    const FrontGeom &arm = lg.fronts[lg.clusters[1].fronts.front()];
    BoundingBox zone_bb = arm.bbox;
    if (arm.bbox.min.x() > lg.fronts[lg.clusters[0].fronts.front()].bbox.min.x())
        zone_bb.min.x() = zone_bb.max.x() - scaled<coord_t>(5.);
    else
        zone_bb.max.x() = zone_bb.min.x() + scaled<coord_t>(5.);
    const ExPolygons E2 { ExPolygon(zone_bb.polygon()) };

    Reachability reach(in);
    LayerEvaluator ev(in, base, reach, {});
    commit_below(ev, reach, uint32_t(n));
    const LayerNeeds L1 = ev.evaluate(uint32_t(n), {}, {}, std::nullopt);
    const ExPolygons F2 = reach.predict_F(E2, uint32_t(n));
    REQUIRE(! F2.empty());
    const BoundingBox touched = get_extents(F2);      // F1 = ∅
    const auto t0 = std::chrono::steady_clock::now();
    const LayerNeeds L2 = ev.evaluate(uint32_t(n), E2, {}, touched);
    const auto t1 = std::chrono::steady_clock::now();
    const LayerNeeds L1b = ev.evaluate(uint32_t(n), {}, {}, touched);        // restore: from the cache
    const auto t2 = std::chrono::steady_clock::now();
    std::cout << "touched: trial " << std::chrono::duration<double>(t1 - t0).count() << " s, restore "
              << std::chrono::duration<double>(t2 - t1).count() << " s" << std::endl;

    LayerEvaluator fresh(in, base, reach, {});
    commit_below(fresh, reach, uint32_t(n));
    const LayerNeeds L2_full = fresh.evaluate(uint32_t(n), E2, {}, std::nullopt);
    CHECK(same_needs(L2, L2_full));
    CHECK(same_needs(L1b, L1));
    // The paint changes the second arm only: fewer cantilever needs there, the first arm untouched.
    CHECK(need_area(L1, Cause::Cantilever) > 0.);
    CHECK(need_area(L2, Cause::Cantilever) < need_area(L1, Cause::Cantilever) - 5.);
    const Polygons first_win { lg.clusters[0].win.polygon() };
    const ExPolygons first1 = intersection_ex(need_union(L1), first_win);
    const ExPolygons first2 = intersection_ex(need_union(L2), first_win);
    CHECK(mm2(first1) > 10.);
    CHECK(first1 == first2);
}

TEST_CASE("Cause overrides keep the cause of an accepted placement", "[SmartSupports][sas-eval]")
{
    const SasPrint     &sp   = cached_print("shelf_4x15", [] { return shelf_cube(4., 15.); }, sas_config());
    const AnalysisInput in   = input_of(sp);
    const BaseModel     base = build_base_model(in, {}, {});
    const int n = layer_at_print_z(in, 18.2);
    REQUIRE(n > 0);
    Reachability   reach(in);
    LayerEvaluator ev(in, base, reach, {});
    commit_below(ev, reach, uint32_t(n));
    const LayerNeeds plain = ev.evaluate(uint32_t(n), {}, {}, std::nullopt);
    REQUIRE(need_area(plain, Cause::Cantilever) > 1.);
    const LayerNeeds moved = ev.evaluate(uint32_t(n), {}, { { in.layers[size_t(n)].lslices, Cause::LongSpan } }, std::nullopt);
    CHECK(need_area(moved, Cause::Cantilever) == 0.);
    CHECK(mm2(moved.need[int(Cause::LongSpan)][int(Level::Should)]) ==
          Approx(mm2(plain.need[int(Cause::Cantilever)][int(Level::Should)])).epsilon(1e-6));
    // An override elsewhere changes nothing.
    const ExPolygons elsewhere { ExPolygon(BoundingBox(Point(scaled<coord_t>(500.), 0), Point(scaled<coord_t>(510.), scaled<coord_t>(10.))).polygon()) };
    CHECK(same_needs(ev.evaluate(uint32_t(n), {}, { { elsewhere, Cause::Island } }, std::nullopt), plain));
}

TEST_CASE("Known reasons: NoTipLayer below the first support layer, InterfaceGap with roofs", "[SmartSupports][sas-eval]")
{
    SECTION("NoTipLayer") {
        // A 10 mm overhang on the second layer: tips are impossible there (tip_layer(1) < 1).
        const SasPrint &sp = cached_print("low_slab", [] {
            return Parts{ box({ 0., 0., 0. }, { 10., 10., 0.2 }), box({ 0., 0., 0.2 }, { 20., 10., 0.6 }) };
        }, sas_config());
        const AnalysisInput in   = input_of(sp);
        const BaseModel     base = build_base_model(in, {}, {});
        const PassResult    pass = run_pass("low slab", in, base, manual_zones(base));
        REQUIRE(pass.layers.size() >= 2);
        const LayerNeeds &ln = pass.layers[1];
        REQUIRE(need_area(ln, Cause::Cantilever, Level::Must) > 10.);
        REQUIRE(ln.why.size() == 1);
        CHECK(ln.why.front().second == Why::NoTipLayer);
        CHECK(mm2(ln.why.front().first) == Approx(need_area(ln)).epsilon(1e-6));
    }
    SECTION("InterfaceGap") {
        // 40° wall: paint the first need layer. Interface 0: the tip contact + gap band cover the front. Interface 2:
        // the roof keeps the contact 0.5175 mm away, the needs stay and are attributed to the roof gap (D30).
        const SasPrint     &sp0  = cached_print("slope_wall_40", [] { return slope_wall(40.); }, sas_config());
        const AnalysisInput in0  = input_of(sp0);
        const BaseModel     base0 = build_base_model(in0, {}, {});
        const PassResult    bare = run_pass("wall 40°", in0, base0, manual_zones(base0));
        const int k = first_need_layer(bare, Cause::Cantilever, Level::Should) + 5;
        REQUIRE(k > 5);
        const LayerNeeds &bare_k = bare.layers[size_t(k)];
        REQUIRE(need_area(bare_k) > 0.3);
        CHECK(bare_k.why.empty());
        std::vector<ExPolygons> E = manual_zones(base0);
        E[size_t(k)] = need_union(bare_k);

        auto at_k = [&](const SasPrint &sp, const char *name) {
            const AnalysisInput in   = input_of(sp);
            const BaseModel     base = build_base_model(in, {}, {});
            Reachability   reach(in);
            LayerEvaluator ev(in, base, reach, {});
            commit_below(ev, reach, uint32_t(k));
            const auto t0 = std::chrono::steady_clock::now();
            LayerNeeds out = ev.evaluate(uint32_t(k), E[size_t(k)], {}, std::nullopt);
            std::cout << name << ": evaluate at the painted layer " << std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count()
                      << " s, need " << need_area(out) << " mm²" << std::endl;
            return out;
        };
        const LayerNeeds i0 = at_k(sp0, "wall 40°, interface 0");
        CHECK(need_area(i0) <= 0.1 * need_area(bare_k));
        const SasPrint &sp2 = cached_print("slope_wall_40", [] { return slope_wall(40.); },
                                           sas_config({ { "support_material_interface_layers", 2 } }));
        const LayerNeeds i2 = at_k(sp2, "wall 40°, interface 2");
        CHECK(need_area(i2) >= 0.9 * need_area(bare_k));
        REQUIRE(i2.why.size() == 1);
        CHECK(i2.why.front().second == Why::InterfaceGap);
        CHECK(mm2(i2.why.front().first) >= 0.9 * need_area(i2));
    }
}

TEST_CASE("evaluate_pass is deterministic and cancelable", "[SmartSupports][sas-eval]")
{
    // Sphere R 20 on the bed (theory §10): ring fronts with Grid fields, a must band in their outer part.
    const SasPrint &sp = cached_print("sphere_20", [] { return Parts{ translate(its_make_sphere(20., M_PI / 90.), Vec3d(0., 0., 20.)) }; },
                                      sas_config());
    const AnalysisInput in   = input_of(sp);
    const BaseModel     base = build_base_model(in, {}, {});
    const PassResult    multi = run_pass("sphere R 20", in, base, manual_zones(base));
    PassResult single;
    {
        tbb::global_control one(tbb::global_control::max_allowed_parallelism, 1);
        single = run_pass("sphere R 20, 1 thread", in, base, manual_zones(base));
    }
    REQUIRE(single.layers.size() == multi.layers.size());
    bool equal = true;
    for (size_t n = 0; n < multi.layers.size(); ++ n)
        equal &= same_needs(single.layers[n], multi.layers[n]) && single.layers[n].stats.need_area[1][2] == multi.layers[n].stats.need_area[1][2];
    CHECK(equal);
    CHECK(total_need_area(multi) > 10.);       // the shallow rings need support (theory §10 "Sphere R 20 on the bed")
    CHECK(max_c(multi) == Approx(6.4).epsilon(0.15));
    // The needs form a band from ≈ 5.8 mm radius to the 45° slope at ≈ 14.1 mm (theory §10), must in its outer part.
    {
        const Point  centre = get_extents(in.layers[in.layers.size() / 2].lslices).center();
        double       r_min = 1e9, r_max = 0., r_must_min = 1e9;
        for (const LayerNeeds &ln : multi.layers)
            for (int l = 0; l < 3; ++ l)
                for (const ExPolygon &ex : ln.need[int(Cause::Cantilever)][l])
                    for (const Point &p : ex.contour.points) {
                        const double r = (p - centre).cast<double>().norm() * SCALING_FACTOR;
                        r_min = std::min(r_min, r);
                        r_max = std::max(r_max, r);
                        if (l == int(Level::Must))
                            r_must_min = std::min(r_must_min, r);
                    }
        std::cout << "sphere R 20: needs between r " << r_min << " and " << r_max << ", must from r " << r_must_min << std::endl;
        CHECK(r_min == Approx(5.8).margin(0.6));
        CHECK(r_max == Approx(14.1).margin(0.6));
        CHECK(r_must_min > r_min + 0.5);
    }

    // Cancel after a few evaluations: CanceledException, no hang.
    std::atomic<int> calls { 0 };
    Reachability reach(in);
    CHECK_THROWS_AS(evaluate_pass(in, base, reach, manual_zones(base), [&calls] { if (++ calls > 40) throw CanceledException(); }, {}),
                    CanceledException);
    // Progress is monotone and ends at 100.
    std::vector<int> pct;
    Reachability reach2(in);
    evaluate_pass(in, base, reach2, manual_zones(base), {}, [&pct](int p) { pct.push_back(p); });
    REQUIRE(! pct.empty());
    CHECK(std::is_sorted(pct.begin(), pct.end()));
    CHECK(pct.back() == 100);
}


TEST_CASE("evaluate_pass timings", "[.benchmark][sas-eval-bench]")
{
    struct Shape { const char *name; std::function<Parts()> parts; };
    const std::vector<Shape> shapes {
        { "figurine_proxy", [] { return figurine_proxy(); } },
        { "t_shape_on_80x80", [] { return t_shape(15., 10., 10., 20., 2., 80.); } },
        { "hole_50", [] { return hole_block(50., 70., 20., 70.); } },
        { "dome_20_25", [] { return dome(20., 25.); } },
        { "shelf_10x30", [] { return shelf_cube(10., 30., 30.); } },
    };
    for (const Shape &shape : shapes) {
        const SasPrint     &sp   = cached_print(shape.name, shape.parts, sas_config());
        const AnalysisInput in   = input_of(sp);
        const auto          t0   = std::chrono::steady_clock::now();
        const BaseModel     base = build_base_model(in, {}, {});
        const auto          t1   = std::chrono::steady_clock::now();
        const PassResult    pass = run_pass(shape.name, in, base, manual_zones(base));
        size_t fronts = 0, clusters = 0;
        for (const LayerGeom &lg : base.layers) {
            fronts   += lg.fronts.size();
            clusters += lg.clusters.size();
        }
        std::cout << shape.name << ": Phase A " << std::chrono::duration<double>(t1 - t0).count() << " s, " << fronts << " fronts, "
                  << clusters << " clusters, needs " << total_need_area(pass) << " mm², max c " << max_c(pass) << std::endl;

        // Largest wide front: domain edges + tension alone (LinesDistancer<Line> queries, WP2b1 note).
        const FrontGeom *big = nullptr;
        const Cluster   *big_cl = nullptr;
        for (const LayerGeom &lg : base.layers)
            for (const Cluster &cl : lg.clusters)
                for (uint32_t idx : cl.fronts)
                    if (lg.fronts[idx].wide && (! big || lg.fronts[idx].area_mm2 > big->area_mm2)) {
                        big    = &lg.fronts[idx];
                        big_cl = &cl;
                    }
        if (big) {
            const ExPolygons GP = diff_ex(big_cl->band, ExPolygons{ big->C });
            const ExPolygons D { big->C };
            const Anchors    A = compute_anchors(D, big->C, GP, {}, big_cl->band);
            const auto       u0 = std::chrono::steady_clock::now();
            const DomainEdges e = domain_edges(D, GP);
            const auto       u1 = std::chrono::steady_clock::now();
            const TensionMap tm = tension_levels(D, *big, D, e, A, in.params);
            const auto       u2 = std::chrono::steady_clock::now();
            std::cout << "  largest wide front " << big->area_mm2 << " mm², " << big->thetas.size() << " θ: domain_edges "
                      << std::chrono::duration<double>(u1 - u0).count() << " s, tension_levels "
                      << std::chrono::duration<double>(u2 - u1).count() << " s, " << tm.cuts.size() << " cuts" << std::endl;
        }
    }
}
