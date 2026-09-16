//
// tact/tact_unit_stand_tick.cpp -- see tact_unit_stand_tick.h. Translated from the DISASSEMBLY, not
// from Ghidra's C.
//
#include "tact/tact_unit_stand_tick.h"

#include "addr/mh_calls.gen.h"  // frontier/sibling callees (Law 4)
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "tact/tact_unit_cmd_advance.h"
#include "tact/tact_unit_set_anim_state.h"
#include "state/rebind_targets.gen.h"

namespace mh::tact {

const unit_stand_tick_calls &live_unit_stand_tick_calls() {
    static const unit_stand_tick_calls c = {
        MH_LIBMH_BIND(llm_tact_unit_set_anim_state),
        MH_LIBMH_BIND(llm_tact_unit_cmd_advance),
    };
    return c;
}

namespace detail {

void unit_stand_tick(tact_store &own, const unit_stand_tick_calls &c, int32_t unit_idx) {
    tact_unit &u = own.unit_at(unit_idx);

    // @0x0043049a-0x004304b8: THE TWO KNEEL/TURN VALUES SELECT THE ANIMATION ARM, AND THIS FILE HAD
    // IT INVERTED (fixed 2026-09-04 from a played session: a unit kneeled and could never stand).
    // Both tests land on a bare trampoline, so follow the jumps to their TARGETS, not the shape of
    // the C:
    //   CMP [anim_state],2 / JZ  0x004304ba -- LAB_004304ba's whole body is `JMP 0x004304fb`, which
    //                                          is the PROGRESS arm below.
    //   CMP [anim_state],3 / JNZ 0x004304bc -- so anim_state != 3 leaves for the dequeue-only arm,
    //                                          and == 3 falls through into that same trampoline.
    // So 2 or 3 -> animate; anything else -> watch for a queued STAND and dequeue it. Inverted, a
    // kneeling unit (anim_state 3) never reached set_anim_state(0), so it never stood back up --
    // neither by a STAND order nor via llm_tact_unit_move_tick, which drives the same body.
    if (u.anim_state != 2 && u.anim_state != 3) {
        // @0x004304bc-0x004304f1: a STAND queued on a unit that is not mid kneel/turn has no
        // animation to run; it is simply dequeued.
        if (u.cmd_queue[u.cmd_index].op == 5) c.unit_cmd_advance(unit_idx, u.cmd_index);
        return; // @0x004304f6
    }

    // @0x004304fb-0x00430510: unconditional stand-progress advance + anim state 3.
    ++u.progress;
    c.unit_set_anim_state(unit_idx, 3);

    // @0x00430515-0x00430523
    if (u.progress <= 0xf) return; // @0x00430577 (falls straight to the epilogue)

    // @0x00430525-0x00430538
    u.progress = 0;
    c.unit_set_anim_state(unit_idx, 0);

    // @0x0043053d-0x00430572: same dequeue check as the kneeling/turning arm above.
    if (u.cmd_queue[u.cmd_index].op == 5) c.unit_cmd_advance(unit_idx, u.cmd_index);
}

} // namespace detail

void unit_stand_tick(int32_t unit_idx) {
    tact_state st = state();
    detail::unit_stand_tick(st.own, live_unit_stand_tick_calls(), unit_idx);
}

} // namespace mh::tact


namespace mh::tact {


} // namespace mh::tact
