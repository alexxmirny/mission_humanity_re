//
// sim/sim_unit_facing24_delta.cpp -- see sim_unit_facing24_delta.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_facing24_to_delta_0049610f.asm), not from Ghidra's .c: the draft's nested
// if/else shape reads correctly and was CONFIRMED (not merely trusted) by walking every CMP/JC/JBE/JZ
// target in the raw listing branch-by-branch -- see the per-case address citations below. The eight
// on-axis codes recovered this way (1,4,7,10,13,16,19,22) all happen to satisfy `facing24 % 3 == 1`,
// but that is NOT how the original computes them (see the header's hazard note) and is NOT how this
// translation computes them either: every branch below tests the SAME literal the asm compares
// against, never a derived modulus, and nothing above 22 is tested at all (so it falls to default).
//
#include "sim/sim_unit_facing24_delta.h"

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace detail {

void facing24_to_delta(uint32_t facing24, int32_t *out_dx, int32_t *out_dy) {
    // The original's own opening two-step unsigned-compare idiom (CMP x,K; JC lo; CMP x,K; JBE eq) is
    // Watcom's way of splitting "x<K / x==K / x>K" from ONE register compare; each pair below
    // reproduces it as plain `<` / `==` on the uint32_t parameter -- unsigned comparisons throughout,
    // matching JC/JBE/JZ (never the signed JL/JLE/JG).

    // ---- facing24 < 10 (0x0049612e-0x00496132 -> LAB_0049617d) --------------------------------------
    if (facing24 < 10) {
        // ---- facing24 < 4 (0x0049617d-0x00496181 -> LAB_0049619c) -----------------------------------
        if (facing24 < 4) {
            // ---- facing24 == 1 (0x0049619c-0x004961a0 -> LAB_00496200) -----------------------------
            if (facing24 == 1) {
                *out_dx = 0;
                *out_dy = 1;
                return;
            }
            // facing24 == 0, 2, or 3: falls through to the trailing default (0x004961a2 -> LAB_00496250).
        } else {
            // facing24 in [4, 10).
            // ---- facing24 == 4, i.e. facing24 < 5 given facing24 >= 4 (0x00496183-0x00496187 ->
            // LAB_00496214) ---------------------------------------------------------------------------
            if (facing24 < 5) {
                *out_dx = -1;
                *out_dy = 1;
                return;
            }
            // ---- facing24 == 7 (0x0049618d-0x00496191 -> LAB_00496228) -----------------------------
            if (facing24 == 7) {
                *out_dx = -1;
                *out_dy = 0;
                return;
            }
            // facing24 == 5, 6, 8, or 9: falls through to the trailing default (0x00496197 -> LAB_00496250).
        }
    } else {
        // facing24 >= 10.
        // ---- facing24 == 10, i.e. facing24 < 11 given facing24 >= 10 (0x00496134-0x00496138 ->
        // LAB_0049623c) -------------------------------------------------------------------------------
        if (facing24 < 11) {
            *out_dx = -1;
            *out_dy = -1;
            return;
        }
        // facing24 >= 11.
        // ---- facing24 < 16 (0x0049613e-0x00496142 -> LAB_00496172) -------------------------------
        if (facing24 < 16) {
            // ---- facing24 == 13 (0x00496172-0x00496176 -> LAB_004961a7) ---------------------------
            if (facing24 == 13) {
                *out_dx = 0;
                *out_dy = -1;
                return;
            }
            // facing24 == 11, 12, 14, or 15: falls through to the trailing default (0x00496178 -> LAB_00496250).
        } else {
            // facing24 >= 16.
            // ---- facing24 == 16, i.e. facing24 < 17 given facing24 >= 16 (0x00496144-0x00496148 ->
            // LAB_004961be) -----------------------------------------------------------------------
            if (facing24 < 17) {
                *out_dx = 1;
                *out_dy = -1;
                return;
            }
            // facing24 >= 17. Original: CMP facing24,19; JC -> default (covers 17,18); CMP facing24,19;
            // JBE -> ==19; else CMP facing24,22; JZ -> ==22; else default. Reproduced below as one
            // guarded block -- facing24 == 17 or 18 (facing24 < 19) skips the block entirely and falls
            // to the trailing default, matching the asm's own JC-to-LAB_0049616d-to-LAB_00496250 path.
            if (facing24 >= 19) {
                // ---- facing24 == 19, i.e. facing24 < 20 given facing24 >= 19 (0x00496154-0x00496158
                // -> LAB_004961d5) ---------------------------------------------------------------
                if (facing24 < 20) {
                    *out_dx = 1;
                    *out_dy = 0;
                    return;
                }
                // ---- facing24 == 22 (0x0049615e-0x00496162 -> LAB_004961ec) ---------------------
                if (facing24 == 22) {
                    *out_dx = 1;
                    *out_dy = 1;
                    return;
                }
                // facing24 == 20, 21, or >= 23: falls through to the trailing default (0x00496168).
            }
        }
    }

    // LAB_00496250 (0x00496250-0x0049625c): the shared default for every facing24 not matched above --
    // reached from seven distinct fallthrough/JMP sites in the asm, and (since no branch anywhere
    // bounds facing24 from above except the explicit `== 22` test) from every value >= 23 too,
    // including ones that would satisfy `facing24 % 3 == 1` -- see the header's hazard note.
    *out_dx = 0;
    *out_dy = 0;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void facing24_to_delta(uint32_t facing24, int32_t *out_dx, int32_t *out_dy) {
    detail::facing24_to_delta(facing24, out_dx, out_dy);
}


} // namespace mh::sim
