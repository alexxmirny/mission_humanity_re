#pragma once
//
// RES2 -- the strategic map zoom: mouse wheel, cursor anchor, camera absorb, our own resampler.
//
// Mechanism and the addresses are below. In short, the stock zoom is a presentation
// post-process of the viewport (llm_map_zoom_apply stretches the CENTRED crop over the viewport;
// llm_map_zoom_translate_cursor maps the cursor back) driven by two doubles that only F8/F9 and the debris
// screen shake write. This module replaces the three functions that decide the picture and keeps the
// player's zoom in its OWN state, never in the game's globals:
//
//   * llm_map_view_zoom_step (F8 / F9)        -> adjusts the player's zoom, same limits as the wheel
//   * llm_map_zoom_apply                      -> stretches an OFFSET crop with the [video] filter
//   * llm_map_zoom_translate_cursor           -> maps the cursor through the same crop
//   * a run-before hook on llm_strat_input_update (shared with the pause hotkey, seams/strat_pre_input.cpp)
//                                             -> wheel -> zoom step anchored at the cursor, the
//                                                camera absorb, the resets
//
// WHY THE GAME'S GLOBALS STAY UNTOUCHED. The screen shake (llm_cam_jump_queue_pop) writes a per-frame scale
// into the same doubles and finishes by writing 1.0, which would flatten a player's zoom after every debris
// burst. So those doubles are the SHAKE factor here and the picture is drawn at (player zoom x shake). They
// stay 1.0 whenever nothing shakes, which also keeps llm_map_zoom_scale_x_get (a sim-side read) exactly as
// the stock game leaves it.
//
// View state only: the zoom, the crop origin and the camera tiles the absorb moves are MF_VIEW / not hashed;
// the determinism gate with the wheel on one peer is the proof.
//
#ifdef __cplusplus
extern "C" {
#endif

// Read [input] wheel_zoom and [video] zoom_module from mh_net.ini and arm. EN-only. Returns 1 if armed.
int MH_MapZoom_Install(void);

// The harness's view of the state (ui_drive `zoom` predicate). z100 = the player's zoom x 100 (100 = 1x),
// *left / *top = the crop origin, *cw / *ch = the crop. All zero/100 when not armed.
void MH_MapZoom_Get(int *z100, int *left, int *top, int *cw, int *ch);

// A display-size round trip (the galaxy screen's switch to 1024x768 and back) re-initialises the wheel counter the
// zoom reads its movement from; the next strategic frame must take it as the new baseline instead of a wheel
// delta. The zoom itself is kept: the viewport is the same size again, so the crop is still valid.
void MH_MapZoom_ReseedWheel(void);

#ifdef __cplusplus
}
#endif
