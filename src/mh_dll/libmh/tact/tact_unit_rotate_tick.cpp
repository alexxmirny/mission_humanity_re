//
// tact/tact_unit_rotate_tick.cpp -- see tact_unit_rotate_tick.h. Translated from the DISASSEMBLY,
// not from Ghidra's C.
//
#include "tact/tact_unit_rotate_tick.h"

#include "addr/mh_calls.gen.h"  // frontier callee (Law 4): llm_tact_unit_rotate_step
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "tact/tact_unit_rotate_step.h"

namespace mh::tact {
namespace detail {

void unit_rotate_tick(tact_store &own, int32_t unit_idx) {
    tact_unit &u = own.unit_at(unit_idx);

    // @0x004307e2-0x004307f0: a firing unit does not rotate -- clear face_cmd_op and return.
    if ((u.status & 0x8) != 0) {
        u.face_cmd_op = 0;
        return;
    }

    // @0x00430804-0x00430822: compare current facing to the commanded target.
    if (u.facing_dir == u.face_cmd_target_dir) {
        // @0x0043083c-0x00430843: already facing the target -- arrived, clear face_cmd_op.
        u.face_cmd_op = 0;
    } else {
        // @0x00430824-0x0043083a: dispatch one rotation step toward the target.
        MH_LIBMH_BIND(llm_tact_unit_rotate_step)(unit_idx, u.face_cmd_target_dir);
    }
}

} // namespace detail

void unit_rotate_tick(int32_t unit_idx) {
    tact_state st = state();
    detail::unit_rotate_tick(st.own, unit_idx);
}

} // namespace mh::tact


namespace mh::tact {


} // namespace mh::tact
