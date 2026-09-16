#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// llm_strat_unit_state member LANDING_REQUEST=0x29 -- see the header banner above for the derivation
// (sim_dock_slot_is_busy.cpp's already-adopted plate vocabulary on this same unbacked field).
inline constexpr uint16_t IDLE_SCATTER_ORDER_LANDING_REQUEST = 0x29;

// ---- the outward calls -----------------------------------------------------------------------
//
// Indirected for the same reason as every other sim/ TU: a direct mh::call:: inside a detail:: body
// reaches into the live game image, making the body untestable by net_selftest.exe simtest. Signatures
// copied verbatim from addr/mh_calls.gen.h. storage_get_approach_tile's out params carry the committed
// mh::call:: pointee type exactly (TACT1-P C6, 2026-09-04; see sim/sim_storage_get_approach_tile.h).
struct unit_state_idle_scatter_calls {
    void (*unit_set_state)(uint16_t new_state); // llm_strat_unit_set_state @0x004866c9
    int32_t (*rand_below)(int32_t upper_bound); // llm_rand_below @0x00499f49
    void (*storage_get_approach_tile)(uint16_t player, uint16_t unit_index, uint32_t *out_fine_x,
                                      uint32_t *out_fine_y, uint32_t storage_idx); // @0x0048b37c
    void (*unit_set_state_order)(uint16_t new_order, uint16_t new_state);          // @0x00486657
};

const unit_state_idle_scatter_calls &live_unit_state_idle_scatter_calls();

namespace detail {

// llm_strat_unit_state_idle_scatter @0x00485ebb. Zero-arg, ambient cur_player/cur_index/cur_unit,
// matching the original's void(void) signature.
void unit_state_idle_scatter(const sim_view &v, sim_store &own, const unit_state_idle_scatter_calls &c);

} // namespace detail

// Live wrapper: the logic applied to state() and live_unit_state_idle_scatter_calls(). Matches the
// committed prototype (sig_llm_strat_unit_state_idle_scatter) exactly.
void unit_state_idle_scatter();

namespace detail {
} // namespace detail

} // namespace mh::sim
