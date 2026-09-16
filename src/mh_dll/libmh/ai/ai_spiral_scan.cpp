//
// ai/ai_spiral_scan.cpp -- see ai_spiral_scan.h. Translated from the DISASSEMBLY
// (tmp/decomp_a2/llm_strat_ai_scan_spiral_ring_for_engage_candidates_004ed495.asm), not from
// Ghidra's C: the decompile mistypes `target_mask` as a byte-width contribution in places and hides
// the packed map::t::unit_full_id chain walk behind an opaque `map__t__unit_full_id` cast -- neither
// is wrong, exactly, but both are re-derived from the raw bytes below rather than trusted.
//
#include "ai/ai_spiral_scan.h"


namespace mh::ai {
namespace {

// tile_object::unit and unit::unit_above are both `uint8_t[2]` in the generated header (the
// generator renders map::t::unit_full_id as a raw byte pair, not a scalar) -- this reassembles the
// little-endian word the original addresses with a single `MOVZX reg, word ptr [...]`. High nibble =
// owning player, low 12 bits = roster index.
inline uint16_t unit_full_id_word(const uint8_t (&packed)[2]) {
    return (uint16_t)(packed[0] | (packed[1] << 8));
}

} // namespace

namespace detail {

int32_t scan_spiral_ring_for_engage_candidates(const ai_view &v, const ai_calls &gc, int32_t player,
                                               int32_t x, int32_t y, int32_t ring_index,
                                               uint32_t target_mask) {
    int32_t        added      = 0;
    const uint32_t cell_count = v.spiral_ring_cell_counts[ring_index];

    // UNSIGNED compare against the count (`CMP EAX,[count] / JC`), as everywhere else in this module.
    for (uint32_t i = 0; i < cell_count; ++i) {
        const spiral_offset &off = v.spiral_offsets[i];
        const int32_t        tx  = (int32_t)(((uint32_t)(x + off.dx)) & *v.map_width_mask);
        const int32_t        ty  = (int32_t)(((uint32_t)(y + off.dy)) & *v.map_height_mask);

        const tile_object &tile      = tile_at(v, tx, ty);
        const uint8_t      fog_count = fog_visible_count_at(v, tx, ty, (uint32_t)player);

        // ---- OFFER 1: the BUILDING at this tile ---------------------------------------------
        //
        // `TEST dword ptr [EBP+8],EAX` -- target_mask tested as a FULL DWORD against the tile's
        // class-owner high nibble (0-0xf0, so the result is identical to a byte-width test, but the
        // width is preserved here because the .asm's width is the spec).
        if (tile.building != 0) {
            const uint32_t class_hi = (uint32_t)tile.class_owner & 0xf0u;
            if ((target_mask & class_hi) != 0) {
                const uint32_t owner = (uint32_t)tile.class_owner & 0x0fu;
                // `CMP ...,-1 / JG skip` -- SIGN test, hostile only.
                if (v.players[player].ai_player_relation[owner] <= -1) {
                    if (fog_count != 0) {
                        gc.engage_candidate_add((uint32_t)tile.class_owner, (int32_t)tile.building);
                        ++added;
                    }
                }
            }
        }

        // ---- OFFER 2: the CHAIN of units stacked on this tile --------------------------------
        //
        // `TEST byte ptr [EBP+8],0x20` -- target_mask tested as a BYTE this time (functionally the
        // same result as a dword test here, since 0x20 lives entirely in the low byte).
        if ((target_mask & 0x20u) != 0 && fog_count != 0) {
            uint16_t link = unit_full_id_word(tile.unit);
            while (link != 0) {
                // Owner extracted from bits 12-15 here -- NOT the `& 0xf` low-nibble extraction offer
                // 1 uses over class_owner; this is a different packed field (the unit chain link, not
                // the tile's class_owner byte).
                const uint32_t owner = (uint32_t)((link & 0xf000u) >> 12);
                const uint32_t index = (uint32_t)(link & 0x0fffu);
                if (v.players[player].ai_player_relation[owner] <= -1) {
                    // 0x20 is OR'd into the ref here -- offer 1's building ref carries no extra bits.
                    gc.engage_candidate_add(owner | 0x20u, (int32_t)index);
                    ++added;
                }
                link = unit_full_id_word(unit_of(v, owner, index).unit_above);
            }
        }
    }

    return added;
}

} // namespace detail

int32_t scan_spiral_ring_for_engage_candidates(int32_t player, int32_t x, int32_t y,
                                               int32_t ring_index, uint32_t target_mask) {
    const ai_state st = state();
    return detail::scan_spiral_ring_for_engage_candidates(st.read, live_calls(), player, x, y,
                                                          ring_index, target_mask);
}


} // namespace mh::ai
