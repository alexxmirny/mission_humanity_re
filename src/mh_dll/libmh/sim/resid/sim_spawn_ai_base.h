#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The outward calls both functions in this TU make, indirected so detail:: stays testable under
// simtest. llm_strat_toroidal_dist_sq is sort_sites_by_dist's only callee; the rest are
// spawn_ai_base's. llm_strat_sort_sites_by_dist itself is NOT a member here -- it is reached as a
// plain detail:: call (G21, same-unit sibling), never through this table.
struct spawn_ai_base_calls {
    void (*ai_scr_parse)(char *filename); // llm_strat_ai_scr_parse @0x004ddb31
    int32_t (*sprintf_ai_scr)(void *dst, const char *fmt,
                              int32_t planet_index); // utils_sprintf__vi @0x004cfb9c (SIM-VARARGS)
    int32_t (*ai_group_create)(int32_t player);      // llm_strat_ai_group_create @0x004d4d85
    void (*ai_group_task_enqueue)(int32_t player, int32_t group, uint16_t a2, uint32_t p4, uint32_t p5,
                                  uint32_t p6, uint32_t p7, uint32_t p8,
                                  uint16_t p9);                      // llm_strat_ai_group_task_enqueue @0x004d4c20
    void (*ai_init_build_candidate_priorities)(int32_t player);      // @0x004dc7be
    void (*ai_build_plan_push)(int32_t player, int32_t ai_build_id); // @0x004dc781
    uint32_t (*toroidal_dist_sq)(int32_t x1, int32_t y1, int32_t x2,
                                 int32_t y2); // llm_strat_toroidal_dist_sq @0x00669f90
};

const spawn_ai_base_calls &live_spawn_ai_base_calls();

namespace detail {

// llm_strat_sort_sites_by_dist @0x004dc117. Reads/writes player_data[player_id].ai_resource_sites[]
// and reads ai_resource_site_count/ai_home_tile_x/ai_home_tile_y through `own` (mutable -- it is the
// writer). Reaches its one callee through `c`. void return, matching the original.
void sort_sites_by_dist(sim_store &own, const spawn_ai_base_calls &c, int32_t player_id);

// llm_strat_spawn_ai_base @0x004dcd0e. Reads map/cfg inputs through `v`, writes the profile ALIVE
// bit + the whole player_data[player] AI block + the active-player counter through `own`, calls its
// intra-unit sibling sort_sites_by_dist directly (G21), and reaches every other (frontier) callee
// through `c`. void return, matching the original.
void spawn_ai_base(const sim_view &v, sim_store &own, const spawn_ai_base_calls &c, int32_t player,
                   int32_t is_alien, int32_t x, int32_t y);

} // namespace detail

// Live wrappers: the logic applied to state() and live_spawn_ai_base_calls().
void sort_sites_by_dist(int32_t player_id);
void spawn_ai_base(int32_t player, int32_t is_alien, int32_t x, int32_t y);

} // namespace mh::sim
