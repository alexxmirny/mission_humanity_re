//
// sim/sim_bldg_finish_order.h -- llm_bldg_finish_current_order, the shared "close out this
// building's in-progress order" hook (RI-SIM / SIM1C).
//
// One function: llm_bldg_finish_current_order @0x00470cab (0x40f bytes), batch C layer 2, called
// from 9+ sites in llm_strat_order_queue_dispatch's building-order table (see sim_order_dispatch.h's
// dispatch_calls -- it is one of that table's frontier members, bound to mh::call:: there for the
// same independent-per-function-verification reason every callee in that module is). Applies
// completion side effects for whichever of the four in-progress states the building was in
// (PROD_WORKING producing / RESEARCHING researching / UPGRADING upgrading / CHARGE_STEP charging),
// then falls back to a config-driven idle state and notifies the UI. Does NOT reset the building's
// own timer fields -- callers do that separately.
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// Indirected for the same reason as every other module here: a direct mh::call:: inside a detail::
// body reaches into the live game image, which makes the body untestable by net_selftest.exe simtest.
struct bldg_finish_order_calls {
    void (*unit_apply_production_completion)(uint32_t player, int32_t unit_proto_id);
    void (*ai_notify_unit_lifecycle)(uint16_t player, uint16_t unit_type, uint32_t unit_id,
                                     uint32_t param_4);
    void (*cfg_apply_project_resources)(uint32_t player_id, uint32_t project_index);
    void (*bldg_grant_type_resources)(uint32_t player_idx, int32_t building_type_idx);
    int32_t (*bldg_uses_workers)(uint32_t player, int32_t building_index);
    int32_t (*bldg_unassign_workers)(uint16_t player, uint32_t building_index, uint32_t count);
    void (*bldg_clear_staffed_flag)(uint16_t player, uint32_t building_id);
    void (*bldg_notify_ui)(uint16_t player, uint32_t b_index);
};

const bldg_finish_order_calls &live_bldg_finish_order_calls();

namespace detail {

// llm_bldg_finish_current_order @0x00470cab.
void bldg_finish_current_order(const sim_view &v, sim_store &own, const bldg_finish_order_calls &c,
                               uint32_t player, uint32_t building_index);

} // namespace detail

// Live wrapper: the logic applied to state() and live_bldg_finish_order_calls(). Matches the
// original's committed __watcall(EAX,EDX) shape.
void bldg_finish_current_order(uint32_t player, uint32_t building_index);


} // namespace mh::sim
