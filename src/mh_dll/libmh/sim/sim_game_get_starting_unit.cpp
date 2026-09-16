//
// sim/sim_game_get_starting_unit.cpp -- see sim_game_get_starting_unit.h. Translated from the
// DISASSEMBLY (tmp/decomp/game_GetStartingUnit_0045eded.asm), cross-checked against the Ghidra .c
// draft, which reads correctly here -- a straightforward bounded loop with two independent,
// non-short-circuited match arms. No FP, no phantom store, no re-materialisation artifact found on
// this body; the opening `CALL utils_assert_stack_capacity` is the inert Watcom prologue helper
// (translator brief rule 6) and is omitted.
//
// LOOP BOUND (0x0045ee0f-0x0045ee15): `MOV local_1c,1` (0x0045ee08) then, at the top of the loop,
// `CMP local_1c,0x64 / JL continue / JMP exit(return 0)` -- the loop body runs while local_1c < 100,
// so the checked range is 1..99 inclusive; index 0 is never read. This is the "index 0 is skipped on
// purpose per the loop bound" the task brief already states, not an off-by-one to fix.
//
// MATCH ARMS (0x0045ee1c-0x0045ee48): TWO SEPARATE `IMUL EAX,local_1c,0x23f` computations (one per
// arm, sizeof(cfg_unit) row stride onto cfg_units[local_1c].type at +0 within the record) rather than
// one shared index reused -- reproduced here as two independent `v.cfg_units[i].type` reads to match
// the assembly's shape (a compiler would fold the redundant index arithmetic regardless, so this is
// not a behavioural choice, just a faithful transcription). Arm 1: `type==A_HELI_MOTHER(0x13) &&
// race==RACE_ALIEN(2)` -> match, return `i`. Falling through (either because type!=0x13, or
// type==0x13 but race!=2) reaches arm 2: `type==H_HELI_MOTHER(0x14) && race==RACE_HUMAN(1)` -> match,
// return `i`. Falling through both arms advances to the next index. Both arms independent (neither
// short-circuits the other via a shared computed flag), matching the JNZ/JZ chain exactly.
//
#include "sim/sim_game_get_starting_unit.h"

#include "sim/sim_unit_type_predicates.h" // UNIT_TYPE_A_HELI_MOTHER / UNIT_TYPE_H_HELI_MOTHER
#include "ai/ai_state.h"                  // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace detail {

int32_t game_get_starting_unit(const sim_view &v, uint32_t race) {
    for (int32_t i = 1; i < 100; ++i) {
        // Arm 1 (0x0045ee1c-0x0045ee30): ALIEN's mother-heli.
        if (v.cfg_units[i].type == UNIT_TYPE_A_HELI_MOTHER && race == RACE_ALIEN) return i;
        // Arm 2 (0x0045ee32-0x0045ee46): HUMAN's mother-heli.
        if (v.cfg_units[i].type == UNIT_TYPE_H_HELI_MOTHER && race == RACE_HUMAN) return i;
    }
    return 0;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

int32_t game_get_starting_unit(uint32_t race) {
    const sim_view v = state().read;
    return detail::game_get_starting_unit(v, race);
}


} // namespace mh::sim
