// Smart support painting: Phase B cluster evaluator (plan §3.8; WP2c1a).
// WP0 stub: evaluate() returns empty needs, commit() and mark_tree_unplaced() are no-ops,
// evaluate_pass() returns empty needs for every layer.

#include "SmartSupportsInternal.hpp"

namespace Slic3r::SmartSupports {

struct LayerEvaluator::Impl
{
    Impl(const AnalysisInput &in, const BaseModel &base, const Reachability &reach, const ThrowOnCancel &cancel) :
        in(in), base(base), reach(reach), cancel(cancel) {}
    const AnalysisInput &in;
    const BaseModel     &base;
    const Reachability  &reach;
    ThrowOnCancel        cancel;
    LayerNeeds           last;
};

LayerEvaluator::LayerEvaluator(const AnalysisInput &in, const BaseModel &base, const Reachability &reach, const ThrowOnCancel &cancel) :
    m_impl(std::make_unique<Impl>(in, base, reach, cancel)) {}

LayerEvaluator::~LayerEvaluator() = default;

const LayerNeeds &LayerEvaluator::evaluate(uint32_t n, const ExPolygons &E, const std::vector<std::pair<ExPolygons, Cause>> &overrides,
                                           std::optional<BoundingBox> touched)
{
    m_impl->last = LayerNeeds();
    return m_impl->last;
}

void LayerEvaluator::commit(uint32_t n) {}

void LayerEvaluator::mark_tree_unplaced(int32_t tree) {}

PassResult evaluate_pass(const AnalysisInput &in, const BaseModel &base, Reachability &reach,
                         const std::vector<ExPolygons> &E_per_layer, const ThrowOnCancel &cancel, const ProgressFn &progress)
{
    PassResult out;
    out.layers.resize(in.layers.size());
    return out;
}

} // namespace Slic3r::SmartSupports
