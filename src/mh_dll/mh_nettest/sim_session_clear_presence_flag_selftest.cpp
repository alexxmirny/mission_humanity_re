//
// sim_session_clear_presence_flag_selftest.cpp -- `simtest` offline oracle for
// llm_game_session_clear_system_presence_flag @0x004987ae (sim/resid/sim_session_clear_presence_flag.h/.cpp,
// RI-SIM sim_resid batch).
//
// NO SHADOW SITE -- session-entry-only writer, proof:OFFLINE (see the header banner). This file is the
// only verification. ZERO outward calls, so detail::session_clear_system_presence_flag takes no `_calls`
// struct.
//
// EXPECTED BEHAVIOUR, from the header banner's own derivation
// (sim/resid/sim_session_clear_presence_flag.h):
//   0x004987e0-0x004987f3: gate. Runs only when Planets[G_PLANET_INDEX].enemy (+0x421) is 0 or 1
//     (signed JLE skips when 2 <= enemy). THE FIELD IS enemy, NOT asteriods (+0x419); T1/T2 below pin
//     that, and the module header banner carries the full offset derivation.
//   0x004987c6/0x004987cd: the outer "loop" is bounds [2,3) -- exactly one iteration, hardcoded player
//     slot 2. Not a real loop over players.
//   0x00498807-0x0049880b / 0x0049881a-0x0049882d: sweeps planets j=1..0x1f, skipping any whose
//     Planets[j].system_index != CurrentSystem.
//   0x00498835-0x0049884b: for a planet in the current system, G_PLANET_STATUS[j]==UNKNOWN(0) with
//     j != G_PLANET_INDEX blocks the clear (still-absent flag cleared to false). j == G_PLANET_INDEX is
//     EXEMPT from this arm even at status 0 -- it falls through to the presence check instead.
//   0x00498863 / 0x0049887b: buildings_alive[j] > 0 OR units_alive[j] > 0 (tracked player = slot 2,
//     _G_LLM_STRAT_PLAYERS[2]) also blocks the clear.
//   0x0049889a: if the still-absent flag survives all 31 planets, `AND byte ptr [...],0xfd` clears ONLY
//     bit 0x2 (STATUS_ALIVE) of _G_LLM_STRAT_PLAYERS[2].status_flags -- a byte-wide AND, so other bits
//     of the (wider) status_flags word must be preserved.
//
#include "sim/resid/sim_session_clear_presence_flag.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

constexpr int32_t TRACKED_PLAYER = 2;

// A planet/system layout where NOTHING in the sweep (j=1..0x1f) can block the clear: every planet's
// system_index is left at the fixture's zeroed default, and current_system is set to a value no planet
// carries, so every j is skipped by the system filter and the still-absent flag survives untouched.
// Individual cases override one planet's fields to test a specific arm.
void seed_no_blockers(sim_fixture &fx) { fx.current_system = 99; }

} // namespace

void run_session_clear_presence_flag_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- THE FIELD, direction A: enemy=0 (gate should RUN, 0x004987ed/0x004987f3), asteriods=5 (the
    // WRONG field would read >=2 and skip). Everything else set so the clear fires when the gate runs.
    // =================================================================================================
    {
        fx.reset();
        seed_no_blockers(fx);
        fx.planet_index                          = 0;
        fx.cfg_planets[0].enemy                  = 0;
        fx.cfg_planets[0].asteriods              = 5;
        fx.profiles[TRACKED_PLAYER].status_flags = 0x7;

        sim_store own = fx.store();
        detail::session_clear_system_presence_flag(fx.view(), own);

        ck_eq(fx.profiles[TRACKED_PLAYER].status_flags, 0x5u,
              "T1: FIELD=enemy(0x421)=0 -> gate RUNS, ALIVE cleared 0x7->0x5, 0x004987ed/0x0049889a "
              "(reading .asteriods=5 here would wrongly SKIP)");
    }

    // =================================================================================================
    // T2 -- THE FIELD, direction B: enemy=5 (gate should SKIP), asteriods=0 (the WRONG field would read
    // <2 and wrongly RUN). Same "nothing blocks" layout, so the ONLY thing suppressing the write is the
    // gate itself.
    // =================================================================================================
    {
        fx.reset();
        seed_no_blockers(fx);
        fx.planet_index                          = 0;
        fx.cfg_planets[0].enemy                  = 5;
        fx.cfg_planets[0].asteriods              = 0;
        fx.profiles[TRACKED_PLAYER].status_flags = 0x7;

        sim_store own = fx.store();
        detail::session_clear_system_presence_flag(fx.view(), own);

        ck_eq(fx.profiles[TRACKED_PLAYER].status_flags, 0x7u,
              "T2: FIELD=enemy(0x421)=5 -> gate SKIPS, status_flags untouched, 0x004987f3 "
              "(reading .asteriods=0 here would wrongly RUN and clear the bit)");
    }

    // =================================================================================================
    // T3 -- gate boundary: enemy=1 runs (0 or 1 both pass), enemy=2 skips (JLE, signed EAX(=2)<=field).
    // An off-by-one in either direction flips one of these two.
    // =================================================================================================
    {
        fx.reset();
        seed_no_blockers(fx);
        fx.planet_index                          = 0;
        fx.cfg_planets[0].enemy                  = 1;
        fx.profiles[TRACKED_PLAYER].status_flags = 0x3;

        sim_store own = fx.store();
        detail::session_clear_system_presence_flag(fx.view(), own);

        ck_eq(fx.profiles[TRACKED_PLAYER].status_flags, 0x1u,
              "T3a: enemy=1 -> gate RUNS (boundary, still < 2), ALIVE cleared, 0x004987ed-0x004987f3");
    }
    {
        fx.reset();
        seed_no_blockers(fx);
        fx.planet_index                          = 0;
        fx.cfg_planets[0].enemy                  = 2;
        fx.profiles[TRACKED_PLAYER].status_flags = 0x3;

        sim_store own = fx.store();
        detail::session_clear_system_presence_flag(fx.view(), own);

        ck_eq(fx.profiles[TRACKED_PLAYER].status_flags, 0x3u,
              "T3b: enemy=2 -> gate SKIPS (boundary, JLE fires), status_flags untouched, 0x004987f3");
    }

    // =================================================================================================
    // T4 -- the hardcoded player: the outer "loop" @0x004987c6/@0x004987cd runs exactly once, for slot
    // 2. Slots 1 and 3 get a presence-free roster (would themselves qualify for the clear if the body
    // ever ran for them) and their ALIVE bit SET -- both must keep it; only slot 2 may be cleared.
    // =================================================================================================
    {
        fx.reset();
        seed_no_blockers(fx);
        fx.planet_index                          = 0;
        fx.cfg_planets[0].enemy                  = 0;
        fx.profiles[1].status_flags              = 0x2;
        fx.profiles[TRACKED_PLAYER].status_flags = 0x2;
        fx.profiles[3].status_flags              = 0x2;

        sim_store own = fx.store();
        detail::session_clear_system_presence_flag(fx.view(), own);

        ck_eq(fx.profiles[1].status_flags, 0x2u,
              "T4a: slot 1 ALIVE bit untouched -- the outer bound is [2,3), single iteration, "
              "0x004987c6/0x004987cd (deliberately preserved from the original)");
        ck_eq(fx.profiles[TRACKED_PLAYER].status_flags, 0x0u,
              "T4b: slot 2 (the ONLY processed player) has ALIVE cleared, 0x0049889a");
        ck_eq(fx.profiles[3].status_flags, 0x2u,
              "T4c: slot 3 ALIVE bit untouched -- same single-iteration bound");
    }

    // =================================================================================================
    // T5 -- the system filter @0x0049881a-0x0049882d: a planet WITH presence (units/buildings alive)
    // but in a DIFFERENT system from CurrentSystem must not stop the clear.
    // =================================================================================================
    {
        fx.reset();
        fx.current_system                               = 5;
        fx.planet_index                                 = 0;
        fx.cfg_planets[0].enemy                         = 0;
        fx.cfg_planets[10].system_index                 = 999; // NOT current_system (5)
        fx.planet_status[10]                            = 0;   // would block (unvisited) if it counted
        fx.profiles[TRACKED_PLAYER].buildings_alive[10] = 7;   // would block (presence) if it counted
        fx.profiles[TRACKED_PLAYER].units_alive[10]     = 7;
        fx.profiles[TRACKED_PLAYER].status_flags        = 0x2;

        sim_store own = fx.store();
        detail::session_clear_system_presence_flag(fx.view(), own);

        ck_eq(fx.profiles[TRACKED_PLAYER].status_flags, 0x0u,
              "T5: planet 10's system_index(999) != CurrentSystem(5) -> filtered out BEFORE the "
              "unvisited/presence checks, so its blocking-looking fields are ignored; ALIVE cleared, "
              "0x0049881a-0x0049882d");
    }

    // =================================================================================================
    // T6 -- the unvisited-planet arm @0x00498835-0x0049884b, BLOCKING side: G_PLANET_STATUS[j]==0 with
    // j != G_PLANET_INDEX blocks the clear. Swapping the `!=` for `==` must fail this case.
    // =================================================================================================
    {
        fx.reset();
        fx.current_system               = 5;
        fx.planet_index                 = 0; // != 15
        fx.cfg_planets[0].enemy         = 0;
        fx.cfg_planets[15].system_index = 5; // in the current system
        fx.planet_status[15]            = 0; // UNKNOWN, and j(15) != planet_index(0)

        fx.profiles[TRACKED_PLAYER].status_flags = 0x2;

        sim_store own = fx.store();
        detail::session_clear_system_presence_flag(fx.view(), own);

        ck_eq(fx.profiles[TRACKED_PLAYER].status_flags, 0x2u,
              "T6: planet 15 in-system, UNKNOWN(0), j != G_PLANET_INDEX -> blocks the clear, "
              "status_flags untouched, 0x0049883c-0x0049884b");
    }

    // =================================================================================================
    // T7 -- the unvisited-planet arm, EXEMPTION side: j == G_PLANET_INDEX at status 0 does NOT block via
    // this arm -- it falls through to the presence check instead. With presence also zero here, nothing
    // blocks at all and the clear fires. Swapping the `!=` for `==` (or removing the exemption) would
    // wrongly block this case.
    // =================================================================================================
    {
        fx.reset();
        fx.current_system               = 5;
        fx.planet_index                 = 15; // == the planet under test
        fx.cfg_planets[15].enemy        = 0;  // gate reads Planets[G_PLANET_INDEX] = Planets[15]
        fx.cfg_planets[15].system_index = 5;  // in the current system
        fx.planet_status[15]            = 0;  // UNKNOWN, but j(15) == planet_index(15): exempt
        // buildings_alive[15]/units_alive[15] left at 0 by reset() -- presence check also passes.
        fx.profiles[TRACKED_PLAYER].status_flags = 0x2;

        sim_store own = fx.store();
        detail::session_clear_system_presence_flag(fx.view(), own);

        ck_eq(fx.profiles[TRACKED_PLAYER].status_flags, 0x0u,
              "T7: planet 15 IS G_PLANET_INDEX -> UNKNOWN(0) does NOT block via the unvisited arm "
              "(falls through to the presence check, which is also clear here); ALIVE cleared, "
              "0x00498841-0x00498849");
    }

    // =================================================================================================
    // T8a/T8b -- the presence check @0x00498863 (buildings_alive) / @0x0049887b (units_alive): DIFFERENT
    // values at the same planet index so consuming the wrong array fails either case.
    // =================================================================================================
    {
        fx.reset();
        fx.current_system                               = 5;
        fx.planet_index                                 = 0;
        fx.cfg_planets[0].enemy                         = 0;
        fx.cfg_planets[20].system_index                 = 5; // in-system
        fx.planet_status[20]                            = 1; // VISITED -- reaches the presence check
        fx.profiles[TRACKED_PLAYER].buildings_alive[20] = 5; // blocks
        fx.profiles[TRACKED_PLAYER].units_alive[20]     = 0; // does NOT block on its own
        fx.profiles[TRACKED_PLAYER].status_flags        = 0x2;

        sim_store own = fx.store();
        detail::session_clear_system_presence_flag(fx.view(), own);

        ck_eq(fx.profiles[TRACKED_PLAYER].status_flags, 0x2u,
              "T8a: buildings_alive[20]=5 (units_alive[20]=0) blocks the clear -- reading units_alive "
              "here instead would wrongly clear it, 0x00498863");
    }
    {
        fx.reset();
        fx.current_system                               = 5;
        fx.planet_index                                 = 0;
        fx.cfg_planets[0].enemy                         = 0;
        fx.cfg_planets[20].system_index                 = 5; // in-system
        fx.planet_status[20]                            = 1; // VISITED -- reaches the presence check
        fx.profiles[TRACKED_PLAYER].buildings_alive[20] = 0; // does NOT block on its own
        fx.profiles[TRACKED_PLAYER].units_alive[20]     = 9; // blocks
        fx.profiles[TRACKED_PLAYER].status_flags        = 0x2;

        sim_store own = fx.store();
        detail::session_clear_system_presence_flag(fx.view(), own);

        ck_eq(fx.profiles[TRACKED_PLAYER].status_flags, 0x2u,
              "T8b: units_alive[20]=9 (buildings_alive[20]=0) blocks the clear -- reading "
              "buildings_alive here instead would wrongly clear it, 0x0049887b");
    }

    // =================================================================================================
    // T9 -- the write mask @0x0049889a: `AND byte ptr [...],0xfd` clears ONLY bit 0x2. Seed other bits
    // set (0x7 = bits 0,1,2) and assert the result is 0x5 -- an assignment (status_flags=0) or a wider
    // mask (e.g. clearing 0x3) fails this.
    // =================================================================================================
    {
        fx.reset();
        seed_no_blockers(fx);
        fx.planet_index                          = 0;
        fx.cfg_planets[0].enemy                  = 0;
        fx.profiles[TRACKED_PLAYER].status_flags = 0x7;

        sim_store own = fx.store();
        detail::session_clear_system_presence_flag(fx.view(), own);

        ck_eq(fx.profiles[TRACKED_PLAYER].status_flags, 0x5u,
              "T9: status_flags 0x7 -> 0x5 -- only bit 0x2 cleared, bits 0x1/0x4 preserved, 0x0049889a");
    }

    // =================================================================================================
    // T10 -- the clean negative case: presence exists (blocks via the presence check), nothing is
    // written at all.
    // =================================================================================================
    {
        fx.reset();
        fx.current_system                              = 5;
        fx.planet_index                                = 0;
        fx.cfg_planets[0].enemy                        = 0;
        fx.cfg_planets[7].system_index                 = 5;
        fx.planet_status[7]                            = 1; // visited -> presence check
        fx.profiles[TRACKED_PLAYER].buildings_alive[7] = 3;
        fx.profiles[TRACKED_PLAYER].status_flags       = 0x2;

        sim_store own = fx.store();
        detail::session_clear_system_presence_flag(fx.view(), own);

        ck_eq(fx.profiles[TRACKED_PLAYER].status_flags, 0x2u,
              "T10: player still present on an in-system planet -- status_flags completely untouched");
    }
}

} // namespace mh::sim::test
