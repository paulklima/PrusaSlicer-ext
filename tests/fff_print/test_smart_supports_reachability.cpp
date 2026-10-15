// Smart support painting: Reachability — avoidance, plate coverage, cover, roof path, real contact cross-check,
// raft indexing, pull model (plan §7.1; WP2r).
// WP0: one smoke test of the Reachability stub on a sliced print; WP2r replaces this file.

#include <catch2/catch_test_macros.hpp>

#include "smart_supports_test_utils.hpp"
#include "libslic3r/Support/SmartSupportsInternal.hpp"

using namespace Slic3r;
using namespace Slic3r::SmartSupports;
using namespace Slic3r::SmartSupports::Testing;

TEST_CASE("Reachability stub on a sliced print (WP0 stub)", "[SmartSupports][sas-reachability]")
{
    const SasPrint     &sp = cached_print("shelf_10x15", [] { return shelf_cube(10., 15.); }, sas_config());
    const AnalysisInput in = build_input(*sp.print_object(), *sp.print_object()->model_object(), 0, UserParams());
    Reachability reach(in);
    reach.compute_collisions([] {});
    reach.start_prefetch(reach.tip_layer(uint32_t(in.layers.size() - 1)), [] { return false; });
    reach.ensure(-1, [] {});
    reach.ensure(reach.tip_layer(uint32_t(in.layers.size() - 1)), [] {});
    CHECK(reach.tip_layer(10) == 10 + in.params.raft_support_layers - (in.params.z_top_layers + 1));
    CHECK(reach.gap_band_mm() == in.params.xy_min_distance);
    // The whole layer as painted zone: the stub contact is F_grown, non-empty under the shelf.
    const uint32_t n = 90;   // first layer of the 2 mm shelf (z 18..20)
    const ExPolygons &L = in.layers[n].lslices;
    const ExPolygons Fg = reach.F_grown(L, n);
    CHECK(! Fg.empty());
    CHECK(reach.predict_F(L, n).size() == Fg.size());
    CHECK(reach.plate_part(L, L, n).size() == L.size());
}
