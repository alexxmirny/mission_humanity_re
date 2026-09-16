#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// ---- the outward calls ---------------------------------------------------------------------------
//
// Indirected for the same reason as every other sim/ TU: a direct mh::call:: inside a detail:: body
// reaches into the live game image, which makes the body untestable by net_selftest.exe simtest.
struct projectile_tick_calls {
    void (*map_wrapped_delta)(int32_t src_x, int32_t src_y, int32_t dst_x, int32_t dst_y, double *out_dx,
                              double *out_dy);                                     // llm_strat_map_wrapped_delta @0x004945de
    int32_t (*map_wrap_delta_row)(int32_t x1, int32_t y1, int32_t x2, int32_t y2); // @0x0049446e
    double (*sqrt_fn)(double x);                                                   // llm_sqrt @0x004da9c0
    void (*unit_get_coords)(uint16_t player, int32_t unit_index, int32_t *out_x,
                            int32_t *out_y); // @0x0044b141
    uint32_t (*fx_anim_spawn)(uint32_t x, uint32_t y, uint32_t anim_id, double elapsed,
                              uint32_t owner_or_flag); // @0x00464855
    void (*snd_play_at)(int32_t sound_id, int32_t tile_col,
                        int32_t tile_row);                                  // event record: the hosted sink runs the original
                                                                            // offscreen_snd_volume+snd_play pair at emit
    int32_t (*dir_from_to)(int32_t x1, int32_t y1, int32_t x2, int32_t y2); // @0x0049482b
    int32_t (*fx_anim_dir_frame_stride)(int32_t start_frame);               // @0x0046177b
    void (*apply_area_damage)(int32_t x, int32_t y, int32_t target_kind, double damage, int32_t ring_count,
                              uint32_t owner_filter_zeroed, uint32_t killer_info,
                              int32_t killer_unit_index); // @0x0044c3fb
};

const projectile_tick_calls &live_projectile_tick_calls();

namespace detail {

// llm_strat_projectile_tick @0x00440e1c. See the header banner above for the full derivation; the
// .cpp carries the per-block address citation. No parameters -- operates on `v`/`own`'s ambient
// cur_projectile alone, matching the original's void(void) signature.
void projectile_tick(const sim_view &v, sim_store &own, const projectile_tick_calls &c);

} // namespace detail

// Live wrapper: the logic applied to state() and live_projectile_tick_calls(). Matches the committed
// prototype (sig_llm_strat_projectile_tick) exactly.
void projectile_tick();

namespace detail {
} // namespace detail

} // namespace mh::sim
