//
// sim_unit_soldier_anim_selftest.cpp -- `simtest` cases for
// llm_strat_unit_squad_pick_lead_soldier_in_direction (@0x0048905e) and
// llm_strat_unit_soldiers_start_walk_anim (@0x004897c7), sim/sim_unit_soldier_anim.h/.cpp,
//
// Both take a `unit_soldier_anim_calls` struct of function pointers; this TU provides a RECORDING
// mock (one static instance, one global g_log reconfigured per case, same shape as
// sim_unit_ctrlgroup_member_selftest.cpp) so every case asserts BOTH the function's own writes/return
// AND the exact callee-invocation sequence + args.
//
// Expected values are derived from the DISASSEMBLY, not the translation:
//   tmp/decomp/llm_strat_unit_squad_pick_lead_soldier_in_direction_0048905e.asm
//   tmp/decomp/llm_strat_unit_soldiers_start_walk_anim_004897c7.asm
// Soldier record (stride 0x1d, base 0xe0e5a8): owner_unit i16 @+0, next_soldier u16 @+2, cur_x i8 @+4
// (0xe0e5ac), cur_y i8 @+5 (0xe0e5ad), sprite_frame i8 @+6 (0xe0e5ae), start_x i8 @+7 (0xe0e5af),
// start_y i8 @+8 (0xe0e5b0), end_x i8 @+9 (0xe0e5b1), end_y i8 @+10 (0xe0e5b2), walk_elapsed dbl @+11
// (0xe0e5b3), walk_duration dbl @+19 (0xe0e5bb). SOLDIERS_PER_PLAYER == 100 (stride 0xb54). cfg
// Unit.step_speed is double[9] indexed BY PLAYER (0x00489967 SHL 3 -> 8-byte stride).
//
#include "sim/sim_unit_soldier_anim.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

struct log_t {
    // ---- facing24_from_points (start_walk_anim only) --------------------------------------------
    int     facing_calls = 0;
    int32_t facing_last[4]{};   // from_x, from_y, to_x, to_y of the last call
    int32_t facing_seq[8][4]{}; // per-call args, up to 8 calls
    uint8_t facing_ret = 0;     // what the mock returns (-> sprite_frame)

    // ---- cursor_apply_anim_frame_offset (pick_lead only) ---------------------------------------
    int     cursor_calls     = 0;
    int32_t cursor_preseed_x = -1; // incoming *out_x the mock saw (must be 0x10 == the pre-seed)
    int32_t cursor_preseed_y = -1;
    int32_t cursor_heading   = -1;
    int8_t  cursor_write_x   = 0; // what the mock writes into out_x/out_y
    int8_t  cursor_write_y   = 0;

    // ---- manhattan_dist (pick_lead only). Returns x0 so a test can dictate each soldier's score
    // via its cur_x and exercise the STRICTLY-greater selection logic directly. ------------------
    int     manhattan_calls = 0;
    int32_t manhattan_seq[8][4]{}; // per-call x0,y0,x1,y1

    void reset() { *this = log_t{}; }
};
log_t g_log;

const unit_soldier_anim_calls &recording_calls() {
    static const unit_soldier_anim_calls c = {
        // facing24_from_points
        [](char from_x, char from_y, char to_x, char to_y) -> uint8_t {
            const int n = g_log.facing_calls;
            if (n < 8) {
                g_log.facing_seq[n][0] = (int32_t)from_x;
                g_log.facing_seq[n][1] = (int32_t)from_y;
                g_log.facing_seq[n][2] = (int32_t)to_x;
                g_log.facing_seq[n][3] = (int32_t)to_y;
            }
            g_log.facing_last[0] = (int32_t)from_x;
            g_log.facing_last[1] = (int32_t)from_y;
            g_log.facing_last[2] = (int32_t)to_x;
            g_log.facing_last[3] = (int32_t)to_y;
            ++g_log.facing_calls;
            return g_log.facing_ret;
        },
        // cursor_apply_anim_frame_offset
        [](char *out_x, char *out_y, int32_t heading) -> void {
            g_log.cursor_preseed_x = (int32_t)*out_x; // read the pre-seed BEFORE overwriting
            g_log.cursor_preseed_y = (int32_t)*out_y;
            g_log.cursor_heading   = heading;
            *out_x                 = (char)g_log.cursor_write_x;
            *out_y                 = (char)g_log.cursor_write_y;
            ++g_log.cursor_calls;
        },
        // manhattan_dist -> returns x0 (see note above)
        [](int32_t x0, int32_t y0, int32_t x1, int32_t y1) -> int32_t {
            const int n = g_log.manhattan_calls;
            if (n < 8) {
                g_log.manhattan_seq[n][0] = x0;
                g_log.manhattan_seq[n][1] = y0;
                g_log.manhattan_seq[n][2] = x1;
                g_log.manhattan_seq[n][3] = y1;
            }
            ++g_log.manhattan_calls;
            return x0;
        },
    };
    return c;
}

// ===================================================================================================
// llm_strat_unit_squad_pick_lead_soldier_in_direction @0x0048905e
//   uint __mh_watcall_ebx_volatile(int player, uint head_soldier_idx, uint heading)
//
// 0x0048907d/81: pre-seed out_x=out_y=0x10, then cursor_apply_anim_frame_offset(&out_x,&out_y,heading).
// 0x0048909b: row = player * 0xb54 (SOLDIERS_PER_PLAYER, FULL 32-bit player, no &0xffff mask).
// Score the HEAD unconditionally: best_dist = manhattan(head.cur_x, head.cur_y, out_x, out_y),
// best_idx = head. Then a plain WHILE (0x004890d7): test soldiers[row+current].next_soldier BEFORE
// advancing -- current := that next, score it, and update best ONLY on best_dist < dist (0x00489140
// JLE skip -> STRICTLY greater; ties keep the earlier soldier). Return best_idx (0x00489153).
// Pure query -- no state writes.
// ===================================================================================================

// ---- (P1) single-soldier squad: head.next_soldier==0, so the WHILE never enters. Verifies the
// cursor pre-seed (0x10), the heading pass-through, and that manhattan sees the cursor's output. ---
void test_pick_single_soldier_returns_head_and_threads_cursor_output() {
    sim_fixture    f;
    const sim_view v = f.view();

    const int32_t  player = 2;
    const uint32_t head   = 2;

    f.soldiers[player * SOLDIERS_PER_PLAYER + head].cur_x        = 5;
    f.soldiers[player * SOLDIERS_PER_PLAYER + head].cur_y        = -4; // non-symmetric
    f.soldiers[player * SOLDIERS_PER_PLAYER + head].next_soldier = 0;

    g_log.reset();
    g_log.cursor_write_x = 3;
    g_log.cursor_write_y = 7;

    const uint32_t r = detail::squad_pick_lead_soldier_in_direction(v, player, head, /*heading=*/0x55,
                                                                    recording_calls());

    ck_eq(r, head, "pick single: no next_soldier -> the WHILE never enters, head is returned");
    ck(g_log.cursor_calls == 1, "pick single: cursor_apply_anim_frame_offset called exactly once");
    ck(g_log.cursor_preseed_x == 0x10 && g_log.cursor_preseed_y == 0x10,
       "pick single: both out-params are pre-seeded to 0x10 before the cursor call");
    ck_eq((uint32_t)g_log.cursor_heading, 0x55u, "pick single: heading passed through unchanged");
    ck(g_log.manhattan_calls == 1, "pick single: manhattan scored the head exactly once, no loop body");
    ck(g_log.manhattan_seq[0][0] == 5 && g_log.manhattan_seq[0][1] == -4,
       "pick single: manhattan got the head's (cur_x,cur_y), cur_y sign-extended");
    ck(g_log.manhattan_seq[0][2] == 3 && g_log.manhattan_seq[0][3] == 7,
       "pick single: manhattan's (x1,y1) are the cursor's written (out_x,out_y)");
}

// ---- (P2) a later soldier strictly outscores the head -> it wins. -------------------------------
// head=S(2) -> S(3) -> 0. score(head)=5, score(3)=9 (mock returns cur_x). 5 < 9 -> best = 3.
void test_pick_later_soldier_strictly_greater_wins() {
    sim_fixture    f;
    const sim_view v = f.view();

    const int32_t player = 3;
    const int32_t base   = player * SOLDIERS_PER_PLAYER;

    f.soldiers[base + 2].cur_x        = 5;
    f.soldiers[base + 2].next_soldier = 3;
    f.soldiers[base + 3].cur_x        = 9;
    f.soldiers[base + 3].next_soldier = 0;

    g_log.reset();
    g_log.cursor_write_x = 1;
    g_log.cursor_write_y = 2;

    const uint32_t r = detail::squad_pick_lead_soldier_in_direction(v, player, /*head=*/2, 0, recording_calls());

    ck_eq(r, 3u, "pick later-wins: soldier 3 (score 9) strictly beats the head (score 5) -> returns 3");
    ck(g_log.manhattan_calls == 2, "pick later-wins: head + one loop soldier were both scored");
}

// ---- (P3) tie between head and a later soldier -> the head is kept (best_dist < dist is FALSE on a
// tie -- the JLE polarity). ------------------------------------------------------------------------
void test_pick_tie_keeps_head() {
    sim_fixture    f;
    const sim_view v = f.view();

    const int32_t player = 2;
    const int32_t base   = player * SOLDIERS_PER_PLAYER;

    f.soldiers[base + 2].cur_x        = 9;
    f.soldiers[base + 2].next_soldier = 3;
    f.soldiers[base + 3].cur_x        = 9; // equal score -> tie
    f.soldiers[base + 3].next_soldier = 0;

    g_log.reset();
    const uint32_t r = detail::squad_pick_lead_soldier_in_direction(v, player, /*head=*/2, 0, recording_calls());

    ck_eq(r, 2u, "pick tie: equal scores -> STRICTLY-greater fails, the earlier (head) is kept -> returns 2");
}

// ---- (P4) three soldiers, a later one ties the running max -> the FIRST to reach the max is kept. -
// head=S(2) -> S(3) -> S(4) -> 0. scores 2, 8, 8. best: head=2; 2<8 -> best=3; 8<8 false -> best stays 3.
void test_pick_three_keeps_first_of_tied_max() {
    sim_fixture    f;
    const sim_view v = f.view();

    const int32_t player = 3;
    const int32_t base   = player * SOLDIERS_PER_PLAYER;

    f.soldiers[base + 2].cur_x        = 2;
    f.soldiers[base + 2].next_soldier = 3;
    f.soldiers[base + 3].cur_x        = 8;
    f.soldiers[base + 3].next_soldier = 4;
    f.soldiers[base + 4].cur_x        = 8; // ties soldier 3's max, but comes later
    f.soldiers[base + 4].next_soldier = 0;

    g_log.reset();
    const uint32_t r = detail::squad_pick_lead_soldier_in_direction(v, player, /*head=*/2, 0, recording_calls());

    ck_eq(r, 3u, "pick three: soldier 3 reaches the max (8) first; soldier 4 ties but does not exceed -> 3");
    ck(g_log.manhattan_calls == 3, "pick three: head + two loop soldiers scored");
}

// ---- (P5) negative cur_x: verifies the MOVSX sign-extension of cur_x/cur_y into manhattan's args
// (0x004890a8/0x004890bc are MOVSX). head score -5 beats -9, head kept. ---------------------------
void test_pick_sign_extension_of_cur_coords() {
    sim_fixture    f;
    const sim_view v = f.view();

    const int32_t player = 2;
    const int32_t base   = player * SOLDIERS_PER_PLAYER;

    f.soldiers[base + 2].cur_x        = (int8_t)-5;
    f.soldiers[base + 2].cur_y        = (int8_t)-3;
    f.soldiers[base + 2].next_soldier = 3;
    f.soldiers[base + 3].cur_x        = (int8_t)-9; // more negative -> smaller score
    f.soldiers[base + 3].next_soldier = 0;

    g_log.reset();
    const uint32_t r = detail::squad_pick_lead_soldier_in_direction(v, player, /*head=*/2, 0, recording_calls());

    ck_eq(r, 2u, "pick sign: head score -5 > -9 -> head kept");
    ck(g_log.manhattan_seq[0][0] == -5 && g_log.manhattan_seq[0][1] == -3,
       "pick sign: head's cur_x/cur_y reached manhattan sign-extended (-5,-3), not zero-extended");
    ck(g_log.manhattan_seq[1][0] == -9,
       "pick sign: the loop soldier's cur_x reached manhattan as -9, sign-extended");
}

// ===================================================================================================
// llm_strat_unit_soldiers_start_walk_anim @0x004897c7  void __watcall(uint player, int unit_index)
//
// Head-seeded DO-WHILE over unit.unit_above's chain (visits record[0] once even for an empty chain).
// For each visited soldier, enter the body iff (end_x!=start_x || end_y!=start_y) AND
// walk_duration==0.0 (the 0x00489874 TEST high&0x7fffffff / 0x00489880 CMP low,0 -- raw integer
// magnitude-zero test, NOT an x87 compare). Body: sprite_frame = facing24(start_x,start_y,end_x,end_y);
// walk_elapsed = 0.0; walk_duration = (|end_x-start_x|+|end_y-start_y|) *
// cfg_units[unit_proto_id].step_speed[player] * 2.0 (x87 left-to-right).
// ===================================================================================================

// ---- (W1) one qualifying soldier: writes sprite_frame, zeroes walk_elapsed, sets walk_duration. --
void test_walk_single_qualifying_soldier() {
    sim_fixture    f;
    const sim_view v   = f.view();
    sim_store      own = f.store();

    const uint32_t p          = 2;
    const int32_t  unit_index = 4;

    f.u(p, unit_index).unit_above[0] = 1; // head = soldier idx 1
    f.u(p, unit_index).unit_above[1] = 0;
    f.u(p, unit_index).unit_proto_id = 4;

    soldier &s      = own.soldier_at(p, 1);
    s.start_x       = 2;
    s.start_y       = 3;
    s.end_x         = 5; // end != start -> condition (a) true
    s.end_y         = 8;
    s.walk_duration = 0.0;   // idle -> condition (b) true
    s.walk_elapsed  = 123.0; // sentinel: must become 0.0
    s.sprite_frame  = 0x11;  // sentinel: must become facing_ret
    s.next_soldier  = 0;

    // step_speed indexed BY PLAYER: seed distinct values so a wrong-player index would show.
    for (int i = 0; i < 9; ++i) f.cfg_units[4].step_speed[i] = 9.0;
    f.cfg_units[4].step_speed[p] = 0.5;

    g_log.reset();
    g_log.facing_ret = 0x2a;
    detail::unit_soldiers_start_walk_anim(v, own, recording_calls(), p, unit_index);

    // dist = |5-2| + |8-3| = 3 + 5 = 8; walk_duration = 8 * 0.5 * 2.0 = 8.0 (all exact).
    ck_eq_d(s.walk_duration, 8.0, "walk single: walk_duration = dist(8) * step_speed(0.5) * 2.0 = 8.0");
    ck_eq_d(s.walk_elapsed, 0.0, "walk single: walk_elapsed zeroed");
    ck_eq((uint32_t)(uint8_t)s.sprite_frame, 0x2au, "walk single: sprite_frame := facing24 return");
    ck(g_log.facing_calls == 1, "walk single: facing24_from_points called once");
    ck(g_log.facing_last[0] == 2 && g_log.facing_last[1] == 3 && g_log.facing_last[2] == 5 &&
           g_log.facing_last[3] == 8,
       "walk single: facing24 got (start_x,start_y,end_x,end_y) = (2,3,5,8)");
}

// ---- (W2) end==start -> condition (a) false -> body skipped, no facing call, fields unchanged. ---
void test_walk_end_equals_start_skips() {
    sim_fixture    f;
    const sim_view v   = f.view();
    sim_store      own = f.store();

    const uint32_t p          = 3;
    const int32_t  unit_index = 5;

    f.u(p, unit_index).unit_above[0] = 1;
    f.u(p, unit_index).unit_proto_id = 4;

    soldier &s      = own.soldier_at(p, 1);
    s.start_x       = 4;
    s.start_y       = 6;
    s.end_x         = 4; // end == start on BOTH axes -> skip
    s.end_y         = 6;
    s.walk_duration = 0.0;
    s.walk_elapsed  = 99.0; // sentinel: must survive
    s.sprite_frame  = 0x22; // sentinel: must survive
    s.next_soldier  = 0;

    g_log.reset();
    g_log.facing_ret = 0x2a;
    detail::unit_soldiers_start_walk_anim(v, own, recording_calls(), p, unit_index);

    ck(g_log.facing_calls == 0, "walk end==start: body skipped -> facing24 never called");
    ck_eq_d(s.walk_elapsed, 99.0, "walk end==start: walk_elapsed untouched");
    ck_eq((uint32_t)(uint8_t)s.sprite_frame, 0x22u, "walk end==start: sprite_frame untouched");
    ck_eq_d(s.walk_duration, 0.0, "walk end==start: walk_duration untouched");
}

// ---- (W3) already walking (walk_duration != 0.0) -> condition (b) false -> body skipped even
// though end != start. -----------------------------------------------------------------------------
void test_walk_already_walking_skips() {
    sim_fixture    f;
    const sim_view v   = f.view();
    sim_store      own = f.store();

    const uint32_t p          = 2;
    const int32_t  unit_index = 7;

    f.u(p, unit_index).unit_above[0] = 1;
    f.u(p, unit_index).unit_proto_id = 4;

    soldier &s      = own.soldier_at(p, 1);
    s.start_x       = 2;
    s.start_y       = 3;
    s.end_x         = 5; // end != start -> (a) true...
    s.end_y         = 8;
    s.walk_duration = 7.5; // ...but non-zero -> (b) false -> skip
    s.walk_elapsed  = 44.0;
    s.sprite_frame  = 0x33;
    s.next_soldier  = 0;

    g_log.reset();
    detail::unit_soldiers_start_walk_anim(v, own, recording_calls(), p, unit_index);

    ck(g_log.facing_calls == 0, "walk already-walking: walk_duration!=0.0 -> body skipped, no facing24 call");
    ck_eq_d(s.walk_duration, 7.5, "walk already-walking: walk_duration untouched (7.5)");
    ck_eq_d(s.walk_elapsed, 44.0, "walk already-walking: walk_elapsed untouched");
    ck_eq((uint32_t)(uint8_t)s.sprite_frame, 0x33u, "walk already-walking: sprite_frame untouched");
}

// ---- (W4) empty chain: FINDING -- the DO-WHILE has no head!=0 guard, so it visits record[0] once.
// If record[0] qualifies, its walk fields are written -- but owner_unit (the per-player COUNT, a
// DIFFERENT field) is NOT corrupted. --------------------------------------------------------------
void test_walk_empty_chain_visits_record0() {
    sim_fixture    f;
    const sim_view v   = f.view();
    sim_store      own = f.store();

    const uint32_t p          = 5;
    const int32_t  unit_index = 2;

    f.u(p, unit_index).unit_above[0] = 0; // empty chain -> head = 0
    f.u(p, unit_index).unit_above[1] = 0;
    f.u(p, unit_index).unit_proto_id = 4;

    soldier &r0      = own.soldier_at(p, 0);
    r0.owner_unit    = 5; // the per-player COUNT sentinel -- must survive
    r0.start_x       = 1;
    r0.start_y       = 1;
    r0.end_x         = 2;
    r0.end_y         = 2;
    r0.walk_duration = 0.0;
    r0.walk_elapsed  = 77.0;
    r0.sprite_frame  = 0x44;
    r0.next_soldier  = 0; // must be 0 so the do-while stops after visiting record[0]

    for (int i = 0; i < 9; ++i) f.cfg_units[4].step_speed[i] = 9.0;
    f.cfg_units[4].step_speed[p] = 0.5;

    g_log.reset();
    g_log.facing_ret = 0x2a;
    detail::unit_soldiers_start_walk_anim(v, own, recording_calls(), p, unit_index);

    // dist = |2-1| + |2-1| = 2; walk_duration = 2 * 0.5 * 2.0 = 2.0.
    ck(g_log.facing_calls == 1, "walk empty: FINDING -- record[0] is still visited once (no head!=0 guard)");
    ck_eq_d(r0.walk_duration, 2.0, "walk empty: record[0].walk_duration written = 2 * 0.5 * 2.0 = 2.0");
    ck_eq_d(r0.walk_elapsed, 0.0, "walk empty: record[0].walk_elapsed zeroed");
    ck_eq((uint32_t)(uint8_t)r0.sprite_frame, 0x2au, "walk empty: record[0].sprite_frame stamped");
    ck_eq((uint32_t)(uint16_t)r0.owner_unit, 5u,
          "walk empty: FINDING -- the COUNT (record[0].owner_unit, a different field) is NOT corrupted");
}

// ---- (W5) multi-soldier chain: iterates the whole chain, processing only the qualifying soldiers
// and skipping the middle one; verifies the facing24 call ORDER matches chain order. --------------
// head=S1(1) -> S2(2) -> S3(3) -> 0. S1 qualifies, S2 end==start (skip), S3 qualifies.
void test_walk_multi_chain_selective_and_ordered() {
    sim_fixture    f;
    const sim_view v   = f.view();
    sim_store      own = f.store();

    const uint32_t p          = 3;
    const int32_t  unit_index = 6;

    f.u(p, unit_index).unit_above[0] = 1; // head = S1
    f.u(p, unit_index).unit_proto_id = 4;

    soldier &s1      = own.soldier_at(p, 1);
    s1.start_x       = 0;
    s1.start_y       = 0;
    s1.end_x         = 1;
    s1.end_y         = 0; // dist 1, qualifies
    s1.walk_duration = 0.0;
    s1.next_soldier  = 2;

    soldier &s2      = own.soldier_at(p, 2);
    s2.start_x       = 3;
    s2.start_y       = 3;
    s2.end_x         = 3;
    s2.end_y         = 3; // end==start -> skip
    s2.walk_duration = 0.0;
    s2.sprite_frame  = 0x55;
    s2.next_soldier  = 3;

    soldier &s3      = own.soldier_at(p, 3);
    s3.start_x       = 0;
    s3.start_y       = 0;
    s3.end_x         = 0;
    s3.end_y         = 2; // dist 2, qualifies
    s3.walk_duration = 0.0;
    s3.next_soldier  = 0;

    for (int i = 0; i < 9; ++i) f.cfg_units[4].step_speed[i] = 9.0;
    f.cfg_units[4].step_speed[p] = 0.5;

    g_log.reset();
    g_log.facing_ret = 0x2a;
    detail::unit_soldiers_start_walk_anim(v, own, recording_calls(), p, unit_index);

    ck(g_log.facing_calls == 2, "walk multi: only the two qualifying soldiers (S1, S3) triggered facing24");
    ck(g_log.facing_seq[0][0] == 0 && g_log.facing_seq[0][1] == 0 && g_log.facing_seq[0][2] == 1 &&
           g_log.facing_seq[0][3] == 0,
       "walk multi: first facing24 call is S1's (0,0,1,0) -- chain order preserved");
    ck(g_log.facing_seq[1][0] == 0 && g_log.facing_seq[1][1] == 0 && g_log.facing_seq[1][2] == 0 &&
           g_log.facing_seq[1][3] == 2,
       "walk multi: second facing24 call is S3's (0,0,0,2) -- the middle S2 was skipped");
    ck_eq_d(s1.walk_duration, 1.0, "walk multi: S1 walk_duration = 1 * 0.5 * 2.0 = 1.0");
    ck_eq_d(s3.walk_duration, 2.0, "walk multi: S3 walk_duration = 2 * 0.5 * 2.0 = 2.0");
    ck_eq_d(s2.walk_duration, 0.0, "walk multi: skipped S2 walk_duration untouched");
    ck_eq((uint32_t)(uint8_t)s2.sprite_frame, 0x55u, "walk multi: skipped S2 sprite_frame untouched");
}

} // namespace

void run_unit_soldier_anim_tests() {
    test_pick_single_soldier_returns_head_and_threads_cursor_output();
    test_pick_later_soldier_strictly_greater_wins();
    test_pick_tie_keeps_head();
    test_pick_three_keeps_first_of_tied_max();
    test_pick_sign_extension_of_cur_coords();
    test_walk_single_qualifying_soldier();
    test_walk_end_equals_start_skips();
    test_walk_already_walking_skips();
    test_walk_empty_chain_visits_record0();
    test_walk_multi_chain_selective_and_ordered();
}

} // namespace mh::sim::test
