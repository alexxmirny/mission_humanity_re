#include "sim/sim_unit_state_enter.h"

#include "sim/sim_order_enqueue.h" // UNIT_STATE_STOP_TO_DEFAULT

#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

std::vector<uint16_t> g_set_state_calls;
void                  rec_unit_set_state(uint16_t new_state) { g_set_state_calls.push_back(new_state); }

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

struct board_unit_call {
    uint16_t player;
    uint32_t unit_idx;
    uint32_t storage_idx;
};
std::vector<board_unit_call> g_board_unit_calls;
void                         rec_storage_board_unit(uint16_t player, uint32_t unit_idx, uint32_t storage_idx) {
    g_board_unit_calls.push_back({player, unit_idx, storage_idx});
}

const unit_state_enter_calls g_calls = {
    &rec_unit_set_state,
    &rec_storage_can_enter,
    &rec_storage_board_unit,
    nullptr, // unit_queue_advance -- unreachable from this function
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
    g_can_enter_calls.clear();
    g_board_unit_calls.clear();
    g_can_enter_result = 0;
}

unit &seed(sim_fixture &fx, int32_t x, int32_t y, int32_t exit_x, int32_t exit_y, uint8_t built_flags) {
    fx.reset();
    reset_recorders();
    unit &u             = fx.u(PLAYER, UNIT_INDEX);
    u.home_storage_slot = static_cast<uint8_t>(STORAGE_SLOT);
    u.x                 = static_cast<uint8_t>(x);
    u.y                 = static_cast<uint8_t>(y);
    fx.cur_unit_ptr     = &u;
    fx.view_cur_player  = PLAYER;
    fx.view_cur_index   = UNIT_INDEX;

    unit_storage &st = fx.storage[PLAYER * STORAGE_PER_PLAYER + STORAGE_SLOT];
    st.b_index       = B_INDEX;
    st.exit_tile_x   = exit_x;
    st.exit_tile_y   = exit_y;

    building &b   = fx.b(PLAYER, B_INDEX);
    b.built_flags = built_flags;

    return u;
}

void call(sim_fixture &fx) {
    sim_store own = fx.store();
    detail::unit_state_enter_storage_begin(fx.view(), own, g_calls);
}

} // namespace

void run_unit_state_enter_storage_begin_tests() {
    sim_fixture fx;

    // =================================================================================================
    // 1 -- NOT at exit tile (x mismatch, 0x0047ff9c JNZ taken) -- abort: unit_set_state(1). Per the
    // FINDING above, the .asm does NOT early-return after the abort call -- it falls through into the
    // SAME storage_can_enter/door logic the ready path uses. Assert can_enter (and the door-side
    // effect it controls) fires too, proving this really is fallthrough and not the .cpp's early
    // return.
    // =================================================================================================
    {
        seed(fx, 10, 20, 99, 20, 3); // x mismatch (10 != 99), y matches, built_flags would pass -- isolates the x gate
        g_can_enter_result = 1;
        call(fx);
        ck(g_set_state_calls.size() == 1 && g_set_state_calls[0] == UNIT_STATE_STOP_TO_DEFAULT,
           "1: x-mismatch -- unit_set_state(STOP_TO_DEFAULT=1), 0x00480018-0x00480022");
        ck(g_can_enter_calls.size() == 1,
           "1: storage_can_enter STILL called after the abort -- the .asm has NO early return "
           "(0x00480022-0x00480039 unconditionally follows 0x0048001d's CALL)");
        ck(g_board_unit_calls.size() == 1,
           "1: can_enter!=0 -- storage_board_unit fires even on the aborted-state path (fallthrough "
           "confirmed independently of case 1's can_enter check)");
    }

    // =================================================================================================
    // 2 -- NOT at exit tile (y mismatch, x matches -- 0x0047ffc6 JZ not taken) -- same abort, proving
    // BOTH coordinates gate independently (x alone is not sufficient). can_enter=1 again to isolate
    // the coordinate gate itself (matches case 1's single-set_state shape).
    // =================================================================================================
    {
        seed(fx, 10, 20, 10, 99, 3); // x matches, y mismatch
        g_can_enter_result = 1;
        call(fx);
        ck(g_set_state_calls.size() == 1 && g_set_state_calls[0] == UNIT_STATE_STOP_TO_DEFAULT,
           "2: y-mismatch (x already matched) -- still aborts, 0x0047ffc0-0x0047ffc8");
    }

    // =================================================================================================
    // 3 -- AT exit tile but built_flags != 3 (0x0047fff9 JZ not taken) -- same abort. can_enter=1 to
    // isolate the built_flags gate itself.
    // =================================================================================================
    {
        seed(fx, 10, 20, 10, 20, 2); // at exit tile, built_flags=2 (not 3)
        g_can_enter_result = 1;
        call(fx);
        ck(g_set_state_calls.size() == 1 && g_set_state_calls[0] == UNIT_STATE_STOP_TO_DEFAULT,
           "3: at exit tile but built_flags(2)!=3 -- aborts, 0x0047fff9-0x00480002");
    }

    // =================================================================================================
    // 3b -- THE PRESERVE-BUG ITSELF, made explicit: not-ready (x-mismatch) AND can_enter==0 --
    // unit_set_state is called TWICE (STOP_TO_DEFAULT, then ENTER_WAIT), door_waiter_count still
    // increments, storage_board_unit NOT called. The unit's real end state is ENTER_WAIT, not
    // STOP_TO_DEFAULT -- the abort is transient and gets overwritten by ordinary last-write-wins
    // semantics, exactly as llm_strat_unit_set_state's own single global-state-field write would.
    // =================================================================================================
    {
        seed(fx, 10, 20, 99, 20, 3); // x mismatch -- not ready
        unit_storage &st     = fx.storage[PLAYER * STORAGE_PER_PLAYER + STORAGE_SLOT];
        st.door_waiter_count = 6;
        g_can_enter_result   = 0;
        call(fx);
        ck(g_set_state_calls.size() == 2 && g_set_state_calls[0] == UNIT_STATE_STOP_TO_DEFAULT &&
               g_set_state_calls[1] == 0x25,
           "3b: PRESERVE-BUG -- unit_set_state called TWICE, STOP_TO_DEFAULT then ENTER_WAIT "
           "(0x0048001d then 0x00480099), the second overwriting the first");
        ck_eq((uint32_t)st.door_waiter_count, 7u,
              "3b: door_waiter_count still increments on the not-ready-AND-denied path (0x0048008e)");
        ck(g_board_unit_calls.empty(), "3b: storage_board_unit NOT called (can_enter==0 branch taken)");
    }

    // =================================================================================================
    // 4 -- READY (at exit tile, built_flags==3), can_enter != 0 -- door_mutex_unit claimed, board_unit
    // called, unit_set_state NOT called this time (ready==true skips the abort call entirely),
    // door_waiter_count untouched.
    // =================================================================================================
    {
        unit &u = seed(fx, 10, 20, 10, 20, 3);
        (void)u;
        unit_storage &st     = fx.storage[PLAYER * STORAGE_PER_PLAYER + STORAGE_SLOT];
        st.door_mutex_unit   = 0;
        st.door_waiter_count = 4;
        g_can_enter_result   = 1;
        call(fx);
        ck(g_set_state_calls.empty(), "4: ready path with can_enter!=0 -- unit_set_state NEVER called");
        ck(g_can_enter_calls.size() == 1 && g_can_enter_calls[0].player == PLAYER &&
               g_can_enter_calls[0].unit_index == static_cast<uint32_t>(UNIT_INDEX) &&
               g_can_enter_calls[0].storage_slot == static_cast<uint32_t>(STORAGE_SLOT),
           "4: storage_can_enter(player, index, home_storage_slot), 0x0048002d-0x00480039");
        ck_eq((uint32_t)st.door_mutex_unit, (uint32_t)UNIT_INDEX,
              "4: door_mutex_unit = cur_index, 0x0048003d-0x0048005a");
        ck_eq((uint32_t)st.door_waiter_count, 4u, "4: door_waiter_count untouched on the claim path");
        ck(g_board_unit_calls.size() == 1 && g_board_unit_calls[0].player == PLAYER &&
               g_board_unit_calls[0].unit_idx == static_cast<uint32_t>(UNIT_INDEX) &&
               g_board_unit_calls[0].storage_idx == static_cast<uint32_t>(STORAGE_SLOT),
           "4: storage_board_unit(player, index, slot), 0x00480063-0x00480076");
    }

    // =================================================================================================
    // 5 -- READY, can_enter == 0 -- door_waiter_count INC, unit_set_state(ENTER_WAIT=0x25),
    // storage_board_unit NOT called, door_mutex_unit untouched.
    // =================================================================================================
    {
        seed(fx, 10, 20, 10, 20, 3);
        unit_storage &st     = fx.storage[PLAYER * STORAGE_PER_PLAYER + STORAGE_SLOT];
        st.door_mutex_unit   = 77;
        st.door_waiter_count = 2;
        g_can_enter_result   = 0;
        call(fx);
        ck(g_board_unit_calls.empty(), "5: can_enter==0 -- storage_board_unit NOT called");
        ck_eq((uint32_t)st.door_waiter_count, 3u, "5: door_waiter_count += 1 (2 -> 3), 0x0048008e INC");
        ck_eq((uint32_t)st.door_mutex_unit, 77u, "5: door_mutex_unit untouched on the wait path");
        ck(g_set_state_calls.size() == 1 && g_set_state_calls[0] == 0x25,
           "5: unit_set_state(ENTER_WAIT=0x25), 0x00480094-0x0048009e");
    }
}

} // namespace mh::sim::test
