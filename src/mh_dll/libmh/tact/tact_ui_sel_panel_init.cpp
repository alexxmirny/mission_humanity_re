//
// tact/tact_ui_sel_panel_init.cpp -- see tact_ui_sel_panel_init.h. Translated from the DISASSEMBLY
// (tmp/decomp_tact/llm_tact_ui_sel_panel_init_00433e94.asm), not from a Ghidra `.c` draft.
//
#include "tact/tact_ui_sel_panel_init.h"

#include "addr/mh_calls.gen.h" // frontier callees (Law 4), indirected via the _calls struct (Law 3b)
#include "state/host_api.h"
#include "state/host_events.h"
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"
#include "crt/crt_select.h" // LIB-CRT: MH_CRT() picks the vendored CRT in the standalone build

namespace mh::tact {

const ui_sel_panel_init_calls &live_ui_sel_panel_init_calls() {
    static const ui_sel_panel_init_calls c = {
        mh::state::evt::inv_tact_sel_panel_init,
        MH_LIBMH_BIND(llm_tact_selection_panel_refresh),
        mh::state::evt::inv_tact_sel_panel,
        mh::state::evt::inv_tact_player_rows,
    };
    return c;
}

namespace detail {

void ui_sel_panel_init(const tact_view &v, tact_store &own, const ui_sel_panel_init_calls &c) {
    (void)v;
    // @0x00433eac-0x00434029 -- THE HEAD IS THE HOST'S (LIFT-TACT slice A). The icon bank's load
    // and 565->555 conversion, the three fixed background panels, both font selections and both
    // labels are one scope now, LIBMH_EVK_INV_TACT_SEL_PANEL_INIT; mh.dll's sink holds every
    // literal and reproduces the sequence at this instant.
    //
    // WHY THE SPLIT LINE IS HERE and not one statement later or earlier. Everything above it is
    // presentation whose only libmh-visible product was the icon-pointer array, and that array's
    // every consumer blitted the pointer or read its (w,h) header as the clip for that same blit
    // -- never for a decision -- so it goes host-side with the draws (docs/libmh-abi.md section 4;
    // this is the deferral being discharged, not deferred again). Everything below it is state
    // libmh owns: the multi-mode latch it reads every tick, and a tail that re-enters libmh's own
    // converted bodies.
    c.sel_panel_init_draw();

    // @0x00434029: reset multi-select mode. STAYS -- llm_tact_sidebar_dispatch and both mode ticks
    // read this latch, so it is libmh's state, not a draw.
    own.ui_sel_panel_multi_mode() = 0;

    // @0x00434033-0x00434047: refresh, redraw, and clear the player-row selection (-1 = none).
    c.selection_panel_refresh();
    c.sel_panel_draw();
    c.draw_player_row_list(-1);
}

} // namespace detail

void ui_sel_panel_init() {
    tact_state st = state();
    detail::ui_sel_panel_init(st.read, st.own, live_ui_sel_panel_init_calls());
}


} // namespace mh::tact
