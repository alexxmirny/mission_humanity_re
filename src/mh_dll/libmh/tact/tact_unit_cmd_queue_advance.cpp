//
// tact/tact_unit_cmd_queue_advance.cpp -- see tact_unit_cmd_queue_advance.h. Translated from the
// DISASSEMBLY, not from Ghidra's C.
//
#include "tact/tact_unit_cmd_queue_advance.h"

#include "addr/mh_calls.gen.h"  // frontier callee (Law 4): llm_tact_unit_cmd_advance
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "tact/tact_unit_cmd_advance.h"

namespace mh::tact {
namespace detail {

void unit_cmd_queue_advance(tact_store &own, int32_t unit_idx, uint32_t cmd_index) {
    tact_unit &u = own.unit_at(unit_idx);

    // @0x004313ec-0x0043140d: only when status bit 0x2 is set AND progress == 0.
    if ((u.status & 0x2) == 0 || u.progress != 0) return;

    // @0x00431414-0x0043142b.
    u.status &= 0xfd;

    // @0x0043143e-0x0043149f: drain from cmd_index, following the unit's OWN cmd_index field as
    // each dispatch may advance it (see header banner).
    for (;;) {
        if (u.cmd_queue[cmd_index].op == 0) break;
        if (u.cmd_queue[cmd_index].op == 0x7f) {
            MH_LIBMH_BIND(llm_tact_unit_cmd_advance)(unit_idx, (int32_t)cmd_index);
            break;
        }
        MH_LIBMH_BIND(llm_tact_unit_cmd_advance)(unit_idx, (int32_t)cmd_index);
        cmd_index = u.cmd_index;
    }
}

} // namespace detail

void unit_cmd_queue_advance(int32_t unit_idx, uint32_t cmd_index) {
    tact_state st = state();
    detail::unit_cmd_queue_advance(st.own, unit_idx, cmd_index);
}

} // namespace mh::tact


namespace mh::tact {


} // namespace mh::tact
