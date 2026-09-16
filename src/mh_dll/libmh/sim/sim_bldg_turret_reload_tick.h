#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_strat_bldg_turret_reload_tick @0x0047c60b. See the header banner above for the full
// derivation; the .cpp carries the per-branch address citation.
void bldg_turret_reload_tick(const sim_view &v, sim_store &own, uint16_t player, uint32_t building_id,
                             double dt);

} // namespace detail

// Live wrapper: the logic applied to state(). Matches the committed prototype
// (sig_llm_strat_bldg_turret_reload_tick) exactly.
void bldg_turret_reload_tick(uint16_t player, uint32_t building_id, double dt);

namespace detail {
} // namespace detail

} // namespace mh::sim
