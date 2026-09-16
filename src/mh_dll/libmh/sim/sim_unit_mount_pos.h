#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// `sprite_meta_entry` and `sim_view::sprite_meta` landed in sim_state.h (conductor, SIM1A sixth
// slice) -- the six previously-unnamed fields this pair reads are now mount1_x/mount1_y/mount2_x/
// mount2_y/submount_x/submount_y (see sim_state.h's comment and the struct's own field comments).

// ---- the outward calls -----------------------------------------------------------------------
//
// Indirected for the same reason as sim_unit_on_destroyed.h's unit_on_destroyed_calls: a direct
// mh::call:: inside a detail:: body reaches into the live game image, which makes the body
// untestable by net_selftest.exe simtest. Both callees are ORIGINAL functions -- present in
// tools/data/sim_migration.json (layer 6, a different batch), so per the translator brief's callee
// rule they stay original rather than being reimplemented here. Shared by both twins: _fine_pos calls
// only fine_axis_pos (twice, once per axis); _render_pos calls fine_axis_pos for X and
// render_fine_y for Y. Signatures copied verbatim from addr/mh_calls.gen.h.
struct mount_pos_calls {
    int32_t (*fine_axis_pos)(uint16_t player, int32_t unit_idx, char axis_is_x);
    uint32_t (*render_fine_y)(uint16_t player, int32_t unit_idx);
};

const mount_pos_calls &live_mount_pos_calls();

namespace detail {

// llm_strat_unit_calc_mount_fine_pos @0x00490841. MASKS its result (bw_mask for X, bh_mask for Y) --
// see HAZARD (b) above. Returns uint per the committed prototype.
uint32_t calc_mount_fine_pos(const sim_view &v, const mount_pos_calls &c, uint16_t player,
                             int32_t unit_idx, uint32_t mount_idx, char axis_is_x);

// llm_strat_unit_calc_mount_render_pos @0x00490b82. Does NOT mask its result -- see HAZARD (b)
// above. Y-axis base is llm_strat_unit_calc_render_fine_y, NOT calc_fine_axis_pos(...,0) -- see
// HAZARD (a) above. Returns int per the committed prototype.
int32_t calc_mount_render_pos(const sim_view &v, const mount_pos_calls &c, uint16_t player,
                              int32_t unit_idx, uint32_t mount_idx, char axis_is_x);

} // namespace detail

// Live wrappers: the logic applied to state().read and live_mount_pos_calls(). Match the originals'
// committed __mh_watcall_ebx_volatile(AX,EDX,EBX,CL) shape.
uint32_t calc_mount_fine_pos(uint16_t player, int32_t unit_idx, uint32_t mount_idx, char axis_is_x);
int32_t  calc_mount_render_pos(uint16_t player, int32_t unit_idx, uint32_t mount_idx, char axis_is_x);

namespace detail {
} // namespace detail

} // namespace mh::sim
