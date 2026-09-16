//
// ai/ai_group_split_off.h -- spins off a sub-group of a player's existing AI unit group (RI-AI /
// AI1C, batch C layer 2 slice, 2026-08-06).
//
// llm_strat_ai_group_split_off_create @0x004e68dd (0x110 bytes)
//
// Called by llm_strat_ai_group_redistribute_units (see ai_group_redistribute.h / .cpp) when a
// group's live member count has grown past what its current mission wants to carry, to peel
// `member_count` units off into a brand-new group that gathers at (centroid_x, centroid_y) and then
// idles, linked back to `source_group_idx` as its parent.
//
// THE GATE IS A SINGLE FOUR-PART AND-CHAIN in the original (all four short-circuit to the same
// epilogue, so a reimplementation may exit early on the first failing part without changing
// behaviour):
//   1. member_count (the requested split size) <= player_data[player].ai_groups[3].member_count --
//      ai_groups[3] is a FIXED SLOT-3 SHARED POOL, not source_group_idx's own count. Both sides are
//      read/compared UNSIGNED (the field is a signed int16_t in the struct; the original MOVZXes it,
//      matching the ushort cast already present in the exported decompile).
//   2. member_count != 0.
//   3. player_data[player].ai_groups[source_group_idx].goal != 7 (already a split-off sub-group).
//   4. player_data[player].ai_groups[source_group_idx].goal != 8.
// then, only if all four hold:
//   5. llm_strat_ai_group_has_split_group_link(player, source_group_idx) must return 0 (no existing
//      linked splinter already covers this source group).
//   6. llm_strat_ai_group_create(player) must not return -1 (a free group slot exists).
// On success: the new group's link_target_group = source_group_idx, goal = 7 (split-off sub-group),
// active_member_count = member_count (both truncated to the field's uint16_t width, matching the
// original's 16-bit stores), and three tasks are enqueued on it in order:
//   - 0x12 (gather), param_4=0x183, param_5=param_6=-1 (centroid unset -> "self"), param_9=member_count
//   - 3    (move),   param_4=0x187, param_5=centroid_x, param_6=centroid_y
//   - 8    (idle),   param_4=0x183, all-zero
// (param_4 here is llm_strat_ai_group_task_enqueue's own 4th argument -- a sub-code/context word the
// task machine reads, not a coordinate; see ai_state.h's ai_calls comment on group_task_enqueue for
// the register/stack layout this maps onto.)
//
// NOTHING WRITES A BYTE OUTSIDE player_data: the pool/goal checks and the new group's field writes
// are all inside player_data[player].ai_groups[...], and both callees this function reaches that
// write anything (llm_strat_ai_group_create, llm_strat_ai_group_task_enqueue) write player_data only
// (see ai_state.cpp's shadow_calls() comments on those two). llm_strat_ai_group_has_split_group_link
// is a pure read (verified against its own disassembly, tmp/decomp_ai/..._004d3af2.asm: two loop-
// local stores to its own stack frame and nothing else).
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

namespace detail {

// WHAT THE CALL ACTUALLY DID -- the whole body is a guarded single write-and-enqueue sequence, so a
// no-op run writes nothing at all; these flags are what separate "the site was reached" from "the
// split actually happened", the same shape as ai_group_hold's group_hold_report.
struct group_split_off_report {
    bool gate_failed   = false; // the four-part pool/goal AND-chain rejected the request
    bool link_exists   = false; // group_has_split_group_link already found a linked splinter
    bool create_failed = false; // group_create returned -1 (no free group slot)
    bool created       = false; // a new group was created, stamped, and its 3 tasks enqueued
};

// llm_strat_ai_group_split_off_create @0x004e68dd.
group_split_off_report group_split_off_create(const ai_view &v, const ai_store &own,
                                              const ai_calls &gc, uint32_t player,
                                              int32_t centroid_x, int32_t centroid_y,
                                              int32_t source_group_idx, uint32_t member_count);

} // namespace detail

void group_split_off_create(uint32_t player, int32_t centroid_x, int32_t centroid_y,
                            int32_t source_group_idx, uint32_t member_count);

} // namespace mh::ai
