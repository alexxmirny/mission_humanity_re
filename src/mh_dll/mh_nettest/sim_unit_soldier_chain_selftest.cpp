//
// sim_unit_soldier_chain_selftest.cpp -- `simtest` cases for llm_strat_unit_soldier_remove_last
// (@0x00489595) and llm_strat_unit_soldier_unlink (@0x0048967b), sim/sim_unit_soldier_chain.h/.cpp,
//
// NEITHER function has callees (the only CALL in either body is the inert
// utils_assert_stack_capacity prologue), so there is no recording calls struct here -- every case
// asserts the soldier-chain array mutations and the unit record directly.
//
// EVERY expected value below was derived from the DISASSEMBLY, not the translation:
//   tmp/decomp/llm_strat_unit_soldier_remove_last_00489595.asm
//   tmp/decomp/llm_strat_unit_soldier_unlink_0048967b.asm
// The soldier record (stride 0x1d) is laid out, from the raw displacements the asm touches (base
// 0xe0e5a8): owner_unit int16_t @+0 (in record [0] this doubles as the per-player live-soldier
// COUNT), next_soldier uint16_t @+2 (0xe0e5aa). Player stride is 0xb54 == 100*0x1d, i.e.
// SOLDIERS_PER_PLAYER == 100. unit.unit_above is a uint8_t[2] reassembled little-endian.
//
#include "sim/sim_unit_soldier_chain.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// A soldier's next_soldier / owner_unit, read back as the raw stored 16-bit patterns.
uint32_t next_of(sim_store &own, uint32_t p, int32_t idx) {
    return (uint32_t)own.soldier_at(p, idx).next_soldier;
}
uint32_t owner_of(sim_store &own, uint32_t p, int32_t idx) {
    return (uint32_t)(uint16_t)own.soldier_at(p, idx).owner_unit;
}

// ===================================================================================================
// llm_strat_unit_soldier_remove_last @0x00489595
//
// Walks the chain (head = unit.unit_above) to the TAIL via a do-while carrying two lagging cursors,
// prev1 (one behind the walker) and prev2 (two behind). On exit: prev2.next_soldier=0 (0x00489626),
// prev1.next_soldier=0 (0x0048963f), prev1.owner_unit=0 (0x00489658), record[0].owner_unit -= 1
// (0x0048966b, DEC word -- the per-player count). The function NEVER writes unit.unit_above.
// ===================================================================================================

// ---- (A) length-2 chain: the normal case the plate describes without a caveat. ------------------
// head=S1(idx1) -> S2(idx2) -> 0. do-while: iter1 prev2=0,prev1=1,cur=S1.next=2 (!=0); iter2
// prev2=1,prev1=2,cur=S2.next=0 (stop). So prev2=1 (S1), prev1=2 (S2, the tail).
void test_remove_last_len2_detaches_tail_and_decrements_count() {
    sim_fixture    f;
    const sim_view v   = f.view();
    sim_store      own = f.store();

    const uint32_t p          = 2;
    const int32_t  unit_index = 4;

    f.u(p, unit_index).unit_above[0] = 1; // head = soldier idx 1 (LE low byte)
    f.u(p, unit_index).unit_above[1] = 0;

    own.soldier_at(p, 0).owner_unit   = 5;  // the per-player live-soldier COUNT sentinel
    own.soldier_at(p, 1).owner_unit   = 51; // S1 owning-unit id -- NON-tail, must survive
    own.soldier_at(p, 1).next_soldier = 2;
    own.soldier_at(p, 2).owner_unit   = 52; // S2 is the tail -- gets blanked
    own.soldier_at(p, 2).next_soldier = 0;

    detail::unit_soldier_remove_last(v, own, p, unit_index);

    ck_eq(next_of(own, p, 1), 0u,
          "remove_last len2: S1(prev2).next_soldier zeroed -- the tail is detached from its predecessor");
    ck_eq(next_of(own, p, 2), 0u, "remove_last len2: S2(prev1, the tail).next_soldier zeroed");
    ck_eq(owner_of(own, p, 2), 0u, "remove_last len2: S2(the tail).owner_unit blanked to 0");
    ck_eq(owner_of(own, p, 1), 51u,
          "remove_last len2: S1(the survivor).owner_unit is NOT touched (only prev1==tail is blanked)");
    ck_eq(owner_of(own, p, 0), 4u, "remove_last len2: record[0].owner_unit (the count) decremented 5 -> 4");
    // unit_above is never written by this function.
    ck_eq((uint32_t)f.u(p, unit_index).unit_above[0], 1u,
          "remove_last len2: unit.unit_above is never written -- still names S1");
}

// ---- (B) length-1 chain: FINDING -- prev2 stays at record 0, so the "detach" write lands on
// record[0].next_soldier (a no-op on the real chain) and unit.unit_above is left STALE, still
// pointing at the now-blanked sole soldier. Faithful to the original, not repaired. ---------------
// head=S1(idx1) -> 0. do-while: iter1 prev2=0,prev1=1,cur=S1.next=0 (stop). prev2=0, prev1=1.
void test_remove_last_len1_detach_lands_on_record0_and_leaves_unit_above_stale() {
    sim_fixture    f;
    const sim_view v   = f.view();
    sim_store      own = f.store();

    const uint32_t p          = 3;
    const int32_t  unit_index = 6;

    f.u(p, unit_index).unit_above[0] = 1; // head = soldier idx 1
    f.u(p, unit_index).unit_above[1] = 0;

    own.soldier_at(p, 0).owner_unit   = 5; // count
    own.soldier_at(p, 0).next_soldier = 9; // sentinel: the prev2==0 detach write must zero THIS
    own.soldier_at(p, 1).owner_unit   = 71;
    own.soldier_at(p, 1).next_soldier = 0;

    detail::unit_soldier_remove_last(v, own, p, unit_index);

    ck_eq(next_of(own, p, 0), 0u,
          "remove_last len1: FINDING -- prev2==record0, so the detach write zeroes record[0].next_soldier "
          "(a no-op on the real chain), NOT unit.unit_above");
    ck_eq(next_of(own, p, 1), 0u, "remove_last len1: S1(prev1).next_soldier zeroed");
    ck_eq(owner_of(own, p, 1), 0u, "remove_last len1: S1(prev1).owner_unit blanked");
    ck_eq(owner_of(own, p, 0), 4u, "remove_last len1: count decremented 5 -> 4");
    ck_eq((uint32_t)f.u(p, unit_index).unit_above[0], 1u,
          "remove_last len1: FINDING -- unit.unit_above left STALE, still pointing at the blanked S1");
}

// ---- (C) length-0 (empty) chain: FINDING -- a genuine original bug. head==0, so both cursors end
// at record 0 and record[0].owner_unit is written 0 (0x00489658) THEN decremented (0x0048966b) to
// -1 (0xffff), corrupting the per-player count. Transcribed as written. -------------------------
// head=0. do-while: iter1 prev2=0,prev1=0,cur=record[0].next=0 (stop). prev2=0, prev1=0.
void test_remove_last_empty_chain_corrupts_count_to_minus_one() {
    sim_fixture    f;
    const sim_view v   = f.view();
    sim_store      own = f.store();

    const uint32_t p          = 5;
    const int32_t  unit_index = 2;

    f.u(p, unit_index).unit_above[0] = 0; // empty chain, head = 0
    f.u(p, unit_index).unit_above[1] = 0;

    own.soldier_at(p, 0).owner_unit   = 5; // count
    own.soldier_at(p, 0).next_soldier = 0; // must be 0 so the do-while stops after one iteration

    detail::unit_soldier_remove_last(v, own, p, unit_index);

    // owner_unit written 0 then DEC word -> -1 == 0xffff as the stored int16_t.
    ck_eq(owner_of(own, p, 0), 0xffffu,
          "remove_last empty: FINDING (original bug) -- record[0].owner_unit set 0 then decremented to "
          "-1 (0xffff), corrupting the per-player count");
    ck_eq(next_of(own, p, 0), 0u, "remove_last empty: record[0].next_soldier zeroed");
}

// ===================================================================================================
// llm_strat_unit_soldier_unlink @0x0048967b
//
// Removes the NAMED soldier_idx from unit_idx's chain. head = unit.unit_above (read via the writable
// unit record -- own.unit_at, because the HEAD case writes it back).
//   HEAD (head==soldier_idx, 0x004896d6): unit.unit_above := soldier[head].next_soldier (a 16-bit
//     copy, split little-endian into the two unit_above bytes -- 0x00489700), then fall to the tail.
//   ELSE (0x00489709): walker=head; scan forward (NO bound, NO null check) while
//     soldier[walker].next_soldier != soldier_idx; then soldier[walker].next_soldier :=
//     soldier[soldier_idx].next_soldier (splice out, 0x0048976e).
//   TAIL (unconditional, 0x0048977c): soldier[soldier_idx].next_soldier=0,
//     soldier[soldier_idx].owner_unit=0, record[0].owner_unit -= 1.
// ===================================================================================================

// ---- (D) HEAD case: soldier_idx IS the chain head. unit.unit_above is rewritten to the removed
// head's next_soldier. Uses a next value > 255 to exercise the little-endian byte split. ---------
void test_unlink_head_rewrites_unit_above_little_endian() {
    sim_fixture    f;
    const sim_view v   = f.view();
    sim_store      own = f.store();

    const uint16_t player   = 2;
    const int32_t  unit_idx = 7;
    const uint32_t p        = player;

    f.u(p, unit_idx).unit_above[0] = 2; // head = soldier idx 2 == soldier_idx below
    f.u(p, unit_idx).unit_above[1] = 0;

    own.soldier_at(p, 0).owner_unit   = 6;      // count
    own.soldier_at(p, 2).next_soldier = 0x0102; // new head value 258 -> exercises the LE split
    own.soldier_at(p, 2).owner_unit   = 72;
    own.soldier_at(p, 5).next_soldier = 0x3333; // untouched sentinel record
    own.soldier_at(p, 5).owner_unit   = 0x7777;

    detail::unit_soldier_unlink(v, own, player, unit_idx, /*soldier_idx=*/2);

    ck_eq((uint32_t)f.u(p, unit_idx).unit_above[0], 0x02u,
          "unlink HEAD: unit_above[0] = low byte of removed head's next_soldier (0x0102 -> 0x02)");
    ck_eq((uint32_t)f.u(p, unit_idx).unit_above[1], 0x01u,
          "unlink HEAD: unit_above[1] = high byte of removed head's next_soldier (0x0102 -> 0x01)");
    ck_eq(next_of(own, p, 2), 0u, "unlink HEAD: removed soldier's next_soldier blanked in the tail");
    ck_eq(owner_of(own, p, 2), 0u, "unlink HEAD: removed soldier's owner_unit blanked in the tail");
    ck_eq(owner_of(own, p, 0), 5u, "unlink HEAD: count decremented 6 -> 5");
    ck_eq(next_of(own, p, 5), 0x3333u, "unlink HEAD: unrelated records untouched");
    ck_eq(owner_of(own, p, 5), 0x7777u, "unlink HEAD: unrelated records untouched");
}

// ---- (E) ELSE case, immediate predecessor: soldier_idx sits right after the head. The scan's loop
// condition (soldier[walker].next_soldier != soldier_idx) is already false at walker==head, so the
// splice uses walker==head with NO loop iterations. unit.unit_above must NOT be written. ----------
// head=S1(1) -> S3(3) -> S7(7) -> 0. unlink 3. splice: S1.next := S3.next(7).
void test_unlink_else_immediate_predecessor_splices_and_leaves_unit_above() {
    sim_fixture    f;
    const sim_view v   = f.view();
    sim_store      own = f.store();

    const uint16_t player   = 3;
    const int32_t  unit_idx = 1;
    const uint32_t p        = player;

    f.u(p, unit_idx).unit_above[0] = 1; // head = S1
    f.u(p, unit_idx).unit_above[1] = 0;

    own.soldier_at(p, 0).owner_unit   = 9; // count
    own.soldier_at(p, 1).next_soldier = 3;
    own.soldier_at(p, 1).owner_unit   = 61;
    own.soldier_at(p, 3).next_soldier = 7; // the value that gets spliced into S1.next
    own.soldier_at(p, 3).owner_unit   = 63;
    own.soldier_at(p, 7).next_soldier = 0;
    own.soldier_at(p, 7).owner_unit   = 67; // untouched (not the removed one)

    detail::unit_soldier_unlink(v, own, player, unit_idx, /*soldier_idx=*/3);

    ck_eq(next_of(own, p, 1), 7u, "unlink ELSE(imm): S1.next_soldier spliced to S3's next (7), skipping S3");
    ck_eq(next_of(own, p, 3), 0u, "unlink ELSE(imm): removed S3.next_soldier blanked");
    ck_eq(owner_of(own, p, 3), 0u, "unlink ELSE(imm): removed S3.owner_unit blanked");
    ck_eq(owner_of(own, p, 7), 67u, "unlink ELSE(imm): the successor S7 is untouched");
    ck_eq(owner_of(own, p, 0), 8u, "unlink ELSE(imm): count decremented 9 -> 8");
    ck_eq((uint32_t)f.u(p, unit_idx).unit_above[0], 1u,
          "unlink ELSE(imm): the ELSE branch NEVER writes unit.unit_above -- still names S1");
}

// ---- (F) ELSE case, multi-hop scan: the predecessor is two links in, so the forward scan actually
// advances the walker before finding the match. --------------------------------------------------
// head=S1(1) -> S4(4) -> S6(6) -> 0. unlink 6. scan: walker=1 (S1.next=4 != 6) -> walker=4
// (S4.next=6 == 6, stop). splice: S4.next := S6.next(0).
void test_unlink_else_multi_hop_scan_advances_walker() {
    sim_fixture    f;
    const sim_view v   = f.view();
    sim_store      own = f.store();

    const uint16_t player   = 4;
    const int32_t  unit_idx = 9;
    const uint32_t p        = player;

    f.u(p, unit_idx).unit_above[0] = 1; // head = S1
    f.u(p, unit_idx).unit_above[1] = 0;

    own.soldier_at(p, 0).owner_unit   = 12; // count
    own.soldier_at(p, 1).next_soldier = 4;
    own.soldier_at(p, 4).next_soldier = 6;
    own.soldier_at(p, 4).owner_unit   = 44; // the predecessor found after one hop -- survives
    own.soldier_at(p, 6).next_soldier = 0;
    own.soldier_at(p, 6).owner_unit   = 66; // the removed one

    detail::unit_soldier_unlink(v, own, player, unit_idx, /*soldier_idx=*/6);

    ck_eq(next_of(own, p, 4), 0u,
          "unlink ELSE(multi): after advancing walker 1 -> 4, S4.next spliced to S6's next (0)");
    ck_eq(next_of(own, p, 1), 4u, "unlink ELSE(multi): S1(before the predecessor) is untouched");
    ck_eq(owner_of(own, p, 4), 44u, "unlink ELSE(multi): the predecessor's owner_unit is untouched");
    ck_eq(next_of(own, p, 6), 0u, "unlink ELSE(multi): removed S6.next_soldier blanked");
    ck_eq(owner_of(own, p, 6), 0u, "unlink ELSE(multi): removed S6.owner_unit blanked");
    ck_eq(owner_of(own, p, 0), 11u, "unlink ELSE(multi): count decremented 12 -> 11");
}

} // namespace

void run_unit_soldier_chain_tests() {
    test_remove_last_len2_detaches_tail_and_decrements_count();
    test_remove_last_len1_detach_lands_on_record0_and_leaves_unit_above_stale();
    test_remove_last_empty_chain_corrupts_count_to_minus_one();
    test_unlink_head_rewrites_unit_above_little_endian();
    test_unlink_else_immediate_predecessor_splices_and_leaves_unit_above();
    test_unlink_else_multi_hop_scan_advances_walker();
}

} // namespace mh::sim::test
