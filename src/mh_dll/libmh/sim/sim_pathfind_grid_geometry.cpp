//
// sim/sim_pathfind_grid_geometry.cpp -- see sim_pathfind_grid_geometry.h. Translated from the
// DISASSEMBLY, not from Ghidra's C drafts:
//   tmp/decomp_sim/llm_strat_dir_step_factor_00449b28.asm
//   tmp/decomp_sim/llm_strat_tile_neighbor_in_dir_0048b294.asm
//   tmp/decomp_sim/llm_strat_heading_candidate_find_slot_0048d2f4.asm
//
#include "sim/sim_pathfind_grid_geometry.h"

namespace mh::sim {

namespace detail {

double dir_step_factor(int32_t dir) {
    // 0x00449b43-0x00449b5d: four CMP/JZ|JNZ checks feeding one shared branch target -- dir in
    // {0x10, 0x16, 0x4, 0xa} all fall through to the same "1.4" store; anything else takes the "else"
    // store. Both stores are read back with a single 8-byte FLD (0x00449b7d), so this is one double
    // constant each, not two 32-bit halves: 0x66666666/0x3ff66666 (low/high dword, little-endian) is
    // the double bit pattern 0x3FF6666666666666 == 1.4 exactly; 0x00000000/0x3ff00000 is
    // 0x3FF0000000000000 == 1.0 exactly.
    if (dir == 0x10 || dir == 0x16 || dir == 0x4 || dir == 0xa) return 1.4;
    return 1.0;
}

void tile_neighbor_in_dir(const sim_view &v, int32_t x, int32_t y, int32_t dir, int32_t *out_col,
                          int32_t *out_row) {
    // 0x0048b2b5-0x0048b2c1: `dir_remap_table[dir].step_primary` -- SHL 4 is the 16-byte row stride
    // (mh_llm_strat_dir_remap_row), the loaded dword is the struct's first field (`.step_primary`,
    // offset 0). SHL 3 immediately after is the 8-byte stride into dir_step_offsets (mh_llm_vec2i).
    const int32_t          step_primary = v.dir_remap_table[dir].step_primary;
    const dir_step_offset &step         = v.dir_step_offsets[step_primary];

    // 0x0048b2c4-0x0048b2d8: *out_col = (x + step.dx) & general.width_mask. SAME step_primary index
    // as the row computation below -- one lookup, both axes.
    *out_col = (int32_t)((uint32_t)(x + step.dx) & map_width_mask(v));

    // 0x0048b2da-0x0048b2fd: *out_row = (y + step.dy) & general.height_mask, re-deriving step_primary
    // a second time (the original recomputes the same index rather than caching it -- value-for-value
    // identical, reproduced once here per house rule 4 since this TU introduces no new helper).
    *out_row = (int32_t)((uint32_t)(y + step.dy) & map_height_mask(v));
}

int32_t heading_candidate_find_slot(const sim_view &v, int32_t heading, int32_t turn_delta) {
    // 0x0048d318-0x0048d35a: `IMUL EDX,heading,0x30` (heading*48) + `i<<4` (i*16) is
    // heading_candidates[heading*3 + i] at the 16-byte mh_llm_strat_heading_slot stride (3 slots per
    // heading, matching sim_state.h's own comment). Loop while i<3 AND the slot's turn_delta is NOT
    // the -1 sentinel (`CMP ...,-1 / JG` is signed "> -1", i.e. ">= 0"; the .c draft's unsigned
    // "< 0x80000000" reads the identical bit pattern). First match returns its slot index; hitting
    // either bound (i==3, or the sentinel) falls through to -1.
    for (int32_t i = 0; i < 3; ++i) {
        const heading_slot &slot = v.heading_candidates[heading * 3 + i];
        if (slot.turn_delta < 0) break; // -1 sentinel: inactive/end of candidates for this heading
        if (slot.turn_delta == turn_delta) return i;
    }
    return -1;
}

} // namespace detail

// ---- the public wrappers --------------------------------------------------------------------------

double dir_step_factor(int32_t dir) { return detail::dir_step_factor(dir); }

void tile_neighbor_in_dir(int32_t x, int32_t y, int32_t dir, int32_t *out_col, int32_t *out_row) {
    const sim_view v = state().read;
    detail::tile_neighbor_in_dir(v, x, y, dir, out_col, out_row);
}

int32_t heading_candidate_find_slot(int32_t heading, int32_t turn_delta) {
    const sim_view v = state().read;
    return detail::heading_candidate_find_slot(v, heading, turn_delta);
}


} // namespace mh::sim
