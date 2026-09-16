//
// sim_time_resync_selftest.cpp -- `simtest` offline oracle for llm_strat_time_resync_and_tick
// @0x00449e21 (sim/resid/sim_time_resync.h/.cpp, RI-SIM sim_resid batch F).
//
// NO SHADOW SITE (see the header banner, G30): llm_strat_time_tick reaches back into THIS function,
// so an armed shadow would recurse through the comparison machinery. This offline oracle
// (net_selftest simtest) is the ONLY verification.
//
// EXPECTED BEHAVIOUR, from the DISASSEMBLY
// (tmp/decomp_sim_resid/llm_strat_time_resync_and_tick_00449e21.asm) -- the .asm is the spec, never
// the .c beside it:
//   0x00449e39: CALL llm_time_get_ticks_ms -- side effect only, return value discarded (EAX is
//     overwritten by the very next CALL before anything reads it).
//   0x00449e3e: CALL time_GetCurrentTime.
//   0x00449e43: FSTP double ptr [LAST_GAME_TIME] -- stores time_GetCurrentTime's return, and ONLY
//     that value; runs strictly BEFORE the next call.
//   0x00449e49: CALL llm_strat_time_tick -- unconditional, return value discarded.
// All three calls are UNCONDITIONAL (no branches anywhere in this 0x37-byte body), which is exactly
// the shape a naive oracle would pass vacuously on -- so this file pins the call COUNT (exactly 1
// each), the ORDER (ticks_ms, then current_time, then time_tick), the VALUE that lands in
// last_game_time (current_time's return, not the pre-seeded value, not ticks_ms's return), and that
// the store at 0x00449e43 happens BEFORE the CALL at 0x00449e49 (time_tick's mock reads
// last_game_time itself and records what it saw).
//
#include "sim/resid/sim_time_resync.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// Distinctive, non-symmetric, non-default values per sim_test_support.h's fixture rule -- chosen so
// a swap (e.g. storing ticks_ms's return instead of current_time's, or storing the pre-seeded value
// unchanged) fails loudly rather than by accident matching.
constexpr uint32_t TICKS_MS_RETURN           = 424242u;
constexpr double   CURRENT_TIME_RETURN       = 1234.5;
constexpr double   PRE_SEEDED_LAST_GAME_TIME = 555.75;

int32_t g_seq = 0; // global call-order counter, shared by every mock below (same idiom as
                   // sim_advisor_tick_selftest.cpp / sim_game_update_progress_selftest.cpp)

uint32_t g_ticks_ms_calls = 0;
int32_t  g_ticks_ms_seq   = -1;
uint32_t rec_get_ticks_ms() {
    ++g_ticks_ms_calls;
    g_ticks_ms_seq = g_seq++;
    return TICKS_MS_RETURN;
}

uint32_t g_current_time_calls = 0;
int32_t  g_current_time_seq   = -1;
double   rec_get_current_time() {
    ++g_current_time_calls;
    g_current_time_seq = g_seq++;
    return CURRENT_TIME_RETURN;
}

// time_tick's real signature takes no arguments, so the only way this mock can observe whether
// LAST_GAME_TIME was already stored by the time it runs is a captured pointer to the fixture --
// same technique sim_advisor_tick_selftest.cpp uses for its T19 order pin (g_mutate_fixture).
sim_fixture *g_fx = nullptr;

uint32_t g_time_tick_calls              = 0;
int32_t  g_time_tick_seq                = -1;
double   g_time_tick_observed_last_time = -1.0; // what LAST_GAME_TIME held AT CALL TIME
int32_t  rec_time_tick() {
    ++g_time_tick_calls;
    g_time_tick_seq = g_seq++;
    if (g_fx != nullptr) g_time_tick_observed_last_time = g_fx->last_game_time;
    return 0;
}

// The C4 pacing chain (SIM-SAVE-DIV). Not an original call -- an instrument prelude the body must
// run BEFORE time_tick on the path where the direct bind skips llm_strat_time_tick's entry detour.
// Recorded here so the ORDER is asserted offline: a prelude that runs after the body it precedes is
// worthless, and nothing else in the suite could see it.
uint32_t g_pace_calls = 0;
int32_t  g_pace_seq   = -1;
void     rec_pace() {
    ++g_pace_calls;
    g_pace_seq = g_seq++;
}

const time_resync_and_tick_calls g_calls = {
    rec_get_ticks_ms,
    rec_get_current_time,
    rec_pace,
    rec_time_tick,
};

void reset_recorders() {
    g_seq                          = 0;
    g_ticks_ms_calls               = 0;
    g_ticks_ms_seq                 = -1;
    g_current_time_calls           = 0;
    g_current_time_seq             = -1;
    g_time_tick_calls              = 0;
    g_time_tick_seq                = -1;
    g_time_tick_observed_last_time = -1.0;
    g_pace_calls                   = 0;
    g_pace_seq                     = -1;
}

} // namespace

void run_time_resync_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- the body's ONE straight-line path (no branches at all): all three outward calls fire
    // exactly once, in .asm order, LAST_GAME_TIME takes time_GetCurrentTime's return (and ONLY that),
    // the store lands before llm_strat_time_tick runs, and llm_time_get_ticks_ms's return is
    // discarded.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        g_fx              = &fx;
        fx.last_game_time = PRE_SEEDED_LAST_GAME_TIME; // distinct from both CURRENT_TIME_RETURN and
                                                       // TICKS_MS_RETURN -- "it was already that"
                                                       // cannot pass this case

        sim_store own = fx.store();
        detail::time_resync_and_tick(fx.view(), own, g_calls);

        // -- call counts: a body that dropped one of the three calls must fail here --
        ck_eq(g_ticks_ms_calls, 1u, "T1: llm_time_get_ticks_ms called exactly once, CALL 0x00449e39");
        ck_eq(g_current_time_calls, 1u, "T1: time_GetCurrentTime called exactly once, CALL 0x00449e3e");
        ck_eq(g_time_tick_calls, 1u, "T1: llm_strat_time_tick called exactly once, CALL 0x00449e49");

        // -- order: the shared g_seq counter pins the .asm's call ordering --
        ck_eq((uint32_t)g_ticks_ms_seq, 0u,
              "T1: llm_time_get_ticks_ms fires FIRST, CALL 0x00449e39");
        ck_eq((uint32_t)g_current_time_seq, 1u,
              "T1: time_GetCurrentTime fires SECOND, CALL 0x00449e3e");
        ck_eq((uint32_t)g_time_tick_seq, 3u,
              "T1: llm_strat_time_tick fires LAST, CALL 0x00449e49");

        // -- THE C4 PACING CHAIN (SIM-SAVE-DIV): exactly once, and IMMEDIATELY BEFORE time_tick.
        // It is not an original call, so nothing about the .asm pins it -- what pins it is what it is
        // FOR: it replays the prelude llm_strat_time_tick's entry detour would have run, and a
        // prelude that lands after the body it precedes has done nothing. The order assertion is the
        // whole value of testing it here, since no rig run can see a hook that is a no-op unless a
        // rebind row is armed.
        ck_eq(g_pace_calls, 1u, "T1: the pacing chain fires exactly once");
        ck_eq((uint32_t)g_pace_seq, 2u,
              "T1: the pacing chain fires THIRD, immediately BEFORE llm_strat_time_tick -- a prelude "
              "that runs after its body is worthless");
        ck((uint32_t)g_pace_seq < (uint32_t)g_time_tick_seq,
           "T1: pacing chain strictly precedes llm_strat_time_tick");

        // -- value: LAST_GAME_TIME takes time_GetCurrentTime's return, nothing else --
        ck_eq_d(fx.last_game_time, CURRENT_TIME_RETURN,
                "T1: LAST_GAME_TIME = time_GetCurrentTime(), FSTP 0x00449e43");

        // -- ticks_ms's return is discarded: it must NOT be what landed in LAST_GAME_TIME --
        ck(fx.last_game_time != (double)TICKS_MS_RETURN,
           "T1: llm_time_get_ticks_ms's return (CALL 0x00449e39) is discarded, never stored anywhere");

        // -- ordering proof: llm_strat_time_tick's mock reads LAST_GAME_TIME itself; if it saw the
        // PRE-seeded value instead of CURRENT_TIME_RETURN, the FSTP at 0x00449e43 ran AFTER (or the
        // CALL at 0x00449e49 ran before) it should have --
        ck_eq_d(g_time_tick_observed_last_time, CURRENT_TIME_RETURN,
                "T1: FSTP 0x00449e43 (LAST_GAME_TIME store) happens BEFORE CALL 0x00449e49 "
                "(llm_strat_time_tick) runs");

        g_fx = nullptr;
    }
}

} // namespace mh::sim::test
