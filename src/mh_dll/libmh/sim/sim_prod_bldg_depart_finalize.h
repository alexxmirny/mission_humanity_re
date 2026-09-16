#pragma once
#include <cstdint>

#include "sim/sim_order_enqueue.h" // BUILDING_TYPE_A_*/H_*, UNIT_STATE_STOP_TO_DEFAULT -- pinned there
                                   // (SIM1C), reused rather than re-declared.
#include "sim/sim_state.h"

namespace mh::sim {

// The six outward calls this closure reaches, indirected for offline testability -- same reason
// sim_bldg_construct_finalize.h / sim_bldg_state_destroyed.h indirect their own committed-original
// callees. Member function-pointer types match each callee's OWN committed prototype in
// addr/mh_calls.gen.h exactly.
struct prod_bldg_depart_finalize_calls {
    // llm_strat_planet_distance @0x00493fb2 -- (src_x, src_y, dst_x, dst_y) -> distance.
    double (*planet_distance)(int32_t src_x, int32_t src_y, int32_t dst_x, int32_t dst_y);
    // llm_prod_planet_distance_factor @0x00490221.
    double (*planet_distance_factor)(int32_t src_planet, int32_t dest_planet);
    // llm_prod_shuttle_fuel_check @0x004934b6 -- nonzero = fuel check failed.
    int32_t (*shuttle_fuel_check)(uint16_t player, int32_t building_index, int32_t dest_planet);
    // llm_strat_unit_add_docked @0x0046443a -- 0 = failed to dock.
    int32_t (*unit_add_docked)(uint32_t unit_proto_id, uint16_t player, uint32_t probe_slot);
    // llm_strat_storage_launch_parked_to_orbit @0x0048f952 -- 0 = no unit launched.
    int32_t (*storage_launch_parked_to_orbit)(uint16_t player, int32_t building_index);
    // llm_strat_prod_unbind_planet @0x0048ff73 -- called 2-arg at BOTH sites, see the phantom-argument
    // hazard above.
    void (*prod_unbind_planet)(int32_t player, int32_t planet_slot);
};

const prod_bldg_depart_finalize_calls &live_prod_bldg_depart_finalize_calls();

namespace detail {

// llm_prod_bldg_depart_finalize @0x0048ec51. See the header banner above for the full derivation; the
// .cpp carries the per-branch address citation. Writes buildings/units/prod_shuttle_slots -- takes the
// mutable store.
int32_t prod_bldg_depart_finalize(const sim_view &v, sim_store &own,
                                  const prod_bldg_depart_finalize_calls &c, uint16_t player,
                                  int32_t building_index, int32_t dest_planet);

} // namespace detail

// Public wrapper. Parameter types match the committed prototype (sig_llm_prod_bldg_depart_finalize)
// exactly.
int32_t prod_bldg_depart_finalize(uint16_t player, int32_t building_index, int32_t dest_planet);

namespace detail {
} // namespace detail

} // namespace mh::sim
