#pragma once
#include <cstdint>

#include "sim/sim_state.h"
#include "state/host_api.h"

namespace mh::sim {

struct turret_acquire_target_calls {
    void (*bldg_get_coords)(uint16_t player, int32_t building_index, int32_t *out_x, int32_t *out_y);
    void (*unit_get_coords)(uint16_t player, int32_t unit_index, int32_t *out_x, int32_t *out_y);
    int32_t (*dir_from_to)(int32_t x1, int32_t y1, int32_t x2, int32_t y2);
};

// The real bindings (`mh::call::...`), lazily constructed once.
const turret_acquire_target_calls &live_turret_acquire_target_calls();

namespace detail {

uint32_t turret_acquire_target(const sim_view &v, sim_store &own, const turret_acquire_target_calls &c,
                               uint32_t player, int32_t building_index, uint32_t *out_1,
                               uint32_t *out_2);

} // namespace detail

// Live wrapper: the logic applied to state(). Matches the committed prototype
// (sig_llm_strat_turret_acquire_target, `uint32_t *` out-pointers -- TACT1-P C6, 2026-09-04) exactly.
uint32_t turret_acquire_target(uint32_t player, int32_t building_index, uint32_t *out_1, uint32_t *out_2);

// ---- llm_strat_turret_fire's outward-call seam ------------------------------------------------------
//
// One function pointer per ORIGINAL callee this function reaches, matching the shape every other
// sim/ TU with outward calls uses (e.g. sim_bldg_state_turret.h's own `bldg_state_turret_attack_calls`).
struct turret_fire_calls {
    // 4th param `axis` (CL) added 2026-08-22 -- was missing from the committed Ghidra prototype;
    // axis==1 selects X, axis==0 selects Y (see llm_strat_bldg_get_sprite_anchor_coord's plate).
    uint32_t (*bldg_get_sprite_anchor_coord)(uint32_t player, int32_t b_index, int32_t anchor_kind,
                                             uint8_t axis);
    int32_t (*dir_from_to)(int32_t x1, int32_t y1, int32_t x2, int32_t y2);
    int32_t (*fx_anim_dir_frame_stride)(int32_t start_frame);
    uint32_t (*fx_anim_spawn)(uint32_t x, uint32_t y, uint32_t frame, double game_clock, uint32_t p5);
    void (*weapon_scatter_offset)(int32_t p1, int32_t missing, uint32_t anchor_x, uint32_t anchor_y,
                                  uint32_t x, uint32_t y, int32_t *out_dx, int32_t *out_dy);
    int32_t (*projectile_spawn)(int32_t src_x, int32_t src_y_ground, int32_t src_y_vis_offset,
                                uint32_t dst_x, uint32_t dst_y_ground, int32_t dst_y_vis_offset,
                                int32_t weapon_id, uint8_t owner_player,
                                uint16_t homing_player_and_flags, int32_t homing_target_unit,
                                uint32_t shooter_ref, int32_t shooter_unit_index);
    void (*snd_play_at)(int32_t sound_id, int32_t tile_col, int32_t tile_row); // event record: the
                                                                               // hosted sink runs the
                                                                               // original offscreen_
                                                                               // snd_volume+snd_play
                                                                               // pair at emit
    void (*apply_area_damage)(int32_t x, int32_t y, int32_t target_kind, double damage,
                              int32_t ring_count, uint32_t owner_filter, uint32_t killer_info,
                              int32_t killer_unit_index);
};

// The real bindings (`mh::call::...`), lazily constructed once.
const turret_fire_calls &live_turret_fire_calls();

namespace detail {

// llm_strat_turret_fire @0x0047bf8e. See the header banner above for the full derivation; the .cpp
// carries the per-block address citation.
void turret_fire(const sim_view &v, sim_store &own, const turret_fire_calls &c, uint32_t player,
                 uint32_t bldg_index, int32_t target_fine_x, int32_t target_fine_y,
                 int32_t target_elevation, uint32_t last_tick_time_lo, uint32_t last_tick_time_hi,
                 uint8_t fire_kind);

} // namespace detail

// Live wrapper: the logic applied to state(). Matches the committed prototype
// (sig_llm_strat_turret_fire) exactly.
void turret_fire(uint32_t player, uint32_t bldg_index, int32_t param_3, int32_t param_4,
                 int32_t param_5, uint32_t param_6, uint32_t param_7, uint8_t param_8);

namespace detail {
} // namespace detail

} // namespace mh::sim
