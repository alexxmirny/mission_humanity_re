//
// ai/ai_army_milestone.cpp -- see ai_army_milestone.h. Translated from the DISASSEMBLY
// (tmp/decomp_a2/llm_strat_ai_army_milestone_advance_or_attack_004e73b3.asm), not from Ghidra's C:
// the decompile still shows ai_build_plan[0x23]/[0x24] (the pre-split slots) and renders the float
// milestone-clock comparison as an int-to-float conversion that is not in the assembly -- both are
// stale artifacts, corrected here against the raw bytes. Every `JMP`/`JZ`/`JGE 0x004e792e` in the
// assembly is a shared Watcom epilogue tail-jump (`return`), exactly the same idiom documented in
// ai_turret_threat.cpp -- not a call.
//
#include "ai/ai_army_milestone.h"

// weapon_power_add_and_trunc: the x87 FILD/FADD/utils_math_trunc/FISTP accumulate that
// unit_squad_firepower_value (0x004d3486-0x004d34a3) and llm_strat_ai_group_pick_best_weapon_unit
// (0x004d6d0b-0x004d6d28) perform INSTRUCTION-FOR-INSTRUCTION identically. Shared rather than
// re-derived: a third private copy of this sequence (ai_mine_yield.cpp already has the second) is
// how two arms of one operation drift apart.
#include "ai/ai_group_muster_pick.h"

namespace mh::ai {
namespace detail {

void army_milestone_advance_or_attack(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                      uint32_t player) {
    // 0x004e73dc/0x004e73e3: CMP ai_attack_milestone_index, attack_milestone_count / JGE return.
    if (v.players[player].ai_attack_milestone_index >= *v.attack_milestone_count) return;

    // Bail if any of this player's existing groups already has an attack-in-progress goal (3 or
    // 0xb). Re-reads ai_group_count every iteration (0x004e7438), matching the assembly exactly --
    // there is no callee in this loop that could change it, but nothing here caches it either.
    // UNSIGNED bound (`CMP EAX,[ai_group_count] / JC`), like the active_player_count sweep below.
    // Inert -- group_create caps the count at 0x20 so the sign bit is unreachable -- but transcribed
    // as read rather than left as the one signed compare in the function.
    for (uint32_t g = 0; g < (uint32_t)v.players[player].ai_group_count; ++g) {
        const int16_t goal = v.players[player].ai_groups[g].goal;
        if (goal == 3 || goal == 0xb) return;
    }

    const int32_t milestone_index = v.players[player].ai_attack_milestone_index;

    // 0x004e7440..0x004e745c: FLD ai_clock / FSUB ai_attack_milestone_clock / FCOMP
    // attack_time_table[index] / FNSTSW / SAHF / JC return. All three FP operands are genuine
    // floats -- no int-to-float conversion anywhere here, unlike the stale decompile. FCOMP sets
    // C0 (-> CF via SAHF) when ST(0) < the memory operand, so JC fires (and returns) exactly when
    // (ai_clock - ai_attack_milestone_clock) < attack_time_table[index]; the gate to proceed is
    // therefore its negation, elapsed >= required, matching the original's "proceed once elapsed"
    // intent with no reversed comparison.
    const float elapsed =
        v.players[player].ai_clock - v.players[player].ai_attack_milestone_clock;
    if (elapsed < v.attack_time_table[milestone_index]) return;

    // Sum the firepower already assembled in ai_groups[2] then ai_groups[0], walking each group's
    // member list via unit::ai_group_next (0 = end). unit_squad_firepower_value is pure.
    uint32_t firepower = 0;
    for (uint16_t u = v.players[player].ai_groups[2].head_unit; u != 0;
         u          = unit_of(v, player, u).ai_group_next) {
        firepower += gc.unit_squad_firepower_value((int32_t)player, (int32_t)u);
    }
    for (uint16_t u = v.players[player].ai_groups[0].head_unit; u != 0;
         u          = unit_of(v, player, u).ai_group_next) {
        firepower += gc.unit_squad_firepower_value((int32_t)player, (int32_t)u);
    }

    if (firepower < (uint32_t)v.attack_strength_table[milestone_index]) {
        // Not strong enough yet (0x004e74fe JNC skip / 0x004e7500..0x004e750c): just advance the
        // milestone index and latch the clock, and wait for the next tick.
        own.players[player].ai_attack_milestone_index += 1;
        own.players[player].ai_attack_milestone_clock = own.players[player].ai_clock;
        return;
    }

    // Pick the weakest surviving HOSTILE opponent. 0x004e7528/0x004e7530: `CMP
    // ai_player_relation[i], -1 / JG skip-if-friendly` -- a SIGNED compare against -1, reproduced
    // here in its original form (not the stale decompile's unsigned-against-0x7fffffff rendering),
    // and matching the sibling hostility tests already in this codebase (ai_engage.cpp /
    // ai_target.cpp: `ai_player_relation[...] > -1` -> skip). Confirmed from the assembly
    // (0x004e753a..0x004e7555) that BOTH sides of the total_unit_power comparison index the SAME
    // ai_opponent_assessments array with the SAME 0x3c stride -- one by `i` (the candidate), one by
    // `weakest` (the current best) -- so the decompile's differing spelling (struct-field syntax for
    // one side, raw pointer arithmetic for the other) is a Ghidra rendering artifact, not a real
    // asymmetry: this is a plain "find the minimum total_unit_power" scan.
    int32_t weakest = -1;
    for (uint32_t i = 0; i < (uint32_t)*v.active_player_count; ++i) {
        if (i == player) continue;
        if (v.players[player].ai_player_relation[i] > -1) continue; // friendly/self -- skip
        // 0x004e7555/0x004e755c: CMP / JNC -- unsigned, matched here even though total_unit_power
        // is a signed field (it is a sum of individual power values and is not observed negative).
        if (weakest == -1 ||
            (uint32_t)v.players[player].ai_opponent_assessments[i].total_unit_power <
                (uint32_t)v.players[player].ai_opponent_assessments[weakest].total_unit_power) {
            weakest = (int32_t)i;
        }
    }

    if (weakest == -1) {
        // No hostile opponent found (0x004e7580 JNZ skip / 0x004e7586..0x004e7592): same
        // advance-and-wait as the not-strong-enough path.
        own.players[player].ai_attack_milestone_index += 1;
        own.players[player].ai_attack_milestone_clock = own.players[player].ai_clock;
        return;
    }

    uint32_t out_x = 0, out_y = 0;
    gc.pick_owned_tile_or_home((int32_t)player, &out_x, &out_y);

    const int32_t group_idx = gc.group_create((int32_t)player);
    // 0x004e75b4/0x004e75b7: CMP -1 / JZ return -- group_create() == -1 bails with NO milestone
    // advance, unlike every other exit in this function.
    if (group_idx == -1) return;

    // The two stores at 0x004e75c5 (word, 0xb) and 0x004e75d1 (DWORD, local_24) land at
    // task_queue_backlog-relative offsets -0x3b and -0x35; task_queue_backlog is at +0x4f in
    // mh_llm_strat_ai_unit_group, so -0x3b -> +0x14 (`goal`, an int16 -- matches the word-sized
    // store) and -0x35 -> +0x1a (`target_player_id`). Both named fields, not offsets.
    //
    // THE SECOND STORE IS 32 BITS WIDE and this draft wrote 16, which the reimpl-verify review
    // caught -- inside a region this site COMPARES, so the two bytes the original also stamps would
    // have shown up as a divergence whenever they were not already zero (and llm_strat_ai_group_create
    // does not zero them: it stops at +0x16). The fix is in the STRUCT, not here: all three writers
    // of +0x1a store a dword, and the old `_pad_0x1c` is a TOTAL-ORPHAN, so target_player_id is an
    // int32 and was retyped as one in Ghidra on 2026-08-01. Assigning the field now emits the same
    // 4-byte store the original makes, with no cast and no reinterpret_cast in sight.
    own.players[player].ai_groups[group_idx].goal             = 0xb;
    own.players[player].ai_groups[group_idx].target_player_id = weakest;

    // Assemble the attack force: pull members from ai_groups[2], falling back to ai_groups[0], until
    // the required strength is met or both are empty. THE HEAD-UNIT READ IS RE-FETCHED EVERY
    // ITERATION (0x004e75f4/0x004e760d/0x004e761b) on purpose -- group_member_move below mutates the
    // source group's member list, so hoisting this read would walk a stale head. Whichever way the
    // loop ends (strength met OR both groups empty), the task-enqueue and milestone-advance below run
    // unconditionally -- the assembly's two exits (0x004e7604 JNC / 0x004e7624 JZ) both fall through
    // to the same code, not to two different outcomes.
    firepower = 0;
    while (firepower < (uint32_t)v.attack_strength_table[milestone_index]) {
        int32_t  src_group = 2;
        uint16_t u         = v.players[player].ai_groups[2].head_unit;
        if (u == 0) {
            u         = v.players[player].ai_groups[0].head_unit;
            src_group = 0;
        }
        if (u == 0) break;

        firepower += gc.unit_squad_firepower_value((int32_t)player, (int32_t)u);
        gc.unit_launch_from_storage_enqueue((uint8_t)player, (int32_t)u, out_x, out_y);
        gc.group_member_move(player, src_group, group_idx, (int32_t)u);
    }

    gc.group_task_enqueue((int32_t)player, group_idx, 0x18, 899, 0, 0, 0, 0, 0);
    own.players[player].ai_attack_milestone_index += 1;
    own.players[player].ai_attack_milestone_clock = own.players[player].ai_clock;
}

// llm_strat_ai_unit_squad_firepower_value @0x004d3437 -- the summand of the two firepower loops
// above. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_ai_unit_squad_firepower_value_004d3437.asm); the header carries the two
// traps (break-vs-continue, and the unsigned dword type ladder).
uint32_t unit_squad_firepower_value(const ai_view &v, int32_t player, int32_t unit_idx) {
    const unit &u = unit_of(v, (uint32_t)player, unit_idx);

    // 0x004d344c-0x004d34aa. EBX is the running total, EDX the slot; `CMP EDX,4 / JC` is an
    // UNSIGNED bound over slots 0..3.
    uint32_t total = 0;
    for (int32_t slot = 0; slot < 4; ++slot) {
        const uint8_t weapon_id = u.weapons[slot].weapon_id;
        // 0x004d3465/0x004d346c: an EMPTY slot ENDS the scan (JZ -> 0x004d34ac, past the loop).
        // This is the branch that differs from pick_best_weapon_unit's -- see the header.
        if (weapon_id == 0) break;
        // 0x004d347b/0x004d3482: not anti-personnel -> next slot (JZ -> 0x004d34a6 = INC EDX).
        if ((v.cfg_weapons[weapon_id].target & 1) == 0) continue;
        // 0x004d3486-0x004d34a3: total = trunc((double)total + Weapon[id].power[player]), the
        // accumulator zero-extended to 64 bits before the FILD and truncated back to its low dword.
        // The `CALL utils_math_trunc` at 0x004d349b is INSIDE weapon_power_add_and_trunc, which
        // inlines it as a control-word RC=11 swap rather than calling it -- utils_math_trunc takes
        // its argument in ST0 and has no stack-passable signature (mh_calls.gen.h marks it
        // MH_UNAVAILABLE__parameter_storage_not_marshallable), so it cannot be reached through
        // mh::call at all.
        total = (uint32_t)weapon_power_add_and_trunc(total,
                                                     &v.cfg_weapons[weapon_id].power[player]);
    }

    // 0x004d34ac-0x004d34cc: Unit[unit_proto_id].type, read as a DWORD (the field is an enum; the
    // original loads all four bytes and compares unsigned).
    const int32_t  proto = (int32_t)u.unit_proto_id;
    const uint32_t type  = (uint32_t)v.cfg_units[proto].type;

    // 0x004d34d2-0x004d3520, transcribed from the jump targets. Every comparison is unsigned.
    if (type == UNIT_TYPE_A_INFANTRY_2 || type == UNIT_TYPE_H_INFANTRY_2) return total * 2; // 0x004d3509
    if (type == UNIT_TYPE_A_INFANTRY_3 || type == UNIT_TYPE_H_INFANTRY_3)
        return total + total * 2;                                                            // 0x004d350d -> 0x004d3510
    if (type == UNIT_TYPE_A_INFANTRY_4 || type == UNIT_TYPE_H_INFANTRY_4) return total << 2; // 0x004d3514
    if (type == UNIT_TYPE_A_INFANTRY_5 || type == UNIT_TYPE_H_INFANTRY_5)
        return total + (total << 2); // 0x004d3519 -> 0x004d3510
    // 0x004d34e5-0x004d34ed: the CONTIGUOUS heli band [A_HELI_MOTHER, H_HELI_CARGO] scores zero.
    // Written as a range because the original tests it as one (`CMP 0x13 / JC` then `CMP 0x18 /
    // JBE`), not as six equality tests.
    if (type >= UNIT_TYPE_A_HELI_MOTHER && type <= UNIT_TYPE_H_HELI_CARGO) return 0; // 0x004d3520
    return total;                                                                    // 0x004d3522
}

} // namespace detail

void army_milestone_advance_or_attack(uint32_t player) {
    const ai_state st = state();
    detail::army_milestone_advance_or_attack(st.read, st.own, live_calls(), player);
}

// THE PUBLIC LIVE WRAPPER, and it exists because the STANDALONE LINK needed it (LIB-REF,
// 2026-09-11). Until now this function had only a `detail::` body and a trace adapter, so
// gen_libmh_rebind's R1 walk found "no public sibling" and routed MH_REBIND_TARGET_llm_strat_ai_
// unit_squad_firepower_value at the ARM -- which lives inside `#ifndef MH_LIBMH_BUILD`. The hosted
// build never noticed, because MH_PROMOTED is `mh::call::<fn>` there and nothing odr-uses the
// target; the standalone build takes its address and the symbol is not in the artifact. Neither the
// VA census nor the object-byte scan can see that (a declared-never-defined symbol is not a VA):
// the FIRST LINK OF libmh.lib INTO AN EXECUTABLE is what found it. Adding the wrapper is also the
// R1-correct answer on its own terms -- the arm carries the oracle's trace line, which does not
// belong on a shipping path.
uint32_t unit_squad_firepower_value(int32_t player, int32_t unit_idx) {
    const ai_state st = state();
    return detail::unit_squad_firepower_value(st.read, player, unit_idx);
}


} // namespace mh::ai
