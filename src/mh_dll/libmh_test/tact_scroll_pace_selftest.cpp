//
// tact_scroll_pace_selftest.cpp -- TSCR1: the tactical camera-scroll pacer (tact/tact_scroll_pace.h) on a fake
// game clock. The retail splice (mh/seams/tact_scroll.cpp) and libmh's tact_frame.cpp step 14 both call it, so
// this is the arithmetic oracle; the end-to-end measurement is the tact_scroll rig row.
//
#include "tact/tact_scroll_pace.h"
#include "tact_test_support.h"

namespace mh::tact::test {

namespace {

// Hold one direction for `seconds` of game clock at a frame interval of `dt`; return the steps taken.
int hold(scroll_dir d, double seconds, double dt, int px_s) {
    scroll_pacer p;
    int          total = 0;
    for (double t = 1.0; t < 1.0 + seconds - 1e-9; t += dt) {
        p.begin_frame(t);
        total += p.steps(d, true, t, px_s);
    }
    return total;
}

} // namespace

void run_scroll_pace_tests() {
    // T1: a direction becoming active steps at once (a tap = one tile) and not again before the interval.
    {
        scroll_pacer p;
        p.begin_frame(1.0);
        ck_eq((uint32_t)p.steps(SCROLL_RIGHT, true, 1.0, 240), 1u, "T1: first active frame steps once");
        p.begin_frame(1.01);
        ck_eq((uint32_t)p.steps(SCROLL_RIGHT, true, 1.01, 240), 0u, "T1: the next frame inside the interval does not");
    }
    ck_eq((uint32_t)kScrollDefaultPxS, 480u, "T0: the shipped default is 480 px/s (user, 2026-10-24)");
    // T2: steady rate, independent of the frame rate. 240 px/s = 7.5 columns/s, 10 rows/s; over 4 s that is 30 + 1
    // (the immediate first step) columns and 40 + 1 rows, at 60 fps and at 30 fps alike.
    for (double dt : {1.0 / 60.0, 1.0 / 30.0}) {
        const int cols = hold(SCROLL_RIGHT, 4.0, dt, 240);
        const int rows = hold(SCROLL_DOWN, 4.0, dt, 240);
        ck_eq((uint32_t)(cols >= 29 && cols <= 32), 1u, "T2: columns per 4 s ~ 30 at any frame rate");
        ck_eq((uint32_t)(rows >= 39 && rows <= 42), 1u, "T2: rows per 4 s ~ 40 at any frame rate");
    }
    // T3: a long frame takes at most 2 steps and re-bases (no catch-up burst).
    {
        scroll_pacer p;
        p.begin_frame(1.0);
        p.steps(SCROLL_LEFT, true, 1.0, 240);
        p.begin_frame(1.4); // 0.4 s later = 3 intervals owed (0.1333 s each)
        ck_eq((uint32_t)p.steps(SCROLL_LEFT, true, 1.4, 240), 2u, "T3: capped at 2 steps per frame");
        p.begin_frame(1.41);
        ck_eq((uint32_t)p.steps(SCROLL_LEFT, true, 1.41, 240), 0u, "T3: re-based, the third step is not replayed");
    }
    // T4: release then press again steps at once; a gap longer than kGapReset forgets the held state.
    {
        scroll_pacer p;
        p.begin_frame(1.0);
        p.steps(SCROLL_UP, true, 1.0, 240);
        p.begin_frame(1.02);
        p.steps(SCROLL_UP, false, 1.02, 240);
        p.begin_frame(1.04);
        ck_eq((uint32_t)p.steps(SCROLL_UP, true, 1.04, 240), 1u, "T4: a new press steps at once");
        p.begin_frame(3.0); // the block was not reached for 2 s (button down / blast step)
        ck_eq((uint32_t)p.steps(SCROLL_UP, true, 3.0, 240), 1u, "T4: after a gap the first frame steps once");
        p.begin_frame(0.5); // the clock went backwards (new mission)
        ck_eq((uint32_t)p.steps(SCROLL_UP, true, 0.5, 240), 1u, "T4: a backwards clock resets");
    }
    // T5: px_s = 0 never steps here (the caller falls back to the retail one-per-frame).
    {
        scroll_pacer p;
        p.begin_frame(1.0);
        ck_eq((uint32_t)p.steps(SCROLL_DOWN, true, 1.0, 0), 0u, "T5: px_s 0 owes nothing");
    }
    // T6: the opposite directions are independent (left+right both held both step, like retail).
    {
        scroll_pacer p;
        p.begin_frame(1.0);
        const int l = p.steps(SCROLL_LEFT, true, 1.0, 240);
        const int r = p.steps(SCROLL_RIGHT, true, 1.0, 240);
        ck_eq((uint32_t)(l == 1 && r == 1), 1u, "T6: each direction keeps its own schedule");
    }
}

} // namespace mh::tact::test
