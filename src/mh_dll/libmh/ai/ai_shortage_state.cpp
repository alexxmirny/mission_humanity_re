//
// ai/ai_shortage_state.cpp -- see ai_shortage_state.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_ai_recompute_shortage_state_004e853f.asm), not from Ghidra's C: the
// decompile's raw `player_idx * 0x28938 + 0x286f8/0x286e0/0x286d8` self-slot arithmetic is real
// (verified against the .asm, see the header comment) but is folded here into the ordinary
// player_data[player_idx].ai_opponent_assessments[player_idx] field access instead of being
// reproduced as byte offsets -- the struct is already typed and the offsets check out exactly.
//
#include "ai/ai_shortage_state.h"


namespace mh::ai {
namespace detail {

void recompute_shortage_state(const ai_view &v, const ai_store &own, const ai_calls &gc,
                              uint32_t player_idx) {
    // `me` is player_idx's own row, held for the whole call the same way
    // ai_turret_threat_rescan holds `own_threat_grid` -- pure pointer arithmetic on a parameter
    // that is constant for this call, not a cached read (nothing here dereferences through a
    // hoisted VALUE; every access below re-derefs `me` at its own use point).
    player_data &me = own.players[player_idx];

    // Refresh the ticking player's own need score first (0x004e8556-0x004e8573).
    me.ai_resource_need_score = gc.player_score_tier((int32_t)player_idx);

    // Drain-and-test the per-opponent pending-event flag array, skipping player_idx's own slot
    // entirely (neither tested nor drained -- the original JZs straight past both on p ==
    // player_idx, 0x004e8580-0x004e8582). `pending` is STICKY and short-circuits the flag TEST
    // once true (0x004e8584-0x004e8586: the running bool is checked before the flag is even
    // loaded), but every other slot is still unconditionally drained to 0 regardless
    // (0x004e85cd) -- do not skip the drain once `pending` is already true.
    bool pending = false;
    for (uint32_t p = 0; p < (uint32_t)MAX_PLAYERS; ++p) {
        if (p == player_idx) continue;
        pending              = pending || (me.ai_intel_flags[p] != 0);
        me.ai_intel_flags[p] = 0;
    }
    if (pending) {
        if (me.ai_resource_shortage_state != 3) me.ai_resource_shortage_state = 3;
        return;
    }

    // Lazily refresh every currently-non-AI (ai_enabled == 0) ACTIVE player's own score cache --
    // NOT limited to player_idx, and not gated on player_idx at all (0x004e85f5-0x004e862f).
    for (uint32_t p = 0; p < (uint32_t)*v.active_player_count; ++p) {
        if (v.players[p].ai_enabled == 0) {
            gc.score_build_categories((int32_t)p);
            own.players[p].ai_resource_need_score = gc.player_score_tier((int32_t)p);
        }
    }

    // Find the strongest-assessed opponent (by total_unit_power) among every OTHER active
    // player, via player_idx's OWN ai_opponent_assessments row (0x004e8631-0x004e868a). Strict
    // `<` (the original's JBE-to-skip on candidate <= best), so a tie keeps the earlier index.
    uint32_t best = 0xffffffffu;
    for (uint32_t p = 0; p < (uint32_t)*v.active_player_count; ++p) {
        if (p == player_idx) continue;
        if (best == 0xffffffffu ||
            me.ai_opponent_assessments[best].total_unit_power <
                me.ai_opponent_assessments[p].total_unit_power) {
            best = p;
        }
    }
    if (best == 0xffffffffu) return; // 0x004e868a-0x004e868d

    // The two exits below (state 2 / state 1) were `goto`-to-shared-store in the original
    // (0x004e8709 / 0x004e8733 both jump to the store at 0x004e8738); reassembled here as a
    // structured if/else-if that provably reaches the same three outcomes -- when the
    // building_count/total_unit_power gate below is false, both branches are skipped and
    // new_state stays 0, exactly as the original's fallthrough to `iVar2 = 0` at 0x004e8733's
    // failing path.
    int32_t     new_state = 0;
    const auto &enemy     = me.ai_opponent_assessments[best];
    if (enemy.building_count != 0 || enemy.total_unit_power != 0) { // 0x004e8693-0x004e86a5
        // The self-slot read -- see the header comment. `own_assessment` is player_idx's own
        // cached economy/military strength, indexed by player_idx itself.
        const auto &own_assessment = me.ai_opponent_assessments[player_idx];

        // enemy.total_unit_power * nAttackOverKill <= own_assessment.total_unit_power * 100, AND
        // nMinAttackStr <= own_assessment.ground_count + own_assessment.soldier_count
        // (0x004e86ac-0x004e8709). Both multiplies are IMUL's 2-operand low-32-bit-truncating
        // form, bit-identical between signed and unsigned -- done here in uint32_t to match the
        // decompile's explicit `(uint)` casts without invoking signed-overflow UB.
        const uint32_t enemy_scaled =
            (uint32_t)enemy.total_unit_power * (uint32_t)*v.attack_overkill_pct;
        const uint32_t own_scaled = (uint32_t)own_assessment.total_unit_power * 100u;
        const uint32_t own_ground_and_soldiers =
            (uint32_t)own_assessment.ground_count + (uint32_t)own_assessment.soldier_count;

        if (enemy_scaled <= own_scaled && (uint32_t)*v.min_attack_str <= own_ground_and_soldiers) {
            new_state = 2;
        } else if ((uint32_t)me.ai_resource_need_threshold <=
                   (uint32_t)me.ai_resource_need_score) { // 0x004e870b-0x004e8733
            new_state = 1;
        }
    }

    // The Ghidra "switch with 1 destination" at 0x004e876b is a dead dispatch: state values 0-4
    // all reach this same store instruction. Reproduced as a plain conditional store, not a
    // switch (0x004e874e-0x004e875a).
    if (new_state != me.ai_resource_shortage_state) me.ai_resource_shortage_state = new_state;
}

} // namespace detail

void recompute_shortage_state(uint32_t player_idx) {
    const ai_state st = state();
    detail::recompute_shortage_state(st.read, st.own, live_calls(), player_idx);
}


} // namespace mh::ai
