#pragma once
//
// RES3 / D13c -- sprite placement on the torus: the biased wrap window.
//
// llm_strat_pixel_delta_wrapped (0x004944f6) turns a (camera, sprite) pair of world pixels into a screen offset
// by wrapping the difference the SHORT way round the map, so on a 64-tile map (2048 px) a sprite more than
// 1024 px right of / below the camera lands at a negative offset and vanishes (air units, the mothership body,
// shadows, selection markers). Retail never gets there (its view is at most 864 px); a render size of 1536 does.
//
// This module re-points the RENDER call sites (21 `call` instructions in the strategic draw functions) at a
// thunk that takes the offset from the window [-K, big-K) instead (gfx/wrap_window.h: K = big - viewport per axis at draw time, floored at 160 / 256).
// The game's function is not touched, so:
//   * the sim callers (llm_strat_weapon_pixel_distance_ratio, llm_strat_weapon_scatter_offset) and the cursor
//     hit-test in llm_strat_render_view keep the exact stock wrap -- determinism holds by construction, in the
//     original configuration and under promotion alike (the promoted body is the stock arithmetic);
//   * nothing contends for the function's entry, which libmh's promotion owns.
//
// [video] wrap_window=0 (dev) leaves every site as shipped (the fix-off UI row).
//
#ifdef __cplusplus
extern "C" {
#endif

// Read [video] wrap_window from mh_net.ini and splice the render sites. EN-only. Returns the number of sites
// spliced (0 when off, refused, or the image is not the expected build).
int MH_ViewWrap_Install(void);

#ifdef __cplusplus
}
#endif
