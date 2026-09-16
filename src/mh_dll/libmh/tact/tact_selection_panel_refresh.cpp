//
// tact/tact_selection_panel_refresh.cpp -- see tact_selection_panel_refresh.h. Translated from the
// DISASSEMBLY (tmp/decomp_tact/llm_tact_selection_panel_refresh_00434af7.asm), not from Ghidra's .c.
//
#include "tact/tact_selection_panel_refresh.h"

#include "addr/mh_calls.gen.h" // frontier callees (Law 4)
#include "state/host_api.h"
#include "state/host_events.h"
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::tact {

const selection_panel_refresh_calls &live_selection_panel_refresh_calls() {
    static const selection_panel_refresh_calls c = {
        mh::state::evt::inv_tact_sel_panel_bg,
        MH_LIBMH_BIND(llm_tact_squad_roster_refresh),
        mh::state::evt::inv_tact_sidebar_roster,
        mh::state::evt::inv_tact_sidebar_rows,
        mh::state::evt::inv_tact_sel_panel,
        MH_LIBMH_BIND(llm_tact_active_unit_count_hud_draw),
    };
    return c;
}

namespace detail {

// ---- inv 6, the R3b hoist (LIFT-R3B found it, LIFT-TACT owns the fix) ---------------------------
//
// llm_tact_ui_sidebar_roster_refresh @0x00434b68 is a textbook R2 split that shipped unsplit: three
// STATE steps and then a redraw loop. The state half is what libmh reads back -- llm_tact_sidebar_dispatch
// reads sidebar_slot_unit_ids_at() at tact_sidebar_dispatch.cpp:144/153 (step 15 of the same frame,
// against this emit at step 6/9) and hands the id to ui_char_panel_row_draw, which writes hashed
// tact_units.status; it also reads sel_panel_icon_slot_state_at() at :130. Behind the notify channel
// that is a poll-mode divergence into HASHED state, which is why LIFT-R3B graded it SEVERE and the
// only one of its eight findings that needed a fix rather than a reason.
//
// So libmh computes the three state steps ITSELF, above the emit, and the scope keeps only the draw.
// The double write is idempotent by construction, not by luck: steps 1-3 are a pure function of
// tact_units + sidebar_slot_visible_count, and nothing between this call and the host's execution of
// the original touches either -- so when the hosted arm runs the whole original it recomputes the
// same values before drawing. Transcribed from the ORIGINAL's order, including the two quirks worth
// not tidying: the scroll clamp's assignment happens INSIDE the first condition (Ghidra renders it
// as a comma expression at 0x00434bd6), and the down-arrow test indexes
// sidebar_slot_unit_ids[visible + scroll] -- one PAST the last visible slot, which is the point.
void roster_slots_rebuild(const tact_view &v, tact_store &own) {
    // @0x00434b7c-0x00434bb2: rebuild the visible-slot list from live units.
    int32_t count = 0;
    for (int32_t i = 1; i < 0x81; ++i) {
        if ((own.unit_at(i).status & 1) == 1 && own.unit_at(i).type != 0) {
            own.sidebar_slot_unit_ids_at(count) = i;
            ++count;
        }
    }
    // @0x00434bb2-0x00434bc9: zero-fill the tail of the 0x40-slot array.
    for (int32_t i = count; i < 0x40; ++i) own.sidebar_slot_unit_ids_at(i) = 0;

    // @0x00434bc9-0x00434bf5: clamp the scroll offset, then floor it at 0.
    const int32_t visible = *v.sidebar_slot_visible_count;
    if (count < own.sidebar_slot_scroll() + visible) {
        own.sidebar_slot_scroll() = count - visible;
        if (own.sidebar_slot_scroll() < 0) own.sidebar_slot_scroll() = 0;
    }

    // @0x00434bf5-0x00434c3d: the UP-arrow pair -- bit 1 set means "can scroll up".
    if (own.sidebar_slot_scroll() < 1) {
        own.sel_panel_icon_slot_state_at(0) &= 1;
        own.sel_panel_icon_slot_state_at(1) &= 1;
    } else {
        own.sel_panel_icon_slot_state_at(0) |= 2;
        own.sel_panel_icon_slot_state_at(1) |= 2;
    }

    // @0x00434c3d-0x00434c8d: the DOWN-arrow pair, gated on the slot one past the last visible one.
    if (own.sidebar_slot_unit_ids_at(visible + own.sidebar_slot_scroll()) < 1) {
        own.sel_panel_icon_slot_state_at(2) &= 1;
        own.sel_panel_icon_slot_state_at(3) &= 1;
    } else {
        own.sel_panel_icon_slot_state_at(2) |= 2;
        own.sel_panel_icon_slot_state_at(3) |= 2;
    }
}


void selection_panel_refresh(const tact_view &v, tact_store &own,
                             const selection_panel_refresh_calls &c) {
    // @0x00434b0f-0x00434b35: the background plate. Its surface/pitch/clip literals and the
    // sel_panel_icon_gfx_at(1) raw pointer moved host-side with the scope (R4) --
    // seams/host_event_sink.cpp draw_sel_panel_bg holds the verbatim call.
    c.sel_panel_bg();

    // @0x00434b3a: unconditional.
    c.squad_roster_refresh();

    // @0x00434b3f-0x00434b54.
    if (own.ui_sel_panel_multi_mode() == 0) {
        // inv 6: the state half runs HERE, before the emit -- see roster_slots_rebuild above.
        roster_slots_rebuild(v, own);
        c.ui_sidebar_roster_refresh();
    } else {
        c.ui_sidebar_draw_rows();
    }

    // @0x00434b54-0x00434b63: both unconditional on this (gate-independent) path.
    c.ui_sel_panel_draw();
    c.active_unit_count_hud_draw();
}

} // namespace detail

void selection_panel_refresh() {
    tact_state st = state();
    detail::selection_panel_refresh(st.read, st.own, live_selection_panel_refresh_calls());
}


} // namespace mh::tact
