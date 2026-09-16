//
// tact_ui_order_buttons_minimap_tick_selftest.cpp -- offline oracle for
// llm_tact_ui_order_buttons_minimap_tick. See tact/tact_ui_order_buttons_minimap_tick.h for the
// full derivation.
//
// SCOPE, DELIBERATELY NARROW: this function has 4 button branches plus the minimap branch, all
// gated in sequence. The 4 button branches each call the SIBLING migration function
// `mh::tact::group_issue_order(...)` DIRECTLY (not through the `_calls` struct -- see the header's
// own banner, translator brief 3b same-set exception), which itself reaches
// `llm_tact_unit_enqueue_command`, an UNMAPPED real game VA inside net_selftest.exe. Since
// group_issue_order is a real function call with no mock seam here, the ONLY thing standing between
// this oracle and a crash into that unmapped VA is that no button branch's `mouse_in_rect(...) == 1`
// guard can ever be satisfied. `mock_mouse_in_rect` below unconditionally returns 0 regardless of
// its arguments, for exactly this reason -- it makes every one of the 4 `c.mouse_in_rect(...) == 1`
// checks false by construction, independent of any rect coordinate or mouse position this file
// picks. Verified against the .cpp: the minimap branch does NOT call mouse_in_rect at all, so this
// mock cannot suppress it. This oracle tests ONLY the top gate (@0x00435d02-0x00435d09) and the
// minimap branch (gate @0x00436033-0x0043608b, body @0x00436090-0x004361a6); the 4 button branches
// are a documented, intentional gap (RIG-only, unmapped real VA hazard).
//
#include "tact/tact_ui_order_buttons_minimap_tick.h"
#include "tact_test_support.h"

namespace mh::tact::test {

namespace {

struct call_log {
    int mouse_in_rect_calls           = 0;
    int order_button_calls            = 0;
    int last_button                   = -1;
    int move_path_preview_clear_calls = 0;
    int vis_map_fill_default_calls    = 0;
};

call_log &log() {
    static call_log l;
    return l;
}
void reset_log() { log() = call_log{}; }

// See file banner: ALWAYS returns 0, regardless of arguments, so none of the 4 button rects can
// ever be reported "hit" -- the load-bearing safety property of this whole oracle.
int32_t mock_mouse_in_rect(int32_t /*rect_x0*/, int32_t /*rect_y0*/, int32_t /*rect_x1*/,
                           int32_t /*rect_y1*/) {
    ++log().mouse_in_rect_calls;
    return 0;
}

// LIFT-TACT slice A: the nine-argument blit became a one-argument SCOPE. The mock got STRONGER
// rather than weaker as a result -- it could only ever count the old call (nothing asserted its
// nine arguments), and it can now record WHICH button the body decided was pressed, which is the
// only part of that draw a host could act on anyway.
void mock_order_button(int32_t button) {
    ++log().order_button_calls;
    log().last_button = button;
}

void mock_move_path_preview_clear() { ++log().move_path_preview_clear_calls; }

void mock_vis_map_fill_default() { ++log().vis_map_fill_default_calls; }

ui_order_buttons_minimap_tick_calls mock_calls() {
    return {mock_mouse_in_rect, mock_order_button, mock_move_path_preview_clear,
            mock_vis_map_fill_default};
}

} // namespace

void run_ui_order_buttons_minimap_tick_tests() {
    // The scope-level restatement of this file's load-bearing property: mock_mouse_in_rect
    // always returns 0, so NO button arm may be taken and no order-button scope may be
    // emitted. Before slice A this could only be phrased as a draw-call count.

    // T1: TOP GATE (@0x00435d02-0x00435d09) -- sidebar_ui_hit_code() > 0 makes the whole function a
    // no-op, before even the first button's mouse_in_rect check. Mouse deliberately placed INSIDE
    // the minimap box and mouse_buttons_cur==1 (i.e. the minimap branch WOULD fire if the top gate
    // did not short-circuit first) so this case actually exercises the gate, not an incidental miss.
    {
        tact_fixture fx;
        fx.sidebar_ui_hit_code      = 5; // > 0
        fx.mouse_buttons_cur        = 1;
        fx.tact_ui_minimap_origin_x = 400;
        fx.tact_ui_minimap_origin_y = 300;
        fx.sidebar_mouse_x          = 450;    // inside the box
        fx.sidebar_mouse_y          = 350;    // inside the box
        fx.map_cam_col              = -12345; // sentinel: must survive untouched
        fx.map_cam_row              = -54321;

        reset_log();
        tact_store own = fx.store();
        detail::ui_order_buttons_minimap_tick(fx.view(), own, mock_calls());

        ck_eq((uint32_t)log().vis_map_fill_default_calls, 0u,
              "T1: top gate (hit_code>0) -> vis_map_fill_default NOT called, 0x00435d02-0x00435d09");
        ck_eq((uint32_t)(int32_t)own.map_cam_col(), (uint32_t)-12345,
              "T1: top gate -> map_cam_col untouched, 0x00435d02-0x00435d09");
        ck_eq((uint32_t)(int32_t)own.map_cam_row(), (uint32_t)-54321,
              "T1: top gate -> map_cam_row untouched, 0x00435d02-0x00435d09");
    }

    // T2: mouse_buttons_cur gate on the minimap branch is a STRICT ==1, not >0 -- 0x00436033-0x00436045.
    // Two sub-cases: 0 and 2, both with the mouse inside the minimap box and hit_code==0.
    for (int32_t buttons : {0, 2}) {
        tact_fixture fx;
        fx.sidebar_ui_hit_code      = 0;
        fx.mouse_buttons_cur        = (uint8_t)buttons;
        fx.tact_ui_minimap_origin_x = 400;
        fx.tact_ui_minimap_origin_y = 300;
        fx.sidebar_mouse_x          = 450;
        fx.sidebar_mouse_y          = 350;
        fx.map_cam_col              = -111;
        fx.map_cam_row              = -222;

        reset_log();
        tact_store own = fx.store();
        detail::ui_order_buttons_minimap_tick(fx.view(), own, mock_calls());

        char msg[160];
        std::snprintf(msg, sizeof(msg),
                      "T2: mouse_buttons_cur==%d (!=1) -> minimap branch does NOT fire, "
                      "0x00436033-0x00436045",
                      (int)buttons);
        ck_eq((uint32_t)log().vis_map_fill_default_calls, 0u, msg);
        ck_eq((uint32_t)(int32_t)own.map_cam_col(), (uint32_t)-111,
              "T2: map_cam_col untouched when mouse_buttons_cur != 1");
        ck_eq((uint32_t)(int32_t)own.map_cam_row(), (uint32_t)-222,
              "T2: map_cam_row untouched when mouse_buttons_cur != 1");
    }

    // T3: mouse OUTSIDE the minimap box, all 4 boundary directions -- the gate is a strict '<' on
    // both sides, so landing EXACTLY on a boundary must NOT fire. Box is (400,300)-(528,300+0x80).
    struct edge_case {
        int32_t     mx, my;
        const char *what;
    };
    const edge_case edges[4] = {
        {400, 350, "T3: mouse_x == origin_x (400) exactly -> origin_x<mouse_x fails, 0x00436047-0x00436054"},
        {528, 350, "T3: mouse_x == origin_x+0x80 (528) exactly -> mouse_x<origin_x+0x80 fails, 0x00436065-0x00436077"},
        {450, 300, "T3: mouse_y == origin_y (300) exactly -> origin_y<mouse_y fails, 0x00436056-0x00436063"},
        {450, 428, "T3: mouse_y == origin_y+0x80 (428) exactly -> mouse_y<origin_y+0x80 fails, 0x00436079-0x0043608b"},
    };
    for (const edge_case &e : edges) {
        tact_fixture fx;
        fx.sidebar_ui_hit_code      = 0;
        fx.mouse_buttons_cur        = 1;
        fx.tact_ui_minimap_origin_x = 400;
        fx.tact_ui_minimap_origin_y = 300;
        fx.sidebar_mouse_x          = e.mx;
        fx.sidebar_mouse_y          = e.my;
        fx.map_cam_col              = -333;
        fx.map_cam_row              = -444;

        reset_log();
        tact_store own = fx.store();
        detail::ui_order_buttons_minimap_tick(fx.view(), own, mock_calls());

        ck_eq((uint32_t)log().vis_map_fill_default_calls, 0u, e.what);
        ck_eq((uint32_t)(int32_t)own.map_cam_col(), (uint32_t)-333,
              "T3: map_cam_col untouched when mouse is outside the minimap box");
        ck_eq((uint32_t)(int32_t)own.map_cam_row(), (uint32_t)-444,
              "T3: map_cam_row untouched when mouse is outside the minimap box");
    }

    // T4: a genuine hit, UNCLAMPED -- distinct non-power-of-two win_w/window_height so /64, /32,
    // /24 are all independently verifiable. win_w=500 -> win_w_div64=7, win_w_div32=15;
    // window_height=333 -> win_h_div64=5, win_h_div24=13 (all four distinct: a term-swap would be
    // observable). map_width/map_height set large (200) so neither clamp triggers.
    // mouse=(450,350), origin=(400,300):
    //   col = (450-400) - 7 = 43   (win_w_div64, @0x0043609d-0x004360b3, store @0x004360b5)
    //   row = (350-300) - 5 = 45   (win_h_div64, @0x00436129-0x0043613c, store @0x00436141)
    {
        tact_fixture fx;
        fx.sidebar_ui_hit_code      = 0;
        fx.mouse_buttons_cur        = 1;
        fx.tact_ui_minimap_origin_x = 400;
        fx.tact_ui_minimap_origin_y = 300;
        fx.sidebar_mouse_x          = 450;
        fx.sidebar_mouse_y          = 350;
        fx.win_w                    = 500;
        fx.window_height            = 333;
        fx.map_width                = 200;
        fx.map_height               = 200;

        reset_log();
        tact_store own = fx.store();
        detail::ui_order_buttons_minimap_tick(fx.view(), own, mock_calls());

        ck_eq((uint32_t)own.map_cam_col(), 43u,
              "T4: unclamped map_cam_col = (450-400) - win_w/64(500/64=7) = 43, 0x0043609d-0x004360b5");
        ck_eq((uint32_t)own.map_cam_row(), 45u,
              "T4: unclamped map_cam_row = (350-300) - win_h/64(333/64=5) = 45, 0x00436129-0x00436141");
        ck_eq((uint32_t)log().vis_map_fill_default_calls, 1u,
              "T4: firing case -> vis_map_fill_default called exactly once, 0x004361a6");
    }

    // T5: LOW clamp -- mouse just inside the box corner (401,301) drives the raw col/row negative,
    // must clamp to EXACTLY 0. win_w=500 (div64=7), window_height=333 (div64=5):
    //   col_raw = (401-400) - 7 = -6  -> clamp to 0, write @0x004360c4
    //   row_raw = (301-300) - 5 = -4  -> clamp to 0, write @0x00436150
    // map_width/map_height kept large (200) so the high clamp cannot also fire here.
    {
        tact_fixture fx;
        fx.sidebar_ui_hit_code      = 0;
        fx.mouse_buttons_cur        = 1;
        fx.tact_ui_minimap_origin_x = 400;
        fx.tact_ui_minimap_origin_y = 300;
        fx.sidebar_mouse_x          = 401;
        fx.sidebar_mouse_y          = 301;
        fx.win_w                    = 500;
        fx.window_height            = 333;
        fx.map_width                = 200;
        fx.map_height               = 200;

        reset_log();
        tact_store own = fx.store();
        detail::ui_order_buttons_minimap_tick(fx.view(), own, mock_calls());

        ck_eq((uint32_t)own.map_cam_col(), 0u,
              "T5: LOW clamp -- raw col (401-400)-7=-6 clamps to exactly 0, 0x004360bb-0x004360c4");
        ck_eq((uint32_t)own.map_cam_row(), 0u,
              "T5: LOW clamp -- raw row (301-300)-5=-4 clamps to exactly 0, 0x00436147-0x00436150");
        ck_eq((uint32_t)log().vis_map_fill_default_calls, 1u,
              "T5: firing case (clamped) -> vis_map_fill_default called exactly once, 0x004361a6");
    }

    // T6: HIGH clamp -- small map_width/map_height so the unclamped center (col=43, row=45 from the
    // same mouse/origin/win_w/window_height as T4) exceeds [0, extent-viewport). Must clamp to
    // EXACTLY (extent - div32/div24) - 1, not to extent or extent-1 (distinguishing a correct clamp
    // from an off-by-one on either side).
    //   map_width=20, win_w_div32=15  -> bound check 20-15=5 <= 43 (true) -> clamp to 5-1=4, write @0x00436117
    //   map_height=20, win_h_div24=13 -> bound check 20-13=7 <= 45 (true) -> clamp to 7-1=6, write @0x004361a1
    {
        tact_fixture fx;
        fx.sidebar_ui_hit_code      = 0;
        fx.mouse_buttons_cur        = 1;
        fx.tact_ui_minimap_origin_x = 400;
        fx.tact_ui_minimap_origin_y = 300;
        fx.sidebar_mouse_x          = 450;
        fx.sidebar_mouse_y          = 350;
        fx.win_w                    = 500;
        fx.window_height            = 333;
        fx.map_width                = 20;
        fx.map_height               = 20;

        reset_log();
        tact_store own = fx.store();
        detail::ui_order_buttons_minimap_tick(fx.view(), own, mock_calls());

        ck_eq((uint32_t)own.map_cam_col(), 4u,
              "T6: HIGH clamp -- (map_width(20)-win_w/32(15))-1 = 4, NOT 20 or 19, 0x004360ee-0x00436117");
        ck_eq((uint32_t)own.map_cam_row(), 6u,
              "T6: HIGH clamp -- (map_height(20)-win_h/24(13))-1 = 6, NOT 20 or 19, 0x0043615a-0x004361a1");
        ck_eq((uint32_t)log().vis_map_fill_default_calls, 1u,
              "T6: firing case (high-clamped) -> vis_map_fill_default called exactly once, 0x004361a6");
        ck_eq((uint32_t)log().order_button_calls, 0u,
              "T6: the minimap arm emits no ORDER_BUTTON scope -- only the four button arms do");
    }

    // The scope-level restatement, checked over every case above: mock_mouse_in_rect returns 0
    // unconditionally, so no button rect can be reported hit and no order-button scope may ever
    // have been emitted. last_button stays at its -1 sentinel.
    ck_eq((uint32_t)log().order_button_calls, 0u,
          "no ORDER_BUTTON scope is emitted while mouse_in_rect is stubbed to always-miss");
    ck_eq((uint32_t)(log().last_button + 1), 0u, "and no button id was ever recorded (-1 sentinel)");
}

} // namespace mh::tact::test
