#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// ---- the outward calls -----------------------------------------------------------------------------
//
// Indirected for the same reason as every other sim/ TU: a direct mh::call:: inside a detail:: body
// reaches into the live game image, which makes the body untestable by net_selftest.exe simtest.
struct path_make_single_step_calls {
    void (*path_free_slot)(uint16_t player, int32_t unit_index);
    void (*tile_delta_wrapped)(int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t *out_dx,
                               int32_t *out_dy);
    void (*path_attach_slot)(int32_t player, int32_t unit_index, int32_t slot);
};

const path_make_single_step_calls &live_path_make_single_step_calls();

namespace detail {

// llm_strat_path_make_single_step @0x0049561f. See the header banner above for the full derivation;
// the .cpp carries the per-line address citation. Returns 1 iff a single-tile waypoint was queued,
// else 0 (no exact-direction match, or no free path slot).
int32_t path_make_single_step(const sim_view &v, sim_store &own, const path_make_single_step_calls &c,
                              uint32_t player, int32_t unit_index);

} // namespace detail

// Live wrapper: the logic applied to state() and live_path_make_single_step_calls(). Matches the
// committed prototype (sig_llm_strat_path_make_single_step) exactly.
int32_t path_make_single_step(uint32_t player, int32_t unit_index);

namespace detail {
} // namespace detail

} // namespace mh::sim
