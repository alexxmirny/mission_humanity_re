#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The one outward call, indirected (like every sim/ TU) so detail:: stays testable under
// libtranstest. utils_fill_data is a frontier original, reached through mh::call:: only inside
// live_available_projects_clear_calls(), never called directly from detail::.
struct available_projects_clear_calls {
    void *(*fill_data)(void *ptr, uint32_t size, uint8_t default_value); // utils_fill_data @0x004d1780
};

const available_projects_clear_calls &live_available_projects_clear_calls();

namespace detail {

// game_ClearAvailableProjects @0x0041c02e. Writes the whole AvailableProjects region through
// `own`; calls utils_fill_data through `c`. Takes no sim_view read -- the original reads nothing
// from state, only overwrites it. void return, matching the original (the fill's returned pointer
// is discarded).
void available_projects_clear(sim_store &own, const available_projects_clear_calls &c);

} // namespace detail

// Live wrapper: the logic applied to state().own and live_available_projects_clear_calls().
void available_projects_clear();

} // namespace mh::sim
