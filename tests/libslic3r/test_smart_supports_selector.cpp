// Smart support painting: state encoding, paint rule, PredicateCursor, get_facets_strict(states) (plan §7.1; WP1a).
// WP0: one smoke test of the WP0 declarations; WP1a replaces this file.

#include <catch2/catch_test_macros.hpp>

#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/Support/SmartSupportStates.hpp"

using namespace Slic3r;
using namespace Slic3r::SmartSupports;

TEST_CASE("Smart support states and selector declarations (WP0 stub)", "[SmartSupports][sas-selector]")
{
    for (uint8_t mask = 1; mask <= cbAll; ++ mask) {
        CHECK(is_auto_state(auto_state(mask)));
        CHECK(auto_mask(auto_state(mask)) == mask);
        CHECK(acts_as_enforcer(auto_state(mask)));
    }
    CHECK(! acts_as_enforcer(TriangleStateType::BLOCKER));
    CHECK(enforcer_like_states().size() == 8);

    TriangleMesh     cube(its_make_cube(10., 10., 10.));
    TriangleSelector selector(cube);
    selector.set_facet(0, TriangleStateType::ENFORCER);
    selector.set_facet(1, auto_state(cbIsland));
    CHECK(selector.get_facets_strict(enforcer_like_states()).indices.size() == 2);
    CHECK(selector.get_facets_strict(std::vector<TriangleStateType>{ TriangleStateType::BLOCKER }).indices.empty());
}
