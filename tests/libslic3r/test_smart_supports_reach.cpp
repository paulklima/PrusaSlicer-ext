// Smart support painting: raster utilities and accumulated reach — ReachField, ReachCarry, segment minimiser
// (plan §7.1; WP2b1). Reference numbers: theory §2.3 / §10, reviews 1 m11, 4 M2/M3/M7, 5 m7.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <random>
#include <thread>

#include "smart_supports_meshes.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/PrintBase.hpp"
#include "libslic3r/Support/SmartSupportsInternal.hpp"

using namespace Slic3r;
using namespace Slic3r::SmartSupports;
using namespace Slic3r::SmartSupports::Testing;

namespace {

constexpr double DEG = M_PI / 180.;

Point pt(const Vec2d &p) { return Point(coord_t(std::round(scaled<double>(p.x()))), coord_t(std::round(scaled<double>(p.y())))); }
Point pt(double x, double y) { return pt(Vec2d(x, y)); }
Vec2d rot(const Vec2d &p, double deg) { return Eigen::Rotation2Dd(deg * DEG) * p; }
ExPolygon rotated(ExPolygon ex, double deg)
{
    ex.rotate(deg * DEG);
    return ex;
}

// Seeds along a polyline, split into pieces of at most `spacing` mm; ĉ from c_at at every sample point.
std::vector<AnchorSeg> seeds_along(const Points &pts, const std::function<float(const Point &)> &c_at, double spacing = 0.1)
{
    Points samples;
    for (size_t i = 0; i + 1 < pts.size(); ++ i) {
        const Vec2d a = pts[i].cast<double>();
        const Vec2d b = pts[i + 1].cast<double>();
        const int   k = std::max(1, int(std::ceil((b - a).norm() * SCALING_FACTOR / spacing)));
        for (int j = 0; j < k; ++ j) {
            const Vec2d p = a + (b - a) * (double(j) / double(k));
            samples.emplace_back(coord_t(std::round(p.x())), coord_t(std::round(p.y())));
        }
    }
    if (! pts.empty())
        samples.emplace_back(pts.back());
    std::vector<AnchorSeg> out;
    for (size_t i = 0; i + 1 < samples.size(); ++ i)
        if (samples[i] != samples[i + 1])
            out.push_back({ Line(samples[i], samples[i + 1]), c_at(samples[i]), c_at(samples[i + 1]) });
    return out;
}
std::vector<AnchorSeg> seeds_along(const std::vector<Vec2d> &pts_mm, float c)
{
    Points pts;
    for (const Vec2d &p : pts_mm)
        pts.emplace_back(pt(p));
    return seeds_along(pts, [c](const Point &) { return c; });
}

// Maximum of the field over the boundary of C, sampled every `step` mm (vertices included).
double max_on_boundary(const ExPolygon &C, const ReachQuery &field, double step = 0.05)
{
    double best = 0.;
    auto   scan = [&](const Polygon &poly) {
        for (size_t i = 0; i < poly.points.size(); ++ i) {
            const Vec2d a = unscale(poly.points[i]);
            const Vec2d b = unscale(poly.points[(i + 1) % poly.points.size()]);
            const int   k = std::max(1, int(std::ceil((b - a).norm() / step)));
            for (int j = 0; j < k; ++ j)
                best = std::max(best, double(field.value(pt(a + (b - a) * (double(j) / double(k))))));
        }
    };
    scan(C.contour);
    for (const Polygon &hole : C.holes)
        scan(hole);
    return best;
}

// Mini evaluator of the ĉ chain (all material grounded, no support, tension or hoop): per layer n ≥ 1
// S = L_n ∩ (L_{n−1} ⊕ w, round, arc tolerance 1 µm as D24), fronts = components of L_n − S,
// material anchors = ∂C ∩ (S ⊕ 2 µm), seeds every ≤ 0.1 mm with ĉ = carry.c_hat, one ReachField per front
// (C' = D = C), carry push of every layer (also without fronts). z_top_n = (n + 1)·h.
struct ChainLayer
{
    ExPolygons                                      fronts;
    std::vector<std::shared_ptr<const ReachField>> fields;
};
std::vector<ChainLayer> run_chain(const std::vector<ExPolygons> &slices, double h, double w, double delta_beta, const Params &params)
{
    std::vector<ChainLayer> out(slices.size());
    ReachCarry              carry(params);
    const coord_t           w_s = scaled<coord_t>(w);
    carry.push(0, h, w_s, {});
    for (size_t n = 1; n < slices.size(); ++ n) {
        const double     z_top   = double(n + 1) * h;
        const ExPolygons S       = intersection_ex(slices[n], offset(slices[n - 1], float(w_s), ClipperLib::jtRound, scaled<double>(0.001)));
        const ExPolygons U       = diff_ex(slices[n], S);
        const Polygons   S_grown = expand(S, float(scaled<double>(0.002)));
        std::vector<FrontCarry> carried;
        for (const ExPolygon &C : U) {
            if (C.area() <= 0.)
                continue;
            std::vector<AnchorSeg> seeds;
            for (const Polyline &anchor : intersection_pl(to_polylines(C), S_grown))
                append(seeds, seeds_along(anchor.points, [&](const Point &a) { return carry.c_hat(a, uint32_t(n), z_top, h, w_s); }));
            auto field = std::make_shared<const ReachField>(ExPolygons{ C }, ExPolygons{ C }, std::move(seeds), delta_beta, params, ThrowOnCancel{});
            carried.push_back({ C, get_extents(C), true, field->cropped(get_extents(C)), {}, nullptr });
            out[n].fronts.emplace_back(C);
            out[n].fields.emplace_back(std::move(field));
        }
        carry.push(uint32_t(n), z_top, w_s, std::move(carried));
    }
    return out;
}

double last_max(const std::vector<ChainLayer> &chain)
{
    double            best = 0.;
    const ChainLayer &last = chain.back();
    for (size_t i = 0; i < last.fronts.size(); ++ i)
        best = std::max(best, max_on_boundary(last.fronts[i], *last.fields[i]));
    return best;
}

// Synthetic field c = c0 + (x − x0), for the carry tests.
class LinearField final : public ReachQuery
{
public:
    LinearField(double x0, double c0) : m_x0(x0), m_c0(c0) {}
    float   value(const Point &p) const override { return float(m_c0 + unscale<double>(p.x()) - m_x0); }
    coord_t res() const override { return scaled<coord_t>(0.1); }
    bool    direct_mode() const override { return true; }

private:
    double m_x0, m_c0;
};

} // namespace

TEST_CASE("segment_min is the exact minimum along the segment", "[SmartSupports][sas-reach]")
{
    SECTION("closed form with a gradient (review 4 M2b)") {
        // c0 + g·s + dist along a 10 mm segment with g = 0.9: minimum c0 + g·sp + d·√(1 − g²) (the foot point gives g·sp + d).
        const AnchorSeg seg { Line(pt(0., 0.), pt(10., 0.)), 0.f, 9.f };
        CHECK(segment_min(seg, pt(5., 0.4), 0.) == Catch::Approx(4.5 + 0.4 * std::sqrt(1. - 0.81)).margin(1e-5));
        // |g| ≥ 1 (reset border, 3 → 0 within 0.1 mm): the low end wins.
        const AnchorSeg steep { Line(pt(0., 0.), pt(0.1, 0.)), 3.f, 0.f };
        CHECK(segment_min(steep, pt(-0.5, 0.2), 0.) == Catch::Approx(std::hypot(0.6, 0.2)).margin(1e-5));
        // Δ: the first Δ mm of distance are free. Here the stationary point lies beyond Δ: the Δ = 0 value minus Δ.
        CHECK(segment_min(seg, pt(5., 0.4), 0.5) == Catch::Approx(4.5 + 0.4 * std::sqrt(1. - 0.81) - 0.5).margin(1e-5));
        // A gentle gradient (g = 0.3) moves the minimum onto the kink √(d² + (s − sp)²) = Δ, s = sp − √(Δ² − d²).
        const AnchorSeg gentle { Line(pt(0., 0.), pt(10., 0.)), 0.f, 3.f };
        CHECK(segment_min(gentle, pt(5., 0.4), 0.5) == Catch::Approx(0.3 * (5. - std::sqrt(0.25 - 0.16))).margin(1e-5));
    }
    SECTION("random segments against brute force") {
        std::mt19937                           rng(7);
        std::uniform_real_distribution<double> coord(-3., 3.), cval(0., 4.);
        for (int iter = 0; iter < 300; ++ iter) {
            AnchorSeg seg { Line(pt(coord(rng), coord(rng)), pt(coord(rng), coord(rng))), float(cval(rng)), float(cval(rng)) };
            if (iter % 3 == 0)
                seg.l.b = seg.l.a + pt(0.05 * coord(rng), 0.05 * coord(rng));        // short and steep
            const Point  p   = pt(coord(rng), coord(rng));
            const Vec2d  a   = unscale(seg.l.a), b = unscale(seg.l.b), pm = unscale(p);
            const double len = (b - a).norm();
            for (double delta : { 0., 0.08, 0.6 }) {
                const int K    = 20000;
                double    best = std::numeric_limits<double>::infinity();
                for (int k = 0; k <= K; ++ k) {
                    const double t = double(k) / K;
                    best = std::min(best, seg.c0 + t * (seg.c1 - seg.c0) + std::max(0., (pm - (a + t * (b - a))).norm() - delta));
                }
                const double g    = len > 0. ? std::abs(seg.c1 - seg.c0) / len : 0.;
                const double tol  = (g + 1.) * len / K + 1e-5;
                const double fast = segment_min(seg, p, delta);
                INFO("iter " << iter << " delta " << delta);
                CHECK(fast <= best + 1e-5);
                CHECK(fast >= best - tol);
            }
        }
    }
}

TEST_CASE("Raster: make_grid, rasterize, cells_to_expolygons", "[SmartSupports][sas-reach]")
{
    const coord_t res = scaled<coord_t>(0.1);
    auto count = [](const Grid &grid, uint8_t value) { return std::count(grid.m.begin(), grid.m.end(), value); };
    auto area_mm2 = [](const ExPolygons &expolys) {
        double a = 0.;
        for (const ExPolygon &ex : expolys)
            a += ex.area();
        return a * SCALING_FACTOR * SCALING_FACTOR;
    };

    SECTION("layout and rectangle with a hole") {
        ExPolygon ex = rectangle_mm(0., 0., 4., 3.);
        ex.holes.emplace_back(polygon_mm({ { 1., 1. }, { 1., 2. }, { 2., 2. }, { 2., 1. } }));
        Grid grid = make_grid(get_extents(ex), res);
        CHECK(grid.origin == Point(-coord_t(0.37 * res), -coord_t(0.61 * res)));
        CHECK(grid.origin.x() + grid.cols * res > scaled<coord_t>(4.));
        CHECK(grid.origin.y() + grid.rows * res > scaled<coord_t>(3.));
        rasterize({ ex }, grid, 3, false);
        // Centres x = 0.013 + 0.1·i, y = −0.011 + 0.1·j: 40 × 30 inside the rectangle, 10 × 10 inside the hole.
        CHECK(count(grid, 3) == 1100);
        const ExPolygons back = cells_to_expolygons(grid, [&](int idx) { return grid.m[idx] == 3; });
        REQUIRE(back.size() == 1);
        CHECK(back.front().holes.size() == 1);
        CHECK(area_mm2(back) == Catch::Approx(11.).epsilon(1e-9));
    }
    SECTION("conservative raster keeps a thin neck") {
        const ExPolygons shape = union_ex(ExPolygons{ rectangle_mm(0., 0., 2., 2.), rectangle_mm(3., 0., 5., 2.), rectangle_mm(1.9, 1., 3.1, 1.02) });
        Grid exact = make_grid(get_extents(shape), res);
        Grid cons  = exact;
        rasterize(shape, exact, 1, false);
        rasterize(shape, cons, 1, true);
        CHECK(cells_to_expolygons(exact, [&](int idx) { return exact.m[idx] != 0; }).size() == 2);
        CHECK(cells_to_expolygons(cons, [&](int idx) { return cons.m[idx] != 0; }).size() == 1);
        for (size_t i = 0; i < exact.m.size(); ++ i)
            if (exact.m[i])
                REQUIRE(cons.m[i]);
    }
    SECTION("overlapping inputs fill as their union; other cells are kept") {
        const ExPolygons two { rectangle_mm(0., 0., 2., 1.), rectangle_mm(1., 0., 3., 1.) };
        Grid grid = make_grid(BoundingBox(pt(-1., -1.), pt(4., 2.)), res);
        grid.m.front() = 7;
        rasterize(two, grid, 1, false);
        CHECK(grid.m.front() == 7);
        CHECK(count(grid, 1) == 300);
        CHECK(area_mm2(cells_to_expolygons(grid, [&](int idx) { return grid.m[idx] == 1; })) == Catch::Approx(3.).epsilon(1e-9));
    }
}

TEST_CASE("ReachField: straight anchor on a rectangle rotated by 13.28°", "[SmartSupports][sas-reach]")
{
    // 13.28° is the worst direction of the 16-neighbour metric (2.73 %); 8 neighbours would be 6.9 % off (review 4 M7).
    const double     angle  = 13.28;
    const Params     params = test_params();
    const ExPolygon  rect   = rotated(rectangle_mm(0., 0., 20., 6.), angle);
    const ReachField field({ rect }, { rect }, seeds_along({ rot({ 0., 0. }, angle), rot({ 20., 0. }, angle) }, 0.f), 0., params, {});
    REQUIRE(! field.direct_mode());
    CHECK(field.res() == scaled<coord_t>(params.grid_res_min));
    double worst = 0.;
    for (double x = 3.; x <= 17.; x += 0.37) {
        for (double d : { 0.02, 0.2, 0.45 })
            CHECK(field.value(pt(rot({ x, d }, angle))) == Catch::Approx(d).margin(0.01));
        for (double d : { 1., 2., 3., 4., 5., 5.9 }) {
            const double v = field.value(pt(rot({ x, d }, angle)));
            CHECK(v >= d - 1e-3);
            CHECK(v <= 1.035 * d);
            worst = std::max(worst, v / d - 1.);
        }
    }
    INFO("worst relative error " << worst);
    CHECK(worst < 0.035);
}

TEST_CASE("ReachField: geodesic L path rotated by 22.5°", "[SmartSupports][sas-reach]")
{
    // The anchor is the end of one arm; points of the other arm are reached around the inner corner (2, 2):
    // geo = 10 + |p − (2, 2)|. At 22.5° 8 neighbours are 8.2 % off, 16 neighbours 1.4 %.
    const double     angle  = 22.5;
    const Params     params = test_params();
    const ExPolygons L      = union_ex(ExPolygons{ rotated(rectangle_mm(0., 0., 2., 12.), angle), rotated(rectangle_mm(0., 0., 12., 2.), angle) });
    REQUIRE(L.size() == 1);
    const ReachField field(L, L, seeds_along({ rot({ 0., 12. }, angle), rot({ 2., 12. }, angle) }, 0.f), 0., params, {});
    REQUIRE(! field.direct_mode());
    for (double x : { 4., 6., 8., 10., 11.5 })
        for (double y : { 0.3, 1., 1.7 }) {
            const double geo = 10. + (Vec2d(x, y) - Vec2d(2., 2.)).norm();
            const double v   = field.value(pt(rot({ x, y }, angle)));
            INFO("x " << x << " y " << y << " geo " << geo << " value " << v);
            CHECK(v >= geo - 0.1);          // the conservative raster cuts the corner by less than a cell
            CHECK(v <= 1.035 * geo);
        }
    for (double y : { 3., 6., 9. })
        for (double x : { 0.5, 1.5 }) {
            const double v = field.value(pt(rot({ x, y }, angle)));
            CHECK(v >= 12. - y - 1e-3);
            CHECK(v <= 1.035 * (12. - y));
        }
}

TEST_CASE("ReachField: knight steps do not jump a one-cell wall", "[SmartSupports][sas-reach]")
{
    // A slot that leaves exactly one row of inactive cells (width 0.95·res after the conservative ⊕ res/2) can only
    // be crossed by a knight step that ignores the cells it passes (review 4 M2d). Eight sub-cell positions: at most
    // one leaves no cell centre in the slot (then the slit legally counts as connected, §3.14 #14).
    Params params       = test_params();
    params.grid_res_min = params.grid_res_max = 0.05;
    const double res    = 0.05;
    const double width  = 1.95 * res;
    int          around = 0;
    for (int k = 0; k < 8; ++ k) {
        const double     ys     = 2. + k * res / 8.;
        const ExPolygons domain = diff_ex(ExPolygons{ rectangle_mm(0., 0., 10., 4.) }, ExPolygons{ rectangle_mm(-1., ys - width / 2., 8., ys + width / 2.) });
        const ReachField field(domain, domain, seeds_along({ { 0.2, 0. }, { 1., 0. } }, 0.f), 0., params, {});
        REQUIRE(field.res() == scaled<coord_t>(res));
        const double v = field.value(pt(0.5, ys + 0.6));
        INFO("slot at " << ys << ": " << v);
        CHECK(v < 20.);
        if (v > 12.)
            ++ around;
    }
    CHECK(around >= 7);
}

TEST_CASE("ReachField: conservative raster, thin neck, fallback", "[SmartSupports][sas-reach]")
{
    // A 0.02 mm neck (narrower than a cell) at the top joins two squares: the right square is reached through it
    // (4 + |(6, 0.5) − (4, 2.9)| = 7.12); without the conservative raster it would fall back to the Euclidean 6.
    const Params     params = test_params();
    const ExPolygons domain = union_ex(ExPolygons{ rectangle_mm(0., 0., 3., 3.), rectangle_mm(2.9, 2.9, 4.1, 2.92), rectangle_mm(4., 0., 7., 3.) });
    REQUIRE(domain.size() == 1);
    const ReachField field(domain, domain, seeds_along({ { 0., 0. }, { 0., 3. } }, 0.f), 0., params, {});
    const double     geo = 4. + (Vec2d(6., 0.5) - Vec2d(4., 2.9)).norm();
    const double     v   = field.value(pt(6., 0.5));
    CHECK(v >= geo - 0.1);
    CHECK(v <= 1.035 * geo);
    // Outside the domain the field still answers (Euclidean fallback), never +∞.
    CHECK(field.value(pt(30., 1.)) == Catch::Approx(30.).margin(1e-3));
    // Without seeds the component is ungrounded: +∞.
    const ReachField none(domain, domain, {}, 0., params, {});
    CHECK(std::isinf(none.value(pt(1., 1.))));
}

TEST_CASE("ReachField: reset border and Direct mode", "[SmartSupports][sas-reach]")
{
    const Params params = test_params();
    // ĉ = 3 on x < 0, ĉ = 0 on x ≥ 0 (constant halves): near the border c = distance to the zero part (review 1 m4).
    std::vector<AnchorSeg> seeds = seeds_along({ { -10., 0. }, { 0., 0. } }, 3.f);
    append(seeds, seeds_along({ { 0., 0. }, { 10., 0. } }, 0.f));
    auto expected = [](double x, double y) { return x >= 0. ? y : std::min(3. + y, std::hypot(x, y)); };

    SECTION("Grid") {
        const ExPolygon  rect = rectangle_mm(-10., 0., 10., 6.);
        const ReachField field({ rect }, { rect }, seeds, 0., params, {});
        REQUIRE(! field.direct_mode());
        for (double x : { -4., -2.9, -2., -1., -0.3, 0.5 })
            for (double y : { 0.05, 0.3, 1., 2. }) {
                const double e = expected(x, y);
                const double v = field.value(pt(x, y));
                INFO("x " << x << " y " << y);
                CHECK(v >= e - 1e-3);
                CHECK(v <= 1.035 * e + 0.01);
            }
    }
    SECTION("Direct") {
        const ExPolygon  strip = rectangle_mm(-10., 0., 10., 0.3);
        const ReachField field({ strip }, { strip }, seeds, 0., params, {});
        REQUIRE(field.direct_mode());
        CHECK(field.res() == scaled<coord_t>(params.grid_res_max));
        for (double x : { -4., -2.9, -2., -1., -0.3, 0.5 })
            for (double y : { 0.05, 0.3 })
                CHECK(field.value(pt(x, y)) == Catch::Approx(expected(x, y)).margin(1e-4));
    }
}

TEST_CASE("ReachField: Direct mode with free ends", "[SmartSupports][sas-reach]")
{
    // Round ends of the exact-zone cover: the corners beyond the anchor ends (0.36 mm away) are still inside ρ, so the
    // strip is Direct (butt ends would push it into Grid mode, review 4 M3).
    const Params     params = test_params();
    const auto       seeds  = seeds_along({ { 0., 0. }, { 10., 0. } }, 0.5f);
    const ExPolygon  strip  = rectangle_mm(-0.2, 0., 10.2, 0.3);
    const ReachField field({ strip }, { strip }, seeds, 0., params, {});
    CHECK(field.direct_mode());
    CHECK(field.value(pt(-0.2, 0.3)) == Catch::Approx(0.5 + std::hypot(0.2, 0.3)).margin(1e-4));
    CHECK(field.value(pt(10.2, 0.3)) == Catch::Approx(0.5 + std::hypot(0.2, 0.3)).margin(1e-4));
    CHECK(field.value(pt(5., 0.3)) == Catch::Approx(0.8).margin(1e-4));
    // Ends 0.67 mm beyond the anchor: Grid mode, same values within the grid tolerance.
    const ExPolygon  longer = rectangle_mm(-0.6, 0., 10.6, 0.3);
    const ReachField grid({ longer }, { longer }, seeds, 0., params, {});
    CHECK(! grid.direct_mode());
    const double e = 0.5 + std::hypot(0.6, 0.3);
    CHECK(grid.value(pt(-0.6, 0.3)) >= e - 1e-3);
    CHECK(grid.value(pt(-0.6, 0.3)) <= e * 1.035);
}

TEST_CASE("ReachField: cropped equals the field inside the bbox", "[SmartSupports][sas-reach]")
{
    const Params     params = test_params();
    const ExPolygon  rect   = rotated(rectangle_mm(0., 0., 20., 6.), 13.28);
    const auto       seeds  = seeds_along({ rot({ 0., 0. }, 13.28), rot({ 20., 0. }, 13.28) }, 0.25f);
    const ReachField field({ rect }, { rect }, seeds, 0., params, {});
    REQUIRE(! field.direct_mode());
    const auto crop = field.cropped(BoundingBox(pt(5., 2.), pt(9., 5.)));
    CHECK(crop->res() == field.res());
    CHECK(! crop->direct_mode());
    for (double x = 5.; x <= 9.; x += 0.0731)
        for (double y = 2.; y <= 5.; y += 0.0537)
            REQUIRE(crop->value(pt(x, y)) == field.value(pt(x, y)));

    const ExPolygon  strip  = rectangle_mm(0., 0., 10., 0.3);
    const ReachField direct({ strip }, { strip }, seeds_along({ { 0., 0. }, { 10., 0. } }, 1.f), 0., params, {});
    const auto       dcrop  = direct.cropped(BoundingBox(pt(2., 0.), pt(3., 0.3)));
    CHECK(dcrop->direct_mode());
    for (double x = 2.; x <= 3.; x += 0.1)
        CHECK(dcrop->value(pt(x, 0.2)) == direct.value(pt(x, 0.2)));
}

TEST_CASE("ReachField: β < α and Δ beyond ρ", "[SmartSupports][sas-reach]")
{
    // β = 10° at h 0.2: Δ = 0.2/tan 10° − 0.2 = 0.934 > ρ; c = 1 + max(0, y − Δ) for a straight anchor with ĉ = 1.
    const Params     params = test_params();
    const double     delta  = 0.2 / std::tan(10. * DEG) - 0.2;
    const ExPolygon  rect   = rectangle_mm(0., 0., 10., 6.);
    const ReachField field({ rect }, { rect }, seeds_along({ { 0., 0. }, { 10., 0. } }, 1.f), delta, params, {});
    REQUIRE(! field.direct_mode());
    for (double x : { 2., 5.3, 8. }) {
        for (double y : { 0.1, 0.5, 0.8, 0.9 })
            CHECK(field.value(pt(x, y)) == Catch::Approx(1.).margin(1e-3));
        for (double y : { 1.5, 3., 5.5 }) {
            CHECK(field.value(pt(x, y)) >= 1. + y - delta - 1e-3);
            CHECK(field.value(pt(x, y)) <= 1. + 1.035 * (y - delta));
        }
    }
}

TEST_CASE("ReachField: cancel and determinism", "[SmartSupports][sas-reach]")
{
    Params params       = test_params();
    params.grid_res_min = params.grid_res_max = 0.05;
    const ExPolygon  rect  = rectangle_mm(0., 0., 30., 30.);                     // ≈ 370k cells
    const auto       seeds = seeds_along({ { 0., 0. }, { 30., 0. } }, 0.f);
    int              calls = 0;
    const ReachField counted({ rect }, { rect }, seeds, 0., params, [&calls] { ++ calls; });
    CHECK(calls >= 4);
    CHECK_THROWS_AS(ReachField({ rect }, { rect }, seeds, 0., params, [] { throw CanceledException(); }), CanceledException);

    const ReachField again({ rect }, { rect }, seeds, 0., params, {});
    for (double x = 0.3; x < 30.; x += 1.7)
        for (double y = 0.1; y < 30.; y += 2.3)
            REQUIRE(again.value(pt(x, y)) == counted.value(pt(x, y)));
}

TEST_CASE("ReachCarry: hanging rule, zero regions, H_s window, threads", "[SmartSupports][sas-reach]")
{
    const Params  params = test_params();                          // H_s 1, δ = 0.1·0.45
    const double  h      = 0.2;
    const coord_t w      = scaled<coord_t>(0.2);
    auto front = [](const ExPolygon &C, bool grounded, std::shared_ptr<const ReachQuery> field, ExPolygons zero) {
        return FrontCarry{ C, get_extents(C), grounded, std::move(field), std::move(zero), nullptr };
    };
    // Layer 1: A = [0, 2] × [0, 4] with c = x, its top strip y ≥ 3 covered (zero); A2 = [10, 12] × [0, 4] with c = 5 + (x − 10).
    // Layer 2: the same outline as A2, not grounded. Layer 3: no fronts.
    const ExPolygon A  = rectangle_mm(0., 0., 2., 4.);
    const ExPolygon A2 = rectangle_mm(10., 0., 12., 4.);
    ReachCarry      carry(params);
    carry.push(0, 0.2, w, {});
    {
        std::vector<FrontCarry> fronts;
        fronts.push_back(front(A, true, std::make_shared<LinearField>(0., 0.), { rectangle_mm(0., 3., 2., 4.) }));
        fronts.push_back(front(A2, true, std::make_shared<LinearField>(10., 5.), {}));
        carry.push(1, 0.4, w, std::move(fronts));
    }
    {
        std::vector<FrontCarry> fronts;
        fronts.push_back(front(A2, false, nullptr, {}));
        carry.push(2, 0.6, w, std::move(fronts));
    }
    carry.push(3, 0.8, w, {});
    // n = 4 (z_top 1.0): layer 1 is 0.4 below → factor 0.6; R = w_4 + w_3 + w_2 + δ = 0.645.
    CHECK(carry.c_hat(pt(2.2, 1.), 4, 1.0, h, w) == Catch::Approx(2. * 0.6).margin(1e-4));
    CHECK(carry.c_hat(pt(2.6, 1.), 4, 1.0, h, w) == Catch::Approx(2. * 0.6).margin(1e-4));
    CHECK(carry.c_hat(pt(2.7, 1.), 4, 1.0, h, w) == 0.f);                                   // 0.7 > R
    CHECK(carry.c_hat(pt(1., 1.), 4, 1.0, h, w) == Catch::Approx(1. * 0.6).margin(1e-4));   // above A: q = a
    CHECK(carry.c_hat(pt(2.2, 3.5), 4, 1.0, h, w) == 0.f);                                  // hangs from the covered strip
    CHECK(carry.c_hat(pt(2.2, 3.), 4, 1.0, h, w) == 0.f);                                   // q on the zero border
    // Next to A2 the topmost layer in reach (2) holds only an ungrounded front: 0; layer 1 is not searched (review 1 m3).
    CHECK(carry.c_hat(pt(12.2, 1.), 4, 1.0, h, w) == 0.f);
    // n = 2: layer 1 directly below, factor 1.
    CHECK(carry.c_hat(pt(12.2, 1.), 2, 0.6, h, w) == Catch::Approx(7.).margin(1e-4));
    // H_s = 0: per-layer reach.
    Params flat   = params;
    flat.user.H_s = 0.;
    ReachCarry none(flat);
    {
        std::vector<FrontCarry> fronts;
        fronts.push_back(front(A, true, std::make_shared<LinearField>(0., 0.), {}));
        none.push(1, 0.4, w, std::move(fronts));
    }
    CHECK(none.c_hat(pt(2.2, 1.), 2, 0.6, h, w) == 0.f);

    // Concurrent queries equal the serial ones.
    std::vector<Point> probes;
    for (double x = -0.5; x < 3.; x += 0.0377)
        for (double y = -0.5; y < 4.5; y += 0.211)
            probes.emplace_back(pt(x, y));
    std::vector<float> serial(probes.size());
    for (size_t i = 0; i < probes.size(); ++ i)
        serial[i] = carry.c_hat(probes[i], 4, 1.0, h, w);
    std::vector<std::vector<float>> results(4, std::vector<float>(probes.size()));
    std::vector<std::thread>        threads;
    for (size_t t = 0; t < results.size(); ++ t)
        threads.emplace_back([&, t] {
            for (size_t i = 0; i < probes.size(); ++ i)
                results[t][i] = carry.c_hat(probes[(i + t * 97) % probes.size()], 4, 1.0, h, w);
        });
    for (std::thread &th : threads)
        th.join();
    for (size_t t = 0; t < results.size(); ++ t)
        for (size_t i = 0; i < probes.size(); ++ i)
            REQUIRE(results[t][i] == serial[(i + t * 97) % probes.size()]);

    // The window: from layer 7 (z_top 1.6) layer 1 (z_top 0.4) is H_s away.
    carry.push(4, 1.0, w, {});
    carry.push(5, 1.2, w, {});
    carry.push(6, 1.4, w, {});
    CHECK(carry.c_hat(pt(2.2, 1.), 7, 1.6, h, w) == Catch::Approx(0.).margin(1e-6));
}

TEST_CASE("Reach chain: H1 accumulation on a 40° wall", "[SmartSupports][sas-reach]")
{
    // Excess per layer h·(1/tan 40° − 1/tan 45°): 80 layers at 0.2 mm or 160 at 0.1 mm → 3.068 (review 1 m11).
    const Params params = test_params();
    struct Case { double h, rot_deg; };
    for (const Case &c : { Case{ 0.2, 0. }, Case{ 0.2, 30. }, Case{ 0.1, 0. } }) {
        const double            step = c.h / std::tan(40. * DEG);
        const int               N    = int(std::lround(16. / c.h));
        std::vector<ExPolygons> slices;
        for (int n = 0; n <= N; ++ n)
            slices.push_back({ rotated(rectangle_mm(-10., 0., n * step, 20.), c.rot_deg) });
        const auto chain = run_chain(slices, c.h, c.h, 0., params);
        for (const ChainLayer &layer : chain)
            for (const auto &field : layer.fields)
                REQUIRE(field->direct_mode());
        INFO("h " << c.h << " rotation " << c.rot_deg);
        CHECK(N * (step - c.h) == Catch::Approx(3.068).epsilon(0.001));
        CHECK(last_max(chain) == Catch::Approx(N * (step - c.h)).epsilon(0.02));
    }
}

TEST_CASE("Reach chain: inverted frustum keeps its corners (round S, review 4 M2a)", "[SmartSupports][sas-reach]")
{
    // Square inverted frustum with 40° faces, 80 layers. The sides accumulate like the wall (3.068); the corners hang
    // from the round corners of S at exactly w and accumulate √2·step − w per layer (the diagonal is flatter).
    // A phantom ĉ = 0 at the corners would lower the corner and spread sideways.
    const Params            params = test_params();
    const double            h = 0.2, step = h / std::tan(40. * DEG);
    const int               N = 80;
    std::vector<ExPolygons> slices;
    for (int n = 0; n <= N; ++ n) {
        const double a = 10. + n * step;
        slices.push_back({ rectangle_mm(-a, -a, a, a) });
    }
    const auto        chain = run_chain(slices, h, h, 0., params);
    const ChainLayer &last  = chain.back();
    REQUIRE(last.fronts.size() == 1);
    const ReachQuery &field = *last.fields.front();
    const double      a     = 10. + N * step;
    const double      side  = N * (step - h);
    CHECK(field.value(pt(a, 0.)) == Catch::Approx(side).epsilon(0.02));
    CHECK(field.value(pt(0., -a)) == Catch::Approx(side).epsilon(0.02));
    CHECK(field.value(pt(a, a)) == Catch::Approx(N * (M_SQRT2 * step - h)).epsilon(0.02));
    for (double t = 0.; t <= a; t += 0.1)
        REQUIRE(field.value(pt(a, t)) >= 0.98 * side);
}

TEST_CASE("Reach chain: β < α", "[SmartSupports][sas-reach]")
{
    const Params params = test_params();
    const double h      = 0.2;
    auto wall = [&](double angle_deg, int N) {
        std::vector<ExPolygons> slices;
        for (int n = 0; n <= N; ++ n)
            slices.push_back({ rectangle_mm(-10., 0., n * h / std::tan(angle_deg * DEG), 20.) });
        return slices;
    };
    // 40° wall, β = 35°: the slope lies between β and α and adds nothing.
    CHECK(last_max(run_chain(wall(40., 80), h, h, h / std::tan(35. * DEG) - h, params)) == Catch::Approx(0.).margin(1e-4));
    // 30° wall, 20 layers: β = α adds h/tan 30° − h per layer, β = 40° only h/tan 30° − h/tan 40°.
    const double excess_alpha = h / std::tan(30. * DEG) - h;
    const double excess_beta  = h / std::tan(30. * DEG) - h / std::tan(40. * DEG);
    CHECK(last_max(run_chain(wall(30., 20), h, h, 0., params)) == Catch::Approx(20. * excess_alpha).epsilon(0.02));
    CHECK(last_max(run_chain(wall(30., 20), h, h, h / std::tan(40. * DEG) - h, params)) == Catch::Approx(20. * excess_beta).epsilon(0.02));
}

TEST_CASE("Reach chain: H_s decay over stair risers (Grid fields)", "[SmartSupports][sas-reach]")
{
    // Treads 2.5 mm (c = 2.3 each); the next tread inherits 2.3·max(0, 1 − (Δz − h)/H_s) (theory §10 stairs: 4.14).
    auto stairs = [](int riser_layers, int treads) {
        std::vector<ExPolygons> slices { { rectangle_mm(-10., 0., 0., 10.) } };
        for (int t = 0; t < treads; ++ t)
            for (int k = 0; k < riser_layers; ++ k)
                slices.push_back({ rectangle_mm(-10., 0., 2.5 * (t + 1), 10.) });
        return slices;
    };
    const Params params = test_params();
    struct Case { int riser_layers; double expected; };
    for (const Case &c : { Case{ 2, 2.3 * 0.8 + 2.3 }, Case{ 5, 2.3 * 0.2 + 2.3 }, Case{ 6, 2.3 } }) {
        const auto chain = run_chain(stairs(c.riser_layers, 2), 0.2, 0.2, 0., params);
        INFO("riser layers " << c.riser_layers);
        REQUIRE(chain[1].fields.size() == 1);
        CHECK(! chain[1].fields.front()->direct_mode());
        CHECK(max_on_boundary(chain[1].fronts.front(), *chain[1].fields.front()) == Catch::Approx(2.3).epsilon(0.02));
        const ChainLayer &second = chain[1 + c.riser_layers];
        REQUIRE(second.fields.size() == 1);
        CHECK(max_on_boundary(second.fronts.front(), *second.fields.front()) == Catch::Approx(c.expected).epsilon(0.02));
    }
    Params per_layer   = params;
    per_layer.user.H_s = 0.;
    const auto chain   = run_chain(stairs(2, 2), 0.2, 0.2, 0., per_layer);
    REQUIRE(chain[3].fields.size() == 1);
    CHECK(max_on_boundary(chain[3].fronts.front(), *chain[3].fields.front()) == Catch::Approx(2.3).epsilon(0.02));
}
