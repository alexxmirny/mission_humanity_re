//
// tact_owner_enqueue_fix_selftest.cpp -- offline oracle pinning FOUR of the FIVE byte-verified fixes
// landed 2026-09-02 in tact/tact_unit_enqueue_command.cpp and tact/tact_unit_owner_tick.cpp (the
// TACT1-P red audit). These bugs survived T1/T2 review precisely because no oracle covered the exact
// distinctions below -- this file exists so they cannot silently regress.
//
// THE FIFTH BEHAVIOR (llm_tact_unit_owner_tick's status&8 / uninterruptible-live-command guard
// ABORTING THE WHOLE FUNCTION, not just the current unit, @0x0043367c -> epilogue 0x00433c03) IS
// **NOT** PINNED HERE, and deliberately so, not by oversight: `detail::unit_owner_tick` calls
// `mh::call::llm_tact_tile_rebuild_occupancy_layer_for_map` UNCONDITIONALLY at step 0
// (tact_unit_owner_tick.cpp:143, @0x004335bb-0x004335d4), before the per-unit loop even starts, and
// that callee is a bare `mh::call::` FRONTIER binding (mh_calls.gen.h:475,
// `detail::s_void_EAX(0x0043356eu, ...)` -- a direct jump to the real game VA), not a mockable
// entry threaded through a `_calls` struct the way `llm_tact_unit_weapons_tick` takes
// `unit_weapons_tick_calls`. `unit_owner_tick` has NO such struct (tact_unit_owner_tick.h/.cpp both
// take only `(v, own, owner)`) -- every one of its outward calls (the occupancy rebuild, the FOV
// probe, weapon-in-range, calc_dir24, the enqueue re-entry, calc_approach_dir24, time_GetCurrentTime,
// llm_rand) is an unmocked frontier call. So there is no way to drive `unit_owner_tick` offline in
// `net_selftest.exe` without crashing into an unmapped VA -- confirmed by reading this file's own
// fixture (tact_test_support.h) and the exemplar oracle (tact_cmd_queue_wrap_selftest.cpp), neither
// of which mocks or tolerates these particular callees. Per the brief for this file: pin only what is
// actually offline-safe, not a vacuous case that "passes" by never reaching the guard at all. The
// abort-vs-skip distinction remains covered only by the rig-level determinism proof
// (frame-2925 unit-10, TACT1-P POZ3 A/B) cited in tact_unit_owner_tick.cpp/.h's own comments.
//
// THE FOUR BEHAVIORS PINNED BELOW are all in `detail::unit_enqueue_command`
// (tact/tact_unit_enqueue_command.cpp), which the "OFFLINE SAFETY" note in
// tact_cmd_queue_wrap_selftest.cpp already establishes is safe for any op outside {9, 0x1f, 0x1d} --
// every case here uses op 6 or 0x46 or 0x7f, none of which reach a frontier call.
//
// Fixture/seed conventions copied from tact_cmd_queue_wrap_selftest.cpp: DISTINCT, non-symmetric
// per-field/per-slot values everywhere, so a swap or a wrong-slot write is directly observable, and
// vectors sized with parentheses never braces (tact_test_support.h's own FIXTURE RULES banner).
//
#include "tact/tact_unit_enqueue_command.h"

#include "tact_test_support.h"

#include <cstdint>
#include <cstdio>

namespace mh::tact::test {

namespace {
using namespace mh::tact;

constexpr int32_t UNIT_IDX = 77; // distinctive, mid-range TACT_UNIT_SLOTS (129) index

// ---- the op==0x46 clear-case seed formula (same shape as tact_cmd_queue_wrap_selftest.cpp's
// seed_queue/ck_entry, duplicated here per-file rather than shared -- these translation units are
// deliberately independent, see that file's own banner on why the suite was split per-stem). --------
uint8_t  seed_if(int i) { return (uint8_t)(0x10 + i); }
uint16_t seed_op(int i) { return (uint16_t)(0x1001 + i); }
uint16_t seed_a0(int i) { return (uint16_t)(0x2001 + i); }
uint16_t seed_a1(int i) { return (uint16_t)(0x3001 + i); }
uint16_t seed_a2(int i) { return (uint16_t)(0x4001 + i); }
uint16_t seed_a3(int i) { return (uint16_t)(0x5001 + i); }

void seed_queue(tact_unit &u) {
    for (int i = 0; i < 128; ++i) {
        u.cmd_queue[i].interrupt_flag = seed_if(i);
        u.cmd_queue[i].op             = seed_op(i);
        u.cmd_queue[i].arg0           = seed_a0(i);
        u.cmd_queue[i].arg1           = seed_a1(i);
        u.cmd_queue[i].arg2           = seed_a2(i);
        u.cmd_queue[i].arg3           = seed_a3(i);
    }
}

// Slot `head` must still hold its ORIGINAL seed in full (the clear preserves it).
void ck_slot_untouched(const mh::game::mh_llm_tact_unit_cmd_entry &got, int slot, const char *tag) {
    char buf[192];
    std::snprintf(buf, sizeof buf, "%s: HEAD slot %d interrupt_flag preserved", tag, slot);
    ck_eq(got.interrupt_flag, seed_if(slot), buf);
    std::snprintf(buf, sizeof buf, "%s: HEAD slot %d op preserved (NOT cleared, @0x0042b616 "
                                   "pre-increment skips it)",
                  tag, slot);
    ck_eq(got.op, seed_op(slot), buf);
    std::snprintf(buf, sizeof buf, "%s: HEAD slot %d arg0 preserved", tag, slot);
    ck_eq(got.arg0, seed_a0(slot), buf);
    std::snprintf(buf, sizeof buf, "%s: HEAD slot %d arg1 preserved", tag, slot);
    ck_eq(got.arg1, seed_a1(slot), buf);
    std::snprintf(buf, sizeof buf, "%s: HEAD slot %d arg2 preserved", tag, slot);
    ck_eq(got.arg2, seed_a2(slot), buf);
    std::snprintf(buf, sizeof buf, "%s: HEAD slot %d arg3 preserved", tag, slot);
    ck_eq(got.arg3, seed_a3(slot), buf);
}

// Every OTHER slot: .op zeroed by the clear, every other field untouched (the loop body at
// 0x0042b639 stores only the .op word, per-slot -- it never zeroes interrupt_flag/arg0..arg3).
void ck_slot_op_cleared(const mh::game::mh_llm_tact_unit_cmd_entry &got, int slot, const char *tag) {
    char buf[192];
    std::snprintf(buf, sizeof buf, "%s: slot %d interrupt_flag untouched by the clear", tag, slot);
    ck_eq(got.interrupt_flag, seed_if(slot), buf);
    std::snprintf(buf, sizeof buf, "%s: slot %d op zeroed, @0x0042b639", tag, slot);
    ck_eq(got.op, 0u, buf);
    std::snprintf(buf, sizeof buf, "%s: slot %d arg0 untouched (clear writes .op only)", tag, slot);
    ck_eq(got.arg0, seed_a0(slot), buf);
    std::snprintf(buf, sizeof buf, "%s: slot %d arg1 untouched", tag, slot);
    ck_eq(got.arg1, seed_a1(slot), buf);
    std::snprintf(buf, sizeof buf, "%s: slot %d arg2 untouched", tag, slot);
    ck_eq(got.arg2, seed_a2(slot), buf);
    std::snprintf(buf, sizeof buf, "%s: slot %d arg3 untouched", tag, slot);
    ck_eq(got.arg3, seed_a3(slot), buf);
}

// Shared body for the two op==0x46 cases (ordinary head, and a head chosen so the clear loop wraps
// past index 0x7f on its very first store). `head_idx` must be < 128.
void run_clear_case(int head_idx, const char *tag) {
    tact_fixture fx;
    tact_unit   &u = fx.units[UNIT_IDX];
    seed_queue(u);
    u.cmd_index = (uint8_t)head_idx;
    // Distinct non-symmetric sentinels, none colliding with the loop's own bit (0x20) or with each
    // other, so an OR-vs-assign bug on status and a col/row swap on the redirect snapshot are both
    // directly observable.
    u.status = 0x41; // bits 0 and 6 -- neither is 0x20, so a plain assign (losing them)
                     // is distinguishable from the real OR-in.
    u.pos_col           = 0x37;
    u.pos_row           = 0x58;
    u.move_redirect_col = 0x99;   // pre-call sentinel, must become pos_col (0x37)
    u.move_redirect_row = 0x88;   // pre-call sentinel, must become pos_row (0x58)
    u.face_cmd_op       = 0x1111; // nonzero sentinel, must be zeroed
    u.attack_cmd_op     = 0x2222; // nonzero sentinel, must be zeroed
    tact_store own      = fx.store();

    const int32_t ret =
        detail::unit_enqueue_command(fx.view(), own, UNIT_IDX, /*op=*/0x46, /*interrupt_flag=*/1,
                                     /*arg0=*/0, /*arg1=*/0, /*arg2=*/0, /*arg3=*/0);
    char buf[192];
    std::snprintf(buf, sizeof buf, "%s: op==0x46 always returns 1, @0x0042b664", tag);
    ck_eq((uint32_t)ret, 1u, buf);

    ck_slot_untouched(u.cmd_queue[head_idx], head_idx, tag);
    for (int i = 0; i < 128; ++i) {
        if (i == head_idx) continue;
        ck_slot_op_cleared(u.cmd_queue[i], i, tag);
    }

    std::snprintf(buf, sizeof buf,
                  "%s: status OR-in 0x20 preserves other bits (0x41|0x20==0x61), @0x0042b5a4-0x0042b5b4",
                  tag);
    ck_eq((uint32_t)u.status, 0x61u, buf);
    std::snprintf(buf, sizeof buf, "%s: move_redirect_col snapshots pos_col, @0x0042b5c8-0x0042b5ce",
                  tag);
    ck_eq((uint32_t)u.move_redirect_col, 0x37u, buf);
    std::snprintf(buf, sizeof buf, "%s: move_redirect_row snapshots pos_row, @0x0042b5e2-0x0042b5e8",
                  tag);
    ck_eq((uint32_t)u.move_redirect_row, 0x58u, buf);
    std::snprintf(buf, sizeof buf, "%s: face_cmd_op zeroed, @0x0042b64b", tag);
    ck_eq((uint32_t)u.face_cmd_op, 0u, buf);
    std::snprintf(buf, sizeof buf, "%s: attack_cmd_op zeroed, @0x0042b65b", tag);
    ck_eq((uint32_t)u.attack_cmd_op, 0u, buf);
    std::snprintf(buf, sizeof buf, "%s: cmd_index itself is never advanced by enqueue", tag);
    ck_eq((uint32_t)u.cmd_index, (uint32_t)head_idx, buf);
}

} // namespace

void run_owner_enqueue_fix_tests() {
    // =============================================================================================
    // BEHAVIOR 2 -- op==6 RE-ENTRY REFUSAL SENSE + RETURN VALUE (@0x0042b6d3-0x0042b6fe). Refuses
    // iff iflag param==1 AND face_cmd_op==6 AND face_interrupt_flag==0; BOTH the refuse and the
    // accept path return 1 (not 0). Old code had the condition inverted and returned 0.
    // =============================================================================================

    // F6a: face_cmd_op==6, face_interrupt_flag==0, caller iflag==1 -> REFUSED, returns 1, face
    // record byte-for-byte UNCHANGED (proves the refusal returns BEFORE any store, not after).
    {
        tact_fixture fx;
        tact_unit   &u        = fx.units[UNIT_IDX];
        u.face_cmd_op         = 6;
        u.face_interrupt_flag = 0;
        u.face_cmd_target_dir = 0x1234; // pre-call sentinel, must survive the refusal
        u.face_cmd_arg1       = 0x2222;
        u.face_cmd_arg2       = 0x3333;
        u.face_cmd_arg3       = 0x4444;
        tact_store own        = fx.store();

        const int32_t ret =
            detail::unit_enqueue_command(fx.view(), own, UNIT_IDX, /*op=*/6, /*interrupt_flag=*/1,
                                         /*arg0=*/7, /*arg1=*/0x5555, /*arg2=*/0x6666,
                                         /*arg3=*/0x7777);
        ck_eq((uint32_t)ret, 1u, "F6a: op==6 re-entry refusal returns 1, NOT 0, @0x0042b6fe");
        ck_eq((uint32_t)u.face_interrupt_flag, 0u, "F6a: refusal leaves face_interrupt_flag untouched");
        ck_eq((uint32_t)u.face_cmd_op, 6u, "F6a: refusal leaves face_cmd_op untouched");
        ck_eq((uint32_t)u.face_cmd_target_dir, 0x1234u,
              "F6a: refusal leaves face_cmd_target_dir untouched -- record NOT rewritten, "
              "@0x0042b6c9-0x0042b6fe");
        ck_eq((uint32_t)u.face_cmd_arg1, 0x2222u, "F6a: refusal leaves face_cmd_arg1 untouched");
        ck_eq((uint32_t)u.face_cmd_arg2, 0x3333u, "F6a: refusal leaves face_cmd_arg2 untouched");
        ck_eq((uint32_t)u.face_cmd_arg3, 0x4444u, "F6a: refusal leaves face_cmd_arg3 untouched");
    }

    // F6b: the converse -- face_interrupt_flag != 0 (already-armed record is NOT protected) ->
    // ACCEPTED, record fully rewritten. Distinguishes the refusal condition's sense: it must be
    // "==0", not "!=0" or unconditional.
    {
        tact_fixture fx;
        tact_unit   &u        = fx.units[UNIT_IDX];
        u.face_cmd_op         = 6;
        u.face_interrupt_flag = 9;      // nonzero -> the gate's own AND-chain is false -> falls through
        u.face_cmd_target_dir = 0x1234; // pre-call sentinel, must be OVERWRITTEN
        u.face_cmd_arg1       = 0x2222;
        u.face_cmd_arg2       = 0x3333;
        u.face_cmd_arg3       = 0x4444;
        tact_store own        = fx.store();

        const int32_t ret =
            detail::unit_enqueue_command(fx.view(), own, UNIT_IDX, /*op=*/6, /*interrupt_flag=*/1,
                                         /*arg0=*/7, /*arg1=*/0x5555, /*arg2=*/0x6666,
                                         /*arg3=*/0x7777);
        ck_eq((uint32_t)ret, 1u, "F6b: op==6 accepted (face_interrupt_flag!=0) returns 1");
        ck_eq((uint32_t)u.face_interrupt_flag, 1u,
              "F6b: face_interrupt_flag rewritten to the caller's iflag param, @0x0042b72c");
        ck_eq((uint32_t)u.face_cmd_op, 6u, "F6b: face_cmd_op re-armed to 6, @0x0042b739");
        ck_eq((uint32_t)u.face_cmd_target_dir, 7u,
              "F6b: face_cmd_target_dir rewritten to arg0, @0x0042b74c");
        ck_eq((uint32_t)u.face_cmd_arg1, 0u, "F6b: face_cmd_arg1 zeroed, @0x0042b75a");
        ck_eq((uint32_t)u.face_cmd_arg2, 0u, "F6b: face_cmd_arg2 zeroed, @0x0042b76a");
        ck_eq((uint32_t)u.face_cmd_arg3, 0u, "F6b: face_cmd_arg3 zeroed, @0x0042b77a");
    }

    // =============================================================================================
    // BEHAVIOR 3 -- op==6 arg0 RANGE-FAIL RETURNS 1, NOT 0 (@0x0042b716). Range is [1, 0x18]
    // INCLUSIVE on both ends (0x0042b70a JL / 0x0042b714 JLE). Boundary tested from both sides.
    // =============================================================================================

    // R1: arg0==0, one below the low bound -> refused, returns 1, record unchanged.
    {
        tact_fixture fx;
        tact_unit   &u        = fx.units[UNIT_IDX];
        u.face_interrupt_flag = 9;      // sentinel, must survive
        u.face_cmd_op         = 0;      // != 6, so the F6-style gate never applies here
        u.face_cmd_target_dir = 0xBEEF; // sentinel, must survive
        u.face_cmd_arg1       = 0x2222;
        tact_store own        = fx.store();

        const int32_t ret = detail::unit_enqueue_command(fx.view(), own, UNIT_IDX, /*op=*/6,
                                                         /*interrupt_flag=*/1, /*arg0=*/0,
                                                         /*arg1=*/0, /*arg2=*/0, /*arg3=*/0);
        ck_eq((uint32_t)ret, 1u, "R1: op==6 arg0==0 (below range) returns 1, NOT 0, @0x0042b716");
        ck_eq((uint32_t)u.face_interrupt_flag, 9u, "R1: refusal leaves face_interrupt_flag untouched");
        ck_eq((uint32_t)u.face_cmd_target_dir, 0xBEEFu,
              "R1: refusal leaves face_cmd_target_dir untouched");
        ck_eq((uint32_t)u.face_cmd_arg1, 0x2222u, "R1: refusal leaves face_cmd_arg1 untouched");
    }

    // R2: arg0==0x19, one above the high bound -> refused, returns 1, record unchanged.
    {
        tact_fixture fx;
        tact_unit   &u        = fx.units[UNIT_IDX];
        u.face_interrupt_flag = 9;
        u.face_cmd_op         = 0;
        u.face_cmd_target_dir = 0xBEEF;
        u.face_cmd_arg1       = 0x2222;
        tact_store own        = fx.store();

        const int32_t ret = detail::unit_enqueue_command(fx.view(), own, UNIT_IDX, /*op=*/6,
                                                         /*interrupt_flag=*/1, /*arg0=*/0x19,
                                                         /*arg1=*/0, /*arg2=*/0, /*arg3=*/0);
        ck_eq((uint32_t)ret, 1u, "R2: op==6 arg0==0x19 (above range) returns 1, NOT 0, @0x0042b716");
        ck_eq((uint32_t)u.face_interrupt_flag, 9u, "R2: refusal leaves face_interrupt_flag untouched");
        ck_eq((uint32_t)u.face_cmd_target_dir, 0xBEEFu,
              "R2: refusal leaves face_cmd_target_dir untouched");
        ck_eq((uint32_t)u.face_cmd_arg1, 0x2222u, "R2: refusal leaves face_cmd_arg1 untouched");
    }

    // R3: arg0==1, the LOW boundary itself -> accepted (record rewritten), proving the JL at
    // 0x0042b70e does NOT fire on 1.
    {
        tact_fixture fx;
        tact_unit   &u        = fx.units[UNIT_IDX];
        u.face_interrupt_flag = 9;
        u.face_cmd_op         = 0;
        u.face_cmd_target_dir = 0xBEEF;
        u.face_cmd_arg1       = 0x2222;
        tact_store own        = fx.store();

        const int32_t ret = detail::unit_enqueue_command(fx.view(), own, UNIT_IDX, /*op=*/6,
                                                         /*interrupt_flag=*/1, /*arg0=*/1,
                                                         /*arg1=*/0, /*arg2=*/0, /*arg3=*/0);
        ck_eq((uint32_t)ret, 1u, "R3: op==6 arg0==1 (low boundary) accepted, returns 1");
        ck_eq((uint32_t)u.face_interrupt_flag, 1u, "R3: face_interrupt_flag rewritten to iflag param");
        ck_eq((uint32_t)u.face_cmd_target_dir, 1u,
              "R3: face_cmd_target_dir rewritten to arg0==1, low boundary ACCEPTED, @0x0042b70e JL");
        ck_eq((uint32_t)u.face_cmd_arg1, 0u, "R3: face_cmd_arg1 zeroed");
    }

    // R4: arg0==0x18, the HIGH boundary itself -> accepted, proving the JLE at 0x0042b714 fires on
    // 0x18 (not just below it).
    {
        tact_fixture fx;
        tact_unit   &u        = fx.units[UNIT_IDX];
        u.face_interrupt_flag = 9;
        u.face_cmd_op         = 0;
        u.face_cmd_target_dir = 0xBEEF;
        u.face_cmd_arg1       = 0x2222;
        tact_store own        = fx.store();

        const int32_t ret = detail::unit_enqueue_command(fx.view(), own, UNIT_IDX, /*op=*/6,
                                                         /*interrupt_flag=*/1, /*arg0=*/0x18,
                                                         /*arg1=*/0, /*arg2=*/0, /*arg3=*/0);
        ck_eq((uint32_t)ret, 1u, "R4: op==6 arg0==0x18 (high boundary) accepted, returns 1");
        ck_eq((uint32_t)u.face_interrupt_flag, 1u, "R4: face_interrupt_flag rewritten to iflag param");
        ck_eq((uint32_t)u.face_cmd_target_dir, 0x18u,
              "R4: face_cmd_target_dir rewritten to arg0==0x18, high boundary ACCEPTED, "
              "@0x0042b714 JLE");
        ck_eq((uint32_t)u.face_cmd_arg1, 0u, "R4: face_cmd_arg1 zeroed");
    }

    // =============================================================================================
    // BEHAVIOR 4 -- op==0x46 PRESERVES THE HEAD SLOT (@0x0042b616 pre-increment BEFORE each store).
    // Old code cleared all 0x80 slots including head; fixed code clears head+1..head+0x7f only.
    // =============================================================================================

    // C1: ordinary mid-range head, no wrap.
    run_clear_case(10, "C1 (no wrap, head=10)");

    // C2: head==0x7f -- the loop's FIRST store already wraps 0x80->0, the most aggressive wrap case
    // reachable (every one of the 0x7f stores lands in [0, 0x7e], head 0x7f itself untouched).
    run_clear_case(0x7f, "C2 (wrap, head=0x7f)");

    // =============================================================================================
    // BEHAVIOR 5 -- op==0x7f: head-empty short-circuits WITHOUT setting status bit 2; head-occupied
    // SETS status bit 2 AND falls through to append an op-0x7f marker entry (@0x0042b6bb). Old code
    // had the bit condition inverted and never appended.
    // =============================================================================================

    // S1: head slot empty (op==0) -> plain return 1, status UNCHANGED (bit 2 not set), queue
    // UNCHANGED (nothing appended, nothing to append into).
    {
        tact_fixture fx;
        tact_unit   &u = fx.units[UNIT_IDX];
        u.status       = 0x94; // sentinel with neither bit 2 (0x2) nor bit 0x20 set
        u.cmd_index    = 0;    // cmd_queue[0].op is 0 by default (fixture zero-init) -- head empty
        tact_store own = fx.store();

        const int32_t ret =
            detail::unit_enqueue_command(fx.view(), own, UNIT_IDX, /*op=*/0x7f,
                                         /*interrupt_flag=*/3, /*arg0=*/0x1111, /*arg1=*/0x2222,
                                         /*arg2=*/0x3333, /*arg3=*/0x4444);
        ck_eq((uint32_t)ret, 1u, "S1: op==0x7f, head empty -> plain return 1, @0x0042b69c JZ");
        ck_eq((uint32_t)u.status, 0x94u,
              "S1: status UNCHANGED -- bit 2 NOT set on the empty-head path, @0x0042b69c short-circuit");
        ck_eq((uint32_t)u.cmd_queue[0].op, 0u, "S1: head slot still empty, nothing appended");
        ck_eq((uint32_t)u.cmd_index, 0u, "S1: cmd_index untouched");
    }

    // S2: head slot occupied -> status bit 2 SET (other bits preserved) AND an op-0x7f entry
    // APPENDED at the first free slot (slot 1: cmd_index==0 is occupied by the seed, slot 1 is the
    // first free one the default scan finds). Head slot itself is untouched by this path.
    {
        tact_fixture fx;
        tact_unit   &u                = fx.units[UNIT_IDX];
        u.status                      = 0x94; // same sentinel as S1: neither bit 2 nor bit 0x20 set
        u.cmd_index                   = 0;
        u.cmd_queue[0].interrupt_flag = 9;      // nonzero -> the unrelated re-entry gate never fires
        u.cmd_queue[0].op             = 0x2001; // occupied, distinct sentinel op (not 0x7f)
        u.cmd_queue[0].arg0           = 0x3001;
        u.cmd_queue[0].arg1           = 0x4001;
        u.cmd_queue[0].arg2           = 0x5001;
        u.cmd_queue[0].arg3           = 0x6001;
        tact_store own                = fx.store();

        const int32_t ret =
            detail::unit_enqueue_command(fx.view(), own, UNIT_IDX, /*op=*/0x7f,
                                         /*interrupt_flag=*/5, /*arg0=*/0x1111, /*arg1=*/0x2222,
                                         /*arg2=*/0x3333, /*arg3=*/0x4444);
        ck_eq((uint32_t)ret, 1u, "S2: op==0x7f, head occupied -> append succeeds, returns 1");
        ck_eq((uint32_t)u.status, 0x96u,
              "S2: status bit 2 SET, other bits preserved (0x94|0x2==0x96), @0x0042b6ab OR 0x2");
        // The op-0x7f entry itself, appended at slot 1 -- the default queue-append arm
        // (@0x0042b83f-0x0042b93c) writes the CALLER's op (0x7f, not the head's original op).
        ck_eq((uint32_t)u.cmd_queue[1].interrupt_flag, 5u,
              "S2: appended entry's interrupt_flag == the enqueue call's iflag param");
        ck_eq((uint32_t)u.cmd_queue[1].op, 0x7fu,
              "S2: appended entry's op == 0x7f (the marker), @0x0042b6bb fall-through to default "
              "append");
        ck_eq((uint32_t)u.cmd_queue[1].arg0, 0x1111u, "S2: appended entry's arg0 == the call's arg0");
        ck_eq((uint32_t)u.cmd_queue[1].arg1, 0x2222u, "S2: appended entry's arg1 == the call's arg1");
        ck_eq((uint32_t)u.cmd_queue[1].arg2, 0x3333u, "S2: appended entry's arg2 == the call's arg2");
        ck_eq((uint32_t)u.cmd_queue[1].arg3, 0x4444u, "S2: appended entry's arg3 == the call's arg3");
        // The head slot itself is NOT touched by the op==0x7f path -- only a NEW entry is appended.
        ck_eq((uint32_t)u.cmd_queue[0].interrupt_flag, 9u, "S2: head slot interrupt_flag untouched");
        ck_eq((uint32_t)u.cmd_queue[0].op, 0x2001u, "S2: head slot op untouched (still the seed)");
        ck_eq((uint32_t)u.cmd_queue[0].arg0, 0x3001u, "S2: head slot arg0 untouched");
        ck_eq((uint32_t)u.cmd_index, 0u, "S2: cmd_index untouched -- enqueue never advances it");
    }
}

} // namespace mh::tact::test
