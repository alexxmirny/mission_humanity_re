//
// ai/ai_group_task_queue_ops.h -- the three raw task_queue_backlog splice primitives (RI-AI / AI1C
// layer 4 preempt/dequeue, layer 6 enqueue).
//
// llm_strat_ai_group_task_preempt @0x004d4b25 (0xfb bytes) -- FRONT-INSERT. Shifts every occupied
// slot [0 .. task_queue_count-1] UP by one (high index first, so the in-place copy never clobbers a
// source it still needs) and installs a new record at slot 0 -- the ACTIVE slot. This is what makes
// a preempted task run immediately rather than merely queue: enqueue appends at the back instead.
//
// llm_strat_ai_group_task_dequeue @0x004d4cd3 (0xb2 bytes) -- POP FRONT. Shifts every slot
// [1 .. task_queue_count-1] DOWN into [0 .. task_queue_count-2] and decrements the count. Called by
// the task-step machine (ai_group_task_machine.cpp) whenever a task's step handler reports
// completion.
//
// llm_strat_ai_group_task_enqueue @0x004d4c20 (0xb3 bytes) -- BACK-INSERT. No-op when
// task_queue_count == 0x40 (cap, checked and reproduced); otherwise writes a new record at slot
// task_queue_count (BEFORE increment) and increments the count -- the plain append, as opposed to
// preempt's front-insert. Added to this cluster's shadow coverage last (layer 6) because it is its
// own callers' callee frontier throughout batches A-C (group_create, group_expansion, group_form,
// group_split_off, army_milestone, invasion, ...): every one of them already calls it through
// `ai_calls::group_task_enqueue`, bound to the ORIGINAL function until this function's own shadow
// site exists and it is promoted.
//
// ALL THREE FUNCTIONS TREAT THE GROUP'S "SLOT 0" AS THE SAME 0x29-BYTE RECORD SHAPE AS
// task_queue_backlog[]'s ELEMENTS, even though slot 0 is physically the group's own flat
// active-task fields (task_code/active_flag/active_sub_code/active_param_a..d/active_queued_time/
// task_start_time at +0x26..+0x4e) and task_queue_backlog[i] (i = slot-1) is the array proper. The
// original expresses this with raw memcpy/direct stores over the shared address arithmetic
// (player*0x288fc + group*0xa66 + slot*0x29 + 0xe7e44e); this translation expresses the same
// "slot i" idea as read_task_slot()/write_task_slot() over the named struct fields instead, shared
// by all three functions in the anonymous namespace of the .cpp -- it is not a new shared-across-files
// helper, just the one restated address computation the bodies in this TU need.
//
// A record's LAST BYTE (the low byte of task_start_time) is copied by a SEPARATE one-byte MOVSB
// after the 0x28-byte MOVSD.REP block in both preempt/dequeue's originals -- almost certainly a
// compiler artifact of the struct's alignment inside the 0x29-byte record, not a semantic split.
// enqueue writes the same 0x29 bytes as nine separate field stores instead of a block move (see the
// .cpp), so it never had a split-tail to begin with. All three slot helpers copy/write the record in
// one struct assignment, which reproduces the same total 0x29 bytes moved either way.
//
// ENQUEUE'S TWO DOUBLE STORES ARE A FLOAT WIDENED TO A DOUBLE, TWICE FROM THE SAME SOURCE. Both
// `queued_time` and `task_start_time` are stamped from the player's `float ai_clock`
// (player_data+0x1003c) via FLD float/FSTP double (0x004d4cb5-0x004d4cc8, two independent
// FLD/FSTP pairs reading the same address) -- same widening llm_strat_ai_group_task_activate does
// for task_start_time alone (ai_group_task_machine.h point 4). A record enqueued and never
// preempted keeps queued_time == task_start_time until activation re-stamps the latter.
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

namespace detail {

struct group_task_preempt_report {
    bool queue_full = false; // task_queue_count was already 0x40 (64) -- no-op, nothing touched
    bool preempted  = false; // the shift + front-install ran
};

struct group_task_dequeue_report {
    bool empty   = false; // task_queue_count was already 0 -- no-op
    bool single  = false; // task_queue_count was 1 -- count set to 0 directly, no shift
    bool shifted = false; // the general shift-down path ran (count was >= 2 on entry)
};

struct group_task_enqueue_report {
    bool queue_full = false; // task_queue_count was already 0x40 (64) -- no-op, nothing written
    bool enqueued   = false; // the record was written at the back and task_queue_count incremented
};

// llm_strat_ai_group_task_preempt @0x004d4b25. `task_code` is the original's `a2` (BX); the six
// trailing values are, in order, the new ACTIVE record's pending_param, active_param_a..d and
// active_sub_code -- see the .cpp for the exact field mapping, read straight off the committed C.
group_task_preempt_report
group_task_preempt(const ai_view &v, const ai_store &own, const ai_calls &gc, int32_t player_id,
                   int32_t group_index, int16_t task_code, int32_t pending_param,
                   int32_t active_param_a, int32_t active_param_b, int32_t active_param_c,
                   int32_t active_param_d, int16_t active_sub_code);

// llm_strat_ai_group_task_dequeue @0x004d4cd3.
group_task_dequeue_report group_task_dequeue(const ai_view &v, const ai_store &own,
                                             const ai_calls &gc, int32_t player_id,
                                             int32_t group_index);

// llm_strat_ai_group_task_enqueue @0x004d4c20. `task_code` is the original's `a2` (BX);
// `pending_param` is `param_4` (ECX); `param_a..param_d` and `sub_code` are the five stack
// arguments (Stack[0x4..0x14]) -- exact field mapping in the .cpp, read straight off the committed
// C and address arithmetic (every one of the nine field offsets matches the struct's own layout
// with zero ambiguity).
group_task_enqueue_report
group_task_enqueue(const ai_view &v, const ai_store &own, const ai_calls &gc, int32_t player_id,
                   int32_t group_index, uint16_t task_code, uint32_t pending_param,
                   uint32_t param_a, uint32_t param_b, uint32_t param_c, uint32_t param_d,
                   uint16_t sub_code);

} // namespace detail

// Matching signatures/order to the committed ai_calls::group_task_preempt / ::group_task_dequeue /
// ::group_task_enqueue prototypes (ai_state.h) and to the generated
// __mh_watcall_ecx_ebx_volatile / __watcall thunks.
void group_task_preempt(int32_t player_id, int32_t group_index, int16_t task_code,
                        int32_t pending_param, int32_t active_param_a, int32_t active_param_b,
                        int32_t active_param_c, int32_t active_param_d, int16_t active_sub_code);
void group_task_dequeue(int32_t player_id, int32_t group_index);
void group_task_enqueue(int32_t player_id, int32_t group_index, uint16_t task_code,
                        uint32_t pending_param, uint32_t param_a, uint32_t param_b,
                        uint32_t param_c, uint32_t param_d, uint16_t sub_code);

} // namespace mh::ai
