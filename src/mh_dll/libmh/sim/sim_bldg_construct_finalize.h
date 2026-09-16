#pragma once
#include <cstdint>

#include "sim/sim_order_enqueue.h" // BUILDING_TYPE_A_*/H_* -- pinned there (SIM1C), reused rather
                                   // than re-declared (would be a C2374/C2086 redefinition otherwise).
#include "sim/sim_state.h"

namespace mh::sim {

// The four outward calls this closure reaches, indirected for offline testability -- same reason
// sim_bldg_defense_cost.h / sim_unit_bldg_energy_refill_full.h indirect their own committed-original
// callees. Member function-pointer types match each callee's OWN committed prototype in
// addr/mh_calls.gen.h exactly.
struct bldg_construct_finalize_calls {
    // map_CreateBuilding @0x004622cb -- the roster-slot writer itself.
    void (*create_building)(uint16_t player, uint32_t index, uint32_t x_b, int32_t y_b,
                            int32_t building_id, uint32_t param_6, uint32_t sub_id);
    // llm_strat_bldg_link_to_network_if_adjacent @0x0049232e -- ALSO in this migration batch (a
    // sibling unit), reached through mh::call:: per the header banner above.
    void (*link_to_network_if_adjacent)(uint16_t player, uint32_t index);
    // llm_strat_bldg_notify_state_change @0x00470c5c.
    void (*notify_state_change)(uint16_t player, uint32_t building_id);
    // llm_strat_ai_notify_bldg_constructed @0x004daec2 -- called on every path (success AND both
    // failure paths), with a different `param_3`/`param_6` pair each time (see the banner above).
    void (*ai_notify_bldg_constructed)(uint32_t player, uint32_t x_b, uint32_t param_3,
                                       uint32_t building_id, uint32_t y_b, uint32_t param_6);
};

const bldg_construct_finalize_calls &live_bldg_construct_finalize_calls();

namespace detail {

// llm_bldg_construct_finalize @0x00462e66. See the header banner above for the full derivation; the
// .cpp carries the per-branch address citation. Pure reader of sim state -- no `sim_store &own`
// (the roster write happens inside map_CreateBuilding, an original callee).
int32_t bldg_construct_finalize(const sim_view &v, const bldg_construct_finalize_calls &c,
                                uint32_t param_1, int32_t y_b, uint16_t player, char param_4,
                                uint32_t x_b, uint32_t building_id);

} // namespace detail

// Public wrapper. Parameter types match the committed prototype (sig_llm_bldg_construct_finalize)
// exactly: int(undefined4 param_1, int y_b, ushort player, char param_4, undefined4 x_b, undefined4
// building_id).
int32_t bldg_construct_finalize(uint32_t param_1, int32_t y_b, uint16_t player, char param_4,
                                uint32_t x_b, uint32_t building_id);

namespace detail {
} // namespace detail

} // namespace mh::sim
