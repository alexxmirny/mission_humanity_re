//
// ai/ai_group_muster_pick.cpp -- see ai_group_muster_pick.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_ai_group_find_slowest_unit_004d6be3.asm and
// tmp/decomp/llm_strat_ai_group_pick_best_weapon_unit_004d6c85.asm), not from Ghidra's .c:
//
//   * Both decompiles alias the SAME decompiler variable across two different meanings mid-loop
//     (find_slowest_unit's `uVar1` is the linked-list index before the first read of the loop body
//     and the unit's cfg proto id immediately after; pick_best_weapon_unit's `local_20` is a 64-bit
//     scratch qword whose only-ever-read half is the low dword). Both are transcribed straight off
//     the assembly's actual register/stack slots below rather than through the .c's aliasing.
//   * pick_best_weapon_unit's `float10 fVar5` / `(ulonglong)ROUND(fVar5)` is Ghidra's rendering of
//     the inlined `utils_math_trunc` (CALL 0x004d0596) plus its FISTP store -- there is no `ROUND`
//     call in the assembly, and the real operation is TRUNCATE toward zero, not round-to-nearest. See
//     the header banner on weapon_power_add_and_trunc.
//
// EVERY ABSOLUTE RE-DERIVED against addr/mh_structs.gen.h's offsetof asserts (unit_group::head_unit
// @0xa; unit::unit_proto_id @0x2, ::weapons @0x37, ::ai_group_next @0xd4; cfg_final_struct_Unit::
// step_speed @0x1d [double[9]]; cfg_final_struct_Weapon::target @0x1, ::power @0x92 [double[9]])
// rather than trusted from the .c's raw literals, and every one of them checks out exactly against
// the .asm's displacements (e.g. 0xdd8c4a - 0xdd8c48(unit base) == 0x2 == unit_proto_id; 0xc3a5b2 -
// 0xc3a520(Weapon base) == 0x92 == power). So no byte offset and no literal VA appears below (Law 1).
//
#include "ai/ai_group_muster_pick.h"

#include "fp/x87_shapes.h" // CRT-X87: the hoisted x87 blocks (the asm moved, it did not change)

namespace mh::ai {
namespace detail {

int32_t weapon_power_add_and_trunc(uint32_t running_total, const double *addend) {
    return ::mh::fp::trunc_add_u32_double_low(running_total, addend);
}

uint32_t group_find_slowest_unit(const ai_view &v, int32_t player_id, int32_t group_index) {
    const unit_group &grp = v.players[player_id].ai_groups[group_index];

    // `best_speed` MUST be float, not double -- see the header banner: the original narrows its
    // running minimum back to float32 on every update (`FSTP float ptr`), even though the column
    // it is compared against is a double.
    float    best_speed = 1e+09f; // MOV dword ptr[ESP],0x4e6e6b28 (0x004d6bf7)
    uint32_t best_index = 0;      // XOR ESI,ESI (0x004d6bfe)

    uint32_t unit_id = grp.head_unit; // 0x004d6c1a
    while (unit_id != 0) {
        const uint16_t proto_id = unit_of(v, (uint32_t)player_id, (int32_t)unit_id).unit_proto_id;
        const double   speed    = v.cfg_units[proto_id].step_speed[player_id]; // 0x004d6c4e
        if (speed < best_speed) {                                              // JBE skips @0x004d6c57 -- update on ST(0) > src
            best_speed = (float)speed;                                         // FSTP float ptr[ESP] (0x004d6c5f)
            best_index = unit_id;                                              // MOV ESI,EBX (0x004d6c62)
        }
        unit_id = unit_of(v, (uint32_t)player_id, (int32_t)unit_id).ai_group_next; // 0x004d6c70
    }
    return best_index;
}

uint32_t group_pick_best_weapon_unit(const ai_view &v, int32_t player_id, int32_t group_index) {
    const unit_group &grp = v.players[player_id].ai_groups[group_index];

    uint32_t best_total = 0; // dword[ESP+0xc] (0x004d6c9a)
    uint32_t best_index = 0; // dword[ESP+0x8] (0x004d6ca2)

    uint32_t unit_id = grp.head_unit; // 0x004d6cc4
    while (unit_id != 0) {
        uint32_t running_total = 0; // XOR EBX,EBX -- reset PER MEMBER (0x004d6cd1)

        const unit &u = unit_of(v, (uint32_t)player_id, (int32_t)unit_id);
        for (int32_t slot = 0; slot < 4; ++slot) { // CMP EDX,0x4 / JC (0x004d6d2c/0x004d6d2f)
            const uint8_t weapon_id = u.weapons[slot].weapon_id;
            if (weapon_id != 0 &&                             // JZ skip @0x004d6cf1
                (v.cfg_weapons[weapon_id].target & 1) != 0) { // JZ skip @0x004d6d07
                running_total = (uint32_t)weapon_power_add_and_trunc(
                    running_total, &v.cfg_weapons[weapon_id].power[player_id]); // 0x004d6d09-0x004d6d28
            }
        }

        // UNSIGNED `>=`, NOT `>` -- see the header banner: a tie (incl. the first member's own
        // score-0 total against the initial best_total of 0) still overwrites, so the group's first
        // member always starts out as the provisional winner. `JC` (below, unsigned) is the ONLY
        // skip path; equal falls through to the update.
        if (running_total >= best_total) { // 0x004d6d31/0x004d6d35
            best_index = unit_id;          // 0x004d6d3b
            best_total = running_total;    // 0x004d6d37
        }

        unit_id = u.ai_group_next; // 0x004d6d4b
    }
    return best_index;
}

} // namespace detail

// ---- the public wrappers ------------------------------------------------------------------------

uint32_t group_find_slowest_unit(int32_t player_id, int32_t group_index) {
    const ai_state st = state();
    return detail::group_find_slowest_unit(st.read, player_id, group_index);
}

uint32_t group_pick_best_weapon_unit(int32_t player_id, int32_t group_index) {
    const ai_state st = state();
    return detail::group_pick_best_weapon_unit(st.read, player_id, group_index);
}

// ---- the differential-oracle arms ----------------------------------------------------------------
//
// Both call nothing through ai_calls (neither shows a real CALL in the .asm besides the inlined,
// unmarshallable utils_math_trunc) and write nothing, so both arms simply re-run the same `detail::`
// body over the live state -- there is no REAL-callee/shadow-stub split to make here, unlike
// ai_turret_threat.cpp's site.

} // namespace mh::ai
