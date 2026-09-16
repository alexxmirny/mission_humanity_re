//
// sim_bldg_state_default_reset_selftest.cpp -- `simtest` cases for llm_strat_bldg_state_default_reset
// (sim/sim_bldg_state_reset_idle.h/.cpp, detail::bldg_state_default_reset).
//
// EXPECTED BEHAVIOUR HAND-DERIVED FROM THE DISASSEMBLY
// (tmp/decomp_sim/llm_strat_bldg_state_default_reset_004711c3.asm) -- NOT read off the .cpp body alone:
//
//   0x004711db-0x004711e0: MOV EAX,[_G_LLM_STRAT_CUR_BUILDING]; MOV word ptr [EAX+0xd],0x1
//                           -- cur_building->state (offset 0xd) = 1 (BLDG_STATE_IDLE_ACTIVATE).
//   0x004711e6-0x004711f4: MOVZX EDX,[_G_LLM_STRAT_CUR_INDEX]; MOVZX EAX,[_G_LLM_STRAT_CUR_PLAYER];
//                           CALL llm_strat_bldg_notify_ui
//                           -- llm_strat_bldg_notify_ui(cur_player, cur_index) -- EAX=cur_player is the
//                           FIRST arg, EDX=cur_index is the SECOND, matching the committed
//                           (uint16_t player, uint32_t b_index) prototype.
//
// The function is TWO EFFECTS, NO BRANCHES, NO OTHER READS -- so this file's job is narrow but exact:
// pin the state write to exactly 1 (not "some nonzero value", not "left alone"), pin the notify_ui
// call to fire EXACTLY ONCE with (player, index) in that order and not swapped, and prove nothing else
// in the fixture (the building's other fields, a DIFFERENT building, tick_budget) moves.
//
#include "sim/sim_bldg_state_reset_idle.h"

#include <cstdint>
#include <cstring>
#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- the one callee (bldg_state_reset_idle_calls has exactly one member) -----------------------
struct NotifyCall {
    uint16_t player;
    uint32_t index;
};
std::vector<NotifyCall> g_notify_calls;
void                    rec_bldg_notify_ui(uint16_t player, uint32_t index) {
    g_notify_calls.push_back({player, index});
}

const bldg_state_reset_idle_calls g_calls = {
    &rec_bldg_notify_ui,
};

void reset_observations() { g_notify_calls.clear(); }

// ---- fixture seeding -----------------------------------------------------------------------------
struct Seed {
    uint16_t player = 0;
    int32_t  index  = 1;

    // Sentinel starting state, DISTINCT from BLDG_STATE_IDLE_ACTIVATE(1) -- a translation that left
    // state alone (or wrote some other constant) must be caught, not just one that wrote 0.
    uint16_t start_state = 0xBEEF;

    // Fields the function must NOT touch -- seeded with distinct, non-zero, non-default sentinels so
    // an accidental write (or a write to the wrong building) is observable.
    int16_t  start_online_state   = -777;
    double   start_cycle_progress = 42.5;
    uint16_t start_building_id    = 55;
    uint8_t  start_x = 21, start_y = 34;
    uint8_t  anim_fill = 0xAA; // memset pattern for the 48-byte anim block

    double start_tick_budget = 12345.5; // shared scratch this function must not zero/touch

    // A SECOND building, at a DIFFERENT (player, index), that must be completely untouched -- catches
    // a translation that wrote through the wrong pointer (e.g. indexed instead of using cur_building).
    uint16_t other_player      = 5;
    int32_t  other_index       = 42;
    uint16_t other_start_state = 0x2222;
};

void seed_and_run(sim_fixture &fx, const Seed &s) {
    fx.reset();

    building &b      = fx.b(s.player, s.index);
    b.state          = s.start_state;
    b.online_state   = s.start_online_state;
    b.cycle_progress = s.start_cycle_progress;
    b.building_id    = s.start_building_id;
    b.x              = s.start_x;
    b.y              = s.start_y;
    std::memset(b.anim, s.anim_fill, sizeof(b.anim));

    building &other      = fx.b(s.other_player, s.other_index);
    other.state          = s.other_start_state;
    other.online_state   = s.start_online_state;
    other.cycle_progress = s.start_cycle_progress;

    fx.cur_building_ptr = &b;
    fx.view_cur_player  = s.player;
    fx.view_cur_index   = (uint16_t)s.index;
    fx.tick_budget      = s.start_tick_budget;

    reset_observations();

    sim_store own = fx.store();
    detail::bldg_state_default_reset(fx.view(), own, g_calls);
}

} // namespace

void run_bldg_state_default_reset_tests() {
    sim_fixture fx;

    // =================================================================================================
    // R1 -- the spine: a non-trivial, non-symmetric (player, index) pair so a swapped-argument bug in
    // the notify_ui call would be caught (player != index, and neither is 0).
    // =================================================================================================
    {
        Seed s;
        s.player      = 3;
        s.index       = 17;
        s.start_state = 0xBEEF; // distinct from IDLE_ACTIVATE(1)
        seed_and_run(fx, s);

        ck_eq((uint32_t)fx.b(s.player, s.index).state, (uint32_t)BLDG_STATE_IDLE_ACTIVATE,
              "R1 (0x004711db-0x004711e0): cur_building->state = IDLE_ACTIVATE(1), exactly");

        ck(g_notify_calls.size() == 1, "R1 (0x004711e6-0x004711f4): bldg_notify_ui called EXACTLY ONCE");
        if (g_notify_calls.size() == 1) {
            ck(g_notify_calls[0].player == s.player,
               "R1 (0x004711ed-0x004711f4, EAX): bldg_notify_ui's 1st arg = cur_player (3), not swapped "
               "with index");
            ck(g_notify_calls[0].index == (uint32_t)s.index,
               "R1 (0x004711e6-0x004711ed, EDX): bldg_notify_ui's 2nd arg = cur_index (17), not swapped "
               "with player");
        }

        // Nothing else on cur_building moves.
        ck_eq((uint32_t)(uint16_t)fx.b(s.player, s.index).online_state, (uint32_t)(uint16_t)s.start_online_state,
              "R1: cur_building->online_state untouched (function writes only state + calls notify_ui)");
        ck_eq_d(fx.b(s.player, s.index).cycle_progress, s.start_cycle_progress,
                "R1: cur_building->cycle_progress untouched");
        ck_eq((uint32_t)fx.b(s.player, s.index).building_id, (uint32_t)s.start_building_id,
              "R1: cur_building->building_id untouched");
        ck_eq((uint32_t)fx.b(s.player, s.index).x, (uint32_t)s.start_x, "R1: cur_building->x untouched");
        ck_eq((uint32_t)fx.b(s.player, s.index).y, (uint32_t)s.start_y, "R1: cur_building->y untouched");
        {
            bool anim_ok = true;
            for (uint8_t byte : fx.b(s.player, s.index).anim)
                if (byte != s.anim_fill) anim_ok = false;
            ck(anim_ok, "R1: cur_building->anim[] block untouched");
        }

        // Nothing outside cur_building moves either.
        ck_eq_d(fx.tick_budget, s.start_tick_budget,
                "R1: tick_budget untouched (default_reset does not zero it, unlike idle_noop/idle_activate)");
        ck_eq((uint32_t)fx.b(s.other_player, s.other_index).state, (uint32_t)s.other_start_state,
              "R1: a DIFFERENT building's state is untouched -- write goes through cur_building, not an index");
        ck_eq((uint32_t)(uint16_t)fx.b(s.other_player, s.other_index).online_state,
              (uint32_t)(uint16_t)s.start_online_state, "R1: the other building's online_state untouched");
        ck_eq_d(fx.b(s.other_player, s.other_index).cycle_progress, s.start_cycle_progress,
                "R1: the other building's cycle_progress untouched");
    }

    // =================================================================================================
    // R2 -- a DIFFERENT (player, index) pair and a DIFFERENT starting state (0, not 0xBEEF), so this
    // case alone would catch a translation that hardcoded R1's player/index into the notify_ui call,
    // or that only writes state=1 when it started at a specific sentinel.
    // =================================================================================================
    {
        Seed s;
        s.player      = 6;
        s.index       = 88;
        s.start_state = 0; // distinct sentinel from R1's 0xBEEF, still != IDLE_ACTIVATE(1)
        seed_and_run(fx, s);

        ck_eq((uint32_t)fx.b(s.player, s.index).state, (uint32_t)BLDG_STATE_IDLE_ACTIVATE,
              "R2 (0x004711db-0x004711e0): state = IDLE_ACTIVATE(1) again, from a different starting value");

        ck(g_notify_calls.size() == 1, "R2: bldg_notify_ui called exactly once");
        if (g_notify_calls.size() == 1) {
            ck(g_notify_calls[0].player == s.player,
               "R2: bldg_notify_ui's 1st arg = cur_player (6), matching the NEW pair, not R1's (3)");
            ck(g_notify_calls[0].index == (uint32_t)s.index,
               "R2: bldg_notify_ui's 2nd arg = cur_index (88), matching the NEW pair, not R1's (17)");
        }
    }
}

} // namespace mh::sim::test
