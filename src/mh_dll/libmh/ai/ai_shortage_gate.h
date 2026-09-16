//
// ai/ai_shortage_gate.h -- the AI's SILO-capacity gate (RI-AI / AI1B, batch B layer 3).
//
// This asks "is my STORAGE CAPACITY running short relative to what I am holding", and only if it is
// does it go on to ask the count-vs-cap question the second half of the name refers to.
//
// RENAMED 2026-08-23. Both halves -- the Ghidra symbol and this C++ function -- used to be called
// `resource_shortage_and_cap_check`, which reads as a test on resource STOCKS. It is not one, and
// the confusion was live rather than cosmetic: `resource shortage` IS a real neighbouring concept in
// this same subsystem, with its own per-player state field (ai_resource_shortage_state, 0..3), its
// own candidate array (ai_resource_shortage_candidates[4]) and its own function
// (llm_strat_ai_react_resource_shortage) -- all of which are correctly named and untouched. This one
// was the odd body wearing the family's name. ghidra_findings 2026-08-02-2250-5 raised it, deferred
// it once for budget, and this is the apply. THE FILE NAME IS DELIBERATELY UNCHANGED: `shortage
// gate` still describes what the module is, and renaming it would churn the vcxproj and every
// include for no reader's benefit.
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

namespace detail {

// The x87 half of the test, kept as the ORIGINAL'S OWN INSTRUCTIONS rather than as C++ arithmetic,
// for the same reason mh::ai::detail::x87_ratio exists in ai_spend_rate.cpp: the original never
// rounds the quotient to a double. It divides in the x87 stack at 64-bit-mantissa extended
// precision and compares that register directly against the widened float (FDIV @0x004e38d1, FLD
// float @0x004e38d4, FCOMPP @0x004e38da). An SSE2 `double` quotient rounds once more, and the
// verdict differs whenever the true ratio falls between the two roundings of the threshold -- rare,
// but a divergence the shadow oracle would report and a desync if promoted.
//
// Returns 1 when the resource is SHORT, i.e. when the threshold is STRICTLY GREATER than the
// quotient: the original's `JBE` @0x004e38df leaves the loop running when C0 (below) or C3 (equal)
// is set, so shortage is exactly "neither set".
int32_t x87_capacity_short(int32_t capacity, double holdings, const float *threshold);

// llm_strat_ai_storage_capacity_short_and_cap_check @0x004e3872. ONE parameter -- see the plate; the
// EDX the old plate read as a second argument is the loop counter (MOV EDX,1 @0x004e3887).
//
// For each resource id d in 1..4, in order:
//   holdings = (double)(int)player_resources[player][d]        FILD dword @0x004e389e
//              -- and if that is +-0.0 it becomes 1.0          @0x004e38a8-@0x004e38be
//   ratio    = (double)(int)storage[player].cap_prev[d] / holdings   FILD @0x004e38ca, FDIV @0x004e38d1
//   if (silo_ratio > ratio) return count_by_id(player, ai_build_candidate_shortage)
//                                      <u ai_score_bldg_type_b;
// and if no d is short, 0.
//
// THREE THINGS THAT LOOK LIKE DETAIL AND ARE NOT.
//  * The zero-flush is on the DIVISOR, not the dividend. That is the second independent sign of
//    which array is which -- the flush exists to stop a division by zero, so the array it protects
//    is the one underneath the bar.
//  * The final compare is UNSIGNED (`SETC` @0x004e390a), even though both operands are int32 fields.
//    With a negative ai_score_bldg_type_b the unsigned reading makes the test pass where a signed
//    one would fail.
//  * It STOPS at the first short resource. Resource 2 being short is never even looked at when
//    resource 1 already was, so the returned count/cap comparison is about the shortage CANDIDATE
//    type, which is a single per-player field -- it does not vary with d.
int32_t storage_capacity_short_and_cap_check(const ai_view &v, const ai_calls &gc, int32_t player);

} // namespace detail

int32_t storage_capacity_short_and_cap_check(int32_t player);

} // namespace mh::ai
