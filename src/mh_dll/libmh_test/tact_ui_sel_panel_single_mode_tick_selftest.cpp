//
// tact_ui_sel_panel_single_mode_tick_selftest.cpp -- offline oracle for
// llm_tact_ui_sel_panel_single_mode_tick @0x004369c1. See
// tact/tact_ui_sel_panel_single_mode_tick.h for the derivation.
//
// This oracle proves the PRIORITY-ORDERED dispatch itself: which one of the ten branches (gate,
// mode-tab click, player-row-list click, 8 scroll buttons, 2 non-exclusive hover/draw checks)
// fires for a given input, the exact arguments each frontier call receives, and the state each
// branch mutates -- not any pixel the eventual (unmocked) renderer would produce.
//
// mouse_in_rect: the real function behind this call (llm_tact_ui_mouse_in_rect @0x00435909) is
// ALREADY migrated and selftested standalone (tact_selftest.cpp's test_pilot_mouse_in_rect --
// half-open [x0,x1) x [y0,y1), low inclusive/high exclusive). The mock below reproduces that exact
// geometry against the fixture's OWN sidebar_mouse_x/y by default, which -- because every branch's
// rect in this function partitions the sidebar into disjoint x/y bands (see the report) -- is
// enough to isolate any one branch just by placing the mouse. The one case that needs a rect
// match the real geometry could never produce (T6, both hover checks firing together, which
// requires simultaneously being in DISJOINT x-ranges [0,0x50) and [0x50,0xa0)) uses an explicit
// forcing flag instead of real geometry, since that is testing the code's structural
// non-exclusivity, not a reachable real mouse position.
//
#include "tact/tact_ui_sel_panel_single_mode_tick.h"
#include "tact_test_support.h"

#include <cstring>

namespace mh::tact::test {

namespace {

struct call_log {
    // LIFT-TACT slice A: the mode-tab blit became LIBMH_EVK_INV_TACT_SEL_PANEL_MODE_TAB. The
    // nine blit arguments are the sink's business now and the UI capture proves them; what still
    // crosses -- and what this log pins -- is that the tab fired and which mode it named.
    int     mode_tab_calls = 0;
    int32_t last_mode      = -1;

    int refresh_calls = 0;

    // mouse_in_rect: full call history so a test can assert exactly which rects were probed
    // (e.g. that the mode-tab branch never calls it at all).
    int     mouse_in_rect_calls = 0;
    int32_t rect_x0_hist[16] = {}, rect_y0_hist[16] = {};
    int32_t rect_x1_hist[16] = {}, rect_y1_hist[16] = {};

    int     draw_player_row_list_calls = 0;
    int32_t last_selected_row          = -1;

    int     draw_left_calls = 0;
    int32_t left_unit_id = -1, left_row = -1;

    int     draw_right_calls = 0;
    int32_t right_unit_id = -1, right_row = -1;
};

call_log &log() {
    static call_log l;
    return l;
}
void reset_log() { log() = call_log{}; }

// Points at the fixture's own sidebar_mouse_x/y for this case -- set once per case, right after
// the fixture is constructed.
const int32_t *g_mouse_x = nullptr;
const int32_t *g_mouse_y = nullptr;

// See the file banner: when set, ANY rect whose y0==0xf0 (i.e. either of the two row-hover checks,
// the only branches that probe that y-band) reports a hit regardless of the real mouse position --
// used ONLY by T6, to force both otherwise x-disjoint hover checks to both see a match.
bool g_force_hover_hit = false;

void mock_sel_panel_mode_tab(int32_t mode) {
    call_log &l = log();
    ++l.mode_tab_calls;
    l.last_mode = mode;
}

void mock_selection_panel_refresh() { ++log().refresh_calls; }

int32_t mock_mouse_in_rect(int32_t rect_x0, int32_t rect_y0, int32_t rect_x1, int32_t rect_y1) {
    call_log &l = log();
    if (l.mouse_in_rect_calls < 16) {
        l.rect_x0_hist[l.mouse_in_rect_calls] = rect_x0;
        l.rect_y0_hist[l.mouse_in_rect_calls] = rect_y0;
        l.rect_x1_hist[l.mouse_in_rect_calls] = rect_x1;
        l.rect_y1_hist[l.mouse_in_rect_calls] = rect_y1;
    }
    ++l.mouse_in_rect_calls;
    if (g_force_hover_hit && rect_y0 == 0xf0) return 1;
    const int32_t mx = *g_mouse_x, my = *g_mouse_y;
    // Same half-open semantics as the real llm_tact_ui_mouse_in_rect (low inclusive, high
    // exclusive) -- see tact_pilot.cpp's detail::ui_mouse_in_rect.
    return (mx >= rect_x0 && my >= rect_y0 && mx < rect_x1 && my < rect_y1) ? 1 : 0;
}

void mock_draw_player_row_list(int32_t selected_row) {
    call_log &l = log();
    ++l.draw_player_row_list_calls;
    l.last_selected_row = selected_row;
}

// LIFT-TACT slice A: the two roster-row draws are one scope with a side now
// (LIBMH_EVK_INV_TACT_SIDEBAR_ROW_HOVER), so these mocks stand for the two SIDES rather than for
// two entries. `highlight_flag` is gone from the signature -- it was the literal 1 at both sites,
// so it is the sink that passes it, and the two checks that used to pin it here went with it.
// Which side fired, for which unit, in which row is what the body still decides and is still
// asserted below.
void mock_sidebar_row_hover_unassigned(int32_t unit_id, int32_t row) {
    call_log &l = log();
    ++l.draw_left_calls;
    l.left_unit_id = unit_id;
    l.left_row     = row;
}

void mock_sidebar_row_hover_group(int32_t unit_id, int32_t row) {
    call_log &l = log();
    ++l.draw_right_calls;
    l.right_unit_id = unit_id;
    l.right_row     = row;
}

ui_sel_panel_single_mode_tick_calls mock_calls() {
    return {mock_sel_panel_mode_tab, mock_selection_panel_refresh,
            mock_mouse_in_rect, mock_draw_player_row_list,
            mock_sidebar_row_hover_unassigned, mock_sidebar_row_hover_group};
}

struct icon_header {
    uint16_t w, h;
};

void put_i32(std::vector<uint8_t> &buf, size_t offset, int32_t v) {
    std::memcpy(buf.data() + offset, &v, sizeof(v));
}

} // namespace

void run_ui_sel_panel_single_mode_tick_tests() {
    // T0: the top gate -- sidebar_ui_hit_code() > 0 means a total no-op, even though the mouse
    // position below would otherwise satisfy the mode-tab click. 0x004369d9-0x004369e0.
    {
        tact_fixture fx;
        g_mouse_x              = &fx.sidebar_mouse_x;
        g_mouse_y              = &fx.sidebar_mouse_y;
        fx.sidebar_ui_hit_code = 7;
        fx.mouse_buttons_cur   = 1;
        fx.sidebar_mouse_x     = 0x10;
        fx.sidebar_mouse_y     = 0xb0; // would hit the mode-tab rect if the gate did not fire first
        reset_log();
        g_force_hover_hit = false;

        tact_store own = fx.store();
        detail::ui_sel_panel_single_mode_tick(fx.view(), own, mock_calls());

        ck_eq((uint32_t)own.sidebar_ui_hit_code(), 7u,
              "T0: gate (hit_code>0) leaves hit_code untouched, 0x004369d9-0x004369e0");
        ck_eq((uint32_t)log().mouse_in_rect_calls, 0u, "T0: gate fires before ANY check runs");
        ck_eq((uint32_t)log().mode_tab_calls, 0u, "T0: gate -- no mode-tab scope");
        ck_eq((uint32_t)log().refresh_calls, 0u, "T0: gate -- no refresh call");
    }

    // T1: the mode-tab click -- exact icon-size read and the literal (x,y)=(0,0xa8). Also proves
    // this branch does NOT call mouse_in_rect at all (unlike every other branch), since its own
    // hit test is four direct comparisons, not a mouse_in_rect call. 0x004369e6-0x00436a89.
    {
        tact_fixture fx;
        g_mouse_x                  = &fx.sidebar_mouse_x;
        g_mouse_y                  = &fx.sidebar_mouse_y;
        fx.sidebar_ui_hit_code     = 0;
        fx.mouse_buttons_cur       = 1;    // == 1, not merely > 0
        fx.sidebar_mouse_x         = 0x10; // < 0x50
        fx.sidebar_mouse_y         = 0xb0; // strictly between 0xa8 and 0xc0
        fx.ui_sel_panel_multi_mode = 7;    // nonzero, so the reset to 0 is observable
        fx.gfx_panel_row_skip      = 42;   // distinct pitch, not 0
        icon_header icon{99, 55};
        fx.sel_panel_icon_gfx[3] = &icon;
        reset_log();
        g_force_hover_hit = false;

        tact_store own = fx.store();
        detail::ui_sel_panel_single_mode_tick(fx.view(), own, mock_calls());

        ck_eq((uint32_t)own.ui_sel_panel_multi_mode(), 0u,
              "T1: mode-tab click resets ui_sel_panel_multi_mode to 0, 0x00436a3c");
        ck_eq((uint32_t)log().mouse_in_rect_calls, 0u,
              "T1: mode-tab hit test is direct comparisons, never calls mouse_in_rect");
        ck_eq((uint32_t)log().mode_tab_calls, 1u,
              "T1: exactly one MODE_TAB scope is emitted, 0x00436a75");
        ck_eq((uint32_t)log().last_mode, 0u,
              "T1: it names the SINGLE mode (icon slot 3). The sibling multi tick emits the\n"
              "        SAME kind with mode=1 -- both draw the same tab at (0,0xa8) and differ\n"
              "        only in icon, which is why one scope serves both");
        ck_eq((uint32_t)log().refresh_calls, 1u, "T1: selection_panel_refresh called once, 0x00436a7a");
        ck_eq((uint32_t)own.sidebar_ui_hit_code(), 2u, "T1: hit_code latched to 2, 0x00436a7f");
    }

    // T2: the player-row-list click -- proves the plain-`/` IDIV truncation (47/20 -> 2, not a
    // rounded/shift result) and that this branch's own gate is mouse_buttons_cur > 0 (not == 1,
    // unlike the mode-tab branch above) by using button value 2. 0x00436a8e-0x00436aeb.
    {
        tact_fixture fx;
        g_mouse_x              = &fx.sidebar_mouse_x;
        g_mouse_y              = &fx.sidebar_mouse_y;
        fx.sidebar_ui_hit_code = 0;
        fx.mouse_buttons_cur   = 2;    // > 0 but != 1 -- proves the ">0" test, not "==1"
        fx.sidebar_mouse_x     = 0x2f; // 47 -- 47/20 truncates to 2, not 2.35 or 3
        fx.sidebar_mouse_y     = 0xc5; // inside [0xc0,0xd8), outside the mode-tab band
        reset_log();
        g_force_hover_hit = false;

        tact_store own = fx.store();
        detail::ui_sel_panel_single_mode_tick(fx.view(), own, mock_calls());

        ck_eq((uint32_t)log().mouse_in_rect_calls, 1u, "T2: exactly one mouse_in_rect probe, 0x00436a9f");
        ck_eq((uint32_t)log().rect_x0_hist[0], 0u, "T2: rect (0,0xc0,0xa0,0xd8) x0, 0x00436a9d");
        ck_eq((uint32_t)log().rect_y0_hist[0], 0xc0u, "T2: rect y0=0xc0, 0x00436a98");
        ck_eq((uint32_t)log().rect_x1_hist[0], 0xa0u, "T2: rect x1=0xa0, 0x00436a93");
        ck_eq((uint32_t)log().rect_y1_hist[0], 0xd8u, "T2: rect y1=0xd8, 0x00436a8e");
        ck_eq((uint32_t)own.sidebar_active_group_id(), 2u,
              "T2: sidebar_active_group_id = mouse_x/0x14 = 47/20 = 2 (IDIV truncation), 0x00436ac7");
        ck_eq((uint32_t)log().draw_player_row_list_calls, 1u,
              "T2: draw_player_row_list called once, 0x00436ad7");
        ck_eq((uint32_t)log().last_selected_row, 2u, "T2: draw_player_row_list's arg is the group id");
        ck_eq((uint32_t)log().refresh_calls, 1u, "T2: selection_panel_refresh called once, 0x00436adc");
        ck_eq((uint32_t)own.sidebar_ui_hit_code(), 10u, "T2: hit_code latched to 10, 0x00436ae1");
    }

    // T3: scroll button i=0 (UNASSIGNED, reset-to-top) -- fires once, sets the debounce bit, and a
    // SECOND tick with the debounce bit already set does NOT re-fire (falls through, and since the
    // mouse is still in the button 0 rect -- below the hover checks' y-band -- nothing else fires
    // either). 0x00436af0-0x00436b4f.
    {
        tact_fixture fx;
        g_mouse_x                        = &fx.sidebar_mouse_x;
        g_mouse_y                        = &fx.sidebar_mouse_y;
        fx.sidebar_ui_hit_code           = 0;
        fx.mouse_buttons_cur             = 3;    // > 0, not 1 -- same family as T2's gate proof
        fx.sidebar_mouse_x               = 0x05; // inside [0,0x14)
        fx.sidebar_mouse_y               = 0xe0; // inside [0xd8,0xf0)
        fx.sidebar_scrollbtn_state[0]    = 2;    // > 0, bit0 clear (not debounced)
        fx.sidebar_unassigned_scroll_row = 9;    // nonzero, so the reset to 0 is observable
        reset_log();
        g_force_hover_hit = false;

        tact_store own = fx.store();
        detail::ui_sel_panel_single_mode_tick(fx.view(), own, mock_calls());

        // TWO mouse_in_rect probes, not one: block 2 (player-row-list) calls mouse_in_rect as the
        // LEFT operand of its own `&&` unconditionally, before checking mouse_buttons_cur -- so it
        // fires (and returns 0, mouse_y=0xe0 is outside [0xc0,0xd8)) even though this case never
        // intends to hit it. Call [1] is button 0's own probe -- see the multi-mode sibling's T4a/T5
        // cases for the identical root cause.
        ck_eq((uint32_t)log().mouse_in_rect_calls, 2u,
              "T3: two mouse_in_rect probes -- block 2's unconditional player-row-list probe "
              "(0x00436a9f, returns 0) THEN button 0's own rect (0x00436b0a)");
        ck_eq((uint32_t)log().rect_x0_hist[0], 0u, "T3: probe[0] is block 2's player-row-list rect x0=0, 0x00436a9d");
        ck_eq((uint32_t)log().rect_y0_hist[0], 0xc0u, "T3: probe[0] y0=0xc0, 0x00436a98");
        ck_eq((uint32_t)log().rect_x1_hist[1], 0x14u, "T3: button 0 rect x1=0x14, 0x00436afe");
        ck_eq((uint32_t)log().rect_y0_hist[1], 0xd8u, "T3: button 0 rect y0=0xd8, 0x00436b03");
        ck_eq((uint32_t)log().rect_y1_hist[1], 0xf0u, "T3: button 0 rect y1=0xf0, 0x00436af9");
        ck_eq((uint32_t)own.sidebar_scrollbtn_state_at(0), 3u,
              "T3: debounce bit 0 set (2|1=3), 0x00436b2a");
        ck_eq((uint32_t)own.sidebar_unassigned_scroll_row(), 0u,
              "T3: button 0 resets sidebar_unassigned_scroll_row to 0, 0x00436b31");
        ck_eq((uint32_t)log().refresh_calls, 1u, "T3: selection_panel_refresh called once, 0x00436b3b");
        ck_eq((uint32_t)own.sidebar_ui_hit_code(), 1u, "T3: hit_code latched to 1, 0x00436b40");

        // T3b: re-tick with the SAME mouse position but the debounce bit already set -- button 0
        // must NOT re-fire (own precondition `(state&1)==0` fails, 0x00436b1f/0x00436b26), and
        // since nothing else in the panel matches this mouse position either, the tick is a total
        // no-op this time.
        fx.sidebar_unassigned_scroll_row = 9; // reseed -- a re-fire would reset this to 0 again
        fx.sidebar_ui_hit_code           = 0;
        reset_log();
        detail::ui_sel_panel_single_mode_tick(fx.view(), own, mock_calls());
        ck_eq((uint32_t)own.sidebar_unassigned_scroll_row(), 9u,
              "T3b: already-debounced button 0 does NOT re-fire -- scroll row unchanged, 0x00436b1f");
        ck_eq((uint32_t)log().refresh_calls, 0u, "T3b: no refresh call on the debounced re-tick");
        ck_eq((uint32_t)own.sidebar_ui_hit_code(), 0u, "T3b: hit_code stays 0 (nothing else matches)");
    }

    // T4: scroll button i=3 (UNASSIGNED, jump-to-bottom) -- reads the UNASSIGNED roster's OWN count
    // field (offset 0) and sidebar_multi_panel_visible_rows, distinct non-round values so a wrong
    // operand order or wrong roster is observable. 0x00436c0b-0x00436c73.
    {
        tact_fixture fx;
        g_mouse_x                           = &fx.sidebar_mouse_x;
        g_mouse_y                           = &fx.sidebar_mouse_y;
        fx.sidebar_ui_hit_code              = 0;
        fx.mouse_buttons_cur                = 3;
        fx.sidebar_mouse_x                  = 0x40; // inside [0x3c,0x50)
        fx.sidebar_mouse_y                  = 0xe5; // inside [0xd8,0xf0)
        fx.sidebar_scrollbtn_state[3]       = 4;    // > 0, bit0 clear
        fx.sidebar_multi_panel_visible_rows = 3;    // distinct, not the fixture's stock 10
        put_i32(fx.unassigned_unit_roster, 0, 37);  // UNASSIGNED count = 37
        put_i32(fx.group_unit_roster, 0, 999);      // decoy: group roster's count, must NOT be read
        reset_log();
        g_force_hover_hit = false;

        tact_store own = fx.store();
        detail::ui_sel_panel_single_mode_tick(fx.view(), own, mock_calls());

        ck_eq((uint32_t)own.sidebar_scrollbtn_state_at(3), 5u,
              "T4: debounce bit 0 set (4|1=5), 0x00436c48");
        ck_eq((uint32_t)own.sidebar_unassigned_scroll_row(), 34u,
              "T4: sidebar_unassigned_scroll_row = UNASSIGNED count(37) - visible_rows(3) = 34, "
              "reads UNASSIGNED not GROUP roster, 0x00436c4f-0x00436c5a");
        ck_eq((uint32_t)log().refresh_calls, 1u, "T4: selection_panel_refresh called once, 0x00436c5f");
        ck_eq((uint32_t)own.sidebar_ui_hit_code(), 1u, "T4: hit_code latched to 1, 0x00436c64");
    }

    // T5: scroll button i=7 (ACTIVE-GROUP, jump-to-bottom) -- reads GROUP_UNIT_ROSTER at the
    // active_group_id<<8 shifted offset, DISTINCT from T4's UNASSIGNED count so a family swap is
    // observable. 0x00436d91-0x00436e02.
    {
        tact_fixture fx;
        g_mouse_x                           = &fx.sidebar_mouse_x;
        g_mouse_y                           = &fx.sidebar_mouse_y;
        fx.sidebar_ui_hit_code              = 0;
        fx.mouse_buttons_cur                = 3;
        fx.sidebar_mouse_x                  = 0x90; // inside [0x8c,0xa0)
        fx.sidebar_mouse_y                  = 0xe0; // inside [0xd8,0xf0)
        fx.sidebar_scrollbtn_state[7]       = 6;    // > 0, bit0 clear
        fx.sidebar_active_group_id          = 2;    // nonzero group, exercises the <<8 offset
        fx.sidebar_multi_panel_visible_rows = 3;
        put_i32(fx.unassigned_unit_roster, 0, 37); // decoy: must NOT be read by this branch
        put_i32(fx.group_unit_roster, 2 << 8, 55); // group 2's count = 55, distinct from 37
        reset_log();
        g_force_hover_hit = false;

        tact_store own = fx.store();
        detail::ui_sel_panel_single_mode_tick(fx.view(), own, mock_calls());

        ck_eq((uint32_t)own.sidebar_scrollbtn_state_at(7), 7u,
              "T5: debounce bit 0 set (6|1=7), 0x00436dce");
        ck_eq((uint32_t)own.sidebar_group_scroll_row(), 52u,
              "T5: sidebar_group_scroll_row = GROUP[2].count(55) - visible_rows(3) = 52, reads the "
              "active-group's OWN shifted offset, not group 0 or the UNASSIGNED roster, "
              "0x00436dd5-0x00436de9");
        ck_eq((uint32_t)log().refresh_calls, 1u, "T5: selection_panel_refresh called once, 0x00436dee");
        ck_eq((uint32_t)own.sidebar_ui_hit_code(), 1u, "T5: hit_code latched to 1, 0x00436df3");
    }

    // T6: the two row-hover/draw checks are NOT mutually exclusive with EACH OTHER and NOT early
    // returns -- both fire in the same tick once every button above has fallen through. This is a
    // genuinely unreachable mouse position in the real game (the two checks' x-ranges [0,0x50) and
    // [0x50,0xa0) are disjoint, so no real cursor position satisfies both), which is exactly why it
    // needs g_force_hover_hit rather than a real mouse coordinate: the point under test is that the
    // SECOND check's own `if` is not gated on whether the first one already fired -- pure code
    // structure, not reachable geometry. 0x00436e02-0x00436f23.
    {
        tact_fixture fx;
        g_mouse_x              = &fx.sidebar_mouse_x;
        g_mouse_y              = &fx.sidebar_mouse_y;
        fx.sidebar_ui_hit_code = 0;
        fx.mouse_buttons_cur   = 1; // both hover checks require == 1
        // A real position outside every earlier rect (x way past the 0-0xa0 panel width), so the
        // mode-tab/player-list/button checks all fail on real geometry regardless of forcing.
        fx.sidebar_mouse_x               = 2000;
        fx.sidebar_mouse_y               = 0xf0; // exactly the hover checks' own y0 -- row (mouse_y-0xf0)/0x18 = 0
        fx.sidebar_active_group_id       = 0;
        fx.sidebar_unassigned_scroll_row = 0;
        fx.sidebar_group_scroll_row      = 0;
        put_i32(fx.unassigned_unit_roster, 0, 50);  // UNASSIGNED count, large enough for row=0
        put_i32(fx.unassigned_unit_roster, 4, 111); // UNASSIGNED row-0 unit id -- distinct
        put_i32(fx.group_unit_roster, 0, 60);       // GROUP[0] count, large enough for row=0
        put_i32(fx.group_unit_roster, 4, 222);      // GROUP[0] row-0 unit id -- distinct
        reset_log();
        g_force_hover_hit = true;

        tact_store own = fx.store();
        detail::ui_sel_panel_single_mode_tick(fx.view(), own, mock_calls());

        ck_eq((uint32_t)log().draw_left_calls, 1u,
              "T6: LEFT hover/draw fires (not gated by RIGHT's outcome), 0x00436e70");
        ck_eq((uint32_t)log().left_unit_id, 111u, "T6: LEFT draws the UNASSIGNED roster's unit id");
        ck_eq((uint32_t)log().left_row, 0u, "T6: LEFT row = (mouse_y-0xf0)/0x18 = 0, 0x00436e3f");
        ck_eq((uint32_t)log().draw_right_calls, 1u,
              "T6: RIGHT hover/draw ALSO fires -- not an early return from LEFT, 0x00436f0c");
        ck_eq((uint32_t)log().right_unit_id, 222u, "T6: RIGHT draws the ACTIVE-GROUP roster's unit id "
                                                   "(distinct from LEFT's -- proves no roster swap)");
        ck_eq((uint32_t)log().right_row, 0u, "T6: RIGHT row = (mouse_y-0xf0)/0x18 = 0, 0x00436ec7");
        // The RIGHT check runs strictly after LEFT and is not itself conditional on LEFT, so its
        // hit_code write is the one left standing.
        ck_eq((uint32_t)own.sidebar_ui_hit_code(), 0x29u,
              "T6: final hit_code is RIGHT's (row+group_scroll_row+0x29=0x29) -- written after "
              "LEFT's, since RIGHT executes unconditionally afterward, 0x00436f1e");
    }

    // T7/T8: the `scroll_row + row + 1 <= count` boundary on the LEFT (UNASSIGNED) list, using a
    // REAL mouse position this time (x=0x10 is inside LEFT's [0,0x50) band and outside RIGHT's
    // [0x50,0xa0), so only LEFT is even geometrically reachable -- no forcing needed).
    {
        // T7: exactly on the boundary -- fires.
        tact_fixture fx;
        g_mouse_x                        = &fx.sidebar_mouse_x;
        g_mouse_y                        = &fx.sidebar_mouse_y;
        fx.sidebar_ui_hit_code           = 0;
        fx.mouse_buttons_cur             = 1;
        fx.sidebar_mouse_x               = 0x10;            // inside [0,0x50)
        fx.sidebar_mouse_y               = 0xf0 + 2 * 0x18; // row = 2
        fx.sidebar_unassigned_scroll_row = 5;
        put_i32(fx.unassigned_unit_roster, 0, 8);                 // scroll_row(5)+row(2)+1 = 8 <= 8 -- fires
        put_i32(fx.unassigned_unit_roster, 4 + (2 + 5) * 4, 321); // the row's unit id
        reset_log();
        g_force_hover_hit = false;

        tact_store own = fx.store();
        detail::ui_sel_panel_single_mode_tick(fx.view(), own, mock_calls());

        ck_eq((uint32_t)log().draw_left_calls, 1u,
              "T7: scroll_row+row+1(8) <= count(8) -- boundary case FIRES, 0x00436e4e");
        ck_eq((uint32_t)log().left_unit_id, 321u, "T7: draws the expected row's unit id");
        ck_eq((uint32_t)log().draw_right_calls, 0u, "T7: RIGHT does not fire (x outside its band)");
        ck_eq((uint32_t)own.sidebar_ui_hit_code(), 2u + 5u + 0x15u,
              "T7: hit_code = row+scroll_row+0x15, 0x00436e82");

        // T8: one past the boundary -- must NOT fire, and hit_code must stay untouched (0).
        tact_fixture fx2;
        g_mouse_x                         = &fx2.sidebar_mouse_x;
        g_mouse_y                         = &fx2.sidebar_mouse_y;
        fx2.sidebar_ui_hit_code           = 0;
        fx2.mouse_buttons_cur             = 1;
        fx2.sidebar_mouse_x               = 0x10;
        fx2.sidebar_mouse_y               = 0xf0 + 2 * 0x18; // row = 2, same as T7
        fx2.sidebar_unassigned_scroll_row = 5;
        put_i32(fx2.unassigned_unit_roster, 0, 7); // scroll_row(5)+row(2)+1 = 8 <= 7 is FALSE
        reset_log();
        g_force_hover_hit = false;

        tact_store own2 = fx2.store();
        detail::ui_sel_panel_single_mode_tick(fx2.view(), own2, mock_calls());

        ck_eq((uint32_t)log().draw_left_calls, 0u,
              "T8: scroll_row+row+1(8) <= count(7) is false -- one past the boundary, does NOT fire, "
              "0x00436e54");
        ck_eq((uint32_t)own2.sidebar_ui_hit_code(), 0u,
              "T8: hit_code stays untouched when the boundary check fails");
    }
}

} // namespace mh::tact::test
