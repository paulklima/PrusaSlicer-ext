#ifndef slic3r_SmartSupportsWait_hpp_
#define slic3r_SmartSupportsWait_hpp_

// Smart support painting: the decision of the panel's WaitingForSlices state (plan §6.6), a pure function of what the
// panel observes on one poll, so that it can be unit-tested without a display (tests/slic3rutils). No wx, no Plater.

namespace Slic3r::GUI::SmartSupportsWait {

// What SmartSupportsPanel::poll_waiting() observes.
struct Observation {
    double since_start_s    = 0.;     // since "Paint where needed" was clicked
    bool   resliced         = false;  // our reslice_FFF_until_step() call has been made
    double since_reslice_s  = 0.;     // since that call
    bool   update_scheduled = false;  // Plater::is_background_process_update_scheduled(): a change waits for Print::apply
    bool   po_found         = true;   // choose_print_object() found the PrintObject of the analysed instance
    bool   ready            = false;  // its posSlice is done
    bool   consistent       = false;  // input_consistent() of that PrintObject with the live object
    bool   slicing_running  = false;  // s_print_statuses[active bed] == PrintStatus::running
    bool   slicing_failed   = false;  // ... is not sliceable (invalid, empty, outside)
    bool   worker_idle      = true;   // the UI job worker is idle
    int    idle_reslices    = 0;      // reslices made because nothing was running while posSlice was not done
};

enum class Action {
    Wait,              // poll again
    StartJob,          // build the input and queue the analysis
    Reslice,           // slice again (the live object changed after the slices were made)
    ResliceIdle,       // slice again (the slicing was stopped before posSlice); counts as an idle reslice
    FailTimeout,
    FailNotInPrint,
    FailSlicing,       // the slicer reports an error or an unsliceable bed
    FailInterrupted    // the slicing stopped again after an idle reslice
};

inline constexpr double TimeoutS   = 15. * 60.;
inline constexpr double RetryS     = 1.;   // between reslices of an inconsistent object
inline constexpr double IdleGraceS = 2.;   // after a reslice call, until the slicing start event has certainly arrived

inline Action decide(const Observation &o)
{
    if (o.since_start_s > TimeoutS)
        return Action::FailTimeout;
    if (! o.resliced)
        return Action::Wait;
    // A model or settings change waits for its Print::apply (the 100 ms background process timer): the slices of the old
    // state must not be analysed (review C M1).
    if (o.update_scheduled)
        return Action::Wait;
    if (! o.po_found)
        return Action::FailNotInPrint;
    if (o.ready) {
        if (! o.consistent)
            return o.since_reslice_s > RetryS ? Action::Reslice : Action::Wait;
        // Queue only on an idle worker: a job queued behind another one can be dropped without its finalize (review C m2).
        return o.worker_idle ? Action::StartJob : Action::Wait;
    }
    if (o.slicing_running || o.since_reslice_s < IdleGraceS)
        return Action::Wait;
    if (o.slicing_failed)
        return Action::FailSlicing;
    // Nothing runs, nothing is scheduled and posSlice is not done: the slicing was stopped, e.g. by a settings change
    // with background processing off (review C M2). Slice once more, then give up instead of waiting for the timeout.
    return o.idle_reslices < 1 ? Action::ResliceIdle : Action::FailInterrupted;
}

} // namespace Slic3r::GUI::SmartSupportsWait

#endif // slic3r_SmartSupportsWait_hpp_
