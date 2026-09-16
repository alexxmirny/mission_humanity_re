//
// sim_fx_anim_chain_find_tail_selftest.cpp -- `simtest` cases for llm_fx_anim_chain_find_tail
// (sim/sim_fx_anim_chain_find_tail.h/.cpp, RI-SIM / SIM1E opening slice).
//
// PURE QUERY, no callees, no writes -- shape follows sim_landing_queries_selftest.cpp (a
// `detail::fn(fx.view(), ...)` call, no recorder mocks / `*_calls` struct needed).
//
// EXPECTED VALUES HAND-DERIVED FROM THE DISASSEMBLY
// (tmp/decomp_sim/llm_fx_anim_chain_find_tail_0045583c.asm), never from the Ghidra .c draft (project
// rule: the .c has silently lied here before). The walk, in the asm's own addresses:
//   idx = start_frame                                                  (0x00455857-0x0045585a)
//   loop (0x0045585d):
//     next = Anim[idx + 1].next                    (read at 0x00455863, re-read at 0x0045587a)
//     if (next == 0) return idx;                                       (0x0045586c-0x00455872)
//     idx += next;                                                     (0x0045587a-0x00455880)
//     if (idx == start_frame) return start_frame;   (compared 0x00455886, taken 0x0045588b-0x00455891)
//     goto loop;                                                       (0x00455893)
// This oracle exists specifically because the batch's own triage called out three things a live
// 15000-step soak (only 2 calls total) cannot be trusted to have exercised: the next==0 sentinel,
// the cycle guard (which returns START, not the tail), and the `Anim[idx + 1]` (not `Anim[idx]`)
// read offset -- each case below is named for which of those it pins.
//
#include "sim/sim_fx_anim_chain_find_tail.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

} // namespace

void run_fx_anim_chain_find_tail_tests() {
    sim_fixture fx;

    // ---- T1: chain of length 1 -- next==0 on the FIRST read terminates immediately -----------------
    // start_frame=5; Anim[6].next=0 (the only frame this case touches). Pins the 0x00455863 zero-test
    // and the 0x0045586c-0x00455872 "return idx as-is" exit taken on the very first iteration.
    fx.reset();
    fx.anim_frames[6].next = 0;
    ck_eq((uint32_t)detail::fx_anim_chain_find_tail(fx.view(), 5), 5u,
          "T1: Anim[start+1].next==0 on first read terminates at start itself, 0x00455863/0x0045586c");

    // ---- T2: multi-hop chain (3 hops) walking FORWARD through positive deltas ----------------------
    // start=10 -> Anim[11].next=3 -> idx=13 -> Anim[14].next=2 -> idx=15 -> Anim[16].next=0 -> tail=15.
    // Every hop's next value is distinct (3, 2, 0) so a swapped-hop or off-by-one read cannot land on
    // the right answer by accident. Pins the 0x00455880 `idx += next` accumulation across >1 iteration
    // and the 0x00455893 loop-back edge.
    fx.reset();
    fx.anim_frames[11].next = 3;
    fx.anim_frames[14].next = 2;
    fx.anim_frames[16].next = 0;
    ck_eq((uint32_t)detail::fx_anim_chain_find_tail(fx.view(), 10), 15u,
          "T2: 3-hop forward chain 10->13->15, tail at next==0, 0x00455880/0x00455893");

    // ---- T3: NEGATIVE delta -- the walk must go BACKWARD correctly (idx += next, next < 0) ---------
    // start=50 -> Anim[51].next=-20 -> idx=30 -> Anim[31].next=0 -> tail=30. If 0x00455880's ADD were
    // ever mistranslated as an unsigned/absolute step, this would land somewhere other than 30 (e.g.
    // stuck at 50 or wrapped to a huge index) instead of walking backward as the original does.
    fx.reset();
    fx.anim_frames[51].next = -20;
    fx.anim_frames[31].next = 0;
    ck_eq((uint32_t)detail::fx_anim_chain_find_tail(fx.view(), 50), 30u,
          "T3: negative-delta link walks BACKWARD (50-20=30) via signed idx+=next, 0x00455880");

    // ---- T4: CYCLE GUARD -- a chain that loops back to start_frame WITHOUT ever hitting next==0 ----
    // start=100 -> Anim[101].next=5 -> idx=105 -> Anim[106].next=-5 -> idx=100 (== start_frame).
    // The 0x00455886 compare fires equal, so the CYCLE GUARD (0x0045588b-0x00455891) returns
    // start_frame (100) -- NOT the midpoint 105 (that would be "returned the last idx before the
    // compare" instead of the guard's own dedicated store), and NOT an infinite loop (that would hang
    // this test rather than return anything, which is the original's real behaviour for a cycle that
    // does NOT pass through start_frame -- deliberately not exercised here, see the header's
    // preserve-as-is note on that case).
    fx.reset();
    fx.anim_frames[101].next = 5;
    fx.anim_frames[106].next = -5;
    ck_eq((uint32_t)detail::fx_anim_chain_find_tail(fx.view(), 100), 100u,
          "T4: cycle back to start_frame (105->100) returns START, not the tail, 0x00455886/0x0045588b");

    // ---- T5: the `Anim[idx + 1]` read offset, pinned by DELIBERATELY DIFFERENT values at idx vs. -----
    // idx+1 -- start=7; Anim[7].next=99 (what a WRONG idx-only read would see) vs. Anim[8].next=0
    // (what the CORRECT idx+1 read sees, per the header's address-arithmetic derivation: base
    // Anim@0x00ae4c70 + (idx+1)*0x10 + next@0x4 == 0xae4c84 + idx*0x10, matching the asm's literal
    // displacement at 0x00455863/0x0045587a). A correct read sees next==0 immediately and returns 7.
    // A wrong idx-only read would see 99, walk to idx=106, then read Anim[107].next (zeroed by
    // reset(), untouched by this case) as 0 and return 106 instead -- a divergence this case would
    // catch by failing its ck_eq below rather than by accident agreeing.
    fx.reset();
    fx.anim_frames[7].next = 99; // WRONG-index value -- must NOT be the one read
    fx.anim_frames[8].next = 0;  // CORRECT idx+1 value -- must be the one read
    ck_eq((uint32_t)detail::fx_anim_chain_find_tail(fx.view(), 7), 7u,
          "T5: reads Anim[idx + 1].next (frame 8), not Anim[idx].next (frame 7), 0x00455863/0x0045587a");
}

} // namespace mh::sim::test
