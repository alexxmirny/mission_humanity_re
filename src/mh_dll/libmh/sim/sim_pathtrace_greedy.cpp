//
// sim/sim_pathtrace_greedy.cpp -- see sim_pathtrace_greedy.h. Translated from the DISASSEMBLY
// (tmp/decomp_sim/llm_strat_trace_greedy_path_0066a9ac.asm,
// tmp/decomp_sim/llm_strat_pathtrace_remove_loops_0066b376.asm), address-by-address, not from any
// Ghidra .c draft.
//
#include "sim/sim_pathtrace_greedy.h"

#include "addr/mh_calls.gen.h" // typed callables for the ORIGINAL functions this closure still calls out to
#include "ai/ai_state.h"       // ai_say / trace_budget -- the shared trace sink, not AI state

#include <cstdint>
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const pathtrace_remove_loops_calls &live_pathtrace_remove_loops_calls() {
    static const pathtrace_remove_loops_calls c{
        MH_LIBMH_BIND(llm_strat_pathtrace_normalize_repeat_dir_table),
    };
    return c;
}

namespace detail {

namespace {

// The "no candidate found yet this step" sentinel BOTH pathtrace_best_dir and (as the trace-buffer
// terminator meaning "dead end") pathtrace_dirs share the literal value 0xfe with -- kept as two
// separately-named constants below since they are conceptually different despite sharing a byte value
// (the original does too: 0xfe appears at both LAB_0066aa1e-family reset sites AND at LAB_0066b35c's
// sibling dead-end path).
constexpr uint32_t kNoCandidate = 0xfeu;
constexpr uint8_t  kDeadEndByte = 0xfeu;
constexpr uint8_t  kGoalByte    = 0xffu;
constexpr uint32_t kNoApproach  = 0xffu;

// 0x0066a9b5-0x0066a9db / 0x0066a9e7-0x0066aa0d: the 8x `SHR reg,1; OR acc,reg` bit-smear, transcribed
// literally rather than assumed to equal `dim-1` -- see the header's prologue note.
inline uint32_t smear8(uint32_t x) {
    uint32_t acc = 0;
    for (int i = 0; i < 8; ++i) {
        x >>= 1;
        acc |= x;
    }
    return acc;
}

// The single-axis toroidal fold shared by every distance/alignment computation in trace_greedy_path:
// `if (v >= half) v -= dim; if (v < neg_half) v += dim;` -- the exact "CMP half; JL skip_sub" /
// "CMP neg_half; JGE skip_add" idiom repeated at every one of this function's ~9 fold sites (the
// is_cardinal alignment check's 2 axes, plus each candidate distance's 2 axes x up to 5 candidates/step).
inline int32_t wrap_axis(int32_t raw, int32_t half, int32_t neg_half, int32_t dim) {
    int32_t v = raw;
    if (v >= half) v -= dim;
    if (v < neg_half) v += dim;
    return v;
}

// abs-via-CDQ/XOR/SUB, matching sim_geom_toroidal.cpp's own idiom (INT_MIN-safe, though a wrapped
// tile-coordinate delta can't realistically reach it).
inline int32_t abs_cdq(int32_t x) {
    const uint32_t mask = static_cast<uint32_t>(x >> 31);
    return static_cast<int32_t>((static_cast<uint32_t>(x) ^ mask) - mask);
}

} // namespace

// =====================================================================================================
// llm_strat_trace_greedy_path @0x0066a9ac
// =====================================================================================================
uint8_t *trace_greedy_path(const sim_view &v, sim_store &own, int32_t start_col, int32_t start_row,
                           int32_t mode, int32_t goal_col, int32_t goal_row, int32_t heading) {
    // ---- 0. prologue: rebuild the map-derived scratch (see header banner) -------------------------
    const uint32_t col_mask      = smear8(static_cast<uint32_t>(*v.map_width));  // 0x0066a9b5-0x0066a9dd
    const uint32_t row_mask      = smear8(static_cast<uint32_t>(*v.map_height)); // 0x0066a9e7-0x0066aa0f
    own.pathtrace_col_mask_mut() = col_mask;                                     // 0x0066a9e2
    own.pathtrace_row_mask_mut() = row_mask;                                     // 0x0066aa14
    // 0x0066a9dd/0x0066aa0f: coord_mask's low TWO bytes only (byte+0=row_mask's AL, byte+1=col_mask's
    // AL); bytes +2/+3 are never written by this function or (per sim_state.h's own note) anywhere
    // else -- written here as a full clean value with 0 in the high word (verified equivalent for
    // every AND site: every value ever masked against coord_mask is a `(col<<8)|row` pair whose own
    // high 16 bits are already zero -- see uncertainties).
    own.pathtrace_coord_mask_mut() =
        (static_cast<uint8_t>(col_mask) << 8) | static_cast<uint8_t>(row_mask);

    const uint32_t map_w           = static_cast<uint32_t>(*v.map_width);  // 0x0066aa19/0x0066aa1e
    const uint32_t map_h           = static_cast<uint32_t>(*v.map_height); // 0x0066aa39
    own.pathtrace_map_w_mut()      = map_w;
    own.pathtrace_map_h_mut()      = map_h;
    const uint32_t half_w          = map_w >> 1; // 0x0066aa23
    const uint32_t half_h          = map_h >> 1; // 0x0066aa43
    own.pathtrace_half_w_mut()     = half_w;
    own.pathtrace_half_h_mut()     = half_h;
    own.pathtrace_neg_half_w_mut() = -static_cast<int32_t>(half_w); // 0x0066aa2a
    own.pathtrace_neg_half_h_mut() = -static_cast<int32_t>(half_h); // 0x0066aa4a
    own.pathtrace_half_w_m1_mut()  = half_w - 1u;                   // 0x0066aa34, dead store (Law 2)
    own.pathtrace_half_h_m1_mut()  = half_h - 1u;                   // 0x0066aa54, dead store (Law 2)

    own.pathtrace_start_col_mut() = static_cast<uint32_t>(start_col) & col_mask; // 0x0066aa59-aa62
    own.pathtrace_start_row_mut() = static_cast<uint32_t>(start_row) & row_mask; // 0x0066aa67-aa70
    own.pathtrace_walk_dir_mut()  = static_cast<uint32_t>(mode);                 // 0x0066aa75-aa78, VERBATIM

    own.pathtrace_goal_col_mut() = static_cast<uint32_t>(goal_col) & col_mask; // 0x0066aa7d-aa86
    own.pathtrace_goal_row_mut() = static_cast<uint32_t>(goal_row) & row_mask; // 0x0066aa90-aa99
    // 0x0066aa8b/0x0066aa9e: (goal_col<<8)|goal_row, low bytes only -- the ONLY writer of this global
    // anywhere (sim_state.h's own note), so a clean 4-byte value is exact.
    own.pathtrace_goal_packed_mut() = (static_cast<uint8_t>(own.pathtrace_goal_col_mut()) << 8) |
                                      static_cast<uint8_t>(own.pathtrace_goal_row_mut());

    own.pathtrace_approach_dir_mut() = static_cast<uint32_t>(heading); // 0x0066aaa8, raw/verbatim first
    // 0x0066aaad-aad5: ALWAYS reset, regardless of the heading==0xff branch below.
    own.pathtrace_forbid_cell_at(0) = 0xffffffffu;
    own.pathtrace_forbid_cell_at(1) = 0xffffffffu;
    own.pathtrace_forbid_cell_at(2) = 0xffffffffu;
    own.pathtrace_forbid_cell_at(3) = 0xffffffffu;
    own.pathtrace_forbid_cell_at(4) = 0xffffffffu;

    // 0x0066aadf: JZ 0x0066ad88 -- only when heading's low byte != 0xff do we re-derive approach_dir
    // through the move-direction table and compute the forbidden-neighbor set.
    if (static_cast<uint8_t>(heading) != kNoApproach) {
        // 0x0066aae5-aaec: only the LOW BYTE (AL) is overwritten with the move-dir table's dir_code --
        // the register's upper 24 bits still carry whatever `heading`'s own upper bits were (heading is
        // never masked before this point), so the stored dword is "dirty" whenever a caller passes a
        // heading with non-zero bits above bit 7. Every later dword-width comparison against this
        // global (the loop's first exclusion test below, and the goal-arrival check in the main loop)
        // inherits that dirtiness -- preserve-original-bug (Law 2), do not clean it up. Caught by
        // reimpl-verify.js (2026-08-20): an earlier draft computed a clean zero-extended value here,
        // which silently changes both of those downstream comparisons for a dirty heading.
        const uint32_t approach_dir_dirty =
            (static_cast<uint32_t>(heading) & ~0xffu) | v.move_dir_table[heading].dir_code; // 0x0066aae5-aaec
        own.pathtrace_approach_dir_mut() = approach_dir_dirty;

        // ---- 0b. forbidden-neighbor loop (0x0066aaf1-0x0066ad87): fully-unrolled 8-iteration
        // original, transcribed as a real loop -- see the header's control-flow-reshaping note.
        // The excluded set is {approach_dir, approach_dir+1, approach_dir-1} mod 8 -- NOT
        // {approach_dir, approach_dir+1, approach_dir-2} (reimpl-verify.js 2026-08-20: the original's
        // third test operates on the ALREADY-incremented EAX, i.e. (approach_dir+1)-2 == approach_dir-1,
        // confirmed by hand-tracing 0x0066aaf8-0x0066ab19's EAX sequence 3->4->2 for approach_dir=3).
        // Test 1 (bare `dir8 == approach_dir_dirty`, unmasked) is the one that sees the dirty upper
        // bits; tests 2/3 are AND-0x7'd and so are unaffected by them either way.
        const uint8_t goal_col_b = static_cast<uint8_t>(own.pathtrace_goal_col_mut());
        const uint8_t goal_row_b = static_cast<uint8_t>(own.pathtrace_goal_row_mut());
        int           cursor     = 0;
        for (uint32_t dir8 = 0; dir8 < 8; ++dir8) {
            if (dir8 == approach_dir_dirty || dir8 == ((approach_dir_dirty + 1u) & 7u) ||
                dir8 == ((approach_dir_dirty - 1u) & 7u))
                continue;
            const uint32_t off  = (dir8 + 4u) & 7u;
            const uint8_t  fcol = static_cast<uint8_t>(goal_col_b + v.dir8_step_offsets[off * 2 + 0]);
            const uint8_t  frow = static_cast<uint8_t>(goal_row_b + v.dir8_step_offsets[off * 2 + 1]);
            own.pathtrace_forbid_cell_at(cursor++) =
                ((static_cast<uint32_t>(fcol) << 8) | frow) & own.pathtrace_coord_mask_mut();
        }
    }

    // ---- 1. main trace loop (0x0066ad88-0x0066b345, per-iteration LAB_0066ada0) --------------------
    uint32_t pos_packed = (static_cast<uint32_t>(static_cast<uint8_t>(own.pathtrace_start_col_mut()))
                           << 8) |
                          static_cast<uint8_t>(own.pathtrace_start_row_mut()); // 0x0066ad88-ad90
    uint32_t dir = own.pathtrace_walk_dir_mut() & 0xffu;                       // 0x0066ad96-ad9a

    const uint32_t coord_mask_v = own.pathtrace_coord_mask_mut();
    uint32_t       step         = 0;
    uint8_t        terminator   = kDeadEndByte;

    for (;;) {
        // (a) record this step.
        own.pathtrace_dir_at(step) = static_cast<uint8_t>(dir);         // 0x0066ada0
        own.pathtrace_pos_at(step) = static_cast<uint16_t>(pos_packed); // 0x0066ada6
        ++step;                                                         // 0x0066adae

        const auto &cur = v.move_dir_table[dir]; // ESI = dir*8 (0x0066adaf-adb2)

        // (b) advance pos by taking a step in `dir`.
        {
            const uint8_t new_col = static_cast<uint8_t>((pos_packed >> 8) + cur.dcol);               // 0x0066adbb
            const uint8_t new_row = static_cast<uint8_t>(pos_packed + cur.drow);                      // 0x0066adb5
            pos_packed            = ((static_cast<uint32_t>(new_col) << 8) | new_row) & coord_mask_v; // 0x0066adc1
        }

        // (c) goal check.
        if (pos_packed == own.pathtrace_goal_packed_mut()) { // 0x0066adc7
            const uint32_t approach_dir = own.pathtrace_approach_dir_mut();
            if (approach_dir == kNoApproach ||                         // 0x0066add3
                static_cast<uint32_t>(cur.dir_code) == approach_dir) { // 0x0066ade3-adea
                terminator = kGoalByte;                                // LAB_0066b35c
                break;
            }
            // else: goal reached but wrong final approach -- keep tracing (fall through).
        }

        // (d) revisit scan -- build the blocked-direction bitmask from every earlier step at this tile.
        uint32_t blocked_mask = 0;
        for (uint32_t k = 0; k < step; ++k) {
            if (own.pathtrace_pos_at(k) == static_cast<uint16_t>(pos_packed)) {
                const uint8_t d = own.pathtrace_dir_at(k);
                blocked_mask |= v.dir_bitmask_table[v.move_dir_table[d].dir_code];
            }
        }

        own.pathtrace_best_dir_mut()  = kNoCandidate;                     // 0x0066ae24
        own.pathtrace_best_dist_mut() = static_cast<int32_t>(0x7fffffff); // 0x0066ae2e

        const uint32_t goal_col_v   = own.pathtrace_goal_col_mut();
        const uint32_t goal_row_v   = own.pathtrace_goal_row_mut();
        const int32_t  half_w_v     = static_cast<int32_t>(own.pathtrace_half_w_mut());
        const int32_t  half_h_v     = static_cast<int32_t>(own.pathtrace_half_h_mut());
        const int32_t  neg_half_w_v = own.pathtrace_neg_half_w_mut();
        const int32_t  neg_half_h_v = own.pathtrace_neg_half_h_mut();
        const int32_t  map_w_v      = static_cast<int32_t>(own.pathtrace_map_w_mut());
        const int32_t  map_h_v      = static_cast<int32_t>(own.pathtrace_map_h_mut());

        // (e) candidate evaluation. `check_forbidden` selects the first-pass (WITH FORBID_CELLS
        // gating) vs second-pass (WITHOUT it) shape -- see the header's 1b note.
        auto try_candidate = [&](uint32_t cand_index, bool check_forbidden) {
            const auto &c = v.move_dir_table[cand_index];
            if ((v.dir_bitmask_table[c.dir_code] & blocked_mask) != 0) return;
            const uint8_t  cand_col    = static_cast<uint8_t>((pos_packed >> 8) + c.dcol);
            const uint8_t  cand_row    = static_cast<uint8_t>(pos_packed + c.drow);
            const uint32_t cand_packed = ((static_cast<uint32_t>(cand_col) << 8) | cand_row) & coord_mask_v;
            if (check_forbidden) {
                for (int k = 0; k < 5; ++k)
                    if (cand_packed == own.pathtrace_forbid_cell_at(k)) return;
            }
            // SCORE FROM THE MASKED COORDINATE, NOT THE uint8 ONE (SIM-SAVE-DIV fault 3, 2026-09-05).
            // `cand_col`/`cand_row` are BYTE-wrapped: (uint8_t)(0 - 1) is 255. The coordinate space is
            // masked to 0..127 (coord_mask 0x7F7F on a 128x128 map), so at the wrap seam the byte and
            // the real coordinate differ by 128 -- and wrap_axis folds by `dim` exactly ONCE, so it
            // cannot recover that: 255 - 0 folds to 127 and stops, where the true delta is -1.
            //
            // WHAT IT COST, measured, because the size of the effect is the point: a unit at (col 0,
            // row 70) heading for (0, 80) scored the WEST candidate at 127 + 10 = 137 instead of
            // 1 + 10 = 11, rejected the step across the seam, took a NORTH step away from the goal
            // instead, and then span in a 7-direction cycle until the trace hit 124 steps -- where the
            // original terminated at 18. Every earlier step of that trace was byte-identical.
            //
            // It is invisible ANYWHERE BUT THE SEAM, which is why 250 steps of promoted-vs-original
            // agreed in all 56 hashed regions before this one unit reached col 0, and why it took three
            // "the body is faithful" investigations to find: the executed instructions DO match, the
            // arithmetic they are fed does not. Fixed: score from cand_packed, which is already masked.
            const int32_t dcol = wrap_axis(static_cast<int32_t>(cand_packed >> 8) - static_cast<int32_t>(goal_col_v),
                                           half_w_v, neg_half_w_v, map_w_v);
            const int32_t drow = wrap_axis(static_cast<int32_t>(cand_packed & 0xffu) - static_cast<int32_t>(goal_row_v),
                                           half_h_v, neg_half_h_v, map_h_v);
            const int32_t dist = abs_cdq(dcol) + abs_cdq(drow);
            if (dist < own.pathtrace_best_dist_mut()) {
                own.pathtrace_best_dist_mut() = dist;
                own.pathtrace_best_dir_mut()  = cand_index;
            }
        };

        // is-cardinal alignment shortcut: skip straight-ahead AND the air-mode gate entirely.
        bool do_first_pass_turns = true;
        bool aligned_away        = false;
        if (cur.is_cardinal != 0) { // 0x0066ae38
            const int32_t col = static_cast<int32_t>(pos_packed >> 8);
            const int32_t row = static_cast<int32_t>(pos_packed & 0xffu);
            const uint8_t sign_col =
                v.coord_sign_lut[static_cast<uint8_t>(
                    wrap_axis(col - static_cast<int32_t>(goal_col_v), half_w_v, neg_half_w_v, map_w_v))];
            const uint8_t sign_row =
                v.coord_sign_lut[static_cast<uint8_t>(
                    wrap_axis(row - static_cast<int32_t>(goal_row_v), half_h_v, neg_half_h_v, map_h_v))];
            aligned_away = (sign_col == static_cast<uint8_t>(cur.dcol)) &&
                           (sign_row == static_cast<uint8_t>(cur.drow)); // 0x0066aec2
        }
        if (!aligned_away) {
            try_candidate(cur.next_straight, /*check_forbidden=*/true); // LAB_0066aecc
            if (*v.pathfinder_air_mode_flag != 0 && dir >= 8u)          // 0x0066afc1-afd1
                do_first_pass_turns = false;
        }
        if (do_first_pass_turns) {
            try_candidate(cur.turn_a, /*check_forbidden=*/true); // LAB_0066afd7
            try_candidate(cur.turn_b, /*check_forbidden=*/true); // LAB_0066b0cc
        }
        if (own.pathtrace_best_dir_mut() == kNoCandidate) {       // LAB_0066b1c1
            try_candidate(cur.turn_b, /*check_forbidden=*/false); // LAB_0066b1d3
            try_candidate(cur.turn_a, /*check_forbidden=*/false); // LAB_0066b28c
        }

        // (f) commit or dead-end.
        if (own.pathtrace_best_dir_mut() != kNoCandidate) { // LAB_0066b345/b1c1
            dir = own.pathtrace_best_dir_mut();
            continue; // -> LAB_0066ada0
        }
        terminator = kDeadEndByte; // already the default, kept explicit for clarity
        break;
    }

    own.pathtrace_dir_at(step) = terminator; // 0x0066b35e/0x0066b35c-b35e
    own.pathtrace_len_mut()    = step;       // 0x0066b364/0x0066b3fb (shared with remove_loops)
    return &own.pathtrace_dir_at(0);         // 0x0066b36a
}

// =====================================================================================================
// llm_strat_pathtrace_remove_loops @0x0066b376
// =====================================================================================================
int32_t pathtrace_remove_loops(const sim_view &v, sim_store &own,
                               const pathtrace_remove_loops_calls &c) {
    uint32_t outer = 0; // ESI

    for (;;) {
        ++outer;                                      // 0x0066b37d
        if (outer >= own.pathtrace_len_mut()) {       // 0x0066b37e-b384, unsigned JNC
            c.pathtrace_normalize_repeat_dir_table(); // 0x0066b408
            return 0;                                 // 0x0066b40d
        }

        const uint16_t pos          = own.pathtrace_pos_at(outer); // 0x0066b38a
        const uint8_t  entering_dir = own.pathtrace_dir_at(outer); // 0x0066b392
        // opposite_idx*8, kept low-byte-only (matches the original's DL, since only DL is ever OR'd
        // with the dir_code byte downstream) -- see the header's size-derivation note on why
        // opposite_idx must resolve to 0..7 here for the merge-table index to stay in bounds.
        const uint8_t merge_key_high =
            static_cast<uint8_t>(v.move_dir_table[entering_dir].opposite_idx * 8u); // 0x0066b399-b3a1

        uint32_t inner = own.pathtrace_len_mut(); // 0x0066b3a4, reloaded fresh every outer iteration

        for (;;) {
            --inner;                                          // 0x0066b3aa
            if (inner <= outer) break;                        // 0x0066b3ab-b3ad, unsigned JBE -> back to the outer loop
            if (own.pathtrace_pos_at(inner) != pos) continue; // 0x0066b3af-b3b7

            // found a revisit of the same tile -- try to merge the two directions.
            const uint8_t  exit_dir_raw = own.pathtrace_dir_at(inner);                                    // 0x0066b3b9
            const uint32_t dir_code     = static_cast<uint32_t>(v.move_dir_table[exit_dir_raw].dir_code); // 0x0066b3c0
            const uint8_t  merged       = v.pathtrace_dir_merge_lut[dir_code | merge_key_high];           // 0x0066b3c7-b3c9
            if (merged == 0xffu) continue;                                                                // 0x0066b3cf-b3d1 -> keep decrementing inner

            own.pathtrace_dir_at(outer) = merged; // 0x0066b3d3 -- splice

            // compaction: copy pos[]/dirs[] from inner+1.. down to outer+1.. until the terminator byte.
            for (;;) {
                ++inner;                                                   // 0x0066b3d9
                ++outer;                                                   // 0x0066b3da
                own.pathtrace_pos_at(outer) = own.pathtrace_pos_at(inner); // 0x0066b3db-b3e3
                const uint8_t d             = own.pathtrace_dir_at(inner); // 0x0066b3eb
                own.pathtrace_dir_at(outer) = d;                           // 0x0066b3f1
                if (d == 0xffu) break;                                     // 0x0066b3f7-b3f9
            }
            own.pathtrace_len_mut() = outer; // 0x0066b3fb
            return 1;                        // 0x0066b401
        }
        // inner scan exhausted without a mergeable splice -- outer loop continues (++outer, re-check).
    }
}

} // namespace detail

// ---- the public wrappers -----------------------------------------------------------------------

uint8_t *trace_greedy_path(int32_t start_col, int32_t start_row, int32_t mode, int32_t goal_col,
                           int32_t goal_row, int32_t heading) {
    sim_state st = state();
    return detail::trace_greedy_path(st.read, st.own, start_col, start_row, mode, goal_col, goal_row,
                                     heading);
}

int32_t pathtrace_remove_loops() {
    sim_state st = state();
    return detail::pathtrace_remove_loops(st.read, st.own, live_pathtrace_remove_loops_calls());
}


} // namespace mh::sim
