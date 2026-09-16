#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The two outward calls, indirected (like every sim/ TU) so detail:: stays testable under
// simtest. Both are frontier originals (neither is a sim_resid sibling), reached through
// mh::call:: only inside live_map_fill_defaults_calls(), never directly from detail::.
struct map_fill_defaults_calls {
    void (*bldg_panel_open)();                                           // llm_ui_bldg_panel_open @0x0041c061
    void *(*fill_data)(void *ptr, uint32_t size, uint8_t default_value); // utils_fill_data @0x004d1780
};

const map_fill_defaults_calls &live_map_fill_defaults_calls();

namespace detail {

// map_FillDefaults @0x0045603e. Reads game_clock and the cfg Progress-table row-0 `.index` field
// (the per-player tech-reset loop bound) through `v`; writes every region named in the header
// banner through `own`; calls llm_ui_bldg_panel_open and utils_fill_data through `c`. void
// return, matching the original.
void map_fill_defaults(const sim_view &v, sim_store &own, const map_fill_defaults_calls &c);

} // namespace detail

// Live wrapper: the logic applied to state() and live_map_fill_defaults_calls().
void map_fill_defaults();

} // namespace mh::sim
