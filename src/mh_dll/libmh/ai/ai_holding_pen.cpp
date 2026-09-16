//
// ai/ai_holding_pen.cpp -- see ai_holding_pen.h. Translated from the DISASSEMBLY
// (tmp/decomp_a2/llm_strat_ai_holding_pen_scan_targets_004ec35f.asm), not from Ghidra's C: the
// decompile's `CONCAT31(extraout_var,bVar4)` masks a plain byte-return call
// (llm_strat_unit_get_sight) as an undefined-upper-bytes concatenation, and its `mVar3`
// (`map__t__unit_full_id`) hides the same packed-word chain walk ai_spiral_scan.cpp's OFFER 2
// re-derives from raw bytes. Neither is wrong, exactly, but both are re-derived from the assembly
// below rather than trusted.
//
#include "ai/ai_holding_pen.h"


namespace mh::ai {
namespace {

// tile_object::unit and unit::unit_above are both `uint8_t[2]` in the generated header (the
// generator renders map::t::unit_full_id as a raw byte pair, not a scalar) -- this reassembles the
// little-endian word the original addresses with a single `MOVZX reg, word ptr [...]`. High nibble =
// owning player, low 12 bits = roster index. Same idiom as ai_spiral_scan.cpp's local helper of the
// same name; not shared across translation units (each TU that needs it defines its own, per the
// "no new shared helpers" rule -- this one is a trivial byte-pair reassembly, not new logic).
inline uint16_t unit_full_id_word(const uint8_t (&packed)[2]) {
    return (uint16_t)(packed[0] | (packed[1] << 8));
}

} // namespace

namespace detail {

int32_t holding_pen_scan_targets(const ai_view &v, const ai_calls &gc, int32_t player,
                                 uint32_t pen_index) {
    int32_t added = 0;

    // `player_data[player].ai_groups[pen_index].head_unit` -- verified by exact absolute-address
    // arithmetic against mh_structs.gen.h: the .asm's `player*0x288fc + pen_index*0xa66 + 0xe7e432`
    // is playerdata_base(0xe6dec0) + ai_groups(0x10568) + pen_index*sizeof(ai_groups[0])(0xa66) +
    // head_unit(0xa). No bounds check on pen_index in the original; none added here.
    uint32_t unit_index = v.players[player].ai_groups[pen_index].head_unit;

    while (unit_index != 0) {
        // `CMP EAX,EBX / JBE` -- UNSIGNED compare (the layer-1 sibling
        // unit_scan_engage_candidates_in_range uses a SIGNED JLE for the identical-shaped max here;
        // this site's opcode is JBE). Both callees return small non-negative values in this game, so
        // the two forms cannot actually disagree, but the width/signedness is transcribed as read
        // rather than unified with the sibling.
        const int32_t sight        = (int32_t)gc.unit_get_sight((uint32_t)player, (int32_t)unit_index);
        const int32_t weapon_range = (int32_t)gc.unit_max_weapon_range((uint32_t)player, (int32_t)unit_index);
        const int32_t radius       = (weapon_range > sight) ? weapon_range : sight;

        // The scan anchor is THIS unit's own (x, y) -- unit.x / unit.y, bytes at +0x84 / +0x85,
        // matching the .asm's `+0xdd8ccc` / `+0xdd8ccd` loads (units_base 0xdd8c48 + 0x84 / +0x85).
        const unit   &anchor   = unit_of(v, (uint32_t)player, (int32_t)unit_index);
        const uint8_t anchor_x = anchor.x;
        const uint8_t anchor_y = anchor.y;

        const uint32_t cell_count = v.spiral_ring_cell_counts[radius];
        // UNSIGNED compare against cell_count (`CMP EAX,[EBP-0x20] / JC`), as everywhere else in this
        // module.
        for (uint32_t i = 0; i < cell_count; ++i) {
            const spiral_offset &off = v.spiral_offsets[i];
            const int32_t        tx  = (int32_t)(((uint32_t)((int32_t)anchor_x + off.dx)) & *v.map_width_mask);
            const int32_t        ty  = (int32_t)(((uint32_t)((int32_t)anchor_y + off.dy)) & *v.map_height_mask);

            const tile_object &tile = tile_at(v, tx, ty);

            // ---- the tile's BUILDING, if any -------------------------------------------------
            //
            // `TEST byte ptr [ECX+0x6],0xc0` gates on the tile being occupied at all (no target_mask
            // and, unlike ai_spiral_scan.cpp's OFFER 1, NO fog-of-war test -- there is no such
            // instruction between the building check and the scan_target_list_add call in the .asm).
            if (tile.building != 0 && ((uint32_t)tile.class_owner & 0xc0u) != 0) {
                const uint32_t owner = (uint32_t)tile.class_owner & 0x0fu;
                // `CMP ...,-1 / JG skip` -- SIGN test, hostile only.
                if (v.players[player].ai_player_relation[owner] <= -1) {
                    // Positional register mapping (EAX=player, EDX=target_id, EBX=target_owner, per
                    // detail::s_void_EAX_EDX_EBX): target_id = the tile's FULL class_owner byte
                    // (owner nibble + class bits, not masked), target_owner = the building id.
                    gc.scan_target_list_add(player, (int32_t)tile.class_owner, (int32_t)tile.building);
                    ++added;
                }
            }

            // ---- every UNIT stacked on this tile ----------------------------------------------
            //
            // Walked via the tile's packed `unit` chain head and each unit's OWN `unit_above` link
            // (offset 0x0, NOT `ai_group_next` at 0xd4 -- verified against the .asm's
            // `+0xdd8c48`-relative load with no added field offset), identically to
            // ai_spiral_scan.cpp's OFFER 2. No target_mask gate here (this function has none) and no
            // fog-of-war test, same as the building offer above.
            uint16_t link = unit_full_id_word(tile.unit);
            while (link != 0) {
                const uint32_t link_owner = (uint32_t)((link & 0xf000u) >> 12);
                const uint32_t link_index = (uint32_t)(link & 0x0fffu);
                if (v.players[player].ai_player_relation[link_owner] <= -1) {
                    // target_id = owner | 0x20 (0x20 OR'd in here, unlike the building offer above),
                    // target_owner = the roster index.
                    gc.scan_target_list_add(player, (int32_t)(link_owner | 0x20u), (int32_t)link_index);
                    ++added;
                }
                link = unit_full_id_word(unit_of(v, link_owner, (int32_t)link_index).unit_above);
            }
        }

        // Next member of the holding pen's own chain: THIS unit's ai_group_next (offset 0xd4),
        // distinct from the unit_above chain walked just above.
        unit_index = unit_of(v, (uint32_t)player, (int32_t)unit_index).ai_group_next;
    }

    return added;
}

} // namespace detail

int32_t holding_pen_scan_targets(int32_t player, uint32_t pen_index) {
    const ai_state st = state();
    return detail::holding_pen_scan_targets(st.read, live_calls(), player, pen_index);
}


} // namespace mh::ai
