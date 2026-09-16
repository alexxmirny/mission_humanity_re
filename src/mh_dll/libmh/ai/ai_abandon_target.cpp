//
// ai/ai_abandon_target.cpp -- see ai_abandon_target.h. Translated from the DISASSEMBLY
// (tmp/decomp_a2/llm_strat_ai_unit_should_abandon_target_004ec84d.asm), not from Ghidra's C: the
// decompile's `extraout_ECX` / `extraout_ECX_00` are the Watcom "value already sitting in the
// register from the previous compare" idiom -- both trace to the SAME live ECX (a per-iteration
// "found one more in-range candidate" increment computed once before the unit/building branch
// splits, then conditionally committed to local_28 in each branch), not two different values, and
// the decompile's `CONCAT31(extraout_var,bVar3)` is just the full EAX return of
// llm_strat_ai_target_ref_has_engageable_weapon tested for != 0.
//
#include "ai/ai_abandon_target.h"


namespace mh::ai {
namespace detail {

bool unit_should_abandon_target(const ai_view &v, const ai_calls &gc, uint32_t player,
                                int32_t unit_index) {
    // Short-circuit: a target that is already dead is always abandoned, before anything else is
    // read. 0x004ec866/0x004ec86d.
    if (gc.unit_attack_target_is_dead((int32_t)player, unit_index) != 0) return true;

    // Own max weapon range, squared once and reused for every distance test below. 0x004ec882.
    // `player` is passed PLAIN here (EAX = EDI = player, no `| 0x80` packed-ref bit) -- unlike
    // every other call in this function, which passes an attacker ref. Same naming trap
    // ai_engage_scan.cpp notes for this same callee: the ai_calls parameter is called `unit_ref`
    // but this call site hands it a bare 0..7 player index.
    const uint32_t own_range = gc.unit_max_weapon_range(player, unit_index);
    const uint32_t range_sq  = own_range * own_range;

    const player_data &pd = v.players[player];
    const unit        &u  = unit_of(v, player, unit_index);

    // The unit's PRIMARY committed target, read once and zero-extended exactly as the assembly's
    // MOVZX does (both fields are int16_t; a plain widening would sign-extend a set high bit).
    const uint32_t own_target_index = (uint32_t)(uint16_t)u.target_index;
    const uint32_t own_target_ref   = (uint32_t)(uint16_t)u.target_ref;

    int32_t  in_range_candidate_found = 0; // local_28: incremented per qualifying in-range hit
    uint32_t candidate_flags          = 0; // local_20: bit 0x1 = unit candidate seen, 0x4 = turret

    for (uint32_t i = 0; i < (uint32_t)pd.ai_target_list_count; ++i) {
        const target_entry &e = pd.ai_target_list[i];

        // 0x004ec8a7-0x004ec8c8: this list entry IS the unit's own committed primary target ->
        // still tracked, still wanted -- return false (keep) immediately, before the rest of the
        // list (or the post-loop decision) is even looked at.
        if (own_target_index == (uint32_t)e.aggressor_index && own_target_ref == e.aggressor_ref) return false;

        // 0x004ec8ea-0x004ec900: this entry must have been recorded for THIS unit and be flagged
        // live (victim_ref tested as a byte, but AND 0xa0 over the full dword is identical).
        if (e.victim_index != unit_index || (e.victim_ref & 0xa0u) == 0) continue;

        // 0x004ec906-0x004ec920: can this unit actually shoot at the tracked target? Register
        // order read off the call site, NOT the (misleading) committed parameter names -- EAX =
        // attacker ref (player | 0x80), EDX = attacker index (unit_index), EBX = the TARGET's
        // packed ref (e.aggressor_ref).
        if (gc.target_ref_has_engageable_weapon((int32_t)(player | 0x80u), unit_index,
                                                e.aggressor_ref) == 0)
            continue;

        if (ref_is_building_by_a0(e.aggressor_ref)) {
            // 0x004ec99a-0x004eca6e: candidate is a BUILDING.
            const building &tb   = building_of(v, ref_owner(e.aggressor_ref), e.aggressor_index);
            const uint32_t  dist = gc.toroidal_dist_sq(tb.x, tb.y, u.x, u.y);
            if (dist <= range_sq) ++in_range_candidate_found;

            // The turret-type check runs UNCONDITIONALLY here (even when the building above was
            // just found out of range) -- there is no early-out between the dist check and this
            // one in the assembly.
            const cfg_building &cb = v.cfg_buildings[tb.building_id];
            if (cb.type == BLDG_TYPE_H_TURRET || cb.type == BLDG_TYPE_A_TURRET)
                candidate_flags |= 0x4u;
        } else {
            // 0x004ec942-0x004ec995: candidate is a UNIT. No turret-type check on this branch --
            // the original JMPs straight to the loop increment, skipping the block above entirely.
            candidate_flags |= 0x1u;
            const unit    &tu   = unit_of(v, ref_owner(e.aggressor_ref), e.aggressor_index);
            const uint32_t dist = gc.toroidal_dist_sq(tu.x, tu.y, u.x, u.y);
            if (dist <= range_sq) ++in_range_candidate_found;
        }
    }

    // 0x004ecaa9-0x004ecaad: nothing in the whole list qualified -> keep the current target.
    if (candidate_flags == 0) return false;

    // 0x004ecac1-0x004ecad6: distance from this unit to its OWN current primary target (not a
    // list entry) -- same ref_a/idx_a/ref_b/idx_b register order as target_ref_has_engageable_weapon
    // established above, this time against the callee's real 4-parameter signature.
    const uint32_t dist_to_current =
        gc.target_dist_sq(player | 0x80u, unit_index, own_target_ref, (int32_t)own_target_index);

    // 0x004ecae7-0x004ecb57: classify the CURRENT target itself -- 0 = unit, 1 = non-turret
    // building, 2 = turret building. Uses ref_is_building_by_a0 again, same polarity as the scan
    // above (not the 0x40 polarity other AI files in this batch use).
    int32_t current_kind;
    if (ref_is_building_by_a0(own_target_ref)) {
        const building     &cb  = building_of(v, ref_owner(own_target_ref), (int32_t)own_target_index);
        const cfg_building &ccb = v.cfg_buildings[cb.building_id];
        current_kind            = (ccb.type == BLDG_TYPE_H_TURRET || ccb.type == BLDG_TYPE_A_TURRET) ? 2 : 1;
    } else {
        current_kind = 0;
    }

    // 0x004ecb59-0x004ecb87, reassembled from the original's sequential-branch form into the
    // equivalent boolean the .c draft already had algebraically right: abandon iff the current
    // target is now out of range, OR it is a non-turret building, OR it is a turret building that
    // the scan above did not see among its engageable candidates while a unit candidate (in range)
    // was seen.
    if (range_sq < dist_to_current ||
        (current_kind != 0 &&
         (current_kind < 2 ||
          ((candidate_flags & 0x4u) == 0 && (candidate_flags & 0x1u) != 0 &&
           in_range_candidate_found != 0))))
        return true;

    return false;
}

} // namespace detail

bool unit_should_abandon_target(uint32_t player, int32_t unit_index) {
    const ai_state st = state();
    return detail::unit_should_abandon_target(st.read, live_calls(), player, unit_index);
}


} // namespace mh::ai
