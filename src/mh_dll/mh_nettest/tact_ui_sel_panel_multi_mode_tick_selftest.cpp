//
// tact_ui_sel_panel_multi_mode_tick_selftest.cpp -- offline oracle for
// llm_tact_ui_sel_panel_multi_mode_tick @0x004361b5. See
// tact/tact_ui_sel_panel_multi_mode_tick.h for the derivation.
//
// Shape imitates tact_ui_sel_panel_single_mode_tick_selftest.cpp (the sibling oracle authored the
// same session): recorder mocks, `ck`/`ck_eq` helpers, one braced block per case, every assertion
// message citing an instruction address in the .asm this file was derived from.
//
// mouse_in_rect: the real function behind this call (llm_tact_ui_mouse_in_rect @0x00435909) is
// already migrated/selftested standalone. The mock below reproduces its exact half-open
// [x0,x1) x [y0,y1) geometry against the fixture's OWN sidebar_mouse_x/y, which is enough to
// isolate every branch just by placing the mouse -- unlike the single-mode sibling, THIS function
// has no case that needs a mouse position two disjoint rects could never share, so no forcing flag
// is needed here (see the two "does not return" notes below for why not).
//
// TWO "DOES NOT RETURN" CLAIMS ARE ASM-ONLY PROOFS, not runtime-observed ones, and this is
// deliberate, not a shortcut:
//   * Block 2 (group assign/recall, @0x00436282-0x004363cd) falls into block 3
//     (highlight-on-hover, gated on `mouse_buttons_cur == 0`) only at the INSTRUCTION level --
//     0x004363cd (hit_code=0xa) flows straight into 0x004363d7 (block 3's own CMP) with no
//     intervening JMP/RET. But block 2's OWN entry gate requires `mouse_buttons_cur > 0`
//     (@0x0043629d), which is the logical negation of block 3's gate -- so no real (or forced)
//     mouse-button value can make both fire in the same call; the two conditions are mutually
//     exclusive on a SHARED SCALAR, not on disjoint rects, so the single-mode sibling's
//     rect-forcing trick does not apply here. Verified straight-line in the .asm instead (T3/T4
//     below assert the tail state each arm leaves, which is all that's independently observable).
//   * Block 5c (camera recenter, @0x00436835-0x0043691a) falls into block 6 (middle-click
//     deselect, gated on `mouse_buttons_cur == 2`) the same way: 0x00436924 (hit_code=1) flows
//     straight into 0x0043692e (block 6's own CMP), but block 5's outer gate requires
//     `mouse_buttons_cur == 1` (@0x00436607) -- again mutually exclusive with block 6's `== 2` on
//     the SAME scalar. T12 below asserts 5c's own effects and leaves the asm-fallthrough as the
//     proof of non-return, per the escape hatch the task brief names.
//
#include "tact/tact_ui_sel_panel_multi_mode_tick.h"
#include "tact_test_support.h"

namespace mh::tact::test {

namespace {

struct call_log {
    // LIFT-TACT slice A: the three icon redraws became TWO on_invalidate scopes, so what this
    // log can observe changed shape. The nine blit arguments -- surface, pitch, x, y, the clip
    // rect, the bitmap -- are the sink's business now and are proven by the hosted UI capture;
    // what still crosses, and what these fields pin, is WHICH scope fired and with what payload.
    // That narrowing is the intended effect of the lift, not an oversight (libmh_host_events.h
    // says so next to kinds 10-13).
    int     mode_tab_calls   = 0;
    int32_t last_mode        = -1;
    int     row_toggle_calls = 0;
    int32_t last_which = -1, last_row = -1;

    int refresh_calls = 0; // selection_panel_refresh

    // mouse_in_rect: full call history so a case can assert exactly which rects were probed (e.g.
    // that the mode-tab branch never calls it at all).
    int     mouse_in_rect_calls = 0;
    int32_t rect_x0_hist[16] = {}, rect_y0_hist[16] = {};
    int32_t rect_x1_hist[16] = {}, rect_y1_hist[16] = {};

    int squad_roster_refresh_calls        = 0;
    int selection_clear_unless_ctrl_calls = 0;

    int     draw_player_row_list_calls = 0;
    int32_t last_selected_row          = -1;

    int      enqueue_calls  = 0;
    int32_t  last_unit_id   = -1;
    int32_t  last_op        = -1;
    uint8_t  last_interrupt = 0xff;
    int32_t  last_arg0      = -1;
    uint16_t last_arg1      = 0xffff;
    uint16_t last_arg2      = 0xffff;
    uint16_t last_arg3      = 0xffff;

    int time_get_current_time_calls = 0;

    int vis_map_fill_default_calls = 0;
};

call_log &log() {
    static call_log l;
    return l;
}
void reset_log() { log() = call_log{}; }

// Points at the fixture's own sidebar_mouse_x/y for this case -- set once per case, right after the
// fixture is constructed.
const int32_t *g_mouse_x = nullptr;
const int32_t *g_mouse_y = nullptr;

// The sentinel time_get_current_time() returns for a given case -- default distinct-nonzero so a
// case that forgets to seed it still produces an observably wrong (not accidentally-right) value.
double g_mock_now = 111.222;

void mock_sel_panel_mode_tab(int32_t mode) {
    call_log &l = log();
    ++l.mode_tab_calls;
    l.last_mode = mode;
}

void mock_sel_panel_row_toggle(int32_t which, int32_t row) {
    call_log &l = log();
    ++l.row_toggle_calls;
    l.last_which = which;
    l.last_row   = row;
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
    const int32_t mx = *g_mouse_x, my = *g_mouse_y;
    // Same half-open semantics as the real llm_tact_ui_mouse_in_rect (low inclusive, high
    // exclusive) -- see tact_pilot.cpp's detail::ui_mouse_in_rect.
    return (mx >= rect_x0 && my >= rect_y0 && mx < rect_x1 && my < rect_y1) ? 1 : 0;
}

void mock_squad_roster_refresh() { ++log().squad_roster_refresh_calls; }
void mock_selection_clear_unless_ctrl() { ++log().selection_clear_unless_ctrl_calls; }

void mock_ui_draw_player_row_list(int32_t selected_row) {
    call_log &l = log();
    ++l.draw_player_row_list_calls;
    l.last_selected_row = selected_row;
}

int32_t mock_unit_enqueue_command(int32_t unit_id, int32_t op, uint8_t interrupt_flag, int32_t arg0,
                                  uint16_t arg1, uint16_t arg2, uint16_t arg3) {
    call_log &l = log();
    ++l.enqueue_calls;
    l.last_unit_id   = unit_id;
    l.last_op        = op;
    l.last_interrupt = interrupt_flag;
    l.last_arg0      = arg0;
    l.last_arg1      = arg1;
    l.last_arg2      = arg2;
    l.last_arg3      = arg3;
    return 0;
}

double mock_time_get_current_time() {
    ++log().time_get_current_time_calls;
    return g_mock_now;
}

void mock_vis_map_fill_default() { ++log().vis_map_fill_default_calls; }

ui_sel_panel_multi_mode_tick_calls mock_calls() {
    return {mock_sel_panel_mode_tab,
            mock_sel_panel_row_toggle,
            mock_selection_panel_refresh,
            mock_mouse_in_rect,
            mock_squad_roster_refresh,
            mock_selection_clear_unless_ctrl,
            mock_ui_draw_player_row_list,
            mock_unit_enqueue_command,
            mock_time_get_current_time,
            mock_vis_map_fill_default};
}

struct icon_header {
    uint16_t w, h;
};

} // namespace

void run_ui_sel_panel_multi_mode_tick_tests() {
    // T0: the top gate -- sidebar_ui_hit_code() > 0 means a total no-op, even though the mouse
    // position below would otherwise satisfy the mode-tab click. 0x004361cd-0x004361d4.
    {
        tact_fixture fx;
        g_mouse_x              = &fx.sidebar_mouse_x;
        g_mouse_y              = &fx.sidebar_mouse_y;
        fx.sidebar_ui_hit_code = 5;
        fx.mouse_buttons_cur   = 1;
        fx.sidebar_mouse_x     = 0x60; // would hit the mode-tab rect if the gate did not fire first
        fx.sidebar_mouse_y     = 0xb0;
        reset_log();

        tact_store own = fx.store();
        detail::ui_sel_panel_multi_mode_tick(fx.view(), own, mock_calls());

        ck_eq((uint32_t)own.sidebar_ui_hit_code(), 5u,
              "T0: gate (hit_code>0) leaves hit_code untouched, 0x004361cd-0x004361d4");
        ck_eq((uint32_t)log().mouse_in_rect_calls, 0u, "T0: gate fires before ANY check runs");
        ck_eq((uint32_t)log().mode_tab_calls, 0u, "T0: gate -- no mode-tab scope");
        ck_eq((uint32_t)log().row_toggle_calls, 0u, "T0: gate -- no row-toggle scope");
        ck_eq((uint32_t)log().refresh_calls, 0u, "T0: gate -- no refresh call");
        ck_eq((uint32_t)log().squad_roster_refresh_calls, 0u, "T0: gate -- no squad_roster_refresh");
        ck_eq((uint32_t)log().enqueue_calls, 0u, "T0: gate -- no unit_enqueue_command");
        ck_eq((uint32_t)log().vis_map_fill_default_calls, 0u, "T0: gate -- no vis_map_fill_default");
    }

    // T1: MODE-TAB CLICK -- exact icon slot 4, position (0,0xa8), size read from the icon header,
    // hit_code=2, and (unlike every other branch) NEVER calls mouse_in_rect -- its own hit test is
    // four direct comparisons. 0x004361da-0x0043627d.
    {
        tact_fixture fx;
        g_mouse_x                  = &fx.sidebar_mouse_x;
        g_mouse_y                  = &fx.sidebar_mouse_y;
        fx.sidebar_ui_hit_code     = 0;
        fx.mouse_buttons_cur       = 1;    // == 1
        fx.sidebar_mouse_x         = 0x60; // >= 0x50
        fx.sidebar_mouse_y         = 0xb0; // strictly between 0xa8 and 0xc0
        fx.ui_sel_panel_multi_mode = 0;
        fx.gfx_panel_row_skip      = 77; // distinct pitch, not 0
        icon_header icon{123, 64};
        fx.sel_panel_icon_gfx[4] = &icon;
        reset_log();

        tact_store own = fx.store();
        detail::ui_sel_panel_multi_mode_tick(fx.view(), own, mock_calls());

        ck_eq((uint32_t)own.ui_sel_panel_multi_mode(), 1u,
              "T1: mode-tab click sets ui_sel_panel_multi_mode=1, 0x00436230");
        ck_eq((uint32_t)log().mouse_in_rect_calls, 0u,
              "T1: mode-tab hit test is direct comparisons, never calls mouse_in_rect");
        ck_eq((uint32_t)log().mode_tab_calls, 1u,
              "T1: exactly one MODE_TAB scope is emitted, 0x00436269");
        ck_eq((uint32_t)log().last_mode, 1u,
              "T1: it names the MULTI mode (icon slot 4) -- the tab the panel switched TO is\n"
              "        what crosses now; the (0,0xa8) origin and the icon's own clip dims moved\n"
              "        into mh.dll's sink with the blit and are proven by the UI capture");
        ck_eq((uint32_t)log().row_toggle_calls, 0u, "T1: the tab click emits no row toggle");
        ck_eq((uint32_t)log().refresh_calls, 1u, "T1: selection_panel_refresh called once, 0x0043626e");
        ck_eq((uint32_t)own.sidebar_ui_hit_code(), 2u, "T1: hit_code latched to 2, 0x00436273");
    }

    // T2: GROUP ASSIGN, RIGHT button (==2). Only owner-0, non-empty, SELECTED units get
    // squad_group_id set; a decoy unselected owner-0 unit and a decoy selected non-owner-0 unit
    // must NOT be touched. group_row = mouse_x/0x14 (plain IDIV). 0x00436282-0x0043633e.
    {
        tact_fixture fx;
        g_mouse_x              = &fx.sidebar_mouse_x;
        g_mouse_y              = &fx.sidebar_mouse_y;
        fx.sidebar_ui_hit_code = 0;
        fx.mouse_buttons_cur   = 2;    // RIGHT
        fx.sidebar_mouse_x     = 0x23; // 35 -- 35/20 = 1 (IDIV truncation)
        fx.sidebar_mouse_y     = 0xc5; // inside [0xc0,0xd8)

        tact_unit &owned_selected       = fx.units[2];
        tact_unit &owned_unselected     = fx.units[3]; // decoy: not selected
        tact_unit &foreign_selected     = fx.units[4]; // decoy: owner != 0
        owned_selected.owner            = 0;
        owned_selected.type             = 1;
        owned_selected.status           = 1; // selected
        owned_selected.squad_group_id   = 9;
        owned_unselected.owner          = 0;
        owned_unselected.type           = 1;
        owned_unselected.status         = 0; // NOT selected
        owned_unselected.squad_group_id = 9;
        foreign_selected.owner          = 7; // != 0
        foreign_selected.type           = 1;
        foreign_selected.status         = 1; // selected, but wrong owner
        foreign_selected.squad_group_id = 9;
        reset_log();

        tact_store own = fx.store();
        detail::ui_sel_panel_multi_mode_tick(fx.view(), own, mock_calls());

        ck_eq((uint32_t)own.sidebar_active_group_id(), 1u,
              "T2: sidebar_active_group_id = mouse_x/0x14 = 35/20 = 1, 0x004362ab-0x004362c6");
        ck_eq((uint32_t)own.unit_at(2).squad_group_id, 1u,
              "T2: owner-0 SELECTED unit gets squad_group_id=group_row, 0x00436329-0x00436336");
        ck_eq((uint32_t)own.unit_at(3).squad_group_id, 9u,
              "T2: owner-0 UNSELECTED decoy untouched, 0x00436314-0x00436325");
        ck_eq((uint32_t)own.unit_at(4).squad_group_id, 9u,
              "T2: owner!=0 SELECTED decoy untouched, 0x004362f9-0x00436300");
        ck_eq((uint32_t)log().squad_roster_refresh_calls, 1u,
              "T2: squad_roster_refresh called once (assign arm), 0x0043633e");
        ck_eq((uint32_t)log().selection_clear_unless_ctrl_calls, 0u,
              "T2: selection_clear_unless_ctrl NOT called on the assign arm");
        ck_eq((uint32_t)log().refresh_calls, 1u, "T2: selection_panel_refresh (tail), 0x004363c0");
        ck_eq((uint32_t)log().draw_player_row_list_calls, 1u, "T2: ui_draw_player_row_list (tail), 0x004363c8");
        ck_eq((uint32_t)log().last_selected_row, 1u, "T2: its arg is group_row, 0x004363c5");
        ck_eq((uint32_t)own.sidebar_ui_hit_code(), 0xau, "T2: hit_code latched to 0xa, 0x004363cd");
    }

    // T3: GROUP RECALL, any OTHER button (here 1, i.e. LEFT). selection_clear_unless_ctrl() runs
    // first, then EVERY owner-0 unit matching group_row gets selected -- no early break (2 matching
    // units seeded, BOTH must end up selected). A wrong-group decoy and a wrong-owner decoy (both
    // otherwise matching) must stay unselected. 0x00436348-0x004363be.
    {
        tact_fixture fx;
        g_mouse_x              = &fx.sidebar_mouse_x;
        g_mouse_y              = &fx.sidebar_mouse_y;
        fx.sidebar_ui_hit_code = 0;
        fx.mouse_buttons_cur   = 1;    // any button OTHER than 2
        fx.sidebar_mouse_x     = 0x46; // 70 -- 70/20 = 3
        fx.sidebar_mouse_y     = 0xc9; // inside [0xc0,0xd8)

        tact_unit &match_a         = fx.units[5];
        tact_unit &match_b         = fx.units[6];
        tact_unit &wrong_group     = fx.units[7]; // decoy: squad_group_id != group_row
        tact_unit &wrong_owner     = fx.units[8]; // decoy: owner != 0, squad_group_id == group_row
        match_a.owner              = 0;
        match_a.type               = 1;
        match_a.squad_group_id     = 3;
        match_a.status             = 0;
        match_b.owner              = 0;
        match_b.type               = 1;
        match_b.squad_group_id     = 3;
        match_b.status             = 0;
        wrong_group.owner          = 0;
        wrong_group.type           = 1;
        wrong_group.squad_group_id = 2;
        wrong_group.status         = 0;
        wrong_owner.owner          = 5;
        wrong_owner.type           = 1;
        wrong_owner.squad_group_id = 3;
        wrong_owner.status         = 0;
        reset_log();

        tact_store own = fx.store();
        detail::ui_sel_panel_multi_mode_tick(fx.view(), own, mock_calls());

        ck_eq((uint32_t)own.sidebar_active_group_id(), 3u,
              "T3: sidebar_active_group_id = 70/20 = 3, 0x004362ab-0x004362c6");
        ck_eq((uint32_t)log().selection_clear_unless_ctrl_calls, 1u,
              "T3: selection_clear_unless_ctrl called once (recall arm), 0x00436348");
        ck_eq((uint32_t)(own.unit_at(5).status & 1), 1u,
              "T3: matching unit A gets selected, 0x004363a1-0x004363b8");
        ck_eq((uint32_t)(own.unit_at(6).status & 1), 1u,
              "T3: matching unit B ALSO gets selected -- no early break, 0x004363a1-0x004363b8");
        ck_eq((uint32_t)(own.unit_at(7).status & 1), 0u,
              "T3: wrong-group decoy stays unselected, 0x00436389-0x0043639d");
        ck_eq((uint32_t)(own.unit_at(8).status & 1), 0u,
              "T3: wrong-owner decoy stays unselected, 0x0043637e-0x00436385");
        ck_eq((uint32_t)log().squad_roster_refresh_calls, 0u,
              "T3: squad_roster_refresh NOT called on the recall arm");
        ck_eq((uint32_t)log().refresh_calls, 1u, "T3: selection_panel_refresh (tail), 0x004363c0");
        ck_eq((uint32_t)log().last_selected_row, 3u,
              "T3: ui_draw_player_row_list arg is group_row, 0x004363c5-0x004363c8");
        ck_eq((uint32_t)own.sidebar_ui_hit_code(), 0xau, "T3: hit_code latched to 0xa, 0x004363cd");
        // Block 2 does NOT return (falls into block 3's own gate at 0x004363d7) -- but that gate
        // requires mouse_buttons_cur==0 while block 2's own entry required >0, so no button value
        // can make block 3 ALSO observably fire here; see the file banner. This case's own
        // assertions above (which run past 0x004363cd with no crash) are what "does not return"
        // means at runtime; the fallthrough itself is asm-only, per the banner.
    }

    // T4: HIGHLIGHT-ON-HOVER, only when NO button held. A positive slot latches the highlight; a
    // <=0 slot leaves the PREVIOUS highlight untouched. 0x004363d7-0x00436445.
    {
        // T4a: slot <= 0 -- highlight must NOT change from its seeded prior value.
        tact_fixture fx;
        g_mouse_x                      = &fx.sidebar_mouse_x;
        g_mouse_y                      = &fx.sidebar_mouse_y;
        fx.sidebar_ui_hit_code         = 0;
        fx.mouse_buttons_cur           = 0; // NO button
        fx.sidebar_mouse_x             = 0x10;
        fx.sidebar_mouse_y             = 0xf5; // row=(0xf5-0xf0)/0x30=0
        fx.sidebar_slot_scroll         = 0;
        fx.sidebar_highlighted_unit_id = 77; // prior value, must survive
        fx.sidebar_slot_unit_ids[0]    = -3; // <= 0
        reset_log();

        tact_store own = fx.store();
        detail::ui_sel_panel_multi_mode_tick(fx.view(), own, mock_calls());

        // TWO probes, not one: block 2 (group assign/recall) calls ui_mouse_in_rect(0,0xc0,0xa0,0xd8)
        // as the LEFT operand of its own `&&` gate UNCONDITIONALLY -- it is called before
        // `mouse_buttons_cur>0` is even checked, so it fires here too (returns 0, mouse_y=0xf5 is
        // outside [0xc0,0xd8)) even though this case targets block 3. Call [1] is block 3's own
        // probe -- the single-mode sibling's T3 case has the identical root cause.
        ck_eq((uint32_t)log().mouse_in_rect_calls, 2u,
              "T4a: two mouse_in_rect probes -- block 2's unconditional group-rect probe "
              "(0x0043629d, returns 0) THEN block 3's own rect (0x004363f2)");
        ck_eq((uint32_t)log().rect_x0_hist[0], 0u, "T4a: probe[0] is block 2's group rect x0=0, 0x00436288");
        ck_eq((uint32_t)log().rect_y0_hist[0], 0xc0u, "T4a: probe[0] y0=0xc0, 0x00436293");
        ck_eq((uint32_t)log().rect_x0_hist[1], 0u, "T4a: rect (0,0xf0,0xa0,height) x0, 0x004363e0-0x004363e6");
        ck_eq((uint32_t)log().rect_y0_hist[1], 0xf0u, "T4a: rect y0=0xf0, 0x004363eb");
        ck_eq((uint32_t)log().rect_x1_hist[1], 0xa0u, "T4a: rect x1=0xa0, 0x004363e6");
        ck_eq((uint32_t)log().rect_y1_hist[1], (uint32_t)fx.window_height,
              "T4a: rect y1=window_height(), 0x004363e0-0x004363e2");
        ck_eq((uint32_t)(int32_t)own.sidebar_highlighted_unit_id(), 77u,
              "T4a: slot<=0 leaves the PREVIOUS highlight untouched, 0x0043642c");

        // T4b: same fixture, now the slot holds a positive id -- highlight latches to it.
        fx.sidebar_slot_unit_ids[0] = 42;
        reset_log();
        detail::ui_sel_panel_multi_mode_tick(fx.view(), own, mock_calls());
        ck_eq((uint32_t)own.sidebar_highlighted_unit_id(), 42u,
              "T4b: positive slot latches sidebar_highlighted_unit_id, 0x00436440");
    }

    // T5: SCROLL BUTTON 0 (reset-to-top), with its debounce bit -- fires once, sets bit 0, and a
    // SECOND tick with the bit already set does NOT re-fire. 0x00436445-0x0043649f.
    {
        tact_fixture fx;
        g_mouse_x                       = &fx.sidebar_mouse_x;
        g_mouse_y                       = &fx.sidebar_mouse_y;
        fx.sidebar_ui_hit_code          = 0;
        fx.mouse_buttons_cur            = 1;    // > 0
        fx.sidebar_mouse_x              = 0x10; // inside [0,0x28)
        fx.sidebar_mouse_y              = 0xe0; // inside [0xd8,0xf0)
        fx.sel_panel_icon_slot_state[0] = 4;    // > 0, bit0 clear
        fx.sidebar_slot_scroll          = 77;   // nonzero, so the reset to 0 is observable
        reset_log();

        tact_store own = fx.store();
        detail::ui_sel_panel_multi_mode_tick(fx.view(), own, mock_calls());

        // TWO probes: block 2's own unconditional group-rect probe fires first (mouse_buttons_cur=1
        // satisfies its `>0` half, but the rect (0,0xc0,0xa0,0xd8) does not contain (0x10,0xe0), so
        // it returns 0) before button 0's own probe. Same root cause as T4a above.
        ck_eq((uint32_t)log().mouse_in_rect_calls, 2u,
              "T5: two mouse_in_rect probes -- block 2's unconditional group-rect probe (0x0043629d) "
              "THEN button 0's own rect (0x0043645f)");
        ck_eq((uint32_t)log().rect_x1_hist[1], 0x28u, "T5: button0 rect x1=0x28, 0x00436453");
        ck_eq((uint32_t)log().rect_y0_hist[1], 0xd8u, "T5: button0 rect y0=0xd8, 0x00436458");
        ck_eq((uint32_t)log().rect_y1_hist[1], 0xf0u, "T5: button0 rect y1=0xf0, 0x0043644e");
        ck_eq((uint32_t)own.sel_panel_icon_slot_state_at(0), 5u,
              "T5: debounce bit 0 set (4|1=5), 0x0043647f");
        ck_eq((uint32_t)own.sidebar_slot_scroll(), 0u,
              "T5: button0 resets sidebar_slot_scroll to 0, 0x00436486");
        ck_eq((uint32_t)log().refresh_calls, 1u, "T5: selection_panel_refresh called once, 0x00436490");
        ck_eq((uint32_t)own.sidebar_ui_hit_code(), 1u, "T5: hit_code latched to 1, 0x00436495");

        // Re-tick with the SAME mouse position but the debounce bit already set -- button 0 must
        // NOT re-fire (precondition `(state&1)==0` fails, 0x00436474/0x0043647b), and since the
        // mouse position matches nothing else in the panel, the tick is a total no-op.
        fx.sidebar_slot_scroll = 77; // reseed -- a re-fire would reset this to 0 again
        fx.sidebar_ui_hit_code = 0;
        reset_log();
        detail::ui_sel_panel_multi_mode_tick(fx.view(), own, mock_calls());
        ck_eq((uint32_t)own.sidebar_slot_scroll(), 77u,
              "T5b: already-debounced button 0 does NOT re-fire -- scroll unchanged, 0x00436474");
        ck_eq((uint32_t)log().refresh_calls, 0u, "T5b: no refresh call on the debounced re-tick");
        ck_eq((uint32_t)own.sidebar_ui_hit_code(), 0u, "T5b: hit_code stays 0 (nothing else matches)");
    }

    // T6: SCROLL BUTTON 3 (jump-to-bottom) -- scroll = FRESH count of selected units (status bit 0,
    // slots 1..0x80) minus sidebar_slot_visible_count. Some units are ALREADY selected before the
    // call and sidebar_slot_scroll is seeded to a value inconsistent with any cached count, so a
    // wrong (cached/stale) count would be caught. 0x00436560-0x00436602.
    {
        tact_fixture fx;
        g_mouse_x                       = &fx.sidebar_mouse_x;
        g_mouse_y                       = &fx.sidebar_mouse_y;
        fx.sidebar_ui_hit_code          = 0;
        fx.mouse_buttons_cur            = 1;
        fx.sidebar_mouse_x              = 0x80; // inside [0x78,0xa0)
        fx.sidebar_mouse_y              = 0xe0; // inside [0xd8,0xf0)
        fx.sel_panel_icon_slot_state[3] = 6;    // > 0, bit0 clear
        fx.sidebar_slot_scroll          = 999;  // must be overwritten, not merely adjusted
        fx.sidebar_slot_visible_count   = 1;    // distinct, not the fixture's stock 5
        // 4 units selected among the walked slots 1..0x80 -- a fresh count must see exactly 4.
        fx.units[10].type   = 1;
        fx.units[10].status = 1;
        fx.units[11].type   = 1;
        fx.units[11].status = 1;
        fx.units[12].type   = 1;
        fx.units[12].status = 1;
        fx.units[13].type   = 1;
        fx.units[13].status = 1;
        reset_log();

        tact_store own = fx.store();
        detail::ui_sel_panel_multi_mode_tick(fx.view(), own, mock_calls());

        ck_eq((uint32_t)own.sel_panel_icon_slot_state_at(3), 7u,
              "T6: debounce bit 0 set (6|1=7), 0x004365a0");
        ck_eq((uint32_t)own.sidebar_slot_scroll(), 3u,
              "T6: scroll = freshly-counted selected(4) - visible_count(1) = 3, "
              "0x004365ae-0x004365ed");
        ck_eq((uint32_t)log().refresh_calls, 1u, "T6: selection_panel_refresh called once, 0x004365f3");
        ck_eq((uint32_t)own.sidebar_ui_hit_code(), 1u, "T6: hit_code latched to 1, 0x004365f8");
    }

    // T7: ROW-LIST CLICK -- DEFENSE-STANCE TOGGLE, wrap 4->0. Neither the enqueue (needs new==4)
    // nor the time restamp (needs new==3) fires on the wrap. Also proves the 5 tile_vis_map writes
    // at (row*2+11)*view_tiles_w()+col-6 and (row*2+12)*view_tiles_w()+col-6, cols 0..4 -- with
    // canaries just outside surviving untouched. 0x00436607-0x0043679f.
    {
        tact_fixture fx;
        g_mouse_x                   = &fx.sidebar_mouse_x;
        g_mouse_y                   = &fx.sidebar_mouse_y;
        fx.sidebar_ui_hit_code      = 0;
        fx.mouse_buttons_cur        = 1;     // LEFT
        fx.sidebar_mouse_x          = 0x90;  // inside defense rect [0x84,0x9a)
        fx.sidebar_mouse_y          = 0x100; // row=(0x100-0xf0)/0x30=0; inside [0xf7,0x10d)
        fx.sidebar_slot_scroll      = 0;
        fx.sidebar_slot_unit_ids[0] = 7; // unit_idx
        fx.units[7].def_stat        = 4; // wraps to 0
        fx.units[7].active_gun      = 0;
        fx.gfx_panel_row_skip       = 55;
        fx.view_tiles_w             = 10;
        icon_header icon{11, 22};
        fx.sel_panel_icon_gfx[0x26] = &icon;
        // Canary bytes just outside the 5-wide write windows on both rows (row=0: rows 11 and 12,
        // base 110 and 120, writes land at [104..108] and [114..118] with view_tiles_w=10).
        fx.tile_vis_map[103] = 0xAA;
        fx.tile_vis_map[109] = 0xAA;
        fx.tile_vis_map[113] = 0xAA;
        fx.tile_vis_map[119] = 0xAA;
        reset_log();

        tact_store own = fx.store();
        detail::ui_sel_panel_multi_mode_tick(fx.view(), own, mock_calls());

        // THREE probes: block 2's own unconditional group-rect probe (mouse_buttons_cur=1 satisfies
        // its `>0` half but (0x90,0x100) is outside (0,0xc0,0xa0,0xd8), returns 0) fires before the
        // outer row-list probe and the inner defense-rect probe. Same root cause as T4a/T5 above.
        ck_eq((uint32_t)log().mouse_in_rect_calls, 3u,
              "T7: block 2's unconditional group-rect probe (0x0043629d) + outer row-list probe + "
              "defense-rect probe, 0x00436622/0x00436689");
        ck_eq((uint32_t)own.unit_at(7).def_stat, 0u,
              "T7: def_stat wraps 4->0 (not INC'd to 5), 0x004366da-0x004366ea");
        ck_eq((uint32_t)log().enqueue_calls, 0u, "T7: new value 0 != 4 -- no unit_enqueue_command");
        ck_eq((uint32_t)log().time_get_current_time_calls, 0u,
              "T7: new value 0 != 3 -- no wander_check_time restamp");
        ck_eq((uint32_t)own.sidebar_ui_hit_code(), 100u, "T7: hit_code = row(0)+100, 0x00436749-0x0043674f");
        ck_eq((uint32_t)log().row_toggle_calls, 1u,
              "T7: exactly one ROW_TOGGLE scope, 0x004366ce");
        ck_eq((uint32_t)log().last_which, 0u, "T7: it is the DEFENSE toggle (was icon slot 0x26)");
        ck_eq((uint32_t)log().last_row, 0u,
              "T7: it carries the ROW, not a y -- the original's row*0x30+0xf7 is panel layout\n"
              "        and stays with the sink");
        const int32_t row_index = 0; // matches sidebar_mouse_y's computed row above
        for (int32_t col = 0; col < 5; ++col) {
            ck_eq((uint32_t)own.tile_vis_map_at((row_index * 2 + 11) * fx.view_tiles_w + col - 6), 2u,
                  "T7: row*2+11 tile_vis_map write, 0x0043677d");
            ck_eq((uint32_t)own.tile_vis_map_at((row_index * 2 + 12) * fx.view_tiles_w + col - 6), 2u,
                  "T7: row*2+12 tile_vis_map write, 0x00436796");
        }
        ck_eq((uint32_t)own.tile_vis_map_at(103), 0xAAu, "T7: canary just before row-11 window untouched");
        ck_eq((uint32_t)own.tile_vis_map_at(109), 0xAAu, "T7: canary just after row-11 window untouched");
        ck_eq((uint32_t)own.tile_vis_map_at(113), 0xAAu, "T7: canary just before row-12 window untouched");
        ck_eq((uint32_t)own.tile_vis_map_at(119), 0xAAu, "T7: canary just after row-12 window untouched");
    }

    // T8: DEFENSE-STANCE -> 3 -- the wander_check_time restamp fires (and ONLY it); the enqueue
    // does not. 0x00436727-0x00436743.
    {
        tact_fixture fx;
        g_mouse_x                   = &fx.sidebar_mouse_x;
        g_mouse_y                   = &fx.sidebar_mouse_y;
        fx.sidebar_ui_hit_code      = 0;
        fx.mouse_buttons_cur        = 1;
        fx.sidebar_mouse_x          = 0x90;
        fx.sidebar_mouse_y          = 0x100;
        fx.sidebar_slot_scroll      = 0;
        fx.sidebar_slot_unit_ids[0] = 9;
        fx.units[9].def_stat        = 2; // -> 3
        fx.view_tiles_w             = 10;
        icon_header icon{5, 6}; // the redraw's icon header -- read unconditionally before the def_stat dispatch
        fx.sel_panel_icon_gfx[0x26] = &icon;
        g_mock_now                  = 123.456;
        reset_log();

        tact_store own = fx.store();
        detail::ui_sel_panel_multi_mode_tick(fx.view(), own, mock_calls());

        ck_eq((uint32_t)own.unit_at(9).def_stat, 3u, "T8: def_stat 2->3 (INC), 0x004366fa");
        ck_eq((uint32_t)log().enqueue_calls, 0u, "T8: new value 3 != 4 -- no unit_enqueue_command");
        ck_eq((uint32_t)log().time_get_current_time_calls, 1u,
              "T8: new value 3 -- time_GetCurrentTime called, 0x00436737");
        ck_eq_d(own.unit_at(9).wander_check_time, 123.456,
                "T8: wander_check_time restamped to the mocked now, 0x00436743");
    }

    // T9: DEFENSE-STANCE -> 4 -- unit_enqueue_command(unit_idx,4,0,0,0,0,0) fires (op 4 = "kneel");
    // the restamp does not. 0x00436710-0x00436722.
    {
        tact_fixture fx;
        g_mouse_x                      = &fx.sidebar_mouse_x;
        g_mouse_y                      = &fx.sidebar_mouse_y;
        fx.sidebar_ui_hit_code         = 0;
        fx.mouse_buttons_cur           = 1;
        fx.sidebar_mouse_x             = 0x90;
        fx.sidebar_mouse_y             = 0x100;
        fx.sidebar_slot_scroll         = 0;
        fx.sidebar_slot_unit_ids[0]    = 15;
        fx.units[15].def_stat          = 3;   // -> 4
        fx.units[15].wander_check_time = 9.0; // must NOT change (new value 4, not 3)
        fx.view_tiles_w                = 10;
        icon_header icon{7, 8}; // the redraw's icon header -- read unconditionally before the def_stat dispatch
        fx.sel_panel_icon_gfx[0x26] = &icon;
        reset_log();

        tact_store own = fx.store();
        detail::ui_sel_panel_multi_mode_tick(fx.view(), own, mock_calls());

        ck_eq((uint32_t)own.unit_at(15).def_stat, 4u, "T9: def_stat 3->4 (INC), 0x004366fa");
        ck_eq((uint32_t)log().enqueue_calls, 1u, "T9: new value 4 -- unit_enqueue_command, 0x00436722");
        ck_eq((uint32_t)log().last_unit_id, 15u, "T9: enqueue's unit_id is unit_idx, 0x0043671f");
        ck_eq((uint32_t)log().last_op, 4u, "T9: enqueue's op is the literal 4, 0x0043671a");
        // The remaining five params (interrupt_flag, arg0, arg1, arg2, arg3) are all the literal 0
        // -- two XOR self-clears (EBX/ECX @0x00436716-0x00436718) plus three PUSH 0 (@0x00436710-
        // 0x00436714) -- so every one of them is asserted, but not pinned to an individual address
        // within that block since all five are the same literal.
        ck_eq((uint32_t)log().last_interrupt, 0u, "T9: interrupt_flag literal 0, 0x00436710-0x00436722");
        ck_eq((uint32_t)log().last_arg0, 0u, "T9: arg0 literal 0, 0x00436710-0x00436722");
        ck_eq((uint32_t)log().last_arg1, 0u, "T9: arg1 literal 0, 0x00436710-0x00436722");
        ck_eq((uint32_t)log().last_arg2, 0u, "T9: arg2 literal 0, 0x00436710-0x00436722");
        ck_eq((uint32_t)log().last_arg3, 0u, "T9: arg3 literal 0, 0x00436710-0x00436722");
        ck_eq((uint32_t)log().time_get_current_time_calls, 0u,
              "T9: new value 4 != 3 -- no restamp call");
        ck_eq_d(own.unit_at(15).wander_check_time, 9.0, "T9: wander_check_time left untouched");
    }

    // T10/T11: ROW-LIST CLICK -- GUN TOGGLE, both XOR directions. 0x004367a4-0x00436830.
    {
        // T10: 0 -> 1.
        tact_fixture fx;
        g_mouse_x                   = &fx.sidebar_mouse_x;
        g_mouse_y                   = &fx.sidebar_mouse_y;
        fx.sidebar_ui_hit_code      = 0;
        fx.mouse_buttons_cur        = 1;
        fx.sidebar_mouse_x          = 0x50;  // inside gun rect [0x44,0x5a)
        fx.sidebar_mouse_y          = 0x110; // row=(0x110-0xf0)/0x30=0; inside [0x101,0x117)
        fx.sidebar_slot_scroll      = 0;
        fx.sidebar_slot_unit_ids[0] = 21;
        fx.units[21].active_gun     = 0;
        fx.gfx_panel_row_skip       = 66;
        icon_header icon{33, 44};
        fx.sel_panel_icon_gfx[0x27] = &icon;
        reset_log();

        tact_store own = fx.store();
        detail::ui_sel_panel_multi_mode_tick(fx.view(), own, mock_calls());

        ck_eq((uint32_t)own.unit_at(21).active_gun, 1u, "T10: active_gun 0->1 (XOR 1), 0x0043681f");
        ck_eq((uint32_t)own.sidebar_ui_hit_code(), 0x78u, "T10: hit_code = row(0)+0x78, 0x00436825-0x0043682b");
        ck_eq((uint32_t)log().row_toggle_calls, 1u,
              "T10: exactly one ROW_TOGGLE scope, 0x00436803");
        ck_eq((uint32_t)log().last_which, 1u, "T10: it is the GUN toggle (was icon slot 0x27)");
        ck_eq((uint32_t)log().last_row, 0u, "T10: carrying the row, same as T7");

        // T11: 1 -> 0, fresh fixture.
        tact_fixture fx2;
        g_mouse_x                    = &fx2.sidebar_mouse_x;
        g_mouse_y                    = &fx2.sidebar_mouse_y;
        fx2.sidebar_ui_hit_code      = 0;
        fx2.mouse_buttons_cur        = 1;
        fx2.sidebar_mouse_x          = 0x50;
        fx2.sidebar_mouse_y          = 0x110;
        fx2.sidebar_slot_scroll      = 0;
        fx2.sidebar_slot_unit_ids[0] = 22;
        fx2.units[22].active_gun     = 1;
        icon_header icon2{55, 66};
        fx2.sel_panel_icon_gfx[0x27] = &icon2;
        reset_log();

        tact_store own2 = fx2.store();
        detail::ui_sel_panel_multi_mode_tick(fx2.view(), own2, mock_calls());

        ck_eq((uint32_t)own2.unit_at(22).active_gun, 0u, "T11: active_gun 1->0 (XOR 1), 0x0043681f");
        ck_eq((uint32_t)own2.sidebar_ui_hit_code(), 0x78u,
              "T11: hit_code = row(0)+0x78, 0x00436825-0x0043682b");
    }

    // T12: ROW-LIST CLICK -- CAMERA RECENTER, the deliberately-preserved asymmetric clamp. Column
    // center uses win_w/0x40 but the column clamp bound uses win_w/0x20; row center uses
    // view_tiles_h()/2 but the row clamp bound uses the FULL view_tiles_h(). Values are chosen so a
    // clamp using the WRONG (center's) divisor produces an observably DIFFERENT result than the
    // real one. Also confirms vis_map_fill_default() fires and hit_code=1 (block 5c does not
    // return -- see the file banner for why that is an asm-only claim here). 0x00436835-0x0043691a.
    {
        tact_fixture fx;
        g_mouse_x                   = &fx.sidebar_mouse_x;
        g_mouse_y                   = &fx.sidebar_mouse_y;
        fx.sidebar_ui_hit_code      = 0;
        fx.mouse_buttons_cur        = 1;
        fx.sidebar_mouse_x          = 0x10; // outside both the defense and gun rects
        fx.sidebar_mouse_y          = 0xf5; // row=(0xf5-0xf0)/0x30=0
        fx.sidebar_slot_scroll      = 0;
        fx.sidebar_slot_unit_ids[0] = 30;
        fx.units[30].pos_col        = 100;
        fx.units[30].pos_row        = 40;
        fx.win_w                    = 640; // win_w/0x40=10, win_w/0x20=20
        fx.map_width                = 15;
        fx.view_tiles_h             = 16; // view_tiles_h/2=8, full=16
        fx.map_height               = 10;
        reset_log();

        tact_store own = fx.store();
        detail::ui_sel_panel_multi_mode_tick(fx.view(), own, mock_calls());

        // Column: center = 100-10=90 (>=0, no low clamp). High clamp: (90-20)=70 > 15 -> triggers;
        // map_cam_col = map_width(15) - win_w/0x20(20) = -5. A wrong implementation reusing
        // win_w/0x40(10) for the clamp bound would instead check (90-10)=80>15 (still triggers) and
        // produce 15-10=5 -- observably different from the correct -5.
        ck_eq((uint32_t)own.map_cam_col(), (uint32_t)-5,
              "T12: map_cam_col uses /0x20 for the clamp bound, not /0x40 -- "
              "0x00436843-0x0043689c gives -5, the wrong divisor would give 5");
        // Row: center = 40-8=32 (>=0, no low clamp). High clamp: (32-16)=16 > 10 -> triggers;
        // map_cam_row = map_height(10) - view_tiles_h(16) = -6. A wrong implementation reusing
        // view_tiles_h/2(8) for the clamp bound would instead check (32-8)=24>10 (still triggers)
        // and produce 10-8=2 -- observably different from the correct -6.
        ck_eq((uint32_t)own.map_cam_row(), (uint32_t)-6,
              "T12: map_cam_row uses the FULL view_tiles_h for the clamp bound, not /2 -- "
              "0x004368c8-0x0043691a gives -6, the wrong divisor would give 2");
        ck_eq((uint32_t)log().vis_map_fill_default_calls, 1u,
              "T12: vis_map_fill_default called once, 0x0043691f");
        ck_eq((uint32_t)own.sidebar_ui_hit_code(), 1u, "T12: hit_code latched to 1, 0x00436924");
        // Block 5c falls straight through into block 6's own gate (0x0043692e, mouse_buttons_cur==2)
        // at the instruction level, with no intervening JMP/RET -- but 5's OUTER gate required
        // mouse_buttons_cur==1 (0x00436607), the negation of block 6's, so no button value makes
        // block 6 ALSO independently observable in this same call. See the file banner.
    }

    // T13: MIDDLE-CLICK DESELECT -- clears only the ONE hovered unit's selection bit; a second
    // seeded selected unit (not under the cursor) must remain selected. 0x0043692e-0x004369b7.
    {
        tact_fixture fx;
        g_mouse_x                   = &fx.sidebar_mouse_x;
        g_mouse_y                   = &fx.sidebar_mouse_y;
        fx.sidebar_ui_hit_code      = 0;
        fx.mouse_buttons_cur        = 2; // MIDDLE
        fx.sidebar_mouse_x          = 0x10;
        fx.sidebar_mouse_y          = 0xf5; // row=(0xf5-0xf0)/0x30=0
        fx.sidebar_slot_scroll      = 0;
        fx.sidebar_slot_unit_ids[0] = 13;
        fx.units[13].status         = 1; // selected -- must be cleared
        fx.units[14].status         = 1; // selected, NOT hovered -- must survive
        reset_log();

        tact_store own = fx.store();
        detail::ui_sel_panel_multi_mode_tick(fx.view(), own, mock_calls());

        ck_eq((uint32_t)(own.unit_at(13).status & 1), 0u,
              "T13: the hovered unit's selection bit is cleared, 0x00436998-0x004369a2");
        ck_eq((uint32_t)(own.unit_at(14).status & 1), 1u,
              "T13: a different selected unit is left untouched -- only unit_idx is written, "
              "0x0043698b-0x004369a2");
        ck_eq((uint32_t)log().refresh_calls, 1u, "T13: selection_panel_refresh called once, 0x004369a8");
        ck_eq((uint32_t)own.sidebar_ui_hit_code(), 1u, "T13: hit_code latched to 1, 0x004369ad");
    }
}

} // namespace mh::tact::test
