//
// tact/tact_unit_cmd_advance.h -- TACT1A/B: the command-queue dequeue/advance
// frontier both stance functions and several already-verified TACT1B rows call.
//
//   llm_tact_unit_cmd_advance @0x00431229 (0x1a6)
//
#pragma once
#include <cstdint>

#include "tact/tact_state.h"

namespace mh::tact {
namespace detail {

// llm_tact_unit_cmd_advance @0x00431229.
//
// 1. @0x00431246-0x0043126b: a dead double-CMP on `progress` (0x00431254-0x0043125b/0x00431264) --
//    neither CMP is followed by a Jcc that reads it, both fall through unconditionally to the same
//    next instruction. Preserved as a no-op, not transcribed.
// 2. @0x0043126b-0x00431297: if move_path_slot != 0, release it
//    (path_slot_flag_at(0, move_path_slot) = 0) -- the ONLY conditional write in the function.
// 3. @0x00431297-0x0043131c: unconditionally zero move_path_slot, then
//    cmd_queue[cmd_slot_index].{op,arg0,arg1,arg2,arg3} (NOT interrupt_flag -- matches the field's
//    own comment: this byte is deliberately left stale whenever op == 0).
// 4. @0x0043131c-0x00431335: zero move_retry_attempts and move_stuck_countdown (a first pass; both
//    are set to 0 again in step 6 below -- the original writes each twice, redundantly; reproduced
//    as one write per field since both writes store the identical value 0).
// 5. @0x00431335-0x00431360: increment+wrap the unit's OWN cmd_index at 0x80 (every wrap site in
//    the file tests `0x7f < idx`, so index 0x80 is unreachable).
// 6. @0x00431360-0x00431396: if the NEW cmd_index slot's op == 0, stamp
//    wander_check_time = time_GetCurrentTime() (pure frontier call).
// 7. @0x00431396-0x004313c6: unconditionally zero move_retry_wait, move_retry_attempts (again) and
//    move_stuck_countdown (again).
void unit_cmd_advance(tact_store &own, int32_t unit_idx, int32_t cmd_slot_index);

} // namespace detail

void unit_cmd_advance(int32_t unit_idx, int32_t cmd_slot_index);


} // namespace mh::tact
