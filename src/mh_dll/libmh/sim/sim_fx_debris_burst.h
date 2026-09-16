#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The three external callees this closure reaches, indirected for offline testability -- a direct
// mh::call:: inside a detail:: body reaches into the live game image, which makes the body untestable
// by net_selftest.exe simtest / the offline fixture (same reasoning as every other module here).
struct debris_burst_calls {
    int32_t (*rand_below_fx)(uint32_t upper_bound); // llm_rand_below_fx @0x00499f84 (RNG seed slot 1;
                                                    // int32 return since the 2026-09-02 re-dump)
    double (*zoom_scale_x_get)();                   // llm_map_zoom_scale_x_get @0x004a8016
};

const debris_burst_calls &live_debris_burst_calls();

namespace detail {

// llm_strat_spawn_debris_burst @0x0044dbd2. `intensity` is param_1 (EAX) of the committed
// __watcall prototype. See the header banner above for the full derivation.
void spawn_debris_burst(int32_t intensity, const sim_view &v, sim_store &own, const debris_burst_calls &c);

} // namespace detail

// Live wrapper: the logic applied to state() and live_debris_burst_calls(). Matches the committed
// prototype (sig_llm_strat_spawn_debris_burst) exactly.
void spawn_debris_burst(int32_t intensity);

namespace detail {
} // namespace detail

} // namespace mh::sim
