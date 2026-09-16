#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The one outward call this function makes. Indirected for the same reason as every other sim/ TU
// (sim_bldg_pay_costs.h's own `pay_costs_calls` is the precedent for this exact one-member shape): a
// direct mh::call:: inside detail:: would reach into the live game image and make the body untestable
// by net_selftest.exe simtest / the offline fixture. game_SpendResource is the sim closure's ONLY
// writer of player_resources (sim_state.h's own comment on that view member).
struct prod_shuttle_load_resource_calls {
    void (*spend_resource)(int32_t player, int32_t resource_id, int32_t amount); // game_SpendResource @0x00497f94
};

const prod_shuttle_load_resource_calls &live_prod_shuttle_load_resource_calls();

namespace detail {

// llm_prod_shuttle_load_resource @0x0048e3bb. See the header banner above for the full derivation;
// the .cpp carries the per-branch address citation. Returns 1 (loaded a nonzero amount) or
// 0 (zero-capacity slot, or the clamped quantity came out to zero).
int32_t prod_shuttle_load_resource(const sim_view &v, sim_store &own,
                                   const prod_shuttle_load_resource_calls &c, uint32_t player,
                                   int32_t building_index, uint32_t resource_id, uint32_t cap);

} // namespace detail

// Live wrapper: the logic applied to state() and live_prod_shuttle_load_resource_calls(). Matches
// the committed prototype (sig_llm_prod_shuttle_load_resource) exactly.
int32_t prod_shuttle_load_resource(uint32_t player, int32_t building_index, uint32_t resource_id,
                                   uint32_t cap);

namespace detail {
} // namespace detail

} // namespace mh::sim
