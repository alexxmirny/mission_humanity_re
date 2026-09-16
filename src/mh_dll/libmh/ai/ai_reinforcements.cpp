//
// ai/ai_reinforcements.cpp -- see ai_reinforcements.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_ai_score_reinforcement_unit_004d352c.asm,
// tmp/decomp/llm_strat_ai_create_reinforcement_unit_0046d8e6.asm and
// tmp/decomp/llm_strat_ai_invasion_spawn_reinforcements_004e8773.asm), not from Ghidra's .c: the
// draft for score_reinforcement_unit happens to agree (it already renders
// `Unit[unit_proto_id].weapons[uVar4].id` and the same infantry-tier ladder), which is itself the
// evidence for the DECLARED NEED below, but the other two are re-derived from the raw bytes,
// especially invasion_spawn_reinforcements' register-reused address arithmetic and the
// uninitialised-EDI trim loop.
//
#include "ai/ai_reinforcements.h"

// weapon_power_add_and_trunc: the x87 FILD/FADD/utils_math_trunc/FISTP accumulate that
// score_reinforcement_unit (0x004d3577-0x004d3596) performs INSTRUCTION-FOR-INSTRUCTION identically
// to ai_army_milestone.cpp's unit_squad_firepower_value and ai_group_muster_pick.cpp's
// group_pick_best_weapon_unit. Shared rather than re-derived a fourth time.
#include "ai/ai_group_muster_pick.h"

namespace mh::ai {
namespace detail {

uint32_t score_reinforcement_unit(const ai_view &v, int32_t player, int32_t unit_proto_id) {
    const cfg_unit &u = v.cfg_units[unit_proto_id];

    // DECLARED NEED: cfg_final_struct_Unit::weapons is still an untyped `uint8_t weapons[8]` in the
    // DLL struct manifest (mh_structs.gen.h), even though Ghidra's own decompile of THIS function
    // already renders `Unit[unit_proto_id].weapons[uVar4].id` -- i.e. Ghidra's type system already
    // knows the sub-struct, it was simply never added to tools/data/dll_addr_manifest.json. This is
    // the exact situation ai_group_muster_pick.h's "DECLARED NEED THIS FILE USED TO CARRY" note
    // describes for mh_map_object_unit::weapons before its 2026-08-06 fix (a DIFFERENT, larger,
    // 0x13-byte-per-slot record -- do not confuse the two). Until the manifest is fixed, index the
    // raw bytes with the confirmed 2-byte stride (weapon id at +0 of each slot; the second byte's
    // meaning is unread here and unnamed) rather than invent a name for it.
    //
    // 0x004d3547-0x004d3599: for slot in 0..3, id-then-flag-then-accumulate.
    uint32_t total = 0;
    for (int32_t slot = 0; slot < 4; ++slot) {
        const uint8_t weapon_id = u.weapons[slot * 2];
        // 0x004d355f: an EMPTY slot ENDS THE SCAN (JZ past the loop) -- break, not continue. Matches
        // unit_squad_firepower_value's break arm, not group_pick_best_weapon_unit's continue arm.
        if (weapon_id == 0) break;
        // 0x004d3575: not ground-capable -> next slot (JZ -> INC, continue).
        if ((v.cfg_weapons[weapon_id].target & 1) == 0) continue;
        // 0x004d3577-0x004d3596: total = trunc((double)total + Weapon[id].power[player]).
        total = (uint32_t)weapon_power_add_and_trunc(total, &v.cfg_weapons[weapon_id].power[player]);
    }

    // 0x004d35a1-0x004d3601, transcribed from the jump targets (every comparison unsigned, matching
    // unit_squad_firepower_value's own note): identical ladder, identical constants.
    const uint32_t type = u.type;
    if (type == UNIT_TYPE_A_INFANTRY_2 || type == UNIT_TYPE_H_INFANTRY_2) return total * 2;
    if (type == UNIT_TYPE_A_INFANTRY_3 || type == UNIT_TYPE_H_INFANTRY_3) return total + total * 2;
    if (type == UNIT_TYPE_A_INFANTRY_4 || type == UNIT_TYPE_H_INFANTRY_4) return total << 2;
    if (type == UNIT_TYPE_A_INFANTRY_5 || type == UNIT_TYPE_H_INFANTRY_5) return total + (total << 2);
    // 0x004d35c4-0x004d3601: six individual equality tests over a contiguous run
    // (0x13,0x14,0x17,0x18,0x15,0x16 in the order the asm tests them) -- exactly the same
    // [A_HELI_MOTHER, H_HELI_CARGO] band unit_squad_firepower_value tests as a range; written as one
    // here for the same reason that file gives.
    if (type >= UNIT_TYPE_A_HELI_MOTHER && type <= UNIT_TYPE_H_HELI_CARGO) return 0;
    return total;
}

void create_reinforcement_unit(const ai_view &v, const ai_calls &gc, uint32_t x, uint32_t y,
                               uint32_t unit_proto_id, uint16_t player) {
    // 0x0046d90e/0x0046d915: SIGNED `Unit[unit_proto_id].type > 0xe` (CMP/JG), not unsigned.
    if ((int32_t)v.cfg_units[unit_proto_id].type > 0xe) {
        // 0x0046d932-0x0046d946: EAX=x, EDX=y, EBX=(uint16_t)unit_proto_id, ECX=(uint16_t)player,
        // stack=1.
        gc.unit_create(x, y, (uint16_t)unit_proto_id, player, 1);
    } else {
        // 0x0046d917-0x0046d92b: same register assignment, EBX->a2, ECX->param_4.
        gc.unit_create_soldier(x, y, (uint16_t)unit_proto_id, player, 1);
    }
}

int32_t invasion_spawn_reinforcements(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                      int32_t player) {
    // 0x004e87a0-0x004e87a8: SIGNED gate.
    if (own.players[player].ai_invasion_points <= 0) return 0;

    // The candidate scan, 0x004e87b1-0x004e8886. Two PARALLEL stack arrays (proto id, score); NEITHER
    // is bounded against its own REINFORCEMENT_CANDIDATE_SCRATCH_CAP entries anywhere in the
    // original -- overflow is reachable in principle if more than 128 Progress rows pass the filter.
    // Reproduced unguarded; see uncertainties.
    int32_t proto_ids[REINFORCEMENT_CANDIDATE_SCRATCH_CAP];
    int32_t scores[REINFORCEMENT_CANDIDATE_SCRATCH_CAP];
    int32_t count = 0;

    // 0x004e887a: `CMP EBX, [cfg_progress_sec->total] / JBE` -- an INCLUSIVE bound.
    for (uint32_t i = 0; i <= (uint32_t)v.cfg_progress_sec->total; ++i) {
        if (v.cfg_inventions[i].type != INVENTION_TYPE_UNIT) continue;   // 0x004e87d3
        if (progress_of(v, player, (int32_t)i).available == 0) continue; // 0x004e87e8

        // Progress[i].index is reloaded three times in the original (0x004e87ee / 0x004e8808 /
        // 0x004e8851); it is read-only cfg data and nothing between the reads can change it, so
        // collapsed to one local read.
        const int32_t proto_id = v.cfg_inventions[i].index;

        // CALL #1 of 2 (0x004e87fb): the FILTER call. Its result decides whether the candidate is
        // even type-checked, let alone stored.
        if (gc.score_reinforcement_unit(player, proto_id) == 0) continue; // 0x004e8802

        // 0x004e881b-0x004e884f: the six-way heli exclusion, see the header banner.
        const uint32_t type = v.cfg_units[proto_id].type;
        if (type >= UNIT_TYPE_A_HELI_MOTHER && type <= UNIT_TYPE_H_HELI_CARGO) continue;

        proto_ids[count] = proto_id;
        // CALL #2 of 2 (0x004e886a): the STORE call. score_reinforcement_unit is a pure function of
        // (player, proto_id) with nothing carried between the two calls, so re-evaluating it here
        // rather than reusing CALL #1's result changes no OUTPUT -- but it is a genuine second call in
        // the original, and a recording-stub oracle over this function counts calls, so it is
        // reproduced literally rather than hoisted into one.
        scores[count] = (int32_t)gc.score_reinforcement_unit(player, proto_id); // 0x004e886a
        ++count;
    }

    if (count == 0) {
        // 0x004e888c-0x004e88bb: the no-candidates path.
        own.players[player].ai_invasion_points = 0;
        // 0x004e88b6: the original calls llm_debug_log_msg_stub("fucked return") here -- a PROVEN
        // no-op (stack probe + return), not reproduced since SIMABI-HOOKS; see ai_state.h.
        return 0;
    }

    // The trim loop, 0x004e88c0-0x004e890e: while (count > 4), find the LOWEST-scoring candidate and
    // swap-remove it (overwrite its slot with the current last element, then shrink count).
    //
    // THE UNINITIALISED MIN-INDEX -- the acknowledged R3 finding (ai_ready.py ack 2026-08-06). In the
    // original, `min_idx` (EDI) is assigned ONLY inside `if (min_val > scores[d])`; it is then used
    // UNCONDITIONALLY after the inner scan as the swap-remove destination. It is declared here OUTSIDE
    // the outer `while (count > ...)` loop and is NOT reset inside it, exactly mirroring the original,
    // where EDI simply carries whatever the PREVIOUS outer pass left it holding -- or, on the very
    // first pass, whatever the CALLER's EDI held: the prologue's `PUSH EDI` at 0x004e8784 shows this
    // function never initialises EDI itself before the trim loop. There is no caller-EDI value to
    // reproduce at this function's own entry point, so it is seeded 0 here (a translator's choice, not
    // a derivation -- see uncertainties). This is UB in the original and is preserved, not guarded:
    // "fixing" it into a proper running minimum would silently change which candidate gets evicted
    // whenever no scored[d] ever exceeds the 1,000,000 seed on some outer pass.
    int32_t min_idx = 0;
    while (count > REINFORCEMENT_CANDIDATE_TRIM_MAX) {
        int32_t min_val = REINFORCEMENT_TRIM_SEED; // reseeded every outer pass, 0x004e88c6
        for (int32_t d = 0; d < count; ++d) {
            // 0x004e88d4/0x004e88db: UNSIGNED `min_val <= scores[d]` skips the update -- both operands
            // are score_reinforcement_unit's unsigned return.
            if ((uint32_t)min_val > (uint32_t)scores[d]) {
                min_val = scores[d];
                min_idx = d;
            }
        }
        --count;
        proto_ids[min_idx] = proto_ids[count];
        scores[min_idx]    = scores[count];
    }

    // The spawn loop, 0x004e8910-0x004e8a17. `spiral_idx` (the original's ESI) is initialised ONCE,
    // before this whole loop, and is NEVER reset between spawn attempts: a failed passable test simply
    // advances it, and the NEXT attempt's spiral walk resumes exactly where the previous one's left
    // off -- across the whole function call, not per attempt. `candidate_idx` round-robins 0..count-1
    // across ACCEPTED spawns only (it is not touched by a failed passable test).
    int32_t  candidate_idx = 0;
    int32_t  spawned_count = 0;
    uint32_t spiral_idx    = 0;

    for (;;) {
        // 0x004e8934-0x004e8941: SIGNED gate. This is the ORDINARY exit -- no zeroing, unlike the two
        // special-case exits below.
        if (own.players[player].ai_invasion_points <= 0) return spawned_count;
        --own.players[player].ai_invasion_points;

        // The spiral walk: advance spiral_idx until a PASSABLE tile turns up. NO BOUND on spiral_idx
        // against spiral_offsets'/spiral_ring_cell_counts' extent anywhere in the original (unlike
        // every OTHER spiral-table consumer in this cluster, which stops at a ring radius) --
        // reproduced as-is; see uncertainties.
        uint32_t tile_x = 0, tile_y = 0;
        for (;;) {
            tile_x = ((uint32_t)(v.players[player].ai_home_tile_x + v.spiral_offsets[spiral_idx].dx)) &
                     *v.map_width_mask;
            tile_y = ((uint32_t)(v.players[player].ai_home_tile_y + v.spiral_offsets[spiral_idx].dy)) &
                     *v.map_height_mask;
            ++spiral_idx; // 0x004e8992 -- advances regardless of the test result below
            const uint8_t p = v.passable[(tile_x << 8) | tile_y];
            if (p == REINFORCEMENT_SPAWN_PASSABLE_A || p == REINFORCEMENT_SPAWN_PASSABLE_B) break;
        }

        gc.create_reinforcement_unit(tile_x, tile_y, (uint32_t)proto_ids[candidate_idx],
                                     (uint16_t)player);
        ++spawned_count;

        if (spawned_count == REINFORCEMENT_SPAWN_CAP) {
            // 0x004e89d5-0x004e89fa: the 100-cap exit -- zeroes ai_invasion_points, returns 100.
            own.players[player].ai_invasion_points = 0;
            return spawned_count;
        }

        // 0x004e89fc-0x004e8a12: candidate_idx round-robins 0..count-1.
        ++candidate_idx;
        if (candidate_idx >= count) candidate_idx = 0;
    }
}

} // namespace detail

uint32_t score_reinforcement_unit(int32_t player, int32_t unit_proto_id) {
    const ai_state st = state();
    return detail::score_reinforcement_unit(st.read, player, unit_proto_id);
}

void create_reinforcement_unit(uint32_t x, uint32_t y, uint32_t unit_proto_id, uint16_t player) {
    const ai_state st = state();
    detail::create_reinforcement_unit(st.read, live_calls(), x, y, unit_proto_id, player);
}

int32_t invasion_spawn_reinforcements(int32_t player) {
    const ai_state st = state();
    return detail::invasion_spawn_reinforcements(st.read, st.own, live_calls(), player);
}


} // namespace mh::ai
