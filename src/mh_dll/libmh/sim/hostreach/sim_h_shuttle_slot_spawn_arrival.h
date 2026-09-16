#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The two outward calls, indirected like every sim/ TU so detail:: stays testable under simtest. Both
// are already-owned sim rows (batch context rule 3b) -- bound to MH_LIBMH_BIND(<name>) in the .cpp, never
// called as bare mh::call:: from inside detail::.
struct shuttle_slot_spawn_arrival_calls {
    uint32_t (*find_mothership_position)(int32_t player, uint32_t *out_x,
                                         uint32_t *out_y); // llm_strat_bldg_find_mothership_position @0x0048fdd0
    uint32_t (*spawn_arrived_unit)(uint16_t player, uint32_t slot, uint32_t x, uint32_t y,
                                   int32_t storage_idx); // llm_strat_prod_spawn_arrived_unit @0x0048f31e
};

const shuttle_slot_spawn_arrival_calls &live_shuttle_slot_spawn_arrival_calls();

namespace detail {

// llm_strat_prod_shuttle_slot_spawn_arrival @0x004906c1. See the header banner above for the full
// derivation, in particular the uninitialised-locals proof; the .cpp carries the per-branch address
// citation. Reads the shuttle-slot roster and PlayerSide/G_PLANET_INDEX through `v`, reaches the two
// owned-row callees through `c`. No sim_store write of its own -- every write happens inside the callees.
void shuttle_slot_spawn_arrival(const sim_view &v, const shuttle_slot_spawn_arrival_calls &c,
                                uint32_t slot_index);

} // namespace detail

// Live wrapper: the logic applied to state().read and live_shuttle_slot_spawn_arrival_calls(). Matches
// the committed prototype (`void __watcall llm_strat_prod_shuttle_slot_spawn_arrival(uint slot_index)`)
// exactly -- this is the promotion seam the conductor's MH_EXPORT_REPLACE adapter forwards to.
void shuttle_slot_spawn_arrival(uint32_t slot_index);

} // namespace mh::sim
