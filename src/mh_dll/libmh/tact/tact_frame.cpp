//
// tact/tact_frame.cpp -- see tact_frame.h. Translated from the DISASSEMBLY, not from Ghidra's .c.
//
#include "tact/tact_frame.h"

#include <utility> // std::swap

#include "addr/mh_calls.gen.h" // frontier callees (Law 4), see the header banner


// Already-translated siblings, called through their own public wrappers (Law 3b same-set exception).
#include "tact/tact_ambient_sound.h"
#include "tact/tact_calc_dir24.h"
#include "tact/tact_camera_center_on_tile.h"
#include "tact/tact_cam_follow_selection_tick.h"
#include "tact/tact_door.h"
#include "tact/tact_group_issue_order.h"
#include "tact/tact_mission_end_return_to_strategic.h"
#include "tact/tact_pilot.h"
#include "tact/tact_scroll_target_proximity_tick.h"
#include "tact/tact_select_next_unit.h"
#include "tact/tact_selection_clear_unless_ctrl.h"
#include "tact/tact_sidebar_dispatch.h"
#include "tact/tact_squad_status.h"
#include "tact/tact_teleport_zone.h"
#include "tact/tact_unit_enqueue_command.h"
#include "tact/tact_unit_owner_tick.h"
#include "tact/tact_update_units_and_fx.h"
#include "tact/tact_view_shift.h"
#include "state/host_api.h"
#include "state/host_events.h"
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"
#include "crt/crt_select.h" // LIB-CRT: MH_CRT() picks the vendored CRT in the standalone build

namespace mh::tact {

const frame_calls &live_frame_calls() {
    static const frame_calls c = {
        mh::tact_host().llm_input_key_queue_empty,
        mh::tact_host().llm_input_key_dequeue,
        MH_LIBMH_BIND(time_GetCurrentTime),
        MH_LIBMH_BIND(llm_tact_selection_panel_refresh),
        mh::state::evt::screenshot_save,
        mh::tact_host().llm_input_mouse_delta_pump,
        mh::tact_host().llm_input_mouse_buttons_get,
        mh::state::evt::inv_tact_cursor_sprite,
        mh::tact_host().llm_tact_render_view,
        mh::state::evt::inv_tact_drag_box,
        mh::state::evt::inv_tact_minimap_overlay,
        mh::state::evt::snd_zone_play,
        mh::state::evt::tact_blast_transition,
        mh::state::evt::inv_tact_drawn_map_clear,
        mh::state::evt::inv_tact_vis_map,
        mh::state::evt::tact_frame_present,
        MH_CRT(w_sprintf__vss), // DECLARED NEED cross-check: already exists (mh_calls.gen.h:1833)
        mh::state::evt::text_tact_exit_confirm,
    };
    return c;
}

namespace detail {

void frame(const tact_view &v, tact_store &own, const frame_calls &c) {
    // ---- step 2 ---------------------------------------------------------------------------------
    mh::tact::ambient_sound_tick();

    // ---- step 3: the key-event dequeue -----------------------------------------------------------
    //
    // UNCERTAINTY (see header banner): fresh locals, not `static` -- "queue empty this frame" reads
    // as "no hotkey fires this frame" rather than replaying a leftover value from stack reuse.
    int32_t  scancode   = 0; // [EBP-0x2c] in the .asm, copied from the event's .scancode
    uint32_t event_type = 0; // [EBP-0x70] in the .asm, the event's own .event_type
    if (c.key_queue_empty() == 0) {
        mh::game::mh_llm_input_key_event ev{};
        c.key_dequeue(reinterpret_cast<uint32_t>(&ev));
        scancode   = static_cast<int32_t>(ev.scancode);
        event_type = ev.event_type;
    }

    // ---- step 4: camera-scroll-HELD latches --------------------------------------------------------
    if ((event_type & 0x100) != 0 && scancode == 0x48) own.cam_scroll_up_held() = 1;
    if ((event_type & 0x80) != 0 && scancode == 0x48) own.cam_scroll_up_held() = 0;
    if ((event_type & 0x100) != 0 && scancode == 0x50) own.cam_scroll_down_held() = 1;
    if ((event_type & 0x80) != 0 && scancode == 0x50) own.cam_scroll_down_held() = 0;
    if ((event_type & 0x100) != 0 && scancode == 0x4d) own.cam_scroll_right_held() = 1;
    if ((event_type & 0x80) != 0 && scancode == 0x4d) own.cam_scroll_right_held() = 0;
    if ((event_type & 0x100) != 0 && scancode == 0x4b) own.cam_scroll_left_held() = 1;
    if ((event_type & 0x80) != 0 && scancode == 0x4b) own.cam_scroll_left_held() = 0;
    if ((event_type & 0x80) != 0) scancode = 0;

    // ---- step 5: early-exit gate --------------------------------------------------------------------
    if (own.active_unit_count() == 0 || (scancode == 0x1c && own.exit_confirm_open() == 1)) {
        own.scroll_cmd()          = 0;
        own.mine_blast_time_end() = 0.0;
        own.exit_confirm_open()   = 0;
        mh::tact::units_reset_hp_for_active();
        mh::tact::squad_sync_hp();
        mh::tact::scroll_target_proximity_tick();
        mh::tact::mission_end_return_to_strategic();
        return;
    }

    // ---- step 6: ESC toggle / digit hotkeys / screenshot ---------------------------------------------
    if (scancode == 1 && (event_type & 0x100) != 0) {
        own.exit_confirm_open() = (own.exit_confirm_open() == 0) ? 1 : 0;
    }
    if (scancode > 1 && scancode < 10) {
        const int32_t target_group = scancode - 2;
        mh::tact::selection_clear_unless_ctrl();
        for (int32_t i = TACT_UNIT_FIRST_SLOT; i <= TACT_UNIT_LAST_SLOT; ++i) {
            tact_unit &u = own.unit_at(i);
            if (u.type != 0 && u.owner == 0 && u.squad_group_id == target_group) {
                u.status |= 1;
            }
        }
        c.selection_panel_refresh();
    }
    if (scancode == 0x1f) {
        c.save_screenshot();
    }

    // ---- step 7: the scroll/mine-blast-transition gate -----------------------------------------------
    bool run_main_body = own.scroll_cmd() < 1;
    if (!run_main_body) {
        own.scroll_cmd() += 1;
        run_main_body = own.scroll_cmd() < 9;
    }

    if (!run_main_body) {
        c.zone_sound_play(own.mine_blast_sound_id());
        own.scroll_cmd()          = 0;
        own.mine_blast_time_end() = 0.0;
        // The sixteen-iteration wipe is ONE scope now (LIFT-TACT slice B): the loop bound is a
        // compile-time 0x10 and nothing of libmh's runs between iterations, so what crosses is
        // "play the exit wipe" and mh.dll's sink runs the identical loop at this instant.
        c.blast_transition();
        c.blink_overlay_clear();
        c.vis_map_fill_default();
        own.exit_confirm_open() = 0;
        mh::tact::units_reset_hp_for_active();
        mh::tact::squad_sync_hp();
        mh::tact::scroll_target_proximity_tick();
        mh::tact::mission_end_return_to_strategic();
        return;
    }

    // ---- step 8: mine-blast countdown expiry ----------------------------------------------------------
    const double now = c.time_now();
    if (own.mine_blast_time_end() < now && own.mine_blast_time_end() > 0.0) {
        own.scroll_cmd() += 1;
        scancode                  = 0;
        own.mine_blast_time_end() = 0.0;
    }

    // ---- step 9: stop-move / toggle-gun / select-next / mine-arm hotkeys --------------------------------
    if (scancode == 0x34) {
        mh::tact::group_issue_order(0x7f, 0, 0, 0, 0);
        mh::tact::group_issue_order(4, 0, 0, 0, 0);
    }
    if (scancode == 0x35) {
        for (int32_t i = TACT_UNIT_FIRST_SLOT; i <= TACT_UNIT_LAST_SLOT; ++i) {
            tact_unit &u = own.unit_at(i);
            if ((u.status & 1) == 1) u.active_gun ^= 1;
        }
        c.selection_panel_refresh();
    }
    if (scancode == 0x33) {
        mh::tact::select_next_unit();
    }
    if (scancode == 0x32) {
        for (int32_t i = TACT_UNIT_FIRST_SLOT; i <= TACT_UNIT_LAST_SLOT; ++i) {
            if ((own.unit_at(i).status & 1) == 1) {
                mh::tact::unit_enqueue_command(i, 9, 0, 0, 0, 0, 0);
                break;
            }
        }
        scancode = 0;
    }

    // ---- step 10: Ctrl+Alt+Shift+W -- mark the whole 128x128 sub-block EXPLORED -------------------------
    // reimpl-verify (2026-08-28, wf_f38cb99c-13f) found a REAL divergence here: the original
    // re-reads the live RSHIFT/LSHIFT globals independently at 7 sites (0x00429f24, 0x0042a01e,
    // 0x0042a437, 0x0042a4d9, 0x0042a696, 0x0042a74b, 0x0042a910), never through a shared/cached
    // local. Steps 10/12 (the next two lines below) are CONTIGUOUS with no intervening calls, so a
    // single snapshot there is bit-identical to re-reading -- kept as `shift_held`. The other 5
    // sites (steps 18/19/20b/21/23a) run AFTER mouse_delta_pump/mouse_buttons_get/gfx_LoadSprite/
    // render_view/door_tick/teleport_zone_scan_tick/sidebar_dispatch/view_shift_* have already
    // executed, so a Shift transition mid-frame must be visible there -- read live via shift_now().
    const bool shift_held = (*v.key_rshift_held & 1) != 0 || (*v.key_lshift_held & 1) != 0;
    const auto shift_now  = [&] { return (*v.key_rshift_held & 1) != 0 || (*v.key_lshift_held & 1) != 0; };
    const bool alt_held   = (*v.key_lalt_held & 1) != 0 || (*v.key_lalt_held & 2) != 0;
    const bool ctrl_held  = (*v.key_lctrl_held & 1) != 0 || (*v.key_lctrl_held & 2) != 0;

    if (scancode == 0x57 && shift_held && alt_held && ctrl_held) {
        // Column-outer / row-inner in the .asm's own address arithmetic; re-expressed to match
        // tile_object_at()'s (x<<8)|y indexing -- see header derivation step 11.
        for (int32_t y = 0; y < TACT_MAP_DIM; ++y) {
            for (int32_t x = 0; x < TACT_MAP_DIM; ++x) {
                mh::state::tile_object &t = own.planes().tile_object_at(x, y);
                t.visibility += 1;
                // Same net effect tact_unit_vision.cpp's unit_vision_add already names: clear
                // FOGGED (0x4000), set EXPLORED (0x8000) -- performed INLINE in the original, not
                // via a call, so transcribed inline here too.
                t.flags[1] &= 0xbf;
                t.flags[1] |= 0x80;
            }
        }
    }

    // ---- step 12: Ctrl+Alt+Shift+X -- instant "heal to 10x max HP" cheat -------------------------------
    if (scancode == 0x58 && shift_held && alt_held && ctrl_held) {
        for (int32_t i = TACT_UNIT_FIRST_SLOT; i <= TACT_UNIT_LAST_SLOT; ++i) {
            tact_unit &u = own.unit_at(i);
            if ((u.status & 1) != 0) {
                character_type &ct = own.character_type_at(u.type);
                ct.energy          = static_cast<int16_t>(ct.energy * 10);
                u.hp               = static_cast<uint16_t>(ct.energy);
            }
        }
    }

    // ---- step 13: mouse pump + cursor sprite -----------------------------------------------------------
    //
    // DECLARED NEED: tact_store::mouse_buttons_cur() (see declared_needs). UNCERTAINTY: the ORIGINAL
    // store here is a 32-bit `MOV [addr],EAX` into a region the registry declares 1 byte
    // (RID_MOUSE_BUTTONS_CUR, `_G_LLM_MOUSE_BUTTONS_CUR` addr comment: "Ghidra type undefined1"),
    // i.e. the original clobbers 3 bytes past the declared region on every call. A `uint8_t&`
    // accessor (matching the region's declared width and tact_view's own `const uint8_t
    // *mouse_buttons_cur`) only reproduces the LOW byte; llm_input_mouse_buttons_get() returning a
    // small button mask (0..3, per every comparison site in this file and its siblings) makes the
    // 3 upper bytes of EAX zero on every real call, so a byte-exact register value makes the two
    // forms observationally identical for any value this function can ever pass -- but the WIDTH of
    // the original store, not just its value, is not reproduced. Flagged rather than silently
    // resolved.
    c.mouse_delta_pump();
    own.mouse_buttons_cur() = static_cast<uint8_t>(c.mouse_buttons_get());
    if (own.mouse_buttons_cur() == 0) own.click_action_taken() = 0;
    c.gfx_load_sprite(own.hovered_unit_id() == 0 ? 2 : 1);

    // ---- step 14: camera panning --------------------------------------------------------------------
    if (own.mouse_buttons_cur() == 0 && own.scroll_cmd() == 0) {
        if (*v.cursor_x == 0 || own.cam_scroll_left_held() == 1) {
            own.map_cam_col() -= 1;
            if (own.map_cam_col() < 0) {
                own.map_cam_col() = 0;
            } else {
                mh::tact::view_shift_col_dec();
            }
        }
        if (*v.cursor_y == 0 || own.cam_scroll_up_held() == 1) {
            own.map_cam_row() -= 1;
            if (own.map_cam_row() < 0) {
                own.map_cam_row() = 0;
            } else {
                mh::tact::view_shift_row_dec();
            }
        }
        if (own.window_width() - 1 == *v.cursor_x || own.cam_scroll_right_held() == 1) {
            own.map_cam_col() += 1;
            const int32_t col_bound = *v.map_width - (*v.win_w / 32); // shift-divide, exact for C++ /
            if (col_bound < own.map_cam_col()) {
                own.map_cam_col() = col_bound;
            } else {
                mh::tact::view_shift_col_inc();
            }
        }
        if (own.window_height() - 1 == *v.cursor_y || own.cam_scroll_down_held() == 1) {
            own.map_cam_row() += 1;
            const int32_t row_bound = *v.map_height - own.window_height() / 24; // real IDIV
            if (row_bound < own.map_cam_row()) {
                own.map_cam_row() = row_bound;
            } else {
                mh::tact::view_shift_row_inc();
            }
        }
        if (own.cam_follow_selection() == 1) {
            mh::tact::cam_follow_selection_tick();
        }
    }

    // ---- step 15 ---------------------------------------------------------------------------------
    c.render_view();
    mh::tact::door_tick();
    mh::tact::teleport_zone_scan_tick();
    mh::tact::sidebar_dispatch();

    // ---- step 16: the "cursor in the game viewport" gate wraps steps 17-24 ------------------------------
    if (*v.cursor_x < *v.win_w) {
        // ---- step 17: hover auto-move-order ---------------------------------------------------------
        if (own.mouse_buttons_cur() == 0) {
            own.click_scan_scratch() = 0;
            own.click_action_taken() = 0;
            for (int32_t i = TACT_UNIT_FIRST_SLOT; i <= TACT_UNIT_LAST_SLOT; ++i) {
                tact_unit &u = own.unit_at(i);
                if ((u.status & 1) == 1 && own.map_cam_col() <= static_cast<int32_t>(u.pos_col) &&
                    own.map_cam_row() <= static_cast<int32_t>(u.pos_row) &&
                    static_cast<int32_t>(u.pos_col) <= own.map_cam_col() + 0xf &&
                    static_cast<int32_t>(u.pos_row) <= own.map_cam_row() + 0x14) {
                    const int32_t dir = mh::tact::calc_dir24(static_cast<int32_t>(u.pos_col) * 0x20,
                                                             static_cast<int32_t>(u.pos_row) * 0x18,
                                                             own.map_cam_col() * 0x20 + *v.cursor_x,
                                                             own.map_cam_row() * 0x18 + *v.cursor_y);
                    if (u.cmd_queue[u.cmd_index].op == 0 || u.move_retry_wait != 0) {
                        mh::tact::unit_enqueue_command(i, 6, 0, dir, 0, 0, 0);
                    }
                }
            }
        }

        // ---- step 18: left-click select one -----------------------------------------------------------
        if (own.mouse_buttons_cur() == 1 && own.drag_select_active() == 0 &&
            own.click_action_taken() == 0 && own.hovered_unit_id() > 0) {
            if (!shift_now() && own.unit_at(own.hovered_unit_id()).owner == 0) {
                mh::tact::selection_clear_unless_ctrl();
                own.unit_at(own.hovered_unit_id()).status |= 1;
                c.selection_panel_refresh();
                own.click_action_taken() = 1;
            }
        }

        // ---- step 19: right-click own-unit command ----------------------------------------------------
        if (own.mouse_buttons_cur() == 2 && own.drag_select_active() == 0 &&
            own.click_action_taken() == 0 && own.hovered_unit_id() > 0) {
            if (!shift_now() && own.unit_at(own.hovered_unit_id()).owner == 0) {
                tact_unit &hovered = own.unit_at(own.hovered_unit_id());
                if ((hovered.status & 1) == 1) {
                    if (hovered.anim_state == 0 || hovered.anim_state == 1) {
                        mh::tact::unit_enqueue_command(own.hovered_unit_id(), 0x7f, 0, 0, 0, 0, 0);
                        mh::tact::unit_enqueue_command(own.hovered_unit_id(), 4, 0, 0, 0, 0, 0);
                    } else if (hovered.anim_state == 3) {
                        mh::tact::unit_enqueue_command(own.hovered_unit_id(), 0x7f, 0, 0, 0, 0, 0);
                        mh::tact::unit_enqueue_command(own.hovered_unit_id(), 5, 0, 0, 0, 0, 0);
                    }
                } else {
                    hovered.status |= 1;
                }
                own.click_action_taken() = 1;
            }
        }

        // ---- step 20: right-click group order (enemy hovered / no hover) ------------------------------
        if (own.mouse_buttons_cur() > 0 && own.drag_select_active() == 0 && own.hovered_unit_id() > 0 &&
            own.active_unit_count_cached() > 0 && own.unit_at(own.hovered_unit_id()).owner == 1) {
            mh::tact::group_issue_order(2, static_cast<uint32_t>(own.mouse_buttons_cur() - 1) ^ 1u, 0,
                                        own.map_cam_col() * 0x20 + *v.cursor_x, own.map_cam_row() * 0x18 + *v.cursor_y);
        }
        if (own.mouse_buttons_cur() > 0 && own.drag_select_active() == 0 && own.hovered_unit_id() == 0) {
            if (shift_now()) {
                if (own.mouse_buttons_cur() == 3) {
                    mh::tact::group_issue_order(2, 1, 0, own.map_cam_col() * 0x20 + *v.cursor_x,
                                                own.map_cam_row() * 0x18 + *v.cursor_y);
                } else {
                    mh::tact::group_issue_order(2, static_cast<uint32_t>(own.mouse_buttons_cur() - 1) ^ 1u, 0,
                                                own.map_cam_col() * 0x20 + *v.cursor_x,
                                                own.map_cam_row() * 0x18 + *v.cursor_y);
                }
            }
        }

        // ---- step 21: right-click move-or-camera-center ------------------------------------------------
        if (own.mouse_buttons_cur() == 2 && own.hovered_unit_id() == 0 && own.click_action_taken() == 0) {
            if (!shift_now()) {
                const int32_t world_col = own.map_cam_col() + *v.cursor_x / 32; // shift-divide == C++ /
                const int32_t world_row = own.map_cam_row() + *v.cursor_y / 24; // real IDIV
                if (own.active_unit_count_cached() < 1) {
                    mh::tact::camera_center_on_tile(world_col, world_row);
                } else {
                    mh::tact::group_issue_order(1, world_col, world_row, 0, 0);
                }
                own.click_action_taken() = 1;
            }
        }

        // ---- step 22: click-preview facing paint ------------------------------------------------------
        if (own.mouse_buttons_cur() == 2 && own.click_action_taken() == 1) {
            for (int32_t i = TACT_UNIT_FIRST_SLOT; i <= TACT_UNIT_LAST_SLOT; ++i) {
                tact_unit &u = own.unit_at(i);
                if ((u.status & 0x41) == 0x41) {
                    const int32_t redirect_col = u.move_redirect_col;
                    const int32_t redirect_row = u.move_redirect_row;
                    const int32_t dir          = mh::tact::calc_dir24(redirect_col * 0x20 + 0x10, redirect_row * 0x18 + 0xc,
                                                                      own.map_cam_col() * 0x20 + *v.cursor_x,
                                                                      own.map_cam_row() * 0x18 + *v.cursor_y);
                    mh::state::tile_overlay(own.planes(), redirect_col, redirect_row) =
                        static_cast<uint8_t>(dir - 0x40);
                    u.click_preview_facing = static_cast<uint8_t>(dir);
                }
            }
        }

        // ---- step 23: left-click deselect-all / drag-start ----------------------------------------------
        if (own.mouse_buttons_cur() == 1 && own.drag_select_active() == 0 && own.click_action_taken() == 0) {
            if (!shift_now() && own.active_unit_count_cached() > 0 && own.hovered_unit_id() == 0) {
                for (int32_t i = TACT_UNIT_FIRST_SLOT; i <= TACT_UNIT_LAST_SLOT; ++i) {
                    own.unit_at(i).status &= 0xfe;
                }
                c.selection_panel_refresh();
                own.click_action_taken() = 1;
            }
        }
        if (own.mouse_buttons_cur() == 1 && own.drag_select_active() == 0 && own.click_action_taken() == 0 &&
            own.hovered_unit_id() == 0 && own.active_unit_count_cached() == 0) {
            own.drag_anchor_x()      = *v.cursor_x;
            own.drag_anchor_y()      = *v.cursor_y;
            own.drag_select_active() = 1;
            own.click_action_taken() = 1;
        }

        // ---- step 24: drag-box clamp / release --------------------------------------------------------
        if (own.drag_select_active() == 1) {
            c.drag_box_clamp(own.drag_anchor_x(), own.drag_anchor_y(), *v.cursor_x, *v.cursor_y);
        }
        if (own.mouse_buttons_cur() == 0 && own.drag_select_active() == 1 && own.click_action_taken() == 0) {
            int32_t col_lo = own.map_cam_col() + own.drag_anchor_x() / 32; // shift-divide == C++ /
            int32_t row_lo = own.map_cam_row() + own.drag_anchor_y() / 24; // real IDIV
            int32_t col_hi = own.map_cam_col() + *v.cursor_x / 32;
            int32_t row_hi = own.map_cam_row() + *v.cursor_y / 24;
            if (col_hi < col_lo) std::swap(col_lo, col_hi);
            if (row_hi < row_lo) std::swap(row_lo, row_hi);

            mh::tact::selection_clear_unless_ctrl();
            own.hovered_unit_id() = 0;
            for (int32_t y = row_lo; y <= row_hi; ++y) {
                for (int32_t x = col_lo; x <= col_hi; ++x) {
                    const int32_t unit_idx = own.planes().tile_object_at(x, y).building;
                    if (unit_idx != 0 && own.unit_at(unit_idx).owner == 0) {
                        own.unit_at(unit_idx).status |= 1;
                    }
                }
            }
            c.selection_panel_refresh();
            own.drag_select_active() = 0;
            own.click_action_taken() = 1;
        }
    }

    // ---- step 25 -----------------------------------------------------------------------------------
    mh::tact::update_units_and_fx();
    mh::tact::unit_owner_tick(1);
    mh::tact::unit_owner_tick(0);
    c.tile_overlay_refresh();

    // ---- step 26: exit-confirm-open overlay -----------------------------------------------------------
    if (own.exit_confirm_open() == 1) {
        // Ghidra auto-label `u_ENTER_-_%s,_ESC_-_%s_005002af` -- see declared_needs.
        static constexpr const wchar_t *kExitPromptFormat = L"ENTER - %s, ESC - %s";
        c.sprintf_vss(own.text_scratch(), kExitPromptFormat, v.text_ptrs[0x29b], v.text_ptrs[0x2dd]);
        c.draw_exit_confirm_text(own.text_scratch());

        // row_base = 240/32 = 7 exactly (the SAME literal y=0xf0 the draw call above used, divided
        // by the same shift-divide-by-32 idiom the camera-pan bounds use) -- algebraically forced,
        // not runtime-variable. See header derivation step 26.
        constexpr int32_t kExitPromptTextY = 0xf0;
        const int32_t     row_base         = kExitPromptTextY / 32;
        for (int32_t i = 0; i < 0x14; ++i) {
            own.tile_vis_map_at(row_base * 20 + i)        = 2;
            own.tile_drawn_map_at(row_base * 20 + i)      = 1;
            own.tile_vis_map_at(row_base * 20 + i + 20)   = 2;
            own.tile_drawn_map_at(row_base * 20 + i + 20) = 1;
        }
        // Redundant with the loop above's cells -- preserved literally (translator-brief.md's
        // "preserve reset-then-fill / redundant writes" rule).
        constexpr int32_t kTileVisMapSlots = 300; // RID_TILE_VIS_MAP / RID_TILE_DRAWN_MAP's own extent
        for (int32_t i = 0; i < kTileVisMapSlots; ++i) {
            own.tile_vis_map_at(i)   = 2;
            own.tile_drawn_map_at(i) = 1;
        }
    }

    // ---- step 27 -----------------------------------------------------------------------------------
    c.frame_present();
}

} // namespace detail

void frame() {
    tact_state st = state();
    detail::frame(st.read, st.own, live_frame_calls());
}


// ---- THE PROMOTED ARM IS GONE (fork F2E: tactical mode is demoted permanently) ------------------
//
// llm_tact_frame was the ONE tactical body whose promotion mattered for coverage rather than for a
// single row: with it live, our per-unit tick chain executed and the domain read 69%; with it
// original, none of that code ran at all. Both routes onto that entry are deleted here -- the
// rebind of the tactical cadence detour's fall-through, and the direct entry patch for a run where
// no cadence armed -- along with the one-owner flag that arbitrated between them and the served-call
// milestone ladder that proved the body was not vacuously green.
//
// WHY, and it is a scope decision rather than a retraction of that evidence. The fork ships two
// hosted configurations, `original` and `brokered`, and the reimplemented spine brokered serves is
// the STRATEGIC one; tactical mode is the game's own in both. An entry install with no configuration
// that arms it is not a dormant feature -- it is a claim about what runs that is false in every run,
// and the counters, the route naming and the one-owner guard were all machinery for reading a claim
// nobody can now make. The TACT1-P evidence stands as what it was: a measurement of a configuration
// this fork does not ship.
//
// THE BODY ABOVE IS UNTOUCHED. net_selftest's tacttest drives it directly, and its rebind row
// survives (ruling Q2), so a standalone host binds it unconditionally.

} // namespace mh::tact
