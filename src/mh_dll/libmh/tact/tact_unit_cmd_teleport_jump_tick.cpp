//
// tact/tact_unit_cmd_teleport_jump_tick.cpp -- see tact_unit_cmd_teleport_jump_tick.h. Translated
// from the DISASSEMBLY, not from Ghidra's C.
//
#include "tact/tact_unit_cmd_teleport_jump_tick.h"

#include "addr/mh_calls.gen.h"  // frontier callees (Law 4): llm_tact_teleport_cmdqueue_jump, llm_tact_unit_cmd_advance
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "tact/tact_unit_cmd_advance.h"
#include "state/rebind_targets.gen.h"

namespace mh::tact {

const unit_cmd_teleport_jump_tick_calls &live_unit_cmd_teleport_jump_tick_calls() {
    static const unit_cmd_teleport_jump_tick_calls c = {
        MH_LIBMH_BIND(llm_tact_teleport_cmdqueue_jump),
        MH_LIBMH_BIND(llm_tact_unit_cmd_advance),
    };
    return c;
}

namespace detail {

void unit_cmd_teleport_jump_tick(tact_store &own, const unit_cmd_teleport_jump_tick_calls &c,
                                 int32_t unit_idx, int32_t cmd_slot_index) {
    tact_unit &u = own.unit_at(unit_idx);

    // @0x0043348e-0x004334ac: destination tile, zero-extended from the entry's uint16_t fields.
    const int32_t dest_x = u.cmd_queue[cmd_slot_index].arg0;
    const int32_t dest_y = u.cmd_queue[cmd_slot_index].arg1;

    // @0x004334b8: the real teleport.
    const int32_t result = c.teleport_cmdqueue_jump(unit_idx, dest_x, dest_y);

    // @0x004334c0-0x004334cc: dequeue only on success; otherwise retry next tick.
    if (result == 0) {
        c.unit_cmd_advance(unit_idx, cmd_slot_index);
    }
}

} // namespace detail

void unit_cmd_teleport_jump_tick(int32_t unit_idx, int32_t cmd_slot_index) {
    tact_state st = state();
    detail::unit_cmd_teleport_jump_tick(st.own, live_unit_cmd_teleport_jump_tick_calls(), unit_idx,
                                        cmd_slot_index);
}

} // namespace mh::tact


namespace mh::tact {


} // namespace mh::tact
