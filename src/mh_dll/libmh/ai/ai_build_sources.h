//
// ai/ai_build_sources.h -- "which unit types can I start building RIGHT NOW" (RI-AI / AI1B layer 3).
//
// llm_strat_ai_count_unit_build_sources @0x004e21e4, `void __watcall f(int player, int *out_counts)`,
// player in EAX and the out-table in EDX.
//
// It is pass 2's input for llm_strat_ai_plan_unit_training: a table indexed by UNIT TYPE holding,
// for each type, HOW MANY of the player's standing buildings list that type in their cfg
// production menu -- then vetoed to zero for every type whose HOUSING CLASS is already full.
// Three passes:
//
//   pass 1  zero out_counts[1 .. G_UNIT_COUNT_TOTAL]. INCLUSIVE (JBE @0x004e220b) and it starts at
//           index 1, so out_counts[0] is never written by this function at all.
//   pass 2  walk the player's building roster COUNT-DRIVEN (see below); for each occupied slot,
//           add 1 to out_counts[t] for every unit type t whose cfg Building[bid].unit_quant[t] is
//           strictly greater than 0.0. The inner bound is INCLUSIVE too (JBE @0x004e2297).
//   pass 3  for every unit type 1 .. G_UNIT_COUNT_TOTAL, look up Unit[t].ai_unit and, if the
//           matching housing class is at or over its LATCHED capacity, force out_counts[t] = 0.
//
// THE ROSTER WALK IS COUNT-DRIVEN, NOT INDEX-BOUNDED, and it is the shape to get right. The live
// count is `buildings[player][0].index` (a WORD read, MOVZX @0x004e2230) and slot 0 is a HEADER, so
// the scan starts at slot 1. The counter is decremented ONLY on a slot whose building_id is
// non-zero (0x004e229f), and the slot cursor advances on every iteration (0x004e22a0) -- so the
// loop runs until it has SEEN that many occupied slots, and an empty slot costs a cursor step and
// nothing else. NOTHING BOUNDS THE CURSOR. If the header count is larger than the number of
// occupied slots actually present, the original walks straight off the end of the player's 100-slot
// row into the next player's. That is the original's behaviour and it is reproduced verbatim; a
// reimplementation that clamped the cursor to BUILDINGS_PER_PLAYER would diverge on exactly the
// corrupted state that makes the overrun happen.
//
// THE HOUSING VETO USES THE LATCHED COLUMN. `cap_prev_*`, not `cap_accum_*` -- the accumulator is
// what the capacity-granting buildings add into during a tick, and the latched column is what the
// previous tick froze. Reading the accumulator would make the veto depend on where in the tick the
// AI happened to run. Four classes, and the class-to-role map is read off the four comparison
// blocks rather than guessed:
//   ai_unit 2, 3, 4, 5  -> used_vehicles vs cap_prev_vehicles   (0x004e22bd .. 0x004e22f2)
//   ai_unit 1           -> used_soldiers vs cap_prev_soldiers   (0x004e2309 .. 0x004e231d)
//   ai_unit 6           -> used_helis    vs cap_prev_helis      (0x004e233a .. 0x004e234e)
//   ai_unit 7, 8        -> used_planes   vs cap_prev_planes     (0x004e236b .. 0x004e2388)
// The four blocks are SEQUENTIAL, not else-if: each falls through into the next. A role can only
// match one of them, so the shape is unobservable -- it is kept anyway.
//
// THE COMPARE THAT DECIDES THE VETO IS SIGNED (JL @0x004e22f2 and its three siblings), so
// `used >= cap` vetoes. Both columns are plain `int` and the sim keeps them non-negative, so the
// signedness is not reachable in a healthy world -- recorded because it was read, not inferred.
//
// THE UNIT-TYPE BOUND IS UNSIGNED (JBE) against cfg_unit_sec->total, which is `uint`. Every loop
// here uses it and every one is inclusive, so the table is written at index total -- one past what
// a half-open reading would touch.
//
// FLOAT: the only FP in the body is `0.0 < Building[bid].unit_quant[t]`, spelled FLDZ / FCOMP /
// FNSTSW / SAHF / JNC @0x004e2283-0x004e228f. FCOMP compares ST0 (0.0) against the memory operand
// and JNC skips the increment when the carry flag is CLEAR, i.e. when NOT (0.0 < quant). So the
// test is STRICTLY GREATER THAN ZERO and a NaN entry counts as "cannot build" (an unordered compare
// sets C0, C2 and C3, so carry is set -- the one case where this comment would be wrong is if a cfg
// could hold a NaN, and no cfg parser path produces one). A negative quant also reads as "cannot
// build", which is why the test is not `!= 0.0`.
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

// What the offline test reads to tell a real pass from a vacuous one -- there is no shadow arm here
// (see below), so these counters are the only window into which branches a call actually took. None
// of this exists in the original.
struct build_sources_report {
    int32_t roster_count = 0; // buildings[player][0].index, the header count the walk was driven by
    int32_t scanned      = 0; // occupied slots actually visited
    int32_t empty_slots  = 0; // slots skipped because building_id == 0
    int32_t max_cursor   = 0; // highest roster slot index touched -- > BUILDINGS_PER_PLAYER means
                              // the count-driven walk ran off the player's row
    int32_t types_hit = 0;    // out_counts entries left non-zero
    int32_t vetoed    = 0;    // entries forced back to 0 by the housing veto
};

namespace detail {

// llm_strat_ai_count_unit_build_sources @0x004e21e4.
//
// `out_counts` is the CALLER's buffer (plan_unit_training's stack scratch), not game state, so it
// is a plain pointer rather than something reached through the store. It must have room for
// [0 .. cfg_unit_sec->total] inclusive.
build_sources_report count_unit_build_sources(const ai_view &v, int32_t player, int32_t *out_counts);


} // namespace detail

// The production wrapper: detail:: applied to state().
void count_unit_build_sources(int32_t player, int32_t *out_counts);

} // namespace mh::ai
