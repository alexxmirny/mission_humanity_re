#include "ai/ai_hq_attack_commit.h"

namespace mh::ai {
namespace detail {

void unit_commit_attack_on_enemy_hq(const ai_view &v, const ai_store &own, const ai_calls &gc) {
    // _G_LLM_STRAT_AI_ATTACK_HQ_UNIT_ID has no ai_view counterpart (nothing else in this cluster
    // reads it), so it comes through ai_store even though this function only reads it -- see the
    // field comment on ai_store::ai_attack_hq_unit_id.
    //
    // The assembly re-loads this global via a fresh IMUL/MOV at every one of its five uses
    // (0x004ba714, 0x004ba728, 0x004ba73e, 0x004ba754, 0x004ba76a) rather than reusing one register
    // across the chain -- but NOTHING between those five loads is a call (only CMP/JMP), so nothing
    // could have written it in between, and a single read serving all five compares is behaviourally
    // identical. The one place a fresh reload is NOT a no-op is after the unit_select_weapon call
    // below (a real call, and per the const-view rule a read is not stable across one) -- that reload
    // is kept explicit.
    const uint32_t id = *own.ai_attack_hq_unit_id;

    // Early-out chain (0x004ba714-0x004ba77e): five state compares against units[1][id].state,
    // written as a cascade of forwarding jumps to two shared landing pads that is easy to misread.
    // Traced instruction-by-instruction: EVERY one of the five compares, on a match, falls into the
    // SAME forwarding-jump chain that lands on the epilogue (0x004ba7de) -- i.e. a match on any of
    // the five is an immediate return, and the function proceeds only when the state matches NONE of
    // them. (Ghidra's own decompile renders the five as CORPSE_FOW_DECAY / GROUP_MARSHAL /
    // MOVE_WALKER / ATTACK_BUILDING / ATTACK_UNIT; those names are not backed by any enum in this
    // tree, so the literals are used directly with the names kept as documentation only.)
    const uint16_t state = unit_of(v, 1, id).state;
    if (state == 4 ||    // CORPSE_FOW_DECAY (Ghidra decompile name, undocumented enum)
        state == 0xa ||  // GROUP_MARSHAL
        state == 0xf ||  // MOVE_WALKER
        state == 0x1c || // ATTACK_BUILDING
        state == 0x1a)   // ATTACK_UNIT
        return;

    // EAX=player=1, EDX=id, EBX=mask=1 (0x004ba780-0x004ba790). Return is a plain byte, zero-extended
    // before the dword compare against 0x64 that follows.
    const uint8_t weapon = gc.unit_select_weapon(1, id, 1);

    // Reload after the call: the original re-reads _G_LLM_STRAT_AI_ATTACK_HQ_UNIT_ID via a fresh MOV
    // on BOTH branches below (0x004ba7ac / 0x004ba7ce) rather than reusing the pre-call value in
    // `id`. Both branches' reload happens with nothing but the CMP/JZ in between, so hoisting the
    // reload to one place ahead of the branch is behaviourally identical to doing it twice.
    const uint32_t id_after_select = *own.ai_attack_hq_unit_id;

    if (weapon == 0x64) {
        // MOVE branch (0x004ba7be-0x004ba7d9): target is the hardcoded human HQ, buildings[0][1] --
        // a literal player/index pair, not derived from anything. x/y are zero-extended BYTE tile
        // coords (MOVZX); the one stack argument is a literal 0.
        const building &hq = building_of(v, 0, 1);
        gc.unit_order_move(1, id_after_select, hq.x, hq.y, 0);
    } else {
        // ATTACK branch (0x004ba7a1-0x004ba7bc): target_ref=0 (XOR EBX,EBX), target_index=1 (literal
        // MOV ECX,1), weapon_id = the zero-extended byte unit_select_weapon returned.
        gc.unit_order_attack_building_reposition(1, id_after_select, 0, 1, weapon);
    }
}

} // namespace detail

void unit_commit_attack_on_enemy_hq() {
    const ai_state st = state();
    detail::unit_commit_attack_on_enemy_hq(st.read, st.own, live_calls());
}

} // namespace mh::ai
