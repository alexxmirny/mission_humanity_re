//
// seams/video_mode_guards.h -- the three mode-change crash guards (P1 cursor clamp, P2 galaxy
// background index clamp, P3 header-aware filtered background restore) and their pure cores.
//
// The mechanism, the two crashes it fixes and the reasoning behind each patch are in
// video_mode_guards.cpp. The cores are exposed here so `net_selftest.exe bgscaletest` can drive them
// without a game process.
//
#pragma once
#include <stdint.h>

namespace mh::video {

// Arms P1-P3 (idempotent). `ini` is the mh_net.ini path; `[video] mode_guards=0` disarms all three (the
// rig's fix-off negative rows). Returns true when every patch armed.
bool install_mode_change_guards(const char *ini);

// ---- P1 ------------------------------------------------------------------------------------------
// Clamp each of the four cursor ints into the NEW mode ([0, W-1] x [0, H-1]). Returns how many changed.
// An in-range cursor is untouched. W or H <= 0 changes nothing.
int clamp_cursor_to_mode(int *x, int *y, int *menu_x, int *menu_y, int W, int H);

// ---- P2 ------------------------------------------------------------------------------------------
// The galaxy screen has one pre-authored background per STOCK mode: min(mode, 2).
int galaxy_bg_mode(int view_size_mode);

// ---- P6 ------------------------------------------------------------------------------------------
// The side-panel icon lists size their rows from (WindowHeight - top) / row height and write one icon-table
// entry per row; the tables hold the rows of the tallest STOCK mode (768). Beyond that they overwrite the
// group's end marker. The row count is taken from min(WindowHeight, 768).
constexpr int PANEL_ROWS_MAX_H = 768;
int           panel_rows_height(int window_height);
// Where the panel's bottom filler starts: the row count capped, the filler (llm_ui_panel_background_tile_draw)
// must still start where the uncapped list would have ended, top + ((H - top) / row_height) * row_height, or it
// paints over the panel base from the capped end down to the window bottom. Returns y_capped unchanged for H <= 768.
int panel_fill_y(int window_height, int top, int row_height, int y_capped);
// RES7 (seams/ui_panel_rows.cpp): true once all ten P6 sites are armed.
bool panel_rows_p6_armed();
// RES7: the three scrollable-list sites (body / arrow handler, list draw) cap at `h` instead of 768 -- the height
// the grown tables hold rows for -- and call `on_height(WindowHeight)` once whenever the height they read has
// changed since the last call (null: no hook). The selected-unit view's three sites keep 768.
void set_panel_list_cap(int h, void(__cdecl *on_height)(int));

// ---- P3 ------------------------------------------------------------------------------------------
enum class bg_result : int {
    passthrough,     // retail's own memcpy (the image is exactly the window's width and tall enough)
    scaled_new,      // scaled to W x H (cache rebuilt) and copied
    scaled_cached,   // scaled to W x H (cache hit) and copied
    skipped_noimage, // NULL image (a table slot past the art) or a zero window -- nothing drawn
    skipped_bad,     // zero image dimension, pitch too small, or the cache buffer could not be allocated
};

// One cached scaled image. Zero-initialise; free with bg_cache_release().
struct bg_cache {
    const void *src;
    uint32_t    hash; // content fingerprint: a freed+reallocated image of the same size at the same address
    int         iw, ih, W, H, filter, fmt;
    uint16_t   *buf; // W*H words, VirtualAlloc'd
    uint32_t    bytes;
};
void bg_cache_release(bg_cache &c);

// `src` is the pixel pointer (the image header's u16 w, u16 h sit at src[-2], src[-1]); `count_words` is
// the caller's W*H. `pitch` is the destination row stride in BYTES. `filter` 0 point, 1 linear, 2 sharp,
// 3 area; `fmt` 565 or 555. Never reads outside the image's own iw*ih pixels, never writes outside
// `dst`'s W x H (pitch-strided) rectangle.
bg_result bg_restore_core(bg_cache &cache, uint16_t *dst, const uint16_t *src, uint32_t count_words, int W, int H,
                          int pitch, int filter, int fmt);

} // namespace mh::video
