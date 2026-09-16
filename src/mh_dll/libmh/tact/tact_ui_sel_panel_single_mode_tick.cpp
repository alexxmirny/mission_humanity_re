//
// tact/tact_ui_sel_panel_single_mode_tick.cpp -- see tact_ui_sel_panel_single_mode_tick.h. Translated
// from the DISASSEMBLY, not from Ghidra's C.
//
#include "tact/tact_ui_sel_panel_single_mode_tick.h"

#include <cstring>

#include "addr/mh_calls.gen.h" // frontier callees (Law 4)
#include "state/host_api.h"
#include "state/host_events.h"
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::tact {

const ui_sel_panel_single_mode_tick_calls &live_ui_sel_panel_single_mode_tick_calls() {
    static const ui_sel_panel_single_mode_tick_calls c = {
        mh::state::evt::inv_tact_sel_panel_mode_tab,
        MH_LIBMH_BIND(llm_tact_selection_panel_refresh),
        MH_LIBMH_BIND(llm_tact_ui_mouse_in_rect),
        mh::state::evt::inv_tact_player_rows,
        mh::state::evt::inv_tact_sidebar_row_unassigned,
        mh::state::evt::inv_tact_sidebar_row_group,
    };
    return c;
}

namespace detail {

void ui_sel_panel_single_mode_tick(const tact_view &v, tact_store &own,
                                   const ui_sel_panel_single_mode_tick_calls &c) {
    // @0x004369d9-0x004369e0: no-op while a sidebar hit is already pending.
    if (own.sidebar_ui_hit_code() > 0) return;

    // @0x004369e6-0x00436a38: the "mode-tab" icon strip click.
    const bool mode_tab_hit = (*v.mouse_buttons_cur == 1) && (*v.sidebar_mouse_y > 0xa8) &&
                              (*v.sidebar_mouse_x < 0x50) && (*v.sidebar_mouse_y < 0xc0);
    if (mode_tab_hit) {
        // @0x00436a3c-0x00436a89.
        own.ui_sel_panel_multi_mode() = 0;
        // The blit moved host-side with the scope (R4): mh.dll's sink holds the icon slot,
        // the origin and the clip. Only what happened crosses.
        c.sel_panel_mode_tab(/*single=*/0);
        c.selection_panel_refresh();
        own.sidebar_ui_hit_code() = 2;
        return;
    }

    // @0x00436a8e-0x00436aeb: the player-row-list strip click.
    if (c.mouse_in_rect(0, 0xc0, 0xa0, 0xd8) == 1 && *v.mouse_buttons_cur > 0) {
        own.sidebar_active_group_id() = *v.sidebar_mouse_x / 0x14;
        c.draw_player_row_list(own.sidebar_active_group_id());
        c.selection_panel_refresh();
        own.sidebar_ui_hit_code() = 10;
        return;
    }

    // @0x00436af0-0x00436b4f: scroll button 0 (UNASSIGNED list, reset to top).
    if (own.sidebar_scrollbtn_state_at(0) > 0 && c.mouse_in_rect(0x00, 0xd8, 0x14, 0xf0) == 1 &&
        *v.mouse_buttons_cur > 0 && (own.sidebar_scrollbtn_state_at(0) & 1) == 0) {
        own.sidebar_scrollbtn_state_at(0) |= 1;
        own.sidebar_unassigned_scroll_row() = 0;
        c.selection_panel_refresh();
        own.sidebar_ui_hit_code() = 1;
        return;
    }

    // @0x00436b4f-0x00436bad: scroll button 1 (UNASSIGNED list, scroll up one).
    if (own.sidebar_scrollbtn_state_at(1) > 0 && c.mouse_in_rect(0x14, 0xd8, 0x28, 0xf0) == 1 &&
        *v.mouse_buttons_cur > 0 && (own.sidebar_scrollbtn_state_at(1) & 1) == 0) {
        own.sidebar_scrollbtn_state_at(1) |= 1;
        own.sidebar_unassigned_scroll_row() -= 1;
        c.selection_panel_refresh();
        own.sidebar_ui_hit_code() = 1;
        return;
    }

    // @0x00436bad-0x00436c0b: scroll button 2 (UNASSIGNED list, scroll down one).
    if (own.sidebar_scrollbtn_state_at(2) > 0 && c.mouse_in_rect(0x28, 0xd8, 0x3c, 0xf0) == 1 &&
        *v.mouse_buttons_cur > 0 && (own.sidebar_scrollbtn_state_at(2) & 1) == 0) {
        own.sidebar_scrollbtn_state_at(2) |= 1;
        own.sidebar_unassigned_scroll_row() += 1;
        c.selection_panel_refresh();
        own.sidebar_ui_hit_code() = 1;
        return;
    }

    // @0x00436c0b-0x00436c73: scroll button 3 (UNASSIGNED list, jump to bottom).
    if (own.sidebar_scrollbtn_state_at(3) > 0 && c.mouse_in_rect(0x3c, 0xd8, 0x50, 0xf0) == 1 &&
        *v.mouse_buttons_cur > 0 && (own.sidebar_scrollbtn_state_at(3) & 1) == 0) {
        own.sidebar_scrollbtn_state_at(3) |= 1;
        int32_t unassigned_count = 0;
        std::memcpy(&unassigned_count, v.unassigned_unit_roster + 0, sizeof(unassigned_count));
        own.sidebar_unassigned_scroll_row() =
            unassigned_count - *v.sidebar_multi_panel_visible_rows;
        c.selection_panel_refresh();
        own.sidebar_ui_hit_code() = 1;
        return;
    }

    // @0x00436c73-0x00436cd5: scroll button 4 (ACTIVE-GROUP list, reset to top).
    if (own.sidebar_scrollbtn_state_at(4) > 0 && c.mouse_in_rect(0x50, 0xd8, 0x64, 0xf0) == 1 &&
        *v.mouse_buttons_cur > 0 && (own.sidebar_scrollbtn_state_at(4) & 1) == 0) {
        own.sidebar_scrollbtn_state_at(4) |= 1;
        own.sidebar_group_scroll_row() = 0;
        c.selection_panel_refresh();
        own.sidebar_ui_hit_code() = 1;
        return;
    }

    // @0x00436cd5-0x00436d33: scroll button 5 (ACTIVE-GROUP list, scroll up one).
    if (own.sidebar_scrollbtn_state_at(5) > 0 && c.mouse_in_rect(0x64, 0xd8, 0x78, 0xf0) == 1 &&
        *v.mouse_buttons_cur > 0 && (own.sidebar_scrollbtn_state_at(5) & 1) == 0) {
        own.sidebar_scrollbtn_state_at(5) |= 1;
        own.sidebar_group_scroll_row() -= 1;
        c.selection_panel_refresh();
        own.sidebar_ui_hit_code() = 1;
        return;
    }

    // @0x00436d33-0x00436d91: scroll button 6 (ACTIVE-GROUP list, scroll down one).
    if (own.sidebar_scrollbtn_state_at(6) > 0 && c.mouse_in_rect(0x78, 0xd8, 0x8c, 0xf0) == 1 &&
        *v.mouse_buttons_cur > 0 && (own.sidebar_scrollbtn_state_at(6) & 1) == 0) {
        own.sidebar_scrollbtn_state_at(6) |= 1;
        own.sidebar_group_scroll_row() += 1;
        c.selection_panel_refresh();
        own.sidebar_ui_hit_code() = 1;
        return;
    }

    // @0x00436d91-0x00436e02: scroll button 7 (ACTIVE-GROUP list, jump to bottom).
    if (own.sidebar_scrollbtn_state_at(7) > 0 && c.mouse_in_rect(0x8c, 0xd8, 0xa0, 0xf0) == 1 &&
        *v.mouse_buttons_cur > 0 && (own.sidebar_scrollbtn_state_at(7) & 1) == 0) {
        own.sidebar_scrollbtn_state_at(7) |= 1;
        // GROUP_UNIT_ROSTER[active_group_id].count -- row offset per the header's own note on the
        // unsigned-shift idiom.
        const int32_t group_row_off =
            static_cast<int32_t>(static_cast<uint32_t>(own.sidebar_active_group_id()) << 8);
        int32_t group_count = 0;
        std::memcpy(&group_count, v.group_unit_roster + group_row_off, sizeof(group_count));
        own.sidebar_group_scroll_row() = group_count - *v.sidebar_multi_panel_visible_rows;
        c.selection_panel_refresh();
        own.sidebar_ui_hit_code() = 1;
        return;
    }

    // @0x00436e02-0x00436e87: LEFT (UNASSIGNED list) row hover/draw. NOT an early return -- see the
    // header banner: this and the RIGHT check below both run unconditionally once every button
    // check above has fallen through.
    if (*v.mouse_buttons_cur == 1 &&
        c.mouse_in_rect(0, 0xf0, 0x50, own.window_height()) == 1) {
        const int32_t row              = (*v.sidebar_mouse_y - 0xf0) / 0x18;
        int32_t       unassigned_count = 0;
        std::memcpy(&unassigned_count, v.unassigned_unit_roster + 0, sizeof(unassigned_count));
        if (own.sidebar_unassigned_scroll_row() + row + 1 <= unassigned_count) {
            int32_t unit_id = 0;
            std::memcpy(&unit_id,
                        v.unassigned_unit_roster + 4 +
                            (row + own.sidebar_unassigned_scroll_row()) * 4,
                        sizeof(unit_id));
            c.sidebar_row_hover_unassigned(unit_id, row);
            own.sidebar_ui_hit_code() = row + own.sidebar_unassigned_scroll_row() + 0x15;
        }
    }

    // @0x00436e87-0x00436f23: RIGHT (ACTIVE-GROUP list) row hover/draw.
    if (*v.mouse_buttons_cur == 1 &&
        c.mouse_in_rect(0x50, 0xf0, 0xa0, own.window_height()) == 1) {
        const int32_t row = (*v.sidebar_mouse_y - 0xf0) / 0x18;
        const int32_t group_row_off =
            static_cast<int32_t>(static_cast<uint32_t>(own.sidebar_active_group_id()) << 8);
        int32_t group_count = 0;
        std::memcpy(&group_count, v.group_unit_roster + group_row_off, sizeof(group_count));
        if (own.sidebar_group_scroll_row() + row + 1 <= group_count) {
            int32_t unit_id = 0;
            std::memcpy(&unit_id,
                        v.group_unit_roster + group_row_off + 4 +
                            (row + own.sidebar_group_scroll_row()) * 4,
                        sizeof(unit_id));
            c.sidebar_row_hover_group(unit_id, row);
            own.sidebar_ui_hit_code() = row + own.sidebar_group_scroll_row() + 0x29;
        }
    }
}

} // namespace detail

void ui_sel_panel_single_mode_tick() {
    tact_state st = state();
    detail::ui_sel_panel_single_mode_tick(st.read, st.own, live_ui_sel_panel_single_mode_tick_calls());
}


} // namespace mh::tact
