//
// sim_fx_anim_seq_cancel_selftest.cpp -- offline `simtest` oracle for llm_fx_anim_seq_cancel
// @0x004539cc (sim/sim_fx_anim_seq_cancel.h/.cpp). Written directly against the DISASSEMBLY
// (tmp/decomp_sim/llm_fx_anim_seq_cancel_004539cc.asm), never the accompanying .c draft -- see the
// module header banner for why (this project's Ghidra .c drafts have lied before).
//
// arm_ready via shadow_sim1e_slice3_run1.ini, but that run made only 2 calls over a 15000-step
// all-AI soak -- essentially none of the live-count early exit, the range filter's edges, the
// dead-slot-costs-nothing rule, the skipped-but-live budget consumption, or the double bookkeeping
// write were exercised live. This offline oracle is the evidence for that gap.
//
// EXPECTED BEHAVIOUR, from the .asm (see the header banner on sim_fx_anim_seq_cancel.h for the full
// derivation):
//   0x004539ea: tail = chain_find_tail(anim_seq_start_frame) -- called exactly once, before the loop.
//   0x004539f2/0x004539f7: remaining_live = fx_anim_pool[0].live, read ONCE -- the pool's live-count
//     header, reused here as a LOCAL early-exit budget that is never written back except via a real
//     match (a completely different write, see below).
//   0x00453a01/0x00453a08/0x00453a0a/0x00453a0e: loop while index<10000 AND remaining_live!=0 --
//     index starts at 1 (slot 0 is the header, never a real record), index bound checked FIRST.
//   0x00453a1e/0x00453a25: a dead slot (.live==0) is skipped WITHOUT consuming the budget.
//   0x00453a27-0x00453a2a: a LIVE slot always consumes one unit of the budget, whether or not it
//     turns out to be in range.
//   0x00453a31-0x00453a49: match iff anim_seq_start_frame <= entries[index].anim_frame <= tail, BOTH
//     ends INCLUSIVE (JL then JLE off two fresh re-reads of .anim_frame).
//   0x00453a51/0x00453a5b: on a match, entries[index].live=0 AND fx_anim_pool[0].live -= 1 -- TWO
//     different dwords; the second is a real decrement of the pool's header, never the local budget.
//
#include "sim/sim_fx_anim_seq_cancel.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

int32_t g_chain_find_tail_calls  = 0;
int32_t g_chain_find_tail_arg    = 0;
int32_t g_chain_find_tail_result = 0;
int32_t rec_chain_find_tail(int32_t start_frame) {
    ++g_chain_find_tail_calls;
    g_chain_find_tail_arg = start_frame;
    return g_chain_find_tail_result;
}

const fx_anim_seq_cancel_calls g_calls = {
    &rec_chain_find_tail,
};

void reset_recorders() {
    g_chain_find_tail_calls  = 0;
    g_chain_find_tail_arg    = 0;
    g_chain_find_tail_result = 0;
}

// Seeds one pool slot with distinct, chosen values -- x/y default to 0 for cases that don't need
// them, and are overridden by the non-corruption case below.
void seed_slot(sim_fixture &fx, int32_t index, int32_t live, int32_t anim_frame, uint16_t x = 0,
               uint16_t y = 0) {
    fx.fx_anim_pool[(size_t)index].live       = live;
    fx.fx_anim_pool[(size_t)index].anim_frame = anim_frame;
    fx.fx_anim_pool[(size_t)index].x          = x;
    fx.fx_anim_pool[(size_t)index].y          = y;
}

} // namespace

void run_fx_anim_seq_cancel_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- range filter, LOW edge INCLUDED: frame == anim_seq_start_frame. Also pins the
    // chain_find_tail mock: called exactly once, with anim_seq_start_frame.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        g_chain_find_tail_result = 700;
        fx.fx_anim_pool[0].live  = 1; // budget: exactly one live entry to visit
        seed_slot(fx, 3, /*live=*/1, /*anim_frame=*/500);

        sim_store own = fx.store();
        detail::fx_anim_seq_cancel(fx.view(), own, g_calls, 500);

        ck_eq((uint32_t)g_chain_find_tail_calls, 1u,
              "T1: chain_find_tail called exactly once, 0x004539ea");
        ck_eq((uint32_t)g_chain_find_tail_arg, 500u,
              "T1: chain_find_tail called with anim_seq_start_frame, 0x004539e7");
        ck_eq((uint32_t)fx.fx_anim_pool[3].live, 0u,
              "T1: frame==anim_seq_start_frame is INCLUDED (low edge), 0x00453a37 JL");
        ck_eq((uint32_t)fx.fx_anim_pool[0].live, 0u,
              "T1: pool header decremented on the match, 0x00453a5b");
    }

    // =================================================================================================
    // T2 -- range filter, LOW edge EXCLUDED: frame == anim_seq_start_frame - 1.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        g_chain_find_tail_result = 700;
        fx.fx_anim_pool[0].live  = 1;
        seed_slot(fx, 3, 1, 499); // anim_seq_start_frame(500) - 1

        sim_store own = fx.store();
        detail::fx_anim_seq_cancel(fx.view(), own, g_calls, 500);

        ck_eq((uint32_t)fx.fx_anim_pool[3].live, 1u,
              "T2: frame==anim_seq_start_frame-1 is EXCLUDED (below low edge), 0x00453a3a JL");
        ck_eq((uint32_t)fx.fx_anim_pool[0].live, 1u, "T2: header untouched -- no match occurred");
    }

    // =================================================================================================
    // T3 -- range filter, HIGH edge INCLUDED: frame == tail.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        g_chain_find_tail_result = 700;
        fx.fx_anim_pool[0].live  = 1;
        seed_slot(fx, 5, 1, 700); // == tail

        sim_store own = fx.store();
        detail::fx_anim_seq_cancel(fx.view(), own, g_calls, 500);

        ck_eq((uint32_t)fx.fx_anim_pool[5].live, 0u,
              "T3: frame==tail is INCLUDED (high edge), 0x00453a49 JLE");
        ck_eq((uint32_t)fx.fx_anim_pool[0].live, 0u,
              "T3: pool header decremented on the match, 0x00453a5b");
    }

    // =================================================================================================
    // T4 -- range filter, HIGH edge EXCLUDED: frame == tail + 1.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        g_chain_find_tail_result = 700;
        fx.fx_anim_pool[0].live  = 1;
        seed_slot(fx, 5, 1, 701); // tail + 1

        sim_store own = fx.store();
        detail::fx_anim_seq_cancel(fx.view(), own, g_calls, 500);

        ck_eq((uint32_t)fx.fx_anim_pool[5].live, 1u,
              "T4: frame==tail+1 is EXCLUDED (above high edge), 0x00453a46-0x00453a49");
        ck_eq((uint32_t)fx.fx_anim_pool[0].live, 1u, "T4: header untouched -- no match occurred");
    }

    // =================================================================================================
    // T5 -- a dead slot (.live==0) is skipped WITHOUT consuming the remaining_live budget: a live,
    // in-range entry AFTER a dead one on a budget of 1 must still be reached and cancelled.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        g_chain_find_tail_result = 700;
        fx.fx_anim_pool[0].live  = 1;                      // budget = 1
        seed_slot(fx, 10, /*live=*/0, /*anim_frame=*/500); // dead -- would match if live
        seed_slot(fx, 20, /*live=*/1, /*anim_frame=*/500); // live, comes after the dead slot

        sim_store own = fx.store();
        detail::fx_anim_seq_cancel(fx.view(), own, g_calls, 500);

        ck_eq((uint32_t)fx.fx_anim_pool[10].live, 0u, "T5: dead slot stays dead, untouched");
        ck_eq((uint32_t)fx.fx_anim_pool[10].anim_frame, 500u,
              "T5: dead slot's other fields untouched -- skipped without any write");
        ck_eq((uint32_t)fx.fx_anim_pool[20].live, 0u,
              "T5: live entry AFTER a dead skip is still reached and cancelled -- dead slots cost no "
              "budget, 0x00453a1e/0x00453a25");
        ck_eq((uint32_t)fx.fx_anim_pool[0].live, 0u,
              "T5: header decremented exactly once (the one real match)");
    }

    // =================================================================================================
    // T6 -- a LIVE slot always consumes one unit of remaining_live budget, EVEN IF it turns out to be
    // out of range (the decrement at 0x00453a27-0x00453a2a happens before the range test). Two
    // out-of-range live entries on a budget of 2 must exhaust the budget and trigger the early exit
    // BEFORE a later in-range live entry is ever visited.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        g_chain_find_tail_result = 700;
        fx.fx_anim_pool[0].live  = 2; // budget = 2
        seed_slot(fx, 3, 1, 100);     // live, out of range (below start) -- consumes budget 2->1
        seed_slot(fx, 6, 1, 100);     // live, out of range (below start) -- consumes budget 1->0
        seed_slot(fx, 9, 1, 500);     // live, IN RANGE -- would match if ever reached

        sim_store own = fx.store();
        detail::fx_anim_seq_cancel(fx.view(), own, g_calls, 500);

        ck_eq((uint32_t)fx.fx_anim_pool[3].live, 1u,
              "T6: out-of-range live entry #1 skipped, not cancelled");
        ck_eq((uint32_t)fx.fx_anim_pool[6].live, 1u,
              "T6: out-of-range live entry #2 skipped, not cancelled -- this is the one that exhausts "
              "the budget");
        ck_eq((uint32_t)fx.fx_anim_pool[9].live, 1u,
              "T6: a live-but-out-of-range entry still consumes the remaining_live budget, so the "
              "early exit fires before this later in-range entry is ever visited, "
              "0x00453a27-0x00453a2a");
        ck_eq((uint32_t)fx.fx_anim_pool[0].live, 2u,
              "T6: header untouched -- no match ever occurred (the early exit fired first)");
    }

    // =================================================================================================
    // T7 -- index 0 (the pool's live-count HEADER) is never treated as a real record; the walk starts
    // at index 1. Give slot 0 both a nonzero .live (the budget) and an in-range .anim_frame -- if the
    // loop mistakenly started at index 0, it would clear its own .live (writing 0) and then ALSO
    // decrement fx_anim_pool[0].live (the SAME dword) again, landing at -1 instead of the untouched 5.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        g_chain_find_tail_result      = 700;
        fx.fx_anim_pool[0].live       = 5;
        fx.fx_anim_pool[0].anim_frame = 500; // in [500,700] -- would match if index 0 were visited

        sim_store own = fx.store();
        detail::fx_anim_seq_cancel(fx.view(), own, g_calls, 500);

        ck_eq((uint32_t)fx.fx_anim_pool[0].live, 5u,
              "T7: index 0 (the live-count header) is NEVER treated as a real record -- a loop "
              "starting at index 0 would read -1 here instead of the untouched 5, 0x004539fa index=1");
    }

    // =================================================================================================
    // T8 -- the upper index bound (10000, exclusive) is real: the last valid slot, index 9999, is
    // reached and processed when the budget allows it.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        g_chain_find_tail_result = 700;
        fx.fx_anim_pool[0].live  = 1;
        seed_slot(fx, 9999, 1, 500); // the last valid slot

        sim_store own = fx.store();
        detail::fx_anim_seq_cancel(fx.view(), own, g_calls, 500);

        ck_eq((uint32_t)fx.fx_anim_pool[9999].live, 0u,
              "T8: index 9999 (the last slot below the 10000 bound) is reached and cancelled, "
              "0x00453a01 index<10000");
        ck_eq((uint32_t)fx.fx_anim_pool[0].live, 0u, "T8: header decremented on the far-end match");
    }

    // =================================================================================================
    // T9 -- non-corruption + exact running count: cancelling one entry must not touch its neighbours
    // (either side), must not touch their fields beyond .live, and the pool header must end at
    // EXACTLY (initial budget - number of real matches) -- pinning the double bookkeeping write
    // (own slot .live=0 AND header -=1) to the precise count, not merely "some decrement happened".
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        g_chain_find_tail_result = 700;
        fx.fx_anim_pool[0].live  = 3;                      // budget matches the 3 live entries below
        seed_slot(fx, 49, 1, 800);                         // live, out of range (above tail) -- left neighbour
        seed_slot(fx, 50, 1, 500);                         // live, in range -- the one real match
        seed_slot(fx, 51, 1, 100, /*x=*/1234, /*y=*/5678); // live, out of range (below start) -- right neighbour

        sim_store own = fx.store();
        detail::fx_anim_seq_cancel(fx.view(), own, g_calls, 500);

        ck_eq((uint32_t)fx.fx_anim_pool[50].live, 0u, "T9: the in-range entry is cancelled");
        ck_eq((uint32_t)fx.fx_anim_pool[49].live, 1u,
              "T9: left neighbour NOT corrupted -- a wrong index would have cleared this instead, "
              "0x00453a51");
        ck_eq((uint32_t)fx.fx_anim_pool[51].live, 1u,
              "T9: right neighbour NOT corrupted -- a wrong index would have cleared this instead, "
              "0x00453a51");
        ck_eq((uint32_t)fx.fx_anim_pool[51].x, 1234u,
              "T9: right neighbour's .x untouched -- non-corruption beyond .live");
        ck_eq((uint32_t)fx.fx_anim_pool[51].y, 5678u,
              "T9: right neighbour's .y untouched -- non-corruption beyond .live");
        ck_eq((uint32_t)fx.fx_anim_pool[0].live, 2u,
              "T9: header decremented by EXACTLY the number of matches (3-1=2) -- pins the double "
              "bookkeeping write's precise count, 0x00453a51/0x00453a5b");
    }
}

} // namespace mh::sim::test
