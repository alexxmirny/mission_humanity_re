//
// ai/ai_attack_commit.cpp -- see ai_attack_commit.h. Translated from the DISASSEMBLY
// (tmp/decomp_ai/llm_strat_ai_commit_attack_order_004d55ef.asm,
// tmp/decomp_ai/llm_strat_ai_commit_attack_order_alt_004d5708.asm), not from the Ghidra `.c`.
//
// PROOF PATH: OFFLINE ONLY (see the header banner). No shadow wiring in this file.
//
#include "ai/ai_attack_commit.h"

namespace mh::ai {
namespace detail {

namespace {

// THE SHARED TAIL (0x004d56e0-0x004d5701) -- the ORIGINAL's own control flow, not a merge this
// translation invented: `commit_attack_order`'s building branch reaches it via `JMP 0x004d56e0`
// (0x004d569e) and its unit branch falls straight through into it; `commit_attack_order_alt` has no
// tail of its own and JMPs into THIS SAME address from both of its branches (0x004d57b7, 0x004d57fc).
// See the header banner's "THE SHARED TAIL" section for the full citation.
//
// `MOV EBX,[EBP-0x18] / AND EBX,0xf` (0x004d56e0-0x004d56e3) re-derives `ref_owner(attacker_ref)`
// (the same quantity every other use site in this file derives the same way), then
// `INC dword ptr [player_data_row + 0xe6deec]` (0x004d56fa) bumps that player's
// `ai_attack_orders_issued` -- a pure statistic (mh_structs.gen.h's own field comment: an image-wide
// scan for this field's address finds only INC sites and no reader).
void bump_attack_orders_issued(const ai_store &own, uint32_t attacker_ref) {
    player_data &pd = own.players[ref_owner(attacker_ref)];
    ++pd.ai_attack_orders_issued;
}

} // namespace

void commit_attack_order(const ai_view &v, const ai_store &own, const ai_calls &gc,
                         uint32_t attacker_ref, int32_t attacker_unit_index, uint32_t target_ref,
                         int32_t target_index) {
    (void)v;
    const uint32_t attacker_player = ref_owner(attacker_ref); // 0x004d5605: AND EAX,0xf
    const uint32_t target_owner    = ref_owner(target_ref);   // 0x004d5635-0x004d563a: AND EDX,0xf

    // 0x004d5617: OR word ptr [attacker_unit+0xdd8d2a],0x8001 -- the merged engagement/order-status
    // word; own.roster.engage_commit_set() is the ONLY sanctioned expression (ai_state.h banner).
    own.roster.engage_commit_set(attacker_player, attacker_unit_index);

    if (ref_is_building_by_40(target_ref)) {
        // ---- BUILDING branch (0x004d5640 JZ taken) ----
        // 0x004d5645: all four registers are LIVE-CARRIED here, not reloaded -- see the header
        // banner's register-provenance note.
        const uint32_t est = gc.estimate_weapon_damage(static_cast<int32_t>(attacker_player),
                                                       attacker_unit_index, target_ref, target_index);
        // 0x004d564a: MOV word ptr [attacker_unit+0xdd8d26],AX -- store, truncating to int16_t.
        own.roster.committed_weapon_damage_est(attacker_player, attacker_unit_index) =
            static_cast<int16_t>(est);
        // 0x004d567c: MOV DX,word ptr [attacker_unit+0xdd8d26] -- READ BACK the truncated value
        // (not `est` directly).
        const int16_t est16 =
            own.roster.committed_weapon_damage_est(attacker_player, attacker_unit_index);
        // 0x004d5683: ADD word ptr [target_building+0xc3d3af],DX -- 16-bit accrual.
        own.roster.bldg_incoming_damage_tally(target_owner, target_index) = static_cast<int16_t>(
            own.roster.bldg_incoming_damage_tally(target_owner, target_index) + est16);

        // 0x004d5699: llm_strat_unit_order_attack_building_reposition_alt_enqueue(attacker_player,
        // attacker_unit_index, target_owner, target_index, 4). The trailing 4 is the literal pushed
        // at 0x004d568a -- no other call site in the image reaches this callee to cross-check its
        // meaning.
        gc.order_attack_building_reposition_alt_enqueue(attacker_player, attacker_unit_index,
                                                        target_owner, target_index, 4u);
    } else {
        // ---- UNIT branch (0x004d5640 JZ not taken) ----
        const uint32_t est = gc.estimate_weapon_damage(static_cast<int32_t>(attacker_player),
                                                       attacker_unit_index, target_ref, target_index);
        // 0x004d56a8: store, truncating to int16_t.
        own.roster.committed_weapon_damage_est(attacker_player, attacker_unit_index) =
            static_cast<int16_t>(est);
        // 0x004d56be: read back the truncated value.
        const int16_t est16 =
            own.roster.committed_weapon_damage_est(attacker_player, attacker_unit_index);
        // 0x004d56c5: ADD word ptr [target_unit+0xdd8d24],BX -- 16-bit accrual onto the TARGET unit.
        own.roster.incoming_threat_damage(target_owner, target_index) = static_cast<int16_t>(
            own.roster.incoming_threat_damage(target_owner, target_index) + est16);

        // 0x004d56db: llm_strat_unit_order_attack_target_enqueue(attacker_player,
        // attacker_unit_index, target_owner, target_index, 4). Same trailing literal as the building
        // branch (0x004d56cc).
        gc.order_attack_target_enqueue(attacker_player, attacker_unit_index, target_owner,
                                       target_index, 4u);
    }

    bump_attack_orders_issued(own, attacker_ref);
}

void commit_attack_order_alt(const ai_view &v, const ai_store &own, const ai_calls &gc,
                             uint32_t attacker_ref, int32_t attacker_unit_index,
                             uint32_t target_ref, int32_t target_index) {
    (void)v;
    const uint32_t attacker_player = ref_owner(attacker_ref); // 0x004d571e
    const uint32_t target_owner    = ref_owner(target_ref);   // 0x004d574e-0x004d5753

    // 0x004d5730: same merged engagement/order-status word OR as the primary.
    own.roster.engage_commit_set(attacker_player, attacker_unit_index);

    if (ref_is_building_by_40(target_ref)) {
        // ---- BUILDING branch (0x004d5759 JZ taken) -- identical to the primary's, including the
        // enqueue callee (order_attack_building_reposition_alt_enqueue, same as commit_attack_order
        // -- this branch is NOT where the two functions differ). ----
        const uint32_t est = gc.estimate_weapon_damage(static_cast<int32_t>(attacker_player),
                                                       attacker_unit_index, target_ref, target_index);
        // 0x004d5763: store, truncating to int16_t.
        own.roster.committed_weapon_damage_est(attacker_player, attacker_unit_index) =
            static_cast<int16_t>(est);
        // 0x004d5795: read back the truncated value.
        const int16_t est16 =
            own.roster.committed_weapon_damage_est(attacker_player, attacker_unit_index);
        // 0x004d579c: 16-bit accrual onto the TARGET building.
        own.roster.bldg_incoming_damage_tally(target_owner, target_index) = static_cast<int16_t>(
            own.roster.bldg_incoming_damage_tally(target_owner, target_index) + est16);

        // 0x004d57b2: same callee and argument shape as the primary's building branch; trailing
        // literal 4 pushed at 0x004d57a3.
        gc.order_attack_building_reposition_alt_enqueue(attacker_player, attacker_unit_index,
                                                        target_owner, target_index, 4u);
    } else {
        // ---- UNIT branch (0x004d5759 JZ not taken) -- THE ONE PLACE the two functions differ:
        // order_attack_target_ALT_enqueue instead of order_attack_target_enqueue. ----
        const uint32_t est = gc.estimate_weapon_damage(static_cast<int32_t>(attacker_player),
                                                       attacker_unit_index, target_ref, target_index);
        // 0x004d57c4: store, truncating to int16_t.
        own.roster.committed_weapon_damage_est(attacker_player, attacker_unit_index) =
            static_cast<int16_t>(est);
        // 0x004d57da: read back the truncated value.
        const int16_t est16 =
            own.roster.committed_weapon_damage_est(attacker_player, attacker_unit_index);
        // 0x004d57e1: 16-bit accrual onto the TARGET unit.
        own.roster.incoming_threat_damage(target_owner, target_index) = static_cast<int16_t>(
            own.roster.incoming_threat_damage(target_owner, target_index) + est16);

        // 0x004d57f7: llm_strat_unit_order_attack_target_ALT_enqueue(attacker_player,
        // attacker_unit_index, target_owner, target_index, 4). Same trailing literal as every other
        // enqueue site in this file (0x004d57e8).
        gc.order_attack_target_alt_enqueue(attacker_player, attacker_unit_index, target_owner,
                                           target_index, 4u);
    }

    bump_attack_orders_issued(own, attacker_ref);
}

} // namespace detail

// ---- the public wrappers ---------------------------------------------------------------------------
//
// Both bound to live_calls() only -- OFFLINE proof path, no shadow_calls() arm (see the header
// banner for why: a shadow arm here would double-fire an order enqueue).

void commit_attack_order(uint32_t attacker_ref, int32_t attacker_unit_index, uint32_t target_ref,
                         int32_t target_index) {
    const ai_state st = state();
    detail::commit_attack_order(st.read, st.own, live_calls(), attacker_ref, attacker_unit_index,
                                target_ref, target_index);
}

void commit_attack_order_alt(uint32_t attacker_ref, int32_t attacker_unit_index,
                             uint32_t target_ref, int32_t target_index) {
    const ai_state st = state();
    detail::commit_attack_order_alt(st.read, st.own, live_calls(), attacker_ref,
                                    attacker_unit_index, target_ref, target_index);
}

} // namespace mh::ai
