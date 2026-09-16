//
// ai/ai_train_plan.h -- pick ONE unit type to train and queue it (RI-AI / AI1B layer 2).
//
// llm_strat_ai_plan_unit_training @0x004e6d03, `void __watcall f(int player)`, player in EAX.
//
// The AI's unit-production planner, and the whole of it is a four-pass funnel over one pass of the
// cfg INVENTION table:
//
//   gate   the plan is only made once the scripted opening build order has been consumed:
//          (ai_build_plan_len_and_flag & 0x7fffffff) > ai_build_plan_cursor  ==>  return, UNSIGNED
//          (JA @0x004e6d43). So a player still working through its starting plan trains nothing.
//   pass 1 rebuild player_data::ai_train_source_state from the invention table: for every cfg row
//          whose .type == 2 that this player has `available`, the row's .index (a UNIT TYPE) is
//          marked 1, or 2 when the per-player row also has `f3` set. Rows the player does not have
//          keep the 0 the clear loop wrote.
//   pass 2 count, per AI ROLE, how many of those unit types the player can actually build right
//          now -- "can build" being count_unit_build_sources' out-table, which is a per-unit-type
//          count of idle production sources.
//   pass 3 pick the role with the FEWEST entries already queued (ai_train_queued_by_ai_unit),
//          among roles pass 2 found at least one buildable type for. UNSIGNED compare, running
//          minimum seeded at 1000000, ties keep the LOWER role index (JNC @0x004e6e71).
//   pass 4 within that role, pick the buildable type with the HIGHEST cfg ai_level. SIGNED compare
//          seeded at -1 (JGE @0x004e6ebe), ties keep the LOWER type index.
//
//   tail   queue it once, then -- if ai_start_units_remaining is non-zero -- decrement and keep
//          queueing the SAME type until it reaches zero. That is how the scripted starting army
//          arrives: one planning pass drains the whole counter in a loop, it is not one per tick.
//
// THREE LOOP BOUNDS ARE INCLUSIVE (`JBE`, not `JC`) and two of the loops start at index 1:
//   0x004e6d72  clear  ai_train_source_state[0 .. G_BUILDING_COUNT_TOTAL]   <- starts at 0
//   0x004e6e2c  scan   Progress[1 .. G_PROGRESS_COUNT_TOTAL]
//   0x004e6ee5  scan   unit types [1 .. G_UNIT_COUNT_TOTAL]
// Do not "fix" either property.
//
// THE CLEAR LOOP USES THE WRONG COUNT, and it is the original's bug, not a transcription slip. It
// bounds a walk over ai_train_source_state -- which is indexed by UNIT type, proved by the
// `Unit[idx].ai_unit` lookup on the same subscript two passes later -- with G_BUILDING_COUNT_TOTAL.
// It is harmless only while the building count covers the unit-type range. Reproduced verbatim; the
// same observation is already recorded on the field's own comment in mh_structs.gen.h.
//
// THREE UNCHECKED SUBSCRIPTS, ALL THE ORIGINAL'S, none of them defended here:
//   ai_train_source_state[Progress[p].index]   -- .index is u16, the array is uint8_t[100]
//   by_type[Progress[p].index]                 -- same subscript, into a 100-int stack buffer
//   by_role[Unit[t].ai_unit]                   -- ai_unit is u32, the buffer is 12 ints
// In the original the last two would walk its own frame (by_type at EBP-0x1dc, by_role at EBP-0x4c,
// then best_count at -0x1c and best_role at -0x18 -- so by_role[12] IS best_count). A clamp here
// would diverge on exactly the corrupted cfg that produces the overrun, so there is none.
//
// ONE DELIBERATE, BOUNDED DIVERGENCE FROM THE ORIGINAL, stated rather than smoothed over.
// count_unit_build_sources fills its out-table over 1..G_UNIT_COUNT_TOTAL INCLUSIVE
// (0x004e2201-0x004e2211) and NEVER WRITES INDEX 0, so in the original `by_type[0]` is whatever the
// previous stack frame left there. This module zero-initialises the buffer, i.e. reads a
// deterministic 0 where the original reads garbage. It is reachable only through a cfg row with
// `.type == 2` and `.index == 0`; no such row exists in the shipped cfg (index 0 is not a unit
// type). Uninitialised reads are UB in C++ and there is no way to reproduce another frame's
// residue, so this is the one place fidelity is knowingly traded for definedness.
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

// The `.type` value on a cfg Invention row that names a trainable unit (CMP byte ptr [..],0x2
// @0x004e6da0). The other E_INVETION_TYPE values are skipped outright.
inline constexpr uint8_t INVENTION_TYPE_UNIT = 2;
// player_data::ai_train_source_state values this pass writes. 0 is "no source", written by the
// clear loop; 1 and 2 are the two `f3` cases (MOV byte ptr [..],0x1 @0x004e6dfd / 0x2 @0x004e6dec).
inline constexpr uint8_t TRAIN_SOURCE_READY  = 1;
inline constexpr uint8_t TRAIN_SOURCE_F3_SET = 2;
// The running-minimum seed for the role pick (MOV dword ptr [EBP + -0x1c],0xf4240 @0x004e6e3b).
inline constexpr int32_t TRAIN_ROLE_COUNT_SEED = 1000000;

namespace detail {

// The out-table count_unit_build_sources fills. 100 == UNIT_TYPE_COUNT; named separately because
// what fixes the size is the CALLER's frame (0x1dc - 0x4c == 0x190 == 100 ints), not the cfg.
inline constexpr int32_t TRAIN_SOURCE_TABLE_LEN = 100;

struct train_plan_report {
    bool    ran        = false; // false == returned at the build-plan gate having written nothing
    int32_t best_role  = -1;    // the role pass 3 picked, -1 if none
    int32_t best_unit  = 0;     // the unit type pass 4 picked, 0 == nothing to train
    int32_t queued     = 0;     // queue_train_unit calls made (>= 1 whenever best_unit != 0)
    int32_t marked     = 0;     // rows pass 1 stamped 1 or 2
    int32_t roles_live = 0;     // by_role entries pass 2 left non-zero
};

// The logic over an EXPLICIT state and an INJECTED call set, so `net_selftest.exe aitest` can drive
// it over heap buffers with recording stubs and no game.
train_plan_report plan_unit_training(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                     int32_t player);

} // namespace detail

void plan_unit_training(int32_t player);

} // namespace mh::ai
