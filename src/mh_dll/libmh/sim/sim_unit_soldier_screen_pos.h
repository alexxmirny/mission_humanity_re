#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The one outward call. Indirected for the same reason as sim_unit_on_destroyed.h /
// sim_unit_update_soldiers.h: a direct mh::call:: inside a detail:: body reaches into the live game
// image, which makes the body untestable by net_selftest.exe simtest. Signature copied verbatim from
// addr/mh_calls.gen.h's own already-committed `llm_strat_unit_calc_interp_pixel_pos`.
struct unit_soldier_get_sprite_screen_pos_calls {
    int32_t (*calc_interp_pixel_pos)(uint16_t player, int32_t unit_idx, char axis_is_x);
};

const unit_soldier_get_sprite_screen_pos_calls &live_unit_soldier_get_sprite_screen_pos_calls();

namespace detail {

// llm_strat_unit_soldier_get_sprite_screen_pos @0x00491086. See the header shape/hazard notes above
// for the full derivation. `*out_x`/`*out_y` are written unconditionally, matching the original
// (no early-return path skips them).
void unit_soldier_get_sprite_screen_pos(const sim_view &v, const unit_soldier_get_sprite_screen_pos_calls &c,
                                        uint16_t player, int32_t unit_idx, int32_t soldier_hop_count,
                                        uint32_t *out_x, uint32_t *out_y);

} // namespace detail

// Live wrapper: the logic applied to state().read and live_unit_soldier_get_sprite_screen_pos_calls().
// Matches the original's committed __mh_watcall_ecx_ebx_volatile(AX,EDX,EBX,ECX,Stack) shape.
void unit_soldier_get_sprite_screen_pos(uint16_t player, int32_t unit_idx, int32_t soldier_hop_count,
                                        uint32_t *out_x, uint32_t *out_y);

namespace detail {
} // namespace detail

} // namespace mh::sim
