#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// ---- llm_strat_bldg_construction_complete's callees ---------------------------------------------------
struct bldg_construction_complete_calls {
    void (*bldg_completion_dispatch)(uint32_t param_1, uint32_t param_2, uint32_t param_3,
                                     uint32_t param_4, double param_5);
    void (*bldg_start_special_anim)(uint16_t player, int32_t b_index, uint32_t unused_ebx,
                                    uint32_t unused_ecx, double timestamp);
    void (*bldg_anim_state_trigger)(uint32_t param_1, int32_t param_2, uint32_t param_3, uint32_t param_4,
                                    uint32_t param_5, uint32_t param_6);
    void (*bldg_set_staffed_flag)(uint16_t player, int32_t building_index);
    void (*refresh_building)(uint16_t p_id, int32_t b_id);
};

const bldg_construction_complete_calls &live_bldg_construction_complete_calls();

namespace detail {

// llm_strat_bldg_construction_complete @0x00478d70. Committed prototype: void(uint32_t player,
// uint32_t building_index, uint32_t param_3, uint32_t param_4).
void bldg_construction_complete(const sim_view &v, sim_store &own, const bldg_construction_complete_calls &c,
                                uint32_t player, uint32_t building_index, uint32_t param_3,
                                uint32_t param_4);

} // namespace detail

// Live wrapper: the logic applied to state() and live_bldg_construction_complete_calls(). Matches the
// original's committed __watcall(EAX,EDX,EBX,ECX) shape.
void bldg_construction_complete(uint32_t player, uint32_t building_index, uint32_t param_3,
                                uint32_t param_4);

namespace detail {
} // namespace detail

} // namespace mh::sim
