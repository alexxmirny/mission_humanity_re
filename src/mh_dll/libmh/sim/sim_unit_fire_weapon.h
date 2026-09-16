#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// ---- the outward calls --------------------------------------------------------------------------
//
// TWO groups (both indirected through this one struct, for the SAME offline-reachability reason):
//   (a) the five ORIGINAL callees -- dist_out_of_range/dir_from_to/rand_below/offscreen_snd_volume/
//       snd_play -- called via mh::call:: in the live binding (live_unit_fire_weapon_calls()).
//   (b) the eight ALREADY-REIMPLEMENTED SIBLINGS the fire-mode switch arms drive
//       (calc_mount_fine_pos/calc_mount_render_pos/soldier_screen_pos in the mount/soldier recompute,
//       and projectile_spawn/fx_anim_dir_frame_stride/fx_anim_spawn/weapon_scatter_offset/
//       apply_area_damage in the case bodies). The header banner's "called as sibling functions
//       directly, NOT via calls" note DESCRIBED THE ORIGINAL SHAPE; it was superseded 2026-08-19 (the
//       SIM1E fire_weapon T3->T1 slice). RATIONALE: each of these is a state()-using PUBLIC WRAPPER
//       that dispatches into its own live _calls table of raw mh::call:: VAs (0x0048c83f / 0x0048ccb4 /
//       0x0048ce21 for the mount/soldier trio; the projectile/fx/scatter/area wrappers likewise), which
//       net_selftest.exe never maps -- so a DIRECT sibling call ACCESS-VIOLATES the offline test process
//       the instant any switch arm is entered, and the whole switch (weapon types 1/2/3/4/5/6/8) was
//       therefore un-coverable by simtest, capping the hub at T3. Routing them through this struct,
//       bound in live_unit_fire_weapon_calls() to the SAME public wrappers, is PRODUCTION
//       BEHAVIOUR-IDENTICAL (an indirect call to the identical function, the invasion_calls precedent)
//       and lets the offline oracle STUB them and exercise every arm. bldg_get_coords stays a direct
//       sibling call: it is PURE (no outward VA call) and reached only on the building-target range-check
//       path, which the oracle covers by region-rebasing instead of stubbing.
struct unit_fire_weapon_calls {
    // (a) ORIGINAL callees -- mh::call:: in the live binding.
    uint32_t (*dist_out_of_range)(int32_t range_min, int32_t range_max, int32_t x, int32_t y,
                                  int32_t target_tile_x, int32_t target_tile_y); // llm_strat_dist_out_of_range @0x00449ac1
    int32_t (*dir_from_to)(int32_t x1, int32_t y1, int32_t x2, int32_t y2);      // @0x0049482b
    int32_t (*rand_below)(int32_t upper_bound);                                  // llm_rand_below @0x00499f49
    void (*snd_play_at)(int32_t sound_id, int32_t tile_col,
                        int32_t tile_row); // event record: the hosted sink runs the original
                                           // offscreen_snd_volume+snd_play pair at emit
    // (b) reimplemented siblings -- bound to their own public wrappers in the live binding (see above).
    uint32_t (*calc_mount_fine_pos)(uint16_t player, int32_t unit_idx, uint32_t mount_idx,
                                    char axis_is_x); // mh::sim::calc_mount_fine_pos @0x00490841
    int32_t (*calc_mount_render_pos)(uint16_t player, int32_t unit_idx, uint32_t mount_idx,
                                     char axis_is_x); // mh::sim::calc_mount_render_pos @0x00490b82
    void (*soldier_screen_pos)(uint16_t player, int32_t unit_idx, int32_t soldier_hop_count,
                               uint32_t *out_x, uint32_t *out_y); // mh::sim::unit_soldier_get_sprite_screen_pos @0x00491086
    int32_t (*projectile_spawn)(int32_t src_x, int32_t src_y_ground, int32_t src_y_vis_offset,
                                uint32_t dst_x, uint32_t dst_y_ground, int32_t dst_y_vis_offset,
                                int32_t weapon_id, uint8_t owner_player, uint16_t homing_player_and_flags,
                                int32_t homing_target_unit, uint32_t shooter_ref,
                                int32_t shooter_unit_index);  // mh::sim::projectile_spawn @0x00464932
    int32_t (*fx_anim_dir_frame_stride)(int32_t start_frame); // mh::sim::fx_anim_dir_frame_stride @0x0046177b
    uint32_t (*fx_anim_spawn)(uint32_t x, uint32_t y, uint32_t anim_frame_id, double elapsed,
                              uint32_t layer); // mh::sim::fx_anim_spawn @0x00464855
    void (*weapon_scatter_offset)(int32_t param_1, int32_t param_2, uint32_t a2, uint32_t param_4,
                                  uint32_t param_5, uint32_t param_6, int32_t *param_7,
                                  int32_t *param_8); // mh::sim::weapon_scatter_offset @0x0048c6b2
    void (*apply_area_damage)(int32_t x, int32_t y, int32_t target_kind, double damage,
                              int32_t ring_count, uint32_t owner_filter_zeroed, uint32_t killer_info,
                              int32_t killer_unit_index); // mh::sim::apply_area_damage @0x0044c3fb
};

const unit_fire_weapon_calls &live_unit_fire_weapon_calls();

namespace detail {

// llm_strat_unit_fire_weapon @0x0048bb6c. See the header banner above for the full derivation.
void unit_fire_weapon(const sim_view &v, sim_store &own, const unit_fire_weapon_calls &c,
                      uint32_t player, uint32_t unit_index, uint8_t weapon_slot_select,
                      uint32_t target_ref, uint16_t target_index, int32_t target_fine_x,
                      int32_t target_fine_y);

} // namespace detail

// Live wrapper: the logic applied to state() and live_unit_fire_weapon_calls(). Matches the committed
// prototype (sig_llm_strat_unit_fire_weapon) exactly.
void unit_fire_weapon(uint32_t player, uint32_t unit_index, uint8_t weapon_slot_select,
                      uint32_t target_ref, uint16_t target_index, int32_t target_fine_x,
                      int32_t target_fine_y);

namespace detail {
} // namespace detail

} // namespace mh::sim
