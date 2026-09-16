//
// tact/tact_unit_cmd_advance_with_defstat.cpp -- see tact_unit_cmd_advance_with_defstat.h.
// Translated from the DISASSEMBLY, not from Ghidra's C.
//
#include "tact/tact_unit_cmd_advance_with_defstat.h"

#include "addr/mh_calls.gen.h"  // frontier callee (Law 4): llm_tact_unit_cmd_advance
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "tact/tact_unit_cmd_advance.h"
#include "state/rebind_targets.gen.h"

namespace mh::tact {

const unit_cmd_advance_with_defstat_calls &live_unit_cmd_advance_with_defstat_calls() {
    static const unit_cmd_advance_with_defstat_calls c = {
        MH_LIBMH_BIND(llm_tact_unit_cmd_advance),
    };
    return c;
}

namespace detail {

void unit_cmd_advance_with_defstat(tact_store &own, const unit_cmd_advance_with_defstat_calls &c,
                                   int32_t unit_idx, int32_t cmd_slot_index) {
    tact_unit &u = own.unit_at(unit_idx);

    // @0x0042f901-0x0042f91b: only the low byte of the head entry's arg0 is read.
    u.def_stat = (uint8_t)u.cmd_queue[cmd_slot_index].arg0;

    // @0x0042f921-0x0042f927: dispatch the real dequeue/advance.
    c.unit_cmd_advance(unit_idx, cmd_slot_index);
}

} // namespace detail

void unit_cmd_advance_with_defstat(int32_t unit_idx, int32_t cmd_slot_index) {
    tact_state st = state();
    detail::unit_cmd_advance_with_defstat(st.own, live_unit_cmd_advance_with_defstat_calls(),
                                          unit_idx, cmd_slot_index);
}

} // namespace mh::tact


namespace mh::tact {


} // namespace mh::tact
