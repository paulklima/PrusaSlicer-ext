// Smart support painting: accumulated reach c(p) — ReachField, ReachCarry, segment minimiser (plan §3.8; WP2b1).
// WP0 stub: ReachField = Euclidean distance to the nearest seed + ĉ there, Direct mode, res 0.1 mm;
// ReachCarry::c_hat = 0; segment_min evaluates the ends and the foot point only.

#include "SmartSupportsInternal.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Slic3r::SmartSupports {

struct ReachField::Impl
{
    std::vector<AnchorSeg> seeds;
    coord_t                res { scaled<coord_t>(0.1) };
};

ReachField::ReachField(const ExPolygons &cprime, const ExPolygons &domain, std::vector<AnchorSeg> seeds, double delta_beta_mm,
                       const Params &params, const ThrowOnCancel &cancel)
{
    auto impl   = std::make_shared<Impl>();
    impl->seeds = std::move(seeds);
    m_impl      = std::move(impl);
}

ReachField::ReachField(std::shared_ptr<const Impl> impl) : m_impl(std::move(impl)) {}

ReachField::~ReachField() = default;

float ReachField::value(const Point &p) const
{
    float best = std::numeric_limits<float>::infinity();
    for (const AnchorSeg &seg : m_impl->seeds) {
        const Vec2d  a   = unscale(seg.l.a);
        const Vec2d  ab  = unscale(seg.l.b) - a;
        const double len = ab.norm();
        const double t   = len > 0. ? std::clamp((unscale(p) - a).dot(ab) / (len * len), 0., 1.) : 0.;
        const double c   = double(seg.c0) + t * double(seg.c1 - seg.c0) + (unscale(p) - (a + t * ab)).norm();
        best = std::min(best, float(c));
    }
    return best;
}

coord_t ReachField::res() const { return m_impl->res; }

bool ReachField::direct_mode() const { return true; }

std::shared_ptr<const ReachQuery> ReachField::cropped(const BoundingBox &bbox) const
{
    return std::shared_ptr<const ReachQuery>(new ReachField(m_impl));
}

struct ReachCarry::Impl
{
    explicit Impl(const Params &params) : params(params) {}
    Params params;
};

ReachCarry::ReachCarry(const Params &params) : m_impl(std::make_unique<Impl>(params)) {}
ReachCarry::~ReachCarry() = default;
ReachCarry::ReachCarry(ReachCarry &&) noexcept = default;
ReachCarry &ReachCarry::operator=(ReachCarry &&) noexcept = default;

void ReachCarry::push(uint32_t layer, double z_top, coord_t w, std::vector<FrontCarry> &&all_fronts) {}

float ReachCarry::c_hat(const Point &a, uint32_t n, double z_top_n, double h_n, coord_t w_n) const { return 0.f; }

float segment_min(const AnchorSeg &seg, const Point &p, double delta_beta_mm)
{
    const Vec2d  a   = unscale(seg.l.a);
    const Vec2d  ab  = unscale(seg.l.b) - a;
    const double len = ab.norm();
    const Vec2d  ap  = unscale(p) - a;
    if (len <= 0.)
        return float(double(seg.c0) + std::max(0., ap.norm() - delta_beta_mm));
    const Vec2d  u   = ab / len;
    const double sp  = ap.dot(u);
    const double d   = std::abs(u.x() * ap.y() - u.y() * ap.x());
    const double g   = double(seg.c1 - seg.c0) / len;
    auto f = [&](double s) { return double(seg.c0) + g * s + std::max(0., std::sqrt(d * d + (s - sp) * (s - sp)) - delta_beta_mm); };
    return float(std::min({ f(0.), f(len), f(std::clamp(sp, 0., len)) }));
}

} // namespace Slic3r::SmartSupports
