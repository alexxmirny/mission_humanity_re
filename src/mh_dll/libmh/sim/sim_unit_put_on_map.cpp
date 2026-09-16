//
// sim/sim_unit_put_on_map.cpp -- see sim_unit_put_on_map.h. Translated from the DISASSEMBLY
// (tmp/decomp/map_unit_PutOnMap_00486c6a.asm), not from Ghidra's .c: the draft's compound
// `pmVar1->unit_id == 0 && pmVar1.player == 0` empty test names two struct fields that do not
// exist on mh_map_tile_object_data (map_t_unit_full_id is `uint8_t[2]` raw bytes) -- the raw asm
// is a single 16-bit word compare, re-derived here as `head == 0` (see the header HAZARD note).
//
#include "sim/sim_unit_put_on_map.h"

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace {

// tile_object::unit and unit::unit_above are both `uint8_t[2]` in the generated header (the
// struct generator renders map_t_unit_full_id as a raw byte pair, not a scalar) -- this
// reassembles the little-endian word the original addresses with a single `MOV .., word ptr
// [...]`. High nibble (bits 12-15) = owning player, low 12 bits = roster index. Same idiom as
// sim_unit_unlink_tile.cpp's / ai_holding_pen.cpp's / ai_spiral_scan.cpp's own local helpers of
// the same name; not shared across translation units (each TU that needs it defines its own, per
// the "no new shared helpers" rule -- this one is a trivial byte-pair reassembly, not new logic).
inline uint16_t unit_full_id_word(const uint8_t (&packed)[2]) {
    return (uint16_t)(packed[0] | (packed[1] << 8));
}

inline void store_unit_full_id_word(uint8_t (&packed)[2], uint16_t value) {
    packed[0] = (uint8_t)(value & 0xffu);
    packed[1] = (uint8_t)((value >> 8) & 0xffu);
}

} // namespace

namespace detail {

void unit_put_on_map(const sim_view &v, sim_store &own, uint16_t player, uint16_t b_id, uint8_t x,
                     uint8_t y) {
    // ---- self's packed full-id (0x00486c8b-0x00486c96) ------------------------------------------
    // The asm shifts the FULL (unmasked) player/b_id dwords, but only the low 16 bits of the
    // result are ever read back afterward (every use below is a `word ptr` load/store), and a
    // `<<12` only lets the shiftee's low 4 bits reach the low 16-bit half -- so this is
    // value-identical to computing over the already-16-bit `player`/`b_id` parameters. Same
    // precedent as sim_unit_unlink_tile.cpp's `packed_self_id`.
    const uint16_t self_id = (uint16_t)(b_id + (uint32_t)(player << 12));

    // ---- x/y, written unconditionally (0x00486c99-0x00486cd7) -------------------------------------
    // Two separate stores, each its own address computation in the .asm -- not hoisted to one
    // shared reference, matching the original's re-derivation discipline.
    own.unit_at(player, (int32_t)b_id).x = x;
    own.unit_at(player, (int32_t)b_id).y = y;

    // ---- read the tile's current stack head (0x00486cd7-0x00486cf3) -------------------------------
    const uint16_t head = unit_full_id_word(tile_at(v, x, y).unit);

    if (head == 0) {
        // ---- EMPTY tile (0x00486ed4-0x00486ee9): self becomes the base directly ------------------
        store_unit_full_id_word(own.tile_object_at(x, y).unit, self_id);
        return;
    }

    // ---- NOT empty: compare self's elevation against the current base's (0x00486cfe-0x00486d45) ---
    // self's elevation is re-read fresh here (not cached in a loop-spanning local below) --
    // matching the .asm, which recomputes `units[player][b_id].elevation` from scratch at every
    // comparison site (0x00486d39, 0x00486e3f) rather than hoisting it to one shared register
    // across the walk loop. Nothing in this function's own body ever writes `elevation`, so this
    // costs nothing functionally, but the re-read is kept explicit per the translator-brief rule
    // against assuming a view read is stable across a loop.
    const uint32_t head_owner = (uint32_t)((head & 0xf000u) >> 12);
    const uint32_t head_id    = (uint32_t)(head & 0x0fffu);

    if (unit_of(v, player, (int32_t)b_id).elevation <= unit_of(v, head_owner, (int32_t)head_id).elevation) {
        // ---- self elevation <= head's: self becomes the NEW base (0x00486d47-0x00486d97) ---------
        // Write order preserved: unit_above = old head FIRST, then the tile head = self.
        const uint16_t old_head = unit_full_id_word(tile_at(v, x, y).unit);
        store_unit_full_id_word(own.unit_at(player, (int32_t)b_id).unit_above, old_head);
        store_unit_full_id_word(own.tile_object_at(x, y).unit, self_id);
        return;
    }

    // ---- self elevation > head's: walk unit_above links, a do/while re-reading .elevation each
    // iteration, starting at head (0x00486d9c-0x00486e9c) --------------------------------------------
    uint16_t cur = head;
    for (;;) {
        const uint32_t cur_owner = (uint32_t)((cur & 0xf000u) >> 12);
        const uint32_t cur_id    = (uint32_t)(cur & 0x0fffu);
        const uint16_t cur_above = unit_full_id_word(unit_of(v, cur_owner, (int32_t)cur_id).unit_above);

        if (cur_above == 0) {
            // ---- EMPTY-SLOT case (LAB_00486ea3, 0x00486dc1/0x00486dc9): insert right after `cur`
            // and return early. Tested BEFORE any advance, so this also covers `cur == head` on the
            // first iteration. --------------------------------------------------------------------
            store_unit_full_id_word(own.unit_at(cur_owner, (int32_t)cur_id).unit_above, self_id);
            return;
        }

        // ---- save the predecessor (this iteration's `cur`, BEFORE advancing) and step to `next`
        // (0x00486dd2-0x00486e01) ------------------------------------------------------------------
        const uint32_t pred_owner = cur_owner;
        const uint32_t pred_id    = cur_id;
        const uint16_t next       = cur_above;
        const uint32_t next_owner = (uint32_t)((next & 0xf000u) >> 12);
        const uint32_t next_id    = (uint32_t)(next & 0x0fffu);

        if (unit_of(v, player, (int32_t)b_id).elevation > unit_of(v, next_owner, (int32_t)next_id).elevation) {
            // ---- self elevation still greater than `next`'s: loop again with cur := next --------
            cur = next;
            continue;
        }

        // ---- SPLICE (0x00486e4d-0x00486e9c): insert self between predecessor and next. Write
        // order preserved: predecessor.unit_above = self FIRST, then self.unit_above = next. -------
        store_unit_full_id_word(own.unit_at(pred_owner, (int32_t)pred_id).unit_above, self_id);
        store_unit_full_id_word(own.unit_at(player, (int32_t)b_id).unit_above, next);
        return;
    }
}

} // namespace detail

// ---- the public wrapper ----------------------------------------------------------------------

void unit_put_on_map(uint16_t player, uint16_t b_id, uint8_t x, uint8_t y) {
    sim_state st = state();
    detail::unit_put_on_map(st.read, st.own, player, b_id, x, y);
}


} // namespace mh::sim
