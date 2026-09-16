#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// ---- llm_strat_bldg_state_dismantling's callees ---------------------------------------------------
struct bldg_state_dismantling_calls {
    void (*bldg_notify_ui)(uint16_t player, uint32_t b_index);
};

const bldg_state_dismantling_calls &live_bldg_state_dismantling_calls();

// ---- llm_strat_bldg_state_dismantle_finish's callees ------------------------------------------------
struct bldg_state_dismantle_finish_calls {
    void (*ai_notify_object_removed)(uint32_t flags, uint32_t object_index, int32_t hard_remove);
    void (*bldg_refund_resources_scaled_by_energy)(int32_t player, int32_t building_index);
    void (*prod_unbind_planet)(int32_t player, int32_t planet_slot);
    void (*prod_shuttle_slot_release)(int32_t player, int32_t slot);
    uint32_t (*player_presence_lost)(uint32_t player, uint32_t mode);
    void (*bldg_unmap_footprint)(uint16_t player, int32_t building_index);
    uint32_t (*sight_add_circle)(uint32_t player, int32_t x, int32_t y, int32_t building_id, uint8_t sight);
    uint32_t (*game_SetEvent)(uint32_t type);
    void (*bldg_notify_ui)(uint16_t player, uint32_t b_index);
};

const bldg_state_dismantle_finish_calls &live_bldg_state_dismantle_finish_calls();

namespace detail {

// llm_strat_bldg_state_dismantling @0x00472eb7. No parameters (void(void), committed prototype).
void bldg_state_dismantling(const sim_view &v, sim_store &own, const bldg_state_dismantling_calls &c);

// llm_strat_bldg_state_dismantle_finish @0x004736a2. No parameters (void(void), committed prototype).
void bldg_state_dismantle_finish(const sim_view &v, sim_store &own,
                                 const bldg_state_dismantle_finish_calls &c);

} // namespace detail

// Live wrappers: the logic applied to state() and each function's own live_*_calls(). Match the
// originals' committed prototypes exactly (addr/mh_export.gen.h's sig_llm_strat_bldg_state_*).
void bldg_state_dismantling();
void bldg_state_dismantle_finish();

namespace detail {
} // namespace detail

} // namespace mh::sim
