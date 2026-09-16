//
// tact/tact_unit_cmd_stance.cpp -- see tact_unit_cmd_stance.h. Translated from the DISASSEMBLY, not
// from Ghidra's C.
//
#include "tact/tact_unit_cmd_stance.h"

#include "addr/mh_calls.gen.h"  // frontier callee (Law 4): llm_tact_unit_cmd_advance -- see the
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "tact/tact_unit_cmd_advance.h"
#include "state/rebind_targets.gen.h"

namespace mh::tact {

const unit_cmd_stance_calls &live_unit_cmd_stance_calls() {
    static const unit_cmd_stance_calls c = {
        MH_LIBMH_BIND(llm_tact_unit_cmd_advance),
    };
    return c;
}

namespace detail {

void unit_cmd_stance_on(tact_store &own, const unit_cmd_stance_calls &c, int32_t unit_idx) {
    tact_unit &u = own.unit_at(unit_idx);

    // @0x0043059d-0x004305b4.
    u.status |= 0x4;

    // @0x004305ba-0x004305ec: dispatch ONLY when the head slot IS an unstarted stance-on marker.
    // THE BRANCH READS THE OTHER WAY ROUND AND THIS FILE HAD IT INVERTED (fixed 2026-09-04,
    // TACT1-P C5). Read the two jumps to their targets rather than the shape of the C:
    //   CMP [op],0x1e / JNZ 0x004305ee -- op != 0x1e leaves through LAB_004305ee, whose whole body
    //                                     is `JMP 0x00430606`, and 0x00430606 is the EPILOGUE.
    //   CMP [progress],0 / JZ 0x004305f0 -- only progress == 0 reaches the call at 0x00430601.
    // So the call fires on (op == 0x1e && progress == 0) and NOTHING else. Inverted, this dispatched
    // on every OTHER command in the queue: on POZ3 with the tactical root promoted it churned the
    // command queue and the mission stopped producing frames after the first one.
    const int32_t cmd_index = u.cmd_index;
    if (u.cmd_queue[cmd_index].op != 0x1e || u.progress != 0) {
        return;
    }

    // @0x004305f0-0x00430606.
    c.unit_cmd_advance(unit_idx, cmd_index);
}

void unit_cmd_stance_off(tact_store &own, const unit_cmd_stance_calls &c, int32_t unit_idx) {
    tact_unit &u = own.unit_at(unit_idx);

    // @0x0043062c-0x00430643.
    u.status &= (uint8_t)~0x4;

    // @0x00430649-0x0043067b: the same shape as stance_on above, one opcode over -- dispatch ONLY
    // when the head slot IS an unstarted stance-off marker. `JNZ 0x0043067d` (op != 0x1c) lands on a
    // bare `JMP 0x00430695`, the epilogue; only `JZ 0x0043067f` (progress == 0) reaches the call.
    // Inverted here too, and fixed with its twin (TACT1-P C5, 2026-09-04).
    const int32_t cmd_index = u.cmd_index;
    if (u.cmd_queue[cmd_index].op != 0x1c || u.progress != 0) {
        return;
    }

    // @0x0043067f-0x00430695.
    c.unit_cmd_advance(unit_idx, cmd_index);
}

} // namespace detail

void unit_cmd_stance_on(int32_t unit_idx) {
    tact_state st = state();
    detail::unit_cmd_stance_on(st.own, live_unit_cmd_stance_calls(), unit_idx);
}

void unit_cmd_stance_off(int32_t unit_idx) {
    tact_state st = state();
    detail::unit_cmd_stance_off(st.own, live_unit_cmd_stance_calls(), unit_idx);
}

} // namespace mh::tact


namespace mh::tact {


} // namespace mh::tact
