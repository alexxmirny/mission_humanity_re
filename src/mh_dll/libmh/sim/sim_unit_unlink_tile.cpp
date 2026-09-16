//
// sim/sim_unit_unlink_tile.cpp -- see sim_unit_unlink_tile.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_unit_unlink_tile_00486f41.asm), not from Ghidra's C: the draft's trailing
// `->unit_id = 0` / `.player = 0` pair is a decompiler artifact of the single 16-bit `unit_above`
// clear (see the header HAZARD note) -- re-derived from the raw store's field offset rather than
// trusted.
//
#include "sim/sim_unit_unlink_tile.h"

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace {

// tile_object::unit and unit::unit_above are both `uint8_t[2]` in the generated header (the struct
// generator renders map_t_unit_full_id as a raw byte pair, not a scalar) -- this reassembles the
// little-endian word the original addresses with a single `MOV .., word ptr [...]`. High nibble
// (bits 12-15) = owning player, low 12 bits = roster index. Same idiom as
// sim_unit_purge_unregistered.cpp's / ai_holding_pen.cpp's / ai_spiral_scan.cpp's own local helpers
// of the same name; not shared across translation units (each TU that needs it defines its own, per
// the "no new shared helpers" rule -- this one is a trivial byte-pair reassembly, not new logic).
inline uint16_t unit_full_id_word(const uint8_t (&packed)[2]) {
    return (uint16_t)(packed[0] | (packed[1] << 8));
}

} // namespace

namespace detail {

void unit_unlink_tile(const sim_view &v, sim_store &own, uint32_t unit_player, uint16_t unit_index) {
    // 0x00486f5e/0x00486f7e/0x00486fcd/0x004870b0: every roster-index computation in this function
    // masks unit_player to its low 16 bits (MOVZX word ptr) before using it as a row index.
    const uint32_t player = unit_player & 0xffffu;

    // ---- snapshot x/y (0x00486f5e-0x00486f9b) --------------------------------------------------
    const int32_t tile_x = unit_of(v, player, (int32_t)unit_index).x;
    const int32_t tile_y = unit_of(v, player, (int32_t)unit_index).y;

    // ---- this unit's own packed full-id (0x00486f9e-0x00486fa9) --------------------------------
    // The asm shifts the FULL (unmasked) unit_player dword rather than the masked word, but only the
    // low 16 bits of the result are ever read back afterward (every use below is a `word ptr` load),
    // and (unit_player << 12) truncated to 16 bits depends only on unit_player's low 4 bits either
    // way -- so this is value-identical to masking first.
    const uint16_t packed_self_id = (uint16_t)(unit_index + (unit_player << 12));

    // ---- is this unit the tile's stack HEAD? (0x00486fac-0x00486fcb) -----------------------------
    const uint16_t head = unit_full_id_word(tile_at(v, tile_x, tile_y).unit);

    if (head == packed_self_id) {
        // ---- HEAD case (0x00486fcd-0x00486fff): repoint the tile straight at our own unit_above --
        const unit &self                           = unit_of(v, player, (int32_t)unit_index);
        own.tile_object_at(tile_x, tile_y).unit[0] = self.unit_above[0];
        own.tile_object_at(tile_x, tile_y).unit[1] = self.unit_above[1];
    } else {
        // ---- MID-CHAIN case (0x00487004-0x004870a9): walk unit_above links, starting at `head`,
        // until the node whose OWN unit_above names US -- that node is the predecessor -- then splice
        // it to skip over us. The predecessor can belong to ANY player (the tile's stacked-unit list
        // is per-tile, not per-player): cur_owner/cur_id are read off the chain, not assumed to be
        // `player`. --------------------------------------------------------------------------------
        uint16_t cur       = head;
        uint32_t cur_owner = 0;
        uint32_t cur_id    = 0;
        for (;;) {
            cur_owner                = (uint32_t)((cur & 0xf000u) >> 12);
            cur_id                   = (uint32_t)(cur & 0x0fffu);
            const uint16_t cur_above = unit_full_id_word(unit_of(v, cur_owner, (int32_t)cur_id).unit_above);
            if (cur_above == packed_self_id) break; // `cur` (this iteration's node) is the predecessor
            cur = cur_above;
        }
        // cur_owner/cur_id are the predecessor's own identity, already derived at the top of the
        // iteration that just broke -- the asm re-derives the identical values a second time off the
        // same still-unchanged local at LAB_00487067 rather than caching across the branch (a
        // value-identical difference, same precedent sim_unit_ctrl_group.cpp's `old_group_id`
        // documents), so they are reused here instead of recomputed.
        const unit &self                                      = unit_of(v, player, (int32_t)unit_index);
        own.unit_at(cur_owner, (int32_t)cur_id).unit_above[0] = self.unit_above[0];
        own.unit_at(cur_owner, (int32_t)cur_id).unit_above[1] = self.unit_above[1];
    }

    // ---- ALWAYS, regardless of which branch above ran: clear our OWN unit_above (0x004870b0-
    // 0x004870c6) -- a DIFFERENT write pair from the tile_objects/predecessor splice above. See the
    // header HAZARD note: this is the field the exported .c mis-renders as `->unit_id = 0` /
    // `.player = 0` (fields that do not exist on mh_map_object_unit); the real single 16-bit store
    // clears units[player][unit_index].unit_above (both bytes), matching sim_unit_remove_from_map.cpp's
    // own byte-wise clear of the identical field. -------------------------------------------------
    own.unit_at(player, (int32_t)unit_index).unit_above[0] = 0;
    own.unit_at(player, (int32_t)unit_index).unit_above[1] = 0;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void unit_unlink_tile(uint32_t unit_player, uint16_t unit_index) {
    sim_state st = state();
    detail::unit_unlink_tile(st.read, st.own, unit_player, unit_index);
}


} // namespace mh::sim
