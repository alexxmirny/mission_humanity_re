//
// tact/tact_select_next_unit.cpp -- see tact_select_next_unit.h. Translated from the DISASSEMBLY,
// not from Ghidra's C.
//
#include "tact/tact_select_next_unit.h"

#include "addr/mh_calls.gen.h"  // frontier callees (Law 4): llm_tact_selection_panel_refresh
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::tact {

const select_next_unit_calls &live_select_next_unit_calls() {
    static const select_next_unit_calls c = {
        MH_LIBMH_BIND(llm_tact_selection_panel_refresh),
    };
    return c;
}

namespace detail {

void select_next_unit(tact_store &own, const select_next_unit_calls &c) {
    // Both locals are initialised before the gate below is checked (@0x0042edd0-0x0042edd7), and
    // `target` keeps this value if loop B below never finds a real candidate -- see the header's
    // PRESERVE-BUG note.
    int32_t candidate = TACT_UNIT_FIRST_SLOT; // [EBP-0x1c]
    int32_t target    = TACT_UNIT_FIRST_SLOT; // [EBP-0x18]

    // @0x0042edde-0x0042ede9: the whole function is a no-op unless the roster actually changed
    // since the last call. NOT the same globals as tact_store::unit_active_count() -- see the
    // header banner.
    if (own.active_unit_count() == own.active_unit_count_cached()) return;

    // LOOP A (@0x0042edef-0x0042ee36): last unit with (status&1)==1 && owner==0, no early break.
    for (int32_t i = TACT_UNIT_FIRST_SLOT; i <= TACT_UNIT_LAST_SLOT; ++i) {
        const tact_unit &u = own.unit_at(i);
        if ((u.status & 1) == 1 && u.owner == 0) {
            candidate = i; // keep overwriting -- the LAST match wins, not the first.
        }
    }

    // LOOP B (@0x0042ee36-0x0042eea1): walk from candidate+1, wrapping on an empty (type==0) slot,
    // budget-limited to TACT_UNIT_LAST_SLOT+1 (0x81) consuming steps. Unchecked indexing: `i` can
    // legitimately reach TACT_UNIT_SLOTS (129) here -- one past the real array -- reproducing the
    // original's own OOB read (see header banner).
    int32_t i      = candidate + 1;
    int32_t budget = 0;
    while (budget <= TACT_UNIT_LAST_SLOT) {
        tact_unit &u = own.unit_at(i);
        if (u.type == 0) {
            // Stepped past the populated array: wrap to slot 1 WITHOUT consuming a budget step.
            i = TACT_UNIT_FIRST_SLOT;
            continue;
        }
        if ((u.status & 1) == 1) {
            // Already selected -- skip, fall through to advance.
        } else if (u.owner == 0) {
            target = i; // found it -- break out entirely, no further advance.
            break;
        }
        ++i;
        ++budget;
    }

    // LOOP C (@0x0042eea1-0x0042eeeb): unconditionally clear the selection bit on every currently-
    // selected unit, regardless of how loop B ended.
    for (int32_t j = TACT_UNIT_FIRST_SLOT; j <= TACT_UNIT_LAST_SLOT; ++j) {
        tact_unit &u = own.unit_at(j);
        if ((u.status & 1) == 1) {
            u.status &= 0xfe;
        }
    }

    // @0x0042eeed-0x0042ef04: stamp `target` unconditionally -- even if loop B never found a real
    // candidate and `target` is still its initial value of TACT_UNIT_FIRST_SLOT. PRESERVE-BUG: do
    // not add a found-a-valid-unit guard the original lacks.
    own.unit_at(target).status |= 1;

    // @0x0042ef0a: unconditional on this (gate-passed) path.
    c.selection_panel_refresh();
}

} // namespace detail

void select_next_unit() {
    tact_state st = state();
    detail::select_next_unit(st.own, live_select_next_unit_calls());
}


} // namespace mh::tact
