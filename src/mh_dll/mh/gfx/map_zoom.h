//
// gfx/map_zoom.h -- the strategic map zoom's pure arithmetic: crop size, zoom limits, the cursor-anchored
// crop offset with its camera absorb, the cursor translation and the wheel accumulator.
//
// WHAT THE ZOOM IS. A presentation post-process: the world is drawn at 1x
// into the viewport, then the crop (cw x ch) of it at offset (left, top) is stretched over the whole viewport
// (G_WIN_W x G_WIN_H). The stock game centres the crop; here the offset is a variable, so the world point under
// the cursor can stay under the cursor while the zoom changes.
//
// WHY A CAMERA ABSORB. The crop can only slide inside the viewport: left in [0, win - cw]. When a zoom
// step wants the crop past that (zooming OUT toward a cursor near an edge) the camera takes the difference
// instead: it moves whole 32 px tiles (the strategic
// camera has no sub-tile offset) and the crop offset gives the same 32 px per tile back, so the picture is
// continuous. Whatever the camera cannot absorb (less than a tile, or a crop with no slack) is clamped.
//
// NO TORUS RULE. A view wider than the map would draw terrain twice and objects once; that is prevented at
// the source by capping the render size to the smallest map (gfx/res_list.h), so the zoom has no map input.
//
// Pure: no Windows, no game memory. `net_selftest.exe mapzoomtest` drives all of it.
//
#pragma once
#include <stdint.h>

namespace mh::gfx::mapzoom {

constexpr int    TILE_PX         = 32;   // the strategic camera's step: one tile
constexpr double WHEEL_STEP      = 1.20; // zoom factor per wheel notch
constexpr double KEY_STEP        = 0.07; // F8 / F9: the stock additive step, kept
constexpr int    WHEEL_NOTCH     = 120;  // WHEEL_DELTA
constexpr int    ABSORB_SLACK_PX = 2;    // an overshoot this small is clamped, not paid for with a camera tile

// Largest zoom for a render of this height (WindowHeight): 2.0 * H / 768, never below the stock 2.0.
double max_zoom(int render_h);

// The crop size along one axis: trunc(win / z) & ~1, the stock rounding, at least 2.
int crop_dim(int win, double z);

// The room the crop can slide in along one axis: win - crop.
int slack(int win, int crop);

// One axis of a cursor-anchored zoom change. `left` is the crop origin, `c` the cursor in viewport pixels
// (0 .. win-1), z_old/z_new the total zoom before/after. Returns the new origin in *out_left and the number
// of camera tiles to move (positive = camera forward, i.e. the crop's world window moved right/down) in
// *cam_tiles; the origin already includes the tile give-back, and is clamped to [0, slack].
void anchor_axis(int left, int win, double z_old, double z_new, int c, int *out_left, int *cam_tiles);

// Clamp an origin into [0, slack] (the render/translate-time guard: shake, a size change, a stale state).
int clamp_origin(int left, int win, double z);

// Cursor translation along one axis: viewport pixel x of the zoomed picture -> unzoomed map pixel. The stock
// edge rule is kept: a result equal to the origin becomes 0, so the edge scroll still fires at the glass.
int translate_axis(int x, int win, int left, int crop);

// The wheel accumulator. `delta` is a raw wheel increment (120 per notch; high-resolution wheels send
// fractions). Returns whole notches, positive = away from the user = zoom in; the remainder is kept, and a
// change of direction drops the remainder of the other direction.
struct wheel_acc {
    int residual = 0;
};
int wheel_notches(wheel_acc &acc, int delta);

} // namespace mh::gfx::mapzoom
