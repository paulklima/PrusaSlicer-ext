// Smart support painting: Reachability — collisions, pulled avoidance chain, tips, roof / tip contact, cover
// (plan §2.3, §3.7; WP2r).
// WP0 stub: tips_possible = true; tips.bp = F_grown; predict_F = F_grown; cover = F; plate_part = Z;
// model_part = ∅; ensure / prefetch no-op.

#include "SmartSupportsInternal.hpp"

#include "libslic3r/ClipperUtils.hpp"

namespace Slic3r::SmartSupports {

struct Reachability::Impl
{
    explicit Impl(const AnalysisInput &in) : in(in) {}
    const AnalysisInput &in;
};

Reachability::Reachability(const AnalysisInput &in) : m_impl(std::make_unique<Impl>(in)) {}

Reachability::~Reachability() = default;

void Reachability::compute_collisions(const ThrowOnCancel &cancel) {}

void Reachability::start_prefetch(int target_support_layer, IsCanceled is_canceled) {}

void Reachability::ensure(int support_layer, const ThrowOnCancel &cancel) {}

int Reachability::tip_layer(uint32_t n) const
{
    const Params &params = m_impl->in.params;
    return int(n) + params.raft_support_layers - (params.z_top_layers + 1);
}

bool Reachability::tips_possible(uint32_t n) const { return true; }

double Reachability::gap_band_mm() const { return m_impl->in.params.xy_min_distance; }

ExPolygons Reachability::F_grown(const ExPolygons &E, uint32_t n) const
{
    const AnalysisInput &in = m_impl->in;
    if (E.empty() || n == 0 || n >= in.layers.size())
        return {};
    const ExPolygons &L  = in.layers[n].lslices;
    const ExPolygons &Lp = in.layers[n - 1].lslices;
    const ExPolygons  O  = diff_ex(L, Lp);
    const ExPolygons  EO = intersection_ex(E, O);
    if (EO.empty())
        return {};
    return diff_ex(offset_ex(EO, scaled<float>(in.params.tip_diameter)), Lp);
}

Reachability::Tips Reachability::tips(const ExPolygons &E, uint32_t n) const
{
    Tips out;
    out.bp = this->F_grown(E, n);
    return out;
}

Reachability::Contact Reachability::contact(const ExPolygons &E, uint32_t n) const
{
    Contact out;
    out.all          = this->F_grown(E, n);
    out.plate        = out.all;
    out.tip_path_all = out.all;
    out.roof_gap     = false;
    return out;
}

ExPolygons Reachability::cover(const ExPolygons &F, uint32_t n) const { return F; }

ExPolygons Reachability::plate_part(const ExPolygons &Z, const ExPolygons &E_layer, uint32_t n) const { return Z; }

ExPolygons Reachability::model_part(const ExPolygons &Z, const ExPolygons &E_layer, uint32_t n) const { return {}; }

bool Reachability::below_model_too_short(const ExPolygons &Z, uint32_t n) const { return false; }

} // namespace Slic3r::SmartSupports
