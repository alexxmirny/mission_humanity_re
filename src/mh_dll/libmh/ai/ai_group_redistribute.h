//
// ai/ai_group_redistribute.h -- llm_strat_ai_group_redistribute_units (RI-AI / AI1C layer 2, the
// 2026-08-05 slice). Translated from the DISASSEMBLY
// (tmp/decomp_ai/llm_strat_ai_group_redistribute_units_004e6a89.asm), not from the exported `.c` --
// the draft's local numbering does not track the real EBP slots and its `member_count`/centroid
// bookkeeping does not match the bytes. Every claim below is read off the listing.
//
// llm_strat_ai_group_redistribute_units @0x004e6a89 (0x27a bytes).
//
// Called with (player, group_index) for a group whose `reinforce_pending` is non-zero (guard 1,
// 0x004e6ac7) and whose `goal` is not 0xb/attack-underway (guard 2, 0x004e6ad5). `goal` then picks
// one of three (kind, pool_group_index) pairs -- a FIXED partner slot, not a search:
//   goal == 7 or 10  -> kind 1, pool_group_index 3
//   goal == 8        -> kind 2, pool_group_index 4
//   anything else    -> kind 0, pool_group_index 2
// (0x004e6ae2-0x004e6b1f; transcribed as an if/else chain rather than the original's `CMP/JC/JBE`
// ladder, which is an equivalent reshape of a pure decision table, not a behaviour change).
//
// THE HAZARD OF THE SLICE, and it is a property of the ORIGINAL, not of this translation. On the
// kind == 0 arm (0x004e6b2c-0x004e6b3a) the original calls group_split_off_create with THREE STACK
// SLOTS THAT ARE NEVER WRITTEN ON THIS PATH: [EBP-0x14] (member_count, the 5th/stack argument),
// [EBP-0x2c] (centroid_x) and [EBP-0x30] (centroid_y). Those three are only filled at
// 0x004e6b7f-0x004e6b98, code that this arm does not reach before the call. So the original passes
// three garbage frame words, and group_split_off_create's own gates (member_count != 0 at
// 0x004e691b, member_count <= a u16 headcount at 0x004e690b) are what usually turn the call into a
// no-op -- without the original "knowing" that. This translation cannot see the original's stack
// garbage (it runs on a different stack), so `detail::group_redistribute_units` takes the three
// values as EXPLICIT parameters, and both non-detail wrappers below pass a named constant 0 for all
// three -- 0 is a documented STANDIN for an unknowable input, not a value read from anywhere. Do not
// replace it with a "plausible" value (the centroid, the member count): that would be a different
// wrong answer wearing a more convincing costume.
//
// PAST THAT CALL (taken or not), the function proceeds unconditionally to compare the partner
// ("pool") group against the ticking ("src") group:
//   if pool.member_count < src.reinforce_pending (unsigned, 0x004e6b79):
//     EXCESS-SPLIT PATH (0x004e6cc4) -- the pool has too few spares to help. Returns immediately
//     unless src.reinforce_pending > src.member_count AND src.goal is neither 3 nor 0xb, in which
//     case it calls group_split_excess_members(player, group_index) before returning.
//   else:
//     MERGE PATH (0x004e6b7f) -- compute src's centroid, clamp a transfer cap to
//     min(src.reinforce_pending * 2, pool.member_count), ADD that cap to pool.reinforce_pending
//     (0x004e6bcf -- the site the `reinforce_pending` field comment on
//     mh_llm_strat_ai_unit_group already credits with this exact write), then walk pool's ORIGINAL
//     head_unit/ai_group_next chain moving members from pool into group_index until the cap or the
//     chain runs out, and finally zero src.reinforce_pending (0x004e6cb5-0x004e6cbf).
//
// TWO SHAPES TO TRANSCRIBE RATHER THAN TIDY, both already true of the original:
//   * The move loop's `u`/`next` walk is over the POOL group's list AS IT WAS AT LOOP ENTRY -- `u`
//     starts at pool.head_unit (0x004e6bd6) and each iteration advances via that unit's OWN
//     `ai_group_next` (captured into `next` BEFORE any move happens, 0x004e6bf0). But the unit
//     actually acted on by unit_flag_and_move/group_member_move is pool.head_unit RE-READ from
//     player_data at the moment of the call (0x004e6c5c, 0x004e6c70) -- which keeps changing as
//     members leave. So `u`/`next` is a loop COUNTER over the original list shape; it is not the
//     unit identity that gets moved.
//   * The type test at 0x004e6c0d-0x004e6c3b compiles to two comparisons of the SAME `kind == 0`
//     local. The observable predicate is `matched || kind != 0` (skip the move only when NEITHER is
//     true) -- see group_redistribute_units's body for the four unit-type literals `matched` tests.
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

namespace detail {

// WHAT A CALL ACTUALLY DID -- most calls are one of the two early returns (guard 1/2) or the
// excess-split path's own early return, all writing nothing the differential oracle can see.
struct redistribute_report {
    bool    no_reinforce_pending = false; // guard 1 (0x004e6ac7): reinforce_pending == 0, no-op
    bool    already_attacking    = false; // guard 2 (0x004e6ad5): goal == 0xb, no-op
    int32_t kind                 = -1;    // 0/1/2, the goal-derived branch selector
    int32_t pool_group_index     = -1;    // 2, 3 or 4 -- the goal-derived fixed partner group

    // THE HAZARD BRANCH. True whenever kind == 0 and group_split_off_create was therefore called
    // with the three documented-garbage stack slots (see the file header). Counted on its own in the
    // shadow arm so a run can say whether this branch was even reached.
    bool kind0_split_off_reached = false;

    bool    excess_split_path   = false; // pool.member_count < src.reinforce_pending (0x004e6b79)
    bool    excess_split_called = false; // ...and group_split_excess_members actually ran
    bool    merge_path          = false; // the "pull members from the pool" path (0x004e6b7f onward)
    int32_t transfer_cap        = 0;     // the merge path's clamped member-count cap
    int32_t units_moved         = 0;
};

// llm_strat_ai_group_redistribute_units @0x004e6a89.
//
// `split_off_member_count`, `split_off_centroid_x`, `split_off_centroid_y` are the three explicit
// stand-ins for the original's uninitialised frame slots on the kind == 0 arm -- see the file
// header. They are used ONLY on that arm; every other path in this function never reads them.
redistribute_report group_redistribute_units(const ai_view &v, const ai_store &own,
                                             const ai_calls &gc, uint32_t player, int32_t group_index,
                                             int32_t split_off_member_count, int32_t split_off_centroid_x,
                                             int32_t split_off_centroid_y);

} // namespace detail

void group_redistribute_units(uint32_t player, int32_t group_index);

} // namespace mh::ai
