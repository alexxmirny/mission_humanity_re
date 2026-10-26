#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// [net] soldier_origin_anim_fix (mp:DSHEAL) -- the shot origin must not depend on the soldier's anim_change_count.
// That byte (+0x1b) is written by the RENDERERS only (llm_strat_render_tile_object / _render_bldg_docked_unit), so
// what each peer's screen drew leaked through the sprite index -> mount offsets -> llm_strat_unit_fire_weapon's shot
// origin -> llm_strat_weapon_scatter_offset's distance-derived bound (0 or 2 extra RNG draws) and, on projectile weapons,
// llm_strat_projectile_spawn. With the key on the sprite-frame sum is `unit.move_microstep` alone, i.e. exactly what the
// retail body computes with the counter read as 0 (the retail carrier is `XOR EDX,EDX` over the 7-byte MOVZX at
// 0x00491166, mh/seams/combat_proto.cpp). Initialiser = off (stock); set from reimpl_fixes by set_fixes.
namespace soldier_origin_anim_detail {
inline bool &flag() {
    static bool on = false;
    return on;
}
} // namespace soldier_origin_anim_detail
inline void set_soldier_origin_anim_fix(bool on) { soldier_origin_anim_detail::flag() = on; }
inline bool soldier_origin_anim_fix() { return soldier_origin_anim_detail::flag(); }

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
