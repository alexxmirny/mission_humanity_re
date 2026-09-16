#pragma once
#include <cstdint>

#include "sim/sim_order_enqueue.h" // BUILDING_TYPE_A_PORT / BUILDING_TYPE_H_PORT (already committed there)
#include "sim/sim_state.h"

namespace mh::sim {

// ---- the outward calls ---------------------------------------------------------------------------
//
// Indirected for the same reason as every other sim/ TU: a direct mh::call:: inside a detail:: body
// reaches into the live game image, which makes the body untestable by net_selftest.exe simtest.
struct locate_active_port_calls {
    // llm_strat_tile_neighbor_reverse_dir @0x0048b308. Bound directly to mh::call:: (matching its
    // committed `void(int32_t, int32_t, int32_t, uint32_t*, uint32_t*)` signature exactly, per
    // the translation lint's positional pairing -- TACT1-P C6, 2026-09-04).
    void (*tile_neighbor_reverse_dir)(int32_t tile_x, int32_t tile_y, int32_t dir_index, uint32_t *out_x,
                                      uint32_t *out_y);
};

const locate_active_port_calls &live_locate_active_port_calls();

namespace detail {

// llm_strat_locate_active_port @0x0048fbfa. See the header banner above for the full derivation.
// Signature mirrors the committed export/call shape (out params are raw typed pointers here, matching
// the asm header's own `int *out_col, int *out_row, uint *out_port_slot`); the public wrapper below
// carries the same pointees (TACT1-P C6, 2026-09-04) to match addr/mh_calls.gen.h's existing
// `llm_strat_locate_active_port(uint32_t, int32_t*, int32_t*, uint32_t*)`.
uint32_t locate_active_port(const sim_view &v, const locate_active_port_calls &c, uint32_t player,
                            int32_t *out_col, int32_t *out_row, uint32_t *out_port_slot);

} // namespace detail

// Live wrapper: the logic applied to state().read and live_locate_active_port_calls(). Signature
// matches the committed __watcall shape already in addr/mh_calls.gen.h and addr/mh_export.gen.h
// exactly (sig_llm_strat_locate_active_port) -- the same shape this project's two existing callers
// (sim_prod_shuttle_complete.cpp, sim_prod_deliver_arrivals.cpp) already call through
// mh::call::llm_strat_locate_active_port.
uint32_t locate_active_port(uint32_t player, int32_t *out_col, int32_t *out_row, uint32_t *out_port_slot);

namespace detail {
} // namespace detail

} // namespace mh::sim
