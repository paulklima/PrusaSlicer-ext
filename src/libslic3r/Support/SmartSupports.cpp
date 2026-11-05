// Smart support painting: run() orchestration, progress, cancel (plan §2.2, §2.3; WP2c1b).
//
// run() = parallel_invoke{ Phase A, collisions } → prefetch of the avoidance chain → joint plan → (test hook) → paint of
// the checked causes → verification (only with a contact deficit) → report. Progress: Phase A 0–15 %, plan 15–80 %,
// paint 80–90 %, verification and report 90–100 %; monotone, from any thread. Cancel: CanceledException from every
// phase, no partial result (the Reachability destructor stops and joins the prefetch thread).

#include "SmartSupportsInternal.hpp"

#include <algorithm>
#include <mutex>

#include <oneapi/tbb/parallel_invoke.h>

#include "libslic3r/PrintBase.hpp"

namespace Slic3r::SmartSupports {

namespace {

// Monotone percent reporting from any thread (Phase A and the collisions run on TBB workers, the prefetch on its own
// thread): calls are serialised and only increasing values are passed on.
class Progress
{
public:
    explicit Progress(const ProgressFn &fn) : m_fn(fn) {}
    void operator()(int pct)
    {
        if (! m_fn)
            return;
        pct = std::clamp(pct, 0, 100);
        std::lock_guard<std::mutex> lock(m_mutex);
        if (pct <= m_last)
            return;
        m_last = pct;
        m_fn(pct);
    }
    // A phase reporting 0..100 mapped to [lo, hi].
    ProgressFn band(int lo, int hi)
    {
        return [this, lo, hi](int pct) { (*this)(lo + (hi - lo) * std::clamp(pct, 0, 100) / 100); };
    }

private:
    const ProgressFn &m_fn;
    std::mutex        m_mutex;
    int               m_last = -1;
};

std::vector<VolumePaint> unchanged_paint(const AnalysisInput &in)
{
    std::vector<VolumePaint> out;
    out.reserve(in.volumes.size());
    for (const VolumeInput &volume : in.volumes)
        out.push_back({ volume.volume_id, volume.paint, false });
    return out;
}

} // namespace

Result run(const AnalysisInput &in, const RunOptions &opt, const IsCanceled &is_canceled, const ProgressFn &progress_fn)
{
    const ThrowOnCancel cancel = [&is_canceled]() { if (is_canceled && is_canceled()) throw CanceledException(); };
    Progress progress(progress_fn);
    progress(0);
    cancel();

    Result result;
    if (in.layers.empty()) {
        result.paint = unchanged_paint(in);
        if (opt.collect_stats)
            result.layer_stats.resize(in.layers.size());
        progress(100);
        return result;
    }

    // Phase A ∥ collisions (plan §2.3, review 7 m6).
    Reachability reach(in);
    BaseModel    base;
    {
        const ProgressFn phase_a = progress.band(0, 15);
        tbb::parallel_invoke([&]() { base = build_base_model(in, cancel, phase_a); },
                             [&]() { reach.compute_collisions(cancel); });
    }
    cancel();
    reach.start_prefetch(reach.tip_layer(base.last_layer_with_fronts), is_canceled ? is_canceled : IsCanceled([]() { return false; }));

    // Joint plan of all causes (P3).
    Plan plan = make_plan(in, base, reach, cancel, progress.band(15, 80));
    progress(80);

    // Paint the placements of the checked causes (WP1b: no progress callback inside).
    const UserParams &up = in.params.user;
    std::vector<Placement> to_paint;
    const std::vector<Placement> *painted = &plan.placements;
    if (opt.test_mutate_before_paint) {
        to_paint = plan.placements;
        opt.test_mutate_before_paint(to_paint);
        painted = &to_paint;
    }
    if (opt.paint) {
        result.paint = paint_placements(in, *painted, up.paint_causes, up.lowest_level, cancel);
        // Painted areas per cause: the placements paint_placements() paints (WP1b contract).
        for (const Placement &pl : *painted)
            if (pl.paintable && pl.cause != Cause::LongSpan && (up.paint_causes & cause_bit(pl.cause)) != 0 &&
                pl.level >= up.lowest_level && pl.layer < in.layers.size() && ! pl.area.empty())
                ++ result.painted_areas[int(pl.cause)];
    } else
        result.paint = unchanged_paint(in);
    progress(90);

    // Verification against the actual paint (D13); only meaningful when this run painted.
    const bool verification = opt.verification < 0 ? in.params.verification : opt.verification != 0;
    std::vector<Need> remaining;
    if (verification && opt.paint)
        remaining = verify(in, base, reach, plan, result.paint, cancel);
    cancel();
    progress(97);

    result.report = build_report(in, plan, remaining, result.report_more);
    if (opt.collect_stats)
        result.layer_stats = std::move(plan.stats);
    result.placements = std::move(plan.placements);
    // Result::fingerprint stays 0: it needs the live Model, the GUI computes config_fingerprint() (WP2c0).
    progress(100);
    return result;
}

} // namespace Slic3r::SmartSupports
