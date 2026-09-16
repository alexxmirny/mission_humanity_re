#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_strat_unit_weapon_reload_tick @0x0047de7d. See the header banner above for the full
// derivation; the .cpp carries the per-branch address citation. `game_clock_unread` is declared
// (every retail caller pushes it) but genuinely unread in the body -- see the banner note.
void unit_weapon_reload_tick(const sim_view &v, sim_store &own, uint8_t weapon_slot,
                             double delta_time, double game_clock_unread);

} // namespace detail

// Live wrapper: the logic applied to state(). Matches the committed prototype
// (sig_llm_strat_unit_weapon_reload_tick) exactly.
void unit_weapon_reload_tick(uint8_t weapon_slot, double delta_time, double game_clock_unread);

namespace detail {
} // namespace detail

} // namespace mh::sim
