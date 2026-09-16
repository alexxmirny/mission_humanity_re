#include "sim/sim_game_update_resource_stats.h"

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {
namespace detail {

// game_UpdateResourceStats @0x004dbeec. 0x004dbef7-0x004dbf06: three guards
// (res_id in [1,4] UNSIGNED, then amount > 0 SIGNED) gate the single accumulate at
// 0x004dbf06-0x004dbf1e. `EAX = res_id` (held live in ECX across the address math and moved back into
// EAX right before the store) indexes the row from a base FOUR BYTES BEFORE `resource_spent[0]` --
// i.e. the field this function actually touches is `resource_spent[res_id - 1]`, per the header
// banner's derivation. No callees.
void update_resource_stats(sim_store &own, int32_t player, int32_t amount, uint32_t res_id) {
    if (res_id >= 1u && res_id <= 4u && amount > 0) {
        own.player_at((uint32_t)player).resource_spent[res_id - 1] += amount;
    }
}

} // namespace detail

// ---- the public wrapper ---------------------------------------------------------------------------

void update_resource_stats(uint32_t player, uint32_t amount, uint32_t res_id) {
    sim_state st = state();
    detail::update_resource_stats(st.own, (int32_t)player, (int32_t)amount, res_id);
}


} // namespace mh::sim
