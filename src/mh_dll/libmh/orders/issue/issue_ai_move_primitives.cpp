//
// orders/issue/issue_ai_move_primitives.cpp -- see issue_ai_move_primitives.h for the spec.
//
// DERIVED FROM tmp/decomp_orders_issue/llm_strat_ai_unit_flag_and_move_004d7e22.asm and
// tmp/decomp_orders_issue/llm_strat_ai_group_scatter_to_passable_tile_004d7e6b.asm, not from the .c
// drafts beside them. Addresses are EN /eng/mh.exe.
//
#include "orders/issue/issue_ai_move_primitives.h"

namespace mh::orders::issue {

void detail::unit_flag_and_move(const issue_view &v, const order_sink &, const issue_calls &c,
                                uint32_t player, int32_t unit_index, uint32_t target_x,
                                uint32_t target_y) {
    // 0x004d7e35 AND EAX,0xf -- masked for the record stride.
    const uint32_t p = player & 0xfu;

    mh::game::mh_map_object_unit &u = v.units[(int32_t)p * v.caps.units + unit_index];
    u.order_notify_status           = 1; // 0x004d7e4a  MOV byte [.. +0xe8],1
    u.order_status_flags |= 0x40;        // 0x004d7e51  OR  byte [.. +0xe3],0x40

    // 0x004d7e5a AND ESI,0xf / 0x004d7e5d MOVZX EAX,SI -- masked a SECOND time for the argument.
    // 0x004d7e58 PUSH 0x0 -- the fifth argument is a literal zero; no sequence id is consumed.
    c.unit_order_move_enqueue((uint16_t)(player & 0xfu), unit_index, target_x, target_y,
                              0u); // 0x004d7e60
}

void detail::group_scatter_to_passable_tile(const issue_view  &v, const order_sink &,
                                            const issue_calls &c, uint32_t player, int32_t anchor_x,
                                            int32_t anchor_y) {
    // 0x004d7e89 XOR ESI,ESI -- ONCE, outside the loop. The spiral index carries across members, so
    // each one lands on a further tile. See the header: resetting it per member is the bug this
    // comment exists to prevent.
    uint32_t s = 0;

    // 0x004d7f01 CMP / 0x004d7f07 JC -- the count is re-read every iteration and compared UNSIGNED.
    for (uint32_t i = 0; i < (uint32_t)*v.ai_group_scratch_count; ++i) {
        uint32_t x = 0;
        uint32_t y = 0;
        // 0x004d7e8d-0x004d7ebe: spiral outward WHILE the tile reads zero; stop on the first
        // non-zero cell. UNBOUNDED in the original -- nothing checks `s` against the table extent.
        for (;;) {
            const int8_t dx = v.ai_tile_spiral_offsets[s * 2 + 0];       // 0x004d7e8d MOVSX
            const int8_t dy = v.ai_tile_spiral_offsets[s * 2 + 1];       // 0x004d7e9e MOVSX
            x               = ((uint32_t)(anchor_x + dx)) & *v.width_m;  // 0x004d7e98 AND
            y               = ((uint32_t)(anchor_y + dy)) & *v.height_m; // 0x004d7eae AND
            ++s;                                                         // 0x004d7eb5 INC ESI
            if (v.passable[(x << 8) | y] != 0) break;                    // 0x004d7eb6 CMP / JZ
        }

        const int32_t unit_id = v.ai_group_scratch_list[i]; // 0x004d7ec3

        // 0x004d7ed1 IMUL EAX,[player],0x5b04 -- the player is NOT masked here, unlike
        // unit_flag_and_move. The original's asymmetry, preserved.
        mh::game::mh_map_object_unit &u = v.units[(int32_t)player * v.caps.units + unit_id];
        u.order_notify_status           = 1; // 0x004d7edb
        u.order_status_flags |= 0x40;        // 0x004d7ee2

        // 0x004d7ee9 PUSH 0x0 -- again a literal zero, no sequence id.
        // 0x004d7eeb re-loads the list entry for the call; same value, transcribed as one local.
        c.unit_order_move_enqueue((uint16_t)player, unit_id, x, y, 0u); // 0x004d7ef6
    }
}

void unit_flag_and_move(uint32_t player, int32_t unit_index, uint32_t target_x, uint32_t target_y) {
    detail::unit_flag_and_move(live_view(), live_sink(), live_calls(), player, unit_index, target_x,
                               target_y);
}

void group_scatter_to_passable_tile(uint32_t player, int32_t anchor_x, int32_t anchor_y) {
    detail::group_scatter_to_passable_tile(live_view(), live_sink(), live_calls(), player, anchor_x,
                                           anchor_y);
}

} // namespace mh::orders::issue
