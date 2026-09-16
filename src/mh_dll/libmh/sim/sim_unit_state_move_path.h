#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// ---- the outward calls -----------------------------------------------------------------------
//
// Indirected for the same reason as every other sim/ TU: a direct mh::call:: inside a detail:: body
// reaches into the live game image, making the body untestable by net_selftest.exe simtest. All
// twelve are ORIGINAL functions outside this batch. Signatures copied verbatim from
// addr/mh_calls.gen.h (checked against this function's own register-order use in the .asm). Out-params
// carry the committed mh::call:: pointee type exactly (TACT1-P C6, 2026-09-04).
struct unit_state_move_path_calls {
    int32_t (*unit_chase_check)();                               // llm_strat_unit_chase_check @0x004866fb
    void (*path_free_slot)(uint16_t player, int32_t unit_index); // @0x004969e8
    void (*unit_notify_status)(uint32_t player, int32_t unit_index,
                               uint32_t status_code); // @0x004dae0e
    void (*unit_set_state)(uint16_t new_state);       // @0x004866c9
    // VERIFIED (STATE, ORDER) order, NOT (ORDER, STATE) -- see the header banner's parameter-order
    // note above. param_1 writes unit.state(+0x6); param_2 writes unit.order(+0x4).
    void (*unit_set_state_order)(uint16_t new_state, uint16_t new_order);       // @0x00486657
    int32_t (*target_class)(uint32_t owner_and_kind_flag, int32_t roster_slot); // @0x004495f9
    uint32_t (*unit_in_weapon_range)(int32_t player, int32_t unit_idx, int32_t tile_x, int32_t tile_y,
                                     int32_t target_class_flags); // @0x0044965d
    void (*storage_get_approach_tile)(uint16_t player, uint16_t unit_index, uint32_t *out_x,
                                      uint32_t *out_y, uint32_t storage_idx);              // @0x0048b37c
    void (*unit_unlink_tile)(uint32_t unit_player, uint16_t unit_index);                   // @0x00486f41
    void (*fow_remove_sight)(uint32_t player, int32_t x, int32_t y, uint8_t radius);       // @0x00496868
    void (*map_unit_PutOnMap)(uint16_t player, uint16_t unit_index, uint8_t x, uint8_t y); // @0x00486c6a
    void (*map_fow_UpdateFoWPlus)(uint32_t player, uint32_t x, uint32_t y, uint8_t sight); // @0x0049681a
};

const unit_state_move_path_calls &live_unit_state_move_path_calls();

namespace detail {

// llm_strat_unit_state_move_path @0x00480b53. See the header derivation above for the full shape.
// Zero-arg, ambient cur_player/cur_index/cur_unit, matching the original's void(void) signature.
void unit_state_move_path(const sim_view &v, sim_store &own, const unit_state_move_path_calls &c);

} // namespace detail

// Live wrapper: the logic applied to state() and live_unit_state_move_path_calls(). Matches the
// committed prototype (sig_llm_strat_unit_state_move_path) exactly.
void unit_state_move_path();

namespace detail {
} // namespace detail

} // namespace mh::sim
