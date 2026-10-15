// Smart support painting: island trees, tolerance, tails, cluster bands (plan §7.1; WP2a).
// WP0: one smoke test of the build_base_model stub on a sliced print; WP2a replaces this file.

#include <catch2/catch_test_macros.hpp>

#include "smart_supports_test_utils.hpp"
#include "libslic3r/Support/SmartSupportsInternal.hpp"

using namespace Slic3r;
using namespace Slic3r::SmartSupports;
using namespace Slic3r::SmartSupports::Testing;

TEST_CASE("build_base_model on a sliced print (WP0 stub)", "[SmartSupports][sas-islands]")
{
    const SasPrint     &sp   = cached_print("inverted_cone_60", [] { return inverted_cone(); }, sas_config());
    const AnalysisInput in   = build_input(*sp.print_object(), *sp.print_object()->model_object(), 0, UserParams());
    const BaseModel     base = build_base_model(in, [] {}, [](int) {});
    REQUIRE(base.layers.size() == in.layers.size());
    for (const LayerGeom &lg : base.layers)
        CHECK(lg.slice_bad.size() == lg.in->lslices.size());
}
