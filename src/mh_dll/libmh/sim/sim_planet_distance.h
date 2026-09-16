#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// Indirected for the same reason as every other sim/ TU: a direct mh::call:: inside a detail:: body
// reaches into the live game image, which makes the body untestable by net_selftest.exe simtest. Both
// callees are ORIGINAL functions outside this batch (neither has its own sim/ reimplementation yet).
struct planet_distance_calls {
    // llm_strat_tile_delta_wrapped @0x004941b9 -- wrapped (dx,dy) tile delta between (x1,y1) and
    // (x2,y2), via two int32 out-pointers.
    void (*tile_delta_wrapped)(int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t *out_dx,
                               int32_t *out_dy);
    double (*sqrt_fn)(double x); // llm_sqrt @0x004da9c0
};

const planet_distance_calls &live_planet_distance_calls();

namespace detail {

// llm_strat_planet_distance @0x00493fb2. See the header banner for the full derivation. The sole
// direct writer of map_width/map_height in the sim closure -- takes `sim_store &own` (non-const) to
// reach map_width_mut()/map_height_mut(), saves+forces+restores them around the callee call.
double planet_distance(const sim_view &v, sim_store &own, const planet_distance_calls &c, int32_t x1,
                       int32_t y1, int32_t x2, int32_t y2);

} // namespace detail

// Live wrapper: the logic applied to state() and live_planet_distance_calls(). Matches the committed
// prototype (sig_llm_strat_planet_distance, int32_t x4 params -- committed EN v402) exactly -- the values are
// signed tile-coordinate deltas, but the ABI-committed parameter type is uint32_t (Ghidra's own
// `undefined4`), so the cast to int32_t for the actual arithmetic happens inside the .cpp, not here.
double planet_distance(int32_t x1, int32_t y1, int32_t x2, int32_t y2);

namespace detail {
} // namespace detail

} // namespace mh::sim
