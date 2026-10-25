//
// gfx/res_list.h -- the generated resolution list ([video] resolution=auto|WxH).
//
// Pure: no Windows, no game memory. Under the owned backends the render size is a UI-scale choice rather than
// a display mode, so the list is GENERATED from the desktop size instead of enumerated:
//
//   * the three stock sizes 640x480 / 800x576 / 1024x768 always lead (indices 0/1/2 keep the engine's meaning:
//     the F5-F7 keys, the options widget and setup.dat all index them);
//   * "native" family: desktop / k for k in {1, 1.25, 1.5, 2, 2.5, 3, 4}, EXACT (RES18: the strategic view draws
//     whole 32 px tiles, so the game renders ceil32(W) x ceil32(H) and the presenter crops to W x H; a size is no
//     longer floored to the grid, which used to leave bars -- 1920x1056 on a 1080p desktop);
//   * "4:3" family: the same heights with W = round(H * 4 / 3);
//   * no generated size (and not Auto) is wider than 16:9: a wider desktop gets 16:9 sizes of its own height
//     (presented with side bars). The 4:3 family stays;
//   * every entry is at least 640x480, never larger than the desktop, at most 2048x1600 (the view must not
//     outgrow the sprite window on the smallest map, see CAP_W) and within the per-tile array ceiling
//     (MAX_TILES) -- the caps and the ceiling are tested on the RENDERED (ceil32) size;
//   * "common" family (RES24): 800x600 1024x768 1152x864 1280x960 1400x1050 1600x1200 1920x1440 2048x1536 (4:3) and
//     1280x720 1366x768 1600x900 1920x1080 2560x1440 3200x1800 3840x2160 (16:9), each when it fits the desktop and
//     passes valid_size; a size already generated above keeps its earlier kind and label;
//   * after the stock three the list is grouped by aspect: every 4:3 size, then every 16:9 size, then all others
//     (aspect_group), each group ascending by area.
//
// The launcher carries a Rust copy of this algorithm with the same test
// vectors; change both together.
//
#pragma once

#include "gfx/wrap_window.h"

namespace mh::gfx::reslist {

constexpr int MIN_W = 640;
constexpr int MIN_H = 480;
// THE VIEW MUST FIT THE SMALLEST MAP. Two limits, the tighter one wins. The smallest map the game loads is 64x64
// tiles (all 31 shipped planet maps are 64, 128 or 256 square), so big = 2048 px there.
//   1. TILE WINDOW. llm_strat_tile_in_view_and_visible tests [cam-1, cam+TILES+1] mod map; once the viewport
//      (W - 160 wide, H - 32 high) plus those two tiles covers the map, the terrain repeats and objects are drawn
//      once: (W - 160) / 32 <= 64 - 3 tiles, i.e. W <= 2112 and H <= 1984.
//   2. SPRITE WINDOW. Sprite placement (air units, shadows, selection markers) uses the biased window
//      [-K, big-K) (gfx/wrap_window.h), correct while viewport + K <= big: with K = 160 / 256 that is
//      W - 160 <= 2048 - 160, i.e. W <= 2048, and H - 32 <= 2048 - 256, i.e. H <= 1824. A sprite reaching more than K
//      px past its offset is misfiled at the far edge when it is that far off the left (wrap_window.h): visible only
//      while W > 2208 - reach, i.e. for the widest sizes.
// The 1600 height cap is the older CPU-cost bound and is the tighter of the heights.
constexpr int MIN_MAP_TILES = 64;
constexpr int PANEL_W       = 160; // the fixed side panel
constexpr int STRIP_H       = 32;  // the fixed bottom strip
constexpr int TILE_WIN_W    = PANEL_W + (MIN_MAP_TILES - 3) * 32;
constexpr int TILE_WIN_H    = STRIP_H + (MIN_MAP_TILES - 3) * 32;
constexpr int SPRITE_WIN_W  = PANEL_W + MIN_MAP_TILES * 32 - wrapwin::K_X;
constexpr int SPRITE_WIN_H  = STRIP_H + MIN_MAP_TILES * 32 - wrapwin::K_Y;
constexpr int NOWRAP_W      = TILE_WIN_W < SPRITE_WIN_W ? TILE_WIN_W : SPRITE_WIN_W;
constexpr int NOWRAP_H      = TILE_WIN_H < SPRITE_WIN_H ? TILE_WIN_H : SPRITE_WIN_H;
constexpr int CAP_W         = NOWRAP_W; // never offer a render size above this
constexpr int CAP_H         = 1600 < NOWRAP_H ? 1600 : NOWRAP_H;
constexpr int AUTO_MIN_W    = 1024; // Auto never goes below the largest stock UI
constexpr int AUTO_MIN_H    = 768;
constexpr int MAX_TILES     = 4096; // the relocated per-tile arrays (seams/video.cpp)
constexpr int MAX_ENTRIES   = 48;

enum kind : int { k_stock     = 0,
                  k_native    = 1,
                  k_4x3       = 2,
                  k_requested = 3,
                  k_common    = 4 }; // RES24: the fixed list of common 4:3 / 16:9 sizes

struct entry {
    int w;    // render width
    int h;    // render height (a multiple of 32, except the engine's 800x600 which renders 576)
    int kind; // enum kind
};

int floor32(int v);
int round32(int v);
int ceil32(int v); // the frame the game renders for a w x h session size (RES18)

// A size the strategic view can show: at least MIN, and its ceil32 render within CAP and MAX_TILES. Any w x h (RES18).
bool valid_size(int w, int h);

// Would the ceil32 render of a w x h size outgrow the smallest map (the tile window or the sprite window above)?
bool view_wraps_smallest_map(int w, int h);

// The tail's grouping (after the stock three): 4:3 first, then 16:9, then everything else. Sizes are 32-snapped, so
// the tests allow for the snap: 4:3 when |3w - 4h| <= 96 (the 4:3 family rounds W to 32, a native size floors
// both sides); 16:9 when -288 <= 9w - 16h <= 496 (the height floored by up to 31 rows, the width by up to 31 px).
// 1920x1056 and 960x512 are 16:9; 1408x1056 and 672x512 are 4:3; 1280x800 (16:10) and 1280x1024 (5:4) are others.
enum group : int { grp_4x3   = 0,
                   grp_16x9  = 1,
                   grp_other = 2 };
int aspect_group(int w, int h);

// Is w x h no wider than 16:9, with the width allowed to round UP (1366x768 qualifies: 768 * 16 / 9 = 1365.3)? Exact since RES18.
bool within_16x9(int w, int h);

// The list for a desktop of dw x dh. Returns the entry count (<= cap). The first three are the stock sizes, then the tail order above.
int generate(int dw, int dh, entry *out, int cap);

// Auto: desktop / (display scale percent), exact (RES18), then clamped: never below 1024x768, never above
// the desktop or the CAP_W x CAP_H cap, and never wider than 16:9. scale_pct <= 0 reads as 100.
entry auto_size(int dw, int dh, int scale_pct);

// "1920x1056" -> true. Accepts x or X; nothing else.
bool parse_size(const char *s, int *w, int *h);

// Index of a w x h entry, or -1.
int index_of(const entry *list, int n, int w, int h);

// Insert e after the stock three, in the tail order (4:3, 16:9, others; ascending by area within a group). Returns its index, or -1 when the list
// is full. An entry already present is not duplicated (its index is returned).
int insert_sorted(entry *list, int n, int cap, entry e, int *new_count);

// Should the presenter use integer scaling for a w x h picture on a dw x dh desktop? True when the fit scale
// min(dw/w, dh/h) is a whole number (or just over one: a 32-snapped height leaves up to 3% slack), so the
// picture is pixel-exact; false for fractional ratios, where fit + the area filter is the right answer.
bool integer_scale_ok(int dw, int dh, int w, int h);

// The launcher/diagnostic label: "1920x1056 (1:1, sharpest)", "1280x704 (1.5x)", "1408x1056 (4:3, 1:1)",
// "640x480 (classic)". Returns the length written (always NUL-terminated within cap).
int describe(const entry &e, int dw, int dh, char *out, int cap);

} // namespace mh::gfx::reslist
