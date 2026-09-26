//
// diag_counter_selftest.cpp -- `net_selftest.exe diagtest`: mh_diag_counter.h's first-hit + rollup
// state machine, with no game, no rig and no log file.
//
// WHY THIS EXISTS. tooling:TL-SUITE-COUNTERS / dead-end G101: a counter whose only print is
// inside a graceful-shutdown path is invisible under a KILLED rig run. mh_diag_counter.h is the
// shared fix -- a first-hit signal plus a "has this moved since the last rollup" gate -- and both are
// pure state transitions over three plain fields, so (like mh_net_watchdog.h's watchdogtest) they are
// asserted directly rather than through a log file a rig run might kill before it is flushed.
//
#include <stdio.h>

#include "include/mh_diag_counter.h"

namespace {

int g_checks = 0, g_fails = 0;

void check(const char *what, bool ok) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        printf("  FAIL: %s\n", what);
    }
}

} // namespace

int run_diagtest() {
    printf("=== diagtest (G101: a counter reported only at teardown is unmeasured under a KILL) ===\n");

    // ---- 1. first-hit fires exactly once, on the transition off zero -----------------------------
    {
        mh_diag_counter c = MH_DIAG_COUNTER_INIT;
        check("a fresh counter reads zero", c.count == 0);
        check("the first increment reports first-hit", mh_diag_counter_note(&c, 1) != 0);
        check("the count reflects the increment", c.count == 1);
        check("the second increment does NOT report first-hit again",
              mh_diag_counter_note(&c, 1) == 0);
        check("...even after many more increments", mh_diag_counter_note(&c, 1) == 0);
        check("the count keeps accumulating regardless", c.count == 3);
    }
    // ---- 2. a delta straight to a large value still only fires once -------------------------------
    {
        mh_diag_counter c = MH_DIAG_COUNTER_INIT;
        check("a multi-unit first jump still reports first-hit", mh_diag_counter_note(&c, 5) != 0);
        check("the count carries the whole delta", c.count == 5);
        check("a second multi-unit bump is not a first-hit", mh_diag_counter_note(&c, 5) == 0);
    }
    // ---- 3. a delta of zero never counts as a hit (an accounting no-op, not an event) --------------
    {
        mh_diag_counter c = MH_DIAG_COUNTER_INIT;
        check("a zero delta on a fresh counter is not a first-hit", mh_diag_counter_note(&c, 0) == 0);
        check("...and does not arm first_hit_done", c.first_hit_done == 0);
        check("a real increment right after still reports first-hit", mh_diag_counter_note(&c, 1) != 0);
    }
    // ---- 4. reset re-arms first-hit for the next match/session ------------------------------------
    {
        mh_diag_counter c = MH_DIAG_COUNTER_INIT;
        (void)mh_diag_counter_note(&c, 1);
        (void)mh_diag_counter_rollup_due(&c); // consume the pending rollup too
        mh_diag_counter_reset(&c);
        check("reset zeroes the count", c.count == 0);
        check("reset zeroes the rollup mark", c.last_reported == 0);
        check("reset re-arms first-hit for the NEXT session",
              mh_diag_counter_note(&c, 1) != 0);
    }

    // ---- 5. rollup_due: only when the count has moved since the last accepted rollup ---------------
    {
        mh_diag_counter c = MH_DIAG_COUNTER_INIT;
        check("a counter that never moved has no rollup due", mh_diag_counter_rollup_due(&c) == 0);
        mh_diag_counter_note(&c, 3);
        check("a moved counter has a rollup due", mh_diag_counter_rollup_due(&c) != 0);
        check("...and the SAME tick right after does not (nothing new happened)",
              mh_diag_counter_rollup_due(&c) == 0);
        mh_diag_counter_note(&c, 2);
        check("a further increment re-arms the rollup", mh_diag_counter_rollup_due(&c) != 0);
        check("consuming it clears it again", mh_diag_counter_rollup_due(&c) == 0);
    }
    // ---- 6. rollup and first-hit are independent signals -------------------------------------------
    // A caller may consume the rollup on a cadence that runs BEFORE it ever checks first-hit (or vice
    // versa, or never checks one of the two at all) -- the two must not clobber each other's state.
    {
        mh_diag_counter c = MH_DIAG_COUNTER_INIT;
        check("rollup_due before any increment is false", mh_diag_counter_rollup_due(&c) == 0);
        const int hit = mh_diag_counter_note(&c, 1);
        check("first-hit still fires after an earlier rollup_due poll", hit != 0);
        check("rollup is independently due after the same increment",
              mh_diag_counter_rollup_due(&c) != 0);
    }
    // ---- 7. THE CLAUSE THAT STOPS A VACUOUS FIX -----------------------------------------------------
    // A helper that reports EVERY call as a hit, or a rollup that is ALWAYS due, would pass blocks 1
    // and 5 in isolation but defeat the entire point (a first-hit line per increment is exactly the
    // "shutdown-only" counter's opposite failure -- log spam instead of silence). Both must go quiet
    // once their condition stops holding.
    {
        mh_diag_counter c    = MH_DIAG_COUNTER_INIT;
        int             hits = 0;
        for (int i = 0; i < 50; ++i) hits += (mh_diag_counter_note(&c, 1) != 0) ? 1 : 0;
        check("exactly one first-hit across 50 increments (not vacuously true or false)", hits == 1);

        int rollups = 0;
        for (int i = 0; i < 50; ++i) {
            mh_diag_counter_note(&c, 1);
            rollups += (mh_diag_counter_rollup_due(&c) != 0) ? 1 : 0;
        }
        check("a rollup fires on every one of 50 ticks that each moved the counter (never vacuously "
              "false)",
              rollups == 50);
        int idle_rollups = 0;
        for (int i = 0; i < 50; ++i) idle_rollups += (mh_diag_counter_rollup_due(&c) != 0) ? 1 : 0;
        check("...and zero further rollups fire once nothing moves (never vacuously true)",
              idle_rollups == 0);
    }

    printf("=== %d checks, %d failures ===\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
