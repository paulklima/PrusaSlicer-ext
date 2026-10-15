// Smart support painting: anchors, domain edges, tension (ties, cuts), chord rule, hoop credit, classification
// (plan §7.1; WP2b2). All inputs are synthetic: band-local regions built from rectangles and a synthetic ReachQuery.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <functional>
#include <limits>

#include "smart_supports_meshes.hpp"
#include "libslic3r/Support/SmartSupportsInternal.hpp"

using namespace Slic3r;
using namespace Slic3r::SmartSupports;
using namespace Slic3r::SmartSupports::Testing;
using Catch::Approx;

namespace {

// c(p) = f(p in mm).
struct ReachFn final : ReachQuery {
    explicit ReachFn(std::function<float(const Vec2d&)> f) : f(std::move(f)) {}
    float   value(const Point &p) const override { return f(unscale(p)); }
    coord_t res() const override { return scaled<coord_t>(0.1); }
    bool    direct_mode() const override { return true; }
    std::function<float(const Vec2d&)> f;
};

double mm2(const ExPolygons &ex) { return area(ex) * SCALING_FACTOR * SCALING_FACTOR; }
double mm(const Polylines &pls) { return total_length(pls) * SCALING_FACTOR; }

ExPolygons U(std::initializer_list<ExPolygon> parts) { return union_ex(ExPolygons(parts)); }

// Area of the regions of one tension level.
double level_area(const TensionMap &tm, TLevel level)
{
    double a = 0.;
    for (const auto &[region, l] : tm.regions)
        if (l == level)
            a += mm2(region);
    return a;
}

TensionMap uniform_tension(const ExPolygons &region, TLevel level)
{
    TensionMap tm;
    tm.regions.emplace_back(region, level);
    if (level == TLevel::Covered)
        tm.covered_all = region;
    return tm;
}

// One band-local situation as the evaluator sees it (no tolerated material): C' = C − F, D = L − GP − F.
struct Scene {
    ExPolygons L, GP, F;
    ExPolygon  C;
    FrontGeom  front;
    ExPolygons cprime, D;
    Anchors    anchors;
    DomainEdges edges;
    TensionMap tension;

    void evaluate(const Params &params)
    {
        front.C = C;
        cprime  = diff_ex(C, F);
        D       = diff_ex(diff_ex(L, GP), F);
        ExPolygons stop = GP;
        append(stop, F);
        anchors = compute_anchors(cprime, C, GP, F, L);
        edges   = domain_edges(D, union_ex(stop));
        tension = tension_levels(cprime, front, D, edges, anchors, params);
    }
};

// Wide bridge [0, len] × [0, 10] between towers; right tower optional (one free end).
Scene bridge(double len, bool right_tower = true)
{
    Scene s;
    s.C  = rectangle_mm(0., 0., len, 10.);
    s.GP = { rectangle_mm(-5., 0., 0., 10.) };
    if (right_tower)
        s.GP.push_back(rectangle_mm(len, 0., len + 5., 10.));
    ExPolygons L = s.GP;
    L.push_back(s.C);
    s.L                 = union_ex(L);
    s.front.wide        = true;
    s.front.thetas      = { Vec2d(1., 0.) };
    return s;
}

// Thin front of a lean-to ceiling: strip [x0, x1] × [0, 0.15] under a ceiling slab, anchored along its bottom side;
// walls (material) left of x0 − 0.2 and right of x1 + 0.2 if present.
Scene thin_strip(double x0, double x1, bool left_wall, bool right_wall)
{
    Scene s;
    s.C = rectangle_mm(x0, 0., x1, 0.15);
    ExPolygons L { rectangle_mm(left_wall ? x0 - 0.2 : x0, -3., right_wall ? x1 + 0.2 : x1, 0.15) };
    if (left_wall)
        L.push_back(rectangle_mm(x0 - 2.2, -3., x0 - 0.2, 10.));
    if (right_wall)
        L.push_back(rectangle_mm(x1 + 0.2, -3., x1 + 2.2, 10.));
    s.L            = union_ex(L);
    s.GP           = diff_ex(s.L, ExPolygons{ s.C });
    s.front.wide   = false;
    return s;
}

} // namespace

TEST_CASE("compute_anchors: material, support, free and neither edges", "[SmartSupports][sas-tension]")
{
    // C = [0, 10] × [0, 5]: grounded material on the left, support contact on the right, open air below,
    // ungrounded material above (neither).
    const ExPolygon  C       = rectangle_mm(0., 0., 10., 5.);
    const ExPolygons GP      { rectangle_mm(-2., 0., 0., 5.) };
    const ExPolygons F       { rectangle_mm(10., 0., 12., 5.) };
    const ExPolygons L       = U({ C, GP.front(), rectangle_mm(0., 5., 10., 6.) });
    const ExPolygons cprime  = diff_ex(C, F);

    SECTION("classification") {
        const Anchors a = compute_anchors(cprime, C, GP, F, L);
        CHECK(mm(a.material) == Approx(5.).margin(0.01));
        CHECK(mm(a.support) == Approx(5.).margin(0.01));
        CHECK(mm(a.free_edges) == Approx(10.).margin(0.01));
        // Canonical: open polylines run first < last.
        for (const Polylines *pls : { &a.material, &a.support, &a.free_edges })
            for (const Polyline &pl : *pls)
                CHECK(! (pl.points.back() < pl.points.front()));
        REQUIRE(a.material.size() == 1);
        CHECK(unscale<double>(a.material.front().first_point().x()) == Approx(0.).margin(0.01));
    }
    SECTION("support wins where the grounded material contains the contact") {
        ExPolygons GP2 = GP;
        GP2.push_back(F.front());
        const Anchors a = compute_anchors(cprime, C, GP2, F, union_ex(ExPolygons{ L.front(), F.front() }));
        CHECK(mm(a.material) == Approx(5.).margin(0.01));
        CHECK(mm(a.support) == Approx(5.).margin(0.01));
    }
    SECTION("whole contour anchored: one closed loop starting at its smallest point") {
        const ExPolygons around = diff_ex(ExPolygons{ rectangle_mm(-1., -1., 11., 6.) }, ExPolygons{ C });
        const Anchors    a      = compute_anchors(ExPolygons{ C }, C, around, {}, U({ rectangle_mm(-1., -1., 11., 6.) }));
        REQUIRE(a.material.size() == 1);
        CHECK(a.material.front().points.front() == a.material.front().points.back());
        CHECK(a.material.front().points.front() == C.contour.points.front());
        CHECK(a.free_edges.empty());
        CHECK(a.support.empty());
    }
    SECTION("empty C'") {
        const Anchors a = compute_anchors({}, C, GP, F, L);
        CHECK(a.material.empty());
        CHECK(a.free_edges.empty());
    }
}

TEST_CASE("domain_edges: anchor and free edges of a D component", "[SmartSupports][sas-tension]")
{
    const ExPolygons  D { rectangle_mm(0., 0., 10., 5.) };
    const DomainEdges e = domain_edges(D, { rectangle_mm(-2., 0., 0., 5.) });
    REQUIRE(e.anchor != nullptr);
    REQUIRE(e.free_ != nullptr);
    CHECK(unscale<double>(e.anchor->distance_from_lines<false>(Point(0, scaled<coord_t>(2.5)))) == Approx(0.).margin(1e-3));
    CHECK(unscale<double>(e.anchor->distance_from_lines<false>(Point(scaled<coord_t>(10.), scaled<coord_t>(2.5)))) == Approx(10.).margin(1e-3));
    CHECK(unscale<double>(e.free_->distance_from_lines<false>(Point(scaled<coord_t>(10.), scaled<coord_t>(2.5)))) == Approx(0.).margin(1e-3));
    CHECK(unscale<double>(e.free_->distance_from_lines<false>(Point(scaled<coord_t>(5.), 0))) == Approx(0.).margin(1e-3));

    const DomainEdges empty = domain_edges({}, {});
    REQUIRE(empty.anchor != nullptr);
    CHECK(std::isinf(empty.anchor->distance_from_lines<false>(Point(0, 0))));
}

TEST_CASE("tension: bridge anchored at both ends", "[SmartSupports][sas-tension]")
{
    const Params params = test_params();             // B_max 40, f_hard 1.5
    struct Case { double len; TLevel level; std::vector<std::pair<double, double>> cuts; };
    for (const Case &c : std::vector<Case>{ { 30., TLevel::Covered, {} },
                                            { 50., TLevel::May, { { 10., 40. } } },
                                            { 70., TLevel::Should, { { 30., 40. } } },
                                            { 80., TLevel::Should, { { 40., 40. } } },
                                            { 90., TLevel::Should, { { 10., 40. }, { 50., 80. } } },   // t/B = f² exactly
                                            { 100., TLevel::Must, { { 20., 40. }, { 60., 80. } } } }) {
        CAPTURE(c.len);
        Scene s = bridge(c.len);
        s.evaluate(params);
        REQUIRE(s.tension.regions.size() == 1);
        CHECK(s.tension.regions.front().second == c.level);
        CHECK(level_area(s.tension, c.level) == Approx(c.len * 10.).epsilon(1e-6));
        CHECK(mm2(s.tension.covered_all) == Approx(c.level == TLevel::Covered ? c.len * 10. : 0.).margin(1e-6));
        // One cut set per scanline (spacing 0.5·ext_w): ≈ 10 / 0.225 lines.
        const size_t lines = c.cuts.empty() ? 0 : s.tension.cuts.size() / c.cuts.size();
        CHECK(s.tension.cuts.size() == lines * c.cuts.size());
        if (! c.cuts.empty())
            CHECK((lines >= 44 && lines <= 45));
        for (size_t i = 0; i < s.tension.cuts.size(); ++ i) {
            const TensionMap::Cut &cut = s.tension.cuts[i];
            CHECK(cut.t == Approx(c.len).margin(1e-3));
            CHECK(cut.lo == Approx(c.cuts[i % c.cuts.size()].first).margin(1e-3));
            CHECK(cut.hi == Approx(c.cuts[i % c.cuts.size()].second).margin(1e-3));
            CHECK(cut.dir.x() == Approx(1.));
            CHECK(unscale<double>(cut.p.x()) == Approx(0.).margin(1e-3));
        }
    }
}

TEST_CASE("tension: one free end gives no finite span", "[SmartSupports][sas-tension]")
{
    Scene s = bridge(30., false);
    s.evaluate(test_params());
    CHECK(level_area(s.tension, TLevel::None) == Approx(300.).epsilon(1e-6));
    CHECK(s.tension.covered_all.empty());
    CHECK(s.tension.cuts.empty());

    SECTION("no bridge direction") {
        Scene s2 = bridge(30.);
        s2.front.thetas.clear();
        s2.evaluate(test_params());
        CHECK(level_area(s2.tension, TLevel::None) == Approx(300.).epsilon(1e-6));
    }
}

TEST_CASE("tension: support contact and ungrounded material", "[SmartSupports][sas-tension]")
{
    const Params params = test_params();
    SECTION("a support contact across the span is an anchor (levels only fall, P4)") {
        Scene s = bridge(90.);
        s.F     = { rectangle_mm(44., -1., 46., 11.) };
        s.evaluate(params);
        CHECK(level_area(s.tension, TLevel::May) == Approx(880.).epsilon(1e-3));
        CHECK(level_area(s.tension, TLevel::Should) == 0.);
        REQUIRE(! s.tension.cuts.empty());
        for (const TensionMap::Cut &cut : s.tension.cuts) {
            CHECK(cut.t == Approx(44.).margin(1e-3));
            CHECK(cut.lo == Approx(4.).margin(1e-3));
            CHECK(cut.hi == Approx(40.).margin(1e-3));
        }
    }
    SECTION("spans run through ungrounded material (a tolerated logo below the bridge)") {
        Scene s = bridge(30.);
        s.evaluate(params);
        // The logo is part of D but not of C'.
        const ExPolygons logo { rectangle_mm(10., 4., 12., 6.) };
        s.cprime  = diff_ex(ExPolygons{ s.C }, logo);
        s.tension = tension_levels(s.cprime, s.front, s.D, s.edges, compute_anchors(s.cprime, s.C, s.GP, {}, s.L), params);
        CHECK(mm2(s.tension.covered_all) == Approx(296.).epsilon(1e-6));
    }
}

TEST_CASE("tension: rotated and slanted bridges", "[SmartSupports][sas-tension]")
{
    const Params params = test_params();
    SECTION("rotated by 30°") {
        Scene s = bridge(30.);
        const double a = M_PI / 6.;
        s.C.rotate(a);
        for (ExPolygons *ex : { &s.GP, &s.L })
            for (ExPolygon &e : *ex)
                e.rotate(a);
        s.front.thetas = { Vec2d(std::cos(a), std::sin(a)) };
        s.evaluate(params);
        CHECK(mm2(s.tension.covered_all) == Approx(300.).epsilon(1e-3));
    }
    SECTION("parallelogram between slanted tower faces (63°)") {
        Scene s;
        s.C  = ExPolygon(polygon_mm({ { 0., 0. }, { 30., 0. }, { 35., 10. }, { 5., 10. } }));
        s.GP = { ExPolygon(polygon_mm({ { -5., 0. }, { 0., 0. }, { 5., 10. }, { 0., 10. } })),
                 ExPolygon(polygon_mm({ { 30., 0. }, { 35., 0. }, { 40., 10. }, { 35., 10. } })) };
        s.L  = U({ s.C, s.GP[0], s.GP[1] });
        s.front.wide   = true;
        s.front.thetas = { Vec2d(1., 0.) };
        s.evaluate(params);
        CHECK(mm2(s.tension.covered_all) == Approx(300.).epsilon(1e-3));
    }
}

TEST_CASE("tension: tied directions, worst level and covered_all", "[SmartSupports][sas-tension]")
{
    const Params params = test_params();
    SECTION("covered under one θ only where both θ have anchored spans") {
        // [0, 30]²: walls left, right and below; above only x ∈ [15, 30] is anchored.
        Scene s;
        s.C  = rectangle_mm(0., 0., 30., 30.);
        s.GP = { rectangle_mm(-2., -2., 0., 30.), rectangle_mm(30., -2., 32., 32.), rectangle_mm(0., -2., 30., 0.),
                 rectangle_mm(15., 30., 30., 32.) };
        s.GP = union_ex(s.GP);
        ExPolygons L = s.GP;
        L.push_back(s.C);
        s.L = union_ex(L);
        s.front.wide   = true;
        s.front.thetas = { Vec2d(1., 0.), Vec2d(0., 1.) };
        s.evaluate(params);
        CHECK(mm2(s.tension.covered_all) == Approx(450.).margin(10.));
        CHECK(level_area(s.tension, TLevel::None) == Approx(450.).margin(10.));
        // Lines along x only: covered everywhere.
        s.front.thetas = { Vec2d(1., 0.) };
        s.evaluate(params);
        CHECK(mm2(s.tension.covered_all) == Approx(900.).epsilon(1e-6));
    }
    SECTION("the worse direction sets the level") {
        // Pocket 30 × 50, anchored all around: 30 covered along x, 50 may along y.
        Scene s;
        s.C            = rectangle_mm(0., 0., 30., 50.);
        s.L            = { rectangle_mm(-2., -2., 32., 52.) };
        s.GP           = diff_ex(s.L, ExPolygons{ s.C });
        s.front.wide   = true;
        s.front.thetas = { Vec2d(1., 0.), Vec2d(0., -1.) };
        s.evaluate(params);
        CHECK(s.tension.covered_all.empty());
        CHECK(level_area(s.tension, TLevel::May) == Approx(1500.).epsilon(1e-3));
        // Cuts of the long direction only.
        REQUIRE(! s.tension.cuts.empty());
        for (const TensionMap::Cut &cut : s.tension.cuts)
            CHECK(cut.t == Approx(50.).margin(1e-3));
    }
}

TEST_CASE("tension: chord rule for thin fronts", "[SmartSupports][sas-tension]")
{
    const Params params = test_params();
    SECTION("lean-to between two side walls 30 apart") {
        Scene s = thin_strip(0.2, 29.8, true, true);
        s.evaluate(params);
        CHECK(mm2(s.tension.covered_all) == Approx(29.6 * 0.15).epsilon(1e-3));
        CHECK(s.tension.cuts.empty());
    }
    SECTION("lean-to between two side walls 50 apart: long span, may") {
        Scene s = thin_strip(0.2, 49.8, true, true);
        s.evaluate(params);
        CHECK(level_area(s.tension, TLevel::May) == Approx(49.6 * 0.15).epsilon(1e-3));
        REQUIRE(s.tension.cuts.size() == 1);
        CHECK(s.tension.cuts.front().lo == Approx(9.6).margin(0.01));
        CHECK(s.tension.cuts.front().hi == Approx(40.).margin(1e-3));
    }
    SECTION("lean-to with one open end") {
        Scene s = thin_strip(0.2, 30., true, false);
        s.evaluate(params);
        CHECK(level_area(s.tension, TLevel::None) == Approx(29.8 * 0.15).epsilon(1e-3));
        CHECK(s.tension.cuts.empty());
    }
    SECTION("hole strip: anchored only along its long side") {
        Scene s = thin_strip(0., 20., false, false);
        s.evaluate(params);
        REQUIRE(s.anchors.free_edges.size() == 1);   // U-shaped, both ends on the long anchor
        CHECK(level_area(s.tension, TLevel::None) == Approx(20. * 0.15).epsilon(1e-3));
    }
    SECTION("thin beam between two towers, both long sides free") {
        Scene s;
        s.C  = rectangle_mm(0., 0., 10., 0.5);
        s.GP = { rectangle_mm(-2., -2., 0., 2.), rectangle_mm(10., -2., 12., 2.) };
        s.L  = U({ s.C, s.GP[0], s.GP[1] });
        s.evaluate(params);
        REQUIRE(s.anchors.free_edges.size() == 2);
        CHECK(mm2(s.tension.covered_all) == Approx(5.).epsilon(1e-3));
    }
    SECTION("curved free edge between two walls") {
        // Quarter ring r ∈ [9.85, 10] hanging from the material inside, ends at walls.
        const ExPolygons quadrant { rectangle_mm(0., 0., 11., 11.) };
        const ExPolygons disk     = intersection_ex(ExPolygons{ ExPolygon(circle_mm(Vec2d::Zero(), 10., 512)) }, quadrant);
        const ExPolygons inner    = intersection_ex(ExPolygons{ ExPolygon(circle_mm(Vec2d::Zero(), 9.85, 512)) }, quadrant);
        const ExPolygons ring     = diff_ex(disk, inner);
        REQUIRE(ring.size() == 1);
        Scene s;
        s.C  = ring.front();
        s.L  = U({ disk.front(), rectangle_mm(9., -1., 11., 0.), rectangle_mm(-1., 9., 0., 11.) });
        s.GP = diff_ex(s.L, ring);
        s.evaluate(params);
        REQUIRE(s.anchors.free_edges.size() == 1);
        CHECK(level_area(s.tension, TLevel::None) == Approx(mm2(ring)).epsilon(1e-3));
    }
}

TEST_CASE("hoop credit band", "[SmartSupports][sas-tension]")
{
    Params params = test_params();                   // B_max 40, B_ceil 40, hoop_width_factor 0.5
    FrontGeom front;
    front.C = ExPolygon(circle_mm(Vec2d::Zero(), 40., 256));
    Polygon hole = circle_mm(Vec2d::Zero(), 10., 256);
    hole.reverse();
    front.C.holes.push_back(hole);
    front.hoop_hole     = 0;
    front.hoop_feret_mm = 20.;
    const ExPolygons cprime { front.C };
    const double     ring_20 = M_PI * (30. * 30. - 10. * 10.);

    CHECK(mm2(hoop_credit(cprime, front, true, params)) == Approx(ring_20).epsilon(0.01));
    CHECK(hoop_credit(cprime, front, false, params).empty());              // no grounded anchor
    SECTION("Feret above B_ceil") {
        front.hoop_feret_mm = 50.;
        CHECK(hoop_credit(cprime, front, true, params).empty());
        params.user.B_ceil = 60.;
        CHECK(! hoop_credit(cprime, front, true, params).empty());
    }
    SECTION("not a hoop candidate") {
        front.hoop_hole = -1;
        CHECK(hoop_credit(cprime, front, true, params).empty());
        front.hoop_hole = 0;
        Polygon hole2 = circle_mm(Vec2d(25., 0.), 3., 64);
        hole2.reverse();
        front.C.holes.push_back(hole2);                                     // exactly one hole required (D26)
        CHECK(hoop_credit(cprime, front, true, params).empty());
    }
    SECTION("band width B_max · hoop_width_factor") {
        params.user.B_max = 10.;
        CHECK(mm2(hoop_credit(cprime, front, true, params)) == Approx(M_PI * (15. * 15. - 10. * 10.)).epsilon(0.01));
    }
    SECTION("only C' gets the credit") {
        const ExPolygons half = diff_ex(cprime, ExPolygons{ rectangle_mm(-50., -50., 50., 0.) });
        CHECK(mm2(hoop_credit(half, front, true, params)) == Approx(0.5 * ring_20).epsilon(0.01));
    }
}

TEST_CASE("classify_front: bending and tension levels, causes, ties", "[SmartSupports][sas-tension]")
{
    const Params     params = test_params();         // K_max 3, f_hard 1.5
    const ExPolygons cprime { rectangle_mm(0., 0., 10., 5.) };
    const ReachFn    reach_x([](const Vec2d &p) { return float(p.x()); });
    const float      no_exit = std::numeric_limits<float>::infinity();
    const int cs = 0, ls = 1, may = int(Level::May), should = int(Level::Should), must = int(Level::Must);
    auto total = [](const FrontNeeds &n) {
        double a = 0.;
        for (int c = 0; c < 2; ++ c)
            for (int l = 0; l < 3; ++ l)
                a += mm2(n.need[c][l]);
        return a;
    };

    SECTION("no finite span: cantilever by c") {
        const FrontNeeds n = classify_front(cprime, reach_x, uniform_tension(cprime, TLevel::None), {}, no_exit, params);
        CHECK(mm2(n.need[cs][should]) == Approx(7.5).margin(0.3));
        CHECK(mm2(n.need[cs][must]) == Approx(27.5).margin(0.3));
        CHECK(total(n) == Approx(35.).margin(0.3));
        CHECK(n.max_c > 9.9f);
        CHECK(n.zero.empty());
        // An empty tension map is the same as None everywhere.
        const FrontNeeds n2 = classify_front(cprime, reach_x, TensionMap(), {}, no_exit, params);
        CHECK(mm2(n2.need[cs][must]) == Approx(mm2(n.need[cs][must])).epsilon(1e-9));
    }
    SECTION("tension may beats bending: long span") {
        const FrontNeeds n = classify_front(cprime, reach_x, uniform_tension(cprime, TLevel::May), {}, no_exit, params);
        CHECK(mm2(n.need[ls][may]) == Approx(35.).margin(0.3));
        CHECK(total(n) == Approx(35.).margin(0.3));
    }
    SECTION("tie at should: long span") {
        const FrontNeeds n = classify_front(cprime, reach_x, uniform_tension(cprime, TLevel::Should), {}, no_exit, params);
        CHECK(mm2(n.need[ls][should]) == Approx(35.).margin(0.3));
        CHECK(total(n) == Approx(35.).margin(0.3));
    }
    SECTION("tension must: tie at must is long span, bending should stays cantilever") {
        const FrontNeeds n = classify_front(cprime, reach_x, uniform_tension(cprime, TLevel::Must), {}, no_exit, params);
        CHECK(mm2(n.need[ls][must]) == Approx(27.5).margin(0.3));
        CHECK(mm2(n.need[cs][should]) == Approx(7.5).margin(0.3));
        CHECK(total(n) == Approx(35.).margin(0.3));
    }
    SECTION("covered by tension") {
        const FrontNeeds n = classify_front(cprime, reach_x, uniform_tension(cprime, TLevel::Covered), {}, no_exit, params);
        CHECK(total(n) == 0.);
        CHECK(mm2(n.zero) == Approx(50.).epsilon(1e-9));
    }
    SECTION("hoop band covers and is zero") {
        const ExPolygons hoop { rectangle_mm(0., 0., 6., 5.) };
        const FrontNeeds n = classify_front(cprime, reach_x, uniform_tension(cprime, TLevel::None), hoop, no_exit, params);
        CHECK(n.need[cs][should].empty());
        CHECK(mm2(n.need[cs][must]) == Approx(20.).margin(0.3));
        CHECK(mm2(n.zero) == Approx(30.).epsilon(1e-9));
    }
    SECTION("mixed tension map") {
        TensionMap tm;
        tm.regions.emplace_back(ExPolygons{ rectangle_mm(0., 0., 5., 5.) }, TLevel::May);
        tm.regions.emplace_back(ExPolygons{ rectangle_mm(5., 0., 10., 5.) }, TLevel::None);
        const FrontNeeds n = classify_front(cprime, reach_x, tm, {}, no_exit, params);
        CHECK(mm2(n.need[ls][may]) == Approx(10.).margin(0.3));
        CHECK(mm2(n.need[cs][must]) == Approx(25.).margin(0.3));
        CHECK(mm2(n.need[cs][should]) == Approx(0.).margin(0.1));
    }
}

TEST_CASE("classify_front: thin C', unreachable points, early exit, determinism", "[SmartSupports][sas-tension]")
{
    const Params  params  = test_params();
    const ReachFn reach_x([](const Vec2d &p) { return float(p.x()); });
    const float   no_exit = std::numeric_limits<float>::infinity();
    const int     cs = 0, should = int(Level::Should), must = int(Level::Must);

    SECTION("a strip thinner than the sample spacing is classified by its boundary samples") {
        const ExPolygons strip { rectangle_mm(0., 0., 10., 0.04) };
        const FrontNeeds n = classify_front(strip, reach_x, TensionMap(), {}, no_exit, params);
        CHECK(mm2(n.need[cs][must]) == Approx(5.5 * 0.04).margin(0.01));
        CHECK(mm2(n.need[cs][should]) == Approx(1.5 * 0.04).margin(0.01));
    }
    SECTION("non-finite c counts as must") {
        const ExPolygons cprime { rectangle_mm(0., 0., 10., 5.) };
        const ReachFn    inf([](const Vec2d &) { return std::numeric_limits<float>::infinity(); });
        const FrontNeeds n = classify_front(cprime, inf, TensionMap(), {}, no_exit, params);
        CHECK(mm2(n.need[cs][must]) == Approx(50.).epsilon(1e-6));
        CHECK(n.max_c == 0.f);
    }
    SECTION("early exit: max ĉ + perimeter/2 + width ≤ K") {
        const ExPolygons small { rectangle_mm(0., 0., 1., 0.5) };
        const ReachFn    ten([](const Vec2d &) { return 10.f; });
        // 1.0 + 1.5 + 0.5 = 3 ≤ K: skipped (the caller guarantees c ≤ that bound).
        FrontNeeds n = classify_front(small, ten, TensionMap(), {}, 1.f, params);
        CHECK(n.need[cs][must].empty());
        CHECK(n.max_c == 10.f);
        n = classify_front(small, ten, TensionMap(), {}, 1.5f, params);
        CHECK(mm2(n.need[cs][must]) == Approx(0.5).epsilon(1e-6));
        n = classify_front(small, ten, TensionMap(), {}, no_exit, params);
        CHECK(mm2(n.need[cs][must]) == Approx(0.5).epsilon(1e-6));
    }
    SECTION("deterministic") {
        const ExPolygons cprime { ExPolygon(circle_mm(Vec2d(3., 2.), 4., 100)) };
        const FrontNeeds a = classify_front(cprime, reach_x, TensionMap(), {}, no_exit, params);
        const FrontNeeds b = classify_front(cprime, reach_x, TensionMap(), {}, no_exit, params);
        for (int l = 0; l < 3; ++ l)
            CHECK(a.need[cs][l] == b.need[cs][l]);
        CHECK(a.max_c == b.max_c);
    }
}

TEST_CASE("tension and classification together: 50 mm bridge", "[SmartSupports][sas-tension]")
{
    // c grows from both towers; the 50 mm span is a may-level long span wherever c exceeds K.
    const Params params = test_params();
    Scene s = bridge(50.);
    s.evaluate(params);
    const ReachFn    reach([](const Vec2d &p) { return float(std::min(p.x(), 50. - p.x())); });
    const FrontNeeds n = classify_front(s.cprime, reach, s.tension, {}, std::numeric_limits<float>::infinity(), params);
    CHECK(mm2(n.need[1][int(Level::May)]) == Approx(440.).margin(1.));
    CHECK(n.need[0][int(Level::Should)].empty());
    CHECK(n.need[0][int(Level::Must)].empty());
    CHECK(n.max_c == Approx(25.f).margin(0.1));
}
