#pragma once
#include <cstdint>

#include "sim/sim_order_enqueue.h" // BUILDING_TYPE_H_MOTHER / BUILDING_TYPE_A_MOTHER (already committed there)
#include "sim/sim_state.h"

namespace mh::sim {
namespace detail {

// llm_strat_game_check_players_mothership_alive @0x00498dc0. See the header banner above for the
// full derivation, including why the return polarity is inverted and why the player range is
// hardcoded to {1,2}.
int32_t game_check_players_mothership_alive(const sim_view &v);

} // namespace detail

// Live wrapper: the logic applied to state().read. Matches the original's committed `int(void)`
// __watcall shape (sig_llm_strat_game_check_players_mothership_alive in mh_export.gen.h). Same name
// libmh/sim/sim_bldg_state_destroyed.h's own `game_check_players_mothership_alive_calls` field already
// uses for the ORIGINAL out-call, so a reader who has seen that caller recognizes this name.
int32_t game_check_players_mothership_alive();

namespace detail {
} // namespace detail

} // namespace mh::sim
