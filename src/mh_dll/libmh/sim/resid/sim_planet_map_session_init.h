#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The outward calls, indirected (like every sim/ TU) so detail:: stays testable under simtest. All
// three are frontier originals (none of them are among the ten sim_resid sibling translations), so
// they are reached through mh::call:: in live_planet_map_session_init_calls(), never called
// directly.
struct planet_map_session_init_calls {
    // llm_gfx_pack_rgb16 @0x0043c56b IS GONE (LIFT-TABLE S4, 2026-09-09) -- the nine-call palette
    // block left for the host with its nine cells, and the host-API entry left with it: this was
    // its only libmh caller. Its declaration here also carried the (r, g, b) parameter names, which
    // were WRONG (the order is red, blue, green); the corrected spelling lives at the sink.
    void (*rng_seed_channel)(int32_t channel, uint32_t value); // llm_strat_rng_seed_channel @0x004b4ce6
    void (*bldg_recompute_cell_grid)();                        // llm_strat_bldg_recompute_cell_grid @0x004dc25f
    void (*ai_spiral_table_init)();                            // llm_strat_ai_spiral_table_init @0x004dc597
};

const planet_map_session_init_calls &live_planet_map_session_init_calls();

namespace detail {

// llm_strat_planet_map_session_init @0x004dc65a. Reads the map extents through `v`, writes
// width_m/height_m, the AI active-player counter, and the nine palette cells through `own`, and
// reaches every callee (all frontier) through `c`. void return, matching the original.
void planet_map_session_init(const sim_view &v, sim_store &own, const planet_map_session_init_calls &c);

} // namespace detail

// Live wrapper: the logic applied to state() and live_planet_map_session_init_calls().
void planet_map_session_init();

} // namespace mh::sim
