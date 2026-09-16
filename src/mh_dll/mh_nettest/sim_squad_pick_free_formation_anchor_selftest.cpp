//
// sim_squad_pick_free_formation_anchor_selftest.cpp -- `simtest` cases for
// llm_strat_squad_pick_free_formation_anchor @0x004899ed (sim/sim_squad_pick_free_formation_anchor.h/.cpp,
// SIM1-G1).
//
// NO SHADOW SITE (see the .h's own header comment): the function's whole observable effect is the two
// caller-owned `char *` out params, so this offline oracle is the ONLY evidence for this unit -- make it
// thorough.
//
// SCOPE:
//   * outer loop starts the SLOT counter at 1, not 0 (0x00489a0c MOV [EBP-0x1c],0x1) -- slot 0 of the
//     248-byte table is never read as a candidate, even if it happens to look like a free/matching one
//     (T1).
//   * outer loop bound `slot <= 5` (0x00489a13 CMP/0x00489a17 JLE) -- both sides: a free slot found
//     inside [1..5] returns early (T1-T5), and exhausting all 5 without finding one falls out WITHOUT
//     writing either out pointer (0x00489a13/17 -> 0x00489aae directly) (T6).
//   * per-slot candidate load: stride 0x30, x at +0, y at +4, PLAIN byte loads, no sign extension at
//     load time (0x00489a2d-0x00489a44) (all cases).
//   * inner scratch scan `i < existing_count` (0x00489a51 CMP/0x00489a54 JL) -- zero (T1), one (T2),
//     mid/three (T5), the real table extent of 5 (T6), and one PAST that real extent, unclamped, exactly
//     as the .asm computes it with no cap against the fixture's declared [5] (T7).
//   * the per-i compare: BOTH x AND y must match to mark in_use (0x00489a70 JNZ short-circuits on x
//     alone; 0x00489a82 JZ requires y too) -- x-only match, y-only match, and both (T4).
//   * the compare sign-extends the byte candidate to int32 before comparing against the scratch dword
//     (0x00489a66/0x00489a78 MOVSX) -- a negative-byte candidate against a negative int32 scratch value
//     (T3).
//   * the write-out is two single-byte stores through char* out params, nothing else touched around
//     them (0x00489a97-0x00489aa5) (all cases via the OutBuf guard bytes).
//
// EXPECTED BEHAVIOUR HAND-DERIVED FROM
// tmp/decomp_sim/llm_strat_squad_pick_free_formation_anchor_004899ed.asm -- every assertion below cites
// the instruction address(es) it pins. NOT read off the .cpp body.
//
#include "sim/sim_squad_pick_free_formation_anchor.h"

#include <cstdint>
#include <cstdio>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

constexpr size_t SLOT_STRIDE = 0x30; // 0x00489a2d/0x00489a3a: IMUL EAX, slot, 0x30
// REBASED 2026-09-02 (LT0): the fixture now holds the WHOLE placement table @0xae3618, so the
// picker's candidates live in the col-5 column at 0x28 + slot*0x30 (the original read the same
// bytes as 0xae3640 + slot*0x30 through the deleted interior symbol).
constexpr size_t COL5_BASE = 0x28;

// Writes one candidate slot's raw bytes (slot in [0..5], though slot 0 is never read by the original).
void set_slot(sim_fixture &fx, int slot, int8_t x, int8_t y) {
    fx.squad_placement_offset_table[COL5_BASE + (size_t)slot * SLOT_STRIDE + 0] = (uint8_t)x;
    fx.squad_placement_offset_table[COL5_BASE + (size_t)slot * SLOT_STRIDE + 4] = (uint8_t)y;
}

void set_scratch(sim_fixture &fx, int i, int32_t x, int32_t y) {
    fx.squad_anchor_scratch[(size_t)i].x = x;
    fx.squad_anchor_scratch[(size_t)i].y = y;
}

uint32_t u8(char c) { return (uint32_t)(uint8_t)c; }

// Guard buffer around the two out-params: proves the original writes EXACTLY one byte at out_x and one
// at out_y and nothing else (0x00489a97-0x00489aa5 is two single-byte MOVs, full stop) -- and, for the
// "all 5 in use" case, proves NEITHER byte is written at all.
struct OutBuf {
    char before = (char)0x11;
    char x      = (char)0x77; // sentinel distinct from every seeded candidate value below
    char mid    = (char)0x33;
    char y      = (char)0x77;
    char after  = (char)0x55;
};

// `tn` is the owning case's tag (e.g. "T1") -- every message below is prefixed with it, per the
// project's "every check message starts with `Tn: `" convention. `ck` only reads its message
// synchronously (printf, no retained pointer), so handing it a stack buffer is safe.
void check_guards_intact(const OutBuf &b, const char *tn) {
    char m1[96], m2[96], m3[96];
    std::snprintf(m1, sizeof(m1), "%s: guard byte before out_x untouched (only out_x/out_y are ever written, 0x00489a97-0x00489aa5)", tn);
    ck(b.before == (char)0x11, m1);
    std::snprintf(m2, sizeof(m2), "%s: guard byte between out_x/out_y untouched", tn);
    ck(b.mid == (char)0x33, m2);
    std::snprintf(m3, sizeof(m3), "%s: guard byte after out_y untouched", tn);
    ck(b.after == (char)0x55, m3);
}

} // namespace

void run_squad_pick_free_formation_anchor_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- existing_count == 0: the inner scan `i < existing_count` (0x00489a51/54) is false on its
    // very first check, so in_use stays 0 and slot 1 (the FIRST slot the outer loop visits, since the
    // counter is seeded to 1, not 0 -- 0x00489a0c) is written out immediately (0x00489a91/95 JNZ not
    // taken -> 0x00489a97-0x00489aa5).
    //
    // Also pins the slot-0 skip: slot 0's table bytes are seeded to look like a plausible candidate
    // (99, 99) and slot 0's scratch-side stand-in is never consulted -- if the outer loop's counter were
    // seeded to 0 instead of 1, this case would return (99, 99) instead of slot 1's (10, 20).
    //
    // Also pins the off-by-one trap on the inner scan itself: scratch[0] is seeded to MATCH slot 1's
    // candidate exactly, even though existing_count == 0 means it must never be consulted -- a
    // `i <= existing_count` translation bug would look at i==0 anyway, wrongly set in_use, and skip to
    // slot 2.
    // =================================================================================================
    {
        fx.reset();
        set_slot(fx, 0, 99, 99);    // never read (loop starts at slot 1) -- 0x00489a0c
        set_slot(fx, 1, 10, 20);    // the candidate this case must return
        set_scratch(fx, 0, 10, 20); // matches slot 1 -- must be IGNORED since existing_count == 0
        OutBuf   out;
        sim_view v = fx.view();
        detail::squad_pick_free_formation_anchor(v, /*existing_count=*/0, &out.x, &out.y);
        ck_eq(u8(out.x), u8((char)10), "T1: existing_count==0 -- out_x == slot 1's candidate x (10), not slot 0's 99 (0x00489a0c seeds slot=1)");
        ck_eq(u8(out.y), u8((char)20), "T1: existing_count==0 -- out_y == slot 1's candidate y (20)");
        check_guards_intact(out, "T1");
    }

    // =================================================================================================
    // T2 -- the inner-scan gate, one scratch entry: false side (no match -> slot 1 free, picked) and
    // true side (match -> slot 1 rejected, falls through to slot 2) of 0x00489a70 JNZ / 0x00489a82 JZ.
    // =================================================================================================
    {
        fx.reset();
        set_slot(fx, 1, 10, 20);
        set_slot(fx, 2, 11, 21);
        set_scratch(fx, 0, 44, 55); // does not match slot 1's (10,20)
        OutBuf   out;
        sim_view v = fx.view();
        detail::squad_pick_free_formation_anchor(v, /*existing_count=*/1, &out.x, &out.y);
        ck_eq(u8(out.x), u8((char)10), "T2a: scratch[0] doesn't match slot 1 -- slot 1 picked (x)");
        ck_eq(u8(out.y), u8((char)20), "T2a: scratch[0] doesn't match slot 1 -- slot 1 picked (y)");
        check_guards_intact(out, "T2a");
    }
    {
        fx.reset();
        set_slot(fx, 1, 10, 20);
        set_slot(fx, 2, 11, 21);
        set_scratch(fx, 0, 10, 20); // matches slot 1's candidate exactly
        OutBuf   out;
        sim_view v = fx.view();
        detail::squad_pick_free_formation_anchor(v, /*existing_count=*/1, &out.x, &out.y);
        ck_eq(u8(out.x), u8((char)11), "T2b: scratch[0] matches slot 1 -- slot 1 rejected, slot 2 picked (x)");
        ck_eq(u8(out.y), u8((char)21), "T2b: scratch[0] matches slot 1 -- slot 1 rejected, slot 2 picked (y)");
        check_guards_intact(out, "T2b");
    }

    // =================================================================================================
    // T3 -- sign extension: the candidate byte is MOVSX'd to int32 before the compare (0x00489a66 for x,
    // 0x00489a78 for y). Slot 1's candidate is (0xFF, 0xFE) as raw bytes -- signed char -1 and -2 -- and
    // scratch[0] is the int32 pair (-1, -2). A correct (sign-extending) translation matches and moves on
    // to slot 2; a translation that zero-extended the byte (treated it as 255/254) would NOT match here
    // and would wrongly re-pick slot 1.
    // =================================================================================================
    {
        fx.reset();
        set_slot(fx, 1, (int8_t)0xFF, (int8_t)0xFE); // -1, -2 as signed bytes
        set_slot(fx, 2, 12, 22);
        set_scratch(fx, 0, -1, -2); // int32 -1, -2 -- matches ONLY under sign extension
        OutBuf   out;
        sim_view v = fx.view();
        detail::squad_pick_free_formation_anchor(v, /*existing_count=*/1, &out.x, &out.y);
        ck_eq(u8(out.x), u8((char)12), "T3: MOVSX sign-extension -- (-1,-2) byte candidate matches int32 (-1,-2) scratch, slot 1 rejected, slot 2 picked (x)");
        ck_eq(u8(out.y), u8((char)22), "T3: MOVSX sign-extension -- slot 2 picked (y)");
        check_guards_intact(out, "T3");
    }

    // =================================================================================================
    // T4 -- BOTH x AND y must match, not just one (0x00489a70 JNZ short-circuits on x alone; only when x
    // matches does the code go on to check y at 0x00489a82 JZ). x-matches-only and y-matches-only both
    // leave in_use false -- slot 1 stays free and is picked in both sub-cases.
    // =================================================================================================
    {
        fx.reset();
        set_slot(fx, 1, 5, 6);
        set_scratch(fx, 0, 5, 99); // x matches, y does not
        OutBuf   out;
        sim_view v = fx.view();
        detail::squad_pick_free_formation_anchor(v, /*existing_count=*/1, &out.x, &out.y);
        ck_eq(u8(out.x), u8((char)5), "T4a: x matches but y doesn't -- in_use stays false, slot 1 still picked (x)");
        ck_eq(u8(out.y), u8((char)6), "T4a: x matches but y doesn't -- slot 1 still picked (y)");
    }
    {
        fx.reset();
        set_slot(fx, 1, 5, 6);
        set_scratch(fx, 0, 99, 6); // y matches, x does not
        OutBuf   out;
        sim_view v = fx.view();
        detail::squad_pick_free_formation_anchor(v, /*existing_count=*/1, &out.x, &out.y);
        ck_eq(u8(out.x), u8((char)5), "T4b: y matches but x doesn't -- in_use stays false, slot 1 still picked (x)");
        ck_eq(u8(out.y), u8((char)6), "T4b: y matches but x doesn't -- slot 1 still picked (y)");
    }

    // =================================================================================================
    // T5 -- "mid": existing_count == 3, slots 1 and 2 both rejected (matched by scratch[0]/scratch[1]
    // respectively), slot 3 free -- exercises the outer loop incrementing across TWO rejections
    // (0x00489a1e/24 the increment-and-loop-back block) before landing on the free one.
    // =================================================================================================
    {
        fx.reset();
        set_slot(fx, 1, 10, 20);
        set_slot(fx, 2, 11, 21);
        set_slot(fx, 3, 12, 22);
        set_scratch(fx, 0, 10, 20); // matches slot 1
        set_scratch(fx, 1, 11, 21); // matches slot 2
        set_scratch(fx, 2, 0, 0);   // does not match slot 3 (or anything else seeded)
        OutBuf   out;
        sim_view v = fx.view();
        detail::squad_pick_free_formation_anchor(v, /*existing_count=*/3, &out.x, &out.y);
        ck_eq(u8(out.x), u8((char)12), "T5: slots 1,2 both in use -- slot 3 picked (x)");
        ck_eq(u8(out.y), u8((char)22), "T5: slots 1,2 both in use -- slot 3 picked (y)");
        check_guards_intact(out, "T5");
    }

    // =================================================================================================
    // T6 -- existing_count == 5, the REAL declared extent of squad_anchor_scratch[5] and the outer
    // loop's own hard cap (0x00489a13 CMP ...,0x5 / 0x00489a17 JLE). All 5 candidate slots are matched by
    // one scratch entry each, so every outer iteration finds in_use and the loop exhausts
    // (0x00489a13/17 -> 0x00489aae) WITHOUT ever reaching the write block. out_x/out_y (and their guard
    // bytes) must read back exactly as seeded -- the original leaves them untouched in this path, and
    // that is deliberate original behaviour (see the header), not a bug to "fix" with a default write.
    // =================================================================================================
    {
        fx.reset();
        set_slot(fx, 1, 10, 20);
        set_slot(fx, 2, 11, 21);
        set_slot(fx, 3, 12, 22);
        set_slot(fx, 4, 13, 23);
        set_slot(fx, 5, 14, 24);
        set_scratch(fx, 0, 10, 20);
        set_scratch(fx, 1, 11, 21);
        set_scratch(fx, 2, 12, 22);
        set_scratch(fx, 3, 13, 23);
        set_scratch(fx, 4, 14, 24);
        OutBuf   out;
        sim_view v = fx.view();
        detail::squad_pick_free_formation_anchor(v, /*existing_count=*/5, &out.x, &out.y);
        ck(out.x == (char)0x77, "T6: all 5 slots in use -- out_x NEVER written, sentinel unchanged (0x00489a13/17 falls straight to 0x00489aae)");
        ck(out.y == (char)0x77, "T6: all 5 slots in use -- out_y NEVER written, sentinel unchanged");
        check_guards_intact(out, "T6");
    }

    // =================================================================================================
    // T7 -- one value PAST the real scratch extent: existing_count == 6. The .asm's inner-scan bound is
    // the RAW PARAMETER (0x00489a51 CMP EAX,[EBP-0x2c]=existing_count), with no clamp anywhere against
    // the real [5]-entry table -- so this reproduces that literally rather than sanitising it. A local
    // 6-entry scratch buffer stands in for "whatever memory follows the real 5-entry table" (the real
    // fixture's squad_anchor_scratch is exactly [5], so indexing it at i==5 would be a genuine
    // heap-buffer-overflow rather than a reproduction of the original's behaviour; overriding
    // sim_view::squad_anchor_scratch with an owned 6-element array reproduces the SAME unclamped-loop
    // behaviour on backing storage this test file owns, without touching the shared fixture -- see
    // NEEDS FIXTURE in the report).
    //
    // Slots 0..4 of the local scratch buffer are seeded to match NEITHER slot 1 nor slot 2's candidate;
    // index 5 -- the "one past" entry -- is seeded to match slot 1's candidate exactly. A translation
    // that faithfully loops `i < existing_count` unclamped reaches i==5, rejects slot 1, and picks
    // slot 2. A translation that (wrongly) clamped existing_count to the real extent of 5 would never
    // look at index 5, wrongly conclude slot 1 is free, and pick slot 1 instead -- this case is built to
    // separate those two outcomes.
    // =================================================================================================
    {
        fx.reset();
        set_slot(fx, 1, 30, 40);
        set_slot(fx, 2, 31, 41);

        squad_formation_anchor_scratch local6[6] = {};
        for (int i = 0; i < 5; ++i) {
            local6[i].x = 0;
            local6[i].y = 0;
        } // matches neither slot 1 nor 2
        local6[5].x = 30;
        local6[5].y = 40; // the "one past real extent" entry -- matches slot 1's candidate

        OutBuf   out;
        sim_view v             = fx.view();
        v.squad_anchor_scratch = local6; // owned by this test, NOT the shared fixture -- see above
        detail::squad_pick_free_formation_anchor(v, /*existing_count=*/6, &out.x, &out.y);
        ck_eq(u8(out.x), u8((char)31), "T7: existing_count==6, unclamped scan reaches index 5 -- slot 1 rejected, slot 2 picked (x) (0x00489a51 compares against the raw parameter, no cap)");
        ck_eq(u8(out.y), u8((char)41), "T7: existing_count==6, unclamped scan reaches index 5 -- slot 2 picked (y)");
        check_guards_intact(out, "T7");
    }
}

} // namespace mh::sim::test
