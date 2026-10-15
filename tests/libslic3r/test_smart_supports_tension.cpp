// Smart support painting: tension, chord rule, hoop credit, classification (plan §7.1; WP2b2).
// WP0: one smoke test of the stubs with a synthetic ReachQuery; WP2b2 replaces this file.

#include <catch2/catch_test_macros.hpp>

#include "smart_supports_meshes.hpp"
#include "libslic3r/Support/SmartSupportsInternal.hpp"

using namespace Slic3r;
using namespace Slic3r::SmartSupports;
using namespace Slic3r::SmartSupports::Testing;

namespace {
// c(p) = x in mm.
struct ReachX final : ReachQuery {
    float   value(const Point &p) const override { return float(unscale<double>(p.x())); }
    coord_t res() const override { return scaled<coord_t>(0.1); }
    bool    direct_mode() const override { return true; }
};
}

TEST_CASE("classify_front by c (WP0 stub)", "[SmartSupports][sas-tension]")
{
    const Params     params = test_params();
    const ExPolygons cprime { rectangle_mm(0., 0., 10., 5.) };
    FrontGeom        front;
    front.C = cprime.front();
    const DomainEdges edges   = domain_edges(cprime, {});
    const Anchors     anchors = compute_anchors(cprime, front.C, {}, {}, cprime);
    const TensionMap  tension = tension_levels(cprime, front, cprime, edges, anchors, params);
    REQUIRE(edges.anchor != nullptr);
    REQUIRE(edges.free_ != nullptr);
    const FrontNeeds needs = classify_front(cprime, ReachX(), tension, hoop_credit(cprime, front, true, params), 0.f, params);
    // K_max 3, f_hard 1.5: should for x ∈ (3, 4.5], must beyond.
    CHECK(! needs.need[0][int(Level::Should)].empty());
    CHECK(! needs.need[0][int(Level::Must)].empty());
    CHECK(needs.max_c > 9.f);
}
