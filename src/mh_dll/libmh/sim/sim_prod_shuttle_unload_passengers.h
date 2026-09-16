#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// ---- the three outward calls, all ring siblings called as ORIGINALS ---------------------------
//
// Indirected for the same reason as every other sim/ TU: a direct mh::call:: inside a detail::
// body reaches into the live game image, which makes the body untestable by net_selftest.exe
// simtest -- and, for this ring specifically, is what lets the offline oracle substitute recording
// stubs for every sibling so an SCC member can be verified in isolation at all.
struct prod_shuttle_unload_passengers_calls {
    // llm_strat_population_add @0x00491328. EAX=player, EDX=count.
    void (*population_add)(uint16_t player, int32_t count);

    // llm_strat_bldg_shuttle_slot_is_free @0x0048fa97. EAX=player, EDX=building_id. Returns >0 if
    // the slot is free.
    int32_t (*bldg_shuttle_slot_is_free)(int32_t player, int32_t building_id);

    // llm_strat_bldg_flush_cargo_hold @0x0048e046. EAX=player, EDX=building_index. SCC sibling --
    // called as the ORIGINAL, never mh::sim's own translation of it.
    void (*bldg_flush_cargo_hold)(uint32_t player, int32_t building_index);
};

const prod_shuttle_unload_passengers_calls &live_prod_shuttle_unload_passengers_calls();

namespace detail {

// llm_prod_shuttle_unload_passengers @0x0048e6f2. See the header banner above for the full
// derivation; the .cpp carries the per-line address citation. Returns 0 if nothing was released,
// 1 if colonists were released (and the flush/free check ran).
int32_t prod_shuttle_unload_passengers(const sim_view &v, sim_store &own,
                                       const prod_shuttle_unload_passengers_calls &c, uint16_t player,
                                       int32_t building_index, int32_t cap);

} // namespace detail

// ---- the public surface, in original-behaviour terms -----------------------------------------
// Parameter/return types match the committed prototype in addr/mh_export.gen.h exactly:
// int32_t(uint16_t player, int32_t building_index, int32_t cap).
int32_t prod_shuttle_unload_passengers(uint16_t player, int32_t building_index, int32_t cap);

namespace detail {
} // namespace detail

} // namespace mh::sim
