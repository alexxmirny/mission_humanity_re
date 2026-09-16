//
// sim_unit_state_enter_wait_selftest.cpp -- `simtest` oracle for llm_strat_unit_state_enter_wait
// @0x00480218 (sim/sim_unit_state_enter.h/.cpp, RI-SIM / SIM1-G3).
//
// ARMABLE (arm_ready:true, this slice) but NOT COVERED: FIVE consecutive all-AI soaks (15000 steps
// @1000%, four before this manifest fix plus one more after it, across two different scenarios) all
// reached 0 calls for this site -- no unit in those runs happened to be genuinely queued at a storage
// door waiting to enter. This offline oracle is this slice's evidence in the meantime; a future rig
// run with a targeted save (a storage building under contention) can still add T1 coverage on top.
//
// EXPECTED BEHAVIOUR HAND-DERIVED FROM tmp/decomp_sim/llm_strat_unit_state_enter_wait_00480218.asm --
// re-read in full this slice (not just the existing .cpp, which the re-read confirmed faithful):
//   0x0048023c-0x00480253: can_enter = storage_can_enter(cur_player, cur_index, home_storage_slot).
//   0x00480255 JZ 0x00480282: can_enter == 0 branches to the "still blocked" path below; can_enter !=
//     0 falls through to 0x00480257-0x0048027d -- door_waiter_count DEC (0xc72898 offset) on
//     storage[cur_player][home_storage_slot], then unit_set_state(0x24 = ENTER_STORAGE_BEGIN), then
//     JMP 0x004803b5 (return) -- none of the abort/wait code below runs on this path.
//   0x004802b1 CMP building.building_id,0 / JZ 0x004802f7->0x00480353->0x0048037c: building_id==0 ->
//     ABORT (unit_set_state(1=STOP_TO_DEFAULT), door_waiter_count DEC, unit_queue_advance).
//   0x004802ea-0x004802f5 FLDZ/FCOMP energy/FNSTSW/SAHF/JC 0x004802f9: JC taken means 0.0 < energy
//     (energy > 0.0), so ONLY energy > 0.0 continues past this gate; energy <= 0.0 (including exactly
//     0.0, which sets C3 not C0, so JC is NOT taken) falls through to the same ABORT via 0x004802f7.
//   0x0048030f CMP door_mutex_unit,0 / JNZ 0x00480351: door_mutex_unit != 0 skips straight to the
//     "still waiting" path (0x00480355) without even reading online_state.
//   0x00480347 CMP online_state,2 / JZ 0x00480353->0x0048037c: only reached when door_mutex_unit==0;
//     online_state==2 -> ABORT; online_state!=2 falls through to 0x00480351->0x00480355 (still
//     waiting).
//   0x00480355-0x0048037a ("still waiting"): tick_budget (both dwords) zeroed, then
//     cur_unit->activity_clock (+0x8) += the boot-constant double at 0x00501400
//     (_G_LLM_STRAT_UNIT_ENTER_WAIT_ACTIVITY_BACKOFF, real value 0.05) -- no state change, no queue
//     advance, door_waiter_count untouched.
//   0x0048037c (ABORT, shared tail): unit_set_state(1), door_waiter_count DEC (SAME offset as the
//     can_enter!=0 path), unit_queue_advance(cur_player, cur_index).
//
#include "sim/sim_unit_state_enter.h"

#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

std::vector<uint16_t> g_set_state_calls;
void                  rec_unit_set_state(uint16_t new_state) { g_set_state_calls.push_back(new_state); }

struct queue_advance_call {
    uint32_t player;
    uint32_t unit_index;
};
std::vector<queue_advance_call> g_queue_advance_calls;
void                            rec_unit_queue_advance(uint32_t player, uint32_t unit_index) {
    g_queue_advance_calls.push_back({player, unit_index});
}

int32_t g_can_enter_result = 0;
struct can_enter_call {
    uint16_t player;
    uint32_t unit_index;
    uint32_t storage_slot;
};
std::vector<can_enter_call> g_can_enter_calls;
int32_t                     rec_storage_can_enter(uint16_t player, uint32_t unit_index, uint32_t storage_slot) {
    g_can_enter_calls.push_back({player, unit_index, storage_slot});
    return g_can_enter_result;
}

const unit_state_enter_calls g_calls = {
    &rec_unit_set_state,
    &rec_storage_can_enter,
    nullptr, // storage_board_unit -- unreachable from this function
    &rec_unit_queue_advance,
    nullptr, // dir_from_to
    nullptr, // unit_walk_step_allowed
    nullptr, // unit_soldiers_set_heading
    nullptr, // dir_step_factor
    nullptr, // target_release_ref
    nullptr, // path_free_slot
};

constexpr uint16_t PLAYER       = 1;
constexpr int32_t  UNIT_INDEX   = 5;
constexpr int32_t  STORAGE_SLOT = 9;
constexpr int32_t  B_INDEX      = 12;

void reset_recorders() {
    g_set_state_calls.clear();
    g_queue_advance_calls.clear();
    g_can_enter_calls.clear();
    g_can_enter_result = 0;
}

} // namespace

void run_unit_state_enter_wait_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- can_enter != 0 (door claimable NOW): door_waiter_count decremented by exactly 1,
    // unit_set_state(ENTER_STORAGE_BEGIN=0x24) called exactly once, nothing else touched (no
    // queue_advance, no tick_budget/activity_clock change) -- 0x00480255 JZ NOT taken.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        g_can_enter_result = 1;

        unit &u             = fx.u(PLAYER, UNIT_INDEX);
        u.home_storage_slot = static_cast<uint8_t>(STORAGE_SLOT);
        u.activity_clock    = 42.0;

        unit_storage &st     = fx.storage[PLAYER * STORAGE_PER_PLAYER + STORAGE_SLOT];
        st.door_waiter_count = 3;

        double &tb = fx.tick_budget;
        tb         = 7.0;

        fx.cur_unit_ptr    = &u;
        fx.view_cur_player = PLAYER;
        fx.view_cur_index  = UNIT_INDEX;

        sim_store own = fx.store();
        detail::unit_state_enter_wait(fx.view(), own, g_calls);

        ck(g_can_enter_calls.size() == 1 && g_can_enter_calls[0].player == PLAYER &&
               g_can_enter_calls[0].unit_index == static_cast<uint32_t>(UNIT_INDEX) &&
               g_can_enter_calls[0].storage_slot == static_cast<uint32_t>(STORAGE_SLOT),
           "T1: storage_can_enter(player, unit_index, home_storage_slot) called once, 0x0048024e");
        ck_eq((uint32_t)st.door_waiter_count, 2u,
              "T1: door_waiter_count decremented by exactly 1 (3 -> 2), 0x0048026d DEC");
        ck(g_set_state_calls.size() == 1 && g_set_state_calls[0] == 0x24,
           "T1: unit_set_state(ENTER_STORAGE_BEGIN=0x24) called exactly once, 0x00480273-0x00480278");
        ck(g_queue_advance_calls.empty(), "T1: unit_queue_advance NOT called on the can-enter-now path");
        ck_eq_d(u.activity_clock, 42.0, "T1: activity_clock untouched on the can-enter-now path");
        ck_eq_d(tb, 7.0, "T1: tick_budget untouched on the can-enter-now path");
    }

    // =================================================================================================
    // T2 -- can_enter == 0, building_id == 0 (the "no such building" gate, 0x004802b1 JZ taken) ->
    // ABORT tail: unit_set_state(STOP_TO_DEFAULT=1), door_waiter_count decremented by 1,
    // unit_queue_advance(player, index) called once.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        g_can_enter_result = 0;

        unit &u             = fx.u(PLAYER, UNIT_INDEX);
        u.home_storage_slot = static_cast<uint8_t>(STORAGE_SLOT);
        u.activity_clock    = 1.0;

        unit_storage &st     = fx.storage[PLAYER * STORAGE_PER_PLAYER + STORAGE_SLOT];
        st.b_index           = B_INDEX;
        st.door_waiter_count = 5;

        building &b   = fx.b(PLAYER, B_INDEX);
        b.building_id = 0; // the gate this case targets

        fx.cur_unit_ptr    = &u;
        fx.view_cur_player = PLAYER;
        fx.view_cur_index  = UNIT_INDEX;

        sim_store own = fx.store();
        detail::unit_state_enter_wait(fx.view(), own, g_calls);

        ck(g_set_state_calls.size() == 1 && g_set_state_calls[0] == 0x01,
           "T2: unit_set_state(STOP_TO_DEFAULT=1) called exactly once, 0x0048037c-0x00480381");
        ck_eq((uint32_t)st.door_waiter_count, 4u, "T2: door_waiter_count decremented by 1 (5 -> 4)");
        ck(g_queue_advance_calls.size() == 1 && g_queue_advance_calls[0].player == PLAYER &&
               g_queue_advance_calls[0].unit_index == static_cast<uint32_t>(UNIT_INDEX),
           "T2: unit_queue_advance(player, index) called once, 0x004803a2-0x004803b0");
        ck_eq_d(u.activity_clock, 1.0, "T2: activity_clock untouched on the abort path");
    }

    // =================================================================================================
    // T3a -- can_enter == 0, building_id != 0, energy == 0.0 EXACTLY (the boundary the FCOMP/JC test
    // resolves to "not greater than", 0x004802f5 JC not taken) -> same ABORT tail as T2.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        g_can_enter_result = 0;

        unit &u             = fx.u(PLAYER, UNIT_INDEX);
        u.home_storage_slot = static_cast<uint8_t>(STORAGE_SLOT);

        unit_storage &st     = fx.storage[PLAYER * STORAGE_PER_PLAYER + STORAGE_SLOT];
        st.b_index           = B_INDEX;
        st.door_waiter_count = 2;

        building &b   = fx.b(PLAYER, B_INDEX);
        b.building_id = B_INDEX;
        b.energy      = 0.0; // boundary: NOT > 0.0

        fx.cur_unit_ptr    = &u;
        fx.view_cur_player = PLAYER;
        fx.view_cur_index  = UNIT_INDEX;

        sim_store own = fx.store();
        detail::unit_state_enter_wait(fx.view(), own, g_calls);

        ck(g_set_state_calls.size() == 1 && g_set_state_calls[0] == 0x01,
           "T3a: energy==0.0 exactly still ABORTs (strict > required), 0x004802ea-0x004802f5");
        ck_eq((uint32_t)st.door_waiter_count, 1u, "T3a: door_waiter_count decremented by 1 (2 -> 1)");
        ck(g_queue_advance_calls.size() == 1, "T3a: unit_queue_advance called once");
    }

    // =================================================================================================
    // T3b -- can_enter == 0, building_id != 0, energy < 0.0 -> same ABORT tail (sanity: not just the
    // exact-zero boundary).
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        g_can_enter_result = 0;

        unit &u             = fx.u(PLAYER, UNIT_INDEX);
        u.home_storage_slot = static_cast<uint8_t>(STORAGE_SLOT);

        unit_storage &st = fx.storage[PLAYER * STORAGE_PER_PLAYER + STORAGE_SLOT];
        st.b_index       = B_INDEX;

        building &b   = fx.b(PLAYER, B_INDEX);
        b.building_id = B_INDEX;
        b.energy      = -3.5;

        fx.cur_unit_ptr    = &u;
        fx.view_cur_player = PLAYER;
        fx.view_cur_index  = UNIT_INDEX;

        sim_store own = fx.store();
        detail::unit_state_enter_wait(fx.view(), own, g_calls);

        ck(g_set_state_calls.size() == 1 && g_set_state_calls[0] == 0x01,
           "T3b: energy < 0.0 ABORTs");
    }

    // =================================================================================================
    // T4 -- can_enter == 0, building_id != 0, energy > 0.0, door_mutex_unit == 0 AND online_state == 2
    // (0x0048030f/0x00480347, both conditions true) -> ABORT.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        g_can_enter_result = 0;

        unit &u             = fx.u(PLAYER, UNIT_INDEX);
        u.home_storage_slot = static_cast<uint8_t>(STORAGE_SLOT);

        unit_storage &st     = fx.storage[PLAYER * STORAGE_PER_PLAYER + STORAGE_SLOT];
        st.b_index           = B_INDEX;
        st.door_mutex_unit   = 0;
        st.door_waiter_count = 9;

        building &b    = fx.b(PLAYER, B_INDEX);
        b.building_id  = B_INDEX;
        b.energy       = 10.0;
        b.online_state = 2;

        fx.cur_unit_ptr    = &u;
        fx.view_cur_player = PLAYER;
        fx.view_cur_index  = UNIT_INDEX;

        sim_store own = fx.store();
        detail::unit_state_enter_wait(fx.view(), own, g_calls);

        ck(g_set_state_calls.size() == 1 && g_set_state_calls[0] == 0x01,
           "T4: door_mutex_unit==0 && online_state==2 ABORTs, 0x0048030f/0x00480347");
        ck_eq((uint32_t)st.door_waiter_count, 8u, "T4: door_waiter_count decremented by 1 (9 -> 8)");
    }

    // =================================================================================================
    // T5a -- still legitimately waiting: door_mutex_unit != 0 (skips the online_state read entirely,
    // 0x00480316 JNZ taken) -- regardless of online_state's value. tick_budget zeroed, activity_clock
    // bumped by the boot-constant backoff (0.05); NO state change, NO queue_advance, door_waiter_count
    // untouched.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        g_can_enter_result = 0;

        unit &u             = fx.u(PLAYER, UNIT_INDEX);
        u.home_storage_slot = static_cast<uint8_t>(STORAGE_SLOT);
        u.activity_clock    = 2.0;

        unit_storage &st     = fx.storage[PLAYER * STORAGE_PER_PLAYER + STORAGE_SLOT];
        st.b_index           = B_INDEX;
        st.door_mutex_unit   = 77; // != 0
        st.door_waiter_count = 6;

        building &b    = fx.b(PLAYER, B_INDEX);
        b.building_id  = B_INDEX;
        b.energy       = 10.0;
        b.online_state = 2; // must NOT matter -- the door_mutex_unit!=0 branch never reads it

        double &tb = fx.tick_budget;
        tb         = 3.0;

        fx.cur_unit_ptr    = &u;
        fx.view_cur_player = PLAYER;
        fx.view_cur_index  = UNIT_INDEX;

        sim_store own = fx.store();
        detail::unit_state_enter_wait(fx.view(), own, g_calls);

        ck(g_set_state_calls.empty(), "T5a: still-waiting path calls unit_set_state ZERO times");
        ck(g_queue_advance_calls.empty(), "T5a: still-waiting path calls unit_queue_advance ZERO times");
        ck_eq((uint32_t)st.door_waiter_count, 6u, "T5a: door_waiter_count untouched while still waiting");
        ck_eq_d(tb, 0.0, "T5a: tick_budget zeroed, 0x00480355-0x0048035f");
        ck_eq_d(u.activity_clock, 2.0 + 0.05,
                "T5a: activity_clock += backoff (0x00501400, 0.05), 0x0048036e-0x00480377");
    }

    // =================================================================================================
    // T5b -- still waiting via the OTHER route: door_mutex_unit == 0 AND online_state != 2 (falls
    // through both gates without aborting). Same still-waiting effects as T5a.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        g_can_enter_result = 0;

        unit &u             = fx.u(PLAYER, UNIT_INDEX);
        u.home_storage_slot = static_cast<uint8_t>(STORAGE_SLOT);
        u.activity_clock    = 0.0;

        unit_storage &st     = fx.storage[PLAYER * STORAGE_PER_PLAYER + STORAGE_SLOT];
        st.b_index           = B_INDEX;
        st.door_mutex_unit   = 0;
        st.door_waiter_count = 1;

        building &b    = fx.b(PLAYER, B_INDEX);
        b.building_id  = B_INDEX;
        b.energy       = 0.01; // just above the boundary
        b.online_state = 1;    // != 2

        double &tb = fx.tick_budget;
        tb         = 9.0;

        fx.cur_unit_ptr    = &u;
        fx.view_cur_player = PLAYER;
        fx.view_cur_index  = UNIT_INDEX;

        sim_store own = fx.store();
        detail::unit_state_enter_wait(fx.view(), own, g_calls);

        ck(g_set_state_calls.empty(), "T5b: door_mutex_unit==0 && online_state!=2 also still-waits");
        ck(g_queue_advance_calls.empty(), "T5b: unit_queue_advance ZERO times");
        ck_eq((uint32_t)st.door_waiter_count, 1u, "T5b: door_waiter_count untouched");
        ck_eq_d(tb, 0.0, "T5b: tick_budget zeroed");
        ck_eq_d(u.activity_clock, 0.05, "T5b: activity_clock += backoff (0.0 -> 0.05)");
    }
}

} // namespace mh::sim::test
