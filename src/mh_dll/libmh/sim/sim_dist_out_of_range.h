#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// ---- the outward calls -----------------------------------------------------------------------
//
// Indirected for the same reason as every other sim/ TU: a direct mh::call:: inside a detail:: body
// reaches into the live game image, which makes the body untestable by net_selftest.exe simtest. One
// callee, an ORIGINAL function outside this batch.
struct dist_out_of_range_calls {
    // llm_strat_tile_dist_wrapped @0x0049404e -- wrapped tile distance between (x1,y1) and (x2,y2).
    int32_t (*tile_dist_wrapped)(int32_t x1, int32_t y1, int32_t x2, int32_t y2);
};

const dist_out_of_range_calls &live_dist_out_of_range_calls();

namespace detail {

// llm_strat_dist_out_of_range @0x00449ac1. See the header banner above for the full derivation.
// Pure read: no sim_view parameter (nothing in this closure reads or writes any sim state here).
uint32_t dist_out_of_range(const dist_out_of_range_calls &c, int32_t range_min, int32_t range_max,
                           int32_t x, int32_t y, int32_t target_tile_x, int32_t target_tile_y);

} // namespace detail

// Live wrapper: the logic applied to live_dist_out_of_range_calls(). Matches the committed prototype
// (sig_llm_strat_dist_out_of_range) exactly.
uint32_t dist_out_of_range(int32_t range_min, int32_t range_max, int32_t x, int32_t y,
                           int32_t target_tile_x, int32_t target_tile_y);

namespace detail {
} // namespace detail

} // namespace mh::sim
