#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// _G_LLM_STRAT_FX_ANIMS[10000]'s per-half element count -- see fx_anim_spawn's derivation above.
// Own-file constant, same precedent as sim_order_dispatch_bldg.cpp's PRODUCTION_QUEUE_CAP /
// sim_unit_recruit.h's RECRUIT_*_CAP (a domain constant local to the TU(s) that need it, not hoisted
// into sim_state.h until a second consumer needs it too).
inline constexpr int32_t FX_ANIM_POOL_CAP = 10000;

// ---- the outward calls ---------------------------------------------------------------------------

// weapon_scatter_offset's two callees, factored out on their own so projectile_spawn_calls below can
// embed the SAME two members for its intra-unit forwarding call (see the .cpp) without a second,
// divergent binding of the identical two functions.
struct weapon_scatter_offset_calls {
    void (*pixel_delta_wrapped)(int32_t src_x, int32_t src_y, int32_t dst_x, int32_t dst_y,
                                int32_t *out_dx,
                                int32_t *out_dy); // llm_strat_pixel_delta_wrapped @0x004944f6
    int32_t (*rand_below)(int32_t upper_bound);   // llm_rand_below @0x00499f49
};

const weapon_scatter_offset_calls &live_weapon_scatter_offset_calls();

struct projectile_spawn_calls {
    // Forwarded into the intra-unit call to weapon_scatter_offset (see the .cpp) -- same two
    // callees as weapon_scatter_offset_calls above, duplicated as struct members (not a nested
    // struct) so this remains a flat, easily-mocked list like every other `..._calls` struct in
    // this tree.
    void (*pixel_delta_wrapped)(int32_t src_x, int32_t src_y, int32_t dst_x, int32_t dst_y,
                                int32_t *out_dx, int32_t *out_dy);
    int32_t (*rand_below)(int32_t upper_bound);

    int32_t (*dir_from_to)(int32_t x1, int32_t y1, int32_t x2, int32_t y2);   // @0x0049482b
    int32_t (*dir_sector_to)(int32_t x0, int32_t y0, int32_t x1, int32_t y1); // @0x004948ff
    void (*map_wrapped_delta)(int32_t src_x, int32_t src_y, int32_t dst_x, int32_t dst_y,
                              double *out_dx, double *out_dy);                     // @0x004945de
    int32_t (*map_wrap_delta_row)(int32_t x1, int32_t y1, int32_t x2, int32_t y2); // @0x0049446e
    double (*sqrt_fn)(double x);                                                   // llm_sqrt @0x004da9c0
};

const projectile_spawn_calls &live_projectile_spawn_calls();

namespace detail {

// llm_fx_anim_dir_frame_stride @0x0046177b. See the header banner above.
int32_t fx_anim_dir_frame_stride(const sim_view &v, int32_t start_frame);

// llm_strat_fx_anim_spawn @0x00464855. See the header banner above. No outward calls, no `v` needed
// (every touched region is read+written through `own`).
uint32_t fx_anim_spawn(sim_store &own, uint32_t x, uint32_t y, uint32_t anim_frame_id, double elapsed,
                       uint32_t layer);

// llm_strat_weapon_scatter_offset @0x0048c6b2. See the header banner above. No sim state at all --
// pure computation over its parameters and the two outward calls in `c`.
void weapon_scatter_offset(int32_t shooter_experience_or_zero, int32_t weapon_missing_scale,
                           int32_t src_x, int32_t src_y, int32_t dst_x, int32_t dst_y,
                           int32_t *out_scatter_x, int32_t *out_scatter_y,
                           const weapon_scatter_offset_calls &c);

// llm_strat_projectile_spawn @0x00464932. See the header banner above for the full derivation.
int32_t projectile_spawn(const sim_view &v, sim_store &own, const projectile_spawn_calls &c,
                         int32_t src_x, int32_t src_y_ground, int32_t src_y_vis_offset, uint32_t dst_x,
                         uint32_t dst_y_ground, int32_t dst_y_vis_offset, int32_t weapon_id,
                         uint8_t owner_player, uint16_t homing_player_and_flags,
                         int32_t homing_target_unit, uint32_t shooter_ref, int32_t shooter_unit_index);

} // namespace detail

// ---- the public surface, in original-behaviour terms -----------------------------------------
// Parameter types match the committed prototypes in addr/mh_export.gen.h exactly (the drift gate
// enforces this on export/shadow installation).

int32_t  fx_anim_dir_frame_stride(int32_t start_frame);
uint32_t fx_anim_spawn(uint32_t x, uint32_t y, uint32_t anim_frame_id, double elapsed, uint32_t layer);
void     weapon_scatter_offset(int32_t param_1, int32_t param_2, uint32_t a2, uint32_t param_4,
                               uint32_t param_5, uint32_t param_6, int32_t *param_7, int32_t *param_8);
int32_t  projectile_spawn(int32_t src_x, int32_t src_y_ground, int32_t src_y_vis_offset, uint32_t dst_x,
                          uint32_t dst_y_ground, int32_t dst_y_vis_offset, int32_t weapon_id,
                          uint8_t owner_player, uint16_t homing_player_and_flags,
                          int32_t homing_target_unit, uint32_t shooter_ref, int32_t shooter_unit_index);

namespace detail {
} // namespace detail

} // namespace mh::sim
