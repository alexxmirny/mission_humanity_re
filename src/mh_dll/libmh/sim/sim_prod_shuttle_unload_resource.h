#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The three outward calls this function makes -- ALL ORIGINAL, indirected for offline testability
// (the offline `simtest` oracle passes recording/mocked stubs here so this ring member can be
// verified in isolation without recursing into the rest of the SCC). Two of the three
// (bldg_shuttle_slot_is_free, bldg_flush_cargo_hold) are SIBLING migrated functions in this same
// ring -- called through the ORIGINAL binary per Law 4 / the ring's ONE RULE, never through
// `mh::sim::bldg_shuttle_slot_is_free` / `mh::sim::bldg_flush_cargo_hold`.
struct prod_shuttle_unload_resource_calls {
    // llm_resource_add @0x00497f4a. EAX=player, EDX=resource_id, EBX=amount.
    void (*resource_add)(int32_t player, int32_t resource_id, int32_t amount);

    // llm_strat_bldg_shuttle_slot_is_free @0x0048fa97. EAX=player, EDX=building_index. Returns >0 if
    // the building's shuttle slot is now free.
    int32_t (*bldg_shuttle_slot_is_free)(int32_t player, int32_t building_index);

    // llm_strat_bldg_flush_cargo_hold @0x0048e046. EAX=player, EDX=building_index. SIBLING ring
    // member (also being migrated in this batch) -- called as the ORIGINAL here.
    void (*bldg_flush_cargo_hold)(uint32_t player, int32_t building_index);
};

const prod_shuttle_unload_resource_calls &live_prod_shuttle_unload_resource_calls();

namespace detail {

// llm_prod_shuttle_unload_resource @0x0048e4fb. See the header banner above for the full derivation;
// the .cpp carries the per-line address citation. Returns 1 (credited a nonzero amount back), or 0
// (computed amount was 0 -- nothing reserved, or cap clamped it to 0 -- nothing touched).
uint8_t prod_shuttle_unload_resource(const sim_view &v, sim_store &own,
                                     const prod_shuttle_unload_resource_calls &c, uint16_t player,
                                     int32_t building_index, uint16_t resource_id, int32_t cap);

} // namespace detail

// Live wrapper: the logic applied to state() and live_prod_shuttle_unload_resource_calls(). Matches
// the committed prototype (sig_llm_prod_shuttle_unload_resource) exactly.
uint8_t prod_shuttle_unload_resource(uint16_t player, int32_t building_index, uint16_t resource_id,
                                     int32_t cap);

namespace detail {
} // namespace detail

} // namespace mh::sim
