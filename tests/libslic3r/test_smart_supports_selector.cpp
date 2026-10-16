// Smart support painting: state encoding, paint rule, PredicateCursor, get_facets_strict(states) and the
// FacetsAnnotation helpers (plan §4, §7.1; WP1a).

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <algorithm>
#include <array>
#include <functional>
#include <memory>
#include <optional>

#include "libslic3r/Model.hpp"
#include "libslic3r/PrintBase.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/Support/SmartSupportStates.hpp"

using namespace Slic3r;
using namespace Slic3r::SmartSupports;
using Catch::Approx;

namespace {

constexpr double CUBE = 10.;

// Area of the facets, optionally only of those with all vertices at height z.
double facets_area(const indexed_triangle_set &its, std::optional<float> z = {})
{
    double area = 0.;
    for (const stl_triangle_vertex_indices &f : its.indices) {
        const Vec3d p0 = its.vertices[f(0)].cast<double>(), p1 = its.vertices[f(1)].cast<double>(), p2 = its.vertices[f(2)].cast<double>();
        if (! z || (std::abs(p0.z() - *z) < 1e-4 && std::abs(p1.z() - *z) < 1e-4 && std::abs(p2.z() - *z) < 1e-4))
            area += 0.5 * (p1 - p0).cross(p2 - p0).norm();
    }
    return area;
}

double state_area(const TriangleSelector &selector, TriangleStateType state, std::optional<float> z = {})
{
    return facets_area(selector.get_facets_strict(state), z);
}

// Original facets of the mesh with all vertices at height z.
std::vector<int> facets_at_z(const TriangleMesh &mesh, float z)
{
    std::vector<int> out;
    for (size_t i = 0; i < mesh.its.indices.size(); ++ i) {
        const stl_triangle_vertex_indices &f = mesh.its.indices[i];
        if (mesh.its.vertices[f(0)].z() == z && mesh.its.vertices[f(1)].z() == z && mesh.its.vertices[f(2)].z() == z)
            out.push_back(int(i));
    }
    return out;
}

// Original facets in the plane x = const.
std::vector<int> facets_at_x(const TriangleMesh &mesh, float x)
{
    std::vector<int> out;
    for (size_t i = 0; i < mesh.its.indices.size(); ++ i) {
        const stl_triangle_vertex_indices &f = mesh.its.indices[i];
        if (mesh.its.vertices[f(0)].x() == x && mesh.its.vertices[f(1)].x() == x && mesh.its.vertices[f(2)].x() == x)
            out.push_back(int(i));
    }
    return out;
}

// Original horizontal facet containing p (tested in XY), -1 if none.
int facet_at(const TriangleMesh &mesh, const Vec3f &p)
{
    auto side = [](const Vec2f &u, const Vec2f &v, const Vec2f &q) { return (v.x() - u.x()) * (q.y() - u.y()) - (v.y() - u.y()) * (q.x() - u.x()); };
    for (int i : facets_at_z(mesh, p.z())) {
        const stl_triangle_vertex_indices &f = mesh.its.indices[i];
        const Vec2f a = mesh.its.vertices[f(0)].head<2>(), b = mesh.its.vertices[f(1)].head<2>(), c = mesh.its.vertices[f(2)].head<2>();
        const Vec2f q = p.head<2>();
        const float s0 = side(a, b, q), s1 = side(b, c, q), s2 = side(c, a, q);
        if ((s0 >= 0 && s1 >= 0 && s2 >= 0) || (s0 <= 0 && s1 <= 0 && s2 <= 0))
            return i;
    }
    return -1;
}

// A stroke of the sphere brush of the paint gizmo.
void paint_sphere(TriangleSelector &selector, const TriangleMesh &mesh, const Vec3f &center, float radius, TriangleStateType state)
{
    const int facet = facet_at(mesh, center);
    REQUIRE(facet >= 0);
    const Transform3d trafo = Transform3d::Identity();
    selector.select_patch(facet,
        TriangleSelector::SinglePointCursor::cursor_factory(center, center - Vec3f(0.f, 0.f, 10.f), radius,
            TriangleSelector::CursorType::SPHERE, trafo, TriangleSelector::ClippingPlane()),
        state, trafo, true);
}

// Smart paint through the PredicateCursor as the smart painter does: rule of one cause bit, downward facets only.
// point_inside defines a convex region, thus three vertices inside mean the whole triangle is inside.
void paint_rule(TriangleSelector &selector, const std::vector<int> &seeds, std::function<bool(const Vec3f&)> point_inside,
                uint8_t cause_bit, bool splitting = true, float edge_limit = 0.05f)
{
    TriangleSelector::PredicateCursor::Predicates predicates;
    predicates.point_inside = std::move(point_inside);
    predicates.seed_facets  = seeds;
    const Transform3d    trafo = Transform3d::Identity();
    const PaintRuleScope rule(selector, tool_paint_rule(cause_bit));
    selector.select_patch(seeds.empty() ? 0 : seeds.front(), std::make_unique<TriangleSelector::PredicateCursor>(std::move(predicates), edge_limit, trafo),
                          auto_state(cause_bit), trafo, splitting, 89.99f);
}

bool on_bottom(const Vec3f &p) { return p.z() < 1e-3f; }

// Cube 10 mm with a manual enforcer spot at (3, 3) and a manual blocker spot at (7, 7) on its bottom.
struct SpottedCube {
    TriangleMesh     mesh { its_make_cube(CUBE, CUBE, CUBE) };
    TriangleSelector selector { mesh };
    std::vector<int> bottom = facets_at_z(mesh, 0.f);
    double           enf_area = 0., blk_area = 0.;
    int              enf_leaves = 0, blk_leaves = 0;

    SpottedCube() {
        paint_sphere(selector, mesh, Vec3f(3.f, 3.f, 0.f), 1.5f, TriangleStateType::ENFORCER);
        paint_sphere(selector, mesh, Vec3f(7.f, 7.f, 0.f), 1.5f, TriangleStateType::BLOCKER);
        enf_area   = state_area(selector, TriangleStateType::ENFORCER);
        blk_area   = state_area(selector, TriangleStateType::BLOCKER);
        enf_leaves = selector.num_facets(TriangleStateType::ENFORCER);
        blk_leaves = selector.num_facets(TriangleStateType::BLOCKER);
    }
};

} // namespace

TEST_CASE("Smart paint state encoding", "[SmartSupports][sas-selector]")
{
    for (int s = 0; s < int(TRIANGLE_STATE_TYPE_COUNT); ++ s) {
        const auto state = TriangleStateType(s);
        CHECK(acts_as_enforcer(state) == (s == 1 || (s >= 3 && s <= 9)));
        CHECK(is_auto_state(state) == (s >= 3 && s <= 9));
        CHECK(int(auto_mask(state)) == (is_auto_state(state) ? s - 2 : 0));
    }
    for (uint8_t mask = 1; mask <= cbAll; ++ mask)
        CHECK(auto_mask(auto_state(mask)) == mask);
    CHECK(auto_state(0) == TriangleStateType::NONE);
    CHECK(int(auto_state(cbIsland | cbCantilever)) == 5);
    const std::vector<TriangleStateType> enforcer_like = enforcer_like_states();
    REQUIRE(enforcer_like.size() == 8);
    CHECK(enforcer_like.front() == TriangleStateType::ENFORCER);
    for (size_t i = 1; i < enforcer_like.size(); ++ i)
        CHECK(int(enforcer_like[i]) == int(i) + 2);

    SECTION("remap maps and the tool rule") {
        const auto clear_island = clear_causes_map(cbIsland);
        CHECK(clear_island.size() == 7);
        CHECK(clear_island.at(auto_state(cbIsland)) == TriangleStateType::NONE);
        CHECK(clear_island.at(auto_state(cbIsland | cbCantilever)) == auto_state(cbCantilever));
        CHECK(clear_island.at(auto_state(cbCantilever)) == auto_state(cbCantilever));
        CHECK(clear_island.count(TriangleStateType::ENFORCER) == 0);
        CHECK(clear_island.count(TriangleStateType::BLOCKER) == 0);
        for (const auto &[from, to] : clear_causes_map(cbAll))
            CHECK(to == TriangleStateType::NONE);
        for (const auto &[from, to] : bake_map()) {
            CHECK(is_auto_state(from));
            CHECK(to == TriangleStateType::ENFORCER);
        }
        const TriangleSelector::PaintRule rule = tool_paint_rule(cbCantilever);
        CHECK(rule(TriangleStateType::NONE, TriangleStateType::BLOCKER) == auto_state(cbCantilever));
        CHECK(rule(auto_state(cbIsland), TriangleStateType::NONE) == auto_state(cbIsland | cbCantilever));
        CHECK(rule(auto_state(cbCantilever), TriangleStateType::NONE) == auto_state(cbCantilever));
        CHECK(rule(TriangleStateType::ENFORCER, TriangleStateType::NONE) == TriangleStateType::ENFORCER);
        CHECK(rule(TriangleStateType::BLOCKER, TriangleStateType::NONE) == TriangleStateType::BLOCKER);
        CHECK(rule(TriangleStateType(20), TriangleStateType::NONE) == TriangleStateType(20));
    }

    SECTION("serialization round trip, painting version 1") {
        TriangleMesh     mesh(its_make_cube(CUBE, CUBE, CUBE));
        TriangleSelector selector(mesh);
        // States 0..9 on the facets 2..11, split facets with manual and auto states on the bottom (facets 0, 1).
        REQUIRE(facets_at_z(mesh, 0.f) == std::vector<int>{ 0, 1 });
        for (int i = 2; i < 12; ++ i)
            selector.set_facet(i, TriangleStateType(i - 2));
        paint_sphere(selector, mesh, Vec3f(3.f, 3.f, 0.f), 1.5f, TriangleStateType::ENFORCER);
        paint_rule(selector, { 0, 1 }, [](const Vec3f &p) { return on_bottom(p) && p.x() < 5.f; }, cbLongSpan);
        const TriangleSelector::TriangleSplittingData data = selector.serialize();
        CHECK(data.minimum_required_painting_version() == 1);
        for (int s = 0; s <= 9; ++ s)
            CHECK(data.used_states[s]);
        CHECK(! data.used_states[10]);

        TriangleSelector loaded(mesh);
        loaded.deserialize(data);
        CHECK(loaded.serialize() == data);
        for (int s = 0; s <= 9; ++ s)
            CHECK(state_area(loaded, TriangleStateType(s)) == Approx(state_area(selector, TriangleStateType(s))).margin(1e-9));
    }
}

TEST_CASE("Paint rule keeps manual paint (R2)", "[SmartSupports][sas-selector]")
{
    SpottedCube cube;
    REQUIRE(cube.enf_area > 5.);
    REQUIRE(cube.blk_area > 5.);
    const double bottom_area = CUBE * CUBE;

    SECTION("whole bottom: fully covered facets keep their subdivision") {
        paint_rule(cube.selector, cube.bottom, on_bottom, cbIsland);
        CHECK(state_area(cube.selector, TriangleStateType::ENFORCER) == Approx(cube.enf_area).margin(1e-6));
        CHECK(state_area(cube.selector, TriangleStateType::BLOCKER) == Approx(cube.blk_area).margin(1e-6));
        CHECK(state_area(cube.selector, auto_state(cbIsland), 0.f) == Approx(bottom_area - cube.enf_area - cube.blk_area).margin(1e-6));
        CHECK(cube.selector.num_facets(TriangleStateType::ENFORCER) == cube.enf_leaves);
        CHECK(cube.selector.num_facets(TriangleStateType::BLOCKER) == cube.blk_leaves);
        // Only the bottom is painted, all of it.
        const indexed_triangle_set painted = cube.selector.get_facets_strict(enforcer_like_states());
        CHECK(facets_area(painted) == Approx(facets_area(painted, 0.f)).margin(1e-9));
        CHECK(facets_area(cube.selector.get_facets_strict(TriangleStateType::NONE), 0.f) == Approx(0.).margin(1e-9));
    }

    SECTION("partially covered: protected leaves are never split, re-runs are identical") {
        // x = 7 crosses the blocker spot, the enforcer spot lies inside.
        auto region = [](const Vec3f &p) { return on_bottom(p) && p.x() < 7.f; };
        paint_rule(cube.selector, cube.bottom, region, cbIsland);
        CHECK(state_area(cube.selector, TriangleStateType::ENFORCER) == Approx(cube.enf_area).margin(1e-6));
        CHECK(state_area(cube.selector, TriangleStateType::BLOCKER) == Approx(cube.blk_area).margin(1e-6));
        CHECK(cube.selector.num_facets(TriangleStateType::ENFORCER) == cube.enf_leaves);
        CHECK(cube.selector.num_facets(TriangleStateType::BLOCKER) == cube.blk_leaves);
        // Leaves crossing x = 7 at the edge limit (0.05 mm) stay unpainted.
        const double auto_area = state_area(cube.selector, auto_state(cbIsland));
        CHECK(auto_area <= 0.7 * bottom_area - cube.enf_area + 1e-6);
        CHECK(auto_area >= 0.7 * bottom_area - cube.enf_area - cube.blk_area - 0.6);

        // The same paint again, and the smart painter's re-run (clear all smart paint, paint), give the same tree.
        const TriangleSelector::TriangleSplittingData first = cube.selector.serialize();
        paint_rule(cube.selector, cube.bottom, region, cbIsland);
        CHECK(FacetsAnnotation::same_paint(cube.selector.serialize(), first));
        for (int run = 0; run < 3; ++ run) {
            cube.selector.remap_states(clear_causes_map(cbAll));
            paint_rule(cube.selector, cube.bottom, region, cbIsland);
            CHECK(FacetsAnnotation::same_paint(cube.selector.serialize(), first));
        }
    }

    SECTION("without a rule the cursor overwrites manual paint (stock behaviour)") {
        TriangleSelector::PredicateCursor::Predicates predicates;
        predicates.point_inside = on_bottom;
        predicates.seed_facets  = cube.bottom;
        cube.selector.select_patch(cube.bottom.front(), std::make_unique<TriangleSelector::PredicateCursor>(predicates, 0.05f, Transform3d::Identity()),
                                   auto_state(cbIsland), Transform3d::Identity(), true, 89.99f);
        CHECK(state_area(cube.selector, auto_state(cbIsland)) == Approx(bottom_area).margin(1e-6));
        CHECK(! cube.selector.has_facets(TriangleStateType::ENFORCER));
        CHECK(! cube.selector.has_facets(TriangleStateType::BLOCKER));
        // Fully covered facets were undivided.
        CHECK(cube.selector.num_facets(auto_state(cbIsland)) == int(cube.bottom.size()));
    }

    SECTION("an identity rule equals no rule") {
        SpottedCube other;
        auto strokes = [](SpottedCube &c, bool rule) {
            if (rule)
                c.selector.set_paint_rule([](TriangleStateType, TriangleStateType requested) { return requested; });
            paint_sphere(c.selector, c.mesh, Vec3f(5.f, 5.f, 0.f), 3.f, TriangleStateType::ENFORCER);
            paint_sphere(c.selector, c.mesh, Vec3f(2.5f, 7.f, 0.f), 1.9f, TriangleStateType::NONE);
            c.selector.clear_paint_rule();
        };
        strokes(cube, false);
        strokes(other, true);
        CHECK(cube.selector.serialize() == other.selector.serialize());
    }

    SECTION("no splitting: the rule is applied to partially covered leaves") {
        TriangleMesh     mesh(its_make_cube(CUBE, CUBE, CUBE));
        TriangleSelector selector(mesh);
        const std::vector<int> bottom = facets_at_z(mesh, 0.f);
        REQUIRE(bottom.size() == 2);
        selector.set_facet(bottom[1], TriangleStateType::ENFORCER);
        // Both bottom facets are cut by x = 5.
        paint_rule(selector, bottom, [](const Vec3f &p) { return on_bottom(p) && p.x() < 5.f; }, cbCantilever, false);
        CHECK(selector.num_facets(auto_state(cbCantilever)) == 1);
        CHECK(selector.num_facets(TriangleStateType::ENFORCER) == 1);
        CHECK(state_area(selector, auto_state(cbCantilever)) == Approx(0.5 * CUBE * CUBE).margin(1e-6));
    }
}

TEST_CASE("Paint rule ORs causes, remap clears them", "[SmartSupports][sas-selector]")
{
    SpottedCube cube;
    const std::vector<int> &bottom = cube.bottom;
    REQUIRE(bottom.size() == 2);
    // Islands on the whole bottom, overhangs on the first bottom facet only: the cursor accepts everything,
    // but only the seed facets are visited (no breadth-first expansion).
    paint_rule(cube.selector, bottom, on_bottom, cbIsland);
    paint_rule(cube.selector, { bottom.front() }, [](const Vec3f&) { return true; }, cbCantilever);

    const double free_area = CUBE * CUBE - cube.enf_area - cube.blk_area;
    const double both      = state_area(cube.selector, auto_state(cbIsland | cbCantilever));
    const double island    = state_area(cube.selector, auto_state(cbIsland));
    CHECK(both > 0.);
    CHECK(island > 0.);
    CHECK(both + island == Approx(free_area).margin(1e-6));
    CHECK(! cube.selector.has_facets(auto_state(cbCantilever)));
    CHECK(state_area(cube.selector, TriangleStateType::ENFORCER) == Approx(cube.enf_area).margin(1e-6));
    CHECK(state_area(cube.selector, TriangleStateType::BLOCKER) == Approx(cube.blk_area).margin(1e-6));

    SECTION("get_facets_strict(states) = sum of the single states") {
        const std::vector<TriangleStateType> states { TriangleStateType::ENFORCER, auto_state(cbIsland), auto_state(cbIsland | cbCantilever) };
        double sum = 0.;
        for (TriangleStateType s : states)
            sum += state_area(cube.selector, s);
        CHECK(facets_area(cube.selector.get_facets_strict(states)) == Approx(sum).margin(1e-6));
        CHECK(facets_area(cube.selector.get_facets_strict(enforcer_like_states())) == Approx(cube.enf_area + free_area).margin(1e-6));
        CHECK(cube.selector.get_facets_strict(std::vector<TriangleStateType>{}).indices.empty());
    }

    SECTION("clear one cause") {
        cube.selector.remap_states(clear_causes_map(cbIsland));
        CHECK(state_area(cube.selector, auto_state(cbCantilever)) == Approx(both).margin(1e-6));
        CHECK(! cube.selector.has_facets(auto_state(cbIsland)));
        CHECK(! cube.selector.has_facets(auto_state(cbIsland | cbCantilever)));
        CHECK(state_area(cube.selector, TriangleStateType::ENFORCER) == Approx(cube.enf_area).margin(1e-6));
    }

    SECTION("clear all causes leaves manual paint only") {
        SpottedCube reference;
        cube.selector.remap_states(clear_causes_map(cbAll));
        for (uint8_t mask = 1; mask <= cbAll; ++ mask)
            CHECK(! cube.selector.has_facets(auto_state(mask)));
        CHECK(state_area(cube.selector, TriangleStateType::ENFORCER) == Approx(cube.enf_area).margin(1e-6));
        CHECK(state_area(cube.selector, TriangleStateType::BLOCKER) == Approx(cube.blk_area).margin(1e-6));
        // The original manual paint is restored exactly.
        CHECK(FacetsAnnotation::same_paint(cube.selector.serialize(), reference.selector.serialize()));
    }
}

TEST_CASE("PredicateCursor", "[SmartSupports][sas-selector]")
{
    TriangleMesh mesh(its_make_cube(CUBE, CUBE, CUBE));

    SECTION("seeds only, also without an angle limit") {
        TriangleSelector selector(mesh);
        const std::vector<int> top = facets_at_z(mesh, float(CUBE));
        TriangleSelector::PredicateCursor::Predicates predicates;
        predicates.point_inside = [](const Vec3f&) { return true; };
        predicates.seed_facets  = { top.front() };
        selector.select_patch(top.front(), std::make_unique<TriangleSelector::PredicateCursor>(predicates, 0.1f, Transform3d::Identity()),
                              TriangleStateType::ENFORCER, Transform3d::Identity(), true);
        CHECK(selector.num_facets(TriangleStateType::ENFORCER) == 1);
        CHECK(state_area(selector, TriangleStateType::ENFORCER, float(CUBE)) == Approx(0.5 * CUBE * CUBE));
    }

    SECTION("empty seeds: nothing happens") {
        SpottedCube cube;
        const TriangleSelector::TriangleSplittingData before = cube.selector.serialize();
        paint_rule(cube.selector, {}, [](const Vec3f&) { return true; }, cbIsland);
        CHECK(cube.selector.serialize() == before);
    }

    SECTION("world coordinates: the predicates see the transformed mesh") {
        // Scale 2 in x, then shift 100 in x: the bottom spans x in [100, 120] in world coordinates.
        Transform3d trafo = Transform3d::Identity();
        trafo.translate(Vec3d(100., 0., 0.));
        trafo.scale(Vec3d(2., 1., 1.));
        TriangleSelector selector(mesh);
        TriangleSelector::PredicateCursor::Predicates predicates;
        predicates.point_inside = [](const Vec3f &p) { return p.z() < 1e-3f && p.x() < 110.f; };
        predicates.seed_facets  = facets_at_z(mesh, 0.f);
        selector.set_paint_rule(tool_paint_rule(cbIsland));
        selector.select_patch(predicates.seed_facets.front(), std::make_unique<TriangleSelector::PredicateCursor>(predicates, 0.05f, trafo),
                              auto_state(cbIsland), Transform3d(trafo.linear()), true, 89.99f);
        selector.clear_paint_rule();
        // x < 110 in world coordinates is x < 5 in mesh coordinates (areas in mesh coordinates).
        const double area = state_area(selector, auto_state(cbIsland));
        CHECK(area <= 0.5 * CUBE * CUBE + 1e-6);
        CHECK(area >= 0.5 * CUBE * CUBE - 0.5);
    }

    SECTION("classify and touches") {
        TriangleSelector selector(mesh);
        int classify_calls = 0, touches_calls = 0;
        auto in_disk = [](const Vec3f &p) { return p.z() < 1e-3f && (p - Vec3f(5.f, 5.f, 0.f)).norm() < 2.f; };
        TriangleSelector::PredicateCursor::Predicates predicates;
        predicates.point_inside = in_disk;
        // The disk lies strictly inside the bottom: no vertex of an original facet is inside it, only touches()
        // lets the cursor descend.
        predicates.classify = [&](const std::array<Vec3f, 3> &tri) {
            ++ classify_calls;
            return int(in_disk(tri[0])) + int(in_disk(tri[1])) + int(in_disk(tri[2]));
        };
        predicates.touches = [&](const std::array<Vec3f, 3> &tri) {
            ++ touches_calls;
            Vec3f lo = tri[0], hi = tri[0];
            for (const Vec3f &p : tri) {
                lo = lo.cwiseMin(p);
                hi = hi.cwiseMax(p);
            }
            return lo.z() < 1e-3f && lo.x() < 7.f && hi.x() > 3.f && lo.y() < 7.f && hi.y() > 3.f;
        };
        predicates.seed_facets = facets_at_z(mesh, 0.f);
        selector.set_paint_rule(tool_paint_rule(cbIsland));
        selector.select_patch(predicates.seed_facets.front(), std::make_unique<TriangleSelector::PredicateCursor>(predicates, 0.05f, Transform3d::Identity()),
                              auto_state(cbIsland), Transform3d::Identity(), true, 89.99f);
        selector.clear_paint_rule();
        CHECK(classify_calls > 0);
        CHECK(touches_calls > 0);
        // The disk of radius 2 without the leaves crossing its border.
        const double area = state_area(selector, auto_state(cbIsland));
        CHECK(area <= M_PI * 4. + 1e-6);
        CHECK(area >= M_PI * 4. - 1.);
    }

    SECTION("a cancel thrown from a predicate leaves the selector valid") {
        SpottedCube cube;
        int calls = 0;
        auto half = [&calls](const Vec3f &p) {
            if (++ calls == 200)
                throw CanceledException();
            return on_bottom(p) && p.x() < 5.f;
        };
        REQUIRE_THROWS_AS(paint_rule(cube.selector, cube.bottom, half, cbIsland), CanceledException);
        CHECK(calls == 200);

        // The rule went with the scope (review A M3): a manual stroke paints what it asks for, not smart paint.
        {
            const std::vector<int> side = facets_at_x(cube.mesh, CUBE);
            REQUIRE(! side.empty());
            const Vec3f c(CUBE, 0.5f * CUBE, 0.5f * CUBE);
            cube.selector.select_patch(side.front(), TriangleSelector::SinglePointCursor::cursor_factory(c, c + Vec3f(10.f, 0.f, 0.f), 1.f,
                                           TriangleSelector::CursorType::SPHERE, Transform3d::Identity(), TriangleSelector::ClippingPlane()),
                                       TriangleStateType::ENFORCER, Transform3d::Identity(), true);
            CHECK(state_area(cube.selector, TriangleStateType::ENFORCER) > cube.enf_area + 1.);
            const double side_auto = state_area(cube.selector, auto_state(cbIsland));
            cube.selector.select_patch(side.front(), TriangleSelector::SinglePointCursor::cursor_factory(c, c + Vec3f(10.f, 0.f, 0.f), 1.f,
                                           TriangleSelector::CursorType::SPHERE, Transform3d::Identity(), TriangleSelector::ClippingPlane()),
                                       TriangleStateType::NONE, Transform3d::Identity(), true);
            CHECK(state_area(cube.selector, TriangleStateType::ENFORCER) == Approx(cube.enf_area).margin(1e-6));
            CHECK(state_area(cube.selector, auto_state(cbIsland)) == Approx(side_auto).margin(1e-6));
        }

        // The tree is consistent: serialization round trip, manual paint intact, the surface complete.
        const TriangleSelector::TriangleSplittingData data = cube.selector.serialize();
        TriangleSelector loaded(cube.mesh);
        loaded.deserialize(data);
        CHECK(loaded.serialize() == data);
        CHECK(state_area(cube.selector, TriangleStateType::ENFORCER) == Approx(cube.enf_area).margin(1e-6));
        CHECK(state_area(cube.selector, TriangleStateType::BLOCKER) == Approx(cube.blk_area).margin(1e-6));
        CHECK(facets_area(cube.selector.get_all_facets_strict()) == Approx(6. * CUBE * CUBE).margin(1e-6));

        // It can be painted again.
        paint_rule(cube.selector, cube.bottom, on_bottom, cbIsland);
        CHECK(state_area(cube.selector, auto_state(cbIsland)) == Approx(CUBE * CUBE - cube.enf_area - cube.blk_area).margin(1e-6));
    }
}

TEST_CASE("Seed fill ignores the paint rule", "[SmartSupports][sas-selector]")
{
    TriangleMesh     mesh(its_make_cube(CUBE, CUBE, CUBE));
    TriangleSelector selector(mesh);
    paint_rule(selector, facets_at_z(mesh, 0.f), on_bottom, cbCantilever);
    REQUIRE(state_area(selector, auto_state(cbCantilever)) == Approx(CUBE * CUBE));

    // Bucket fill selects the smart painted region (same state); the requested state is applied as is.
    const Vec3f hit(2.f, 5.f, 0.f);
    selector.set_paint_rule(tool_paint_rule(cbIsland));
    selector.bucket_fill_select_triangles(hit, facet_at(mesh, hit), TriangleSelector::ClippingPlane(), 30.f, 0.f,
                                          TriangleSelector::BucketFillPropagate::YES);
    selector.seed_fill_apply_on_triangles(TriangleStateType::BLOCKER);
    CHECK(state_area(selector, TriangleStateType::BLOCKER) == Approx(CUBE * CUBE));
    CHECK(! selector.has_facets(auto_state(cbCantilever)));
    CHECK(! selector.has_facets(auto_state(cbIsland | cbCantilever)));

    // Smart fill with an auto state.
    selector.seed_fill_unselect_all_triangles();
    selector.seed_fill_select_triangles(hit, facet_at(mesh, hit), Transform3d::Identity(), TriangleSelector::ClippingPlane(), 30.f, 0.f);
    selector.seed_fill_apply_on_triangles(auto_state(cbLongSpan));
    selector.clear_paint_rule();
    CHECK(state_area(selector, auto_state(cbLongSpan)) == Approx(CUBE * CUBE));
    CHECK(! selector.has_facets(TriangleStateType::BLOCKER));
}

TEST_CASE("FacetsAnnotation smart paint helpers", "[SmartSupports][sas-selector]")
{
    Model             model;
    ModelObject      *mo = model.add_object();
    ModelVolume      *mv = mo->add_volume(TriangleMesh(its_make_cube(CUBE, CUBE, CUBE)));
    // add_volume() centres the mesh, the helpers of this file expect the cube at [0, 10]³.
    mv->set_mesh(its_make_cube(CUBE, CUBE, CUBE));
    FacetsAnnotation &fa = mv->supported_facets;
    CHECK(! fa.has_auto_states());
    CHECK(! fa.has_enforcer_like_facets());

    auto set_paint = [mv](const std::function<void(TriangleSelector&)> &paint) {
        TriangleSelector selector(mv->mesh());
        paint(selector);
        mv->supported_facets.set(selector);
    };
    // has_auto_states() / has_enforcer_like_facets() through one TriangleSelector::has_facets() scan per state.
    auto reference = [&fa]() {
        bool any_auto = false;
        for (uint8_t mask = 1; mask <= cbAll; ++ mask)
            any_auto |= TriangleSelector::has_facets(fa.get_data(), auto_state(mask));
        return std::make_pair(any_auto, any_auto || TriangleSelector::has_facets(fa.get_data(), TriangleStateType::ENFORCER));
    };

    SECTION("the single pass scan equals the per state scans") {
        const TriangleMesh &mesh = mv->mesh();
        const std::vector<std::function<void(TriangleSelector&)>> paints {
            [](TriangleSelector &s) { s.set_facet(3, TriangleStateType::BLOCKER); },
            [](TriangleSelector &s) { s.set_facet(3, TriangleStateType::ENFORCER); },
            [](TriangleSelector &s) { s.set_facet(11, auto_state(cbAll)); },
            [](TriangleSelector &s) { s.set_facet(5, TriangleStateType(20)); s.set_facet(6, TriangleStateType(255)); s.set_facet(7, TriangleStateType(16)); },
            [](TriangleSelector &s) { s.set_facet(5, TriangleStateType(20)); s.set_facet(9, auto_state(cbIsland)); },
            [&mesh](TriangleSelector &s) {
                paint_sphere(s, mesh, Vec3f(3.f, 3.f, 0.f), 1.5f, TriangleStateType::BLOCKER);
                paint_rule(s, facets_at_z(mesh, 0.f), on_bottom, cbLongSpan);
            },
            [&mesh](TriangleSelector &s) {
                paint_sphere(s, mesh, Vec3f(3.f, 3.f, 0.f), 1.5f, TriangleStateType::BLOCKER);
                paint_sphere(s, mesh, Vec3f(6.f, 3.f, 0.f), 1.5f, TriangleStateType(17));
            },
        };
        for (size_t i = 0; i < paints.size(); ++ i) {
            INFO("paint " << i);
            set_paint(paints[i]);
            const auto [any_auto, any_enforcer_like] = reference();
            CHECK(fa.has_auto_states() == any_auto);
            CHECK(fa.has_enforcer_like_facets() == any_enforcer_like);
        }
    }

    SECTION("get_facets_strict(mv, states), bake, same_paint, reset") {
        set_paint([mv](TriangleSelector &s) {
            paint_sphere(s, mv->mesh(), Vec3f(3.f, 3.f, 0.f), 1.5f, TriangleStateType::ENFORCER);
            paint_sphere(s, mv->mesh(), Vec3f(7.f, 7.f, 0.f), 1.5f, TriangleStateType::BLOCKER);
            paint_rule(s, facets_at_z(mv->mesh(), 0.f), on_bottom, cbIsland);
        });
        CHECK(fa.has_auto_states());
        CHECK(fa.has_enforcer_like_facets());
        const double enf_like = facets_area(fa.get_facets_strict(*mv, enforcer_like_states()));
        const double blk      = facets_area(fa.get_facets_strict(*mv, TriangleStateType::BLOCKER));
        CHECK(enf_like + blk == Approx(CUBE * CUBE).margin(1e-6));

        TriangleSelector baked(mv->mesh());
        baked.deserialize(fa.get_data_auto_baked(*mv));
        for (uint8_t mask = 1; mask <= cbAll; ++ mask)
            CHECK(! baked.has_facets(auto_state(mask)));
        CHECK(state_area(baked, TriangleStateType::ENFORCER) == Approx(enf_like).margin(1e-6));
        CHECK(state_area(baked, TriangleStateType::BLOCKER) == Approx(blk).margin(1e-6));
        CHECK(fa.get_data_auto_baked(*mv).minimum_required_painting_version() == 1);

        // same_paint() ignores used_states.
        TriangleSelector::TriangleSplittingData data = fa.get_data();
        data.used_states[42] = true;
        CHECK(FacetsAnnotation::same_paint(data, fa.get_data()));
        CHECK(data != fa.get_data());

        // triangle_as_string() is the 3MF string of get_triangle_as_string().
        for (int i = 0; i < int(mv->mesh().its.indices.size()); ++ i)
            CHECK(FacetsAnnotation::triangle_as_string(fa.get_data(), i) == fa.get_triangle_as_string(i));

        // reset() is stock again (review A M4): the paint goes, used_states stay (only the 3MF painting version reads them).
        const auto used = fa.get_data().used_states;
        fa.reset();
        CHECK(! fa.has_auto_states());
        CHECK(! fa.has_enforcer_like_facets());
        CHECK(fa.get_data().used_states == used);
    }

    SECTION("data after the end of a tree is ignored as by deserialize() (review A M2)") {
        // Strings as in a 3MF (the last digit is the first code): a BLOCKER leaf, then a stray ENFORCER code / a stray
        // auto(1) leaf (two nibbles) that no tree reaches.
        for (const char *str : { "48", "0C8" }) {
            INFO(str);
            fa.reset();
            fa.reserve(12);
            fa.set_triangle_from_string(0, str);
            fa.shrink_to_fit();
            REQUIRE(TriangleSelector::has_facets(fa.get_data(), TriangleStateType::BLOCKER));
            CHECK(! TriangleSelector::has_facets(fa.get_data(), TriangleStateType::ENFORCER));
            CHECK(! fa.has_enforcer_like_facets());
            CHECK(! fa.has_auto_states());
        }
        // A split root (code 1: two children) with an auto(1) leaf (codes C, 0) and a BLOCKER leaf (8): found.
        fa.reset();
        fa.reserve(12);
        fa.set_triangle_from_string(0, "80C1");
        fa.shrink_to_fit();
        const auto [any_auto, any_enforcer_like] = reference();
        CHECK(any_auto);
        CHECK(fa.has_auto_states() == any_auto);
        CHECK(fa.has_enforcer_like_facets() == any_enforcer_like);
    }
}
