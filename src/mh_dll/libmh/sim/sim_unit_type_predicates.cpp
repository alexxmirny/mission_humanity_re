//
// sim/sim_unit_type_predicates.cpp -- see sim_unit_type_predicates.h. Translated from the
// DISASSEMBLY (tmp/decomp/llm_strat_unit_is_aircraft_004d45c0.asm,
// tmp/decomp/map_unit_IsAiPlane_004d4658.asm, tmp/decomp/map_unit_IsAiHeli_004d46a4.asm,
// tmp/decomp/map_unit_IsAiGround_004d4760.asm, tmp/decomp/map_unit_IsAiSoldier_004d47be.asm,
// tmp/decomp/llm_strat_unit_get_ai_group_index_004d48fa.asm), not from Ghidra's C -- the six .c
// drafts read correctly for the VALUE comparisons (verified below) but every literal and every
// term-to-CMP mapping was independently re-derived from the listings per house rules.
//
// ---- deriving the UNIT_TYPE_* constants (header) ---------------------------------------------
//
// All five predicates share one index computation: unit_of(v, ref_owner(unit_ref), unit_index) then
// v.cfg_units[that_unit.unit_proto_id].<field> -- read directly off the address arithmetic common to
// all five bodies (`AND EAX,0xf; IMUL EAX,EAX,0x5b04; IMUL EDX,EDX,0xe9; MOVZX _,[EDX+EAX+0xdd8c4a]`
// is exactly UNITS_PER_PLAYER*sizeof(unit) row stride over sizeof(unit) column stride, landing on
// unit_proto_id; the subsequent `*9*64-1` = *575 = sizeof(cfg_unit) turns that id into a cfg_units[]
// byte offset). Confirmed independently: 0xe4a176 (the `type` compare base) and 0xe4a2cf (the
// `ai_unit` compare base in IsAiSoldier) differ by exactly 0x159, matching docs/structs.md's field
// offsets +0xde and +0x237 on cfg_final_struct_Unit (0x237-0xde == 0x159).
//
// is_plane's two CMPs (0x004d4689/0x004d4692, values 0x11/0x12) and is_heli's two (0x004d46d5/
// 0x004d46de, values 0xf/0x10) each match their own draft's "A_x || H_x" term order one-for-one, so
// UNIT_TYPE_A_PLANE=0x11, UNIT_TYPE_H_PLANE=0x12, UNIT_TYPE_A_HELI=0xf, UNIT_TYPE_H_HELI=0x10 read
// directly, no inference needed beyond "first CMP = first OR term" (see the uncertainty note below).
//
// is_aircraft chains TEN such CMPs in the order 0x11,0x12,0xf,0x10,0x13,0x14,0x17,0x18,0x15,0x16
// (0x004d45f5-0x004d464d). The four already pinned above account for the first four; the next two
// (0x13,0x14) match mh::ai::ai_state.h's independently-derived UNIT_TYPE_A_HELI_MOTHER/
// H_HELI_MOTHER exactly, and the following one (0x18) matches its UNIT_TYPE_H_HELI_CARGO -- three
// agreements from a wholly different function's disassembly, which is why the remaining four
// (0x17/0x15/0x16, and 0x18's now-forced pair 0x17=A_HELI_CARGO) are treated as solid rather than
// merely plausible: the draft's own term order is A_HELI_CARGO/H_HELI_CARGO then
// A_HELI_SHUTTLE/H_HELI_SHUTTLE, so 0x17=A_HELI_CARGO (H_HELI_CARGO=0x18 already pinned) and
// 0x15=A_HELI_SHUTTLE, 0x16=H_HELI_SHUTTLE follow the same "first CMP = first term" rule.
//
// is_ground chains FOUR CMPs in the order 0xe,0xd,0xb,0xc (0x004d4791-0x004d47ac) against the
// draft's "A_GROUND || H_GROUND || A_WALKER || H_WALKER" -- so UNIT_TYPE_A_GROUND=0xe,
// UNIT_TYPE_H_GROUND=0xd, UNIT_TYPE_A_WALKER=0xb, UNIT_TYPE_H_WALKER=0xc. Note these do NOT follow
// an "A < H" pattern the way the heli/plane pairs do (A_GROUND 0xe > H_GROUND 0xd, but A_WALKER 0xb <
// H_WALKER 0xc) -- read as-is from the CMP order, not normalized to a guessed pattern.
//
#include "sim/sim_unit_type_predicates.h"

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace detail {

int32_t is_aircraft(const sim_view &v, uint32_t unit_ref, int32_t unit_index) {
    // 0x004d45ca/0x004d45cc: TEST AL,0x40 / JNZ false.
    if ((unit_ref & REF_BUILDING_BIT) != 0) return 0;

    const unit    &u   = unit_of(v, ref_owner(unit_ref), unit_index);
    const uint32_t typ = v.cfg_units[u.unit_proto_id].type;

    // 0x004d45f5-0x004d464d: ten CMPs, JZ-to-true on any match, XOR EAX,EAX (false) at the end.
    if (typ == UNIT_TYPE_A_PLANE || typ == UNIT_TYPE_H_PLANE || typ == UNIT_TYPE_A_HELI ||
        typ == UNIT_TYPE_H_HELI || typ == UNIT_TYPE_A_HELI_MOTHER || typ == UNIT_TYPE_H_HELI_MOTHER ||
        typ == UNIT_TYPE_A_HELI_CARGO || typ == UNIT_TYPE_H_HELI_CARGO ||
        typ == UNIT_TYPE_A_HELI_SHUTTLE || typ == UNIT_TYPE_H_HELI_SHUTTLE) {
        return 1;
    }
    return 0;
}

int32_t is_plane(const sim_view &v, uint32_t unit_ref, int32_t unit_index) {
    // 0x004d4662/0x004d4664: TEST AL,0x40 / JNZ false.
    if ((unit_ref & REF_BUILDING_BIT) != 0) return 0;

    const unit    &u   = unit_of(v, ref_owner(unit_ref), unit_index);
    const uint32_t typ = v.cfg_units[u.unit_proto_id].type;

    // 0x004d4689-0x004d4699.
    if (typ == UNIT_TYPE_A_PLANE || typ == UNIT_TYPE_H_PLANE) return 1;
    return 0;
}

int32_t is_heli(const sim_view &v, uint32_t unit_ref, int32_t unit_index) {
    // 0x004d46ae/0x004d46b0: TEST AL,0x40 / JNZ false.
    if ((unit_ref & REF_BUILDING_BIT) != 0) return 0;

    const unit    &u   = unit_of(v, ref_owner(unit_ref), unit_index);
    const uint32_t typ = v.cfg_units[u.unit_proto_id].type;

    // 0x004d46d5-0x004d46e5.
    if (typ == UNIT_TYPE_A_HELI || typ == UNIT_TYPE_H_HELI) return 1;
    return 0;
}

int32_t is_ground(const sim_view &v, uint32_t unit_ref, int32_t unit_index) {
    // 0x004d476a/0x004d476c: TEST AL,0x40 / JNZ false.
    if ((unit_ref & REF_BUILDING_BIT) != 0) return 0;

    const unit    &u   = unit_of(v, ref_owner(unit_ref), unit_index);
    const uint32_t typ = v.cfg_units[u.unit_proto_id].type;

    // 0x004d4791-0x004d47b3.
    if (typ == UNIT_TYPE_A_GROUND || typ == UNIT_TYPE_H_GROUND || typ == UNIT_TYPE_A_WALKER ||
        typ == UNIT_TYPE_H_WALKER) {
        return 1;
    }
    return 0;
}

int32_t is_soldier(const sim_view &v, uint32_t unit_ref, int32_t unit_index) {
    // 0x004d47cb/0x004d47cd: TEST AL,0x40 / JNZ false.
    if ((unit_ref & REF_BUILDING_BIT) != 0) return 0;

    // Reads Unit[proto].ai_unit -- a DIFFERENT cfg field from `type` above (0x004d47f7:
    // CMP dword ptr [...+0xe4a2cf],0x1, base offset by +0x159 from the `type` compares' 0xe4a176,
    // matching cfg_final_struct_Unit's +0x237 vs +0xde field offsets exactly).
    const unit    &u        = unit_of(v, ref_owner(unit_ref), unit_index);
    const uint32_t ai_class = v.cfg_units[u.unit_proto_id].ai_unit;

    if (ai_class == AI_UNIT_SOLDIER) return 1;
    return 0;
}

uint32_t get_ai_group_index(const sim_view &v, int32_t player, int32_t unit_index) {
    // 0x004d4904-0x004d4910: NO mask, NO 0x40 test -- `player` (EAX) is used raw as the row
    // multiplicand, unlike every predicate above. See the file header.
    const unit &u = unit_of(v, (uint32_t)player, unit_index);
    return (uint32_t)u.ai_group_index;
}

} // namespace detail

// ---- the public wrappers ------------------------------------------------------------------------

int32_t is_aircraft(uint32_t unit_ref, int32_t unit_index) {
    const sim_view v = state().read;
    return detail::is_aircraft(v, unit_ref, unit_index);
}

int32_t is_plane(uint32_t unit_ref, uint32_t unit_id) {
    const sim_view v = state().read;
    return detail::is_plane(v, unit_ref, (int32_t)unit_id);
}

int32_t is_heli(uint32_t unit_ref, uint32_t unit_id) {
    const sim_view v = state().read;
    return detail::is_heli(v, unit_ref, (int32_t)unit_id);
}

int32_t is_ground(uint16_t unit_ref, uint32_t unit_id) {
    const sim_view v = state().read;
    return detail::is_ground(v, (uint32_t)unit_ref, (int32_t)unit_id);
}

int32_t is_soldier(uint32_t unit_ref, uint32_t unit_id) {
    const sim_view v = state().read;
    return detail::is_soldier(v, unit_ref, (int32_t)unit_id);
}

uint32_t get_ai_group_index(int32_t player, int32_t unit_index) {
    const sim_view v = state().read;
    return detail::get_ai_group_index(v, player, unit_index);
}


} // namespace mh::sim
