#pragma once
#include <cstdint>

#include "tact/tact_state.h"

namespace mh::tact {

// The one outward call this function makes, indirected for offline testability -- same shape as
// tact_unit_cmd_teleport_jump_tick_calls.
struct teleport_cmdqueue_jump_calls {
    void (*unit_teleport)(int32_t teleport_id, int32_t building_id); // llm_tact_unit_teleport @0x00432df0
};

const teleport_cmdqueue_jump_calls &live_teleport_cmdqueue_jump_calls();

namespace detail {

// llm_tact_teleport_cmdqueue_jump @0x004334da.
//
// 1. @0x004334f9-0x0043351c: the scratch zone (slot TACT_TELEPORT_SCRATCH_SLOT) is written
//    UNCONDITIONALLY, before the passability check -- id=0x41 (the slot's own index, matching the
//    argument the callee receives on success), mode=0, no_enemy=0, field_28=0, association[0]=0,
//    dest_col[0]=dest_x, dest_row[0]=dest_y (both raw, un-clamped stores into element 0 of the
//    8-element dest_col/dest_row arrays).
// 2. @0x0043352e-0x0043353c: EAX = byte offset of tile_objects[dest_x][dest_y] (dest_x*2048 +
//    dest_y*8, the same (x<<11)|(y<<3) folding tact_state.h's tile_at() expresses as (x<<8)|y
//    element-indexed then *8 for the struct stride).
// 3. @0x0043353c-0x00433544: if that tile's `.building` field (offset +2 in
//    mh_map_tile_object_data) is NONZERO, skip the call and return 0 -- a destination tile with a
//    building on it refuses the teleport.
// 4. @0x00433546-0x00433553: otherwise call llm_tact_unit_teleport(teleport_id=0x41,
//    building_id=unit_id) and return 1.
int32_t teleport_cmdqueue_jump(tact_store &own, const mh::state::mode_planes &planes,
                               const teleport_cmdqueue_jump_calls &c, int32_t unit_id, int32_t dest_x,
                               int32_t dest_y);

} // namespace detail

int32_t teleport_cmdqueue_jump(int32_t unit_id, int32_t dest_x, int32_t dest_y);


} // namespace mh::tact
