// Smart support painting: the panel's WaitingForSlices decision (SmartSupportsWait.hpp), reviews C M1, C M2, C m2.
// Pure logic: no display, no Plater.

#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/Gizmos/SmartSupportsWait.hpp"

using namespace Slic3r::GUI::SmartSupportsWait;

namespace {

// Sliced, consistent, idle: the state in which the job starts.
Observation sliced()
{
    Observation o;
    o.since_start_s   = 3.;
    o.resliced        = true;
    o.since_reslice_s = 3.;
    o.ready           = true;
    o.consistent      = true;
    return o;
}

} // namespace

TEST_CASE("Paint where needed: waiting for the slices", "[SmartSupports][sas-gui]")
{
    SECTION("the job starts on consistent slices and an idle worker only") {
        CHECK(decide(sliced()) == Action::StartJob);
        Observation o = sliced();
        o.worker_idle = false;                          // C m2: never queue behind another job
        CHECK(decide(o) == Action::Wait);
    }
    SECTION("nothing before our reslice call") {
        Observation o = sliced();
        o.resliced = false;
        CHECK(decide(o) == Action::Wait);
    }
    SECTION("a pending model or settings change is waited for (C M1)") {
        Observation o = sliced();
        o.update_scheduled = true;
        CHECK(decide(o) == Action::Wait);
        o.ready = false;
        o.po_found = false;                             // even a missing PrintObject is not final yet
        CHECK(decide(o) == Action::Wait);
    }
    SECTION("an object edited after the slices: reslice, throttled") {
        Observation o = sliced();
        o.consistent      = false;
        o.since_reslice_s = 0.5;
        CHECK(decide(o) == Action::Wait);
        o.since_reslice_s = RetryS + 0.1;
        CHECK(decide(o) == Action::Reslice);
    }
    SECTION("slicing in progress or just requested: wait") {
        Observation o = sliced();
        o.ready           = false;
        o.slicing_running = true;
        o.since_reslice_s = 60.;
        CHECK(decide(o) == Action::Wait);
        o.slicing_running = false;
        o.since_reslice_s = 0.5 * IdleGraceS;           // the slicing start event may still be on its way
        CHECK(decide(o) == Action::Wait);
    }
    SECTION("the slicing stopped before posSlice (C M2): one reslice, then a failure instead of the 15 min timeout") {
        Observation o = sliced();
        o.ready           = false;
        o.since_reslice_s = IdleGraceS + 0.1;
        CHECK(decide(o) == Action::ResliceIdle);
        o.idle_reslices = 1;
        CHECK(decide(o) == Action::FailInterrupted);
        o.slicing_failed = true;                        // the slicer reports an error
        o.idle_reslices  = 0;
        CHECK(decide(o) == Action::FailSlicing);
    }
    SECTION("not in the print, timeout") {
        Observation o = sliced();
        o.po_found = false;
        o.ready    = false;
        CHECK(decide(o) == Action::FailNotInPrint);
        o = sliced();
        o.since_start_s = TimeoutS + 1.;
        CHECK(decide(o) == Action::FailTimeout);
        o.resliced = false;
        CHECK(decide(o) == Action::FailTimeout);
    }
}
