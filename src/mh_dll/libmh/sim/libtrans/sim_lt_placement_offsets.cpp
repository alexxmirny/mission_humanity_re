//
// sim/libtrans/sim_lt_placement_offsets.cpp -- see sim_lt_placement_offsets.h. Translated from the
// DISASSEMBLY (tmp/decomp_lib_trans/llm_ui_cursor_apply_anim_frame_offset_00486a8a.asm,
// tmp/decomp_lib_trans/llm_ui_cursor_lookup_offset_pair_00486b17.asm), not from the Ghidra .c
// drafts beside them.
//
#include "sim/libtrans/sim_lt_placement_offsets.h"

namespace mh::sim {
namespace detail {

void facing_step_apply(const sim_view &v, char *out_fine_x, char *out_fine_y, int32_t facing) {
    // 0x00486aac-0x00486ab5: index = facing*8 (SHL EAX,0x3) into the .dx field (@+0) of
    // _G_LLM_STRAT_FACING_STEP_SIGN. No bounds check on `facing` -- reproduce the absence.
    const int32_t dx = v.facing_step_offset[facing].dx;
    // 0x00486ab8-0x00486adc: EXACT-EQUALITY test, not a sign test (see the header). Any value
    // outside {-1, 0, +1} falls through untouched -- facing==0 is a no-op by construction, since
    // slot 0 of the table is never written by the boot init loop.
    if (dx == -1) {
        *out_fine_x -= 0x20; // 0x00486ad1: byte-width SUB, wraps mod 256 -- do not widen to int
    } else if (dx == 1) {
        *out_fine_x += 0x20; // 0x00486ad9: byte-width ADD, wraps mod 256 -- do not widen to int
    }

    // 0x00486adf-0x00486ae8: the .dy field (@+4), same table, same index -- computed and applied
    // strictly AFTER the dx block above (the original's two blocks are sequential, not
    // interleaved; the two out-pointers are distinct at all three live call sites, but the order
    // is the original's contract).
    const int32_t dy = v.facing_step_offset[facing].dy;
    if (dy == -1) {
        *out_fine_y -= 0x20; // 0x00486b04
    } else if (dy == 1) {
        *out_fine_y += 0x20; // 0x00486b0c... (see .asm: LAB_00486b01/LAB_00486b09)
    }
}

void squad_placement_offset_lookup(const sim_view &v, int32_t soldier_count, int32_t slot, char *out_a,
                                   char *out_b) {
    // 0x00486b38-0x00486b64: base = slot*0x30 (row) + soldier_count*8 (col) into
    // _G_LLM_STRAT_SQUAD_PLACEMENT_OFFSET_TABLE; cell field +0 -> out_a, cell field +4 -> out_b.
    // Both fields are read as a single BYTE (`MOV DL, byte ptr [...]`) -- the upper 3 bytes of the
    // underlying 4-byte cell field are discarded, never read-and-truncated. No bounds check on
    // either index (the table is 6x6, 288 B); callers are trusted to stay inside it, exactly as
    // the original is.
    const std::size_t base = static_cast<std::size_t>(slot) * 0x30 + static_cast<std::size_t>(soldier_count) * 8;
    *out_a                 = static_cast<char>(v.squad_placement_offset_table[base + 0]);
    *out_b                 = static_cast<char>(v.squad_placement_offset_table[base + 4]);
}

} // namespace detail

void facing_step_apply(char *out_fine_x, char *out_fine_y, int32_t facing) {
    const sim_view v = state().read;
    detail::facing_step_apply(v, out_fine_x, out_fine_y, facing);
}

void squad_placement_offset_lookup(int32_t soldier_count, int32_t slot, char *out_a, char *out_b) {
    const sim_view v = state().read;
    detail::squad_placement_offset_lookup(v, soldier_count, slot, out_a, out_b);
}

} // namespace mh::sim
