// Smart support painting: accumulated reach — ReachField, ReachCarry, segment minimiser (plan §7.1; WP2b1).
// WP0: one smoke test of the stubs; WP2b1 replaces this file.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "smart_supports_meshes.hpp"
#include "libslic3r/Support/SmartSupportsInternal.hpp"

using namespace Slic3r;
using namespace Slic3r::SmartSupports;
using namespace Slic3r::SmartSupports::Testing;

TEST_CASE("Grid and ReachField stubs (WP0 stub)", "[SmartSupports][sas-reach]")
{
    const BoundingBox bbox(Point(0, 0), Point(scaled<coord_t>(10.), scaled<coord_t>(5.)));
    const Grid        grid = make_grid(bbox, scaled<coord_t>(0.1));
    CHECK(grid.cols > 0);
    CHECK(grid.rows > 0);
    CHECK(grid.v.size() == size_t(grid.cols) * size_t(grid.rows));
    CHECK(grid.m.size() == grid.v.size());

    const Params     params = test_params();
    const AnchorSeg  seg { Line(Point(0, 0), Point(scaled<coord_t>(10.), 0)), 0.f, 0.f };
    const ExPolygons cprime { rectangle_mm(0., 0., 10., 5.) };
    const ReachField field(cprime, cprime, { seg }, 0., params, [] {});
    CHECK(field.value(Point(scaled<coord_t>(5.), scaled<coord_t>(2.))) == Catch::Approx(2.f).margin(1e-4));
    CHECK(field.cropped(bbox) != nullptr);
    CHECK(segment_min(seg, Point(scaled<coord_t>(5.), scaled<coord_t>(2.)), 0.) == Catch::Approx(2.f).margin(1e-4));

    ReachCarry carry(params);
    carry.push(0, 0.2, scaled<coord_t>(0.2), {});
    CHECK(carry.c_hat(Point(0, 0), 1, 0.4, 0.2, scaled<coord_t>(0.2)) >= 0.f);
}
