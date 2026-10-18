//
// wheel_list_selftest.cpp -- `net_selftest.exe wheellisttest`: the pure parts of the D15 mouse-wheel list scrolling
// (gfx/wheel_list.{h,cpp}).
//
//   1. the feed: 120 per notch, fractions kept, a reversal drops the other direction's rest, the first call and a
//      gap longer than GAP_MS drop what happened while the consumer was away (and report how much);
//   2. rows per notch: three in a menu list; 48 px worth in a side-panel list (46 and 52 px rows: one, 16 px: three,
//      never below one);
//   3. scroll_top: direction (forward = toward the top), clamped at both ends, a list that fits does not move,
//      a negative or oversized start is brought into range, no overflow on a huge notch count;
//   4. page_after: forward = previous page, clamped into [0, count - 1], an empty list leaves the index alone;
//   5. in_rect: the half-open rectangle the side-panel hit test uses (the selection column x 0..0x50 against the
//      control-group column x 0x50..0xa0 never overlap).
//
#include <stdint.h>
#include <stdio.h>

#include "gfx/wheel_list.h"

namespace {

namespace wl = mh::gfx::wheel_list;

int g_checks = 0, g_fails = 0;

void check(const char *what, bool ok) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        printf("  FAIL: %s\n", what);
    }
}

void test_feed() {
    wl::feed f;
    int      dropped = -1;
    check("first call takes no step", wl::feed_notches(f, 1200, 1000, &dropped) == 0 && dropped == 0);
    check("one notch forward", wl::feed_notches(f, 1320, 1016) == 1);
    check("one notch back", wl::feed_notches(f, 1200, 1032) == -1);
    check("no movement, no step", wl::feed_notches(f, 1200, 1048) == 0);
    check("a half notch is kept", wl::feed_notches(f, 1260, 1064) == 0);
    check("...and completed by the next half", wl::feed_notches(f, 1320, 1080) == 1);
    check("three notches in one frame", wl::feed_notches(f, 1320 + 360, 1096) == 3);
    wl::feed_notches(f, 1680 + 60, 1112); // half a notch forward
    check("a reversal drops the other direction's rest", wl::feed_notches(f, 1740 - 120, 1128) == -1);
    // the gap rule: movement while the consumer was away is dropped, not applied as one step
    int d = 0;
    check("a gap drops the movement", wl::feed_notches(f, 1620 + 600, 1128 + wl::GAP_MS + 1, &d) == 0);
    check("...and reports it", d == 600);
    check("the next frame starts from the new total", wl::feed_notches(f, 1620 + 600 + 120, 1128 + wl::GAP_MS + 17) == 1);
    // exactly GAP_MS is not a gap
    wl::feed g;
    wl::feed_notches(g, 0, 5000);
    check("a silence of exactly GAP_MS still counts", wl::feed_notches(g, 120, 5000 + wl::GAP_MS) == 1);
    // the unsigned millisecond clock wrapping does not read as a gap
    wl::feed w;
    wl::feed_notches(w, 0, 0xfffffff0u);
    check("tick wrap is not a gap", wl::feed_notches(w, 120, 0x00000010u) == 1);
    // a reseeded feed (display-size round trip) has no baseline
    wl::feed r;
    wl::feed_notches(r, 5000, 100);
    r = wl::feed();
    check("a reseeded feed takes the first total as its baseline", wl::feed_notches(r, 0, 116) == 0 && wl::feed_notches(r, 120, 132) == 1);
}

void test_rows_per_notch() {
    check("menu: 3 rows", wl::MENU_ROWS_PER_NOTCH == 3);
    check("46 px rows: 1", wl::hud_rows_per_notch(46) == 1);
    check("52 px rows: 1", wl::hud_rows_per_notch(52) == 1);
    check("16 px rows: 3", wl::hud_rows_per_notch(16) == 3);
    check("48 px rows: 1", wl::hud_rows_per_notch(48) == 1);
    check("100 px rows: never 0", wl::hud_rows_per_notch(100) == 1);
    check("row height 0 (a torn read): 1", wl::hud_rows_per_notch(0) == 1 && wl::hud_rows_per_notch(-5) == 1);
}

void test_scroll_top() {
    // 20 entries, 8 visible: max top 12
    check("backward moves down 3", wl::scroll_top(0, -1, 3, 20, 8) == 3);
    check("forward moves up 3", wl::scroll_top(6, 1, 3, 20, 8) == 3);
    check("forward clamps at 0", wl::scroll_top(2, 1, 3, 20, 8) == 0);
    check("forward at 0 stays", wl::scroll_top(0, 5, 3, 20, 8) == 0);
    check("backward clamps at count - visible", wl::scroll_top(10, -1, 3, 20, 8) == 12);
    check("backward at the end stays", wl::scroll_top(12, -4, 3, 20, 8) == 12);
    check("two notches", wl::scroll_top(0, -2, 3, 20, 8) == 6);
    check("a list that fits does not move", wl::scroll_top(0, -3, 3, 8, 8) == 0 && wl::scroll_top(0, -3, 3, 5, 8) == 0);
    check("a stale oversized top comes back into range", wl::scroll_top(40, -1, 3, 20, 8) == 12);
    check("a negative top comes back into range", wl::scroll_top(-4, -1, 3, 20, 8) == 0);
    check("a huge notch count does not overflow", wl::scroll_top(0, 2000000000, 3, 20, 8) == 0 && wl::scroll_top(0, -2000000000, 3, 20, 8) == 12);
    // the HUD form: visible = 1 (the draw clamps the upper bound itself), 16 px rows, 3 rows per notch
    check("hud: count 10, backward 3 rows", wl::scroll_top(0, -1, 3, 10, 1) == 3);
    check("hud: never past count - 1", wl::scroll_top(8, -1, 3, 10, 1) == 9);
    check("hud: empty list stays at 0", wl::scroll_top(0, -1, 3, 0, 1) == 0);
    check("hud: 1 row per notch", wl::scroll_top(4, 1, 1, 10, 1) == 3);
}

void test_page() {
    check("backward = next page", wl::page_after(0, -1, 4) == 1);
    check("forward = previous page", wl::page_after(2, 1, 4) == 1);
    check("forward clamps at 0", wl::page_after(0, 1, 4) == 0);
    check("backward clamps at count - 1", wl::page_after(3, -1, 4) == 3);
    check("several notches clamp", wl::page_after(1, -9, 4) == 3 && wl::page_after(2, 9, 4) == 0);
    check("one page: nothing moves", wl::page_after(0, -1, 1) == 0 && wl::page_after(0, 1, 1) == 0);
    check("empty: the index is left alone", wl::page_after(0, -1, 0) == 0);
    check("huge count of notches", wl::page_after(0, -2000000000, 4) == 3);
}

void test_rect() {
    // the selection column (x 0..0x50) and the control-group column (x 0x50..0xa0) of the panel, y 100..768
    check("inside the left column", wl::in_rect(10, 200, 0, 100, 0x50, 768));
    check("0x4f is left", wl::in_rect(0x4f, 200, 0, 100, 0x50, 768) && !wl::in_rect(0x4f, 200, 0x50, 100, 0xa0, 768));
    check("0x50 is right", !wl::in_rect(0x50, 200, 0, 100, 0x50, 768) && wl::in_rect(0x50, 200, 0x50, 100, 0xa0, 768));
    check("0xa0 is outside", !wl::in_rect(0xa0, 200, 0x50, 100, 0xa0, 768));
    check("above the list", !wl::in_rect(10, 99, 0, 100, 0x50, 768) && wl::in_rect(10, 100, 0, 100, 0x50, 768));
    check("the bottom edge is exclusive", wl::in_rect(10, 767, 0, 100, 0x50, 768) && !wl::in_rect(10, 768, 0, 100, 0x50, 768));
    check("the map (negative panel x) is outside", !wl::in_rect(-5, 200, 0, 100, 0x50, 768));
}

} // namespace

int run_wheellisttest() {
    printf("=== wheellisttest (D15: wheel notches + gap rule, rows per notch, list / page clamps, panel rect) ===\n");
    test_feed();
    test_rows_per_notch();
    test_scroll_top();
    test_page();
    test_rect();
    printf("wheellisttest: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
