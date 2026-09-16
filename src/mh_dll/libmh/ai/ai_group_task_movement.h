//
// ai/ai_group_task_movement.h -- three of the AI unit-group task machine's per-tick step handlers
// (RI-AI / AI1C layer 3, the 2026-08-06 slice):
//
//   llm_strat_ai_group_task_patrol_shuttle     @0x004eabd3 (0x14f bytes)  task_code 5
//   llm_strat_ai_group_task_loiter_wander      @0x004ead22 (0x183 bytes)  task_code 0xe
//   llm_strat_ai_group_task_nudge_stragglers   @0x004eaecb (0x9c bytes)   task_code 6
//
// All three are called from llm_strat_ai_group_task_activate's NINETEEN-arm dispatch (see
// ai_group_task_machine.cpp's ACTIVATE_ARMS table) through the ai_calls slots this file supplies
// real bodies for: group_task_patrol_shuttle, group_task_loiter_wander, group_task_nudge_stragglers.
// That dispatcher is NOT retranslated here; this file only supplies what its `gc.` calls resolve to.
//
// patrol_shuttle and loiter_wander share one shape: on first activation (active_sub_code == 0) they
// hand off via task_code 8 with every other argument zeroed; otherwise they enqueue one or more
// task_code-3 MOVE legs, re-enqueue themselves with active_sub_code decremented as a bounce counter,
// and finally dequeue the current (just-activated) queue entry -- this is what keeps the task machine
// re-entering the SAME task_code (5 or 0xe) every tick until the bounce counter is spent, at which
// point task_code 8 takes over. Both were translated from the DISASSEMBLY, not from Ghidra's .c: the
// .c is close for patrol_shuttle but renumbers stack slots in a way that does not track the real EBP
// offsets, and it hides the entire x87 sequence in loiter_wander behind extraout_ST1/extraout_ST1_00
// placeholders that had to be re-derived instruction by instruction (see the function's own comment).
//
// nudge_stragglers is unrelated in shape: a single pass over the group's member linked list, nudging
// any unit with no pending order toward the group's own active rally point.
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

// The logic over an EXPLICIT state, so `net_selftest.exe aitest` can drive it over heap buffers with
// no game and no rig. The wrappers below are this applied to state(); the split costs one inlined
// call each.
namespace detail {

// llm_strat_ai_group_task_patrol_shuttle @0x004eabd3, task_code 5.
//
// On first activation (active_sub_code == 0): re-enqueues via task_code 8 with pending_param carried
// over and every other slot zero. Nothing else runs on this path -- no midpoint call, no move legs.
//
// Otherwise: computes the wrapped midpoint of (active_param_a, active_param_b)-(active_param_c,
// active_param_d) via gc.tile_midpoint_wrapped, then enqueues FOUR task_code-3 MOVE legs in this
// fixed order -- midpoint, anchor-1 (active_param_a/b), midpoint again, anchor-2 (active_param_c/d)
// -- each carrying the group's own pending_param and zero for the remaining three trailing
// parameters. Then re-enqueues itself (task_code 5) with the four anchor params carried over
// unchanged and active_sub_code decremented by one as the bounce counter, and dequeues the current
// (active) queue entry. All five enqueue calls and the dequeue read player_data fields that nothing
// in this body writes, so a single stack-cached `pending`/`grp` read is behaviourally identical to
// the original's several separate re-reads of the same memory.
void group_task_patrol_shuttle(const ai_view &v, const ai_store &own, const ai_calls &gc,
                               int32_t player_id, int32_t group_index);

// llm_strat_ai_group_task_loiter_wander @0x004ead22, task_code 0xe.
//
// On first activation (active_sub_code == 0): re-enqueues via task_code 8, pending_param carried
// over, everything else zero -- no draw, no centroid call. Symmetric with patrol_shuttle's own
// first-activation arm.
//
// Otherwise, draws a random sin/cos jitter offset (x87, see the uncertainty on this in the
// translator's report) around either the group's own anchor (active_param_a/b, when
// active_param_a != -1) or its live member centroid (via gc.group_compute_centroid, when
// active_param_a == -1), wraps the jittered tile through the map's wrap masks, enqueues ONE
// task_code-3 MOVE to it, re-enqueues itself (task_code 0xe) to the SAME jittered tile with
// active_sub_code decremented as the bounce counter, and dequeues the current queue entry.
//
// THE SIN-DERIVED JITTER PAIRS WITH THE X AXIS (width mask) AND THE COS-DERIVED JITTER PAIRS WITH
// THE Y AXIS (height mask) -- read verbatim off 0x004eadd9-0x004eae14, not the more natural-looking
// opposite pairing; see the report's uncertainty on this before "fixing" it.
void group_task_loiter_wander(const ai_view &v, const ai_store &own, const ai_calls &gc,
                              int32_t player_id, int32_t group_index);

// llm_strat_ai_group_task_nudge_stragglers @0x004eaecb, task_code 6.
//
// Walks the group's member linked list from head_unit via unit::ai_group_next (0 = end of chain, the
// same singly-linked walk ai_group_task_predicates.cpp's four functions use). Any member with no
// pending order (gc.unit_is_order_pending == 0) is re-flagged and moved toward the group's own
// active rally point (active_param_a, active_param_b) via gc.unit_flag_and_move. Calls no enqueue/
// dequeue at all -- unlike its two siblings above, this handler is not itself a queue-front task
// step; it runs to completion in one call. Writes nothing directly (own is unused); the only writes
// are inside gc.unit_flag_and_move.
void group_task_nudge_stragglers(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                 uint32_t player_id, int32_t group_index);

} // namespace detail

void group_task_patrol_shuttle(int32_t player_id, int32_t group_index);
void group_task_loiter_wander(int32_t player_id, int32_t group_index);
void group_task_nudge_stragglers(uint32_t player_id, int32_t group_index);

} // namespace mh::ai
