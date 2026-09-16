//
// sim/sim_landing_queries.h -- two small landing-site accessors (RI-SIM / SIM1F):
//
//   llm_strat_count_landing_spots   @0x00454d8d (0x58 bytes)  -- pure read
//   llm_strat_set_landing_site      @0x00454fe3 (0x72 bytes)  -- per-player/per-planet write
//
// Grouped as one TU: both are tiny leaf accessors of the landing-site data, neither calls anything
// (only the inert assert_stack_capacity prologue), so neither needs a `_calls` struct.
//
// ---- llm_strat_count_landing_spots -----------------------------------------------------------------
//
// `int __watcall llm_strat_count_landing_spots(void)`. Returns the number of leading active entries
// in _G_LLM_STRAT_LANDING_SPOTS[16] (sim_view::landing_spots): the length of the run whose
// .status != -1, capped at 16. Consumed by game::UpdateProgress (PLANET case) and
// llm_strat_spawn_enemy_landing. FAITHFULLY-REPRODUCED read order: the loop test is
// `landing_spots[i].status != -1 && i < 16`, so when all 16 slots are active it reads
// landing_spots[16].status (one past the array) BEFORE the `i < 16` test stops it -- a read of the
// adjacent region, exactly as the original does over real process memory.
//
// ---- llm_strat_set_landing_site --------------------------------------------------------------------
//
// `uint8_t __watcall llm_strat_set_landing_site(game_t_Player player, game_t_PlanetIndex planet,
// int x, int param_4, int param_5)`. Stores the landing position for one player/planet:
//   profiles[player].landing_x[planet]          = x;
//   profiles[player].landing_y[planet]          = param_4;   // y
//   profiles[player].landing_spot_index[planet] = param_5;   // which spot was claimed
// and returns `(uint8_t)param_5`. The mutation is through the mutable profile accessor
// (sim_store::profile_at). The committed prototype widens the three value params to uint32_t.
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_strat_count_landing_spots @0x00454d8d. Pure read of sim_view::landing_spots; no callees.
int32_t count_landing_spots(const sim_view &v);

// llm_strat_set_landing_site @0x00454fe3. Writes profiles[player].landing_*; no callees.
uint8_t set_landing_site(sim_store &own, uint32_t player, uint32_t planet, int32_t x, int32_t y,
                         int32_t spot_index);


} // namespace detail

// Public wrappers. Signatures match the committed prototypes in addr/mh_calls.gen.h exactly.
int32_t count_landing_spots();
uint8_t set_landing_site(uint32_t player, uint32_t planet, uint32_t x, uint32_t param_4,
                         uint32_t param_5);

} // namespace mh::sim
