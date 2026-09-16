//
// tact/tact_unit_kneel_tick.cpp -- see tact_unit_kneel_tick.h. Translated from the DISASSEMBLY, not
// from Ghidra's C.
//
#include "tact/tact_unit_kneel_tick.h"

#include "addr/mh_calls.gen.h"  // frontier callees (Law 4): llm_tact_unit_set_anim_state,
                                // llm_tact_unit_cmd_advance
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "tact/tact_unit_cmd_advance.h"
#include "tact/tact_unit_set_anim_state.h"
#include "state/rebind_targets.gen.h"

namespace mh::tact {

const unit_kneel_tick_calls &live_unit_kneel_tick_calls() {
    static const unit_kneel_tick_calls c = {
        MH_LIBMH_BIND(llm_tact_unit_set_anim_state),
        MH_LIBMH_BIND(llm_tact_unit_cmd_advance),
    };
    return c;
}

namespace detail {

void unit_kneel_tick(tact_store &own, const unit_kneel_tick_calls &c, int32_t unit_id) {
    tact_unit &u = own.unit_at(unit_id);

    // @0x00430380-0x0043038e: already fully kneeled -- nothing to do.
    if (u.anim_state == 3) return;

    // @0x00430394-0x004303b2: mid kneel-down anim with no progress accrued yet -- skip straight to
    // the dequeue check below without touching progress or anim_state. Any other state (a different
    // anim_state, or anim_state == 2 with progress already nonzero) falls to the progress-advance
    // path below.
    if (u.anim_state == 2 && u.progress == 0) {
        // @0x004303b6-0x004303f0
        if (u.cmd_queue[u.cmd_index].op == 4) {
            c.cmd_advance(unit_id, u.cmd_index);
        }
        return;
    }

    // @0x004303f5-0x00430474
    u.progress = static_cast<uint8_t>(u.progress + 1);
    c.set_anim_state(unit_id, 2);
    // Re-read `u.progress` here rather than reuse the value just written above -- matching the
    // original, which reloads it from the array after the call instead of keeping a register copy
    // (0x0043040f/0x00430416). `u` is a live reference, so this happens automatically as long as no
    // local caches the value across the call.
    if (u.progress <= 0xf) return;

    u.progress = 0;
    c.set_anim_state(unit_id, 3);
    // Same discipline: cmd_index and cmd_queue[cmd_index].op are re-read AFTER this second call
    // (0x0043043a/0x00430441), matching the original's own re-load.
    if (u.cmd_queue[u.cmd_index].op == 4) {
        c.cmd_advance(unit_id, u.cmd_index);
    }
}

} // namespace detail

void unit_kneel_tick(int32_t unit_id) {
    tact_state st = state();
    detail::unit_kneel_tick(st.own, live_unit_kneel_tick_calls(), unit_id);
}

} // namespace mh::tact


namespace mh::tact {


} // namespace mh::tact
