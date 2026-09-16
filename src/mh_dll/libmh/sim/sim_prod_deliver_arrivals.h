#pragma once
#include <cstdint>

#include "sim/sim_state.h"
#include "sim/sim_unit_type_predicates.h" // UNIT_TYPE_A_HELI_MOTHER/_H_HELI_MOTHER (this header)
                                          // + UNIT_TYPE_A_HELI_CARGO/_H_HELI_CARGO (transitively via
                                          // sim_order_enqueue.h) -- existing Ghidra enum members
                                          // (rule 17a), not re-derived locally.

namespace mh::sim {

// ---- the outward calls ---------------------------------------------------------------------------
//
// Indirected for the same reason as every other sim/ TU: a direct mh::call:: inside a detail:: body
// reaches into the live game image, which makes the body untestable by net_selftest.exe simtest.
struct prod_deliver_arrivals_calls {
    uint32_t (*locate_active_port)(uint32_t player, int32_t *out_col, int32_t *out_row,
                                   uint32_t *out_port_slot);
    int32_t (*prod_bind_planet)(int32_t player, int32_t queue_slot, int32_t shuttle_slot);
    uint32_t (*find_mothership_position)(int32_t player, uint32_t *out_x, uint32_t *out_y);
    uint32_t (*spawn_arrived_unit)(uint16_t player, uint32_t slot, uint32_t a2, uint32_t a3, int32_t a4);
};

const prod_deliver_arrivals_calls &live_prod_deliver_arrivals_calls();

namespace detail {

// llm_strat_prod_deliver_arrivals @0x0048dc65. See the header banner above for the full derivation;
// the .cpp carries the per-branch address citation.
void prod_deliver_arrivals(const sim_view &v, sim_store &own, const prod_deliver_arrivals_calls &c);

} // namespace detail

// Live wrapper: the logic applied to state() and live_prod_deliver_arrivals_calls(). Matches the
// committed prototype (sig_llm_strat_prod_deliver_arrivals) exactly -- void(void). Also the name two
// sibling units in this same batch (sim_prod_unbind_planet.cpp, sim_prod_shuttle_complete.cpp) already
// call directly.
void prod_deliver_arrivals();

namespace detail {
} // namespace detail

} // namespace mh::sim
