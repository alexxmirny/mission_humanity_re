//
// sim_pathtrace_greedy_selftest.cpp -- `simtest` cases for SIM-TRACER-ORACLE:
//   llm_strat_trace_greedy_path @0x0066a9ac (0x9c7 = 2503 bytes), sim/sim_pathtrace_greedy.{h,cpp}
//
// WHY THIS FILE EXISTS. Until 2026-09-06 this function had NO offline test at all. It is reached in the
// suites only through callers that MOCK it (sim_unit_group_step_plane_selftest's T8/T9 record the CALL
// and its arguments, never the body), so 25340 `simtest` checks never executed one instruction of
// 2503 bytes of solver. SIM-SAVE-DIV fault 3 lived in it for a fortnight: the candidate distance was
// scored from the BYTE-wrapped coordinate where the original masks the PACKED value first, which is
// wrong ONLY at the map's wrap seam -- precisely the case no caller-level mock can reach. The fix's
// only evidence was a rig run (both peers, a save fixture, ~10 minutes). This file is the offline arm.
//
// THE DECISIVE PROPERTY, and why the obvious case does not test it. The original masks the packed
// candidate (`AND EDX,[_G_LLM_STRAT_PATHTRACE_COORD_MASK]` @0x0066aef8) and only then reads the two
// axes out of it (`MOVZX EAX,DH` @0x0066af3c for the column, `MOVZX ECX,DL` @0x0066af69 for the row).
// Score from the pre-mask byte instead and the value is 128 too large; `wrap_axis` folds by `dim`
// exactly ONCE, so it can repair that only while the true delta is small:
//
//     step WEST off column 0 -> column byte 0xFF; masked 0x7F = 127; the buggy form scores from 255.
//     goal_col 127 -> correct 0, buggy wrap(128) = 0    -> IDENTICAL
//     goal_col  64 -> correct 63, buggy 63              -> IDENTICAL
//     goal_col  62 -> correct 63, buggy 65              -> differs by 2
//     goal_col   0 -> correct  1, buggy 127             -> differs by 126   <-- maximal
//
// So a case with the goal near column 127 -- the intuitive "step west across the seam onto the goal" --
// proves NOTHING: both forms agree there exactly. The discriminating band is `goal_col < 63`. The arms
// below therefore put the goal at column 0 with the unit at column 0, which is also what the real
// divergence looked like (sim_pathtrace_greedy.cpp's own comment records a unit at (0,70) bound for
// (0,80) scoring the WEST candidate 137 instead of 11).
//
// A SEAM CASE ALONE IS NOT ENOUGH, so each one is PAIRED with an interior control at column 60 whose
// geometry is otherwise identical -- same start row, same goal offset, same initial heading, same
// expected route shape. The controls must produce the SAME trace under both scoring forms. Together the
// pairs assert the actual claim -- "wrong only at the seam" -- rather than merely asserting that our
// tracer produces some particular output. Mutation-tested 2026-09-06: reverting the five scoring sites
// to the byte-wrapped coordinate turns both seam arms red and leaves both controls green.
//
// EXPECTED VALUES. The routes are asserted in full, and they are not transcribed from a run: the tracer
// turns at most 45 degrees per step, so reversing an initial NORTH heading costs a fixed 4-step
// turnaround, and the arms' lengths are that turnaround plus the toroidal Manhattan distance. Arm B is
// 5 + 10 = 15 for a 10-tile southward goal; arm A is the 8-step loop back onto the start tile. IF ONE OF
// THESE EVER FAILS, RE-DERIVE IT FROM tmp/decomp_sim/llm_strat_trace_greedy_path_0066a9ac.asm -- do NOT
// re-record it from the body under test. That is exactly how the shuttle fixtures came to argue for the
// bug they were seeded from.
//
// SCOPE. The tracer reads only its own scratch plus four read-only tables and consults no map: the
// candidate gate is `dir_bitmask_table[move_dir_table[d].dir_code] & <running revisit mask>`, a
// visited/loop mask and not terrain. It makes no outward call (confirmed by reading the whole
// 0x9c7-byte body -- see the header banner), so unlike the bldg_anim_state family there is no
// un-mockable `mh::call::` VA here and the whole body is offline-reachable. Not covered below: the
// `heading != 0xff` forbidden-neighbour prologue (these arms pass 0xff, the no-approach-constraint
// path), air mode, and pathtrace_remove_loops.
//
#include "sim/sim_pathtrace_greedy.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- the four read-only tables, as the real bytes in /eng/mh.exe --------------------------------------
// Read out of the image via ReVA 2026-09-06 (and matching sim_test_support.h's own recorded capture of
// 2026-08-20). sim_fixture zero-initializes all four and reset() re-zeros them, so a test that wants the
// tracer to do anything real must install them -- which is part of why nothing had executed this body.

// _G_LLM_STRAT_MOVE_DIR_TABLE @0x0066a224, llm_strat_move_dir_step[24], 8-byte records:
// dir_code / opposite_idx / dcol / drow / next_straight / turn_a / turn_b / is_cardinal.
// Rows 0-3 are the cardinals (dir_code 0/2/4/6 = N/E/S/W), rows 4-7 the diagonals; rows 8-23 repeat
// those eight directions with different opposite_idx links. Row 3 is WEST -- note this is NOT dir8's
// encoding, where 6 is west (dir8_step_offsets below). Same index, different meaning, in two tables the
// same function reads -- a same-index-different-meaning trap that has bitten before.
constexpr uint8_t kMoveDirTable[24 * 8] = {
    0x00, 0x04, 0x00, 0xFF, 0x00, 0x10, 0x08, 0x01, // 0  N   (0,-1)
    0x02, 0x06, 0x01, 0x00, 0x01, 0x11, 0x09, 0x01, // 1  E   (1,0)
    0x04, 0x00, 0x00, 0x01, 0x02, 0x12, 0x0A, 0x01, // 2  S   (0,1)
    0x06, 0x02, 0xFF, 0x00, 0x03, 0x13, 0x0B, 0x01, // 3  W   (-1,0)
    0x01, 0x05, 0x01, 0xFF, 0x04, 0x14, 0x0D, 0x00, // 4  NE  (1,-1)
    0x03, 0x07, 0x01, 0x01, 0x05, 0x15, 0x0E, 0x00, // 5  SE  (1,1)
    0x05, 0x01, 0xFF, 0x01, 0x06, 0x16, 0x0F, 0x00, // 6  SW  (-1,1)
    0x07, 0x03, 0xFF, 0xFF, 0x07, 0x17, 0x0C, 0x00, // 7  NW  (-1,-1)
    0x01, 0x04, 0x01, 0xFF, 0x04, 0x14, 0x0D, 0x00, // 8  NE
    0x03, 0x06, 0x01, 0x01, 0x05, 0x15, 0x0E, 0x00, // 9  SE
    0x05, 0x00, 0xFF, 0x01, 0x06, 0x16, 0x0F, 0x00, // 10 SW
    0x07, 0x02, 0xFF, 0xFF, 0x07, 0x17, 0x0C, 0x00, // 11 NW
    0x00, 0x03, 0x00, 0xFF, 0x00, 0x10, 0x08, 0x01, // 12 N
    0x02, 0x05, 0x01, 0x00, 0x01, 0x11, 0x09, 0x01, // 13 E
    0x04, 0x07, 0x00, 0x01, 0x02, 0x12, 0x0A, 0x01, // 14 S
    0x06, 0x01, 0xFF, 0x00, 0x03, 0x13, 0x0B, 0x01, // 15 W
    0x07, 0x04, 0xFF, 0xFF, 0x07, 0x17, 0x0C, 0x00, // 16 NW
    0x01, 0x06, 0x01, 0xFF, 0x04, 0x14, 0x0D, 0x00, // 17 NE
    0x03, 0x00, 0x01, 0x01, 0x05, 0x15, 0x0E, 0x00, // 18 SE
    0x05, 0x02, 0xFF, 0x01, 0x06, 0x16, 0x0F, 0x00, // 19 SW
    0x00, 0x05, 0x00, 0xFF, 0x00, 0x10, 0x08, 0x01, // 20 N
    0x02, 0x07, 0x01, 0x00, 0x01, 0x11, 0x09, 0x01, // 21 E
    0x04, 0x01, 0x00, 0x01, 0x02, 0x12, 0x0A, 0x01, // 22 S
    0x06, 0x03, 0xFF, 0x00, 0x03, 0x13, 0x0B, 0x01, // 23 W
};

// _G_LLM_STRAT_DIR8_STEP_OFFSETS @0x0066a2e4, char[16] = 8 signed (dx,dy) pairs, clockwise from N.
constexpr int8_t kDir8StepOffsets[16] = {0, -1, 1, -1, 1, 0, 1, 1, 0, 1, -1, 1, -1, 0, -1, -1};

// dir_code values as they appear in move_dir_table, for readable route expectations.
enum : uint8_t { DC_N  = 0,
                 DC_NE = 1,
                 DC_E  = 2,
                 DC_SE = 3,
                 DC_S  = 4,
                 DC_SW = 5,
                 DC_W  = 6,
                 DC_NW = 7 };

constexpr int32_t MAP_DIM     = 128;  // a power of two, so smear8() yields the clean 0x7F7F coord_mask
constexpr uint8_t GOAL_BYTE   = 0xFF; // trace terminator: goal reached
constexpr int32_t NO_APPROACH = 0xFF; // `heading` value that skips the forbidden-neighbour prologue

// _G_LLM_STRAT_DIR_BITMASK_TABLE @0x0066a0a4 = {1,2,4,...,128}, i.e. 1 << dir_code.
// _G_LLM_STRAT_COORD_SIGN_LUT   @0x0066a124 = sign(int8_t(i)): 0 at 0, +1 for 1..127, 0xFF for 128..255.
void install_tracer_tables(sim_fixture &f) {
    for (int i = 0; i < 8; ++i) f.dir_bitmask_table[(size_t)i] = 1u << i;
    f.coord_sign_lut[0] = 0;
    for (int i = 1; i < 128; ++i) f.coord_sign_lut[(size_t)i] = 1;
    for (int i = 128; i < 256; ++i) f.coord_sign_lut[(size_t)i] = 0xFF;
    memcpy(f.move_dir_table.data(), kMoveDirTable, sizeof(kMoveDirTable));
    memcpy(f.dir8_step_offsets.data(), kDir8StepOffsets, sizeof(kDir8StepOffsets));
    f.map_width  = MAP_DIM;
    f.map_height = MAP_DIM;
    // reset() leaves this 0; both arms want the ground path (the air gate only fires for dir >= 8).
    f.pathfinder_air_mode_flag = 0;
}

// ck() prints immediately and retains nothing, so one rebuilt static buffer per check is safe.
const char *tag_msg(const char *tag, const char *what) {
    static char buf[224];
    snprintf(buf, sizeof(buf), "trace_greedy_path %s: %s", tag, what);
    return buf;
}
const char *tag_msg_step(const char *tag, const char *what, size_t step) {
    static char buf[224];
    snprintf(buf, sizeof(buf), "trace_greedy_path %s: %s (step %zu)", tag, what, step);
    return buf;
}

// Run one trace and compare the emitted route against `want` (one dir_code per step), the length, the
// terminator, and -- for the seam arms -- that the walk really did leave column 0.
// `want_seam_col` is the column the trace must be standing in at step `seam_step`; pass seam_step < 0
// to skip that check.
void check_trace(const char *tag, int32_t start_col, int32_t start_row, int32_t mode, int32_t goal_col,
                 int32_t goal_row, const uint8_t *want, size_t want_len, int seam_step,
                 int32_t want_seam_col) {
    sim_fixture f;
    f.reset();
    install_tracer_tables(f);
    const sim_view v = f.view();
    sim_store      s = f.store();

    const uint8_t *ret =
        detail::trace_greedy_path(v, s, start_col, start_row, mode, goal_col, goal_row, NO_APPROACH);

    ck(ret == f.pathtrace_dirs.data(), tag_msg(tag, "returns &pathtrace_dirs[0]"));
    ck_eq(f.pathtrace_len, (uint32_t)want_len, tag_msg(tag, "pathtrace_len"));
    if (f.pathtrace_len != (uint32_t)want_len) return; // the checks below would read a different trace
    ck_eq((uint32_t)f.pathtrace_dirs[want_len], (uint32_t)GOAL_BYTE,
          tag_msg(tag, "terminator is 0xff (goal reached), not 0xfe (dead end)"));

    // Report the FIRST mismatching step by name with its dir_code, not the whole route as one bit --
    // which direction it took instead is the diagnosis (see ck_eq's own note in sim_test_support.h).
    size_t bad = want_len;
    for (size_t i = 0; i < want_len; ++i) {
        if (f.move_dir_table[(size_t)f.pathtrace_dirs[i] * 8 + 0] != want[i]) { // +0 = .dir_code
            bad = i;
            break;
        }
    }
    if (bad == want_len) {
        ck(true, tag_msg(tag, "route matches the derived direction sequence step for step"));
    } else {
        ck_eq((uint32_t)f.move_dir_table[(size_t)f.pathtrace_dirs[bad] * 8 + 0], (uint32_t)want[bad],
              tag_msg_step(tag, "route diverges from the derived direction sequence", bad));
    }

    if (seam_step >= 0) {
        ck_eq((uint32_t)(f.pathtrace_pos[(size_t)seam_step] >> 8), (uint32_t)want_seam_col,
              tag_msg(tag, "the walk stands where expected on the seam (the anti-vacuity check)"));
    }
}

// ---- ARM A / CONTROL A2: the 8-step loop back onto the start tile -------------------------------------
// Start standing ON the goal but heading NORTH. The goal test runs AFTER the step, so the tracer must
// turn all the way around and come back: N, then the 4-step 45-degrees-per-step turnaround through the
// west (NW W SW S), then SE E NE back onto the tile. 8 steps.
//   at the seam (column 0) the turnaround walks through columns 127/126/125 -- the wrap the bug broke.
//   the pre-mask form instead scores those west candidates ~127 and oscillates N/NE/N/NW northward,
//   crawling the whole 128-row map before arriving from the far side: 128 steps.
constexpr uint8_t kRouteA[] = {DC_N, DC_NW, DC_W, DC_SW, DC_S, DC_SE, DC_E, DC_NE};

void test_seam_loop_back_onto_start_tile() {
    check_trace("A seam (0,70)->(0,70)", /*start_col=*/0, /*start_row=*/70, /*mode=*/0, /*goal_col=*/0,
                /*goal_row=*/70, kRouteA, sizeof(kRouteA), /*seam_step=*/2, /*want_seam_col=*/127);
}

void test_interior_loop_back_onto_start_tile_is_the_same() {
    // Identical geometry 60 columns east of the seam: same route, and it must be the same under BOTH
    // scoring forms. This is what makes arm A's red specific to the seam.
    check_trace("A2 interior (60,70)->(60,70)", 60, 70, 0, 60, 70, kRouteA, sizeof(kRouteA),
                /*seam_step=*/2, /*want_seam_col=*/59);
}

// ---- ARM B / CONTROL B2: the recorded divergence, 10 tiles south --------------------------------------
// The case sim_pathtrace_greedy.cpp's own fault-3 comment records: (0,70) bound for (0,80). 5 steps of
// turnaround plus the 10-tile toroidal Manhattan distance = 15.
//   the pre-mask form takes 118 steps here, going north around the map instead.
constexpr uint8_t kRouteB[] = {DC_N, DC_NW, DC_W, DC_SW, DC_S, DC_SE, DC_SE, DC_SE,
                               DC_S, DC_S, DC_S, DC_S, DC_S, DC_S, DC_S};

void test_seam_ten_south_is_the_short_route() {
    check_trace("B seam (0,70)->(0,80)", 0, 70, 0, 0, 80, kRouteB, sizeof(kRouteB), /*seam_step=*/2,
                /*want_seam_col=*/127);
}

void test_interior_ten_south_is_the_same() {
    check_trace("B2 interior (60,70)->(60,80)", 60, 70, 0, 60, 80, kRouteB, sizeof(kRouteB),
                /*seam_step=*/2, /*want_seam_col=*/59);
}

// ---- ARM C: the seam wrap works at all ---------------------------------------------------------------
// Deliberately NOT a discriminating case -- with the goal at column 127 the two scoring forms agree
// exactly (see the header table), so this proves only that a single WEST step off column 0 lands on
// column 127 and is recognised as the goal. Kept because that is a real property worth pinning, and
// labelled because a reader could otherwise mistake it for the load-bearing arm. mode 3 = WEST.
// No seam_step check here: pathtrace_pos records the position BEFORE each step, so the only entry is
// the start tile. The wrap is proven by the terminator instead -- reaching 0xff requires the stepped
// position to have equalled goal_packed = (127 << 8) | 70, which only the wrap can produce.
constexpr uint8_t kRouteC[] = {DC_W};

void test_single_west_step_wraps_onto_column_127() {
    check_trace("C wrap (0,70)->(127,70)", 0, 70, 3, 127, 70, kRouteC, sizeof(kRouteC),
                /*seam_step=*/-1, /*want_seam_col=*/0);
}

} // namespace

void run_pathtrace_greedy_tests() {
    test_seam_loop_back_onto_start_tile();
    test_interior_loop_back_onto_start_tile_is_the_same();
    test_seam_ten_south_is_the_short_route();
    test_interior_ten_south_is_the_same();
    test_single_west_step_wraps_onto_column_127();
}

} // namespace mh::sim::test
