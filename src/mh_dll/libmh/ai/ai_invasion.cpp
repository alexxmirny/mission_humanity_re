//
// ai/ai_invasion.cpp -- see ai_invasion.h. Translated from the DISASSEMBLY
// (tmp/decomp_ai/llm_strat_ai_invasion_launch_attack_group_004e8a1f.asm).
//
#include "ai/ai_invasion.h"


namespace mh::ai {
namespace detail {

invasion_launch_report invasion_launch_attack_group(const ai_view &v, const ai_store &own,
                                                    const ai_calls &gc, uint32_t player_id) {
    invasion_launch_report rep{};
    player_data           &pd = own.players[player_id];

    // Guard 1 (0x004e8a4a/0x004e8a51): still reinforcing.
    if (pd.ai_invasion_points != 0) {
        rep.invasion_points_pending = true;
        return rep;
    }
    // Guard 2 (0x004e8a57/0x004e8a5f): nothing staged in the home group.
    if (pd.ai_groups[0].member_count == 0) {
        rep.home_group_empty = true;
        return rep;
    }
    // Guard 3 (0x004e8a69-0x004e8a87): a census over 0..ai_group_count, UNSIGNED bound (JC), bailing
    // the moment any existing group already carries goal 3 or 0xb.
    for (uint32_t g = 0; g < (uint32_t)pd.ai_group_count; ++g) {
        ++rep.census_scanned;
        const int16_t goal = pd.ai_groups[g].goal;
        if (goal == 3 || goal == 0xb) {
            rep.attack_already_underway = true;
            return rep;
        }
    }

    // Pick the weakest opponent. NO hostility filter (contrast ai_army_milestone.cpp's sibling
    // scan, which skips friendly/self via ai_player_relation) -- the bytes here test only
    // `cand == player`. best stays -1 if no other active player exists.
    int32_t weakest = -1;
    for (uint32_t cand = 0; cand < (uint32_t)*v.active_player_count; ++cand) { // 0x004e8b04 CMP/JC
        if (cand == player_id) continue;                                       // 0x004e8ab7 JZ
        if (weakest == -1 ||                                                   // 0x004e8abc JZ
            (uint32_t)pd.ai_opponent_assessments[cand].total_unit_power <
                (uint32_t)pd.ai_opponent_assessments[weakest].total_unit_power) { // 0x004e8ae6 JNC
            weakest = (int32_t)cand;
        }
    }
    rep.target_player_id = weakest;

    const int32_t g = gc.group_create((int32_t)player_id);
    rep.group_index = g;
    if (g == -1) { // 0x004e8b18 -- the one real early return past the census
        rep.create_failed = true;
        return rep;
    }
    pd.ai_groups[g].goal             = 0xb;     // word @0x004e8b27
    pd.ai_groups[g].target_player_id = weakest; // dword @0x004e8b30

    // Drain ai_groups[0] completely into the new group. head_unit is RE-READ every iteration
    // (0x004e8b3d/0x004e8b52) because group_member_move mutates the source list.
    while (pd.ai_groups[0].member_count != 0) {
        const uint16_t u = pd.ai_groups[0].head_unit;
        gc.group_member_move(player_id, /*src*/ 0, /*dst*/ g, (int32_t)u);
        ++rep.units_moved;
    }

    // Two enqueues, same group, same pending_param (0x183): attack_random (0x18), then the
    // terminal/park code 0 -- NOT a typo, see the header. The second reaches the game through the
    // shared tail also used by llm_strat_ai_group_expansion_form_or_repurpose (JMP 0x004e7925).
    gc.group_task_enqueue((int32_t)player_id, g, 0x18, 0x183, 0, 0, 0, 0, 0);
    gc.group_task_enqueue((int32_t)player_id, g, 0, 0x183, 0, 0, 0, 0, 0);
    rep.tasks_enqueued = 2;
    rep.launched       = true;
    return rep;
}

} // namespace detail

void invasion_launch_attack_group(uint32_t player_id) {
    const ai_state st = state();
    (void)detail::invasion_launch_attack_group(st.read, st.own, live_calls(), player_id);
}


} // namespace mh::ai
