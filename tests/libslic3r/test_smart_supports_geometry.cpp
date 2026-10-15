// Smart support painting: Phase A geometry — Feret diameter, bridge directions, weak contact, hoop candidates
// (plan §7.1; WP2a).
// WP0: one smoke test of the build_base_model stub on a synthetic input; WP2a replaces this file.

#include <catch2/catch_test_macros.hpp>

#include "smart_supports_meshes.hpp"
#include "libslic3r/Support/SmartSupportsInternal.hpp"

using namespace Slic3r;
using namespace Slic3r::SmartSupports;
using namespace Slic3r::SmartSupports::Testing;

TEST_CASE("build_base_model sizes the layers (WP0 stub)", "[SmartSupports][sas-geometry]")
{
    const AnalysisInput in   = synthetic_input(shelf_cube(10., 15.), 0.2, 0.2, test_params());
    const BaseModel     base = build_base_model(in, [] {}, [](int) {});
    REQUIRE(in.layers.size() == 100);
    REQUIRE(base.layers.size() == in.layers.size());
    CHECK(base.layers.front().w == 0);
    CHECK(base.layers[1].w > 0);
    for (size_t n = 0; n < base.layers.size(); ++ n) {
        CHECK(base.layers[n].in == &in.layers[n]);
        CHECK(base.layers[n].slice_tree.size() == in.layers[n].lslices.size());
    }
}
