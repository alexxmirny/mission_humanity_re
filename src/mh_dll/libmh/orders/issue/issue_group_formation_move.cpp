//
// orders/issue/issue_group_formation_move.cpp -- see issue_group_formation_move.h for the spec.
//
// DERIVED FROM tmp/decomp_orders_issue/llm_strat_ai_group_move_formation_rotating_004d7da9.asm, not
// from the .c draft beside it. Addresses are EN /eng/mh.exe.
//
#include "orders/issue/issue_group_formation_move.h"

namespace mh::orders::issue {

void detail::group_move_formation_rotating(const issue_view  &v, const order_sink &,
                                           const issue_calls &c, uint32_t player,
                                           int32_t unused_param2, int32_t unused_param3,
                                           int32_t target_x, int32_t target_y) {
    // Dead in the original -- EDX is never read and EBX is clobbered by the loop's first
    // instruction. Named away rather than removed: the parameters are the original's ABI.
    (void)unused_param2;
    (void)unused_param3;

    // 0x004d7e03: the bound is re-read from the region every iteration and compared UNSIGNED (JC).
    for (uint32_t i = 0; i < (uint32_t)*v.ai_group_scratch_count; ++i) {
        const int32_t unit_id = v.ai_group_scratch_list[i]; // 0x004d7dc4

        mh::game::mh_map_object_unit &u = v.units[(int32_t)player * v.caps.units + unit_id];
        u.order_notify_status           = 1; // 0x004d7dd5  MOV byte [.. +0xe8],1
        u.order_status_flags |= 0x40;        // 0x004d7ddd  OR  byte [.. +0xe3],0x40

        // 0x004d7de5 MOVZX: the seq id is read fresh per member, so one formation's orders all carry
        // the SAME stamp -- the only writer is the tail below. 0x004d7ded re-loads the list entry for
        // the call; same value, transcribed as one local.
        const uint32_t seq = v.order_seq_id_by_player[player];
        c.unit_order_move_enqueue((uint16_t)player, unit_id, (uint32_t)target_x, (uint32_t)target_y,
                                  seq); // 0x004d7dfd
    }

    // 0x004d7e0b-0x004d7e1d, reached on EVERY call including the empty-list one: INC, and INC again
    // only if that wrapped the byte to 0 (JNZ over the second INC).
    uint8_t &seq = v.order_seq_id_by_player[player];
    ++seq;
    if (seq == 0) ++seq;
}

void group_move_formation_rotating(uint32_t player, int32_t unused_param2, int32_t unused_param3,
                                   int32_t target_x, int32_t target_y) {
    detail::group_move_formation_rotating(live_view(), live_sink(), live_calls(), player,
                                          unused_param2, unused_param3, target_x, target_y);
}

} // namespace mh::orders::issue
