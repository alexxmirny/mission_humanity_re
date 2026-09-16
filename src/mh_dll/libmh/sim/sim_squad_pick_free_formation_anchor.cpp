//
// sim/sim_squad_pick_free_formation_anchor.cpp -- see sim_squad_pick_free_formation_anchor.h.
// Translated from the DISASSEMBLY
// (tmp/decomp_sim/llm_strat_squad_pick_free_formation_anchor_004899ed.asm), not from Ghidra's C
// draft (the draft is a faithful transcription too, but the .asm is the checked spec).
//
#include "sim/sim_squad_pick_free_formation_anchor.h"

namespace mh::sim {
namespace detail {

void squad_pick_free_formation_anchor(const sim_view &v, int32_t existing_count, char *out_x, char *out_y) {
    // 0x00489a0c: loop counter seeded to 1, not 0 -- row 0 of the table is never a candidate.
    // 0x00489a13-0x00489a19: `slot <= 5` keeps the loop going.
    for (int32_t slot = 1; slot <= 5; ++slot) {
        // 0x00489a2d-0x00489a44: the original computes 0xae3640 + slot*0x30 (x at +0, y at +4,
        // plain byte loads stored as `char`, matching cVar1/cVar2). REBASED 2026-09-02 (LT0): the
        // view now binds the WHOLE placement table @0xae3618, so the old implicit +0x28 interior
        // base (cell row0/col5) is explicit -- this loop reads the fixed col-5 column, row = slot.
        const std::size_t base   = 0x28 + static_cast<std::size_t>(slot) * 0x30;
        char              cand_x = static_cast<char>(v.squad_placement_offset_table[base + 0]);
        char              cand_y = static_cast<char>(v.squad_placement_offset_table[base + 4]);

        // 0x00489a4e-0x00489a91: scan the in-use list [0 .. existing_count). MOVSX at
        // 0x00489a66/0x00489a78 sign-extends the byte candidate to int32 before the dword compare
        // against the scratch entry's x/y -- so the comparison is signed-byte-widened-to-int32.
        bool in_use = false;
        for (int32_t i = 0; i < existing_count; ++i) {
            const squad_formation_anchor_scratch &entry = v.squad_anchor_scratch[i];
            if (static_cast<int32_t>(cand_x) == entry.x && static_cast<int32_t>(cand_y) == entry.y) {
                in_use = true;
                break;
            }
        }

        if (!in_use) {
            // 0x00489a97-0x00489aa5: raw byte stores through the caller-owned out pointers.
            *out_x = cand_x;
            *out_y = cand_y;
            return;
        }
    }
    // 0x00489a13/0x00489a17->0x00489aae: all 5 slots were in use -- fall out WITHOUT ever writing
    // through out_x/out_y. This is the original's exact behaviour, not an omission: preserve it.
}

} // namespace detail

void squad_pick_free_formation_anchor(int32_t existing_count, char *out_x, char *out_y) {
    const sim_view v = state().read;
    detail::squad_pick_free_formation_anchor(v, existing_count, out_x, out_y);
}

} // namespace mh::sim
