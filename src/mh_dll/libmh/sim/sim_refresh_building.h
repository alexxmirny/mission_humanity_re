#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The one external call this closure reaches -- llm_strat_bldg_uses_workers @0x004988b0, ALSO being
// translated as a sibling unit in this same batch. Per Law 4 / the translator brief's standing policy,
// siblings-still-in-flight are called through the ORIGINAL address (mh::call::), never a sibling's
// translated detail:: body -- indirected here (rather than calling mh::call:: directly in the body)
// for the same offline-testability reason sim_bldg_power_network_recompute.h's own calls struct gives.
struct refresh_building_calls {
    // llm_strat_bldg_uses_workers @0x004988b0. `int __watcall(uint32_t player, int32_t building_index)`
    // per its committed prototype (addr/mh_calls.gen.h's llm_strat_refresh_building wraps EAX=player/
    // EDX=building_index the same way this function's own callers do).
    int32_t (*uses_workers)(uint32_t player, int32_t building_index);
};

const refresh_building_calls &live_refresh_building_calls();

namespace detail {

// llm_strat_refresh_building @0x004705de. See the header banner above for the full derivation.
// Writes exactly ONE field, `buildings[p_id][b_id].efficiency`, on every path.
void refresh_building(const sim_view &v, sim_store &own, const refresh_building_calls &c,
                      uint16_t p_id, int32_t b_id);

} // namespace detail

// Live wrapper: the logic applied to state() and live_refresh_building_calls(). Matches the original's
// committed __watcall(uint16_t p_id, int32_t b_id) shape (sig_llm_strat_refresh_building).
void refresh_building(uint16_t p_id, int32_t b_id);

namespace detail {
} // namespace detail

} // namespace mh::sim
