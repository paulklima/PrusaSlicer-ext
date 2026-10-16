// Smart support painting: Reachability — collisions, pulled avoidance chain, tips, roof / tip contact, cover
// (plan §2.3, §3.7; WP2r).
//
// Mirrors the tree / organic support generator of 2.9.6 for one PrintObject, in support-layer indices
// (support layer s = object layer + raft_support_layers):
// - the collision of the tip radius and the Fast avoidance to the build plate (TreeModelVolumes.cpp:411-585,
//   618-715), computed by a pulled chain (D31): ensure() or a prefetch std::thread, one writer at a time;
// - the enforced overhang grown by the tip diameter (generate_overhangs(), TreeSupport.cpp:284-299);
// - the tip area of generate_initial_areas() incl. the outset of the forbidden remainder (TreeSupport.cpp:1320-1366),
//   the roof decision per component (sample_overhang_area(), TreeSupport.cpp:1122-1150, 1414-1426) and the tip
//   status plate / model (convert_lines_to_internal(), TreeSupport.cpp:381-418);
// - short on-model branches without a roof are deleted (create_nodes_from_area(), TreeSupport.cpp:2708-2716).
// Tips are modelled as an area (D34); the gap band of width G = xy_min next to L_{n−1} counts as covered.
// Every query is clipped to a window around its argument (plan §3.7 "Window").

#include "SmartSupportsInternal.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <exception>
#include <limits>
#include <mutex>
#include <thread>

#include <oneapi/tbb/blocked_range.h>
#include <oneapi/tbb/parallel_for.h>
#include <oneapi/tbb/task_arena.h>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/PrintBase.hpp"
#include "libslic3r/Utils.hpp"

namespace Slic3r::SmartSupports {

namespace {

// Exact inside bbox (the slicer's own trimming, ClipperUtils::clip_clipper_polygons_with_subject_bbox()).
inline Polygons clip(const ExPolygons &src, const BoundingBox &bbox) { return ClipperUtils::clip_clipper_polygons_with_subject_bbox(src, bbox); }
inline Polygons clip(const Polygons &src, const BoundingBox &bbox)   { return ClipperUtils::clip_clipper_polygons_with_subject_bbox(src, bbox); }

// Copy of the file-static safe_union() of TreeSupport.cpp:747-778.
Polygons safe_union(const Polygons &first)
{
    Polygons result;
    if (! first.empty()) {
        result = union_(first);
        if (result.empty())
            result = union_(offset(to_polylines(first), scaled<float>(0.002), ClipperLib::jtMiter, 1.2));
    }
    return result;
}

// Copy of the file-static safe_offset_inc() of TreeSupport.cpp:790-836 (without the error report).
Polygons safe_offset_inc(const Polygons &me, coord_t distance, const Polygons &collision, coord_t safe_step_size,
                         coord_t last_step_offset_without_check, size_t min_amount_offset)
{
    bool     do_final_difference = last_step_offset_without_check == 0;
    Polygons ret                 = safe_union(me);

    Polygons collision_trimmed_buffer;
    auto collision_trimmed = [&collision_trimmed_buffer, &collision, &ret, distance]() -> const Polygons& {
        if (collision_trimmed_buffer.empty() && ! collision.empty())
            collision_trimmed_buffer = ClipperUtils::clip_clipper_polygons_with_subject_bbox(collision,
                get_extents(ret).inflated(std::max<coord_t>(0, distance) + SCALED_EPSILON));
        return collision_trimmed_buffer;
    };

    if (distance == 0 || safe_step_size < 0 || last_step_offset_without_check < 0)
        return do_final_difference ? diff(ret, collision_trimmed()) : union_(ret);

    coord_t step_size = safe_step_size;
    int     steps     = distance > last_step_offset_without_check ? (distance - last_step_offset_without_check) / step_size : 0;
    if (distance - steps * step_size > last_step_offset_without_check) {
        if ((steps + 1) * step_size <= distance)
            ++ steps;
        else
            do_final_difference = true;
    }
    if (steps + (distance < last_step_offset_without_check || (distance % step_size) != 0) < int(min_amount_offset) && min_amount_offset > 1) {
        step_size = distance / coord_t(min_amount_offset);
        if (step_size >= safe_step_size) {
            step_size = safe_step_size;
            steps     = int(min_amount_offset);
        } else
            steps = distance / step_size;
    }
    for (int i = 0; i < steps; ++ i) {
        ret = diff(offset(ret, float(step_size), ClipperLib::jtRound, scaled<float>(0.01)), collision_trimmed());
        if (i % 10 == 7)
            ret = polygons_simplify(ret, scaled<double>(0.015), false);
    }
    if (const float last_offset = float(distance - steps * step_size); last_offset > SCALED_EPSILON)
        ret = offset(ret, last_offset, ClipperLib::jtRound, scaled<float>(0.01));
    ret = polygons_simplify(ret, scaled<double>(0.015), false);
    if (do_final_difference)
        ret = diff(ret, collision_trimmed());
    return union_(ret);
}

// Arc tolerance of the tip circles and of the gap band.
constexpr double arc_tolerance = scaled<double>(0.005);

} // namespace

struct Reachability::Impl
{
    explicit Impl(const AnalysisInput &in);
    ~Impl();

    // Tip area of the painted overhang of one layer (generate_initial_areas()), window-local.
    struct TipArea {
        int         t = -1;               // tip layer (support-layer index)
        BoundingBox window;
        Polygons    F_grown;              // the slicer's overhang_raw
        Polygons    forbid;               // relevant_forbidden = forbid_t ⊕ 5 µm
        Polygons    avoid;                // avoid_t
        Polygons    T0;                   // overhang_regular before the outset = the roof candidates
        Polygons    T;                    // overhang_regular after the outset of the forbidden remainder
    };
    // Contact of tips placed anywhere in a tip-centre area.
    struct TipContact { Polygons plate, model; };

    const ExPolygons* slices(int s) const
        { return s >= num_raft && s < num_layers ? &in.layers[size_t(s - num_raft)].lslices : nullptr; }
    int  tip_layer(uint32_t n) const { return int(n) + num_raft - (z_top + 1); }
    // tip_layer(n) ≥ max(num_raft − (z_top + 1), 1), the first support layer with tips (shared with Phase A2).
    bool tips_possible(uint32_t n) const { return n < in.layers.size() && n >= first_tip_layer(in.params); }
    // xy distance of the collision with the i-th layer above (TreeModelVolumes.cpp:521-522).
    coord_t range_above(int i) const { return coord_t(xy_min - (i - (z_top == 1 ? 0.5 : 0)) * xy_min / z_top); }
    BoundingBox window_of(const ExPolygons &E) const { return get_extents(E).inflated(window_margin); }

    void compute_collisions(const ThrowOnCancel &cancel);
    void step_avoidance(int s);
    void advance(int k, const ThrowOnCancel *cancel);
    void prefetch_body(int target, const IsCanceled &is_canceled);
    void join_finished_prefetch();

    Polygons   forbid_raw(int s, const BoundingBox &window);
    Polygons   collision_radius0(int t, const BoundingBox &window) const;
    Polygons   model_short(int t, const BoundingBox &window) const;
    Polygons   f_grown(const ExPolygons &E, uint32_t n, const BoundingBox &window) const;
    TipArea    tip_area(const ExPolygons &E, uint32_t n);
    TipContact tip_contact(const TipArea &a, const Polygons &centers) const;
    Polygons   roofs(const TipArea &a);

    const AnalysisInput &in;

    // ---- constants of TreeSupportSettings / TreeModelVolumes / generate_initial_areas() (scaled)
    int     num_raft          = 0;      // TreeSupportSettings::raft_layers.size()
    int     num_layers        = 0;      // support layers: num_raft + in.layers.size()
    int     z_top             = 0;
    int     z_bottom          = 0;
    int     interface_layers  = 0;
    int     min_dtt_to_model  = 0;
    bool    rests_on_model    = true;
    bool    force_tip_to_roof = false;
    coord_t tip_radius        = 0;      // min_radius
    coord_t line_width        = 0;
    coord_t xy_min            = 0;
    coord_t resolution        = 0;
    coord_t extra_outset      = 0;
    coord_t circle_step       = 0;      // circle_length_to_half_linewidth_change
    coord_t window_margin     = 0;
    float   tip_diameter      = 0;      // enforcer_overhang_offset
    float   gap_band          = 0;      // G
    double  minimum_roof_area = 0;      // scaled²
    double  area_min          = 0;      // A_min, scaled²
    // Sub-steps of the avoidance (TreeModelVolumes.cpp:666-681).
    int     move_steps        = 0;
    float   move_step         = 0;
    float   last_move_step    = 0;

    // ---- collision coll_s of the tip radius; written once by compute_collisions()
    std::vector<Polygons> coll;
    std::once_flag        coll_once;
    std::atomic<bool>     coll_done { false };

    // ---- avoidance chain: avoid[0 .. ready] are final. One writer at a time: the prefetch thread while
    //      prefetch_running, otherwise advance() under the mutex.
    std::vector<Polygons>   avoid;
    std::atomic<int>        ready { -1 };
    std::mutex              mutex;
    std::condition_variable cv;
    std::thread             prefetch;
    bool                    prefetch_running = false;   // guarded by mutex
    std::exception_ptr      prefetch_error;             // guarded by mutex
    std::atomic<bool>       stop { false };
};

Reachability::Impl::Impl(const AnalysisInput &in) : in(in)
{
    const Params &p   = in.params;
    num_raft          = std::max(0, p.raft_support_layers);
    num_layers        = num_raft + int(in.layers.size());
    z_top             = std::max(0, p.z_top_layers);
    z_bottom          = std::max(0, p.z_bottom_layers);
    interface_layers  = std::max(0, p.interface_layers);
    rests_on_model    = ! p.buildplate_only;

    const coord_t layer_height = scaled<coord_t>(p.layer_height);
    tip_radius        = std::clamp(scaled<coord_t>(p.tip_diameter), coord_t(0), scaled<coord_t>(p.branch_diameter)) / 2;
    line_width        = scaled<coord_t>(p.support_line_width);
    xy_min            = scaled<coord_t>(p.xy_min_distance);
    resolution        = scaled<coord_t>(p.gcode_resolution);
    min_dtt_to_model  = layer_height > 0 ? int(round_up_divide<coord_t>(scaled<coord_t>(p.min_height_to_model), layer_height)) : 0;
    // TreeSupport.cpp:1259-1270
    extra_outset      = std::max(coord_t(0), tip_radius - line_width / 2) + (p.min_xy_dist ? line_width / 2 : 0);
    circle_step       = tip_radius < line_width ? tip_radius / 2 :
        coord_t(scale_(std::sqrt(sqr(unscale<double>(tip_radius)) - sqr(unscale<double>(tip_radius - line_width / 2)))));
    tip_diameter      = scaled<float>(p.tip_diameter);
    gap_band          = scaled<float>(p.xy_min_distance);
    minimum_roof_area = p.minimum_roof_area / sqr(SCALING_FACTOR);
    area_min          = p.A_min / sqr(SCALING_FACTOR);
    // force_tip_to_roof (TreeSupport.cpp:1273): soluble interface or a tip circle larger than the roof minimum.
    force_tip_to_roof = interface_layers > 0 &&
        (p.contact_distance < EPSILON || sqr(unscale<double>(tip_radius)) * M_PI > p.minimum_roof_area);
    window_margin     = scaled<coord_t>(p.tip_diameter) + extra_outset + tip_radius +
                        scaled<coord_t>(std::max(p.xy_distance, p.xy_min_distance)) + scaled<coord_t>(1.);

    // TreeSupportSettings::maximum_move_distance (TreeSupportCommon.cpp:94) − 2 (TreeModelVolumes.cpp:74).
    const double angle    = std::clamp(p.tree_angle_deg * M_PI / 180., 0., 0.5 * M_PI - EPSILON);
    const double move     = std::min(std::tan(angle) * double(layer_height), double(std::numeric_limits<coord_t>::max() / 4));
    const float  max_move = float(std::max<coord_t>(coord_t(move) - 2, 0));
    move_step = float(1.9 * std::max(tip_radius, xy_min));
    if (const int step_int = int(move_step); step_int > 0)
        move_steps = round_up_divide<int>(int(max_move), step_int);
    last_move_step = max_move - float(move_steps - 1) * move_step;
    if (last_move_step < scaled<float>(0.05) && move_steps > 1) {
        // Avoid a very short last step, stretch the other steps instead.
        move_step      = max_move / float(-- move_steps);
        last_move_step = move_step;
    }

    coll.assign(size_t(num_layers), Polygons());
    avoid.assign(size_t(num_layers), Polygons());
}

Reachability::Impl::~Impl()
{
    stop.store(true);
    if (prefetch.joinable())
        prefetch.join();
}

// TreeModelVolumes::calculateCollision() of the tip radius with xy_min (TreeModelVolumes.cpp:425-541), per support layer:
// coll_s = ∪_{j=s−z_bot..s} L_j ⊕ (r + xy_min)  ∪  ∪_{i=1..z_top} L_{s+i} ⊕ (r + range_i). Outlines simplified by the
// G-code resolution, raft layers without outlines (their collision still holds the object layers within z_top above).
// Neither simplified nor blocked by support blockers, as in the slicer (its processing_last_mesh is never true).
void Reachability::Impl::compute_collisions(const ThrowOnCancel &cancel)
{
    std::call_once(coll_once, [this, &cancel]() {
        // Isolated: a thread waiting in call_once must not run while stolen by this parallel_for.
        tbb::this_task_arena::isolate([this, &cancel]() {
            const size_t          cnt   = size_t(num_layers);
            const coord_t         delta = tip_radius + xy_min;
            std::vector<Polygons> outlines(cnt), lower(cnt);
            tbb::parallel_for(tbb::blocked_range<size_t>(size_t(num_raft), cnt), [&](const tbb::blocked_range<size_t> &range) {
                for (size_t s = range.begin(); s < range.end(); ++ s) {
                    if (cancel)
                        cancel();
                    outlines[s] = polygons_simplify(to_polygons(*this->slices(int(s))), double(resolution), false);
                    lower[s]    = delta == 0 ? union_(outlines[s]) : offset(union_ex(outlines[s]), float(delta), ClipperLib::jtMiter, 1.2);
                }
            });
            tbb::parallel_for(tbb::blocked_range<size_t>(0, cnt), [&](const tbb::blocked_range<size_t> &range) {
                for (size_t s = range.begin(); s < range.end(); ++ s) {
                    if (cancel)
                        cancel();
                    Polygons c;
                    for (int j = std::max(0, int(s) - z_bottom); j <= int(s); ++ j)
                        append(c, lower[size_t(j)]);
                    for (int i = 1; i <= z_top; ++ i)
                        if (const int j = int(s) + i; j < num_layers && ! outlines[size_t(j)].empty())
                            append(c, offset(union_ex(outlines[size_t(j)]), float(tip_radius + this->range_above(i)), ClipperLib::jtMiter, 1.2));
                    coll[s] = union_(c);
                }
            });
        });
        coll_done.store(true, std::memory_order_release);
    });
}

// One layer of TreeModelVolumes::calculateAvoidance(), Fast, to the build plate (TreeModelVolumes.cpp:683-698):
// avoid_0 = coll_0; avoid_s = union(coll_s, avoid_{s−1} ⊖ step) per sub-step, simplified.
void Reachability::Impl::step_avoidance(int s)
{
    if (s == 0) {
        avoid[0] = coll[0];
        return;
    }
    const Polygons &collision = coll[size_t(s)];
    Polygons        latest    = avoid[size_t(s - 1)];
    for (int istep = 0; istep < move_steps; ++ istep)
        latest = union_(collision, offset(latest, istep + 1 == move_steps ? - last_move_step : - move_step,
                                          ClipperLib::jtRound, double(resolution)));
    avoid[size_t(s)] = polygons_simplify(latest, double(resolution), false);
}

// Advances the chain to k (clamped). Waits for a running prefetch first (in 50 ms slices, calling cancel between them),
// then computes the rest itself under the mutex: there is never more than one writer. Rethrows prefetch errors.
void Reachability::Impl::advance(int k, const ThrowOnCancel *cancel)
{
    k = std::min(k, num_layers - 1);
    if (k < 0 || ready.load(std::memory_order_acquire) >= k)
        return;
    if (! coll_done.load(std::memory_order_acquire))
        this->compute_collisions(cancel != nullptr ? *cancel : ThrowOnCancel());
    std::unique_lock<std::mutex> lock(mutex);
    for (;;) {
        if (prefetch_error)
            std::rethrow_exception(prefetch_error);
        if (ready.load(std::memory_order_acquire) >= k)
            return;
        if (! prefetch_running)
            break;
        cv.wait_for(lock, std::chrono::milliseconds(50));
        if (cancel != nullptr && *cancel) {
            lock.unlock();
            (*cancel)();
            lock.lock();
        }
    }
    for (int s = ready.load(std::memory_order_relaxed) + 1; s <= k; ++ s) {
        if (cancel != nullptr && *cancel)
            (*cancel)();
        this->step_avoidance(s);
        ready.store(s, std::memory_order_release);
    }
}

// The prefetch thread: the single writer up to target; ClipperUtils only (no TBB), polls is_canceled per layer and
// captures any exception for ensure().
void Reachability::Impl::prefetch_body(int target, const IsCanceled &is_canceled)
{
    try {
        for (;;) {
            const int s = ready.load(std::memory_order_acquire) + 1;
            if (s > target || stop.load() || (is_canceled && is_canceled()))
                break;
            this->step_avoidance(s);
            {
                std::lock_guard<std::mutex> lock(mutex);
                ready.store(s, std::memory_order_release);
            }
            cv.notify_all();
        }
    } catch (...) {
        std::lock_guard<std::mutex> lock(mutex);
        prefetch_error = std::current_exception();
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        prefetch_running = false;
    }
    cv.notify_all();
}

// Waits for the prefetch thread to finish and joins it (serial caller only).
void Reachability::Impl::join_finished_prefetch()
{
    if (! prefetch.joinable())
        return;
    {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [this]() { return ! prefetch_running; });
    }
    prefetch.join();
}

// Source of the slicer's forbidden area at support layer s: the collision when support may rest on the model, else
// the avoidance (TreeSupport.cpp:1132-1135, 1323-1328; D35).
Polygons Reachability::Impl::forbid_raw(int s, const BoundingBox &window)
{
    if (s < 0)
        return {};
    if (rests_on_model)
        return clip(coll[size_t(s)], window);
    this->advance(s, nullptr);
    return clip(avoid[size_t(s)], window);
}

// Collision of radius 0 (TreeModelVolumes::getCollision(0, t, true)), window-local, from the unsimplified slices
// (the slicer simplifies by the G-code resolution first; the difference is below that resolution).
Polygons Reachability::Impl::collision_radius0(int t, const BoundingBox &window) const
{
    Polygons out;
    for (int j = std::max(0, t - z_bottom); j <= t; ++ j)
        if (const ExPolygons *s = this->slices(j); s != nullptr && ! s->empty()) {
            Polygons p = clip(*s, window);
            append(out, xy_min == 0 ? union_(p) : offset(union_ex(p), float(xy_min), ClipperLib::jtMiter, 1.2));
        }
    for (int i = 1; i <= z_top; ++ i)
        if (const ExPolygons *s = this->slices(t + i); s != nullptr && ! s->empty())
            append(out, offset(union_ex(clip(*s, window)), float(this->range_above(i)), ClipperLib::jtMiter, 1.2));
    return union_(out);
}

// Tip positions whose branch to the model would be shorter than support_tree_min_height_to_model: the collision blocks
// the descent within min_dtt_to_model layers (TreeSupport.cpp:2708-2716). This is the plan's
// model_short = (∪_{m=t−k..t} L_m) ⊕ (r + xy), k = min_dtt + z_bottom, expressed through coll.
Polygons Reachability::Impl::model_short(int t, const BoundingBox &window) const
{
    Polygons out;
    for (int s = std::max(0, t - min_dtt_to_model); s < t; ++ s)
        append(out, clip(coll[size_t(s)], window));
    return union_(out);
}

// generate_overhangs() for painted overhangs (TreeSupport.cpp:284-299): (E ∩ (L_n − L_{n−1})) ⊕ tip_d − L_{n−1}.
Polygons Reachability::Impl::f_grown(const ExPolygons &E, uint32_t n, const BoundingBox &window) const
{
    if (E.empty() || n == 0 || n >= in.layers.size())
        return {};
    const Polygons Lp = clip(in.layers[n - 1].lslices, window);
    const Polygons EO = intersection(E, diff(clip(in.layers[n].lslices, window), Lp));
    if (EO.empty())
        return {};
    return diff(offset(union_ex(EO), tip_diameter), Lp);
}

// generate_initial_areas() up to the roof split (TreeSupport.cpp:1320-1366), support_offset = 0.
Reachability::Impl::TipArea Reachability::Impl::tip_area(const ExPolygons &E, uint32_t n)
{
    TipArea a;
    if (E.empty() || ! this->tips_possible(n))
        return a;
    a.window  = this->window_of(E);
    a.F_grown = this->f_grown(E, n, a.window);
    if (a.F_grown.empty())
        return a;
    a.t = this->tip_layer(n);
    this->advance(a.t, nullptr);
    a.avoid  = clip(avoid[size_t(a.t)], a.window);
    a.forbid = offset(union_ex(rests_on_model ? clip(coll[size_t(a.t)], a.window) : a.avoid), scaled<float>(0.005), ClipperLib::jtMiter, 1.2);
    a.T0     = diff(a.F_grown, a.forbid);
    // Outset of the forbidden remainder in steps, limited by the collision of radius 0 (TreeSupport.cpp:1339-1366).
    Polygons remaining = intersection(diff(a.F_grown, offset(union_ex(a.T0), float(line_width * 0.5), ClipperLib::jtMiter, 1.2)), a.forbid);
    a.T = a.T0;
    Polygons raw_collision;
    bool     raw_collision_done = false;
    for (coord_t acc = 0; ! remaining.empty() && acc + line_width / 8 < extra_outset;) {
        const coord_t step = std::min(acc + 2 * line_width > tip_radius ? line_width / 8 : circle_step, extra_outset - acc);
        acc += step;
        if (! raw_collision_done) {
            raw_collision      = this->collision_radius0(a.t, a.window);
            raw_collision_done = true;
        }
        const coord_t offset_step = xy_min + line_width;
        remaining = diff(remaining, safe_offset_inc(a.T, coord_t(1.5 * acc), raw_collision, offset_step, 0, 1));
        a.T       = union_(a.T, diff(safe_offset_inc(remaining, acc, raw_collision, offset_step, 0, 1), a.forbid));
    }
    return a;
}

// Contact of tips placed anywhere in centers: circles of the tip radius, clipped to F_grown. Status by the avoidance
// (convert_lines_to_internal()); on-model branches that are too short are deleted unless they carry a roof (D23).
Reachability::Impl::TipContact Reachability::Impl::tip_contact(const TipArea &a, const Polygons &centers) const
{
    TipContact out;
    if (centers.empty())
        return out;
    if (const Polygons bp = diff(centers, a.avoid); ! bp.empty())
        out.plate = intersection(a.F_grown, offset(bp, float(tip_radius), ClipperLib::jtRound, arc_tolerance));
    if (rests_on_model) {
        Polygons model = intersection(centers, a.avoid);
        if (! model.empty() && ! force_tip_to_roof)
            model = diff(model, this->model_short(a.t, a.window));
        if (! model.empty())
            out.model = intersection(a.F_grown, offset(model, float(tip_radius), ClipperLib::jtRound, arc_tolerance));
    }
    return out;
}

// Roof components (interface layers > 0): components K of T0 with area(K − forbid_{t−1} ⊕ 5 µm) ≥ minimum_roof_area
// (sample_overhang_area(), TreeSupport.cpp:1122-1150). The roof-line check (:1162-1170) is not mirrored (§3.14 #16).
Polygons Reachability::Impl::roofs(const TipArea &a)
{
    Polygons out;
    if (interface_layers <= 0 || a.T0.empty() || a.t < 1)
        return out;
    const Polygons forbid_next = offset(union_ex(this->forbid_raw(a.t - 1, a.window)), scaled<float>(0.005), ClipperLib::jtMiter, 1.2);
    for (ExPolygon &K : union_ex(a.T0)) {
        Polygons k = to_polygons(std::move(K));
        if (area(diff(k, forbid_next)) >= minimum_roof_area)
            append(out, std::move(k));
    }
    return out;
}

Reachability::Reachability(const AnalysisInput &in) : m_impl(std::make_unique<Impl>(in)) {}

Reachability::~Reachability() = default;

void Reachability::compute_collisions(const ThrowOnCancel &cancel) { m_impl->compute_collisions(cancel); }

void Reachability::start_prefetch(int target_support_layer, IsCanceled is_canceled)
{
    Impl     &impl   = *m_impl;
    const int target = std::min(target_support_layer, impl.num_layers - 1);
    if (target < 0)
        return;
    impl.join_finished_prefetch();
    if (! impl.coll_done.load(std::memory_order_acquire))
        impl.compute_collisions([&is_canceled]() { if (is_canceled && is_canceled()) throw CanceledException(); });
    if (impl.ready.load(std::memory_order_acquire) >= target)
        return;
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        impl.prefetch_running = true;
    }
    impl.prefetch = std::thread([&impl, target, is_canceled = std::move(is_canceled)]() { impl.prefetch_body(target, is_canceled); });
}

void Reachability::ensure(int support_layer, const ThrowOnCancel &cancel)
{
    if (support_layer < 0)
        return;
    Impl &impl = *m_impl;
    impl.advance(support_layer, &cancel);
    // A prefetch that has finished (always the case when support_layer was beyond its target) is joined here.
    bool finished;
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        finished = ! impl.prefetch_running;
    }
    if (finished && impl.prefetch.joinable())
        impl.prefetch.join();
}

int Reachability::tip_layer(uint32_t n) const { return m_impl->tip_layer(n); }

bool Reachability::tips_possible(uint32_t n) const { return m_impl->tips_possible(n); }

double Reachability::gap_band_mm() const { return m_impl->in.params.xy_min_distance; }

ExPolygons Reachability::F_grown(const ExPolygons &E, uint32_t n) const
{
    if (E.empty())
        return {};
    return union_ex(m_impl->f_grown(E, n, m_impl->window_of(E)));
}

Reachability::Tips Reachability::tips(const ExPolygons &E, uint32_t n) const
{
    Tips                out;
    const Impl::TipArea a = m_impl->tip_area(E, n);
    if (a.T.empty())
        return out;
    out.bp = diff_ex(a.T, a.avoid);
    if (m_impl->rests_on_model) {
        out.model       = intersection_ex(a.T, a.avoid);
        out.spill_model = out.model;
    }
    return out;
}

Reachability::Contact Reachability::contact(const ExPolygons &E, uint32_t n) const
{
    Impl               &impl = *m_impl;
    Contact             out;
    const Impl::TipArea a    = impl.tip_area(E, n);
    if (a.T.empty())
        return out;
    const Impl::TipContact all_tips = impl.tip_contact(a, a.T);
    out.tip_path_all = union_ex(all_tips.plate, all_tips.model);
    const Polygons roofs = impl.roofs(a);
    if (roofs.empty()) {
        out.plate = union_ex(all_tips.plate);
        out.model = union_ex(all_tips.model);
        out.all   = out.tip_path_all;
        return out;
    }
    // Roof path: the contact is the roof polygon itself, starting r + xy_min/2 from L_{n−1}; the failed components
    // and the outset take the tip path (TreeSupport.cpp:1414-1426).
    out.roof_gap = true;
    const Impl::TipContact rest = impl.tip_contact(a, diff(a.T, roofs, ApplySafetyOffset::Yes));
    out.plate = union_ex(diff(roofs, a.avoid), rest.plate);
    out.model = impl.rests_on_model ? union_ex(intersection(roofs, a.avoid), rest.model) : union_ex(rest.model);
    Polygons all = roofs;
    append(all, rest.plate);
    append(all, rest.model);
    out.all = union_ex(all);
    return out;
}

ExPolygons Reachability::cover(const ExPolygons &F, uint32_t n) const
{
    const Impl &impl = *m_impl;
    if (F.empty())
        return {};
    if (n == 0 || n >= impl.in.layers.size() || impl.gap_band <= 0)
        return F;
    const BoundingBox window = get_extents(F).inflated(coord_t(impl.gap_band) + scaled<coord_t>(1.));
    const Polygons    L      = clip(impl.in.layers[n].lslices, window);
    const Polygons    Lp     = clip(impl.in.layers[n - 1].lslices, window);
    if (L.empty() || Lp.empty())
        return F;
    const Polygons band = intersection(intersection(L, offset(F, impl.gap_band, ClipperLib::jtRound, arc_tolerance)),
                                       offset(Lp, impl.gap_band, ClipperLib::jtRound, arc_tolerance));
    return band.empty() ? F : union_ex(F, band);
}

ExPolygons Reachability::plate_part(const ExPolygons &Z, const ExPolygons &E_layer, uint32_t n) const
{
    if (Z.empty())
        return {};
    const Contact c = this->contact(E_layer.empty() ? Z : union_ex(E_layer, Z), n);
    return c.plate.empty() ? ExPolygons() : intersection_ex(Z, this->cover(c.plate, n));
}

ExPolygons Reachability::model_part(const ExPolygons &Z, const ExPolygons &E_layer, uint32_t n) const
{
    if (Z.empty())
        return {};
    const Contact c = this->contact(E_layer.empty() ? Z : union_ex(E_layer, Z), n);
    if (c.model.empty())
        return {};
    ExPolygons model = intersection_ex(Z, this->cover(c.model, n));
    return c.plate.empty() ? model : diff_ex(model, intersection_ex(Z, this->cover(c.plate, n)));
}

// True if on-model tip-path tips of Z are deleted as too short and that leaves at least A_min of Z uncovered
// (roof components carry a roof and are exempt, D23).
bool Reachability::below_model_too_short(const ExPolygons &Z, uint32_t n) const
{
    Impl &impl = *m_impl;
    if (Z.empty() || ! impl.rests_on_model || impl.force_tip_to_roof)
        return false;
    const Impl::TipArea a = impl.tip_area(Z, n);
    if (a.T.empty())
        return false;
    const Polygons roofs   = impl.roofs(a);
    const Polygons centers = roofs.empty() ? a.T : diff(a.T, roofs, ApplySafetyOffset::Yes);
    const Polygons shorts  = intersection(intersection(centers, a.avoid), impl.model_short(a.t, a.window));
    if (shorts.empty())
        return false;
    const ExPolygons F_short = union_ex(intersection(a.F_grown, offset(shorts, float(impl.tip_radius), ClipperLib::jtRound, arc_tolerance)));
    const ExPolygons lost    = diff_ex(intersection_ex(Z, this->cover(F_short, n)), this->cover(this->contact(Z, n).all, n));
    return area(lost) >= impl.area_min;
}

} // namespace Slic3r::SmartSupports
