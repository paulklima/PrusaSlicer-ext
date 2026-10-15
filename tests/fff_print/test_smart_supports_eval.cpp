// Smart support painting: Phase B evaluator — evaluate_pass on reference cases #1, #5, #13, #14 and cone tails
// (plan §7.1; WP2c1a).
// WP0: one smoke test of the evaluator stubs on a sliced print; WP2c1a replaces this file.

#include <catch2/catch_test_macros.hpp>

#include "smart_supports_test_utils.hpp"
#include "libslic3r/Support/SmartSupportsInternal.hpp"

using namespace Slic3r;
using namespace Slic3r::SmartSupports;
using namespace Slic3r::SmartSupports::Testing;

TEST_CASE("evaluate_pass is sized per layer (WP0 stub)", "[SmartSupports][sas-eval]")
{
    const SasPrint     &sp   = cached_print("shelf_10x15", [] { return shelf_cube(10., 15.); }, sas_config());
    const AnalysisInput in   = build_input(*sp.print_object(), *sp.print_object()->model_object(), 0, UserParams());
    const BaseModel     base = build_base_model(in, [] {}, [](int) {});
    Reachability reach(in);
    const PassResult pass = evaluate_pass(in, base, reach, std::vector<ExPolygons>(in.layers.size()), [] {}, [](int) {});
    CHECK(pass.layers.size() == in.layers.size());

    LayerEvaluator ev(in, base, reach, [] {});
    const LayerNeeds &needs = ev.evaluate(1, {}, {}, std::nullopt);
    CHECK(needs.need[int(Cause::Cantilever)][int(Level::Must)].empty());
    ev.commit(1);
}
