//
// ai/ai_group_task_machine.h -- the AI unit-group TASK MACHINE (RI-AI / AI1C layer 2).
//
// llm_strat_ai_group_task_activate @0x004eb131 (0x114 bytes)
// llm_strat_ai_group_task_step     @0x004e96b1 (0x1eb bytes)
//
// These are the two halves of one state machine and they share a translation unit for that reason:
// llm_strat_ai_unit_group_tick (batch C layer 1, already translated in ai_group_tick.cpp) runs them
// against the same group in one loop -- `while (!active_flag) activate(); while (step()) ;` -- and
// both index task slot 0 of the same record with the same arithmetic. Side by side, a field taken
// from the wrong offset in one of them shows up as a disagreement with the other.
//
// ACTIVATE is a dispatcher and nothing else. It copies pending_param into current_param, stamps
// task_start_time from the player's AI clock, sets active_flag, and jumps through a 25-entry table
// at 0x004eb0cd on the group's live task_code. STEP polls whatever activation started and answers
// its caller's loop: non-zero means "re-enter this group immediately", zero means "leave it".
//
// FOUR THINGS THAT ARE EASY TO GET WRONG HERE, all of them load-bearing.
//
//  1. THE TWO JUMP TABLES ARE NOT IN THE EXPORTED .asm. Ghidra renders the indirect JMP and lists
//     the case labels, but the table CONTENTS live in the data at 0x004eb0cd / 0x004e964d and were
//     read out of the image directly. Both tables are reproduced verbatim in the .cpp, and a reader
//     checking this translation has to check them against the image, not against the listing.
//
//  2. THE BOUND TEST IS UNSIGNED, ON 16 BITS. Both bodies do `CMP <16-bit reg>,0x18` / `JA`
//     (0x004eb185 and 0x004e96ff), so task_code is compared as a uint16_t even though the struct
//     field is int16_t. A negative task_code -- e.g. -1, which the wander-bounce constant's
//     truncation shows this family can produce in a neighbouring field -- reads as 0xffff and lands
//     in the out-of-range arm. A signed reading would index the table with a negative value.
//
//  3. ACTIVATE STAMPS BEFORE IT DISPATCHES, AND STAMPS UNCONDITIONALLY. current_param,
//     task_start_time and active_flag are written at 0x004eb15a-0x004eb175, ahead of the bound test
//     -- so even a task_code the table rejects leaves the group marked active. That is what stops
//     llm_strat_ai_unit_group_tick's inner `while (!active_flag)` loop from spinning forever on a
//     junk code, and moving the stamps after the dispatch would hang the game rather than clean it up.
//
//  4. task_start_time IS A FLOAT WIDENED TO A DOUBLE. `FLD float ptr [player + ai_clock]` /
//     `FSTP double ptr [group + task_start_time]` at 0x004eb168-0x004eb16e: the source field is
//     `float ai_clock` (player_data +0x1003c) and the destination is `double task_start_time`
//     (unit_group +0x47). Reading the source as a double would put eight bytes of adjacent state
//     into the group record and the shadow oracle would report it, but only on a run that reaches
//     the site -- so it is spelled explicitly.
//
// AND ONE THAT IS NOT DECIDABLE FROM THE BODY: THE INDETERMINATE RETURN.
//
// task_step's table sends task_code 1 -- and every code above 0x18 -- to caseD_1 @0x004e9892, which
// is `MOV EAX,EBX` with EBX never written since the `PUSH EBX` at 0x004e96be. It returns the
// CALLER's EBX. That is not a decompiler artifact; it is a genuine uninitialised return, and there
// is no C++ that reproduces it, because a shadow arm is entered through the seam dispatcher and
// cannot see the caller's registers.
//
// What the register actually holds is knowable, though, and it says the path is dead: the sole
// caller is llm_strat_ai_unit_group_tick, whose EBX at the call site 0x004eb400 is the GROUP INDEX
// (the loop counter incremented at 0x004eb409). The caller loops `while (step() != 0)`, and neither
// this arm nor activation changes any state the loop reads -- so for any group index >= 1 the
// original hangs, and for group 0 it exits. A task_code of 1 is therefore not a corner case the
// game tolerates; it is a freeze. We return 0 (the group-0 behaviour, i.e. the non-hanging one) and
// COUNT the path: detail::group_task_report::indeterminate says it was taken. If a shadow run ever
// reports a divergence on this site with indet > 0, that divergence is this known arm and not a
// translation bug -- and it is also the discovery that the path is reachable at all.
//
// REGION SET (the write-closure derivation, cross-validated against the 2026-08-05-1221 session's
// independent derivation): activate reaches 69 functions and its closure writes player_data, units,
// the three order regions and _G_LLM_STRAT_RNG_STATE; step reaches 15 and writes player_data only.
// Neither closure contains llm_strat_order_schedule or llm_strat_order_dispatch, so nothing escapes
// and every callee runs FOR REAL in the shadow arm.
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

namespace detail {

// WHAT A CALL ACTUALLY DID. Both bodies are dominated by paths that write little or nothing, so a
// bare call count says almost nothing about what was compared. These separate the arms.
struct group_task_report {
    // activate
    bool stamped      = false; // the three unconditional stamps ran (every call)
    bool dispatched   = false; // the task_code selected a real handler arm
    bool out_of_range = false; // task_code > 0x18 (unsigned) -- rejected before the table
    bool table_no_op  = false; // task_code 0, 1 or 0xa -- in range, dispatches to the epilogue
    // step
    bool     idle           = false; // task_queue_count == 0, the early `return 1`
    bool     indeterminate  = false; // the caller's-EBX arm (task_code 1, or > 0x18). See the header.
    bool     dequeued       = false; // group_task_dequeue was called
    bool     cleared_flag   = false; // active_flag was cleared so the task re-activates
    bool     cleared_target = false; // the engage arm zeroed resolved_target_ref / _index
    int32_t  task_code      = -1;    // the code this call dispatched on (-1 = never read: idle)
    uint32_t ret            = 0;     // step's return value
};

// llm_strat_ai_group_task_activate @0x004eb131.
group_task_report group_task_activate(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                      int32_t player_id, int32_t group_index);
// llm_strat_ai_group_task_step @0x004e96b1. `indeterminate_ret` is what the original returns on the
// caller's-EBX arm; production and the shadow arm pass 0 (see the header), and `aitest` passes a
// marker so the arm is separable in a test.
group_task_report group_task_step(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                  uint32_t player_id, int32_t group_index,
                                  uint32_t indeterminate_ret);

} // namespace detail

void     group_task_activate(int32_t player_id, int32_t group_index);
uint32_t group_task_step(uint32_t player_id, int32_t group_index);

} // namespace mh::ai
