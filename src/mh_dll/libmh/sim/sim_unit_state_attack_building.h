#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// GROUP_MARSHAL (0x0a) / ATTACK_BUILDING (0x1c) `order`/`state` literals -- see the header banner above
// (same values as sim_unit_state_move_walker.h's MOVE_WALKER_STATE_GROUP_MARSHAL/_ATTACK_BUILDING, this
// TU's own local copy per the established per-TU-constant convention).
inline constexpr uint16_t ATTACK_BUILDING_ORDER_GROUP_MARSHAL   = 0x0a;
inline constexpr uint16_t ATTACK_BUILDING_STATE_ATTACK_BUILDING = 0x1c;

// llm_strat_unit_notify_status's status-code literal this function passes on the re-plan path -- see
// the header banner (same value as move_walker's MOVE_WALKER_NOTIFY_REPLAN, this TU's own local copy).
inline constexpr uint32_t ATTACK_BUILDING_NOTIFY_REPLAN = 1;

// ---- the outward calls -----------------------------------------------------------------------
//
// Indirected for the same reason as every other sim/ TU: a direct mh::call:: inside a detail:: body
// reaches into the live game image, making the body untestable by net_selftest.exe simtest. All eight
// are ORIGINAL functions outside this batch -- none need reimplementing here (in particular
// llm_strat_unit_fire_weapon and llm_strat_unit_select_weapon already have their OWN reimplementations
// elsewhere in this tree, but per the translator brief's "callees stay original" rule this TU still
// calls the real function through mh::call::, not the sibling detail:: version).
struct unit_state_attack_building_calls {
    void (*unit_get_coords)(uint16_t player, int32_t unit_index, int32_t *out_fine_x,
                            int32_t *out_fine_y);                                     // @0x0044b141
    int32_t (*dir_from_to)(int32_t x1, int32_t y1, int32_t x2, int32_t y2);           // @0x0049482b
    void (*target_release_ref)(uint32_t player_idx, int32_t unit_idx, uint32_t mode); // @0x004dac44
    void (*unit_set_state_order)(uint16_t new_order, uint16_t new_state);             // @0x00486657
    uint8_t (*unit_select_weapon)(uint16_t player, int32_t unit_index,
                                  uint32_t target_mask); // @0x0048ba9e
    void (*bldg_get_coords)(uint16_t player, int32_t building_index, int32_t *out_fine_x,
                            int32_t *out_fine_y); // @0x00449b8a
    void (*unit_notify_status)(uint32_t player, int32_t unit_index,
                               uint32_t status_code); // @0x004dae0e
    void (*unit_fire_weapon)(uint32_t player, uint32_t unit_index, uint8_t weapon_slot_select,
                             uint32_t target_ref, uint16_t target_index, int32_t target_fine_x,
                             int32_t target_fine_y); // @0x0048bb6c
};

const unit_state_attack_building_calls &live_unit_state_attack_building_calls();

namespace detail {

// llm_strat_unit_state_attack_building @0x00484e10. See the header derivation above for the full shape.
// Zero-arg, ambient cur_player/cur_index/cur_unit, matching the original's void(void) signature.
void unit_state_attack_building(const sim_view &v, sim_store &own,
                                const unit_state_attack_building_calls &c);

} // namespace detail

// Live wrapper: the logic applied to state() and live_unit_state_attack_building_calls(). Matches the
// committed prototype (sig_llm_strat_unit_state_attack_building) exactly.
void unit_state_attack_building();

namespace detail {
} // namespace detail

} // namespace mh::sim
