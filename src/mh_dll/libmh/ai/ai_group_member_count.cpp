//
// ai/ai_group_member_count.cpp -- see ai_group_member_count.h. Translated from the DISASSEMBLY:
//   tmp/decomp/llm_strat_ai_group_member_count_adjust_004db499.asm
//
#include "ai/ai_group_member_count.h"


namespace mh::ai {
namespace {

// The three seed-group destinations group_member_move is routed to on the mode != 0 path
// (LAB_004db4ee/LAB_004db501/LAB_004db517 setting EBX to 2/4/3 respectively), all well inside
// AI_SEED_GROUP_COUNT (5, ai_state.h) -- real, always-present group slots, not synthesized indices.
inline constexpr int32_t SEED_GROUP_GROUND_OR_SOLDIER = 2;
inline constexpr int32_t SEED_GROUP_HELI              = 3;
inline constexpr int32_t SEED_GROUP_PLANE             = 4;

} // namespace

namespace detail {

void group_member_count_adjust(const ai_view &v, const ai_store &own, const ai_calls &gc,
                               uint32_t player, uint32_t unit_index, uint32_t group_or_type,
                               uint32_t mode) {
    (void)group_or_type; // NEVER READ in the original -- see the header comment: EBX is clobbered
                         // with a player_data-offset scratch value before it is ever used.

    const uint32_t player_00 = player & 0xfu; // AND EAX,0xf -- unconditional, no MAX_PLAYERS check
    if (v.players[player_00].ai_enabled == 0) return;

    const uint32_t src_group =
        gc.unit_get_ai_group_index((int32_t)player_00, (int32_t)unit_index);

    if (mode == 0) {
        // `DEC word ptr [player_data + player_00*0x288fc + src_group*0xa66 + 0xe7e430]` --
        // 0xe7e430 - 0xe6dec0 (player_data base) - 0x10568 (ai_groups[]) == 0x8 ==
        // offsetof(mh_llm_strat_ai_unit_group, active_member_count). A bare 16-bit wrapping
        // decrement, no floor check -- and `src_group` is used UNCHECKED, exactly as the original:
        // whatever unit_get_ai_group_index returned (including the 0xffff "no group" sentinel)
        // indexes straight in with no bound against AI_SEED_GROUP_COUNT/ai_group_count/array extent.
        unit_group &g         = own.players[player_00].ai_groups[src_group];
        g.active_member_count = (uint16_t)(g.active_member_count - 1);
        return;
    }

    // Classification order matches the original exactly: ground first (falls straight to the class-2
    // call site), then plane, then heli, then soldier (which also lands on the class-2 call site) --
    // none of the four -> return with no call at all.
    int32_t dst_group;
    if (gc.unit_is_ai_ground((uint16_t)player_00, unit_index) != 0) {
        dst_group = SEED_GROUP_GROUND_OR_SOLDIER;
    } else if (gc.unit_is_ai_plane(player_00, unit_index) != 0) {
        dst_group = SEED_GROUP_PLANE;
    } else if (gc.unit_is_ai_heli(player_00, unit_index) != 0) {
        dst_group = SEED_GROUP_HELI;
    } else if (gc.unit_is_ai_soldier(player_00, unit_index) != 0) {
        dst_group = SEED_GROUP_GROUND_OR_SOLDIER;
    } else {
        return;
    }

    gc.group_member_move(player_00, (int32_t)src_group, dst_group, (int32_t)unit_index);
}

} // namespace detail

void group_member_count_adjust(uint32_t player, uint32_t unit_index, uint32_t group_or_type,
                               uint32_t mode) {
    const ai_state st = state();
    detail::group_member_count_adjust(st.read, st.own, live_calls(), player, unit_index,
                                      group_or_type, mode);
}


} // namespace mh::ai
