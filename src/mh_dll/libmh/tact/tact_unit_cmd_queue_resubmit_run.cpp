//
// tact/tact_unit_cmd_queue_resubmit_run.cpp -- see tact_unit_cmd_queue_resubmit_run.h. Translated
// from the DISASSEMBLY, not from Ghidra's C.
//
#include "tact/tact_unit_cmd_queue_resubmit_run.h"

#include "addr/mh_calls.gen.h"  // frontier callees (Law 4)
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "tact/tact_unit_cmd_advance.h"
#include "tact/tact_unit_enqueue_command.h"

namespace mh::tact {
namespace detail {

void unit_cmd_queue_resubmit_run(tact_store &own, int32_t unit_idx, int32_t queue_slot) {
    tact_unit &u = own.unit_at(unit_idx);

    // @0x004306c8-0x004307ac: INCLUSIVE loop, arg0+1 iterations -- see header banner.
    int32_t cur_slot = queue_slot;
    for (int32_t i = 0; i <= (int32_t)u.cmd_queue[queue_slot].arg0; ++i) {
        auto &e = u.cmd_queue[cur_slot];
        MH_LIBMH_BIND(llm_tact_unit_enqueue_command)(unit_idx, e.op, e.interrupt_flag, e.arg0, e.arg1,
                                                     e.arg2, e.arg3);
        // @0x00430796-0x004307a5: wrap 0x80 -> 0.
        ++cur_slot;
        if (cur_slot == 0x80) cur_slot = 0;
    }

    // @0x004307b1-0x004307b7: the ORIGINAL queue_slot, not the walked cur_slot.
    MH_LIBMH_BIND(llm_tact_unit_cmd_advance)(unit_idx, queue_slot);
}

} // namespace detail

void unit_cmd_queue_resubmit_run(int32_t unit_idx, int32_t queue_slot) {
    tact_state st = state();
    detail::unit_cmd_queue_resubmit_run(st.own, unit_idx, queue_slot);
}

} // namespace mh::tact


namespace mh::tact {


} // namespace mh::tact
