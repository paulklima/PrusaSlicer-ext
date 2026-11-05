// Smart support painting: Phase A geometry (plan §3.5, §7.1; WP2a) on synthetic inputs — Feret diameter, bridge
// directions, fronts and clusters, weak contact, hoop candidates, unprintable material, determinism, cancel.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <string>
#include <tuple>

#include <oneapi/tbb/global_control.h>

#include "smart_supports_meshes.hpp"
#include "libslic3r/PrintBase.hpp"
#include "libslic3r/Support/SmartSupportsInternal.hpp"

using namespace Slic3r;
using namespace Slic3r::SmartSupports;
using namespace Slic3r::SmartSupports::Testing;
using Catch::Approx;

namespace {

double area_mm2(const ExPolygons &e) { return area(e) * SCALING_FACTOR * SCALING_FACTOR; }

BaseModel base_of(const AnalysisInput &in) { return build_base_model(in, [] {}, [](int) {}); }

// Index of the layer whose slab contains z.
size_t layer_at(const AnalysisInput &in, double z)
{
    for (size_t n = 0; n < in.layers.size(); ++ n)
        if (z < in.layers[n].slice_z + 0.5 * in.layers[n].height)
            return n;
    return in.layers.size() - 1;
}

// Angle of a direction modulo 180°, in degrees [0, 180).
double angle_deg(const Vec2d &d)
{
    double a = std::atan2(d.y(), d.x()) * 180. / M_PI;
    if (a < 0.)
        a += 180.;
    return a >= 180. ? a - 180. : a;
}

double angle_diff_deg(double a, double b)
{
    const double d = std::abs(a - b);
    return std::min(d, 180. - d);
}

bool has_direction(const std::vector<Vec2d> &dirs, double deg, double tol = 0.5)
{
    for (const Vec2d &d : dirs)
        if (angle_diff_deg(angle_deg(d), deg) <= tol)
            return true;
    return false;
}

// n ≡ −n: every direction is a unit vector with its angle in [0°, 180°), no two are equal modulo 180°.
void check_canonical(const std::vector<Vec2d> &dirs)
{
    for (size_t i = 0; i < dirs.size(); ++ i) {
        CHECK(dirs[i].norm() == Approx(1.).margin(1e-9));
        const double a = std::atan2(dirs[i].y(), dirs[i].x());
        CHECK(a >= 0.);
        CHECK(a < M_PI);
        for (size_t j = i + 1; j < dirs.size(); ++ j)
            CHECK(angle_diff_deg(angle_deg(dirs[i]), angle_deg(dirs[j])) > 1.);
    }
}

// Square tube (outer side `side`, wall 2, height 10) closed by a 2 mm lid with the given holes (centre, diameter).
Parts lidded_tube(double side, const std::vector<std::pair<Vec2d, double>> &holes)
{
    ExPolygon tube = rectangle_mm(0., 0., side, side);
    Polygon   inner = rectangle_mm(2., 2., side - 2., side - 2.).contour;
    inner.reverse();
    tube.holes.push_back(inner);
    ExPolygon lid = rectangle_mm(0., 0., side, side);
    for (const auto &[c, d] : holes) {
        Polygon h = circle_mm(c, 0.5 * d);
        h.reverse();
        lid.holes.push_back(std::move(h));
    }
    return { extrude(tube, 10.), extrude(lid, 2., 10.) };
}

// Flat ring (radii r_out / r_in, z 10..12) on one post (x0..x1 × −1.5..1.5, z 0..10).
Parts halo(double r_out, double r_in, double x0, double x1)
{
    ExPolygon ring(circle_mm(Vec2d::Zero(), r_out));
    Polygon   hole = circle_mm(Vec2d::Zero(), r_in);
    hole.reverse();
    ring.holes.push_back(std::move(hole));
    return { extrude(ring, 2., 10.), box({ x0, -1.5, 0. }, { x1, 1.5, 10. }) };
}

} // namespace

TEST_CASE("Feret diameter", "[SmartSupports][sas-geometry]")
{
    CHECK(unscale<double>(feret_diameter(rectangle_mm(0., 0., 10., 10.).contour)) == Approx(10. * std::sqrt(2.)).margin(1e-5));
    CHECK(unscale<double>(feret_diameter(circle_mm(Vec2d(3., 4.), 5.))) == Approx(10.).margin(1e-5));
    CHECK(unscale<double>(feret_diameter(polygon_mm({ { 0., 0. }, { 10., 0. }, { 0., 3. } }))) == Approx(std::sqrt(109.)).margin(1e-5));
    // Concave: the hull decides (L shape 10 × 10 with a 8 × 8 notch).
    CHECK(unscale<double>(feret_diameter(polygon_mm({ { 0., 0. }, { 10., 0. }, { 10., 2. }, { 2., 2. }, { 2., 10. }, { 0., 10. } }))) ==
          Approx(std::sqrt(200.)).margin(1e-5));
    // A hole (clockwise) has the same diameter.
    Polygon hole = circle_mm(Vec2d::Zero(), 4.);
    hole.reverse();
    CHECK(unscale<double>(feret_diameter(hole)) == Approx(8.).margin(1e-5));
    CHECK(feret_diameter(Polygon()) == 0.);
    CHECK(feret_diameter(polygon_mm({ { 1., 1. } })) == 0.);
}

TEST_CASE("Bridge directions follow the free edges (BridgeDetector cost)", "[SmartSupports][sas-geometry]")
{
    const Params params = test_params();
    // Shelf of depth D (+x) and length L (y) in front of a wall (x < 0): free edges x = D (length L) and y = 0, L
    // (length D each). Lines outward cost L, lines along the wall cost 2·D.
    auto shelf_dirs = [&](double D, double L, int max_dirs = 8) {
        return bridge_directions({ rectangle_mm(0., 0., D, L) }, { rectangle_mm(-20., -5., 0., L + 5.) }, params.tie_tolerance, max_dirs);
    };
    SECTION("short shelf: lines outward") {
        const std::vector<Vec2d> dirs = shelf_dirs(10., 15.);
        REQUIRE(dirs.size() == 1);
        CHECK(has_direction(dirs, 0.));
        check_canonical(dirs);
    }
    SECTION("long shelf: lines along the wall") {
        const std::vector<Vec2d> dirs = shelf_dirs(10., 30.);
        REQUIRE(dirs.size() == 1);
        CHECK(has_direction(dirs, 90.));
    }
    SECTION("tie: both directions") {
        const std::vector<Vec2d> dirs = shelf_dirs(10., 20.);
        REQUIRE(dirs.size() == 2);
        CHECK(has_direction(dirs, 0.));
        CHECK(has_direction(dirs, 90.));
        check_canonical(dirs);
    }
    SECTION("tie tolerance 10 %") {
        // Costs 20 (along) and 21 (outward): tied, the cheaper first.
        std::vector<Vec2d> dirs = shelf_dirs(10., 21.);
        REQUIRE(dirs.size() == 2);
        CHECK(angle_diff_deg(angle_deg(dirs[0]), 90.) < 0.5);
        CHECK(angle_diff_deg(angle_deg(dirs[1]), 0.) < 0.5);
        // Costs 20 and 23: not tied.
        dirs = shelf_dirs(10., 23.);
        REQUIRE(dirs.size() == 1);
        CHECK(has_direction(dirs, 90.));
        // max_dirs = 1 keeps the best one.
        dirs = shelf_dirs(10., 21., 1);
        REQUIRE(dirs.size() == 1);
        CHECK(has_direction(dirs, 90.));
    }
    SECTION("n ≡ −n: mirrored shelf and rotated shelf") {
        // Shelf on the other side of the wall: the same direction, canonical.
        std::vector<Vec2d> dirs = bridge_directions({ rectangle_mm(-10., 0., 0., 15.) }, { rectangle_mm(0., -5., 20., 20.) },
                                                    params.tie_tolerance, 8);
        REQUIRE(dirs.size() == 1);
        CHECK(has_direction(dirs, 0.));
        check_canonical(dirs);
        // Rotated by 120°: the outward direction rotates with it (300° ≡ 120°).
        ExPolygon  region = rectangle_mm(0., 0., 10., 15.);
        ExPolygon  wall   = rectangle_mm(-20., -5., 0., 20.);
        region.rotate(120. * M_PI / 180.);
        wall.rotate(120. * M_PI / 180.);
        dirs = bridge_directions({ region }, { wall }, params.tie_tolerance, 8);
        REQUIRE(dirs.size() == 1);
        CHECK(has_direction(dirs, 120.));
        check_canonical(dirs);
    }
    SECTION("anchored all around: minor principal axis") {
        const ExPolygons region { rectangle_mm(0., 0., 10., 4.) };
        const ExPolygons around = diff_ex(ExPolygons{ rectangle_mm(-5., -5., 15., 9.) }, region);
        const std::vector<Vec2d> dirs = bridge_directions(region, around, params.tie_tolerance, 8);
        REQUIRE(dirs.size() == 1);
        CHECK(has_direction(dirs, 90.));
    }
    SECTION("round front: ties sample the half circle") {
        const std::vector<Vec2d> dirs = bridge_directions({ ExPolygon(circle_mm(Vec2d::Zero(), 10.)) }, {}, params.tie_tolerance, 8);
        CHECK(dirs.size() >= 4);
        CHECK(dirs.size() <= 8);
        check_canonical(dirs);
        // At least π / max_dirs apart, and every direction is close to a kept one (the greedy packing is maximal).
        for (size_t i = 0; i < dirs.size(); ++ i)
            for (size_t j = i + 1; j < dirs.size(); ++ j)
                CHECK(angle_diff_deg(angle_deg(dirs[i]), angle_deg(dirs[j])) >= 22.5 - 1e-6);
        for (double a = 0.; a < 180.; a += 5.)
            CHECK(has_direction(dirs, a, 25.));
    }
    SECTION("empty input") {
        CHECK(bridge_directions({}, {}, params.tie_tolerance, 8).empty());
    }
}

TEST_CASE("Fronts, clusters and bridge directions of shelves", "[SmartSupports][sas-geometry]")
{
    SECTION("shelf 10 x 15: one wide front, lines outward") {
        const AnalysisInput in   = synthetic_input(shelf_cube(10., 15.), 0.2, 0.2, test_params());
        const BaseModel     base = base_of(in);
        REQUIRE(base.layers.size() == in.layers.size());
        const size_t n0 = layer_at(in, 18.1);
        for (size_t n = 0; n < base.layers.size(); ++ n) {
            const LayerGeom &lg = base.layers[n];
            CHECK(lg.in == &in.layers[n]);
            CHECK(lg.slice_bad.size() == in.layers[n].lslices.size());
            CHECK(lg.slice_tree.size() == in.layers[n].lslices.size());
            CHECK(lg.fronts.size() == size_t(n == n0 ? 1 : 0));
            CHECK(lg.clusters.size() == lg.fronts.size());
            CHECK(lg.tail_needs.empty());
            CHECK(lg.tolerated.empty());
        }
        CHECK(base.layers.front().w == 0);
        CHECK(unscale<double>(base.layers[n0].w) == Approx(0.2).margin(1e-6));
        const FrontGeom &f = base.layers[n0].fronts.front();
        CHECK(f.area_mm2 == Approx(9.8 * 15.).epsilon(0.01));
        CHECK(f.wide);
        REQUIRE(f.thetas.size() == 1);
        CHECK(has_direction(f.thetas, 0.));
        CHECK(f.hoop_hole == -1);
        CHECK(f.slices == std::vector<int>{ 0 });
        const Cluster &cl = base.layers[n0].clusters.front();
        CHECK(cl.fronts == std::vector<uint32_t>{ 0 });
        CHECK(area_mm2(diff_ex(ExPolygons{ f.C }, cl.band)) < 1e-6);
        CHECK(base.trees.empty());
        CHECK(base.last_layer_with_fronts == n0);
    }
    SECTION("shelf 10 x 30 (cube 30): lines along the wall") {
        const AnalysisInput in   = synthetic_input(shelf_cube(10., 30., 30.), 0.2, 0.2, test_params());
        const BaseModel     base = base_of(in);
        const LayerGeom    &lg   = base.layers[layer_at(in, 28.1)];
        REQUIRE(lg.fronts.size() == 1);
        CHECK(lg.fronts.front().thetas.size() == 1);
        CHECK(has_direction(lg.fronts.front().thetas, 90.));
    }
    SECTION("shelf 10 x 20 (tie): both directions") {
        const AnalysisInput in   = synthetic_input(shelf_cube(10., 20.), 0.2, 0.2, test_params());
        const BaseModel     base = base_of(in);
        const LayerGeom    &lg   = base.layers[layer_at(in, 18.1)];
        REQUIRE(lg.fronts.size() == 1);
        CHECK(lg.fronts.front().thetas.size() == 2);
        CHECK(has_direction(lg.fronts.front().thetas, 0.));
        CHECK(has_direction(lg.fronts.front().thetas, 90.));
    }
    SECTION("custom bridge angle fixes θ") {
        Params params = test_params();
        params.custom_bridge_angle_deg = 30.;
        const AnalysisInput in   = synthetic_input(shelf_cube(10., 15.), 0.2, 0.2, params);
        const BaseModel     base = base_of(in);
        const LayerGeom    &lg   = base.layers[layer_at(in, 18.1)];
        REQUIRE(lg.fronts.size() == 1);
        REQUIRE(lg.fronts.front().thetas.size() == 1);
        CHECK(has_direction(lg.fronts.front().thetas, 30.));
    }
    SECTION("thin front: no θ") {
        // Shelf 0.5 deep: front 0.3 wide < 2 extrusion widths.
        const AnalysisInput in   = synthetic_input(shelf_cube(0.5, 15.), 0.2, 0.2, test_params());
        const BaseModel     base = base_of(in);
        const LayerGeom    &lg   = base.layers[layer_at(in, 18.1)];
        REQUIRE(lg.fronts.size() == 1);
        CHECK(! lg.fronts.front().wide);
        CHECK(lg.fronts.front().thetas.empty());
    }
}

TEST_CASE("Weak contact", "[SmartSupports][sas-geometry]")
{
    // A 10 × 10 × 2 plate on a support of the given footprint (2 mm high).
    auto plate_on = [](double sx, double sy) {
        return synthetic_input({ box({ -0.5 * sx, -0.5 * sy, 0. }, { 0.5 * sx, 0.5 * sy, 2. }), box({ -5., -5., 2. }, { 5., 5., 4. }) },
                               0.2, 0.2, test_params());
    };
    SECTION("on a thin post: weak, an island tree") {
        const AnalysisInput in   = plate_on(0.5, 0.5);
        const BaseModel     base = base_of(in);
        const size_t        n    = layer_at(in, 2.1);
        REQUIRE(in.layers[n].lslices.size() == 1);
        CHECK(base.layers[n].slice_bad[0] == 1);
        CHECK(base.layers[n].slice_tree[0] == 0);
        REQUIRE(base.trees.size() == 1);
        CHECK(base.trees[0].first == n);
        CHECK(base.trees[0].merge == UINT32_MAX);
        CHECK(! base.trees[0].tolerated);
        for (size_t k = 0; k < n; ++ k)
            CHECK(base.layers[k].slice_bad == std::vector<uint8_t>(in.layers[k].lslices.size(), 0));
    }
    SECTION("on a thin rib: the overlap is long, not weak") {
        const AnalysisInput in   = plate_on(0.3, 10.);
        const BaseModel     base = base_of(in);
        CHECK(base.layers[layer_at(in, 2.1)].slice_bad[0] == 0);
        CHECK(base.trees.empty());
    }
    SECTION("on a 2 x 2 post: overlap below κ·area but extent ≥ a_min") {
        const AnalysisInput in   = plate_on(2., 2.);
        const BaseModel     base = base_of(in);
        CHECK(base.layers[layer_at(in, 2.1)].slice_bad[0] == 0);
        CHECK(base.trees.empty());
    }
    SECTION("on a base of the same size") {
        const AnalysisInput in   = plate_on(10., 10.);
        const BaseModel     base = base_of(in);
        for (const LayerGeom &lg : base.layers)
            CHECK(lg.slice_bad == std::vector<uint8_t>(lg.in->lslices.size(), 0));
        CHECK(base.trees.empty());
    }
}

TEST_CASE("Hoop candidates", "[SmartSupports][sas-geometry]")
{
    SECTION("dome: every ring is a hoop band with the hole's Feret diameter") {
        const AnalysisInput in   = synthetic_input(dome(20., 25.), 0.2, 0.2, test_params());
        const BaseModel     base = base_of(in);
        int rings = 0;
        for (size_t n = layer_at(in, 15.5); n <= layer_at(in, 19.5); ++ n) {
            const LayerGeom &lg = base.layers[n];
            REQUIRE(! lg.fronts.empty());
            const FrontGeom *ring = nullptr;
            for (const FrontGeom &f : lg.fronts)
                if (! ring || f.area_mm2 > ring->area_mm2)
                    ring = &f;
            INFO("slice_z " << in.layers[n].slice_z);
            CHECK(ring->C.holes.size() == 1);
            CHECK(ring->hoop_hole == 0);
            CHECK(ring->hoop_feret_mm == Approx(2. * std::sqrt(400. - sqr(in.layers[n].slice_z))).margin(0.1));
            ++ rings;
        }
        CHECK(rings >= 19);
    }
    SECTION("lid with one hole: hoop band") {
        const AnalysisInput in   = synthetic_input(lidded_tube(30., { { Vec2d(15., 15.), 8. } }), 0.2, 0.2, test_params());
        const BaseModel     base = base_of(in);
        const LayerGeom    &lg   = base.layers[layer_at(in, 10.1)];
        REQUIRE(lg.fronts.size() == 1);
        CHECK(lg.fronts.front().C.holes.size() == 1);
        CHECK(lg.fronts.front().wide);
        CHECK(lg.fronts.front().hoop_hole == 0);
        CHECK(lg.fronts.front().hoop_feret_mm == Approx(8.).margin(0.01));
    }
    SECTION("lid with four holes: no hoop band") {
        const AnalysisInput in = synthetic_input(lidded_tube(30., { { Vec2d(8., 8.), 4. }, { Vec2d(22., 8.), 4. },
                                                                    { Vec2d(8., 22.), 4. }, { Vec2d(22., 22.), 4. } }),
                                                 0.2, 0.2, test_params());
        const BaseModel  base = base_of(in);
        const LayerGeom &lg   = base.layers[layer_at(in, 10.1)];
        REQUIRE(lg.fronts.size() == 1);
        CHECK(lg.fronts.front().C.holes.size() == 4);
        CHECK(lg.fronts.front().hoop_hole == -1);
    }
    SECTION("halo (free outer edge): no hoop band") {
        const AnalysisInput in   = synthetic_input(halo(15., 12., 13.5, 16.5), 0.2, 0.2, test_params());
        const BaseModel     base = base_of(in);
        const LayerGeom    &lg   = base.layers[layer_at(in, 10.1)];
        REQUIRE(lg.fronts.size() == 1);
        CHECK(lg.fronts.front().C.holes.size() == 1);
        CHECK(lg.fronts.front().hoop_hole == -1);
    }
    SECTION("ring on a post inside the ring: the front's hole is the post, no hoop band") {
        const AnalysisInput in   = synthetic_input(halo(15., 12., 12.8, 14.2), 0.2, 0.2, test_params());
        const BaseModel     base = base_of(in);
        const LayerGeom    &lg   = base.layers[layer_at(in, 10.1)];
        REQUIRE(lg.fronts.size() == 1);
        CHECK(lg.fronts.front().C.holes.size() == 2);
        CHECK(lg.fronts.front().hoop_hole == -1);
    }
}

// WP5 regression (WP2c1a finding): ClipperUtils' offset() drops vertices closer than 0.5 % of the offset distance, so
// a front thinner than ≈ 0.005·m (m = 2.5 mm + w) vanished from offset(∪C, m) and its band was empty.
TEST_CASE("Bands contain thin fronts (two-step offset)", "[SmartSupports][sas-geometry]")
{
    const double arc = scaled<double>(0.005);
    SECTION("offset_keep_thin keeps a 10 µm strip that a plain offset loses") {
        const ExPolygons strip { rectangle_mm(0., 0., 20., 0.01) };
        const float      d = scaled<float>(2.8);
        CHECK(offset(strip, d, ClipperLib::jtRound, arc).empty());       // the Clipper behaviour behind the bug
        // Documented bounds (SmartSupportsInternal.hpp): src ⊕ (d − 2.25·arc − 0.0008·d) ⊂ result ⊂ src ⊕ d; the
        // references use a 0.1 µm arc tolerance (and the strip's axis, since the strip itself degenerates).
        const double   fine  = scaled<double>(0.0001);
        const float    inner = d - float(2.25 * arc + 0.0008 * d + scaled<double>(0.0005));
        const Polygons grown = offset_keep_thin(strip, d, ClipperLib::jtRound, arc);
        const double   exact = 20. * 0.01 + 2. * 20.01 * 2.8 + M_PI * 2.8 * 2.8;
        CHECK(area(grown) * SCALING_FACTOR * SCALING_FACTOR == Approx(exact).epsilon(0.002));
        const Polyline axis { Point(0, scaled<coord_t>(0.005)), Point(scaled<coord_t>(20.), scaled<coord_t>(0.005)) };
        CHECK(area_mm2(diff_ex(offset(axis, inner, ClipperLib::jtRound, fine, ClipperLib::etOpenRound), grown)) < 1e-9);
        CHECK(area_mm2(diff_ex(grown, offset(axis, d + scaled<float>(0.0052), ClipperLib::jtRound, fine, ClipperLib::etOpenRound))) < 1e-9);
        // Wide input: within the same bounds of the exact offset.
        const ExPolygons square { rectangle_mm(0., 0., 5., 5.) };
        const Polygons   sq     = offset_keep_thin(square, d, ClipperLib::jtRound, arc);
        CHECK(area_mm2(diff_ex(offset(square, inner, ClipperLib::jtRound, fine), sq)) < 1e-9);
        CHECK(area_mm2(diff_ex(sq, offset(square, d + scaled<float>(0.0001), ClipperLib::jtRound, fine))) < 1e-9);
        CHECK(offset_keep_thin(ExPolygons(), d).empty());
        CHECK(area_mm2(union_ex(offset_keep_thin(square, 0.f))) == Approx(25.));
    }
    // 44° wall at α 45 (fronts 0.2/tan 44° − 0.2 ≈ 7 µm) and 40° wall at auto α (w = 0.225, fronts ≈ 13 µm).
    for (const auto &[angle, alpha, width_um] : { std::tuple<double, int, double>{ 44., 45, 7.1 }, { 40., 0, 13.4 } }) {
        DYNAMIC_SECTION("wall " << angle << "°, α " << alpha << ": every band holds its fronts") {
            Params params          = test_params();
            params.user.alpha_deg  = alpha;
            const AnalysisInput in   = synthetic_input(slope_wall(angle, 6., 5., 20.), 0.2, 0.2, params);
            const BaseModel     base = base_of(in);
            size_t clusters = 0, plain_empty = 0;
            for (size_t n = 1; n < base.layers.size(); ++ n) {
                const LayerGeom &lg = base.layers[n];
                const coord_t    m  = scaled<coord_t>(params.cluster_margin + params.rho_exact) + lg.w;
                for (const Cluster &cl : lg.clusters) {
                    ++ clusters;
                    ExPolygons Cs;
                    for (uint32_t idx : cl.fronts) {
                        Cs.push_back(lg.fronts[idx].C);
                        // Front width = area / length (20 mm).
                        CHECK(lg.fronts[idx].area_mm2 / 20. * 1000. == Approx(width_um).margin(1.));
                    }
                    CHECK(area_mm2(diff_ex(Cs, cl.band)) < 1e-6);
                    CHECK(! cl.band_prev.empty());
                    CHECK(area_mm2(cl.band) > 20. * 2.5);                     // ≈ 20 × m on the material side
                    plain_empty += offset(Cs, float(m), ClipperLib::jtRound, arc).empty();
                }
            }
            CHECK(clusters > 20);
            CHECK(plain_empty == clusters);                               // the case is really the degenerate one
        }
    }
}

TEST_CASE("Unprintable material", "[SmartSupports][sas-geometry]")
{
    // A 10 × 10 block with a 0.15 mm fin of 5 mm sticking out in +x.
    const AnalysisInput in   = synthetic_input({ box({ 0., 0., 0. }, { 10., 10., 5. }), box({ 9., 5., 0. }, { 15., 5.15, 5. }) },
                                               0.2, 0.2, test_params());
    const BaseModel     base = base_of(in);
    for (size_t n : { size_t(0), layer_at(in, 2.5) }) {
        const ExPolygons &u = base.layers[n].unprintable;
        CHECK(area_mm2(u) == Approx(0.75).epsilon(0.15));
        CHECK(get_extents(u).min.x() >= scaled<coord_t>(9.9));
    }
    const AnalysisInput plain = synthetic_input({ box({ 0., 0., 0. }, { 10., 10., 5. }) }, 0.2, 0.2, test_params());
    for (const LayerGeom &lg : base_of(plain).layers)
        CHECK(area_mm2(lg.unprintable) < 1e-3);
}

TEST_CASE("Phase A is deterministic and cancellable", "[SmartSupports][sas-geometry]")
{
    const AnalysisInput in = synthetic_input(figurine_proxy(), 0.2, 0.2, test_params());
    std::vector<int> pct;
    const BaseModel  a     = build_base_model(in, [] {}, [&pct](int p) { pct.push_back(p); });
    BaseModel        b;
    {
        tbb::global_control single(tbb::global_control::max_allowed_parallelism, 1);
        b = base_of(in);
    }
    REQUIRE(! pct.empty());
    CHECK(pct.front() == 0);
    CHECK(pct.back() == 100);
    CHECK(std::is_sorted(pct.begin(), pct.end()));

    REQUIRE(a.layers.size() == b.layers.size());
    size_t fronts = 0;
    for (size_t n = 0; n < a.layers.size(); ++ n) {
        const LayerGeom &la = a.layers[n], &lb = b.layers[n];
        REQUIRE(la.fronts.size() == lb.fronts.size());
        fronts += la.fronts.size();
        for (size_t i = 0; i < la.fronts.size(); ++ i) {
            CHECK(la.fronts[i].C == lb.fronts[i].C);
            CHECK(la.fronts[i].thetas == lb.fronts[i].thetas);
            CHECK(la.fronts[i].hoop_hole == lb.fronts[i].hoop_hole);
            CHECK(la.fronts[i].slices == lb.fronts[i].slices);
        }
        REQUIRE(la.clusters.size() == lb.clusters.size());
        for (size_t c = 0; c < la.clusters.size(); ++ c) {
            CHECK(la.clusters[c].win == lb.clusters[c].win);
            CHECK(la.clusters[c].fronts == lb.clusters[c].fronts);
            CHECK(la.clusters[c].band == lb.clusters[c].band);
            CHECK(la.clusters[c].band_prev == lb.clusters[c].band_prev);
        }
        CHECK(la.unprintable == lb.unprintable);
        CHECK(la.slice_bad == lb.slice_bad);
        CHECK(la.slice_tree == lb.slice_tree);
    }
    CHECK(fronts > 100);
    CHECK(a.trees.size() == b.trees.size());
    CHECK(a.last_layer_with_fronts == b.last_layer_with_fronts);

    // The cancel callback is called from the worker threads too.
    std::atomic<int> calls { 0 };
    CHECK_THROWS_AS(build_base_model(in, [&calls] { if (++ calls > 3) throw CanceledException(); }, [](int) {}), CanceledException);
}

TEST_CASE("first_tip_layer() is the first layer of Reachability::tips_possible()", "[SmartSupports][sas-geometry]")
{
    // Phase A2 lets slices below it rest on the plate (host test 1); it must agree with the slicer mirror for any
    // top contact gap and raft: tip_layer(n) = n + raft − (z_top + 1) ≥ max(raft − (z_top + 1), 1).
    for (int z_top : { 0, 1, 2, 3 })
        for (int raft : { 0, 1, 2, 3, 5, 8 }) {
            AnalysisInput in;
            in.layers.resize(12);
            in.params.z_top_layers        = z_top;
            in.params.raft_support_layers = raft;
            const Reachability reach(in);
            const uint32_t     first = first_tip_layer(in.params);
            for (uint32_t n = 0; n < 12; ++ n) {
                CHECK(reach.tips_possible(n) == (reach.tip_layer(n) >= std::max(raft - (z_top + 1), 1)));
                CHECK(reach.tips_possible(n) == (n >= first));
            }
        }
}
