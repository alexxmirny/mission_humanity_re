//
// tact/tact_ui_sel_panel_multi_mode_tick.cpp -- see tact_ui_sel_panel_multi_mode_tick.h. Translated
// from the DISASSEMBLY (tmp/decomp_tact/llm_tact_ui_sel_panel_multi_mode_tick_004361b5.asm).
//
#include "tact/tact_ui_sel_panel_multi_mode_tick.h"

#include "addr/mh_calls.gen.h" // frontier callees (Law 4), indirected via the _calls struct (Law 3b)
#include "state/host_api.h"
#include "state/host_events.h"
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "tact/tact_unit_enqueue_command.h"
#include "state/rebind_targets.gen.h"

namespace mh::tact {

const ui_sel_panel_multi_mode_tick_calls &live_ui_sel_panel_multi_mode_tick_calls() {
    static const ui_sel_panel_multi_mode_tick_calls c = {
        mh::state::evt::inv_tact_sel_panel_mode_tab,
        mh::state::evt::inv_tact_sel_panel_row_toggle,
        MH_LIBMH_BIND(llm_tact_selection_panel_refresh),
        MH_LIBMH_BIND(llm_tact_ui_mouse_in_rect),
        MH_LIBMH_BIND(llm_tact_squad_roster_refresh),
        MH_LIBMH_BIND(llm_tact_selection_clear_unless_ctrl),
        mh::state::evt::inv_tact_player_rows,
        MH_LIBMH_BIND(llm_tact_unit_enqueue_command),
        MH_LIBMH_BIND(time_GetCurrentTime),
        mh::state::evt::inv_tact_vis_map,
    };
    return c;
}

namespace detail {

void ui_sel_panel_multi_mode_tick(const tact_view &v, tact_store &own,
                                  const ui_sel_panel_multi_mode_tick_calls &c) {
    // GATE @0x004361cd-0x004361d4: a hit code left pending from a prior frame blocks the whole tick.
    if (own.sidebar_ui_hit_code() > 0) return;

    // ---- 1. MODE-TAB CLICK @0x004361da-0x0043627d --------------------------------------------
    const bool over_tab_hotzone = (*v.mouse_buttons_cur == 1) && (*v.sidebar_mouse_y > 0xa8) &&
                                  (*v.sidebar_mouse_y < 0xc0) && (*v.sidebar_mouse_x >= 0x50);
    if (over_tab_hotzone) {
        own.ui_sel_panel_multi_mode() = 1;

        // The blit moved host-side with the scope (R4): mh.dll's sink holds the icon slot,
        // the origin and the clip. Only what happened crosses.
        c.sel_panel_mode_tab(/*multi=*/1);
        c.selection_panel_refresh();
        own.sidebar_ui_hit_code() = 2;
        return;
    }

    // ---- 2. GROUP ASSIGN/RECALL @0x00436282-0x004363cd ----------------------------------------
    // Does NOT return on this path -- falls into 3 regardless of how the inner if/else resolves.
    if (c.ui_mouse_in_rect(0, 0xc0, 0xa0, 0xd8) == 1 && *v.mouse_buttons_cur > 0) {
        const int32_t group_row       = *v.sidebar_mouse_x / 0x14; // genuine IDIV, truncates toward 0
        own.sidebar_active_group_id() = group_row;

        if (*v.mouse_buttons_cur == 2) {
            // "assign to group": every owner-0, non-empty, currently-selected unit -> group_row.
            for (int32_t i = TACT_UNIT_FIRST_SLOT; i <= TACT_UNIT_LAST_SLOT; ++i) {
                tact_unit &u = own.unit_at(i);
                if (u.owner == 0 && u.type != 0 && (u.status & 1) != 0) {
                    u.squad_group_id = static_cast<uint8_t>(group_row);
                }
            }
            c.squad_roster_refresh();
        } else {
            // "recall group": clear/re-derive selection from every owner-0 unit in group_row. No
            // early break -- every matching unit gets selected.
            c.selection_clear_unless_ctrl();
            for (int32_t i = TACT_UNIT_FIRST_SLOT; i <= TACT_UNIT_LAST_SLOT; ++i) {
                tact_unit &u = own.unit_at(i);
                if (u.type != 0 && u.owner == 0 && static_cast<int32_t>(u.squad_group_id) == group_row) {
                    u.status |= 1;
                }
            }
        }
        c.selection_panel_refresh();
        c.ui_draw_player_row_list(group_row);
        own.sidebar_ui_hit_code() = 0xa;
    }

    // ---- 3. HIGHLIGHT-ON-HOVER @0x004363d7-0x00436445 -----------------------------------------
    if (*v.mouse_buttons_cur == 0 && c.ui_mouse_in_rect(0, 0xf0, 0xa0, own.window_height()) == 1) {
        const int32_t row       = (*v.sidebar_mouse_y - 0xf0) / 0x30; // genuine IDIV
        const int32_t candidate = own.sidebar_slot_unit_ids_at(row + own.sidebar_slot_scroll());
        if (candidate > 0) own.sidebar_highlighted_unit_id() = candidate;
    }

    // ---- 4. FOUR SCROLL-BUTTON HOT-ZONES @0x00436445-0x00436602 -------------------------------
    if (own.sel_panel_icon_slot_state_at(0) > 0 && c.ui_mouse_in_rect(0, 0xd8, 0x28, 0xf0) == 1 &&
        *v.mouse_buttons_cur > 0 && (own.sel_panel_icon_slot_state_at(0) & 1) == 0) {
        own.sel_panel_icon_slot_state_at(0) |= 1;
        own.sidebar_slot_scroll() = 0;
        c.selection_panel_refresh();
        own.sidebar_ui_hit_code() = 1;
    } else if (own.sel_panel_icon_slot_state_at(1) > 0 &&
               c.ui_mouse_in_rect(0x28, 0xd8, 0x50, 0xf0) == 1 && *v.mouse_buttons_cur > 0 &&
               (own.sel_panel_icon_slot_state_at(1) & 1) == 0) {
        own.sel_panel_icon_slot_state_at(1) |= 1;
        own.sidebar_slot_scroll() -= 1;
        c.selection_panel_refresh();
        own.sidebar_ui_hit_code() = 1;
    } else if (own.sel_panel_icon_slot_state_at(2) > 0 &&
               c.ui_mouse_in_rect(0x50, 0xd8, 0x78, 0xf0) == 1 && *v.mouse_buttons_cur > 0 &&
               (own.sel_panel_icon_slot_state_at(2) & 1) == 0) {
        own.sel_panel_icon_slot_state_at(2) |= 1;
        own.sidebar_slot_scroll() += 1;
        c.selection_panel_refresh();
        own.sidebar_ui_hit_code() = 1;
    } else if (own.sel_panel_icon_slot_state_at(3) > 0 &&
               c.ui_mouse_in_rect(0x78, 0xd8, 0xa0, 0xf0) == 1 && *v.mouse_buttons_cur > 0 &&
               (own.sel_panel_icon_slot_state_at(3) & 1) == 0) {
        own.sel_panel_icon_slot_state_at(3) |= 1;

        // "scroll to bottom": count every currently-selected unit (status bit 0), slots 1..0x80.
        int32_t selected_count = 0;
        for (int32_t i = TACT_UNIT_FIRST_SLOT; i <= TACT_UNIT_LAST_SLOT; ++i) {
            if ((own.unit_at(i).status & 1) == 1) ++selected_count;
        }
        // DECLARED NEED [1]: *v.sidebar_slot_visible_count does not exist yet -- see the header.
        own.sidebar_slot_scroll() = selected_count - *v.sidebar_slot_visible_count;
        c.selection_panel_refresh();
        own.sidebar_ui_hit_code() = 1;
    } else {
        // ---- 5. ROW-LIST CLICK @0x00436607-0x0043692e -------------------------------------
        if (*v.mouse_buttons_cur == 1 && c.ui_mouse_in_rect(0, 0xf0, 0xa0, own.window_height()) == 1) {
            const int32_t row      = (*v.sidebar_mouse_y - 0xf0) / 0x30; // genuine IDIV
            const int32_t unit_idx = own.sidebar_slot_unit_ids_at(row + own.sidebar_slot_scroll());
            if (unit_idx > 0) {
                if (c.ui_mouse_in_rect(0x84, row * 0x30 + 0xf7, 0x9a, row * 0x30 + 0x10d) == 1) {
                    // 5a. DEFENSE-STANCE TOGGLE -- RETURN.
                    // R4: the row -> y arithmetic is panel layout and stays with the sink.
                    c.sel_panel_row_toggle(/*defense=*/0, row);

                    tact_unit &u = own.unit_at(unit_idx);
                    if (u.def_stat == 4) {
                        u.def_stat = 0;
                    } else {
                        u.def_stat = static_cast<uint8_t>(u.def_stat + 1);
                    }
                    if (u.def_stat == 4) {
                        // op 4 = "kneel" per mh_tact_unit_record::op's doc comment; no Ghidra enum
                        // for the opcode domain yet (declared_needs).
                        c.unit_enqueue_command(unit_idx, 4, 0, 0, 0, 0, 0);
                    }
                    if (u.def_stat == 3) {
                        u.wander_check_time = c.time_get_current_time();
                    }
                    own.sidebar_ui_hit_code() = row + 100;

                    // Stamp 5 consecutive columns dirty on both row*2+11 and row*2+12, through the
                    // SAME _G_LLM_TILE_VIS_MAP array tile_vis_map_at() already binds -- the
                    // "_G_LLM_TILE_VIS_MAP_M6" auto-label the assembly touches is just that array's
                    // base minus 6 (see the header banner).
                    for (int32_t col = 0; col < 5; ++col) {
                        own.tile_vis_map_at((row * 2 + 11) * own.view_tiles_w() + col - 6) = 2;
                        own.tile_vis_map_at((row * 2 + 12) * own.view_tiles_w() + col - 6) = 2;
                    }
                    return;
                }

                if (c.ui_mouse_in_rect(0x44, row * 0x30 + 0x101, 0x5a, row * 0x30 + 0x117) == 1) {
                    // 5b. GUN TOGGLE -- RETURN.
                    c.sel_panel_row_toggle(/*gun=*/1, row);
                    own.unit_at(unit_idx).active_gun ^= 1;
                    own.sidebar_ui_hit_code() = row + 0x78;
                    return;
                }

                // 5c. CAMERA RECENTER -- falls through into 6, does NOT return.
                //
                // Both power-of-two divisions below are compiled as SAR/SBB (win_w) or SAR/ADD
                // (view_tiles_h) truncate-toward-zero idioms in the original; plain C++ `/`
                // reproduces them exactly (see the header banner).
                const tact_unit &u = own.unit_at(unit_idx);

                own.map_cam_col() = static_cast<int32_t>(u.pos_col) - *v.win_w / 0x40;
                if (own.map_cam_col() < 0) own.map_cam_col() = 0;
                if (own.map_cam_col() - *v.win_w / 0x20 > *v.map_width) {
                    own.map_cam_col() = *v.map_width - *v.win_w / 0x20;
                }

                own.map_cam_row() = static_cast<int32_t>(u.pos_row) - own.view_tiles_h() / 2;
                if (own.map_cam_row() < 0) own.map_cam_row() = 0;
                if (own.map_cam_row() - own.view_tiles_h() > *v.map_height) {
                    own.map_cam_row() = *v.map_height - own.view_tiles_h();
                }

                c.vis_map_fill_default();
                own.sidebar_ui_hit_code() = 1;
            }
        }
        // Every path through 5 (gate failed, unit_idx<=0, or the 5c fallthrough) lands here alike.

        // ---- 6. MIDDLE-CLICK DESELECT @0x0043692e-0x004369b7 ---------------------------------
        if (*v.mouse_buttons_cur == 2 && c.ui_mouse_in_rect(0, 0xf0, 0xa0, own.window_height()) == 1) {
            const int32_t row      = (*v.sidebar_mouse_y - 0xf0) / 0x30; // genuine IDIV
            const int32_t unit_idx = own.sidebar_slot_unit_ids_at(row + own.sidebar_slot_scroll());
            if (unit_idx > 0) {
                own.unit_at(unit_idx).status &= 0xfe;
                c.selection_panel_refresh();
                own.sidebar_ui_hit_code() = 1;
            }
        }
    }
}

} // namespace detail

void ui_sel_panel_multi_mode_tick() {
    tact_state st = state();
    detail::ui_sel_panel_multi_mode_tick(st.read, st.own, live_ui_sel_panel_multi_mode_tick_calls());
}


} // namespace mh::tact
