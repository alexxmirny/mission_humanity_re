//
// tact/tact_sidebar_dispatch.cpp -- see tact_sidebar_dispatch.h. Translated from the DISASSEMBLY
// (tmp/decomp_tact/llm_tact_sidebar_dispatch_00435972.asm), not from Ghidra's .c.
//
#include "tact/tact_sidebar_dispatch.h"

#include <cstring>

#include "addr/mh_calls.gen.h" // frontier callees (Law 4)
#include "state/host_api.h"
#include "state/host_events.h"
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::tact {

const sidebar_dispatch_calls &live_sidebar_dispatch_calls() {
    static const sidebar_dispatch_calls c = {
        MH_LIBMH_BIND(llm_tact_ui_order_buttons_minimap_tick),
        MH_LIBMH_BIND(llm_tact_ui_sel_panel_multi_mode_tick),
        MH_LIBMH_BIND(llm_tact_ui_sel_panel_single_mode_tick),
        mh::state::evt::inv_tact_group_panel,
        mh::state::evt::inv_tact_player_rows,
        MH_LIBMH_BIND(llm_tact_selection_panel_refresh),
        mh::state::evt::inv_tact_sel_panel,
        mh::state::evt::inv_tact_char_panel_row_draw,
    };
    return c;
}

namespace detail {

void sidebar_dispatch(const tact_view &v, tact_store &own, const sidebar_dispatch_calls &c) {
    // @0x0043598a-0x004359e3: this whole block is SKIPPED (no mouse_x/y write, no per-mode tick)
    // when the cursor is left of the sidebar (cursor_x < win_w).
    if (*v.cursor_x >= *v.win_w) {
        // @0x00435997-0x004359b6.
        own.sidebar_mouse_x()             = *v.cursor_x - *v.win_w;
        own.sidebar_mouse_y()             = *v.cursor_y;
        own.sidebar_highlighted_unit_id() = 0;

        // @0x004359bb-0x004359e3: exactly one per-mode tick, keyed on mouse Y vs the 0xa8 minimap
        // boundary and (above it) on the multi/single select-panel mode.
        if (own.sidebar_mouse_y() < 0xa8) {
            c.minimap_tick();
        } else if (own.ui_sel_panel_multi_mode() == 0) {
            c.multi_mode_tick();
        } else {
            c.single_mode_tick();
        }
    }

    int32_t &hit_code = own.sidebar_ui_hit_code();

    // @0x004359e3-0x004359fa: only dispatch a pending hit on button RELEASE (hit_code>0 recorded by
    // a frontier hit-tester, and the mouse button that produced it already back up).
    if (hit_code <= 0 || *v.mouse_buttons_cur != 0) return;

    // @0x00435a01-0x00435a0d: hit code 2 is consumed here with no further action -- clearing it
    // to 0 does not exit early; execution falls straight into the range checks below, where 0
    // never matches any of them.
    if (hit_code == 2) hit_code = 0;

    if (hit_code >= 0x3c && hit_code < 0x46) {
        // @0x00435a21-0x00435a80: redraw the group-assign icon panel, then stamp the three
        // TILE_VIS_MAP cells immediately covering it as dirty (see this TU's header banner --
        // these three writes are three CONSECUTIVE tile_vis_map_at() cells, not independent
        // globals).
        // The blit moved host-side with the scope (R4): mh.dll's sink holds the icon slot,
        // the origin and the clip. Only what happened crosses.
        c.group_panel();
        const int32_t vis_base            = own.view_tiles_w() * 7 - 6;
        own.tile_vis_map_at(vis_base + 0) = 2;
        own.tile_vis_map_at(vis_base + 1) = 2;
        own.tile_vis_map_at(vis_base + 2) = 2;
        hit_code                          = 0;
        return;
    }

    if (hit_code == 0xa) {
        // @0x00435a8e-0x00435a98.
        c.draw_player_row_list(-1);
        hit_code = 0;
        return;
    }

    if (hit_code >= 0x14 && hit_code < 0x3c) {
        // @0x00435abb-0x00435b21: assign (< 0x28, via the icon-slot LUT) or unassign (>= 0x28, via
        // the roster table) one unit's squad_group_id, then refresh. See this TU's header banner
        // for the full addressing derivation -- both branches read a raw little-endian int32 unit
        // index from a byte-addressed base that is NOT bounds-matched to the properly-typed array
        // it starts from.
        if (hit_code < 0x28) {
            // @0x00435ac4-0x00435adc.
            int32_t unit_idx = 0;
            std::memcpy(&unit_idx, v.sidebar_icon_slot_unit_lut + hit_code * 4, sizeof(unit_idx));
            own.unit_at(unit_idx).squad_group_id = static_cast<uint8_t>(own.sidebar_active_group_id());
        } else {
            // @0x00435ae4-0x00435b01. The (active_group_id << 8) row term is computed as an
            // UNSIGNED shift here so a sentinel active_group_id == -1 reproduces the original
            // SHL's bit pattern (0xffffff00, i.e. "back up one 256-byte row") rather than
            // triggering C++'s undefined negative-left-shift behaviour.
            const int32_t group_row_bytes =
                static_cast<int32_t>(static_cast<uint32_t>(own.sidebar_active_group_id()) << 8);
            const int32_t byte_off = hit_code * 4 + group_row_bytes + 0x60;
            int32_t       unit_idx = 0;
            std::memcpy(&unit_idx, v.unassigned_unit_roster + byte_off, sizeof(unit_idx));
            own.unit_at(unit_idx).squad_group_id = 0xff;
        }
        c.selection_panel_refresh();
        c.draw_player_row_list(-1);
        hit_code = 0;
        return;
    }

    // @0x00435b26: everything else (hit_code < 0x14, or hit_code >= 0x3c and not already handled
    // above) lands here, split by select-panel mode.
    if (own.ui_sel_panel_multi_mode() == 0) {
        if (hit_code == 1) {
            // @0x00435b40-0x00435b9e: toggle bit 1 of every non-negative sel_panel_icon_slot_state
            // slot (4 of them); "(slot | 1) > 0" reproduces the original's byte-OR-then-full-EAX
            // sign test bit-for-bit (ORing bit 0 only ever changes the low byte, so it is
            // equivalent to ORing the full register).
            bool changed = false;
            for (int32_t i = 0; i < 4; ++i) {
                const int32_t slot = own.sel_panel_icon_slot_state_at(i);
                if ((slot | 1) > 0) {
                    own.sel_panel_icon_slot_state_at(i) = slot & 2;
                    changed                             = true;
                    hit_code                            = 0;
                }
            }
            if (changed) c.sel_panel_draw();
            return;
        }

        if (hit_code >= 0x64 && hit_code < 0x78) {
            // @0x00435bc2-0x00435bf9.
            const int32_t unit_idx =
                own.sidebar_slot_unit_ids_at((hit_code - 0x64) + own.sidebar_slot_scroll());
            if (unit_idx > 0) c.char_panel_row_draw(unit_idx, hit_code - 0x64);
            hit_code = 0;
            return;
        }

        if (hit_code >= 0x78 && hit_code < 0x8c) {
            // @0x00435c1f-0x00435c56.
            const int32_t unit_idx =
                own.sidebar_slot_unit_ids_at((hit_code - 0x78) + own.sidebar_slot_scroll());
            if (unit_idx > 0) c.char_panel_row_draw(unit_idx, hit_code - 0x78);
            hit_code = 0;
            return;
        }

        // @0x00435c08-0x00435c1d / 0x00435c65: no other hit code does anything in single mode --
        // hit_code is left untouched (NOT cleared).
        return;
    }

    // @0x00435c6a-0x00435cdb: multi-select mode. Any hit code other than 1 is a no-op (hit_code
    // left untouched).
    if (hit_code != 1) return;

    // @0x00435c77-0x00435cd5: the same toggle as the single-mode ==1 case above, but over
    // sidebar_scrollbtn_state (8 slots).
    bool changed = false;
    for (int32_t i = 0; i < 8; ++i) {
        const int32_t slot = own.sidebar_scrollbtn_state_at(i);
        if ((slot | 1) > 0) {
            own.sidebar_scrollbtn_state_at(i) = slot & 2;
            changed                           = true;
            hit_code                          = 0;
        }
    }
    if (changed) c.sel_panel_draw();
}

} // namespace detail

void sidebar_dispatch() {
    tact_state st = state();
    detail::sidebar_dispatch(st.read, st.own, live_sidebar_dispatch_calls());
}


} // namespace mh::tact
