//
// tact/tact_ui_order_buttons_minimap_tick.cpp -- see tact_ui_order_buttons_minimap_tick.h.
// Translated from the DISASSEMBLY, not from Ghidra's C.
//
#include "tact/tact_ui_order_buttons_minimap_tick.h"

#include "addr/mh_calls.gen.h"           // frontier callees (Law 4): mouse_in_rect,
                                         // move_path_preview_clear, vis_map_fill_default
#include "tact/tact_group_issue_order.h" // sibling migration member; own public wrapper, called
#include "state/host_api.h"
#include "state/host_events.h"
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"
// directly per the translator brief's 3b same-set exception
// (NOT through the calls-struct).

namespace mh::tact {

const ui_order_buttons_minimap_tick_calls &live_ui_order_buttons_minimap_tick_calls() {
    static const ui_order_buttons_minimap_tick_calls c = {
        MH_LIBMH_BIND(llm_tact_ui_mouse_in_rect),
        mh::state::evt::inv_tact_order_button,
        MH_LIBMH_BIND(llm_tact_move_path_preview_clear),
        mh::state::evt::inv_tact_vis_map,
    };
    return c;
}

namespace detail {

void ui_order_buttons_minimap_tick(const tact_view &v, tact_store &own,
                                   const ui_order_buttons_minimap_tick_calls &c) {
    // @0x00435d02-0x00435d09: no-op once a sidebar hit is already pending.
    if (own.sidebar_ui_hit_code() > 0) return;

    // ---- button 1: rect (1,0x8e)-(0x15,0xa2), icon slot 0x23 (35), order 0x46 (CLEAR) -----------
    // @0x00435d0f-0x00435dc3.
    if (c.mouse_in_rect(1, 0x8e, 0x15, 0xa2) == 1 && *v.mouse_buttons_cur > 0 &&
        own.sidebar_ui_hit_code() == 0) {
        // The blit moved host-side with the scope (R4): mh.dll's sink holds the
        // icon slot 0x23 and the (1, 0x8f) origin. Only the identity crosses.
        c.order_button(0);
        const int32_t vis_base            = own.view_tiles_w() * 7 - 6; // see header banner
        own.tile_vis_map_at(vis_base + 0) = 2;
        own.tile_vis_map_at(vis_base + 1) = 2;
        own.tile_vis_map_at(vis_base + 2) = 2;
        mh::tact::group_issue_order(0x46, 0, 0, 0, 0);
        own.sidebar_ui_hit_code() = 0x3c;
        return;
    }

    // ---- button 2: rect (0x14,0x8f)-(0x25,0xa4), icon slot 0x24 (36), orders 0x46, 0x40 ----------
    // @0x00435dc8-0x00435e8e.
    if (c.mouse_in_rect(0x14, 0x8f, 0x25, 0xa4) == 1 && *v.mouse_buttons_cur > 0 &&
        own.sidebar_ui_hit_code() == 0) {
        // The blit moved host-side with the scope (R4): mh.dll's sink holds the
        // icon slot 0x24 and the (0x14, 0x8f) origin. Only the identity crosses.
        c.order_button(1);
        const int32_t vis_base            = own.view_tiles_w() * 7 - 6;
        own.tile_vis_map_at(vis_base + 0) = 2;
        own.tile_vis_map_at(vis_base + 1) = 2;
        own.tile_vis_map_at(vis_base + 2) = 2;
        mh::tact::group_issue_order(0x46, 0, 0, 0, 0);
        mh::tact::group_issue_order(0x40, 0, 0, 0, 0);
        own.sidebar_ui_hit_code() = 0x3d;
        return;
    }

    // ---- button 3: rect (0x25,0x8f)-(0x36,0xa4), icon slot 0x28 (40), orders 0x47, 0x47 ----------
    // (twice, literally -- not a typo in the original). @0x00435e93-0x00435f5e.
    if (c.mouse_in_rect(0x25, 0x8f, 0x36, 0xa4) == 1 && *v.mouse_buttons_cur > 0 &&
        own.sidebar_ui_hit_code() == 0) {
        // The blit moved host-side with the scope (R4): mh.dll's sink holds the
        // icon slot 0x28 and the (0x25, 0x8f) origin. Only the identity crosses.
        c.order_button(2);
        const int32_t vis_base            = own.view_tiles_w() * 7 - 6;
        own.tile_vis_map_at(vis_base + 0) = 2;
        own.tile_vis_map_at(vis_base + 1) = 2;
        own.tile_vis_map_at(vis_base + 2) = 2;
        c.move_path_preview_clear();
        mh::tact::group_issue_order(0x47, 0, 0, 0, 0);
        mh::tact::group_issue_order(0x47, 0, 0, 0, 0);
        own.sidebar_ui_hit_code() = 0x3f;
        return;
    }

    // ---- button 4: rect (0x36,0x91)-(0x4a,0xa5), icon slot 0x25 (37), orders 7, 0x47 -------------
    // @0x00435f63-0x0043602e.
    if (c.mouse_in_rect(0x36, 0x91, 0x4a, 0xa5) == 1 && *v.mouse_buttons_cur > 0 &&
        own.sidebar_ui_hit_code() == 0) {
        // The blit moved host-side with the scope (R4): mh.dll's sink holds the
        // icon slot 0x25 and the (0x36, 0x91) origin. Only the identity crosses.
        c.order_button(3);
        const int32_t vis_base            = own.view_tiles_w() * 7 - 6;
        own.tile_vis_map_at(vis_base + 0) = 2;
        own.tile_vis_map_at(vis_base + 1) = 2;
        own.tile_vis_map_at(vis_base + 2) = 2;
        c.move_path_preview_clear();
        mh::tact::group_issue_order(7, 0, 0, 0, 0);
        mh::tact::group_issue_order(0x47, 0, 0, 0, 0);
        own.sidebar_ui_hit_code() = 0x3e;
        return;
    }

    // ---- minimap click-to-recenter-camera --------------------------------------------------------
    // gate @0x00436033-0x0043608b, body @0x00436090-0x004361a6: none of the 4 buttons hit; gated on
    // hit_code==0, mouse_buttons_cur==1, and the mouse being strictly inside the 0x80 x 0x80 minimap
    // box (declared_needs: the two minimap-origin globals -- see header banner).
    if (own.sidebar_ui_hit_code() == 0 && *v.mouse_buttons_cur == 1 &&
        *v.tact_ui_minimap_origin_x < *v.sidebar_mouse_x &&
        *v.tact_ui_minimap_origin_y < *v.sidebar_mouse_y &&
        *v.sidebar_mouse_x < *v.tact_ui_minimap_origin_x + 0x80 &&
        *v.sidebar_mouse_y < *v.tact_ui_minimap_origin_y + 0x80) {
        // @0x0043609d-0x004360b3 / @0x004360ce-0x004360e1: G_WIN_W/64 and G_WIN_W/32, the compiler's
        // shift-form of C++ truncating division by a power of two (see header banner). G_WIN_W has no
        // writer in this closure, so the /32 term's two independent re-reads (@0x004360ce,
        // @0x004360f6) are provably the same value -- CSE'd to one local, same reasoning as
        // sim_path_group_steps.cpp's half_w/half_h.
        const int32_t win_w       = *v.win_w;
        const int32_t win_w_sign  = win_w >> 31; // 0 or -1, matches `SAR reg,0x1f`
        const int32_t win_w_div64 = (win_w + (win_w_sign & 0x3f)) >> 6;
        const int32_t win_w_div32 = (win_w + (win_w_sign & 0x1f)) >> 5;

        own.map_cam_col() = (*v.sidebar_mouse_x - *v.tact_ui_minimap_origin_x) - win_w_div64;
        if (own.map_cam_col() < 0) own.map_cam_col() = 0;
        if (*v.map_width - win_w_div32 <= own.map_cam_col()) {
            own.map_cam_col() = (*v.map_width - win_w_div32) - 1;
        }

        // @0x00436129-0x0043613f: WindowHeight/64, same shift-form idiom. Read through the store
        // (no read-only view member for WindowHeight yet; read-only usage here, same pattern as
        // tact_sidebar_dispatch.cpp's use of sel_panel_icon_gfx_at for a read).
        const int32_t win_h       = own.window_height();
        const int32_t win_h_sign  = win_h >> 31;
        const int32_t win_h_div64 = (win_h + (win_h_sign & 0x3f)) >> 6;
        // @0x0043615a-0x0043616f / @0x00436181-0x00436196: WindowHeight/24 via a genuine IDIV (not
        // the shift form) -- plain truncating division reproduces it directly. Computed twice
        // independently in the assembly; CSE'd here for the same no-writer-in-between reason.
        const int32_t win_h_div24 = win_h / 0x18;

        own.map_cam_row() = (*v.sidebar_mouse_y - *v.tact_ui_minimap_origin_y) - win_h_div64;
        if (own.map_cam_row() < 0) own.map_cam_row() = 0;
        if (*v.map_height - win_h_div24 <= own.map_cam_row()) {
            own.map_cam_row() = (*v.map_height - win_h_div24) - 1;
        }

        c.vis_map_fill_default();
    }
}

} // namespace detail

void ui_order_buttons_minimap_tick() {
    tact_state st = state();
    detail::ui_order_buttons_minimap_tick(st.read, st.own, live_ui_order_buttons_minimap_tick_calls());
}


} // namespace mh::tact
