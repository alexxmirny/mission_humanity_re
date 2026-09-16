//
// ai/ai_target_query.h -- two pure AI target-reference predicates (RI-AI / AI1A, batch A layer 3).
//
// Unrelated to each other beyond sharing this translation unit (assigned together) and the same
// packed-target-ref vocabulary from ai_state.h. Neither writes anything; each carries its verdict
// entirely in its return value.
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

// The logic over an EXPLICIT state, so net_selftest.exe aitest can drive it over heap buffers with
// no game and no rig. The wrappers below are these applied to state()/live_calls(); the split costs
// one inlined call.
namespace detail {

// llm_strat_ai_target_ref_has_engageable_weapon @0x004d3cf0.
//
// Builds a 2-bit "targeting mode" selector from attacker_weapon_flags -- the two arms are NOT
// symmetric, read straight off 0x004d3d00-0x004d3d16 (`TEST BL,0x40 / JNZ mode1 ; TEST BL,0x20 / JZ
// mode1 ; mode = 2`):
//   (flags & 0x40) != 0        -> mode = 1
//   else (flags & 0x20) != 0   -> mode = 2
//   else                       -> mode = 1   (same value as the first case, not a third outcome)
// Then calls BOTH target_ref_has_ground_weapon (-> mode bit 0) and target_ref_has_aa_weapon
// (-> mode bit 1) UNCONDITIONALLY, in that order -- the call COUNT is observable to the shadow
// oracle, so neither predicate is short-circuited on `mode` -- and returns whether the mode-selected
// bit came back set.
int32_t target_ref_has_engageable_weapon(const ai_calls &gc, int32_t target_ref_kind,
                                         int32_t target_ref_index, uint32_t attacker_weapon_flags);

// llm_strat_ai_target_dist_sq @0x004d81a6.
//
// Resolves each of the two packed refs to an (x, y) INDEPENDENTLY -- BUILDING when
// ref_is_building_by_40 (NOT ref_is_building_by_a0 -- the two disagree on class nibble 0, see
// ai_state.h), UNIT otherwise, each indexed by its own (ref & 0xf) owner nibble and its own idx
// argument -- then tail-calls toroidal_dist_sq(a.x, a.y, b.x, b.y). Both coordinate fields are
// `byte` in the roster and read zero-extended in the original (MOVZX), never sign-extended.
//
// The argument order is confirmed from the cdecl push order at the shared call site
// (0x004d82bc): the pushes across all four unit/building combinations are, in program order,
// b.y, b.x, a.y, a.x -- and since cdecl pushes right-to-left, the LAST push is the FIRST argument,
// giving (a.x, a.y, b.x, b.y). Reading the push order top-to-bottom as the argument order would
// give the wrong (reversed) call.
uint32_t target_dist_sq(const ai_view &v, const ai_calls &gc, uint32_t ref_a, int32_t idx_a,
                        uint32_t ref_b, int32_t idx_b);

} // namespace detail

int32_t  target_ref_has_engageable_weapon(int32_t target_ref_kind, int32_t target_ref_index,
                                          uint32_t attacker_weapon_flags);
uint32_t target_dist_sq(uint32_t ref_a, int32_t idx_a, uint32_t ref_b, int32_t idx_b);

} // namespace mh::ai
