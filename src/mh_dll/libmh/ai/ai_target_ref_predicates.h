//
// ai/ai_target_ref_predicates.h -- three pure packed-target-ref predicates (RI-AI / AI1A, batch A
// layer 4).
//
// Unrelated to each other beyond sharing this translation unit (assigned together) and the same
// packed-target-ref vocabulary from ai_state.h. Every one of them is PURE: no write, whole verdict
// in the return value.
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

// The logic over an EXPLICIT state, so net_selftest.exe aitest can drive it over heap buffers with
// no game and no rig. The wrappers below are these applied to state()/live_calls(); the split costs
// one inlined call.
namespace detail {

// llm_strat_ai_target_ref_is_alive @0x004d4082.
//
// Selects the roster with ref_is_building_by_a0 (NOT the 0x40 form -- the two selectors disagree on
// class nibble 0, see ai_state.h). Empty slot (building_id/unit_proto_id == 0) -> dead. Otherwise
// the liveness test is an x87 compare, read verbatim off the assembly's FCOMPP/FNSTSW/SAHF/Jcc
// sequence in BOTH roster branches:
//
//     FLD energy ; FSUB pending_damage ; FLDZ ; FCOMPP ; FNSTSW AX ; SAHF ; J{N}C dead
//
// FCOMPP compares ST(0) (the zero) against ST(1) (energy - pending_damage), so CF ends up set when
// 0.0 < diff (alive) -- and an UNORDERED compare (diff is NaN, x87 exceptions masked) also sets CF,
// so the original reports a NaN energy as ALIVE. Writing the test as
// `energy - pending_damage <= 0.0 -> dead` reproduces this for free: IEEE `<=` is false for NaN,
// so the dead branch is skipped and the function falls through to alive, exactly matching the
// assembly. The tempting rewrite `!(diff > 0.0) -> dead` is WRONG -- `>` is also false for NaN, so
// its negation is true and a NaN energy would (incorrectly) read as dead. Do not simplify to that
// form.
int32_t target_ref_is_alive(const ai_view &v, uint32_t target_ref_packed, int32_t target_index);

// llm_strat_ai_target_ref_has_ground_weapon @0x004d73dd.
//
// UNIT case (ref_is_building_by_a0 false): the function's own body is only the branch test --
// it TAIL-JUMPS to llm_strat_unit_has_ground_weapon (0x004d7377) with the ref UNMASKED, routed here
// through calls.unit_has_ground_weapon.
//
// BUILDING case (ref_is_building_by_a0 true): this IS the function's own inline body (there is no
// separate bldg_has_ground_weapon) -- it masks the owner nibble, reads
// Building[buildings[owner][idx].building_id].weapon_id, and if nonzero tests
// Weapon[weapon_id].target & 1 (ground-capable; confirmed from THIS function's own
// `TEST byte ptr [...],0x1` at 0x004d744e, not inferred from the aa sibling's mask). The body then
// JMPs to 0x004d74cf, an address INSIDE llm_strat_bldg_has_aa_weapon -- it borrows that function's
// epilogue rather than calling it, so nothing is routed through calls.bldg_has_aa_weapon here.
int32_t target_ref_has_ground_weapon(const ai_view &v, const ai_calls &gc, uint32_t target_ref_packed,
                                     int32_t target_index);

// llm_strat_ai_target_ref_has_aa_weapon @0x004d7457 (0x15 bytes -- the whole function).
//
// UNIT case (ref_is_building_by_a0 false): TAIL-JUMPS to llm_strat_unit_has_aa_weapon (0x004d7311)
// with the ref UNMASKED, routed through calls.unit_has_aa_weapon -- same shape as the ground
// predicate's unit case.
//
// BUILDING case (ref_is_building_by_a0 true): masks the owner nibble into EAX and FALLS THROUGH
// (no jump at all) into llm_strat_bldg_has_aa_weapon (0x004d746c), simply the next function in the
// image -- a tail call spelled without a jump. Routed through calls.bldg_has_aa_weapon, passing the
// ALREADY-MASKED owner nibble as its `player` argument and target_index unchanged as its
// `building_index` argument; that callee does its own `& 2` test and is out of this translation's
// scope.
int32_t target_ref_has_aa_weapon(const ai_calls &gc, uint32_t target_ref_packed, int32_t target_index);

} // namespace detail

int32_t target_ref_is_alive(uint32_t target_ref_packed, int32_t target_index);
int32_t target_ref_has_ground_weapon(uint32_t target_ref_packed, int32_t target_index);
int32_t target_ref_has_aa_weapon(uint32_t target_ref_packed, int32_t target_index);


} // namespace mh::ai
