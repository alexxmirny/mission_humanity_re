#include "sim/sim_pathfind_route_leg_reconcile.h"

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace detail {

int32_t pathfind_route_leg_reconcile(const sim_view &v, sim_store &own, int32_t step_count) {
    // 0x0042062e-0x00420643: reset the result accumulator, refresh the wrap mask from the live map
    // width. Kept as a local rather than re-read through own.path_wrap_mask_mut() below -- nothing
    // else in this body (or, per the region's own comment, anywhere concurrent) writes the mask again
    // before we need its value, so the local is value-identical to re-reading the global.
    int32_t       result     = 0;
    const int32_t wrap_mask  = *v.map_width - 1;
    own.path_wrap_mask_mut() = static_cast<uint32_t>(wrap_mask);

    // 0x0042064a-0x00420662: DEAD CODE in the original -- a `for (i=0;i<step_count;i++) {}` loop with
    // an empty body (no memory write anywhere in it), whose counter slot is unconditionally
    // overwritten before its next real use (0x004206ae). Omitted; see the header banner.

    // 0x0042065e-0x0042066c: wave_limit = scratch[step_count-1].wave_rank + 1. NOTE: this indexes
    // step_count-1 UNCONDITIONALLY, before the step_count<=1 guard below ever runs -- if step_count
    // were 0 this reads scratch[-1], one record before the array. own.group_move_scratch_at() is
    // deliberately NOT bounds-checked (sim_state.h's own W2 policy: "several original bodies index
    // deliberately out of range"), so this matches the original's behaviour rather than asserting;
    // see uncertainties[] on why this is flagged rather than guarded.
    const int32_t wave_limit = own.group_move_scratch_at(step_count - 1).wave_rank + 1;

    int32_t leg_start = 0; // [EBP-0x38] -- the running segment-start cursor into the scratch array.

    for (int32_t wave = 0; wave < wave_limit; ++wave) {
        // 0x00420692-0x00420696: transcribed at its literal per-wave site (not hoisted above the
        // loop) even though step_count is loop-invariant -- see the header banner's reasoning.
        if (step_count <= 1) return 1;

        int32_t sum_dx = 0;             // [EBP-0x2c]
        int32_t sum_dy = 0;             // [EBP-0x28]
        int32_t j      = leg_start + 1; // [EBP-0x3c], first sub-walk cursor

        // 0x004206b1-0x00420794: accumulate the torus-wrapped tile delta of every member sharing this
        // wave_rank, each measured against the FIXED leg_start member (not the previous element).
        while (j < step_count && own.group_move_scratch_at(j).wave_rank == wave) {
            const group_scratch_member &leg = own.group_move_scratch_at(leg_start);
            const group_scratch_member &mem = own.group_move_scratch_at(j);

            const int32_t width = *v.map_width;
            int32_t       dx    = mem.tile_col - leg.tile_col;
            // 0x004206e4-0x00420723: truncating-toward-zero divide-by-2 (C's `/` matches the asm's
            // NEG/SAR/SUB/SAR idiom exactly for this operation).
            if ((-width) / 2 < dx) {
                // NOTE: 0x80 (128), a LITERAL constant, not `width` -- transcribed exactly as coded;
                // see uncertainties[] on why this asymmetry with the lower-wrap branch is preserved.
                if (width / 2 < dx) dx = 0x80 - dx;
            } else {
                dx += width;
            }
            sum_dx += dx;

            const int32_t height = *v.map_height;
            int32_t       dy     = mem.tile_row - leg.tile_row;
            if ((-height) / 2 < dy) {
                if (height / 2 < dy) dy = 0x80 - dy;
            } else {
                dy += height;
            }
            sum_dy += dy;

            ++j;
        }

        if (j == leg_start + 1) {
            // 0x00420796-0x004207ac: the sub-walk matched nothing at this wave. leg_start is left
            // UNTOUCHED (the original jumps straight back to the wave increment without writing
            // [EBP-0x38]) -- only the leg_start==0 result-forcing fires.
            if (leg_start == 0) result = 1;
            continue;
        }

        // 0x004207b1-0x00420829: average the accumulated delta over the run length, wrap it through
        // the path-wrap mask, and fall back to leg_start's own (unaveraged) tile if the averaged tile
        // turns out to be impassable.
        const int32_t count    = j - leg_start;
        int32_t       tile_col = (own.group_move_scratch_at(leg_start).tile_col + sum_dx / count) & wrap_mask;
        int32_t       tile_row = (own.group_move_scratch_at(leg_start).tile_row + sum_dy / count) & wrap_mask;

        if (v.passable[(tile_col << 8) | tile_row] == 0) {
            tile_col = own.group_move_scratch_at(leg_start).tile_col;
            tile_row = own.group_move_scratch_at(leg_start).tile_row;
        }

        // 0x00420837-0x00420876: resolve the region under the (possibly reverted) averaged tile;
        // fall back to scratch[0]'s OWN region if the resolved id is 0. Neither dereference here nor
        // the ones in the re-walk below null-checks `.region` before reading `.index` -- see
        // uncertainties[].
        uint16_t region_id = own.region_cell_at(tile_col, tile_row).region->index;
        if (region_id == 0) {
            const group_scratch_member &m0 = own.group_move_scratch_at(0);
            region_id                      = own.region_cell_at(m0.tile_col, m0.tile_row).region->index;
        }

        // 0x00420877-0x00420905: re-walk the SAME wave_rank run, this time starting AT leg_start (not
        // leg_start+1) -- so this pass also re-examines leg_start's own member. Any member whose own
        // region disagrees with region_id is bumped to wave_rank=wave_limit (removed from this wave's
        // consideration); the result bookkeeping only fires for the leg_start==0 segment.
        j = leg_start;
        if (leg_start == 0) result = 0;

        while (j < step_count && own.group_move_scratch_at(j).wave_rank == wave) {
            const group_scratch_member &mem        = own.group_move_scratch_at(j);
            const uint16_t              mem_region = own.region_cell_at(mem.tile_col, mem.tile_row).region->index;
            if (mem_region != region_id) {
                own.group_move_scratch_at(j).wave_rank = wave_limit;
                if (leg_start == 0) --result;
            }
            ++j;
        }

        if (leg_start == 0) result += j;

        leg_start = j; // 0x004208fb: advance the segment cursor past the run just processed.
    }

    // 0x00420906-0x0042090c: the wave loop exhausted wave_limit without the step_count<=1 early
    // return -- final answer is whatever the leg_start==0 segment(s) left in `result`.
    return result;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

int32_t pathfind_route_leg_reconcile(int32_t step_count) {
    sim_state st = state();
    return detail::pathfind_route_leg_reconcile(st.read, st.own, step_count);
}


} // namespace mh::sim
