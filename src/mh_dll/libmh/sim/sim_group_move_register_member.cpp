//
// sim/sim_group_move_register_member.cpp -- see sim_group_move_register_member.h. Translated from the
// DISASSEMBLY (tmp/decomp_sim/llm_strat_group_move_register_member_0048d16b.asm).
//
#include "sim/sim_group_move_register_member.h"

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace detail {

void group_move_register_member(const sim_view &v, sim_store &own, int32_t player, int32_t unit_idx,
                                int32_t *scratch_count) {
    // Read once -- the original re-dereferences the scratch_count pointer at each of its four scratch-
    // row stores, but nothing changes *scratch_count until the final increment, so the four reads are
    // value-identical to one cached index. 0x0048d18a/0x0048d1d3/0x0048d247/0x0048d26a/0x0048d276.
    const int32_t index = *scratch_count;

    // units[player][unit_idx].x / .y, read once. Nothing in this function writes the unit's x/y, so
    // reusing this pair for both the passable-array indexing (steps 2-3) and the scratch row's
    // tile_col/tile_row (step 4) is value-identical to the original's four separate fresh MOVZX reads
    // of the same two bytes (0x0048d1a9/0x0048d1c3/0x0048d1ff/0x0048d221/0x0048d240/0x0048d263).
    const unit   &u      = unit_of(v, static_cast<uint32_t>(player), unit_idx);
    const int32_t tile_x = static_cast<int32_t>(u.x);
    const int32_t tile_y = static_cast<int32_t>(u.y);

    group_scratch_member &row = own.group_move_scratch_at(index);

    // 0x0048d18d-0x0048d193.
    row.unit_idx = unit_idx;

    // 0x0048d199-0x0048d1d9: the tile's CURRENT passable[] byte, read BEFORE it is overwritten below.
    // passable[x][y] is indexed (x<<8)|y -- the Ghidra draft's `passable[0][CONCAT11(x,y)]` -- the
    // same indexing sim_store::passable_at() already uses.
    row.saved_passable = static_cast<int32_t>(v.passable[(tile_x << 8) | tile_y]);

    // 0x0048d1df-0x0048d22a: lift this member off the collision map -- the group pathfinder must not
    // treat its own members as obstacles -- by writing the unit's own at-placement passable snapshot
    // over the live grid cell. llm_strat_unit_state_group_marshal restores row.saved_passable once the
    // wave is committed (not this function's concern).
    own.passable_at(tile_x, tile_y) = u.origin_tile_was_passable;

    // 0x0048d230-0x0048d270.
    row.tile_col = tile_x;
    row.tile_row = tile_y;

    // 0x0048d276-0x0048d27b.
    *scratch_count = index + 1;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void group_move_register_member(int32_t player, int32_t unit_idx, int32_t *scratch_count) {
    sim_state st = state();
    detail::group_move_register_member(st.read, st.own, player, unit_idx, scratch_count);
}


} // namespace mh::sim
