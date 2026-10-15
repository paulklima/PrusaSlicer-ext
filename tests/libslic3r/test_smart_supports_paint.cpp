// Smart support painting: prism painting of placements (plan §7.1; WP1b).
// WP0: one smoke test of the paint_placements stub on a synthetic input; WP1b replaces this file.

#include <catch2/catch_test_macros.hpp>

#include "smart_supports_meshes.hpp"
#include "libslic3r/Support/SmartSupportsInternal.hpp"

using namespace Slic3r;
using namespace Slic3r::SmartSupports;
using namespace Slic3r::SmartSupports::Testing;

TEST_CASE("paint_placements returns one paint per volume (WP0 stub)", "[SmartSupports][sas-paint]")
{
    const AnalysisInput in = synthetic_input({ wedge(45.), box({ 30., 0., 0. }, { 40., 10., 10. }) }, 0.2, 0.2, test_params());
    REQUIRE(in.volumes.size() == 2);
    const std::vector<VolumePaint> paint = paint_placements(in, {}, cbIsland | cbCantilever, Level::Should, [] {});
    REQUIRE(paint.size() == in.volumes.size());
    for (size_t i = 0; i < paint.size(); ++ i) {
        CHECK(paint[i].volume_id == in.volumes[i].volume_id);
        CHECK(! paint[i].changed);
    }
}
