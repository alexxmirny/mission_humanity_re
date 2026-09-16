//
// sim_invasion_due_check_selftest.cpp -- `simtest` offline oracle for llm_strat_invasion_due_check
// @0x0049949d (sim/resid/sim_invasion_due_check.h/.cpp, RI-SIM / sim_resid batch F).
//
// NO SHADOW SITE (sim_resid rule 1, batch F) -- this offline oracle is the only verification. Expected
// behaviour hand-derived from the DISASSEMBLY
// (tmp/decomp_sim_resid/llm_strat_invasion_due_check_0049949d.asm), not the .c draft beside it:
//
//   0x004994bd-0x004994c8 FLDZ/FCOMP invasion_time[planet]/JNC -- armed gate: timer must be STRICTLY
//     > 0.0 (JNC = "not carry" = timer <= 0.0 -> not due).
//   0x004994d2-0x004994e1 FLD invasion_time[planet]/FCOMP game_clock/JC -- passed gate: timer must be
//     STRICTLY < game_clock (JC = carry set = timer < game_clock; timer == game_clock does NOT fire).
//   0x004994e5: fires llm_strat_spawn_enemy_landing.
//   0x004994f5/0x004994ff: clears the CURRENT planet's timer to the raw 64-bit pattern
//     0xBFF0000000000000, i.e. the double -1.0.
//   0x00499509-0x00499518: landing return == 0 -> llm_strat_player_presence_lost(local_player_slot, 0);
//     landing return != 0 -> not called.
//   0x0049951d/0x00499526/0x0049952d: the corrected `int` return -- 1 on the fired arm (either landing
//     outcome), 0 on either not-due arm.
//   The planet index used throughout is [G_PLANET_INDEX*8 + 0xe585f8] -- only the CURRENT planet's
//   slot is ever read or written.
//
#include "sim/resid/sim_invasion_due_check.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

int32_t g_spawn_return = 1;
int32_t g_spawn_calls  = 0;
int32_t stub_spawn_enemy_landing() {
    ++g_spawn_calls;
    return g_spawn_return;
}

struct PresenceCall {
    uint32_t player, mode;
};
std::vector<PresenceCall> g_presence_calls;
uint32_t                  stub_player_presence_lost(uint32_t player, uint32_t mode) {
    g_presence_calls.push_back({player, mode});
    return 0;
}

const invasion_due_check_calls g_calls = {
    &stub_spawn_enemy_landing,
    &stub_player_presence_lost,
};

// Clears the RECORDS only. g_spawn_return is mock CONFIGURATION, not a record, and resetting it here
// is what made T6 silently run the landing-SUCCEEDS path: the case set g_spawn_return = 0 before
// calling run(), and run()'s reset put it back to 1 (caught 2026-08-31 when T6 failed on a correct
// translation). Configuration now arrives as run()'s parameter, so a case cannot be overwritten by
// its own harness.
void reset_calls() {
    g_spawn_calls = 0;
    g_presence_calls.clear();
}

int32_t run(sim_fixture &fx, int32_t spawn_return) {
    reset_calls();
    g_spawn_return = spawn_return;
    sim_store own  = fx.store();
    return detail::invasion_due_check(fx.view(), own, g_calls);
}

} // namespace

void run_invasion_due_check_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- gate (a) boundary: timer EXACTLY 0.0 -- not armed (JNC @0x004994c8 requires timer > 0.0).
    // Not due: no calls, return 0, timer left exactly as seeded.
    // =================================================================================================
    {
        fx.reset();
        fx.planet_index                                  = 2;
        fx.game_clock                                    = 100.0;
        fx.planet_invasion_time[(size_t)fx.planet_index] = 0.0;

        const int32_t ret = run(fx, /*spawn_return=*/1);

        ck_eq((uint32_t)ret, 0u, "T1: timer==0.0 -- not armed, 0x004994bd-0x004994c8 FLDZ/FCOMP/JNC");
        ck_eq((uint32_t)g_spawn_calls, 0u, "T1: spawn_enemy_landing never called on the not-due arm");
        ck(g_presence_calls.empty(), "T1: player_presence_lost never called on the not-due arm");
        ck_eq_d(fx.planet_invasion_time[(size_t)fx.planet_index], 0.0, "T1: timer untouched (no clear on the not-due arm)");
    }

    // =================================================================================================
    // T2 -- gate (a): timer NEGATIVE (-1.0, the already-cleared value) -- not armed. Not due.
    // =================================================================================================
    {
        fx.reset();
        fx.planet_index                                  = 2;
        fx.game_clock                                    = 100.0;
        fx.planet_invasion_time[(size_t)fx.planet_index] = -1.0;

        const int32_t ret = run(fx, /*spawn_return=*/1);

        ck_eq((uint32_t)ret, 0u, "T2: timer==-1.0 -- not armed, 0x004994bd-0x004994c8 FLDZ/FCOMP/JNC");
        ck_eq((uint32_t)g_spawn_calls, 0u, "T2: spawn_enemy_landing never called on the not-due arm");
        ck(g_presence_calls.empty(), "T2: player_presence_lost never called on the not-due arm");
        ck_eq_d(fx.planet_invasion_time[(size_t)fx.planet_index], -1.0, "T2: timer untouched");
    }

    // =================================================================================================
    // T3 -- gate (b) boundary: timer ARMED but EXACTLY EQUAL to game_clock -- JC @0x004994e1 requires
    // STRICT timer < game_clock, so equal does NOT fire. This is the boundary a `<=` mutation flips.
    // =================================================================================================
    {
        fx.reset();
        fx.planet_index                                  = 2;
        fx.game_clock                                    = 50.0;
        fx.planet_invasion_time[(size_t)fx.planet_index] = 50.0;

        const int32_t ret = run(fx, /*spawn_return=*/1);

        ck_eq((uint32_t)ret, 0u, "T3: timer==game_clock -- not passed, 0x004994d2-0x004994e1 FLD/FCOMP/JC");
        ck_eq((uint32_t)g_spawn_calls, 0u, "T3: spawn_enemy_landing never called on the not-due arm");
        ck(g_presence_calls.empty(), "T3: player_presence_lost never called on the not-due arm");
        ck_eq_d(fx.planet_invasion_time[(size_t)fx.planet_index], 50.0, "T3: timer untouched");
    }

    // =================================================================================================
    // T4 -- gate (b): timer ARMED but ABOVE game_clock (clearly not yet passed). Not due.
    // =================================================================================================
    {
        fx.reset();
        fx.planet_index                                  = 2;
        fx.game_clock                                    = 50.0;
        fx.planet_invasion_time[(size_t)fx.planet_index] = 100.0;

        const int32_t ret = run(fx, /*spawn_return=*/1);

        ck_eq((uint32_t)ret, 0u, "T4: timer > game_clock -- not passed, 0x004994d2-0x004994e1 FLD/FCOMP/JC");
        ck_eq((uint32_t)g_spawn_calls, 0u, "T4: spawn_enemy_landing never called on the not-due arm");
        ck(g_presence_calls.empty(), "T4: player_presence_lost never called on the not-due arm");
        ck_eq_d(fx.planet_invasion_time[(size_t)fx.planet_index], 100.0, "T4: timer untouched");
    }

    // =================================================================================================
    // T5 -- FIRED, landing SUCCEEDS (nonzero): armed && passed both hold. spawn_enemy_landing called
    // once; player_presence_lost NOT called (0x00499509 JNZ skips it); timer cleared to the raw
    // 0xBFF0000000000000 pattern, i.e. exactly -1.0 (0x004994f5/0x004994ff); return 1
    // (0x0049951d, reached on the success side of the landing check too).
    //
    // ALSO pins the planet indexing: several DISTINCT planets are seeded, G_PLANET_INDEX is non-zero
    // (5), and every slot OTHER than the target must be left exactly as seeded -- a wrong index or a
    // whole-array write would otherwise still pass T1-T4/T6.
    // =================================================================================================
    {
        fx.reset();
        fx.planet_index = 5;
        fx.game_clock   = 50.0;
        // Decoys on other slots, all distinct and non-default so a mis-indexed read/write is visible.
        fx.planet_invasion_time[0] = 11.0;
        fx.planet_invasion_time[1] = 22.0;
        fx.planet_invasion_time[2] = 33.0;
        fx.planet_invasion_time[3] = 44.0;
        fx.planet_invasion_time[4] = 5.0; // armed+passed itself, but NOT the current planet
        fx.planet_invasion_time[6] = 66.0;
        fx.planet_invasion_time[7] = -1.0;
        // Target planet 5: armed (>0.0) and passed (<50.0).
        fx.planet_invasion_time[5] = 10.0;

        const int32_t ret = run(fx, /*spawn_return=*/1);

        ck_eq((uint32_t)ret, 1u, "T5: fired arm returns 1, 0x0049951d");
        ck_eq((uint32_t)g_spawn_calls, 1u, "T5: spawn_enemy_landing called exactly once, 0x004994e5");
        ck(g_presence_calls.empty(), "T5: player_presence_lost NOT called -- landing succeeded, 0x0049950d JNZ skip");
        ck_eq_d(fx.planet_invasion_time[5], -1.0,
                "T5: planet 5's timer cleared to -1.0, the 0xBFF0000000000000 two-dword store at "
                "0x004994f5/0x004994ff");
        // Planet indexing pin: every other slot untouched.
        ck_eq_d(fx.planet_invasion_time[0], 11.0, "T5: planet 0 untouched -- indexing pin, [G_PLANET_INDEX*8+0xe585f8]");
        ck_eq_d(fx.planet_invasion_time[1], 22.0, "T5: planet 1 untouched -- indexing pin");
        ck_eq_d(fx.planet_invasion_time[2], 33.0, "T5: planet 2 untouched -- indexing pin");
        ck_eq_d(fx.planet_invasion_time[3], 44.0, "T5: planet 3 untouched -- indexing pin");
        ck_eq_d(fx.planet_invasion_time[4], 5.0, "T5: planet 4 untouched (armed+passed itself, but NOT current) -- indexing pin");
        ck_eq_d(fx.planet_invasion_time[6], 66.0, "T5: planet 6 untouched -- indexing pin");
        ck_eq_d(fx.planet_invasion_time[7], -1.0, "T5: planet 7 untouched -- indexing pin");
    }

    // =================================================================================================
    // T6 -- FIRED, landing FAILS (returns 0): player_presence_lost(local_player_slot, mode=0) called
    // exactly once (0x00499509-0x00499518, EAX=MOVZX'd local slot, EDX=0); timer still cleared to
    // -1.0; return still 1 (0x0049951d is reached on this arm too, not only on success).
    // =================================================================================================
    {
        fx.reset();
        fx.planet_index            = 3;
        fx.game_clock              = 50.0;
        fx.local_player_slot       = 0x1234; // distinctive non-zero value the mock must record verbatim
        fx.planet_invasion_time[3] = 10.0;   // armed && passed

        const int32_t ret = run(fx, /*spawn_return=*/0); // landing FAILS

        ck_eq((uint32_t)ret, 1u, "T6: fired arm returns 1 even when the landing failed, 0x0049951d");
        ck_eq((uint32_t)g_spawn_calls, 1u, "T6: spawn_enemy_landing called exactly once, 0x004994e5");
        ck(g_presence_calls.size() == 1, "T6: player_presence_lost called exactly once, 0x00499509 JNZ falls through");
        if (g_presence_calls.size() == 1) {
            ck_eq(g_presence_calls[0].player, 0x1234u,
                  "T6: player_presence_lost's player arg is the MOVZX'd local_player_slot, 0x00499511-0x00499518");
            ck_eq(g_presence_calls[0].mode, 0u, "T6: player_presence_lost's mode arg is 0 (XOR EDX,EDX), 0x0049950f");
        }
        ck_eq_d(fx.planet_invasion_time[3], -1.0,
                "T6: planet 3's timer cleared to -1.0 on the failed-landing arm too, 0x004994f5/0x004994ff");
    }
}

} // namespace mh::sim::test
