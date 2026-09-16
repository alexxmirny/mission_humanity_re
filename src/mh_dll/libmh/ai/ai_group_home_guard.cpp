//
// ai/ai_group_home_guard.cpp -- see ai_group_home_guard.h. Translated from the LIVE Ghidra decompile
// at 0x004e6f39 (already adversarially corrected in an earlier AI-PREP pass) cross-checked against
// tmp/decomp_ai/llm_strat_ai_group_home_guard_replenish_004e6f39.asm.
//
#include "ai/ai_group_home_guard.h"


namespace mh::ai {
namespace detail {

namespace {
constexpr uint32_t AI_UNIT_FIGHTER = 8; // cfg_enum_ai_E_UNIT (confirmed via ReVA: SOLDIER=1 .. FIGHTER=8)
} // namespace

home_guard_report group_home_guard_replenish(const ai_view &v, const ai_store &own,
                                             const ai_calls &gc, int32_t player_id) {
    home_guard_report rep{};
    player_data      &pd = own.players[player_id];

    // Step 1: unconditional.
    gc.group1_drain_to_group0(player_id);

    // Step 2: target headcount. Watcom's 2-operand IMUL truncates the product to 32 bits -- done here
    // in uint32_t (defined wraparound) rather than int32_t (signed overflow is UB) -- then the
    // result is reinterpreted signed for the division, matching the original's CDQ+IDIV exactly.
    const housing_stats &h = v.unit_housing[player_id];
    const uint32_t       housing_sum =
        (uint32_t)h.used_soldiers + (uint32_t)h.used_vehicles; // defined wraparound, not int32_t UB
    const uint32_t product = housing_sum * (uint32_t)*v.home_guard_target_pct;
    rep.target             = (int32_t)product / 100; // 0x004e6f5a-0x004e6f79

    if (pd.ai_resource_shortage_state == 3) { // 0x004e6f94/0x004e6f9b
        rep.target            = (int32_t)((uint32_t)(uint16_t)pd.ai_groups[0].member_count +
                               (uint32_t)(uint16_t)pd.ai_groups[2].member_count);
        rep.shortage_override = true;
    }

    // Step 3: route idle-role FIGHTER members of group 0 to storage. Only outside shortage_state 3.
    // `next` is the member's OWN ai_group_next, captured BEFORE the call (0x004e6fe7/0x004e6fee),
    // not re-read off head_unit.
    if (pd.ai_resource_shortage_state != 3) { // 0x004e6fc6/0x004e6fcd
        uint16_t u = pd.ai_groups[0].head_unit;
        while (u != 0) {
            const uint16_t next = unit_of(v, player_id, u).ai_group_next;
            if (v.cfg_units[unit_of(v, player_id, u).unit_proto_id].ai_unit == AI_UNIT_FIGHTER) {
                gc.route_unit_to_home_storage(player_id, (int32_t)u);
                ++rep.routed_home;
            }
            u = next;
        }
    }

    // Step 4: refill from pool 2, NO type filter. head_unit is RE-READ every iteration (unlike
    // steps 3/5) because group_member_move changes which unit is head.
    while ((uint32_t)rep.target > (uint32_t)(uint16_t)pd.ai_groups[0].member_count && // 0x004e7040
           pd.ai_groups[2].member_count != 0) {                                       // 0x004e7045
        const uint16_t u = pd.ai_groups[2].head_unit;
        gc.unit_launch_from_storage_enqueue((uint8_t)player_id, (int32_t)u, pd.ai_home_tile_x,
                                            pd.ai_home_tile_y);
        gc.group_member_move(player_id, /*src*/ 2, /*dst*/ 0, (int32_t)u);
        ++rep.refilled_from_pool2;
    }

    // Step 5: shortage restock from pool 4, FIGHTER members only. Same captured-next shape as step 3.
    if (pd.ai_resource_shortage_state == 3) { // 0x004e7096/0x004e709d
        uint16_t u = pd.ai_groups[4].head_unit;
        while (u != 0) {
            const uint16_t next = unit_of(v, player_id, u).ai_group_next;
            if (v.cfg_units[unit_of(v, player_id, u).unit_proto_id].ai_unit == AI_UNIT_FIGHTER) {
                gc.unit_launch_from_storage_enqueue((uint8_t)player_id, (int32_t)u,
                                                    pd.ai_home_tile_x, pd.ai_home_tile_y);
                gc.group_member_move(player_id, /*src*/ 4, /*dst*/ 0, (int32_t)u);
                ++rep.refilled_from_pool4;
            }
            u = next;
        }
    }

    gc.group_reposition_members(player_id, 0);
    return rep;
}

} // namespace detail

void group_home_guard_replenish(int32_t player_id) {
    const ai_state st = state();
    (void)detail::group_home_guard_replenish(st.read, st.own, live_calls(), player_id);
}


} // namespace mh::ai
