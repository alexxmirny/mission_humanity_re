//
// tact_ui_sel_panel_init_selftest.cpp -- offline oracle for llm_tact_ui_sel_panel_init.
// See tact/tact_ui_sel_panel_init.h for the derivation.
//
// WHAT THIS FILE LOST AND WHY, stated rather than left to be inferred from a shrunken diff.
// LIFT-TACT slice A split this body: the icon bank's load + 565->555 conversion, the three fixed
// background panels, both font selections and both labels crossed to the host as ONE scope
// (LIBMH_EVK_INV_TACT_SEL_PANEL_INIT), so the ~40 assertions that pinned their arguments -- the
// "panelb\<name>.gfx" path composition, the resource pointers threaded into the icon registry, the
// three blits' x/y/clip pairs, the font-0-then-font-1 ordering, the two labels' positions and the
// packed colour -- no longer have anything in libmh to assert against. Those pixels are the UI
// capture's business now (docs/libmh-abi.md section 5).
//
// WHAT IS LEFT IS NOT A REMNANT. The split line is the property under test: the scope must be
// requested BEFORE the multi-mode latch resets and before the refresh/redraw/clear tail, because
// the hosted sink dispatches synchronously at emit and a reordering here silently reorders the
// original (R5). That ordering, and the latch write itself, are exactly what a host-side move
// could get wrong and what nothing else offline would catch.
//
#include "tact/tact_ui_sel_panel_init.h"
#include "tact_test_support.h"

#include <cstring>
#include <vector>

namespace mh::tact::test {

namespace {

struct call_log {
    std::vector<const char *> order;
    int                       init_draw_calls = 0, panel_refresh_calls = 0;
    int                       sel_panel_draw_calls = 0, draw_player_row_list_calls = 0;
    int32_t                   last_selected_row = -999;
};

call_log &log() {
    static call_log l;
    return l;
}
void reset_log() { log() = call_log(); }

void mock_init_draw() {
    ++log().init_draw_calls;
    log().order.push_back("sel_panel_init_draw");
}
void mock_panel_refresh() {
    ++log().panel_refresh_calls;
    log().order.push_back("selection_panel_refresh");
}
void mock_sel_panel_draw() {
    ++log().sel_panel_draw_calls;
    log().order.push_back("sel_panel_draw");
}
void mock_draw_player_row_list(int32_t selected_row) {
    ++log().draw_player_row_list_calls;
    log().last_selected_row = selected_row;
    log().order.push_back("draw_player_row_list");
}

ui_sel_panel_init_calls mock_calls() {
    return {mock_init_draw, mock_panel_refresh, mock_sel_panel_draw, mock_draw_player_row_list};
}

} // namespace

void run_ui_sel_panel_init_tests() {
    // T1: the whole body, in order. Each of the four fires exactly once, and the ORDER is the
    // assertion -- @0x00433eac (the head, now a scope), 0x00434029 (the latch), then
    // 0x00434033-0x00434047's three tail calls.
    {
        tact_fixture fx;
        fx.ui_sel_panel_multi_mode = 1; // poisoned, so a missing write is visible as 1, not as 0

        reset_log();
        tact_store own = fx.store();
        detail::ui_sel_panel_init(fx.view(), own, mock_calls());

        ck_eq((uint32_t)log().init_draw_calls, 1u,
              "T1: the head crosses once as LIBMH_EVK_INV_TACT_SEL_PANEL_INIT, 0x00433eac");
        ck_eq((uint32_t)log().panel_refresh_calls, 1u, "T1: selection_panel_refresh, 0x00434033");
        ck_eq((uint32_t)log().sel_panel_draw_calls, 1u, "T1: sel_panel_draw, 0x0043403a");
        ck_eq((uint32_t)log().draw_player_row_list_calls, 1u,
              "T1: draw_player_row_list, 0x00434042");

        static const char *const kExpected[] = {"sel_panel_init_draw", "selection_panel_refresh",
                                                "sel_panel_draw", "draw_player_row_list"};
        ck_eq((uint32_t)log().order.size(), 4u, "T1: exactly four outward calls");
        for (size_t i = 0; i < log().order.size() && i < 4; ++i) {
            ck(std::strcmp(log().order[i], kExpected[i]) == 0,
               "T1: call order matches the original's -- the head is requested BEFORE the tail, "
               "which the hosted sink's synchronous dispatch turns into the original ordering (R5)");
        }
    }

    // T2: the multi-select latch resets to 0, and it is a WRITE rather than an accident of a
    // zeroed fixture -- the fixture poisons it to 1 first. This is the one piece of state the
    // split kept in libmh, because sidebar_dispatch and both mode ticks read it every tick.
    {
        tact_fixture fx;
        fx.ui_sel_panel_multi_mode = 1;

        reset_log();
        tact_store own = fx.store();
        detail::ui_sel_panel_init(fx.view(), own, mock_calls());

        ck_eq((uint32_t)own.ui_sel_panel_multi_mode(), 0u,
              "T2: ui_sel_panel_multi_mode reset to 0 over a poisoned 1, 0x00434029");
    }

    // T3: the player-row clear passes the -1 SENTINEL, not 0 -- a zero would select row 0, which
    // is a different thing from selecting nothing and is what a sloppy relocation would produce.
    {
        tact_fixture fx;

        reset_log();
        tact_store own = fx.store();
        detail::ui_sel_panel_init(fx.view(), own, mock_calls());

        ck_eq((uint32_t)(int32_t)log().last_selected_row, (uint32_t)(int32_t)-1,
              "T3: draw_player_row_list(-1) -- the no-selection sentinel, 0x00434040");
    }
}

} // namespace mh::tact::test
