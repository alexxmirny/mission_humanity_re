//
// gfx/wheel_list.h -- D15: the mouse wheel on the game's lists. The pure arithmetic: how a raw wheel total
// becomes whole notches (with the "drop what happened while we were away" rule), how many rows a notch moves
// in a menu list and a side-panel list, and the clamps. No Windows, no game memory; seams/ui_wheel.cpp is the
// glue and `net_selftest.exe wheellisttest` drives all of it.
//
// Direction, everywhere: wheel FORWARD (positive, away from the user) moves toward the TOP of the list and to
// the PREVIOUS page; backward moves toward the bottom / the next page.
//
#pragma once

namespace mh::gfx::wheel_list {

constexpr int      MENU_ROWS_PER_NOTCH = 3;   // menu / dialog lists show 8-16 rows
constexpr int      HUD_PX_PER_NOTCH    = 48;  // side-panel lists: about 48 px of list per notch
constexpr unsigned GAP_MS              = 750; // a longer silence than any frame: another screen sat in between

// Signed notches from a cumulative wheel total. One feed per consumer (each keeps its OWN baseline), called
// once per frame it can use the wheel. Movement that happened while the consumer was not being called
// (`now - last call > GAP_MS`) or before its first call is dropped, not applied as one step when it returns.
struct feed {
    bool     have     = false;
    int      last     = 0;
    unsigned last_ms  = 0;
    int      residual = 0; // the notch accumulator (120 per notch; a reversal drops the other direction's rest)
};

// Returns whole notches, positive = forward. `dropped` (optional) receives the delta that was thrown away.
int feed_notches(feed &f, int total, unsigned now_ms, int *dropped = nullptr);

// Rows a side-panel list moves per notch: 48 px worth, at least one row (46/52 px rows: 1; 16 px rows: 3).
int hud_rows_per_notch(int row_height);

// The list's new top row after `notches` (positive = forward = up). top is clamped into [0, max(0, count - visible)].
int scroll_top(int top, int notches, int rows_per_notch, int count, int visible);

// The info screen's new page after `notches` (forward = previous), clamped into [0, count - 1].
int page_after(int index, int notches, int count);

// Is (px, py) inside the rect [x, right) x [y, bottom)?
bool in_rect(int px, int py, int x, int y, int right, int bottom);

} // namespace mh::gfx::wheel_list
