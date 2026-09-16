#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// ---- the outward calls -----------------------------------------------------------------------
//
// Indirected for the same reason as every other sim/ TU: a direct mh::call:: inside a detail:: body
// reaches into the live game image, making the body untestable by net_selftest.exe simtest. All ten are
// ORIGINAL functions outside this batch -- none need reimplementing here; live_unit_state_attack_unit_
// calls() is the only binder. Signatures copied verbatim from addr/mh_calls.gen.h. Out-params carry
// the committed mh::call:: pointee type exactly (TACT1-P C6, 2026-09-04).
struct unit_state_attack_unit_calls {
    void (*target_release_ref)(uint32_t player_idx, int32_t unit_idx, uint32_t mode); // @0x004dac44
    void (*unit_notify_status)(uint32_t player, int32_t unit_index,
                               uint32_t status_code); // @0x004dae0e
    void (*unit_order_move_auto)(uint16_t player, int32_t unit_idx, uint32_t x,
                                 uint32_t y);                                   // @0x0046a56d
    void (*unit_set_state_order)(uint16_t new_order, uint16_t new_state);       // @0x00486657
    int32_t (*target_class)(uint32_t owner_and_kind_flag, int32_t roster_slot); // @0x004495f9
    uint32_t (*unit_in_weapon_range)(int32_t player, int32_t unit_idx, int32_t tile_x, int32_t tile_y,
                                     int32_t target_class_flags); // @0x0044965d
    void (*unit_get_coords)(uint16_t player, int32_t unit_index, int32_t *out_x,
                            int32_t *out_y);                                // @0x0044b141
    int32_t (*dir_from_to)(int32_t x1, int32_t y1, int32_t x2, int32_t y2); // @0x0049482b
    uint8_t (*unit_select_weapon)(uint16_t player, int32_t unit_index,
                                  uint32_t target_mask); // @0x0048ba9e
    void (*unit_fire_weapon)(uint32_t player, uint32_t unit_index, uint8_t weapon_slot,
                             uint32_t target_ref, uint16_t target_index, int32_t target_fine_x,
                             int32_t target_fine_y); // @0x0048bb6c
};

const unit_state_attack_unit_calls &live_unit_state_attack_unit_calls();

namespace detail {

// llm_strat_unit_state_attack_unit @0x00485008. See the header derivation above for the full shape.
// Zero-arg, ambient cur_player/cur_index/cur_unit, matching the original's void(void) signature.
void unit_state_attack_unit(const sim_view &v, sim_store &own, const unit_state_attack_unit_calls &c);

} // namespace detail

// Live wrapper: the logic applied to state() and live_unit_state_attack_unit_calls(). Matches the
// committed prototype (sig_llm_strat_unit_state_attack_unit) exactly.
void unit_state_attack_unit();

namespace detail {
} // namespace detail

} // namespace mh::sim
