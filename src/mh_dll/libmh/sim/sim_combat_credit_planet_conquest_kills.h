#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The player-profile status_flags bit tested here (E_STRAT_PLAYER_STATUS bit1, per
// mh_structs.gen.h's status_flags field comment). A file-local re-declaration of the same bit
// ai/ai_state.h's PLAYER_STATUS_ALIVE / orders/order_queue.cpp's PLAYER_ALIVE / lockstep/resync.cpp's
// PLAYER_ALIVE already carry for their own domains -- libmh/sim/ does not depend on libmh/ai/ or
// libmh/orders/ (see sim_state.h's own duplicated-`using`-alias precedent for the same reasoning), so
// this is another independent copy of the same constant, not a new discovery.
inline constexpr uint32_t STRAT_PLAYER_STATUS_ALIVE = 0x2u;

inline constexpr double CONQUEST_UNIT_KILL_DAMAGE     = 2000.0; // llm_strat_unit_kill_credit's damage arg
inline constexpr double CONQUEST_BUILDING_KILL_DAMAGE = 5000.0; // llm_strat_bldg_kill_credit's damage arg

// ---- the outward calls ---------------------------------------------------------------------------
//
// Indirected for the same reason as every other sim/ TU: a direct mh::call:: inside a detail:: body
// reaches into the live game image, which makes the body untestable by net_selftest.exe simtest.
struct combat_credit_planet_conquest_kills_calls {
    void (*unit_kill_credit)(uint32_t victim_player, int32_t victim_unit_index, double damage,
                             uint32_t killer_info, int32_t killer_unit_index);
    void (*bldg_kill_credit)(uint32_t victim_player, int32_t victim_building_index, double damage,
                             uint32_t killer_info, int32_t killer_unit_index);
};

const combat_credit_planet_conquest_kills_calls &live_combat_credit_planet_conquest_kills_calls();

namespace detail {

// llm_combat_credit_planet_conquest_kills @0x0049928a. See the header banner above for the full
// derivation, including why the outer/witness loop counter and the witness value swap names in the
// Ghidra .c draft.
void combat_credit_planet_conquest_kills(const sim_view &v, uint32_t player,
                                         const combat_credit_planet_conquest_kills_calls &c);

} // namespace detail

// Live wrapper: the logic applied to state().read and live_combat_credit_planet_conquest_kills_calls().
// Matches the committed prototype (sig_llm_combat_credit_planet_conquest_kills) exactly.
void combat_credit_planet_conquest_kills(uint32_t player);

namespace detail {
} // namespace detail

} // namespace mh::sim
