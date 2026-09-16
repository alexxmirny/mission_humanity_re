//
// sim/sim_pathfind_route.cpp -- see sim_pathfind_route.h. Translated from the DISASSEMBLY, not the
// Ghidra .c drafts:
//   tmp/decomp_sim/llm_strat_pathfind_trace_route_0041ef3e.asm
//   tmp/decomp_sim/llm_strat_pathfind_plan_group_route_0042039c.asm
//
#include "sim/sim_pathfind_route.h"
#include "sim/sim_stack_guard.h" // LIFT-TABLE S5: the stack probe is libmh-internal, not a host service

#include "addr/mh_calls.gen.h" // typed callables for the ORIGINAL functions this closure still calls out to
#include "ai/ai_state.h"       // ai_say / trace_budget -- the shared trace sink, not AI state
#include "state/host_api.h"
#include "addr/mh_rebind.gen.h"
#include "state/rebind_targets.gen.h"

namespace mh::sim {

namespace detail {

namespace {

// `region_cell_at(col,row).terrain_flags >> 8` -- the per-cell "value" both functions read. Routed
// through sim_store's existing MUTABLE accessor for a purely read-only purpose, matching
// sim_group_move_order_pathfind.cpp's identical precedent (its own terrain_ahead/terrain_here/
// best_terrain locals) -- there is no separate const sim_view accessor for the region grid.
inline uint32_t region_value(sim_store &own, uint32_t col, uint32_t row) {
    return own.region_cell_at(static_cast<int32_t>(col), static_cast<int32_t>(row)).terrain_flags >> 8;
}

// The 4-neighbour cardinal-open bitmask trace_route's phase 1 builds, per the header banner.
inline constexpr uint32_t BIT_ROW_PLUS  = 1; // row+1  -> heading 1
inline constexpr uint32_t BIT_COL_MINUS = 2; // col-1  -> heading 7
inline constexpr uint32_t BIT_ROW_MINUS = 4; // row-1  -> heading 0xd
inline constexpr uint32_t BIT_COL_PLUS  = 8; // col+1  -> heading 0x13

} // namespace

// llm_strat_pathfind_trace_route @0x0041ef3e. See the header banner for the full derivation.
int32_t pathfind_trace_route(const sim_view &v, sim_store &own, uint8_t src_col, uint8_t src_row,
                             uint8_t dst_col, uint8_t dst_row, void *step_ctx) {
    const int32_t wrap = static_cast<int32_t>(*v.path_wrap_mask);

    uint8_t col = src_col;
    uint8_t row = src_row;

    // 0x0041ef5f-0x0041ef8f: local_44 (committed heading), local_38 (straight-run length), and the
    // SOURCE cell's own region value (local_34, the running "best value" tracker) -- all set before
    // any loop.
    uint32_t committed_heading = 0;
    int32_t  straight_run      = 0;
    uint32_t best_value        = region_value(own, col, row);

    // 0x0041efd0-0x0041efdd: the source cell itself is the blocked sentinel -- fail before any step.
    if (best_value == 0xffffffu) {
        return 1;
    }

    // 0x0041ef92-0x0041efaf: the DESTINATION cell's value, read exactly ONCE here and never shifted
    // again -- see the header banner's "A CAUGHT GHIDRA DRAFT BUG" note.
    const uint32_t dst_value = region_value(own, dst_col, dst_row);

    // ---- phase 1 (0x0041efe2-0x0041f658): greedy region-value descent ----------------------------
    while ((col != dst_col || row != dst_row) && dst_value <= best_value) {
        const uint32_t here_value = region_value(own, col, row);
        if (here_value == 0) {
            return 1; // 0x0041efd6/0x0041f025: blocked mid-route
        }
        if (here_value == 1) {
            break; // 0x0041f031/0x0041f035: "close enough" -- drop into phase 2
        }

        uint32_t candidate_heading = committed_heading; // local_40, reset every iteration
        uint32_t cardinal_open     = 0;                 // local_3c
        best_value                 = here_value;

        const uint8_t row_plus  = static_cast<uint8_t>((static_cast<int32_t>(row) + 1) & wrap);
        const uint8_t row_minus = static_cast<uint8_t>((static_cast<int32_t>(row) - 1) & wrap);
        const uint8_t col_plus  = static_cast<uint8_t>((static_cast<int32_t>(col) + 1) & wrap);
        const uint8_t col_minus = static_cast<uint8_t>((static_cast<int32_t>(col) - 1) & wrap);

        // The ONE update rule shared by all 8 checks below. Ghidra printed the FIRST (row+1) check as
        // a simpler "only on strict improvement" form and the other 7 as "strict improvement OR a tie
        // that overrides an already-diverged candidate" -- these are the SAME rule: at the point the
        // first check runs, `candidate_heading == committed_heading` always (nothing has diverged
        // yet), which is exactly the case the general form's tie-clause folds away to false. Verified
        // algebraically against every one of the 8 raw comma-expressions before consolidating.
        auto consider = [&](uint32_t value, uint32_t heading) {
            if (value <= best_value) {
                if (value < best_value || candidate_heading != committed_heading) {
                    candidate_heading = heading;
                }
                best_value = value;
            }
        };

        // 0x0041efff-0x0041f0ed: row+1.
        const uint32_t v_row_plus = region_value(own, col, row_plus);
        if (v_row_plus < 0xffffffu && v_row_plus != 0) {
            cardinal_open |= BIT_ROW_PLUS;
            consider(v_row_plus, 1);
        }
        // 0x0041f0ed-0x0041f192: col-1.
        const uint32_t v_col_minus = region_value(own, col_minus, row);
        if (v_col_minus < 0xffffffu && v_col_minus != 0) {
            cardinal_open |= BIT_COL_MINUS;
            consider(v_col_minus, 7);
        }
        // 0x0041f192-0x0041f237: row-1.
        const uint32_t v_row_minus = region_value(own, col, row_minus);
        if (v_row_minus < 0xffffffu && v_row_minus != 0) {
            cardinal_open |= BIT_ROW_MINUS;
            consider(v_row_minus, 0xd);
        }
        // 0x0041f237-0x0041f2dc: col+1.
        const uint32_t v_col_plus = region_value(own, col_plus, row);
        if (v_col_plus < 0xffffffu && v_col_plus != 0) {
            cardinal_open |= BIT_COL_PLUS;
            consider(v_col_plus, 0x13);
        }

        // 0x0041f2dc-0x0041f317: (col-1,row+1), gated on row+1 OR col-1 open.
        if ((cardinal_open & (BIT_ROW_PLUS | BIT_COL_MINUS)) != 0) {
            const uint32_t v_diag = region_value(own, col_minus, row_plus);
            if (v_diag < 0xffffffu && v_diag != 0) consider(v_diag, 4);
        }
        // 0x0041f39d-0x0041f3d8: (col-1,row-1), gated on col-1 OR row-1 open.
        if ((cardinal_open & (BIT_COL_MINUS | BIT_ROW_MINUS)) != 0) {
            const uint32_t v_diag = region_value(own, col_minus, row_minus);
            if (v_diag < 0xffffffu && v_diag != 0) consider(v_diag, 0xa);
        }
        // 0x0041f45e-0x0041f499: (col+1,row-1), gated on row-1 OR col+1 open.
        if ((cardinal_open & (BIT_ROW_MINUS | BIT_COL_PLUS)) != 0) {
            const uint32_t v_diag = region_value(own, col_plus, row_minus);
            if (v_diag < 0xffffffu && v_diag != 0) consider(v_diag, 0x10);
        }
        // 0x0041f51f-0x0041f55a: (col+1,row+1), gated on row+1 OR col+1 open.
        if ((cardinal_open & (BIT_ROW_PLUS | BIT_COL_PLUS)) != 0) {
            const uint32_t v_diag = region_value(own, col_plus, row_plus);
            if (v_diag < 0xffffffu && v_diag != 0) consider(v_diag, 0x16);
        }

        // 0x0041f5e0-0x0041f609: commit the candidate. First iteration ever (committed_heading==0)
        // always adopts the candidate; afterward, a DIFFERENT candidate only wins once the current
        // straight run has taken at least one step (straight_run>0) -- prefers finishing a straight
        // run over re-routing on every single tile.
        if (committed_heading == 0) {
            committed_heading = candidate_heading;
        }
        if (candidate_heading != committed_heading && straight_run > 0) {
            committed_heading = candidate_heading;
            straight_run      = 0;
        }

        MH_LIBMH_BIND(llm_strat_group_path_step_record)(
            static_cast<int32_t>(reinterpret_cast<intptr_t>(step_ctx)), committed_heading, col, row);

        // 0x0041f61c-0x0041f651: advance by the committed heading's (dx,dy), byte-wrapped.
        col = static_cast<uint8_t>(
            wrap & (static_cast<int32_t>(col) + v.map_dir_step_deltas[committed_heading * 2 + 0]));
        row = static_cast<uint8_t>(
            wrap & (static_cast<int32_t>(row) + v.map_dir_step_deltas[committed_heading * 2 + 1]));
        ++straight_run;
    }

    // ---- phase 2 (0x0041f65d-0x0041f7ec): corner-cut-guarded straight-line walk ------------------
    for (;;) {
        if (col == dst_col && row == dst_row) {
            return 0; // 0x0041f7dc: destination reached exactly
        }

        // 0x0041f671-0x0041f757: pick the diagonal-preferring step direction toward (dst_col,
        // dst_row). Every branch assigns dcol/drow/heading; the chain is exhaustive (col==dst,
        // row==dst, then the four col</col>=/row</row>= combinations of the remaining case).
        int32_t  dcol    = 0;
        int32_t  drow    = 0;
        uint32_t heading = 0; // every branch below assigns a real value; initialised defensively
        if (col == dst_col) {
            if (row < dst_row) {
                drow    = 1;
                heading = 1;
            } else {
                drow    = -1;
                heading = 0xd;
            }
        } else if (row == dst_row) {
            if (col < dst_col) {
                dcol    = 1;
                heading = 0x13;
            } else {
                dcol    = -1;
                heading = 7;
            }
        } else if (row < dst_row) {
            if (col < dst_col) {
                dcol    = 1;
                heading = 0x16;
            } else {
                dcol    = -1;
                heading = 4;
            }
            drow = 1;
        } else if (col < dst_col) {
            dcol    = 1;
            drow    = -1;
            heading = 0x10;
        } else {
            dcol    = -1;
            drow    = -1;
            heading = 0xa;
        }

        // 0x0041f757-0x0041f799: the corner-cut guard -- stop (return 1, blocked) only when BOTH the
        // col-only-flank and the row-only-flank tiles are impassable.
        const uint8_t flank_col =
            static_cast<uint8_t>((static_cast<int32_t>(col) + dcol) & wrap);
        const uint8_t flank_row =
            static_cast<uint8_t>((static_cast<int32_t>(row) + drow) & wrap);
        const bool col_flank_open = v.passable[(static_cast<uint32_t>(flank_col) << 8) | row] != 0;
        const bool row_flank_open = v.passable[(static_cast<uint32_t>(col) << 8) | flank_row] != 0;
        if (!col_flank_open && !row_flank_open) {
            break;
        }

        MH_LIBMH_BIND(llm_strat_group_path_step_record)(
            static_cast<int32_t>(reinterpret_cast<intptr_t>(step_ctx)), heading, col, row);
        col = static_cast<uint8_t>((static_cast<int32_t>(col) + dcol) & wrap);
        row = static_cast<uint8_t>((static_cast<int32_t>(row) + drow) & wrap);
    }
    return 1; // 0x0041f799: corner-cut fully blocked
}

// llm_strat_pathfind_plan_group_route @0x0042039c. See the header banner for the full derivation.
int32_t pathfind_plan_group_route(const sim_view &v, sim_store &own, uint32_t start_col,
                                  uint32_t start_row, uint32_t dest_col, uint32_t dest_row,
                                  uint32_t *out_col, uint32_t *out_row) {
    const uint32_t wrap  = *v.path_wrap_mask;
    const int32_t  map_w = *v.map_width;
    const int32_t  map_h = *v.map_height;

    // 0x004203e2-0x004203eb: dest packed as (dest_col<<8)|dest_row -- passed to find_route/
    // flood_reachable/route_search as the param those callees' own committed prototypes name
    // "start_region"/"query_cell", even though it is the DEST here, not this walk's own start (see
    // the header banner).
    const uint16_t dest_packed = static_cast<uint16_t>((static_cast<uint8_t>(dest_col) << 8) |
                                                       static_cast<uint8_t>(dest_row));

    uint32_t col = start_col;
    uint32_t row = start_row;
    // Only ever read on the (provably dead, see the header banner) trailing path below; initialised
    // so that dead path cannot read an indeterminate value.
    uint16_t current_packed = 0;

    for (;;) { // 0x004203bd: outer do-while
        const llm_map_region *region =
            own.region_cell_at(static_cast<int32_t>(col), static_cast<int32_t>(row)).region;

        // 0x004203ee-0x00420580: inner while -- step toward (dest_col,dest_row) one wrap-corrected
        // tile at a time, re-checking the region on every step. Same halving/wrap-correction idiom as
        // sim_group_move_order_pathfind.cpp's centroid computation (bit-identical to plain C `/` for
        // every int32 input here).
        while (col != dest_col || row != dest_row) {
            int32_t dcol = static_cast<int32_t>(col) - static_cast<int32_t>(dest_col);
            if (map_w / 2 < dcol) dcol -= map_w / 2;
            if (dcol <= -(map_w / 2)) dcol += map_w;

            int32_t drow = static_cast<int32_t>(row) - static_cast<int32_t>(dest_row);
            if (map_h / 2 < drow) drow -= map_h / 2;
            if (drow <= -(map_h / 2)) drow += map_h;

            if (dcol < 0) col += 1;
            if (dcol > 0) col -= 1;
            if (drow < 0) row += 1;
            if (drow > 0) row -= 1;
            col &= wrap;
            row &= wrap;

            const llm_map_region *stepped_region =
                own.region_cell_at(static_cast<int32_t>(col), static_cast<int32_t>(row)).region;
            if (stepped_region != region && stepped_region != nullptr) {
                region         = stepped_region;
                current_packed = static_cast<uint16_t>((static_cast<uint8_t>(col) << 8) |
                                                       static_cast<uint8_t>(row));
                if (MH_LIBMH_BIND(llm_map_region_find_route)(dest_packed, current_packed) != 0) {
                    return 0; // 0x00420574: blocked mid-route
                }
            }
        }

        // 0x004203f4/0x004203fc/0x00420418: reached, per the inner while's own negated exit
        // condition, ALWAYS true here -- see the header banner's reachability derivation. Everything
        // from this point down to the function's end is therefore UNREACHABLE in the compiled
        // binary; transcribed anyway (Law 2), not silently dropped.
        if (col == dest_col && row == dest_row) {
            return 1; // 0x00420603
        }

        current_packed = static_cast<uint16_t>((static_cast<uint8_t>(col) << 8) |
                                               static_cast<uint8_t>(row));
        if (MH_LIBMH_BIND(llm_map_region_find_route)(dest_packed, current_packed) == 0) {
            continue; // 0x004205b5: outer do-while re-loops
        }
        break; // 0x004205b5-false: find_route blocked -> falls to the trailing block below
    }

    // ---- UNREACHABLE (0x00420595-0x00420602; see the header banner) -------------------------------
    *out_col = col;
    *out_row = row;
    if (MH_LIBMH_BIND(llm_map_region_flood_reachable)(static_cast<int32_t>(dest_packed),
                                                      static_cast<int32_t>(current_packed)) == 0) {
        return 1;
    }
    MH_LIBMH_BIND(llm_map_region_route_search)(dest_packed, static_cast<int16_t>(current_packed),
                                               &own.group_route_step_at(0).dir_code);
    mh::sim::stack_capacity_guard_noop(); // LIFT-TABLE S5: the Watcom __STK probe, libmh-internal
    return 0;
}

} // namespace detail

// ---- the public wrappers ---------------------------------------------------------------------------

int32_t pathfind_trace_route(uint8_t src_col, uint8_t src_row, uint8_t dst_col, uint8_t dst_row,
                             void *step_ctx) {
    sim_state st = state();
    return detail::pathfind_trace_route(st.read, st.own, src_col, src_row, dst_col, dst_row, step_ctx);
}

int32_t pathfind_plan_group_route(uint32_t start_col, uint32_t start_row, uint32_t dest_col,
                                  uint32_t dest_row, uint32_t *out_col, uint32_t *out_row) {
    sim_state st = state();
    return detail::pathfind_plan_group_route(st.read, st.own, start_col, start_row, dest_col, dest_row,
                                             out_col, out_row);
}


} // namespace mh::sim
