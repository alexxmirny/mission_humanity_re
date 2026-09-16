//
// ai/ai_group_centroid.cpp -- see ai_group_centroid.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_ai_group_compute_centroid_004d5c17.asm), not from Ghidra's C: the .c draft
// is actually accurate here (its own plate documents both original quirks correctly), but every
// literal below was re-derived against the listing rather than transcribed from it, per house rules.
//
#include "ai/ai_group_centroid.h"


namespace mh::ai {
namespace detail {

void group_compute_centroid(const ai_view &v, const ai_calls &gc, int32_t player, int32_t group_index,
                            uint32_t *out_x, uint32_t *out_y) {
    const player_data &pd  = v.players[player];
    const unit_group  &grp = pd.ai_groups[group_index];

    // EMPTY GROUP: return the player's home tile, not a computed centroid and not (0,0).
    // 0x004d5c51-0x004d5c6b.
    if (grp.member_count == 0) {
        *out_x = (uint32_t)pd.ai_home_tile_x;
        *out_y = (uint32_t)pd.ai_home_tile_y;
        return;
    }

    // The HEAD unit's (x, y) is the origin every member's delta is taken relative to, snapshotted
    // ONCE before the walk (0x004d5c93-0x004d5ca7) -- not re-read per member.
    const uint16_t head_unit_idx = grp.head_unit;
    const unit    &head          = unit_of(v, (uint32_t)player, (int32_t)head_unit_idx);
    const int32_t  head_x        = head.x;
    const int32_t  head_y        = head.y;

    int32_t sum_dx = 0;
    int32_t sum_dy = 0;

    // The chain walk: ai_group_next (unit record +0xd4), terminating at index 0. Starts at the head
    // unit itself (0x004d5cae/0x004d5d72), so the head is visited as a member too.
    uint16_t unit_index = head_unit_idx;
    while (unit_index != 0) {
        // Idle/parked members are excluded from the delta SUM but NOT from member_count's divisor --
        // see the header comment. 0x004d5cb7.
        if (gc.unit_is_idle_or_parked(player, (int32_t)unit_index) == 0) {
            const unit &u = unit_of(v, (uint32_t)player, (int32_t)unit_index);

            // width/height are RE-READ AT EACH COMPARISON in the original (fresh MOV per site,
            // 0x004d5cdb/0x004d5cf3/0x004d5d25/0x004d5d3d) rather than hoisted into a local before
            // the loop -- kept that way here per the const-view rule (ai_state.h: nothing here caches
            // a read across an iteration on the assumption it is stable).
            int32_t dx = (int32_t)u.x - head_x;
            if (dx > *v.map_width / 2) dx -= *v.map_width;    // 0x004d5ce9/0x004d5ced
            if (dx < -(*v.map_width) / 2) dx += *v.map_width; // 0x004d5d04/0x004d5d08

            int32_t dy = (int32_t)u.y - head_y;
            if (dy > *v.map_height / 2) dy -= *v.map_height;    // 0x004d5d33/0x004d5d37
            if (dy < -(*v.map_height) / 2) dy += *v.map_height; // 0x004d5d4e/0x004d5d52

            sum_dx += dx; // 0x004d5d58
            sum_dy += dy; // 0x004d5d5b
        }
        unit_index = unit_of(v, (uint32_t)player, (int32_t)unit_index).ai_group_next; // 0x004d5d6a
    }

    // Divide by (member_count - 1), UNSIGNED compare against 1 (JBE skips the divide), and the
    // divisor is the ORIGINAL member_count -- idle members skipped above do not shrink it.
    // 0x004d5d99-0x004d5dc2. IDIV is a real instruction here (not a shift-based approximation), so
    // C++'s truncating `/` on int32_t reproduces it exactly.
    if ((uint16_t)grp.member_count > 1) {
        const int32_t divisor = (int32_t)(uint16_t)grp.member_count - 1;
        sum_dx /= divisor;
        sum_dy /= divisor;
    }
    // member_count == 1 (or the raw sum when > 1 after divide): the sum is used AS-IS, only the final
    // AND mask below is applied -- no divide is skipped-but-implied.

    // Final: (head + sum) masked with the wrap MASK (AND, not modulo) -- valid only because map
    // dimensions are powers of two. 0x004d5dc5-0x004d5df7. map_width_mask/map_height_mask are
    // width_m/height_m (0x00fe5b40/0x00fe5b44) -- see ai_state.cpp's RID_WIDTH_M/RID_HEIGHT_M wiring.
    *out_x = (uint32_t)(head_x + sum_dx) & *v.map_width_mask;
    *out_y = (uint32_t)(head_y + sum_dy) & *v.map_height_mask;
}

} // namespace detail

void group_compute_centroid(int32_t player, int32_t group_index, uint32_t *out_x, uint32_t *out_y) {
    const ai_state st = state();
    detail::group_compute_centroid(st.read, live_calls(), player, group_index,
                                   out_x, out_y);
}


} // namespace mh::ai
