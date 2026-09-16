//
// sim_game_insert_item_in_player_array_selftest.cpp -- `simtest` offline oracle for
// game_InsertItemInPlayerArray (sim/sim_game_insert_item_in_player_array.{h,cpp}, RI-SIM / SIM1F).
// The function scans a caller-supplied int32 array for `item`, then either OVERWRITES the matched
// slot with `new_item` (new_item != 0) or REMOVES it (new_item == 0: shift the tail down one and
// zero-fill the vacated last slot), and fires game_SetEvent(BUILD_PROJECTS_REFRESH=7) iff the
// caller is the local viewing player. No match -> the array is left unchanged and no event fires.
//
// EXPECTED VALUES HAND-TRACED FROM THE .cpp's own asm-derived logic
// (tmp/decomp/game_InsertItemInPlayerArray_00414106.asm), register by register -- the do-while scan
// order (compare BEFORE advance/decrement, so `remaining` ends nonzero iff a match was found), the
// remaining==0 no-match early-out, the new_item==0 shift-and-zero path, and the MOVZX (zero-extend,
// not sign-extend) PlayerSide widening in the notify gate.
//
#include "sim/sim_game_insert_item_in_player_array.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// The one outward callee (game_SetEvent), recorded.
std::vector<uint32_t> g_events;
uint32_t              stub_set_event(uint32_t type) {
    g_events.push_back(type);
    return 0;
}
const insert_item_in_player_array_calls g_calls = {stub_set_event};

// Run the reimplementation against a local array and a fixture whose player_side is set.
void run_insert(sim_fixture &fx, int16_t player_side, std::vector<int32_t> &arr, uint32_t player,
                int32_t item, int32_t new_item) {
    fx.player_side = player_side;
    g_events.clear();
    detail::insert_item_in_player_array(fx.view(), g_calls, arr.data(), (int32_t)arr.size(), player,
                                        item, new_item);
}

bool eq_arr(const std::vector<int32_t> &got, const std::vector<int32_t> &want) {
    if (got.size() != want.size()) return false;
    for (size_t i = 0; i < got.size(); ++i)
        if (got[i] != want[i]) return false;
    return true;
}

} // namespace

void run_insert_item_in_player_array_tests() {
    sim_fixture fx;

    // ---- C1: match found, new_item != 0 -> overwrite matched slot only, no shift; event fires -----
    {
        fx.reset();
        std::vector<int32_t> arr = {10, 20, 30, 40};
        run_insert(fx, /*player_side*/ 3, arr, /*player*/ 3, /*item*/ 30, /*new_item*/ 99);
        ck(eq_arr(arr, {10, 20, 99, 40}), "C1: matched slot overwritten with new_item, no shift");
        ck(g_events.size() == 1 && g_events[0] == 7u,
           "C1: player == PlayerSide -> game_SetEvent(BUILD_PROJECTS_REFRESH=7) once");
    }

    // ---- C2: removal (new_item == 0) -> shift tail down one, zero-fill last slot ------------------
    {
        fx.reset();
        std::vector<int32_t> arr = {10, 20, 30, 40};
        run_insert(fx, /*player_side*/ 5, arr, /*player*/ 5, /*item*/ 20, /*new_item*/ 0);
        ck(eq_arr(arr, {10, 30, 40, 0}), "C2: removal shifts {30,40} down and zero-fills last slot");
        ck(g_events.size() == 1 && g_events[0] == 7u, "C2: event fires on removal too (a change was made)");
    }

    // ---- C3: no match -> array unchanged, no event, no OOB write ----------------------------------
    {
        fx.reset();
        std::vector<int32_t> arr = {10, 20, 30, 40};
        run_insert(fx, /*player_side*/ 3, arr, /*player*/ 3, /*item*/ 99, /*new_item*/ 5);
        ck(eq_arr(arr, {10, 20, 30, 40}), "C3: no match -> array unchanged");
        ck(g_events.empty(), "C3: no match -> no event");
    }

    // ---- C4: match found but player != PlayerSide -> array changes, but NO event ------------------
    {
        fx.reset();
        std::vector<int32_t> arr = {10, 20, 30};
        run_insert(fx, /*player_side*/ 5, arr, /*player*/ 1, /*item*/ 20, /*new_item*/ 7);
        ck(eq_arr(arr, {10, 7, 30}), "C4: matched slot overwritten");
        ck(g_events.empty(), "C4: player(1) != PlayerSide(5) -> no event");
    }

    // ---- C5: match at FIRST element, removal -> whole array shifts down, last zeroed --------------
    {
        fx.reset();
        std::vector<int32_t> arr = {50, 20};
        run_insert(fx, /*player_side*/ 2, arr, /*player*/ 2, /*item*/ 50, /*new_item*/ 0);
        ck(eq_arr(arr, {20, 0}), "C5: first-element removal shifts {20} down, zero-fills last");
        ck(g_events.size() == 1 && g_events[0] == 7u, "C5: event fires");
    }
}

} // namespace mh::sim::test
