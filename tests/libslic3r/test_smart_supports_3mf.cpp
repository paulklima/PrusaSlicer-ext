// Smart support painting: 3MF round trip, bake, legacy files (plan §7.1; WP1a).
// WP0: one smoke test of the FacetsAnnotation helpers; WP1a replaces this file.

#include <catch2/catch_test_macros.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/Support/SmartSupportStates.hpp"

using namespace Slic3r;
using namespace Slic3r::SmartSupports;

TEST_CASE("FacetsAnnotation smart paint helpers (WP0 stub)", "[SmartSupports][sas-3mf]")
{
    Model        model;
    ModelObject *mo = model.add_object();
    ModelVolume *mv = mo->add_volume(TriangleMesh(its_make_cube(10., 10., 10.)));
    CHECK(! mv->supported_facets.has_auto_states());
    CHECK(! mv->supported_facets.has_enforcer_like_facets());

    TriangleSelector selector(mv->mesh());
    selector.set_facet(0, auto_state(cbIsland | cbCantilever));
    selector.set_facet(1, TriangleStateType::BLOCKER);
    mv->supported_facets.set(selector);
    CHECK(mv->supported_facets.has_auto_states());
    CHECK(mv->supported_facets.has_enforcer_like_facets());
    CHECK(mv->supported_facets.get_triangle_as_string(0) == FacetsAnnotation::triangle_as_string(mv->supported_facets.get_data(), 0));

    const TriangleSelector::TriangleSplittingData baked = mv->supported_facets.get_data_auto_baked(*mv);
    CHECK(! TriangleSelector::has_facets(baked, auto_state(cbIsland | cbCantilever)));
    CHECK(TriangleSelector::has_facets(baked, TriangleStateType::ENFORCER));
    CHECK(TriangleSelector::has_facets(baked, TriangleStateType::BLOCKER));
    CHECK(FacetsAnnotation::same_paint(mv->supported_facets.get_data(), mv->supported_facets.get_data()));
    CHECK(! FacetsAnnotation::same_paint(mv->supported_facets.get_data(), baked));

    mv->supported_facets.reset();
    CHECK(! mv->supported_facets.has_auto_states());
}
