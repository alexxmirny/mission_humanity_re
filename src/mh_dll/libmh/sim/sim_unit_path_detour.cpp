#include "sim/sim_unit_path_detour.h"

#include "addr/mh_calls.gen.h"  // mh::call::llm_strat_unit_path_queue_splice -- bound live below
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const unit_path_detour_calls &live_unit_path_detour_calls() {
    static const unit_path_detour_calls c = {
        MH_LIBMH_BIND(llm_strat_unit_path_queue_splice),
    };
    return c;
}

namespace detail {

// The splice call's fourth argument (ECX) at every one of the six call sites -- a literal 2, unbacked
// by any Ghidra name found. See the header banner's "argument names are generic, not semantic" note.
inline constexpr int32_t PATH_DETOUR_SPLICE_COUNT = 2;

// The 24-way heading domain's ONE-SIDED wrap arithmetic, transcribed exactly as the assembly performs
// it at each of the twelve wrap sites -- a negative delta only ever checks `< 1 -> +24` (never
// `> 24`), a positive delta only ever checks `> 24 -> -24` (never `< 1`). See the header banner's note
// on why this is NOT the same as a general clamp for an out-of-[1,24] cur_heading.
inline int32_t path_detour_heading_minus(int32_t heading, int32_t delta) {
    int32_t r = heading - delta;
    if (r < 1) r += 24;
    return r;
}
inline int32_t path_detour_heading_plus(int32_t heading, int32_t delta) {
    int32_t r = heading + delta;
    if (r > 24) r -= 24;
    return r;
}

// One candidate probe, and (on success) the two-waypoint commit. `probe_heading` is the direction
// tried and tested for passability/occupancy; `second_heading` is the DIFFERENT, paired direction
// written into the second new waypoint on success (see the header banner's per-block table -- the two
// are NOT the same value). `base_col`/`base_row` are the unit's own tile, read ONCE by the caller and
// reused unchanged across every candidate (0x004209e3-0x00420a0d), matching the assembly's
// stack-cached locals rather than re-deriving the unit's position per block.
//
// Returns true iff the candidate tile is free (passable != 0 AND tile_objects.building == 0), in
// which case the path queue has been spliced and the two new waypoints written; false otherwise
// (0x00420acf/0x00420afb-style JZ fail, and their five structural siblings).
static bool unit_path_detour_try_candidate(const sim_view &v, sim_store &own,
                                           const unit_path_detour_calls &c, int32_t player,
                                           int32_t unit_idx, int32_t probe_heading,
                                           int32_t second_heading, int32_t base_col, int32_t base_row) {
    const uint8_t dx = v.map_dir_step_deltas[probe_heading * 2 + 0];
    const uint8_t dy = v.map_dir_step_deltas[probe_heading * 2 + 1];

    // Both axes wrapped through the SAME mask -- see the header banner's HAZARD note. Read via the
    // const view (not own.path_wrap_mask_mut()) since this is a pure read of the value the caller
    // already wrote at function entry.
    const uint32_t mask     = *v.path_wrap_mask;
    const int32_t  cand_col = static_cast<int32_t>(static_cast<uint32_t>(base_col + dx) & mask);
    const int32_t  cand_row = static_cast<int32_t>(static_cast<uint32_t>(base_row + dy) & mask);

    if (v.passable[(cand_col << 8) | cand_row] == 0) return false;  // e.g. 0x00420acf JZ
    if (tile_at(v, cand_col, cand_row).building != 0) return false; // e.g. 0x00420afb JZ

    // 0x00420b02-0x00420c8a and its five structural siblings: splice the path queue, then write the
    // two new waypoints at the SPLICED (post-call) cursor, not the pre-call one.
    unit         &um         = own.unit_at(static_cast<uint32_t>(player), unit_idx);
    const int32_t slot_id    = um.path_slot_id;
    const int32_t cursor_now = um.path_cursor;
    const int32_t new_cursor =
        c.unit_path_queue_splice(player, slot_id, cursor_now, PATH_DETOUR_SPLICE_COUNT);
    um.path_cursor = new_cursor;

    own.path_buffer_at(static_cast<uint32_t>(player), slot_id, new_cursor).heading =
        static_cast<uint8_t>(probe_heading);
    own.path_buffer_at(static_cast<uint32_t>(player), slot_id, new_cursor).run_length = 1;
    own.path_buffer_at(static_cast<uint32_t>(player), slot_id, new_cursor + 1).heading =
        static_cast<uint8_t>(second_heading);
    own.path_buffer_at(static_cast<uint32_t>(player), slot_id, new_cursor + 1).run_length = 1;
    return true;
}

int32_t unit_path_detour(const sim_view &v, sim_store &own, const unit_path_detour_calls &c,
                         int32_t player, int32_t unit_idx, int32_t alt_unit_idx) {
    // 0x004209d1-0x004209d7: recompute the shared torus-wrap mask from the current RAW map width --
    // NOT geom->width_mask (a different, already-computed global this function does not touch).
    own.path_wrap_mask_mut() = static_cast<uint32_t>(*v.map_width - 1);

    // 0x004209dc-0x00420a58: the unit's own tile (read once, reused unchanged below) and its CURRENT
    // waypoint's heading -- own.path_buffer_at(player, unit.path_slot_id, unit.path_cursor).heading.
    // See the header banner's note: this is the unit's own cursor, not a literal entry 0.
    const unit    &u0       = unit_of(v, static_cast<uint32_t>(player), unit_idx);
    const int32_t  base_col = static_cast<int32_t>(u0.x);
    const int32_t  base_row = static_cast<int32_t>(u0.y);
    const uint32_t path_index =
        static_cast<uint32_t>(player) * PATH_WAYPOINTS_PER_PLAYER +
        static_cast<uint32_t>(u0.path_slot_id) * PATH_WAYPOINTS_PER_SLOT +
        static_cast<uint32_t>(u0.path_cursor);
    const int32_t cur_heading = v.path_buffers[path_index].heading;

    // 0x00420a5b-0x00420a72: `(cur_heading - 1) % 6 == 0` (signed IDIV, matching C++ `%` exactly)
    // selects the search order.
    if (((cur_heading - 1) % 6) == 0) {
        // two candidates: +6 (LAB_004212f5), then -6 (LAB_00421513) on failure.
        if (unit_path_detour_try_candidate(v, own, c, player, unit_idx,
                                           path_detour_heading_plus(cur_heading, 6),
                                           path_detour_heading_minus(cur_heading, 6), base_col,
                                           base_row)) {
            return 0;
        }
        if (unit_path_detour_try_candidate(v, own, c, player, unit_idx,
                                           path_detour_heading_minus(cur_heading, 6),
                                           path_detour_heading_plus(cur_heading, 6), base_col,
                                           base_row)) {
            return 0;
        }
    } else {
        // four candidates, in try order: -6/+3, -3/+6, +6/-3, +3/-6 -- see the header banner's table,
        // mechanically re-verified against the raw .asm bytes (not inferred block-to-block).
        if (unit_path_detour_try_candidate(v, own, c, player, unit_idx,
                                           path_detour_heading_minus(cur_heading, 6),
                                           path_detour_heading_plus(cur_heading, 3), base_col,
                                           base_row)) {
            return 0;
        }
        if (unit_path_detour_try_candidate(v, own, c, player, unit_idx,
                                           path_detour_heading_minus(cur_heading, 3),
                                           path_detour_heading_plus(cur_heading, 6), base_col,
                                           base_row)) {
            return 0;
        }
        if (unit_path_detour_try_candidate(v, own, c, player, unit_idx,
                                           path_detour_heading_plus(cur_heading, 6),
                                           path_detour_heading_minus(cur_heading, 3), base_col,
                                           base_row)) {
            return 0;
        }
        if (unit_path_detour_try_candidate(v, own, c, player, unit_idx,
                                           path_detour_heading_plus(cur_heading, 3),
                                           path_detour_heading_minus(cur_heading, 6), base_col,
                                           base_row)) {
            return 0;
        }
    }

    // 0x0042172e-0x0042174d: every candidate in the selected order failed. Recurse on the alternate
    // unit with alt cleared to 0 (bounding recursion to depth 1), or report "no detour". Recurses
    // WITHIN detail:: (reusing the already-bound v/own/c), matching this codebase's established
    // self-recursion precedent (sim_bldg_propagate_network_connectivity.cpp,
    // sim_unit_create_soldier.cpp) -- SIM1-G2 first-slice prep's own brief told the translator to
    // recurse through the public wrapper instead, which was wrong -- that brief was corrected
    // on 2026-08-20; this is the fix.
    if (alt_unit_idx != 0) {
        return unit_path_detour(v, own, c, player, alt_unit_idx, 0);
    }
    return 1;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

int32_t unit_path_detour(int32_t player, int32_t unit_idx, int32_t alt_unit_idx) {
    sim_state st = state();
    return detail::unit_path_detour(st.read, st.own, live_unit_path_detour_calls(), player, unit_idx,
                                    alt_unit_idx);
}


} // namespace mh::sim
