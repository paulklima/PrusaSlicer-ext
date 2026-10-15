// Smart support painting: reference cases of plan §7.2 (theory §10) through run() (WP2c1b).
// WP0: one smoke test of the run() stub through run_sas(); WP2c1b replaces this file.

#include <catch2/catch_test_macros.hpp>

#include "smart_supports_test_utils.hpp"

using namespace Slic3r;
using namespace Slic3r::SmartSupports;
using namespace Slic3r::SmartSupports::Testing;

TEST_CASE("run returns one paint per volume (WP0 stub)", "[SmartSupports][sas-reference]")
{
    const SasPrint &sp = cached_print("shelf_10x15", [] { return shelf_cube(10., 15.); }, sas_config());
    RunOptions opts;
    opts.collect_stats = true;
    const SasRun r = run_sas(sp.print, UserParams(), opts);
    REQUIRE(r.result.paint.size() == r.input.volumes.size());
    CHECK(r.result.layer_stats.size() == r.input.layers.size());
    CHECK(! r.input.params.verification);
    for (size_t i = 0; i < r.result.paint.size(); ++ i)
        CHECK(r.result.paint[i].volume_id == r.input.volumes[i].volume_id);
}
