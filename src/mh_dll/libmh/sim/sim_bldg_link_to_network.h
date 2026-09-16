#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The two callees this closure reaches, indirected for offline testability (same reason as every
// other module here: a direct mh::call:: inside a detail:: body reaches into the live game image,
// which makes the body untestable by net_selftest.exe simtest).
struct bldg_link_to_network_calls {
    // llm_bldg_set_connected_flag @0x004965d6. Already committed in addr/mh_calls.gen.h.
    void (*bldg_set_connected_flag)(uint16_t player, int32_t b_index);
    // llm_strat_bldg_propagate_network_connectivity @0x0049209b. Already committed in
    // addr/mh_calls.gen.h.
    void (*bldg_propagate_network_connectivity)(uint16_t player, int32_t b_index);
};

const bldg_link_to_network_calls &live_bldg_link_to_network_calls();

namespace detail {

// llm_strat_bldg_link_to_network_if_adjacent @0x0049232e. See the header banner above for the full
// derivation; the .cpp carries the per-branch address citation. Read-only over sim state (no
// sim_store parameter -- see the CALLEES note above: every write happens inside `gc`'s two callees).
void bldg_link_to_network_if_adjacent(const sim_view &v, const bldg_link_to_network_calls &gc,
                                      uint16_t player, uint32_t index);

} // namespace detail

// ---- the public surface, in original-behaviour terms -----------------------------------------
// Parameter types match the committed prototype in addr/mh_calls.gen.h exactly (the drift gate
// enforces this on export/shadow installation): void(uint16_t player, uint32_t index).
void bldg_link_to_network_if_adjacent(uint16_t player, uint32_t index);

namespace detail {
} // namespace detail

} // namespace mh::sim
