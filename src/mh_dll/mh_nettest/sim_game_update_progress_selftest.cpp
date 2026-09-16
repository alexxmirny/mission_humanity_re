//
// sim_game_update_progress_selftest.cpp -- `simtest` offline oracle for game_UpdateProgress
// (sim/sim_game_update_progress.{h,cpp}, RI-SIM). Every outward call is routed through the
// three-member `_calls` struct, so the whole function (the unconditional `.acquired` write, the
// six-way type dispatch, the PLANET/SYSTEM per-other-player loops, and the shared propagate_unlocks
// tail) is fully offline-coverable -- no live-image call, no float.
//
// EXPECTED VALUES HAND-DERIVED FROM THE DISASSEMBLY (tmp/decomp/game_UpdateProgress_004402b0.asm) per
// the header's own branch-by-branch derivation, not the .cpp. The INVENTION_TYPE_*/PLANET_STATUS_*/
// STRAT_PLAYER_STATUS_SLOT_ENABLED constants are TU-local to the .cpp's anonymous namespace, so this
// file defines its own local copies (values cross-checked against the header banner), same posture
// sim_game_set_event_selftest.cpp's local `EV_*` enum already established for this codebase.
//
#include "sim/sim_game_update_progress.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// Local copies of the .cpp's anonymous-namespace constants (see file banner).
enum : uint8_t {
    INV_TYPE_BUILDING = 1,
    INV_TYPE_UNIT     = 2,
    INV_TYPE_PROJECT  = 3,
    INV_TYPE_PLANET   = 4,
    INV_TYPE_SYSTEM   = 5,
    INV_TYPE_UPGRADE  = 6,
};
constexpr int32_t  PLANET_STATUS_CONQUERED     = 2;
constexpr int32_t  PLANET_STATUS_INVASION      = 4;
constexpr uint32_t STRAT_PLAYER_STATUS_ENABLED = 0x1u;

// A single global sequence counter lets the two call logs below be checked for cross-log ORDER, not
// just membership -- essential for the SYSTEM case (propagate then finalize, per other player, then
// an unconditional trailing finalize, then the shared tail's propagate) and the PLANET case (which
// must show NO finalize_acquire calls at all despite looking superficially like SYSTEM's loop).
int g_seq = 0;

struct FinalizeCall {
    uint16_t player, inv;
    int      seq;
};
struct PropagateCall {
    uint16_t player;
    int      seq;
};
std::vector<FinalizeCall>  g_finalize;
std::vector<PropagateCall> g_propagate;
int32_t                    g_landing_spots_return = 0;
int                        g_landing_spots_calls  = 0;

int32_t stub_count_landing_spots() {
    ++g_landing_spots_calls;
    return g_landing_spots_return;
}
void stub_finalize_acquire(uint16_t player, uint16_t inv) { g_finalize.push_back({player, inv, g_seq++}); }
void stub_propagate_unlocks(uint16_t player) { g_propagate.push_back({player, g_seq++}); }

const game_update_progress_calls g_calls = {stub_count_landing_spots, stub_finalize_acquire,
                                            stub_propagate_unlocks};

void clear_calls() {
    g_seq = 0;
    g_finalize.clear();
    g_propagate.clear();
    g_landing_spots_return = 0;
    g_landing_spots_calls  = 0;
}

} // namespace

void run_update_progress_tests() {
    sim_fixture fx;

    // ---- the unconditional write (0x004402ea-0x00440303): progress[plr][inv].acquired = true, BEFORE
    // the dispatch, in every case including the no-op ones. -------------------------------------------

    // ---- type BUILDING(1): no-op body, only the shared tail fires ----------------------------------
    fx.reset();
    clear_calls();
    fx.cfg_inventions[10].type = INV_TYPE_BUILDING;
    {
        sim_store own = fx.store();
        detail::game_update_progress(fx.view(), own, g_calls, /*plr*/ 2, /*inv*/ 10);
    }
    ck_eq((uint32_t)fx.progress[2 * PROGRESS_ROW_COUNT + 10].acquired, 1u,
          "BUILDING: progress[2][10].acquired=1 regardless of the no-op dispatch");
    ck_eq((uint32_t)g_propagate.size(), 1u, "BUILDING: propagate_unlocks fires exactly once (the tail)");
    ck_eq((uint32_t)g_propagate[0].player, 2u, "BUILDING: propagate_unlocks(plr=2)");
    ck_eq((uint32_t)g_finalize.size(), 0u, "BUILDING: no finalize_acquire");
    ck_eq((uint32_t)g_landing_spots_calls, 0u, "BUILDING: no count_landing_spots (PLANET-only callee)");

    // ---- type UNIT(2): same no-op shape as BUILDING -------------------------------------------------
    fx.reset();
    clear_calls();
    fx.cfg_inventions[11].type = INV_TYPE_UNIT;
    {
        sim_store own = fx.store();
        detail::game_update_progress(fx.view(), own, g_calls, /*plr*/ 3, /*inv*/ 11);
    }
    ck_eq((uint32_t)g_propagate.size(), 1u, "UNIT: propagate_unlocks fires exactly once");
    ck_eq((uint32_t)g_finalize.size(), 0u, "UNIT: no finalize_acquire");

    // ---- type PROJECT(3): finalize_acquire(plr,inv) THEN the shared tail's propagate_unlocks(plr) --
    fx.reset();
    clear_calls();
    fx.cfg_inventions[12].type = INV_TYPE_PROJECT;
    {
        sim_store own = fx.store();
        detail::game_update_progress(fx.view(), own, g_calls, /*plr*/ 4, /*inv*/ 12);
    }
    ck_eq((uint32_t)g_finalize.size(), 1u, "PROJECT: finalize_acquire fires once");
    ck_eq((uint32_t)g_finalize[0].player, 4u, "PROJECT: finalize_acquire(plr=4, ...)");
    ck_eq((uint32_t)g_finalize[0].inv, 12u, "PROJECT: finalize_acquire(..., inv=12)");
    ck_eq((uint32_t)g_propagate.size(), 1u, "PROJECT: propagate_unlocks fires once (the tail)");
    ck((uint32_t)g_finalize[0].seq < (uint32_t)g_propagate[0].seq,
       "PROJECT: finalize_acquire happens BEFORE the tail's propagate_unlocks");

    // ---- type UPGRADE(6): same shape as PROJECT ------------------------------------------------------
    fx.reset();
    clear_calls();
    fx.cfg_inventions[13].type = INV_TYPE_UPGRADE;
    {
        sim_store own = fx.store();
        detail::game_update_progress(fx.view(), own, g_calls, /*plr*/ 5, /*inv*/ 13);
    }
    ck_eq((uint32_t)g_finalize.size(), 1u, "UPGRADE: finalize_acquire fires once");
    ck_eq((uint32_t)g_propagate.size(), 1u, "UPGRADE: propagate_unlocks fires once");

    // ---- default (out-of-range type, e.g. 0 or 7): no-op, only the tail -----------------------------
    fx.reset();
    clear_calls();
    fx.cfg_inventions[14].type = 0; // out of the 1..6 domain
    {
        sim_store own = fx.store();
        detail::game_update_progress(fx.view(), own, g_calls, /*plr*/ 6, /*inv*/ 14);
    }
    ck_eq((uint32_t)g_finalize.size(), 0u, "default type(0): no finalize_acquire");
    ck_eq((uint32_t)g_propagate.size(), 1u, "default type(0): only the tail propagate_unlocks fires");
    fx.reset();
    clear_calls();
    fx.cfg_inventions[15].type = 7; // above the domain too
    {
        sim_store own = fx.store();
        detail::game_update_progress(fx.view(), own, g_calls, /*plr*/ 6, /*inv*/ 15);
    }
    ck_eq((uint32_t)g_finalize.size(), 0u, "default type(7): no finalize_acquire");
    ck_eq((uint32_t)g_propagate.size(), 1u, "default type(7): only the tail propagate_unlocks fires");

    // ==================================================================================================
    // type PLANET(4)
    // ==================================================================================================

    // PLANET, landing_spots>2 and planet_status[index] != INVASION -> planet_status_at(index) forced
    // to CONQUERED. `index` (a DIFFERENT field than `inv`) selects the row -- seeded distinct from inv
    // to catch an index/inv mixup.
    fx.reset();
    clear_calls();
    fx.cfg_inventions[16].type  = INV_TYPE_PLANET;
    fx.cfg_inventions[16].index = 6;  // != inv(16)
    fx.planet_status[6]         = 0;  // != INVASION(4)
    fx.planet_status[16]        = 77; // a DIFFERENT slot (== inv, not == index) -- must stay untouched
    g_landing_spots_return      = 5;  // > 2
    fx.player_side              = 9;  // != plr below -> the per-other-player loop does not run
    {
        sim_store own = fx.store();
        detail::game_update_progress(fx.view(), own, g_calls, /*plr*/ 1, /*inv*/ 16);
    }
    ck_eq((uint32_t)g_landing_spots_calls, 1u, "PLANET: count_landing_spots called exactly once");
    ck_eq((uint32_t)fx.planet_status[6], (uint32_t)PLANET_STATUS_CONQUERED,
          "PLANET: planet_status_at(index=6) forced to CONQUERED");
    ck_eq((uint32_t)fx.planet_status[16], 77u,
          "PLANET: planet_status[inv=16] (the WRONG slot) is untouched -- index, not inv, is used");
    ck_eq((uint32_t)g_finalize.size(), 0u, "PLANET: no finalize_acquire anywhere in this case");
    ck_eq((uint32_t)g_propagate.size(), 1u, "PLANET, plr!=player_side: only the tail propagate_unlocks");

    // PLANET, landing_spots<=2 -> planet_status NOT forced, even though the id differs from INVASION.
    fx.reset();
    clear_calls();
    fx.cfg_inventions[17].type  = INV_TYPE_PLANET;
    fx.cfg_inventions[17].index = 6;
    fx.planet_status[6]         = 0;
    g_landing_spots_return      = 2; // NOT > 2
    fx.player_side              = 9;
    {
        sim_store own = fx.store();
        detail::game_update_progress(fx.view(), own, g_calls, /*plr*/ 1, /*inv*/ 17);
    }
    ck_eq((uint32_t)fx.planet_status[6], 0u, "PLANET landing_spots<=2: planet_status unchanged");

    // PLANET, planet_status[index] already == INVASION -> the guard's second half blocks the write too.
    fx.reset();
    clear_calls();
    fx.cfg_inventions[18].type  = INV_TYPE_PLANET;
    fx.cfg_inventions[18].index = 6;
    fx.planet_status[6]         = PLANET_STATUS_INVASION;
    g_landing_spots_return      = 5; // > 2, but planet_status already INVASION
    fx.player_side              = 9;
    {
        sim_store own = fx.store();
        detail::game_update_progress(fx.view(), own, g_calls, /*plr*/ 1, /*inv*/ 18);
    }
    ck_eq((uint32_t)fx.planet_status[6], (uint32_t)PLANET_STATUS_INVASION,
          "PLANET planet_status==INVASION already: NOT overwritten even though landing_spots>2");

    // PLANET, plr == player_side: the per-other-enabled-player loop runs. propagate_unlocks(i) fires
    // per enabled OTHER player, progress[i][inv].acquired is set, but -- unlike SYSTEM below --
    // finalize_acquire is NEVER called for those players. plr itself is excluded even though its own
    // slot is also "enabled". A disabled other player is skipped entirely.
    fx.reset();
    clear_calls();
    fx.cfg_inventions[19].type  = INV_TYPE_PLANET;
    fx.cfg_inventions[19].index = 20; // distinct from inv(19) again
    fx.planet_status[20]        = 0;
    g_landing_spots_return      = 0; // <=2, isolate the loop behaviour from the planet_status write
    fx.player_side              = 1;
    fx.profiles[1].status_flags = STRAT_PLAYER_STATUS_ENABLED;        // plr itself: enabled but must be excluded
    fx.profiles[3].status_flags = STRAT_PLAYER_STATUS_ENABLED | 0x4u; // other bits set too -- must use &, not ==
    fx.profiles[5].status_flags = 0;                                  // disabled -- must be skipped
    {
        sim_store own = fx.store();
        detail::game_update_progress(fx.view(), own, g_calls, /*plr*/ 1, /*inv*/ 19);
    }
    ck_eq((uint32_t)fx.progress[3 * PROGRESS_ROW_COUNT + 19].acquired, 1u,
          "PLANET loop: progress[3][19].acquired set for the one enabled other player");
    ck_eq((uint32_t)fx.progress[5 * PROGRESS_ROW_COUNT + 19].acquired, 0u,
          "PLANET loop: progress[5][19] untouched -- player 5 is disabled");
    ck_eq((uint32_t)g_finalize.size(), 0u, "PLANET loop: finalize_acquire is NEVER called, even per-player");
    // propagate_unlocks fires once for player 3 (the loop) and once more for plr(1) (the shared tail).
    ck_eq((uint32_t)g_propagate.size(), 2u, "PLANET loop: propagate_unlocks(3) + tail propagate_unlocks(1)");
    ck_eq((uint32_t)g_propagate[0].player, 3u, "PLANET loop: the loop's own propagate_unlocks(3) comes first");
    ck_eq((uint32_t)g_propagate[1].player, 1u, "PLANET loop: the tail propagate_unlocks(plr=1) comes last");

    // ==================================================================================================
    // type SYSTEM(5)
    // ==================================================================================================

    // SYSTEM, plr == player_side: per enabled other player, propagate_unlocks(i) THEN
    // finalize_acquire(i,inv) (both, unlike PLANET's loop); after the loop, an UNCONDITIONAL
    // finalize_acquire(plr,inv); then the shared tail's propagate_unlocks(plr).
    fx.reset();
    clear_calls();
    fx.cfg_inventions[21].type  = INV_TYPE_SYSTEM;
    fx.player_side              = 2;
    fx.profiles[2].status_flags = STRAT_PLAYER_STATUS_ENABLED; // plr itself -- excluded from the loop
    fx.profiles[4].status_flags = STRAT_PLAYER_STATUS_ENABLED; // the one other enabled player
    {
        sim_store own = fx.store();
        detail::game_update_progress(fx.view(), own, g_calls, /*plr*/ 2, /*inv*/ 21);
    }
    ck_eq((uint32_t)fx.progress[4 * PROGRESS_ROW_COUNT + 21].acquired, 1u,
          "SYSTEM loop: progress[4][21].acquired set for the one enabled other player");
    ck_eq((uint32_t)g_propagate.size(), 2u, "SYSTEM: propagate_unlocks(4) [loop] + propagate_unlocks(2) [tail]");
    ck_eq((uint32_t)g_finalize.size(), 2u,
          "SYSTEM: finalize_acquire(4,21) [loop] + finalize_acquire(2,21) [unconditional, after the loop]");
    ck_eq((uint32_t)g_finalize[0].player, 4u, "SYSTEM: the loop's own finalize_acquire targets player 4");
    ck_eq((uint32_t)g_finalize[1].player, 2u, "SYSTEM: the unconditional finalize_acquire targets plr(2)");
    // exact ordering: propagate(4) < finalize(4) < finalize(2, unconditional) < propagate(2, tail).
    ck(g_propagate[0].seq < g_finalize[0].seq, "SYSTEM order: loop's propagate_unlocks(4) before finalize_acquire(4)");
    ck(g_finalize[0].seq < g_finalize[1].seq,
       "SYSTEM order: loop's finalize_acquire(4) before the unconditional finalize_acquire(2)");
    ck(g_finalize[1].seq < g_propagate[1].seq,
       "SYSTEM order: the unconditional finalize_acquire(2) before the tail's propagate_unlocks(2)");

    // SYSTEM, plr != player_side: the loop does not run at all, but the trailing
    // finalize_acquire(plr,inv) is UNCONDITIONAL -- it still fires, followed by the shared tail.
    fx.reset();
    clear_calls();
    fx.cfg_inventions[22].type  = INV_TYPE_SYSTEM;
    fx.player_side              = 9;                           // != plr(2) below
    fx.profiles[4].status_flags = STRAT_PLAYER_STATUS_ENABLED; // would be a loop candidate if the gate failed
    {
        sim_store own = fx.store();
        detail::game_update_progress(fx.view(), own, g_calls, /*plr*/ 2, /*inv*/ 22);
    }
    ck_eq((uint32_t)fx.progress[4 * PROGRESS_ROW_COUNT + 22].acquired, 0u,
          "SYSTEM plr!=player_side: the loop did NOT run (player 4's progress untouched)");
    ck_eq((uint32_t)g_finalize.size(), 1u,
          "SYSTEM plr!=player_side: the loop's finalize_acquire is absent, but the unconditional one fires");
    ck_eq((uint32_t)g_finalize[0].player, 2u, "SYSTEM plr!=player_side: the unconditional call targets plr(2)");
    ck_eq((uint32_t)g_propagate.size(), 1u, "SYSTEM plr!=player_side: only the shared tail's propagate_unlocks");
    ck(g_finalize[0].seq < g_propagate[0].seq,
       "SYSTEM plr!=player_side order: the unconditional finalize_acquire precedes the tail");
}

} // namespace mh::sim::test
