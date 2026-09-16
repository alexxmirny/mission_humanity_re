//
// tact_sidebar_dispatch_selftest.cpp -- offline oracle for llm_tact_sidebar_dispatch. See
// tact/tact_sidebar_dispatch.h for the full control-flow derivation (the two independently-gated
// blocks, the LUT-vs-roster addressing hazard, and the TILE_VIS_MAP_M4/M5/M6 aliasing).
//
// COVERAGE THE LIFT ADDED, worth naming because it inverts what this banner used to say. The two
// hit_code >= 0x64 branches used to be untestable on their positive (unit_idx > 0) side: the call
// went through the sibling migration member's own public wrapper, which bound seven outward calls
// to real game VAs that do not exist in this standalone harness process, so invoking it would have
// crashed the suite rather than failed a check. LIFT-TACT slice A moved that body host-side whole,
// so the branch goes through the calls struct like every other -- T18/T19 below exercise the side
// that was previously provable only by re-deriving the CMP/JLE pair on paper.
// The roster branch (hit_code in [0x28,0x3c)) is exercised only with the documented
// active_group_id == -1 sentinel, which the header proves is the only value keeping the read inside
// unassigned_unit_roster's own 256 B; a non-sentinel active_group_id walks into
// _G_LLM_TACT_GROUP_UNIT_ROSTER's real adjacent territory, a separate, non-adjacent vector in this
// fixture (same declared_needs gap the header itself names, not fixed here).
//
#include "tact/tact_sidebar_dispatch.h"
#include "tact_test_support.h"

#include <cstring>

namespace mh::tact::test {

namespace {

struct call_log {
    int     minimap_calls = 0, multi_calls = 0, single_calls = 0, sel_panel_draw_calls = 0;
    int     char_panel_row_draw_calls = 0;
    int32_t char_panel_unit_idx = -1, char_panel_row_slot = -1;
    int     group_panel_calls          = 0;
    int     draw_player_row_list_calls = 0;
    int32_t last_selected_row          = -999;
    int     panel_refresh_calls        = 0;
};

call_log &log() {
    static call_log l;
    return l;
}
void reset_log() { log() = call_log{}; }

void mock_minimap() { ++log().minimap_calls; }
void mock_multi() { ++log().multi_calls; }
void mock_single() { ++log().single_calls; }
void mock_sel_panel_draw() { ++log().sel_panel_draw_calls; }
void mock_char_panel_row_draw(int32_t unit_idx, int32_t row_slot) {
    call_log &l = log();
    ++l.char_panel_row_draw_calls;
    l.char_panel_unit_idx = unit_idx;
    l.char_panel_row_slot = row_slot;
}
// LIFT-TACT slice A: the group-assign panel's blit became a NULLARY scope -- its geometry was all
// literal, so nothing crosses at all and the mock has nothing left to record but the fact.
void mock_group_panel() { ++log().group_panel_calls; }
void mock_draw_player_row_list(int32_t selected_row) {
    ++log().draw_player_row_list_calls;
    log().last_selected_row = selected_row;
}
void mock_panel_refresh() { ++log().panel_refresh_calls; }

sidebar_dispatch_calls mock_calls() {
    return {mock_minimap, mock_multi, mock_single,
            mock_group_panel, mock_draw_player_row_list, mock_panel_refresh,
            mock_sel_panel_draw, mock_char_panel_row_draw};
}

} // namespace

void run_sidebar_dispatch_tests() {
    // T1: cursor LEFT of the sidebar (cursor_x < win_w) -> block 1 entirely skipped: no mouse_x/y
    // latch, no per-mode tick, sidebar_highlighted_unit_id untouched. hit_code<=0 keeps block 2 a no-op too.
    {
        tact_fixture fx;
        fx.cursor_x                    = 100;
        fx.win_w                       = 640;
        fx.cursor_y                    = 55;
        fx.sidebar_mouse_x             = -1;
        fx.sidebar_mouse_y             = -1;
        fx.sidebar_highlighted_unit_id = 42;
        fx.sidebar_ui_hit_code         = 0;

        reset_log();
        tact_store own = fx.store();
        detail::sidebar_dispatch(fx.view(), own, mock_calls());

        ck_eq((uint32_t)log().minimap_calls, 0u, "T1: cursor left of sidebar -> no per-mode tick, 0x00435995");
        ck_eq((uint32_t)own.sidebar_mouse_x(), (uint32_t)-1, "T1: mouse_x untouched");
        ck_eq((uint32_t)own.sidebar_highlighted_unit_id(), 42u, "T1: sidebar_highlighted_unit_id untouched");
    }

    // T2: cursor OVER the sidebar, mouse_y < 0xa8 -> minimap_tick, mouse_x/y latched relative to
    // win_w, sidebar_highlighted_unit_id cleared.
    {
        tact_fixture fx;
        fx.cursor_x                    = 700;
        fx.win_w                       = 640;
        fx.cursor_y                    = 50;
        fx.sidebar_highlighted_unit_id = 7;

        reset_log();
        tact_store own = fx.store();
        detail::sidebar_dispatch(fx.view(), own, mock_calls());

        ck_eq((uint32_t)log().minimap_calls, 1u, "T2: mouse_y(50) < 0xa8 -> minimap_tick, 0x004359c7");
        ck_eq((uint32_t)own.sidebar_mouse_x(), 60u, "T2: sidebar_mouse_x = cursor_x - win_w, 0x00435997");
        ck_eq((uint32_t)own.sidebar_mouse_y(), 50u, "T2: sidebar_mouse_y = cursor_y, 0x004359a3");
        ck_eq((uint32_t)own.sidebar_highlighted_unit_id(), 0u, "T2: sidebar_highlighted_unit_id cleared, 0x004359b6");
    }

    // T3: mouse_y >= 0xa8, multi_mode == 0 -> multi_mode_tick (NOT single_mode_tick, despite the
    // name -- confirmed against the .asm: CMP MULTI_MODE,0; JNZ single; fallthrough calls
    // 0x004361b5 = llm_tact_ui_sel_panel_multi_mode_tick when multi_mode==0).
    {
        tact_fixture fx;
        fx.cursor_x                = 700;
        fx.win_w                   = 640;
        fx.cursor_y                = 200;
        fx.ui_sel_panel_multi_mode = 0;

        reset_log();
        tact_store own = fx.store();
        detail::sidebar_dispatch(fx.view(), own, mock_calls());

        ck_eq((uint32_t)log().multi_calls, 1u, "T3: mouse_y>=0xa8, multi_mode==0 -> multi_mode_tick, 0x004359d7");
        ck_eq((uint32_t)log().single_calls, 0u, "T3: single_mode_tick NOT called");
    }

    // T4: mouse_y >= 0xa8, multi_mode != 0 -> single_mode_tick.
    {
        tact_fixture fx;
        fx.cursor_x                = 700;
        fx.win_w                   = 640;
        fx.cursor_y                = 200;
        fx.ui_sel_panel_multi_mode = 1;

        reset_log();
        tact_store own = fx.store();
        detail::sidebar_dispatch(fx.view(), own, mock_calls());

        ck_eq((uint32_t)log().single_calls, 1u, "T4: multi_mode!=0 -> single_mode_tick, 0x004359de");
        ck_eq((uint32_t)log().multi_calls, 0u, "T4: multi_mode_tick NOT called");
    }

    // T5: block 2 GATE -- mouse_buttons_cur != 0 (button still held) suppresses dispatch even with
    // hit_code > 0. cursor left of sidebar isolates block 1's own tick calls out of the picture.
    {
        tact_fixture fx;
        fx.cursor_x            = 0;
        fx.win_w               = 640;
        fx.sidebar_ui_hit_code = 5;
        fx.mouse_buttons_cur   = 1;

        reset_log();
        tact_store own = fx.store();
        detail::sidebar_dispatch(fx.view(), own, mock_calls());

        ck_eq((uint32_t)own.sidebar_ui_hit_code(), 5u, "T5: button still held -> hit_code untouched, 0x004359ea");
        ck_eq((uint32_t)log().draw_player_row_list_calls, 0u, "T5: no dispatch while button held");
    }

    // T6: hit_code == 2 is consumed with no action -- cleared to 0, falls through the range checks
    // (none match 0), no call fires.
    {
        tact_fixture fx;
        fx.cursor_x                = 0;
        fx.win_w                   = 640;
        fx.sidebar_ui_hit_code     = 2;
        fx.mouse_buttons_cur       = 0;
        fx.ui_sel_panel_multi_mode = 0;

        reset_log();
        tact_store own = fx.store();
        detail::sidebar_dispatch(fx.view(), own, mock_calls());

        ck_eq((uint32_t)own.sidebar_ui_hit_code(), 0u, "T6: hit_code 2 cleared to 0, 0x00435a0d");
        ck_eq((uint32_t)log().group_panel_calls, 0u, "T6: no other action for hit_code 2");
        ck_eq((uint32_t)log().draw_player_row_list_calls, 0u, "T6: no other action for hit_code 2");
    }

    // T7: hit_code in [0x3c,0x46) -> group-assign icon panel redraw with the FIXED args, then the
    // three TILE_VIS_MAP cells at view_tiles_w()*7-6 .. -4 stamped, hit_code cleared.
    {
        tact_fixture fx;
        fx.cursor_x            = 0;
        fx.win_w               = 640;
        fx.sidebar_ui_hit_code = 0x40;
        fx.mouse_buttons_cur   = 0;
        fx.view_tiles_w        = 20; // vis_base = 20*7-6 = 134
        fx.gfx_panel_row_skip  = 99;
        for (auto &b : fx.tile_vis_map) b = 0xAB;
        void *icon0              = (void *)0x1234;
        fx.sel_panel_icon_gfx[0] = icon0;

        reset_log();
        tact_store own = fx.store();
        detail::sidebar_dispatch(fx.view(), own, mock_calls());

        ck_eq((uint32_t)log().group_panel_calls, 1u,
              "T7: the group-assign panel scope fires, 0x00435a4c -- nullary, because its\n"
              "        icon slot 0, (2,0x8e) origin and 0x46x0x17 clip were all literals and\n"
              "        moved into mh.dll's sink whole");
        ck_eq((uint32_t)own.tile_vis_map_at(134), 2u, "T7: tile_vis_map[vis_base+0]=2, 0x00435a5c");
        ck_eq((uint32_t)own.tile_vis_map_at(135), 2u, "T7: tile_vis_map[vis_base+1]=2, 0x00435a68");
        ck_eq((uint32_t)own.tile_vis_map_at(136), 2u, "T7: tile_vis_map[vis_base+2]=2, 0x00435a76");
        ck_eq((uint32_t)own.tile_vis_map_at(133), 0xABu, "T7: cell just below vis_base untouched");
        ck_eq((uint32_t)own.sidebar_ui_hit_code(), 0u, "T7: hit_code cleared");
    }

    // T8: hit_code == 0xa -> draw_player_row_list(-1), hit_code cleared.
    {
        tact_fixture fx;
        fx.cursor_x            = 0;
        fx.win_w               = 640;
        fx.sidebar_ui_hit_code = 0xa;
        fx.mouse_buttons_cur   = 0;

        reset_log();
        tact_store own = fx.store();
        detail::sidebar_dispatch(fx.view(), own, mock_calls());

        ck_eq((uint32_t)log().draw_player_row_list_calls, 1u, "T8: draw_player_row_list fires, 0x00435a8e");
        ck_eq((uint32_t)log().last_selected_row, (uint32_t)-1, "T8: called with -1");
        ck_eq((uint32_t)own.sidebar_ui_hit_code(), 0u, "T8: hit_code cleared");
    }

    // T9: hit_code in [0x14,0x28) -- LUT/ASSIGN branch. unit_idx read as a raw int32 from
    // sidebar_icon_slot_unit_lut at byte offset hit_code*4 (past the LUT's own declared 40 B, see
    // this TU's file banner and the fixture's own 160 B sizing note); squad_group_id set to
    // active_group_id; both follow-up calls fire.
    {
        tact_fixture fx;
        fx.cursor_x                = 0;
        fx.win_w                   = 640;
        fx.sidebar_ui_hit_code     = 0x14; // -> byte offset 0x50
        fx.mouse_buttons_cur       = 0;
        fx.sidebar_active_group_id = 3;
        const int32_t target_unit  = 9;
        std::memcpy(fx.sidebar_icon_slot_unit_lut.data() + 0x50, &target_unit, sizeof(target_unit));

        reset_log();
        tact_store own = fx.store();
        detail::sidebar_dispatch(fx.view(), own, mock_calls());

        ck_eq((uint32_t)own.unit_at(target_unit).squad_group_id, 3u,
              "T9: unit_at(9).squad_group_id = active_group_id, 0x00435ac4-0x00435adc");
        ck_eq((uint32_t)log().panel_refresh_calls, 1u, "T9: selection_panel_refresh fires, 0x00435b06");
        ck_eq((uint32_t)log().draw_player_row_list_calls, 1u, "T9: draw_player_row_list(-1) fires, 0x00435b17");
        ck_eq((uint32_t)own.sidebar_ui_hit_code(), 0u, "T9: hit_code cleared");
    }

    // T10: hit_code in [0x28,0x3c) -- roster/UNASSIGN branch, active_group_id == -1 (the documented
    // sentinel that keeps the read inside unassigned_unit_roster's own 256 B -- see this TU's file
    // banner). byte_off = hit_code*4 + (-256) + 0x60; for hit_code=0x28 that is exactly 0.
    {
        tact_fixture fx;
        fx.cursor_x                = 0;
        fx.win_w                   = 640;
        fx.sidebar_ui_hit_code     = 0x28;
        fx.mouse_buttons_cur       = 0;
        fx.sidebar_active_group_id = -1;
        const int32_t target_unit  = 12;
        std::memcpy(fx.unassigned_unit_roster.data() + 0, &target_unit, sizeof(target_unit));

        reset_log();
        tact_store own = fx.store();
        detail::sidebar_dispatch(fx.view(), own, mock_calls());

        ck_eq((uint32_t)own.unit_at(target_unit).squad_group_id, 0xffu,
              "T10: unit_at(12).squad_group_id = 0xff (unassign), 0x00435ae4-0x00435b01");
        ck_eq((uint32_t)log().panel_refresh_calls, 1u, "T10: selection_panel_refresh fires");
        ck_eq((uint32_t)log().draw_player_row_list_calls, 1u, "T10: draw_player_row_list(-1) fires");
    }

    // T11: single-select mode, hit_code == 1 -- toggle bit 1 of every non-negative
    // sel_panel_icon_slot_state slot (4 of them); redraw fires iff at least one toggled.
    {
        tact_fixture fx;
        fx.cursor_x                     = 0;
        fx.win_w                        = 640;
        fx.sidebar_ui_hit_code          = 1;
        fx.mouse_buttons_cur            = 0;
        fx.ui_sel_panel_multi_mode      = 0;
        fx.sel_panel_icon_slot_state[0] = 0;  // non-negative -> toggled: (0|1)>0 true -> 0&2=0
        fx.sel_panel_icon_slot_state[1] = 2;  // non-negative -> toggled: (2|1)>0 true -> 2&2=2
        fx.sel_panel_icon_slot_state[2] = -1; // NEGATIVE -> (−1|1) stays −1, NOT >0 -> untouched
        fx.sel_panel_icon_slot_state[3] = 5;  // non-negative -> (5|1)=5>0 -> 5&2=0

        reset_log();
        tact_store own = fx.store();
        detail::sidebar_dispatch(fx.view(), own, mock_calls());

        ck_eq((uint32_t)own.sel_panel_icon_slot_state_at(0), 0u, "T11: slot0 0->0&2=0, 0x00435b6c");
        ck_eq((uint32_t)own.sel_panel_icon_slot_state_at(1), 2u, "T11: slot1 2->2&2=2");
        ck_eq((uint32_t)own.sel_panel_icon_slot_state_at(2), (uint32_t)-1, "T11: slot2 negative -> untouched, 0x00435b52");
        ck_eq((uint32_t)own.sel_panel_icon_slot_state_at(3), 0u, "T11: slot3 5->5&2=0");
        ck_eq((uint32_t)log().sel_panel_draw_calls, 1u, "T11: at least one toggled -> sel_panel_draw fires, 0x00435b9b");
        ck_eq((uint32_t)own.sidebar_ui_hit_code(), 0u, "T11: hit_code cleared when any slot toggled");
    }

    // T12: single-select mode, hit_code==1, ALL slots negative -> nothing toggles -> NO redraw.
    {
        tact_fixture fx;
        fx.cursor_x                = 0;
        fx.win_w                   = 640;
        fx.sidebar_ui_hit_code     = 1;
        fx.mouse_buttons_cur       = 0;
        fx.ui_sel_panel_multi_mode = 0;
        for (int i = 0; i < 4; ++i) fx.sel_panel_icon_slot_state[i] = -1;

        reset_log();
        tact_store own = fx.store();
        detail::sidebar_dispatch(fx.view(), own, mock_calls());

        ck_eq((uint32_t)log().sel_panel_draw_calls, 0u, "T12: nothing toggled -> no redraw, 0x00435b9e");
        ck_eq((uint32_t)own.sidebar_ui_hit_code(), 1u, "T12: hit_code NOT cleared when nothing toggled");
    }

    // T13: multi-select mode, hit_code==1 -- SAME toggle but over sidebar_scrollbtn_state (8 slots).
    {
        tact_fixture fx;
        fx.cursor_x                = 0;
        fx.win_w                   = 640;
        fx.sidebar_ui_hit_code     = 1;
        fx.mouse_buttons_cur       = 0;
        fx.ui_sel_panel_multi_mode = 1;
        for (int i = 0; i < 8; ++i) fx.sidebar_scrollbtn_state[i] = -1;
        fx.sidebar_scrollbtn_state[3] = 6; // non-negative -> (6|1)=7>0 -> 6&2=2

        reset_log();
        tact_store own = fx.store();
        detail::sidebar_dispatch(fx.view(), own, mock_calls());

        ck_eq((uint32_t)own.sidebar_scrollbtn_state_at(3), 2u, "T13: multi-mode toggle over the 8-slot table, 0x00435c99");
        ck_eq((uint32_t)log().sel_panel_draw_calls, 1u, "T13: redraw fires on the multi-mode toggle too");
    }

    // T14: multi-select mode, hit_code != 1 -- no-op, hit_code left UNTOUCHED (not cleared).
    {
        tact_fixture fx;
        fx.cursor_x                = 0;
        fx.win_w                   = 640;
        fx.sidebar_ui_hit_code     = 0x50; // outside every handled range
        fx.mouse_buttons_cur       = 0;
        fx.ui_sel_panel_multi_mode = 1;

        reset_log();
        tact_store own = fx.store();
        detail::sidebar_dispatch(fx.view(), own, mock_calls());

        ck_eq((uint32_t)own.sidebar_ui_hit_code(), 0x50u, "T14: multi-mode, unhandled hit_code left untouched, 0x00435c6a-0x00435cdb");
    }

    // T15: single-select mode, hit_code in [0x64,0x78), unit_idx <= 0 -- the gated-OFF side: no
    // char-panel row draw is requested, hit_code still cleared.
    {
        tact_fixture fx;
        fx.cursor_x                 = 0;
        fx.win_w                    = 640;
        fx.sidebar_ui_hit_code      = 0x64;
        fx.mouse_buttons_cur        = 0;
        fx.ui_sel_panel_multi_mode  = 0;
        fx.sidebar_slot_scroll      = 0;
        fx.sidebar_slot_unit_ids[0] = 0; // unit_idx <= 0 -> the call is gated off, 0x00435bf1

        reset_log();
        tact_store own = fx.store();
        detail::sidebar_dispatch(fx.view(), own, mock_calls());

        ck_eq((uint32_t)own.sidebar_ui_hit_code(), 0u, "T15: hit_code cleared even when unit_idx<=0, 0x00435bf9");
        ck_eq((uint32_t)log().char_panel_row_draw_calls, 0u,
              "T15: unit_idx<=0 gates the char-panel row draw OFF, 0x00435bf1");
    }

    // T16: single-select mode, hit_code in [0x78,0x8c), unit_idx <= 0 -- same gated-OFF proof for
    // the second branch.
    {
        tact_fixture fx;
        fx.cursor_x                 = 0;
        fx.win_w                    = 640;
        fx.sidebar_ui_hit_code      = 0x78;
        fx.mouse_buttons_cur        = 0;
        fx.ui_sel_panel_multi_mode  = 0;
        fx.sidebar_slot_scroll      = 0;
        fx.sidebar_slot_unit_ids[0] = -1; // unit_idx <= 0

        reset_log();
        tact_store own = fx.store();
        detail::sidebar_dispatch(fx.view(), own, mock_calls());

        ck_eq((uint32_t)own.sidebar_ui_hit_code(), 0u, "T16: hit_code cleared even when unit_idx<=0, 0x00435c56");
        ck_eq((uint32_t)log().char_panel_row_draw_calls, 0u,
              "T16: unit_idx<=0 gates the char-panel row draw OFF, 0x00435c4e");
    }

    // T17: single-select mode, hit_code outside every handled range -> left untouched (not 0x1 and
    // not in [0x64,0x8c)).
    {
        tact_fixture fx;
        fx.cursor_x                = 0;
        fx.win_w                   = 640;
        fx.sidebar_ui_hit_code     = 0x50;
        fx.mouse_buttons_cur       = 0;
        fx.ui_sel_panel_multi_mode = 0;

        reset_log();
        tact_store own = fx.store();
        detail::sidebar_dispatch(fx.view(), own, mock_calls());

        ck_eq((uint32_t)own.sidebar_ui_hit_code(), 0x50u, "T17: single-mode, unhandled hit_code left untouched, 0x00435c08-0x00435c1d");
    }

    // T18: the FIRST hit-code arm with unit_idx > 0 -- the side this suite could not reach until
    // LIFT-TACT slice A turned the sibling call into a scope. It pins BOTH halves of the record:
    // the unit comes from sidebar_slot_unit_ids[(hit_code-0x64)+scroll], and the row slot is the
    // hit code's OFFSET within the arm, not the hit code itself.
    {
        tact_fixture fx;
        fx.cursor_x                 = 0;
        fx.win_w                    = 640;
        fx.sidebar_ui_hit_code      = 0x66; // offset 2 within [0x64,0x78)
        fx.mouse_buttons_cur        = 0;
        fx.ui_sel_panel_multi_mode  = 0;
        fx.sidebar_slot_scroll      = 1;
        fx.sidebar_slot_unit_ids[3] = 77; // (0x66-0x64) + scroll 1 = index 3

        reset_log();
        tact_store own = fx.store();
        detail::sidebar_dispatch(fx.view(), own, mock_calls());

        ck_eq((uint32_t)log().char_panel_row_draw_calls, 1u,
              "T18: unit_idx>0 requests the char-panel row draw, 0x00435bf1");
        ck_eq((uint32_t)log().char_panel_unit_idx, 77u,
              "T18: the unit is read through (hit_code-0x64)+sidebar_slot_scroll, 0x00435bd8");
        ck_eq((uint32_t)log().char_panel_row_slot, 2u,
              "T18: the row slot is hit_code-0x64, NOT the hit code, 0x00435bef");
        ck_eq((uint32_t)own.sidebar_ui_hit_code(), 0u, "T18: hit_code cleared, 0x00435bf9");
    }

    // T19: the SECOND arm's positive side -- same shape, different bias (0x78), which is the pair
    // of literals a transcription slip would swap.
    {
        tact_fixture fx;
        fx.cursor_x                 = 0;
        fx.win_w                    = 640;
        fx.sidebar_ui_hit_code      = 0x79; // offset 1 within [0x78,0x8c)
        fx.mouse_buttons_cur        = 0;
        fx.ui_sel_panel_multi_mode  = 0;
        fx.sidebar_slot_scroll      = 0;
        fx.sidebar_slot_unit_ids[1] = 88;

        reset_log();
        tact_store own = fx.store();
        detail::sidebar_dispatch(fx.view(), own, mock_calls());

        ck_eq((uint32_t)log().char_panel_row_draw_calls, 1u,
              "T19: second arm also requests the draw, 0x00435c4e");
        ck_eq((uint32_t)log().char_panel_unit_idx, 88u, "T19: unit from index (0x79-0x78)+0 = 1");
        ck_eq((uint32_t)log().char_panel_row_slot, 1u,
              "T19: the row slot is hit_code-0x78, the OTHER bias, 0x00435c4c");
        ck_eq((uint32_t)own.sidebar_ui_hit_code(), 0u, "T19: hit_code cleared, 0x00435c56");
    }
}

} // namespace mh::tact::test
