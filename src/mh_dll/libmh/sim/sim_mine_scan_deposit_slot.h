#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The two toroidal wrap-delta primitives this function calls, indirected for offline testability --
// same reason and SAME two callees sim_unit_state_squad_merge.cpp's own `calls` struct already
// documents (mh::call::llm_map_wrap_delta_x/_y).
struct mine_scan_deposit_slot_calls {
    int32_t (*wrap_delta_x)(int32_t pos_a, uint32_t unused_param, int32_t pos_b); // @0x004940a9
    int32_t (*wrap_delta_y)(int32_t x1, int32_t y1, int32_t x2, int32_t y2);      // @0x00494131
};

const mine_scan_deposit_slot_calls &live_mine_scan_deposit_slot_calls();

namespace detail {

// llm_strat_mine_scan_deposit_slot @0x0047ab20. See the header banner above for the full derivation;
// the .cpp carries the per-branch address citation.
uint32_t mine_scan_deposit_slot(const sim_view &v, sim_store &own, uint8_t slot_index, uint32_t player,
                                int32_t building_index, const mine_scan_deposit_slot_calls &c);

} // namespace detail

// Live wrapper: the logic applied to state() and live_mine_scan_deposit_slot_calls(). Matches the
// committed prototype (sig_llm_strat_mine_scan_deposit_slot) exactly.
uint32_t mine_scan_deposit_slot(uint8_t slot_index, uint32_t player, int32_t building_index);

namespace detail {
} // namespace detail

} // namespace mh::sim
