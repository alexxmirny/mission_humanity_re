#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The outward calls, indirected (like every sim/ TU) so detail:: stays testable under simtest. Both
// are frontier originals (neither is a sim_resid sibling translation), so they are reached through
// mh::call:: in live_player_init_calls(), never called directly.
struct player_init_calls {
    void (*player_set_color)(int32_t player_idx, uint32_t color_index); // llm_strat_player_set_color @0x00454c51
    void (*ai_init_build_candidate_priorities)(int32_t player_index);   // llm_strat_ai_init_build_candidate_priorities @0x004dc7be
};

const player_init_calls &live_player_init_calls();

namespace detail {

// llm_strat_init_human_player_data @0x004dd91d. Reads the three AI think-cycle periods + the clock
// stagger fraction through `v`, writes one player_data record + the active-player high-water mark
// through `own`, reaches llm_strat_ai_init_build_candidate_priorities through `c`. void return,
// matching the original.
void init_human_player_data(const sim_view &v, sim_store &own, const player_init_calls &c,
                            uint32_t player_idx, int32_t is_alien_race);

// llm_strat_player_profile_init @0x00454985. Writes ONE _G_LLM_STRAT_PLAYERS record through `own`,
// reaches llm_strat_player_set_color through `c`. No sim_view reads. void return, matching the
// original. Parameter types/order/storage per the disassembly header (game_t_Player player: EAX; uint
// controller_flags: EDX; uint race: EBX; double game_clock/uint color_index/char *name_str/int
// side_id: stack).
void player_profile_init(sim_store &own, const player_init_calls &c, int32_t player,
                         uint32_t controller_flags, uint32_t race, double game_clock,
                         uint32_t color_index, char *name_str, int32_t side_id);

// llm_strat_player_param_defaults_init @0x00455a84. Writes three parallel double[8] arrays through
// `own`. No sim_view reads, no callees. void return, matching the original (void __watcall(void)).
void player_param_defaults_init(sim_store &own);

} // namespace detail

// Live wrappers: the logic applied to state() and live_player_init_calls().
void player_profile_init(int32_t player, uint32_t controller_flags, uint32_t race, double game_clock,
                         uint32_t color_index, char *name_str, int32_t side_id);
void player_param_defaults_init();
void init_human_player_data(uint32_t player_idx, int32_t is_alien_race);

} // namespace mh::sim
