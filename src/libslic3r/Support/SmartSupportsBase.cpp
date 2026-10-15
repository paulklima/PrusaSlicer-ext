// Smart support painting: Phase A (fronts, clusters, bands, θ, hoop candidates, weak contact, unprintable),
// Phase A2 (island trees, tolerance, tail), Feret diameter (plan §3.5, §3.6; WP2a).
// WP0 stub: layers sized with z / h / w filled, no fronts, no trees.

#include "SmartSupportsInternal.hpp"

#include <cmath>

namespace Slic3r::SmartSupports {

BaseModel build_base_model(const AnalysisInput &in, const ThrowOnCancel &cancel, const ProgressFn &progress)
{
    const Params &params = in.params;
    BaseModel     base;
    base.layers.resize(in.layers.size());
    for (size_t n = 0; n < in.layers.size(); ++ n) {
        const LayerInput &li = in.layers[n];
        LayerGeom        &lg = base.layers[n];
        lg.in    = &li;
        lg.h     = li.height;
        lg.z_bot = li.slice_z - 0.5 * li.height;
        lg.z_top = li.slice_z + 0.5 * li.height;
        if (n > 0) {
            // §3.4: w_n = 0.5·ext_width_avg(n−1) for α = 0, else h_{n−1} / tan α; w_β = h_{n−1} / tan β, β ∈ (0, α_eff].
            const LayerInput &below     = in.layers[n - 1];
            const double      w_auto    = 0.5 * below.ext_width_avg_mm;
            const double      alpha_eff = params.user.alpha_deg > 0 ? double(params.user.alpha_deg) :
                                          (w_auto > 0. ? 180. / M_PI * atan(below.height / w_auto) : 90.);
            const double      w_mm      = params.user.alpha_deg > 0 ? below.height / tan(M_PI / 180. * double(params.user.alpha_deg)) : w_auto;
            lg.w = scaled<coord_t>(w_mm);
            if (params.user.beta_equals_alpha || params.user.beta_deg >= alpha_eff || params.user.beta_deg <= 0.)
                lg.w_beta = lg.w;
            else
                lg.w_beta = std::max(lg.w, scaled<coord_t>(below.height / tan(M_PI / 180. * params.user.beta_deg)));
        }
        lg.slice_bad.assign(li.lslices.size(), 0);
        lg.slice_tree.assign(li.lslices.size(), -1);
    }
    return base;
}

double feret_diameter(const Polygon &polygon)
{
    return 0.;
}

std::vector<Vec2d> bridge_directions(const ExPolygons &bridge_region, const ExPolygons &lower_local, double tie_tol, int max_dirs)
{
    return {};
}

} // namespace Slic3r::SmartSupports
