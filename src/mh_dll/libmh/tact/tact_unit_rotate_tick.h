//
// tact/tact_unit_rotate_tick.h -- TACT1B: advance a unit toward its commanded facing.
//
//   llm_tact_unit_rotate_tick @0x004307c5 (0x90)
//
#pragma once
#include <cstdint>

#include "tact/tact_state.h"

namespace mh::tact {
namespace detail {

// llm_tact_unit_rotate_tick @0x004307c5.
//
// 1. @0x004307e2-0x004307f0: if status bit 0x8 (FIRE) is set, clear face_cmd_op (+0x5e0) and
//    return -- a firing unit does not rotate.
// 2. @0x00430804-0x00430822: else compare facing_dir (+0xa) to face_cmd_target_dir (+0x5e2, the
//    immediate FACE/TURN record's target heading).
// 3. @0x00430824-0x0043083a: if they differ, dispatch llm_tact_unit_rotate_step(unit_idx,
//    face_cmd_target_dir) (frontier) and return.
// 4. @0x0043083c-0x00430843: if they already match, clear face_cmd_op and return (arrived).
void unit_rotate_tick(tact_store &own, int32_t unit_idx);

} // namespace detail

void unit_rotate_tick(int32_t unit_idx);


} // namespace mh::tact
