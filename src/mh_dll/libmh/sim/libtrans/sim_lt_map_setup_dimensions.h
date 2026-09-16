#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// Indirected per translator-brief rule 3b: a direct mh::call:: inside a detail:: body reaches into
// the live game image, which the offline oracle (net_selftest.exe libtranstest) cannot map.
struct lt_map_setup_dimensions_calls {
    void (*set_minimap_zoom_for_size)(); // llm_map_set_minimap_zoom_for_size @0x00498a3f
};

const lt_map_setup_dimensions_calls &live_lt_map_setup_dimensions_calls();

namespace detail {

// llm_map_setup_dimensions @0x004989c2. See the header banner above for the write order, the
// re-read discipline, and the pathfinder-byte width.
void map_setup_dimensions(const sim_view &v, sim_store &own, const lt_map_setup_dimensions_calls &c);

} // namespace detail

// Live wrapper: the logic applied to state() and live_lt_map_setup_dimensions_calls(). Matches the
// original's committed __watcall(void) shape.
void map_setup_dimensions();

namespace detail {
} // namespace detail

} // namespace mh::sim
