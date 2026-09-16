//
// ai/ai_group_redistribute.cpp -- see ai_group_redistribute.h. Translated from the DISASSEMBLY
// (tmp/decomp_ai/llm_strat_ai_group_redistribute_units_004e6a89.asm).
//
#include "ai/ai_group_redistribute.h"


namespace mh::ai {
namespace detail {

redistribute_report group_redistribute_units(const ai_view &v, const ai_store &own,
                                             const ai_calls &gc, uint32_t player, int32_t group_index,
                                             int32_t split_off_member_count, int32_t split_off_centroid_x,
                                             int32_t split_off_centroid_y) {
    redistribute_report rep{};
    player_data        &pd  = own.players[player];
    unit_group         &src = pd.ai_groups[group_index];

    // Guard 1 (0x004e6abf/0x004e6ac7).
    if (src.reinforce_pending == 0) {
        rep.no_reinforce_pending = true;
        return rep;
    }
    // Guard 2 (0x004e6acd/0x004e6ad5).
    if (src.goal == 0xb) {
        rep.already_attacking = true;
        return rep;
    }

    // The goal-derived (kind, pool_group_index) decision table (0x004e6adb-0x004e6b1f), transcribed
    // as an if/else chain over the same three outcomes the original's CMP/JC/JBE ladder produces --
    // see the file header for the full case-by-case correspondence.
    int32_t kind, pool_group_index;
    if (src.goal == 7 || src.goal == 10) {
        kind             = 1;
        pool_group_index = 3;
    } else if (src.goal == 8) {
        kind             = 2;
        pool_group_index = 4;
    } else {
        kind             = 0;
        pool_group_index = 2;
    }
    rep.kind             = kind;
    rep.pool_group_index = pool_group_index;

    // THE HAZARD (0x004e6b26-0x004e6b3a). See the file header: all three arguments here are
    // documented stand-ins for uninitialised original stack slots, not real values -- the CALLER
    // (both the live wrapper and the shadow arm below) is what supplies the named constant 0.
    if (kind == 0) {
        gc.group_split_off_create(player, split_off_centroid_x, split_off_centroid_y, group_index,
                                  (uint32_t)split_off_member_count);
        rep.kind0_split_off_reached = true;
    }

    unit_group &pool = pd.ai_groups[pool_group_index];

    // 0x004e6b6b-0x004e6b79: pool.member_count vs src.reinforce_pending, UNSIGNED 16-bit compare
    // (the original reads both without sign extension before the CMP/JC).
    if ((uint16_t)pool.member_count < src.reinforce_pending) {
        // ---- the excess-split path (0x004e6cc4) ----
        rep.excess_split_path = true;
        // 0x004e6cc4/0x004e6cd2: JBE returns when reinforce_pending <= member_count; only the
        // strictly-greater case continues.
        if (src.reinforce_pending > (uint16_t)src.member_count) {
            // 0x004e6cd8-0x004e6cee: goal == 3 or 0xb also returns with no call.
            if (src.goal != 3 && src.goal != 0xb) {
                gc.group_split_excess_members(player, group_index);
                rep.excess_split_called = true;
            }
        }
        return rep;
    }

    // ---- the merge path (0x004e6b7f onward) ----
    rep.merge_path = true;

    uint32_t centroid_x = 0, centroid_y = 0; // committed llm_strat_ai_group_compute_centroid out-params
    gc.group_compute_centroid((int32_t)player, group_index, &centroid_x, &centroid_y);

    // transfer_cap = min(src.reinforce_pending * 2, pool.member_count) -- 0x004e6b8f-0x004e6ba8, a
    // full 32-bit local throughout (the original's [EBP-0x14]).
    int32_t        transfer_cap = (int32_t)src.reinforce_pending * 2;
    const uint32_t pool_members = (uint16_t)pool.member_count;
    if (pool_members < (uint32_t)transfer_cap) transfer_cap = (int32_t)pool_members;

    // 0x004e6bcf: pool.reinforce_pending += transfer_cap, a 16-bit ADD (defined wraparound, matching
    // the original exactly). This is the write the field comment on
    // mh_llm_strat_ai_unit_group::reinforce_pending already documents at this address.
    pool.reinforce_pending = (uint16_t)(pool.reinforce_pending + (uint16_t)transfer_cap);

    // The loop counter's ORIGINAL starting point (0x004e6bd6) -- pool.head_unit read ONCE, before any
    // member has moved.
    uint32_t u = pool.head_unit;
    while (u != 0 && transfer_cap != 0) {
        // `next` is captured from the WALKED unit's own ai_group_next (0x004e6bf0) -- the loop's
        // shape follows the list as it stood at loop entry, even though the actual move target below
        // is re-read fresh off pool.head_unit every time. Transcribed, not tidied: see the file
        // header.
        const unit    &walked = unit_of(v, player, (int32_t)u);
        const uint32_t next   = walked.ai_group_next;

        // The 4 unit-type codes eligible for this move (cfg_enum_ai_E_UNIT values, read straight off
        // the four CMPs at 0x004e6c0d-0x004e6c2f). SOLDIER == 1 and FIGHTER == 8 are confirmed via
        // ReVA elsewhere in this cluster (ai_group_home_guard.cpp); the exported `.c` draft further
        // guesses these four as SOLDIER/TANK/JEEP/WALKER (1/4/2/5), but that mapping for 2, 4 and 5
        // specifically is NOT independently confirmed here, so the literals are left unnamed rather
        // than asserting unverified role names.
        const cfg_unit &cu      = v.cfg_units[walked.unit_proto_id];
        const bool      matched = (cu.ai_unit == 1) || (cu.ai_unit == 4) || (cu.ai_unit == 2) ||
                             (cu.ai_unit == 5);

        // The observable predicate from the two same-local comparisons at 0x004e6c31-0x004e6c3b:
        // skip the move ONLY when neither condition holds.
        if (matched || kind != 0) {
            // Both calls take pool.head_unit RE-READ at the moment of the call (0x004e6c5c,
            // 0x004e6c70) -- two separate loads in the original, kept as two separate reads here even
            // though nothing between them can change the field (unit_flag_and_move writes `units`,
            // not the group's head_unit).
            gc.unit_flag_and_move(player, (int32_t)pool.head_unit, centroid_x,
                                  centroid_y);
            gc.group_member_move(player, /*src_group=*/pool_group_index, /*dst_group=*/group_index,
                                 (int32_t)pool.head_unit);
            --transfer_cap;
            ++rep.units_moved;
        }
        u = next;
    }
    rep.transfer_cap = transfer_cap;

    // 0x004e6cbf: src.reinforce_pending = 0, unconditionally, once the loop above exits.
    src.reinforce_pending = 0;
    return rep;
}

} // namespace detail

void group_redistribute_units(uint32_t player, int32_t group_index) {
    const ai_state st = state();
    // The hazard's named constant: NOT a value read from anywhere, a documented stand-in for the
    // original's uninitialised stack slots on the kind == 0 arm. See ai_group_redistribute.h.
    (void)detail::group_redistribute_units(st.read, st.own, live_calls(), player, group_index,
                                           /*split_off_member_count=*/0, /*split_off_centroid_x=*/0,
                                           /*split_off_centroid_y=*/0);
}


} // namespace mh::ai
