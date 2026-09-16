//
// sim/sim_unit_hover_tile_crowded.cpp -- see sim_unit_hover_tile_crowded.h. Translated from the
// DISASSEMBLY (tmp/decomp_sim/llm_strat_unit_hover_tile_crowded_0048cf3e.asm), not from the Ghidra
// .c draft.
//
#include "sim/sim_unit_hover_tile_crowded.h"

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace {

// llm_strat_unit_state literal for this field -- NOT backed by a real Ghidra enum (see the header
// banner). Own local copy per this codebase's established "no new shared helpers" convention;
// matches sim_unit_state_hover_engage.cpp's identically-valued constant of the same name.
inline constexpr uint16_t UNIT_STATE_HOVER_ENGAGE = 0x2e; // 0x0048cff8

// tile_object::unit and unit::unit_above are both `uint8_t[2]` in the generated header (the
// generator renders map::t::unit_full_id as a raw byte pair, not a scalar) -- reassembles the
// little-endian word the original addresses with a single `MOVZX/MOV reg, word ptr [...]`. Same
// idiom as sim_combat_kill_credit.cpp's / sim_unit_state_move_walker.cpp's local helper of the same
// name; not shared across translation units (each TU that needs it defines its own).
inline uint16_t unit_full_id_word(const uint8_t (&packed)[2]) {
    return static_cast<uint16_t>(packed[0] | (packed[1] << 8));
}

} // namespace

namespace detail {

int32_t unit_hover_tile_crowded(const sim_view &v, int32_t player, uint32_t unit_index) {
    // 0x0048cf55-0x0048cfa4: chain head = tile_objects[self.x][self.y].unit, self = units[player][unit_index].
    const unit &self = unit_of(v, static_cast<uint32_t>(player), static_cast<int32_t>(unit_index));
    uint16_t    link = unit_full_id_word(tile_at(v, self.x, self.y).unit);

    // 0x0048cfa7-0x0048d03c: walk the tile's unit stack via each unit's OWN `.unit_above` link.
    // `link == 0` (a single 16-bit compare, not two subfield tests) is the sentinel/tail.
    while (link != 0) {
        const uint32_t owner      = static_cast<uint32_t>(link >> 12) & 0xfu;
        const uint32_t link_index = static_cast<uint32_t>(link) & 0x0fffu;

        // 0x0048cfb2-0x0048d000: skip the HOVER_ENGAGE test entirely when this chain entry IS the
        // querying unit itself (owner AND index both match); otherwise, any OTHER unit sitting in
        // HOVER_ENGAGE ends the walk with "crowded".
        if ((owner != static_cast<uint32_t>(player) || link_index != unit_index) &&
            unit_of(v, owner, static_cast<int32_t>(link_index)).state == UNIT_STATE_HOVER_ENGAGE) {
            return 1;
        }

        // 0x0048d00d-0x0048d03c: advance to the next entry in the chain.
        link = unit_full_id_word(unit_of(v, owner, static_cast<int32_t>(link_index)).unit_above);
    }
    return 0;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

int32_t unit_hover_tile_crowded(int32_t player, uint32_t unit_index) {
    const sim_view v = state().read;
    return detail::unit_hover_tile_crowded(v, player, unit_index);
}


} // namespace mh::sim
