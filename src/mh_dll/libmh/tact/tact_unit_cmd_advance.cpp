//
// tact/tact_unit_cmd_advance.cpp -- see tact_unit_cmd_advance.h. Translated from the DISASSEMBLY,
// not from Ghidra's C.
//
#include "tact/tact_unit_cmd_advance.h"

#include "addr/mh_calls.gen.h" // frontier callee (Law 4): time_GetCurrentTime
#include "state/mode_planes.h"
#include "state/promoted_select.h" // LIB-REF-SPLIT: MH_PROMOTED

namespace mh::tact {
namespace detail {

void unit_cmd_advance(tact_store &own, int32_t unit_idx, int32_t cmd_slot_index) {
    tact_unit &u = own.unit_at(unit_idx);

    // @0x00431246-0x0043126b: dead double-CMP on `progress`, no Jcc consumes either result. Not
    // reproduced.

    // @0x0043126b-0x00431297: release the move-path slot, if one was held.
    if (u.move_path_slot != 0) {
        own.planes().path_slot_flag_at(0, u.move_path_slot) = 0;
    }

    // @0x00431297-0x0043131c: unconditional resets.
    u.move_path_slot                 = 0;
    u.cmd_queue[cmd_slot_index].op   = 0;
    u.cmd_queue[cmd_slot_index].arg0 = 0;
    u.cmd_queue[cmd_slot_index].arg1 = 0;
    u.cmd_queue[cmd_slot_index].arg2 = 0;
    u.cmd_queue[cmd_slot_index].arg3 = 0;
    u.move_retry_attempts            = 0;
    u.move_stuck_countdown           = 0;

    // @0x00431335-0x00431360: advance the unit's OWN cmd_index, wrapping at 0x80.
    ++u.cmd_index;
    if (u.cmd_index >= 0x80) {
        u.cmd_index = 0;
    }

    // @0x00431360-0x00431396: if the NEW slot is free, stamp the wander-check timer.
    if (u.cmd_queue[u.cmd_index].op == 0) {
        u.wander_check_time = MH_PROMOTED_ROW(time_GetCurrentTime)();
    }

    // @0x00431396-0x004313c6: final unconditional resets.
    u.move_retry_wait      = 0;
    u.move_retry_attempts  = 0;
    u.move_stuck_countdown = 0;
}

} // namespace detail

void unit_cmd_advance(int32_t unit_idx, int32_t cmd_slot_index) {
    tact_state st = state();
    detail::unit_cmd_advance(st.own, unit_idx, cmd_slot_index);
}

} // namespace mh::tact


namespace mh::tact {


} // namespace mh::tact
