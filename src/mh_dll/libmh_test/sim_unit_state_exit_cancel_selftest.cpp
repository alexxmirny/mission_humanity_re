//
// sim_unit_state_exit_cancel_selftest.cpp -- `simtest` oracle for
// llm_strat_unit_state_exit_cancel @0x0047f351 (sim/sim_unit_state_exit.h/.cpp,
// RI-SIM / SIM1-G3).
//
// This slice's all-AI soak rig run (migration_sweep.py, 15000 steps @1000% speed) reached
// llm_strat_unit_state_exit_walk_out (16129 calls, 0 divergences -- see the ledger) but reached this
// function 0 times -- a real unit never happened to cancel a pending exit mid-flight in that
// scenario. Per the anti-vacuity rule, 0 calls is NOT coverage, so this offline oracle is this
// function's evidence for this slice instead of a rig arm.
//
// SCOPE: the whole function (0x0047f369-0x0047f397, only 3 real instructions worth of effect) --
// door_waiter_count decremented by exactly 1 (not reset, not incremented) on the CURRENT unit's
// OWN home_storage_slot, and unit_set_state(PARKED=0x1f) called exactly once.
//
// EXPECTED BEHAVIOUR HAND-DERIVED FROM tmp/decomp_sim/llm_strat_unit_state_exit_cancel_0047f351.asm.
//
#include "sim/sim_unit_state_exit.h"

#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

std::vector<uint16_t> g_set_state_calls;
void                  rec_unit_set_state(uint16_t new_state) { g_set_state_calls.push_back(new_state); }

const unit_state_exit_calls g_calls = {
    nullptr, // storage_can_exit -- unreachable from this function
    nullptr, // storage_place_exit_ground
    nullptr, // path_find_free_slot
    nullptr, // storage_exit_air
    &rec_unit_set_state,
    nullptr, // race_alert_text_emit
    nullptr, // storage_type_accepts_unit
    nullptr, // dir_step_factor
    nullptr, // map_fow_UpdateFoWPlus
    nullptr, // ai_group_member_count_adjust
    nullptr, // unit_set_state_order
};

constexpr uint16_t GUARD_PLAYER = 2;
constexpr int32_t  GUARD_INDEX  = 6;
constexpr uint8_t  GUARD_SLOT   = 8;

} // namespace

void run_unit_state_exit_cancel_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- door_waiter_count decremented by exactly 1 (not reset to 0, not decremented twice), on the
    // CURRENT unit's own home_storage_slot; unit_set_state(PARKED=0x1f) called exactly once.
    // =================================================================================================
    {
        fx.reset();

        unit_storage &guard_st     = fx.storage[GUARD_PLAYER * STORAGE_PER_PLAYER + GUARD_SLOT];
        guard_st.door_waiter_count = 77; // must read back unchanged (different player/slot)

        const uint16_t player = 1;
        const int32_t  slot   = 3;

        unit &u             = fx.u(player, 0);
        u.home_storage_slot = static_cast<uint8_t>(slot);

        unit_storage &st     = fx.storage[player * STORAGE_PER_PLAYER + slot];
        st.door_waiter_count = 5;

        fx.cur_unit_ptr    = &u;
        fx.view_cur_player = player;
        fx.view_cur_index  = 0;

        g_set_state_calls.clear();
        sim_store own = fx.store();
        detail::unit_state_exit_cancel(fx.view(), own, g_calls);

        ck_eq((uint32_t)st.door_waiter_count, 4u,
              "T1: door_waiter_count decremented by exactly 1 (5 -> 4), 0x0047f387 DEC");
        ck(g_set_state_calls.size() == 1 && g_set_state_calls[0] == 0x1f,
           "T1: unit_set_state(PARKED=0x1f) called exactly once (0x0047f38d-0x0047f392)");
        ck_eq((uint32_t)guard_st.door_waiter_count, 77u,
              "T1: a DIFFERENT (player, slot)'s door_waiter_count is untouched");
    }

    // =================================================================================================
    // T2 -- a different (player, home_storage_slot) pair re-derives the row correctly (not a fixed
    // slot 0 / player 0).
    // =================================================================================================
    {
        fx.reset();
        const uint16_t player = 4;
        const int32_t  slot   = 11;

        unit &u             = fx.u(player, 9);
        u.home_storage_slot = static_cast<uint8_t>(slot);

        unit_storage &st     = fx.storage[player * STORAGE_PER_PLAYER + slot];
        st.door_waiter_count = 1;

        fx.cur_unit_ptr    = &u;
        fx.view_cur_player = player;
        fx.view_cur_index  = 9;

        g_set_state_calls.clear();
        sim_store own = fx.store();
        detail::unit_state_exit_cancel(fx.view(), own, g_calls);

        ck_eq((uint32_t)st.door_waiter_count, 0u, "T2: door_waiter_count 1 -> 0 at a non-default row");
        ck(g_set_state_calls.size() == 1 && g_set_state_calls[0] == 0x1f,
           "T2: unit_set_state(PARKED) still called exactly once");
    }
}

} // namespace mh::sim::test
