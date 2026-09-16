//
// sim/sim_group_scratch_centroid.cpp -- see sim_group_scratch_centroid.h. Translated from the
// DISASSEMBLY (tmp/decomp/llm_strat_ai_group_scratch_compute_centroid_0048d36f.asm), not from
// Ghidra's C: the .c draft's two closing stores happen to land on the right outputs (the x-derived
// value into *out_x, the y-derived value into *out_y -- verified line-by-line against the listing's
// two closing MOV pairs at 0x0048d526-0x0048d534, no swap), but every literal below was re-derived
// from the listing per house rules, not transcribed from the draft.
//
// Moved from ai/ai_group_scratch_centroid.cpp 2026-08-07 with no change to the body: the function is
// sim code, not AI code (see the header). The one real change is that it now reads the group-move
// scratch as a TYPED RECORD (scratch[i].unit_idx) instead of column 0 of a hand-derived 5-int32 row
// -- the record was recovered the same day, so the stride constant it used to carry is gone.
//
#include "sim/sim_group_scratch_centroid.h"

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace detail {

void group_scratch_compute_centroid(const sim_view &v, int32_t player, int32_t member_count,
                                    int32_t *out_x, int32_t *out_y) {
    // Anchor: group member 0's (x, y), read unconditionally regardless of member_count.
    // 0x0048d39e-0x0048d3d7.
    const int32_t anchor_unit_idx = v.group_move_scratch[0].unit_idx;
    const unit   &anchor          = unit_of(v, (uint32_t)player, anchor_unit_idx);
    const int32_t base_x          = (int32_t)anchor.x;
    const int32_t base_y          = (int32_t)anchor.y;

    int32_t sum_dx = 0;
    int32_t sum_dy = 0;

    // 0x0048d3e1-0x0048d4c3. Runs for member indices [1, member_count) -- no idle/parked filter, no
    // cap check against the array's real capacity (100 rows).
    for (int32_t i = 1; i < member_count; ++i) {
        const int32_t member_unit_idx = v.group_move_scratch[i].unit_idx;
        const unit   &m               = unit_of(v, (uint32_t)player, member_unit_idx);

        // width/height are RE-READ AT EACH COMPARISON in the original (fresh MOV per site, matching
        // ai_group_centroid.h's group_compute_centroid) rather than hoisted before the loop -- kept
        // that way here per the const-view rule (ai_state.h: nothing here caches a read across an
        // iteration on the assumption it is stable).
        int32_t dx = (int32_t)m.x - base_x;               // 0x0048d413/0x0048d41a
        if (dx > *v.map_width / 2) dx -= *v.map_width;    // 0x0048d420-0x0048d43c
        if (dx < -(*v.map_width) / 2) dx += *v.map_width; // 0x0048d43f-0x0048d45a

        int32_t dy = (int32_t)m.y - base_y;                 // 0x0048d46d/0x0048d474
        if (dy > *v.map_height / 2) dy -= *v.map_height;    // 0x0048d47a-0x0048d496
        if (dy < -(*v.map_height) / 2) dy += *v.map_height; // 0x0048d499-0x0048d4b4

        sum_dx += dx; // 0x0048d4b7-0x0048d4ba
        sum_dy += dy; // 0x0048d4bd-0x0048d4c0
    }

    // 0x0048d4c8-0x0048d4ef. SIGNED compare (member_count > 1); the divisor is recomputed once per
    // axis in the original (two separate DEC EBX sequences) but is the same value both times.
    if (member_count > 1) {
        const int32_t divisor = member_count - 1;
        sum_dx /= divisor; // IDIV -- C++'s truncating `/` on int32_t matches exactly
        sum_dy /= divisor;
    }

    // 0x0048d4f2-0x0048d523. TRUE MODULO via the IDIV remainder, NOT the sibling's power-of-two AND
    // mask -- `%` on int32_t matches IDIV's remainder (sign included) exactly.
    *out_x = (base_x + sum_dx + *v.map_width) % *v.map_width;   // 0x0048d4f2-0x0048d509 -> *out_x
    *out_y = (base_y + sum_dy + *v.map_height) % *v.map_height; // 0x0048d50c-0x0048d523 -> *out_y
}

} // namespace detail

void group_scratch_compute_centroid(int32_t player, int32_t member_count, int32_t *out_x, int32_t *out_y) {
    const sim_view v = state().read;
    detail::group_scratch_compute_centroid(v, player, member_count, out_x, out_y);
}


} // namespace mh::sim
