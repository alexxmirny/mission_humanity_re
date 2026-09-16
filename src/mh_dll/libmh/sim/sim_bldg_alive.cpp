//
// sim/sim_bldg_alive.cpp -- see sim_bldg_alive.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_bldg_is_alive_004d3d45.asm), not from Ghidra's C draft.
//
#include "sim/sim_bldg_alive.h"

namespace mh::sim {
namespace detail {

int32_t bldg_is_alive(const sim_view &v, int32_t player, int32_t building_index) {
    const building &b = building_of(v, (uint32_t)player, building_index);
    // 0x004d3d83-0x004d3d8e -- see the header for why this is `!(<= 0.0)` and not `0.0 <`.
    if (!(b.energy <= 0.0)) {
        // 0x004d3d90-0x004d3d98. A WORD compare against 4; `state` is uint16_t.
        if (b.state != BLDG_STATE_RUBBLE_SIGHT_DECAY) return 1;
    }
    return 0;
}

} // namespace detail

int32_t bldg_is_alive(int32_t player, int32_t building_index) {
    const sim_view v = state().read;
    return detail::bldg_is_alive(v, player, building_index);
}

} // namespace mh::sim
