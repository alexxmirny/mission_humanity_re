//
// sim/sim_path_group_steps.cpp -- see sim_path_group_steps.h. Translated from the DISASSEMBLY
// (tmp/decomp_sim/llm_strat_unit_path_queue_splice_00421758.asm,
// tmp/decomp_sim/llm_strat_pathfind_build_steps_0041e836.asm,
// tmp/decomp_sim/llm_strat_group_path_step_record_0041f7ef.asm), address-by-address.
//
#include "sim/sim_path_group_steps.h"

#include "addr/mh_calls.gen.h"  // typed callables for the ORIGINAL functions this closure still calls out to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const path_group_steps_calls &live_path_group_steps_calls() {
    static const path_group_steps_calls c = {
        MH_LIBMH_BIND(llm_strat_tile_dist_wrapped),
        MH_LIBMH_BIND(llm_strat_pathfind_dir_code_from_delta),
        MH_LIBMH_BIND(llm_strat_group_path_step_append),
    };
    return c;
}

namespace detail {

namespace {

// (tile_x<<8)|tile_y -- the codebase's standard flat passable/tile_objects index (matches
// sim_store::passable_at()'s / sim_store::tile_object_at()'s own indexing convention, and
// sim_group_move_order_pathfind.cpp's identical private helper -- duplicated here per house rule 4,
// this one never leaves this translation unit).
inline uint32_t flat_idx(uint32_t x, uint32_t y) { return (x << 8) | y; }

} // namespace

// ---- llm_strat_unit_path_queue_splice @0x00421758 -----------------------------------------------

int32_t unit_path_queue_splice(sim_store &own, int32_t owner_index, int32_t unit_index, int32_t slot,
                               int32_t count) {
    if (slot >= count) {
        // 0x00421781-0x004217c0: early-exit shape -- STILL decrements the entry at `slot` (reading the
        // OLD value first, which is what the two-way return below is keyed on).
        path_waypoint &e              = own.path_buffer_at(owner_index, unit_index, slot);
        const uint8_t  old_run_length = e.run_length;
        --e.run_length;
        return (old_run_length == 0) ? (slot - count + 1) : (slot - count);
    }

    // 0x004217c5-0x004217f2: slot<count -- same decrement/read, and if the old run_length was exactly
    // 0, `count` itself is also decremented before the shift below uses it.
    path_waypoint &e0              = own.path_buffer_at(owner_index, unit_index, slot);
    const uint8_t  old_run_length0 = e0.run_length;
    --e0.run_length;
    if (old_run_length0 == 0) --count;

    // 0x004217f2-0x00421891: shift the tail of the run down by `count` entries, HIGH INDEX TO LOW
    // (dst[i] = src[i-count], i decreasing from `PATH_WAYPOINTS_PER_SLOT-count` down to `slot+1`
    // inclusive), matching the asm's DEC-and-compare-down pattern exactly -- direction and off-by-one
    // are load-bearing, so this is an index-by-index copy loop, not std::memmove/std::copy.
    for (int32_t i = PATH_WAYPOINTS_PER_SLOT - count; i > slot; --i) {
        const path_waypoint &src = own.path_buffer_at(owner_index, unit_index, i - count);
        path_waypoint       &dst = own.path_buffer_at(owner_index, unit_index, i);
        dst.run_length           = src.run_length;
        dst.heading              = src.heading;
    }
    return slot;
}

// ---- llm_strat_pathfind_build_steps @0x0041e836 -------------------------------------------------

int32_t pathfind_build_steps(const sim_view &v, sim_store &own, const path_group_steps_calls &c,
                             int32_t start_x, int32_t start_y, int32_t target_range,
                             uint32_t unused_reserved, int32_t path_slot_index) {
    // 0x0041e854: spilled to its stack home at entry and read nowhere else in the body -- genuine
    // unused original parameter (see header banner).
    (void)unused_reserved;

    const uint32_t player = *v.cur_player;

    // 0x0041e857-0x0041e89e: scan for the first empty (run_length==0) waypoint in this slot; `cursor`
    // lands on PATH_WAYPOINTS_PER_SLOT (300) if every entry is occupied.
    int32_t cursor = PATH_WAYPOINTS_PER_SLOT;
    for (int32_t i = 0; i < PATH_WAYPOINTS_PER_SLOT; ++i) {
        if (v.path_buffers[player * PATH_WAYPOINTS_PER_PLAYER + path_slot_index * PATH_WAYPOINTS_PER_SLOT +
                           i]
                .run_length == 0) {
            cursor = i;
            break;
        }
    }
    // `write_idx` is the SAME variable as `cursor` in the original (EBP-0x28), which continues to
    // double as the running write cursor for the rest of the function; split into two C++ locals only
    // because `cursor`'s ORIGINAL value is separately needed by the two cleanup paths below.
    int32_t write_idx = cursor;

    // 0x0041e8a4-0x0041e8c6: if already within target_range of the anchor, do nothing at all.
    const int32_t dist0 = c.tile_dist_wrapped(start_x, start_y, *v.group_anchor_x, *v.group_anchor_y);
    if (target_range > dist0) return 0;

    for (;;) {
        // 0x0041e8d2-0x0041e968: wrapped delta from the current position to the anchor. TWO
        // SEQUENTIAL, INDEPENDENT corrections per axis (not if/else) -- see uncertainties[].
        int32_t dx = *v.group_anchor_x - start_x;
        {
            const int32_t half_w = (*v.map_width - (*v.map_width >> 31)) >> 1; // recomputed twice in
                                                                               // the asm; CSE'd here,
                                                                               // provably identical
                                                                               // (pure fn of map_width).
            if (half_w < dx) dx -= *v.map_width;
            if (-half_w >= dx) dx += *v.map_width;
        }
        int32_t dy = *v.group_anchor_y - start_y;
        {
            const int32_t half_h = (*v.map_height - (*v.map_height >> 31)) >> 1;
            if (half_h < dy) dy -= *v.map_height;
            if (-half_h >= dy) dy += *v.map_height;
        }
        // 0x0041e968-0x0041e9a4: reduce to a unit sign step (IDIV dx/abs(dx) == sign(dx) for every
        // nonzero dx; no negative-truncation subtlety since the divisor is the dividend's own |value|).
        if (dx != 0) dx = (dx > 0) ? 1 : -1;
        if (dy != 0) dy = (dy > 0) ? 1 : -1;

        const int32_t dir_code  = c.pathfind_dir_code_from_delta(dx, dy);
        int32_t       candidate = dir_code;

        // 0x0041e9b8-0x0041ea18: is the diagonal (start+dx,start+dy) step walkable? `v.passable[...]
        // != 0` IS passable, `== 0` is blocked -- confirmed by cross-reference to
        // sim_group_move_order_pathfind.cpp's own `ahead_ok`/`side_ok` naming of the identical table,
        // not assumed from this function's asm alone.
        {
            const uint32_t diag_idx = flat_idx(static_cast<uint32_t>(start_x + dx) & *v.path_wrap_mask,
                                               static_cast<uint32_t>(start_y + dy) & *v.path_wrap_mask);
            if (v.passable[diag_idx] == 0) goto try_turn_minus3;

            // 0x0041e9e0-0x0041e9fd: row-only half-step -- start_x is used RAW (unmasked) here, only
            // start_y is masked; transcribed exactly, not "fixed" to mask both.
            const uint32_t row_half_idx =
                flat_idx(static_cast<uint32_t>(start_x), static_cast<uint32_t>(start_y + dy) & *v.path_wrap_mask);
            if (v.passable[row_half_idx] != 0) goto write_step;

            // 0x0041e9ff-0x0041ea18: col-only half-step -- the mirror image (start_x masked, start_y RAW).
            const uint32_t col_half_idx =
                flat_idx(static_cast<uint32_t>(start_x + dx) & *v.path_wrap_mask, static_cast<uint32_t>(start_y));
            if (v.passable[col_half_idx] == 0) goto try_turn_minus3;
        }
        goto write_step;

    try_turn_minus3: { // 0x0041ea1f: try dir_code-3 (wrapped into [1,24], lower bound only).
        candidate = dir_code - 3;
        if (candidate < 1) candidate += 24;
        const int32_t  ddx = v.map_dir_step_deltas[candidate * 2 + 0];
        const int32_t  ddy = v.map_dir_step_deltas[candidate * 2 + 1];
        const uint32_t diag_idx =
            flat_idx(static_cast<uint32_t>(ddx + start_x) & *v.path_wrap_mask,
                     static_cast<uint32_t>(ddy + start_y) & *v.path_wrap_mask);
        if (v.passable[diag_idx] == 0) goto try_turn_plus3;

        // NOTE order here is COL-half first, then row-half -- opposite of the diag block above; this
        // asymmetry is real (re-derived address-by-address), not a transcription slip.
        const uint32_t col_half_idx =
            flat_idx(static_cast<uint32_t>(ddx + start_x) & *v.path_wrap_mask, static_cast<uint32_t>(start_y));
        if (v.passable[col_half_idx] != 0) goto write_step;

        const uint32_t row_half_idx =
            flat_idx(static_cast<uint32_t>(start_x), static_cast<uint32_t>(ddy + start_y) & *v.path_wrap_mask);
        if (v.passable[row_half_idx] == 0) goto try_turn_plus3;
        goto write_step;
    }

    try_turn_plus3: { // 0x0041eabb: try dir_code+3 (wrapped down by 24 if >24, upper bound only).
        candidate = dir_code + 3;
        if (candidate > 24) candidate -= 24;
        const int32_t  ddx = v.map_dir_step_deltas[candidate * 2 + 0];
        const int32_t  ddy = v.map_dir_step_deltas[candidate * 2 + 1];
        const uint32_t diag_idx =
            flat_idx(static_cast<uint32_t>(ddx + start_x) & *v.path_wrap_mask,
                     static_cast<uint32_t>(ddy + start_y) & *v.path_wrap_mask);
        if (v.passable[diag_idx] == 0) goto dead_end;

        const uint32_t col_half_idx =
            flat_idx(static_cast<uint32_t>(ddx + start_x) & *v.path_wrap_mask, static_cast<uint32_t>(start_y));
        if (v.passable[col_half_idx] != 0) goto write_step;

        const uint32_t row_half_idx =
            flat_idx(static_cast<uint32_t>(start_x), static_cast<uint32_t>(ddy + start_y) & *v.path_wrap_mask);
        if (v.passable[row_half_idx] == 0) goto dead_end;
        goto write_step;
    }

    dead_end: { // 0x0041eb54: no direction works at all -- erase the ORIGINAL cursor entry, return 0.
        own.path_buffer_at(player, path_slot_index, cursor).run_length = 0;
        own.path_buffer_at(player, path_slot_index, cursor).heading    = 0;
        return 0;
    }

    write_step: { // 0x0041eba8: accept `candidate`'s step; the WRITTEN heading is always the ORIGINAL
                  // dir_code, never the turn candidate -- see header banner point 4.
        start_x = static_cast<int32_t>(static_cast<uint32_t>(start_x + v.map_dir_step_deltas[candidate * 2 + 0]) &
                                       *v.path_wrap_mask);
        start_y = static_cast<int32_t>(static_cast<uint32_t>(start_y + v.map_dir_step_deltas[candidate * 2 + 1]) &
                                       *v.path_wrap_mask);

        own.path_buffer_at(player, path_slot_index, write_idx).run_length = 1;
        own.path_buffer_at(player, path_slot_index, write_idx).heading    = static_cast<uint8_t>(dir_code);
        ++write_idx;

        // 0x0041ec26-0x0041ecd0: close enough now, AND the new tile is passable and building-free?
        const int32_t dist = c.tile_dist_wrapped(start_x, start_y, *v.group_anchor_x, *v.group_anchor_y);
        if (dist <= target_range) {
            const uint32_t here = flat_idx(static_cast<uint32_t>(start_x), static_cast<uint32_t>(start_y));
            if (v.passable[here] != 0 && v.tile_objects[here].building == 0) {
                own.path_buffer_at(player, path_slot_index, write_idx).run_length = 0;
                own.path_buffer_at(player, path_slot_index, write_idx).heading    = 0;
                return 1;
            }
        }

        // 0x0041ecd0-0x0041ed23: more than 64 waypoints placed since the ORIGINAL cursor -> abort,
        // erasing the entry at `cursor` (NOT `write_idx`).
        if (write_idx - cursor > 64) {
            own.path_buffer_at(player, path_slot_index, cursor).run_length = 0;
            own.path_buffer_at(player, path_slot_index, cursor).heading    = 0;
            return 0;
        }

        // 0x0041ed2c-0x0041ed61: otherwise, if the new position is still passable and building-free,
        // place another step; else give up WITHOUT any cleanup this time (asymmetric with the abort
        // above -- transcribed exactly).
        const uint32_t here2 = flat_idx(static_cast<uint32_t>(start_x), static_cast<uint32_t>(start_y));
        if (v.passable[here2] == 0) return 0;
        if (v.tile_objects[here2].building != 0) return 0;
    }
        continue; // 0x0041ed54: JZ back to 0x0041e8d2 -- loop for another step.
    }
}

// ---- llm_strat_group_path_step_record @0x0041f7ef -----------------------------------------------

void group_path_step_record(const sim_view &v, sim_store &own, const path_group_steps_calls &c,
                            int32_t order_idx, uint32_t heading, int32_t col, int32_t row) {
    const int32_t owner     = *v.group_order_owner;
    int32_t      &build_idx = own.group_path_build_idx_mut();

    const uint8_t col_byte = static_cast<uint8_t>(col);
    const uint8_t row_byte = static_cast<uint8_t>(row);

    // Shared "decrement the entry before the cursor; shrink the cursor if it hit 0" idiom -- appears
    // verbatim at 0x0041f906/0x0041f92f (delta==15), 0x0041f9ff/0x0041fa28 (delta==9),
    // 0x0041fb69/0x0041fb8b (check_val match), and 0x0041fd26/0x0041fd48 (adj-passable). Function-local
    // per house rule 4 (never leaves this one function's body).
    auto dec_prev_and_maybe_shrink = [&]() {
        path_waypoint &prev = own.path_buffer_at(static_cast<uint32_t>(owner), order_idx, build_idx - 1);
        --prev.run_length;
        if (prev.run_length == 0) --build_idx;
    };

    // 0x0041f810-0x0041f836: delta between the entry immediately before the write cursor and the new
    // heading, wrapped into [0,23]. NOTE: when build_idx==0, this reads path_buffer_at(...,-1) -- one
    // whole entry BEFORE this (owner,order_idx) slot's own run. See uncertainties[]: this matches the
    // asm exactly, and is harmless here because `delta`'s value is only ever consumed under a
    // `build_idx>1` guard below.
    int32_t delta = static_cast<int32_t>(
                        own.path_buffer_at(static_cast<uint32_t>(owner), order_idx, build_idx - 1).heading) -
                    static_cast<int32_t>(heading);
    if (delta < 0) delta += 24;

    // `record_heading` starts as the caller's own `heading`; the two REVERSAL branches below replace it
    // with a computed turn candidate instead.
    int32_t record_heading = static_cast<int32_t>(heading);

    if (delta == 15 && build_idx > 1) {
        // 0x0041f857-0x0041f8ea
        const int32_t p    = own.path_buffer_at(static_cast<uint32_t>(owner), order_idx, build_idx - 1).heading;
        const int32_t rem  = (p - 1) % 6;
        int32_t       cand = (rem == 0) ? (p + 6) : (p + 3);
        if (cand > 24) cand -= 24;
        record_heading = cand;
        dec_prev_and_maybe_shrink();
    } else if (delta == 9 && build_idx > 1) {
        // 0x0041f950-0x0041f9e3 (symmetric: subtract instead of add, wrap the OTHER direction).
        const int32_t p    = own.path_buffer_at(static_cast<uint32_t>(owner), order_idx, build_idx - 1).heading;
        const int32_t rem  = (p - 1) % 6;
        int32_t       cand = (rem == 0) ? (p - 6) : (p - 3);
        if (cand < 1) cand += 24;
        record_heading = cand;
        dec_prev_and_maybe_shrink();
    }

    // 0x0041fa30-0x0041fb04: does the entry now immediately before the cursor already carry
    // `record_heading`? Extend its run (saturating at 255) or start a fresh run right there.
    if (build_idx > 0 &&
        own.path_buffer_at(static_cast<uint32_t>(owner), order_idx, build_idx - 1).heading == record_heading) {
        path_waypoint &prev = own.path_buffer_at(static_cast<uint32_t>(owner), order_idx, build_idx - 1);
        if (prev.run_length < 0xff) {
            ++prev.run_length;
        } else {
            own.path_buffer_at(static_cast<uint32_t>(owner), order_idx, build_idx).run_length = 1;
            own.path_buffer_at(static_cast<uint32_t>(owner), order_idx, build_idx).heading =
                static_cast<uint8_t>(record_heading);
            ++build_idx;
        }
        return;
    }

    // 0x0041fb04-0x0041fb9a: a SECOND coalescing test, against `((record_heading+11) % 24) + 1`.
    const int32_t check_val = ((record_heading + 11) % 24) + 1;
    if (build_idx > 0 &&
        own.path_buffer_at(static_cast<uint32_t>(owner), order_idx, build_idx - 1).heading == check_val) {
        dec_prev_and_maybe_shrink();
        return;
    }

    // 0x0041fb9f-0x0041fc19: normalize the entry-before-cursor's heading into [1,12] (mirror around 24
    // if >=13) and diff it against record_heading.
    const int32_t prev_heading_raw =
        own.path_buffer_at(static_cast<uint32_t>(owner), order_idx, build_idx - 1).heading;
    const int32_t turn_class = (prev_heading_raw >= 13) ? (24 - prev_heading_raw) : prev_heading_raw;
    const int32_t turn_diff  = record_heading - turn_class;

    if (turn_diff == 6 || turn_diff == -6) {
        // 0x0041fc32-0x0041fcaf: a THIRD candidate turn direction ("adj"), derived from the
        // entry-before-cursor's own heading (re-read live -- build_idx may already have changed above).
        int32_t adj;
        int32_t dir_idx;
        if (turn_diff < 0) {
            const int32_t p = own.path_buffer_at(static_cast<uint32_t>(owner), order_idx, build_idx - 1).heading;
            adj             = p - 3;
            if (adj < 1) adj += 24;
            dir_idx = adj - 6;
        } else {
            const int32_t p = own.path_buffer_at(static_cast<uint32_t>(owner), order_idx, build_idx - 1).heading;
            adj             = p + 3;
            if (adj > 24) adj -= 24;
            dir_idx = adj + 6;
        }
        if (dir_idx < 1) dir_idx += 24;
        if (dir_idx > 24) dir_idx -= 24;

        const uint32_t wrap_mask = *v.path_wrap_mask;
        const uint8_t  step_col =
            static_cast<uint8_t>((col_byte + v.map_dir_step_deltas[dir_idx * 2 + 0]) & wrap_mask);
        const uint8_t step_row =
            static_cast<uint8_t>((row_byte + v.map_dir_step_deltas[dir_idx * 2 + 1]) & wrap_mask);

        if (v.passable[flat_idx(step_col, step_row)] != 0) {
            // 0x0041fd0a-0x0041fd7c: DEC the entry before the cursor, then call the ORIGINAL
            // llm_strat_group_path_step_append -- TWICE, conditionally, with the SAME arguments both
            // times (preserved exactly, per the brief -- not simplified to one call with a flag).
            dec_prev_and_maybe_shrink();
            c.group_path_step_append(order_idx, static_cast<uint32_t>(adj));
            if ((adj - 1) % 6 == 0) {
                c.group_path_step_append(order_idx, static_cast<uint32_t>(adj));
            }
            return;
        }
        // else: falls through to the plain append below (0x0041fd04 -> 0x0041fd83).
    }

    // 0x0041fd83-0x0041fdcd: plain append -- no coalescing applied.
    own.path_buffer_at(static_cast<uint32_t>(owner), order_idx, build_idx).run_length = 1;
    own.path_buffer_at(static_cast<uint32_t>(owner), order_idx, build_idx).heading =
        static_cast<uint8_t>(record_heading);
    ++build_idx;
}

} // namespace detail

// ---- the public wrappers --------------------------------------------------------------------------

int32_t unit_path_queue_splice(int32_t owner_index, int32_t unit_index, int32_t slot, int32_t count) {
    sim_state st = state();
    return detail::unit_path_queue_splice(st.own, owner_index, unit_index, slot, count);
}

int32_t pathfind_build_steps(int32_t start_x, int32_t start_y, int32_t target_range,
                             uint32_t unused_reserved, int32_t path_slot_index) {
    sim_state st = state();
    return detail::pathfind_build_steps(st.read, st.own, live_path_group_steps_calls(), start_x, start_y,
                                        target_range, unused_reserved, path_slot_index);
}

void group_path_step_record(int32_t order_idx, uint32_t heading, int32_t col, int32_t row) {
    sim_state st = state();
    detail::group_path_step_record(st.read, st.own, live_path_group_steps_calls(), order_idx, heading, col,
                                   row);
}


} // namespace mh::sim
