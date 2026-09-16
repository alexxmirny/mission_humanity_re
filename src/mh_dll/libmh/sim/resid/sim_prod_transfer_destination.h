#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The two real (frontier) outward calls, indirected like every sim/ TU so detail:: stays testable
// under simtest. Both are frontier originals (not sim_resid siblings), reached through
// live_prod_transfer_destination_calls() from mh::call::, never called directly.
struct prod_transfer_destination_calls {
    double (*planet_distance)(int32_t x1, int32_t y1, int32_t x2, int32_t y2); // llm_strat_planet_distance @0x00493fb2
    double (*prod_transfer_progress)(int32_t slot);                            // llm_strat_prod_transfer_progress @0x00490368
};

const prod_transfer_destination_calls &live_prod_transfer_destination_calls();

namespace detail {

// llm_strat_prod_set_transfer_destination @0x0048efcc. Reads/writes the shuttle slot roster
// (own.prod_shuttle_slot_at) and reads Planets/Unit/Building cfg tables through `v`. Returns 1 on a
// successful reroute, -1 if the slot is not an active transfer (matching the original's int return).
int32_t prod_set_transfer_destination(const sim_view &v, sim_store &own, const prod_transfer_destination_calls &c,
                                      uint32_t player_idx, int32_t prod_slot, int32_t dest_planet);

} // namespace detail

// Live wrapper: the logic applied to state() and live_prod_transfer_destination_calls().
int32_t prod_set_transfer_destination(uint32_t player_idx, int32_t prod_slot, int32_t dest_planet);

} // namespace mh::sim
