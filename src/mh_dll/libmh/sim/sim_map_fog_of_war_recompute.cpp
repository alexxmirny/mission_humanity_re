//
// sim/sim_map_fog_of_war_recompute.cpp -- see sim_map_fog_of_war_recompute.h. Translated from the
// DISASSEMBLY (tmp/decomp/llm_map_fog_of_war_recompute_00428b11.asm), not the Ghidra .c draft's
// `flags.f` 16-bit rendering -- see the header banner for the byte-level `flags[1]` derivation.
//
#include "sim/sim_map_fog_of_war_recompute.h"

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace detail {

void fog_of_war_recompute(const sim_view &v, sim_store &own) {
    // ---- pass (1) RESET (0x00428b29-0x00428bd9) ----------------------------------------------------
    for (int32_t tile_x = 0; tile_x < 256; ++tile_x) {
        for (int32_t tile_y = 0; tile_y < 256; ++tile_y) {
            tile_object &t = own.tile_object_at(tile_x, tile_y);
            t.visibility   = 0;                                                // 0x00428b71
            t.flags[1]     = static_cast<uint8_t>((t.flags[1] & 0x3f) | 0x40); // 0x00428b8d/ba
        }
    }

    // ---- pass (2) PER-HUMAN-PLAYER VISIBILITY (0x00428bd9-0x00428c90) ------------------------------
    for (uint32_t player = 0; player < 8; ++player) {
        const uint8_t bit = static_cast<uint8_t>(1u << (player & 0x1fu));
        if ((*v.is_human & static_cast<uint32_t>(bit)) == 0) continue; // 0x00428bfd
        for (int32_t tile_x = 0; tile_x < 256; ++tile_x) {
            for (int32_t tile_y = 0; tile_y < 256; ++tile_y) {
                if (own.fog_visible_by_count_at(tile_x, tile_y, player) != 0) { // 0x00428c4f
                    tile_object &t = own.tile_object_at(tile_x, tile_y);
                    t.visibility   = static_cast<uint8_t>(t.visibility | bit); // 0x00428c6d/81
                }
            }
        }
    }

    // ---- pass (3) DISCOVERED -> KNOWN/FOGGED/VISIBLE (0x00428c90-0x00428d94) -----------------------
    for (int32_t tile_x = 0; tile_x < 256; ++tile_x) {
        for (int32_t tile_y = 0; tile_y < 256; ++tile_y) {
            const uint8_t discovered = own.fog_discovered_at(tile_x, tile_y);     // 0x00428cd3
            if ((*v.is_human & static_cast<uint32_t>(discovered)) == 0) continue; // 0x00428cda

            tile_object &t = own.tile_object_at(tile_x, tile_y);
            t.flags[1]     = static_cast<uint8_t>(t.flags[1] | 0xc0); // 0x00428cfb, UNCONDITIONAL once
                                                                      // the discovered check passes --
                                                                      // see the header banner.

            if ((*v.is_human & static_cast<uint32_t>(t.visibility)) != 0) {    // 0x00428d28, re-read fresh
                t.flags[1] = static_cast<uint8_t>((t.flags[1] & 0xbf) | 0x80); // 0x00428d45/72
            }
        }
    }
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void fog_of_war_recompute() {
    sim_state st = state();
    detail::fog_of_war_recompute(st.read, st.own);
}


} // namespace mh::sim
