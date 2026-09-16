#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// This file's own literal operand -- see the DECLARED NEED note above for why it is file-local rather
// than a real enum, and why it does not collide with sim_order_dispatch_bldg.cpp's sibling
// BLDG_STATE_MINE_SCAN_DEPOSITS (0x72, the state that transitions INTO this function).
inline constexpr uint16_t BLDG_STATE_MINE_CHECK_DEPOSITS = 0x73; // the strategic-sim notes: mine "check"

// The one external callee this closure reaches, indirected for offline testability -- same reason as
// every other sim/ TU: a direct mh::call:: inside a detail:: body reaches into the live game image,
// which makes the body untestable by net_selftest.exe simtest.
struct bldg_state_mine_scan_calls {
    uint32_t (*mine_scan_deposit_slot)(uint8_t slot_index, uint32_t player, int32_t building_index);
};

const bldg_state_mine_scan_calls &live_bldg_state_mine_scan_calls();

namespace detail {

// llm_strat_bldg_state_mine_scan_deposits @0x00474384. See the header derivation above.
void bldg_state_mine_scan_deposits(const sim_view &v, sim_store &own, const bldg_state_mine_scan_calls &c);

} // namespace detail

// Live wrapper: the logic applied to state() and live_bldg_state_mine_scan_calls(). Matches the
// original's committed void(void) prototype exactly.
void bldg_state_mine_scan_deposits();

namespace detail {
} // namespace detail

} // namespace mh::sim
