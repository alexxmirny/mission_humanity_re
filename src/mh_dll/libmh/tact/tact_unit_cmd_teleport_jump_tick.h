#pragma once
#include <cstdint>

#include "tact/tact_state.h"

namespace mh::tact {

// The two outward calls this function makes, indirected via mh:: for offline testability -- same
// shape as tact_unit_cmd_advance_with_defstat_calls.
struct unit_cmd_teleport_jump_tick_calls {
    int32_t (*teleport_cmdqueue_jump)(int32_t unit_id, int32_t dest_x,
                                      int32_t dest_y); // llm_tact_teleport_cmdqueue_jump @0x004334da
    void (*unit_cmd_advance)(int32_t unit_idx,
                             int32_t cmd_slot_index); // llm_tact_unit_cmd_advance @0x00431229
};

const unit_cmd_teleport_jump_tick_calls &live_unit_cmd_teleport_jump_tick_calls();

namespace detail {

// llm_tact_unit_cmd_teleport_jump_tick @0x00433464.
//
// 1. @0x0043348e-0x004334ac: dest_x = cmd_queue[cmd_slot_index].arg0, dest_y =
//    cmd_queue[cmd_slot_index].arg1 (both zero-extended uint16_t reads).
// 2. @0x004334b8: result = llm_tact_teleport_cmdqueue_jump(unit_idx, dest_x, dest_y).
// 3. @0x004334c0-0x004334cc: dequeue (llm_tact_unit_cmd_advance(unit_idx, cmd_slot_index)) only
//    when result == 0; otherwise leave the command queued (retry next tick).
void unit_cmd_teleport_jump_tick(tact_store &own, const unit_cmd_teleport_jump_tick_calls &c,
                                 int32_t unit_idx, int32_t cmd_slot_index);

} // namespace detail

void unit_cmd_teleport_jump_tick(int32_t unit_idx, int32_t cmd_slot_index);


} // namespace mh::tact
