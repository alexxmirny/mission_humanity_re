//
// ai/ai_group_task_queue_ops.cpp -- see ai_group_task_queue_ops.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_ai_group_task_{preempt_004d4b25,dequeue_004d4cd3,enqueue_004d4c20}.asm),
// cross-read against the committed .c only to confirm field-to-parameter mapping -- all three .c
// bodies expand the group's address arithmetic as raw offsets from `player_data`, which is not
// reproduced here; the group is reached through `own.players[player_id].ai_groups[group_index]`
// instead, same as every other group-task function in this cluster (ai_group_task_machine.cpp,
// ai_group_hold.cpp).
//
#include "ai/ai_group_task_queue_ops.h"


namespace mh::ai {
namespace detail {

namespace {

using group_task = mh::game::mh_llm_strat_ai_group_task;

// The group's "slot i" record, i = 0 .. task_queue_count-1: slot 0 is the group's own flat
// active-task fields (+0x26..+0x4e), slot i>=1 is task_queue_backlog[i-1] -- SAME 0x29-byte layout
// either way (mh_llm_strat_ai_group_task's field order matches the flat fields' order exactly). Both
// _preempt and _dequeue shift records between slots by this uniform indexing; the original does it
// with a raw memcpy over the shared address arithmetic, this reads/writes the named fields instead.
group_task read_task_slot(const unit_group &grp, int32_t slot) {
    if (slot == 0) {
        group_task t{};
        t.pending_param   = grp.pending_param;
        t.task_code       = grp.task_code;
        t.active_flag     = grp.active_flag;
        t.sub_code        = grp.active_sub_code;
        t.param_a         = grp.active_param_a;
        t.param_b         = grp.active_param_b;
        t.param_c         = grp.active_param_c;
        t.param_d         = grp.active_param_d;
        t.queued_time     = grp.active_queued_time;
        t.task_start_time = grp.task_start_time;
        return t;
    }
    return grp.task_queue_backlog[slot - 1];
}

void write_task_slot(unit_group &grp, int32_t slot, const group_task &t) {
    if (slot == 0) {
        grp.pending_param      = t.pending_param;
        grp.task_code          = t.task_code;
        grp.active_flag        = t.active_flag;
        grp.active_sub_code    = t.sub_code;
        grp.active_param_a     = t.param_a;
        grp.active_param_b     = t.param_b;
        grp.active_param_c     = t.param_c;
        grp.active_param_d     = t.param_d;
        grp.active_queued_time = t.queued_time;
        grp.task_start_time    = t.task_start_time;
        return;
    }
    grp.task_queue_backlog[slot - 1] = t;
}

} // namespace

// llm_strat_ai_group_task_preempt @0x004d4b25.
//
// No-op (return unchanged) when the queue is already full (task_queue_count == 0x40, CMP/JZ
// @0x004d4b5e/0x004d4b66) -- a cap the original CHECKS here, unlike most of this cluster's scratch
// tables (ai_state.h Batch A comments); reproduce the check, don't drop it.
//
// Otherwise: clear active_flag on the CURRENT slot 0 record first (@0x004d4b6c) -- BEFORE the shift
// below, so when that record is copied up into slot 1 it is already stamped "not the active task".
// Then shift every occupied slot [0 .. count-1] up by one, HIGH INDEX FIRST (the original's loop
// counts DOWN from count to 1, @LAB_004d4b7c), so the in-place copy never overwrites a source slot it
// still has to read. Finally install the new record at slot 0: task_code = `task_code` (the
// original's `a2`), pending_param, active_flag = 0 again (the original's SECOND active_flag store,
// @0x004d4bdd -- redundant with the first but reproduced exactly), active_param_a..d and
// active_sub_code from the five trailing arguments, and increment task_queue_count.
group_task_preempt_report
group_task_preempt(const ai_view &v, const ai_store &own, const ai_calls &gc, int32_t player_id,
                   int32_t group_index, int16_t task_code, int32_t pending_param,
                   int32_t active_param_a, int32_t active_param_b, int32_t active_param_c,
                   int32_t active_param_d, int16_t active_sub_code) {
    (void)v;
    (void)gc;
    group_task_preempt_report rep{};
    unit_group               &grp = own.players[player_id].ai_groups[group_index];

    if (grp.task_queue_count == 0x40) { // CMP word [..+0x12],0x40 / JZ @0x004d4b5e/0x004d4b66
        rep.queue_full = true;
        return rep;
    }

    grp.active_flag = 0; // 0x004d4b6c -- stamped on the CURRENT slot 0, before the shift propagates it

    const int32_t count = (int32_t)(uint16_t)grp.task_queue_count; // MOVZX @0x004d4b73

    // LAB_004d4b7c: slot[i] = slot[i-1] for i = count down to 1.
    for (int32_t i = count; i >= 1; --i) write_task_slot(grp, i, read_task_slot(grp, i - 1));

    grp.task_queue_count = (int16_t)(count + 1); // INC word [..+0x12] @0x004d4bc1
    grp.task_code        = task_code;            // 0x004d4bcc
    grp.pending_param    = pending_param;        // 0x004d4bd7
    grp.active_flag      = 0;                    // 0x004d4bdd -- the original's second store, reproduced
    grp.active_param_a   = active_param_a;       // 0x004d4be8
    grp.active_param_b   = active_param_b;       // 0x004d4bf2
    grp.active_param_c   = active_param_c;       // 0x004d4bfc
    grp.active_param_d   = active_param_d;       // 0x004d4c06
    grp.active_sub_code  = active_sub_code;      // 0x004d4c10

    rep.preempted = true;
    return rep;
}

// llm_strat_ai_group_task_dequeue @0x004d4cd3.
//
// Three original branches, all reproduced rather than collapsed (the count==1 case is a subset of
// the general shift's behaviour -- zero loop iterations, then the same decrement -- but the original
// special-cases it with its own early return, @0x004d4d1a-0x004d4d23, so this does too):
//   count == 0  -> no-op (CMP/JZ @0x004d4d02/0x004d4d0a)
//   count == 1  -> task_queue_count = 0 directly (CMP/JNZ/MOV @0x004d4d10-0x004d4d1a)
//   count >= 2  -> shift slot[i] = slot[i+1] for i = 0 .. count-2 (LAB_004d4d2c, LOW index first --
//                  this direction is safe in-place because each destination is read before it is
//                  overwritten by the NEXT iteration, unlike preempt's insert), then decrement.
group_task_dequeue_report group_task_dequeue(const ai_view &v, const ai_store &own,
                                             const ai_calls &gc, int32_t player_id,
                                             int32_t group_index) {
    (void)v;
    (void)gc;
    group_task_dequeue_report rep{};
    unit_group               &grp = own.players[player_id].ai_groups[group_index];

    if (grp.task_queue_count == 0) { // CMP word [..+0x12],0 / JZ @0x004d4d02/0x004d4d0a
        rep.empty = true;
        return rep;
    }
    if (grp.task_queue_count == 1) { // CMP/JNZ @0x004d4d10/0x004d4d18
        grp.task_queue_count = 0;    // 0x004d4d1a
        rep.single           = true;
        return rep;
    }

    const int32_t count = (int32_t)(uint16_t)grp.task_queue_count;
    for (int32_t i = 0; i < count - 1; ++i) // LAB_004d4d2c
        write_task_slot(grp, i, read_task_slot(grp, i + 1));

    --grp.task_queue_count; // DEC word [..+0x12] @0x004d4d79
    rep.shifted = true;
    return rep;
}

// llm_strat_ai_group_task_enqueue @0x004d4c20.
//
// No-op (return unchanged) when the queue is already full (task_queue_count == 0x40, CMP/JZ
// @0x004d4c4b/0x004d4c53) -- the same cap preempt checks, reproduced the same way.
//
// Otherwise: read the CURRENT task_queue_count as the destination slot (MOVZX @0x004d4c55, BEFORE
// the increment -- so a queue with count == 0 writes slot 0, the group's own active-task fields, and
// a nonempty queue appends past the end of task_queue_backlog[]), increment the count
// (@0x004d4c5c), then write all nine record fields into that slot: task_code = `task_code` (a2),
// pending_param = `pending_param` (param_4), active_flag = 0 (a fresh record is never the active
// one), param_a..param_d and sub_code from the trailing arguments, and queued_time == task_start_time
// = the player's ai_clock widened float->double (see the header) -- both stamped identically at
// enqueue time; task_start_time is re-stamped on activation, queued_time never is.
group_task_enqueue_report
group_task_enqueue(const ai_view &v, const ai_store &own, const ai_calls &gc, int32_t player_id,
                   int32_t group_index, uint16_t task_code, uint32_t pending_param,
                   uint32_t param_a, uint32_t param_b, uint32_t param_c, uint32_t param_d,
                   uint16_t sub_code) {
    (void)gc;
    group_task_enqueue_report rep{};
    unit_group               &grp = own.players[player_id].ai_groups[group_index];

    if (grp.task_queue_count == 0x40) { // CMP word [..+0x12],0x40 / JZ @0x004d4c4b/0x004d4c53
        rep.queue_full = true;
        return rep;
    }

    const int32_t slot = (int32_t)(uint16_t)grp.task_queue_count; // MOVZX @0x004d4c55
    ++grp.task_queue_count;                                       // INC word [..+0x12] @0x004d4c5c

    group_task t{};
    t.task_code        = (int16_t)task_code;                    // 0x004d4c66
    t.pending_param    = (int32_t)pending_param;                // 0x004d4c6e
    t.active_flag      = 0;                                     // 0x004d4c75
    t.param_a          = (int32_t)param_a;                      // 0x004d4c81
    t.param_b          = (int32_t)param_b;                      // 0x004d4c8c
    t.param_c          = (int32_t)param_c;                      // 0x004d4c97
    t.param_d          = (int32_t)param_d;                      // 0x004d4ca2
    t.sub_code         = (int16_t)sub_code;                     // 0x004d4cad
    const double clock = (double)v.players[player_id].ai_clock; // FLD float, twice, @0x004d4cb5/0x004d4cc2
    t.queued_time      = clock;                                 // FSTP double @0x004d4cbb
    t.task_start_time  = clock;                                 // FSTP double @0x004d4cc8
    write_task_slot(grp, slot, t);

    rep.enqueued = true;
    return rep;
}

} // namespace detail

void group_task_preempt(int32_t player_id, int32_t group_index, int16_t task_code,
                        int32_t pending_param, int32_t active_param_a, int32_t active_param_b,
                        int32_t active_param_c, int32_t active_param_d, int16_t active_sub_code) {
    const ai_state st = state();
    (void)detail::group_task_preempt(st.read, st.own, live_calls(), player_id, group_index,
                                     task_code, pending_param, active_param_a, active_param_b,
                                     active_param_c, active_param_d, active_sub_code);
}

void group_task_dequeue(int32_t player_id, int32_t group_index) {
    const ai_state st = state();
    (void)detail::group_task_dequeue(st.read, st.own, live_calls(), player_id, group_index);
}

void group_task_enqueue(int32_t player_id, int32_t group_index, uint16_t task_code,
                        uint32_t pending_param, uint32_t param_a, uint32_t param_b,
                        uint32_t param_c, uint32_t param_d, uint16_t sub_code) {
    const ai_state st = state();
    (void)detail::group_task_enqueue(st.read, st.own, live_calls(), player_id, group_index,
                                     task_code, pending_param, param_a, param_b, param_c, param_d,
                                     sub_code);
}

// ---- the differential-oracle arms ---------------------------------------------------------------
//
// All three bodies write ONLY inside player_data (the group's own ai_groups[] slot), so the site's
// restore covers everything any arm can touch. Aggregate counters + shadow_cadence_due, matching
// the sibling group-task-machine/hold arms in this same family rather than turret_threat_rescan's
// per-call trace -- these are called far more often (every task activation/completion/enqueue) than a
// per-call log line would stay useful for.

} // namespace mh::ai
