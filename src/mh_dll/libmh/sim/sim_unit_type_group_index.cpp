//
// sim/sim_unit_type_group_index.cpp -- see sim_unit_type_group_index.h. Translated from the
// DISASSEMBLY (tmp/decomp_sim/llm_strat_unit_type_group_index_00492b60.asm). The .c draft's
// nested-if shape was independently re-derived from the raw CMP/JC/JBE chain and confirmed
// boolean-equivalent (every branch target and MOV-immediate cross-checked against the .asm) before
// being kept, not copied on trust -- see the header banner for the band table.
//
#include "sim/sim_unit_type_group_index.h"

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace {

// ---- cfg_enum_E_UNIT_TYPE band boundaries this function's CMP chain tests, per naming rule 17a
// ("existing enum -> use its member names"). Values are the DTM dump sim_weapon_damage_calc.cpp
// already closed as a declared need (see that file + this header's banner). File-private per the
// mh/sim convention this file's own sibling already uses (sim_weapon_damage_calc.cpp's own
// E_UNIT_TYPE_A_GROUND) -- not shared/exported, since no generated C++ enum exists for this Ghidra
// type (mh_structs.gen.h types `cfg_final_struct_Unit::type` as a bare uint32_t).
inline constexpr uint32_t E_UNIT_TYPE_UNDEFINED    = 0x0;
inline constexpr uint32_t E_UNIT_TYPE_H_INFANTRY_5 = 0xa;
inline constexpr uint32_t E_UNIT_TYPE_A_HELI       = 0xf;
inline constexpr uint32_t E_UNIT_TYPE_H_HELI       = 0x10;
inline constexpr uint32_t E_UNIT_TYPE_H_PLANE      = 0x12;

// ---- the four "unit type group" result codes this function returns -- a SEPARATE, small ID space
// from the enum above despite the numeric overlap on the heli/plane pair (see the header banner).
inline constexpr int32_t GROUP_IDX_NONE           = 0x0;  // 0x00492bdd
inline constexpr int32_t GROUP_IDX_GROUND_VEHICLE = 0xf;  // 0x00492bb9
inline constexpr int32_t GROUP_IDX_INFANTRY       = 0x10; // 0x00492bd4
inline constexpr int32_t GROUP_IDX_HELI           = 0x11; // 0x00492bc2
inline constexpr int32_t GROUP_IDX_PLANE          = 0x12; // 0x00492bcb

} // namespace

namespace detail {

int32_t unit_type_group_index(const sim_view &v, int32_t unit_id) {
    // KEPT AS uint32_t (reimpl-verify caught a real divergence 2026-08-20: the asm's whole branch
    // chain is UNSIGNED CMP/JC/JBE on the raw 32-bit field, so a signed int32_t compare disagrees
    // for any bit-31-set value -- e.g. type==0xffffffff would signed-compare as -1, incorrectly
    // routing into the infantry/ground split instead of falling through to GROUP_IDX_NONE). Compare
    // in unsigned space throughout, matching ai_army_milestone.cpp's existing treatment of this same
    // field rather than ai_active_unit_tick.cpp's signed one.
    const uint32_t type = v.cfg_units[unit_id].type;

    // 0x00492b8b/0x00492b8f: outer split, type < A_HELI, UNSIGNED (matches the asm's JC).
    if (type < E_UNIT_TYPE_A_HELI) {
        // 0x00492ba9/0x00492bad (-> 0x00492bb7 -> 0x00492bdd): type == UNDEFINED (0) falls straight
        // through to the shared GROUP_IDX_NONE tail without touching the infantry/ground split below.
        if (type == E_UNIT_TYPE_UNDEFINED) {
            return GROUP_IDX_NONE;
        }
        // 0x00492baf/0x00492bb3: 1 <= type <= H_INFANTRY_5 (0xa) -> infantry (0x00492bd4); else
        // (0xb..0xe, A_WALKER..A_GROUND) -> ground vehicle (0x00492bb9).
        if (type <= E_UNIT_TYPE_H_INFANTRY_5) {
            return GROUP_IDX_INFANTRY;
        }
        return GROUP_IDX_GROUND_VEHICLE;
    }

    // 0x00492b91/0x00492b95: A_HELI (0xf) <= type <= H_HELI (0x10) -> heli.
    if (type <= E_UNIT_TYPE_H_HELI) {
        return GROUP_IDX_HELI;
    }
    // 0x00492b97/0x00492b9b: H_HELI < type <= H_PLANE (0x12) -> plane.
    if (type <= E_UNIT_TYPE_H_PLANE) {
        return GROUP_IDX_PLANE;
    }
    // 0x00492b9d-0x00492ba3: type > H_PLANE -- the CMP against 0x18 here is DEAD (both its
    // JBE-taken edge at 0x00492ba1 and the unconditional JMP fallthrough at 0x00492ba3 land on the
    // identical LAB_00492bdd), so every type above H_PLANE returns GROUP_IDX_NONE regardless of the
    // 0x18 boundary -- not reproduced as a branch since it has no observable effect.
    return GROUP_IDX_NONE;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

int32_t unit_type_group_index(int32_t unit_id) {
    const sim_view v = state().read;
    return detail::unit_type_group_index(v, unit_id);
}


} // namespace mh::sim
