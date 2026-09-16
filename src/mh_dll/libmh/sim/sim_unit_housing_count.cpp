//
// sim/sim_unit_housing_count.cpp -- see sim_unit_housing_count.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_unit_housing_count_add_0049779b.asm and, for the fifth-slice addition,
// tmp/decomp/llm_strat_unit_housing_count_remove_00497842.asm), not from Ghidra's C: each draft's
// overall shape (nested if/else-if over cVar1) reads correctly and matches the ladder derived below,
// but every threshold, every field offset, and the signed-vs-unsigned reading of every jcc was
// independently re-walked against the raw CMP/JC/JBE targets per the translator brief. (For
// unit_housing_count_remove, the draft .c's arithmetic -- SUB soldier_count / -1 on vehicles/helis/
// planes -- was independently confirmed to match the DEC/SUB opcodes exactly; unlike some other
// units in this project, this draft did not lie.)
//
#include "sim/sim_unit_housing_count.h"

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace detail {

void unit_housing_count_add(const sim_view &v, sim_store &own, int32_t player, int32_t unit_proto_id) {
    // 0x004977b8-0x004977c5: Unit[unit_proto_id].type -- a full dword read (IMUL by 0x23f ==
    // sizeof(cfg_unit), landing on the +0xde `type` field, itself declared `uint32_t` in
    // mh_structs.gen.h). Every comparison below is UNSIGNED (JC/JBE), matching that storage.
    const uint32_t type = v.cfg_units[unit_proto_id].type;

    // 0x004977c8-0x00497839: the five-way ladder. See the header for the full derivation and the
    // cfg_enum_E_UNIT_TYPE member names (all four already pinned in sibling sim/ files).
    if (type < UNIT_TYPE_A_HELI) {
        // 0x004977e6-0x004977f6 (sub-ladder, type < 0xf).
        if (type != UNIT_TYPE_UNDEFINED) {
            if (type < UNIT_TYPE_A_WALKER) {
                // LAB_00497820 (0x00497820-0x00497833): used_soldiers += Unit[unit_proto_id].
                // soldier_count -- BOTH sides of this add are full 32-bit (ADD dword ptr,EDX; the
                // source read at 0x0049782d is `MOV EDX,[...]`, not a byte/word load), so this is a
                // plain int32_t accumulate, not a narrowing one.
                own.unit_housing_at(player).used_soldiers += v.cfg_units[unit_proto_id].soldier_count;
            } else {
                // LAB_004977f6 (0x004977f6-0x00497802): used_vehicles += 1 (INC dword ptr).
                own.unit_housing_at(player).used_vehicles += 1;
            }
        }
        // type == UNIT_TYPE_UNDEFINED (LAB_004977f4, 0x004977f4-0x004977f6): nothing -- the
        // sub-ladder's own JC target skips straight to the function's single exit.
    } else if (type < UNIT_TYPE_A_PLANE) {
        // LAB_00497804 (0x00497804-0x00497810): used_helis += 1 (INC dword ptr).
        own.unit_housing_at(player).used_helis += 1;
    } else if (type < UNIT_TYPE_A_HELI_MOTHER) {
        // LAB_00497812 (0x00497812-0x0049781e): used_planes += 1 (INC dword ptr).
        own.unit_housing_at(player).used_planes += 1;
    }
    // type >= UNIT_TYPE_A_HELI_MOTHER (0x13+): nothing. The asm's own final CMP/JBE against 0x18
    // (0x004977da-0x004977e0) is immediately followed by an unconditional JMP to the SAME target
    // (0x00497839) the JBE would have taken -- both arms of that compare converge, so it changes
    // nothing here; see the header note and uncertainties below.
}

// llm_strat_unit_housing_count_remove @0x00497842. Exact mirror of unit_housing_count_add above over
// the SAME four-way type ladder and the same table -- see the header derivation. Every arm
// DECREMENTS instead of incrementing.
void unit_housing_count_remove(const sim_view &v, sim_store &own, int32_t player, int32_t unit_proto_id) {
    // 0x0049785f-0x0049786c: Unit[unit_proto_id].type, same field/read shape as _add (IMUL by
    // 0x23f, dword MOV). Every comparison below is UNSIGNED (JC/JBE), matching the field's uint32_t
    // storage, and the five threshold immediates (0xf, 0x1, 0xa, 0x10, 0x12, 0x18) are bit-for-bit
    // identical to _add's.
    const uint32_t type = v.cfg_units[unit_proto_id].type;

    // 0x0049786f-0x004978da: the five-way ladder, same shape as _add's.
    if (type < UNIT_TYPE_A_HELI) {
        // 0x0049788d-0x0049789b (sub-ladder, type < 0xf).
        if (type != UNIT_TYPE_UNDEFINED) {
            if (type < UNIT_TYPE_A_WALKER) {
                // LAB_004978c7 (0x004978c7-0x004978da): used_soldiers -= Unit[unit_proto_id].
                // soldier_count -- `SUB dword ptr [...],EDX` against a full dword MOV read of
                // soldier_count (0x004978d4), so a plain int32_t decrement-by-value, not narrowing.
                own.unit_housing_at(player).used_soldiers -= v.cfg_units[unit_proto_id].soldier_count;
            } else {
                // LAB_0049789d (0x0049789d-0x004978a9): used_vehicles -= 1 (DEC dword ptr).
                own.unit_housing_at(player).used_vehicles -= 1;
            }
        }
        // type == UNIT_TYPE_UNDEFINED (LAB_0049789b, 0x00497891-0x0049789b): nothing -- the
        // sub-ladder's own JC target jumps straight to the function's single exit.
    } else if (type < UNIT_TYPE_A_PLANE) {
        // LAB_004978ab (0x004978ab-0x004978b7): used_helis -= 1 (DEC dword ptr).
        own.unit_housing_at(player).used_helis -= 1;
    } else if (type < UNIT_TYPE_A_HELI_MOTHER) {
        // LAB_004978b9 (0x004978b9-0x004978c5): used_planes -= 1 (DEC dword ptr).
        own.unit_housing_at(player).used_planes -= 1;
    }
    // type >= UNIT_TYPE_A_HELI_MOTHER (0x13+): nothing. The asm's own final CMP/JBE against 0x18
    // (0x00497881-0x00497887) is immediately followed by an unconditional JMP to the SAME target
    // (0x004978e0) the JBE would have taken -- both arms of that compare converge, exact mirror of
    // _add's own vestigial 0x18 threshold; see the header note and uncertainties below.
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void unit_housing_count_add(int32_t player, int32_t unit_proto_id) {
    sim_state st = state();
    detail::unit_housing_count_add(st.read, st.own, player, unit_proto_id);
}

// Live wrapper for unit_housing_count_remove -- same shape as unit_housing_count_add above.
void unit_housing_count_remove(int32_t player, int32_t unit_proto_id) {
    sim_state st = state();
    detail::unit_housing_count_remove(st.read, st.own, player, unit_proto_id);
}


} // namespace mh::sim
