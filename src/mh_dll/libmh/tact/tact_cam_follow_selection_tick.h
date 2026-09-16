//
// tact/tact_cam_follow_selection_tick.h -- TACT1E: the per-frame camera-follow-selection nudge.
//
//   llm_tact_cam_follow_selection_tick @0x0042e957 (0x133)
//
// Called (once per frame, per its sole caller) while `_G_LLM_TACT_CAM_FOLLOW_SELECTION` is set: it
// nudges `_G_LLM_MAP_CAM_COL`/`_G_LLM_MAP_CAM_ROW` two tiles at a time toward
// `_G_LLM_TACT_CAM_FOLLOW_TARGET_COL`/`_ROW`, and clears the follow flag once the camera has caught
// up to within one tile on both axes.
//
// SHAPE: four near-identical branch groups (col-too-far-one-way, row-too-far-one-way,
// col-too-far-the-other-way, row-too-far-the-other-way). Each group either
//   (a) nudges the camera 2 tiles toward the target and calls the frontier
//       `llm_tact_vis_map_fill_default` (the redraw-cache refill the camera move invalidates), or
//   (b) resets that axis's LOCAL residual delta to 0 (the "already at the edge, treat as caught
//       up" case).
// Crucially, group (a)'s path does NOT recompute the residual delta after moving the camera -- the
// final gate below reads whatever the four groups left in the two LOCAL deltas, which is either the
// STALE pre-move delta (nudge case) or an explicit 0 (edge case). Reproduced literally: the C++
// below never re-reads `own.map_cam_col()/row()` for the final comparison, only the `delta_col`/
// `delta_row` locals.
//
// THE TWO DIVISION IDIOMS (@0x0042e9d9-0x0042e9ec col branch, @0x0042ea1d-0x0042ea30 row branch)
// both reduce to plain truncating-toward-zero integer division and are reproduced with C++ `/`:
//   - col: `G_WIN_W / 32` -- the .asm's SAR/SHL/SBB/SAR sequence is the standard Watcom
//     shift-based signed-divide-by-power-of-2 pattern. Verified by hand for both signs (e.g.
//     G_WIN_W=-33 -> the idiom yields -1, matching trunc(-33/32)=-1): it is the compiler's own
//     encoding of `/32`, not a divergent optimization, so plain `/` is exact, not an approximation.
//   - row: `WindowHeight / 24` -- a literal `IDIV`, which C++ `/` on `int` matches directly.
//
// WindowHeight (own, `window_height()`) and G_WIN_W (view, `win_w`) are DIFFERENT globals --
// `G_WIN_W` is `WindowWidth - 0xa0`, precomputed and cached separately (see tact_state.h's own
// comment on `win_w`) -- and this function reads each from its own region, never converts one to
// the other.
//
// PROOF PATH: proof:OFFLINE(tacttest, shared:2) per tools/data/tact_migration.json -- the write
// closure reaches the frontier `llm_tact_vis_map_fill_default`, a `class:"state"` (shared, ungated)
// callee that refills the whole `_G_LLM_TILE_VIS_MAP_PTR` redraw cache (tact_shared_callees.json),
// so this site is NOT rig-armed (arm_ready:false) -- proven by net_selftest.exe tacttest only, via
// the mockable `cam_follow_selection_tick_calls` seam below (matching the `ambient_sound_calls`
// shape in tact_ambient_sound.h).
//
#pragma once
#include <cstdint>

#include "tact/tact_state.h"

namespace mh::tact {

// Mockable callee seam -- `llm_tact_vis_map_fill_default` is a shared/effectful frontier callee
// (writes the whole tile-vis-map redraw cache), so the offline oracle must mock it rather than let
// a translated body call `mh::call::` directly (translator-brief.md 3b).
struct cam_follow_selection_tick_calls {
    void (*vis_map_fill_default)();
};

namespace detail {

// llm_tact_cam_follow_selection_tick @0x0042e957.
//
//  0. @0x0042e96f-0x0042e988: delta_col = own.map_cam_col() - *v.cam_follow_target_col;
//     delta_row = own.map_cam_row() - *v.cam_follow_target_row.
//  1. @0x0042e98b-0x0042e9af: if delta_col > 1: nudge cam_col left by 2 (+ refill) if
//     cam_col > 1, else force delta_col = 0.
//  2. @0x0042e9af-0x0042e9d3: same shape for delta_row / cam_row (nudge up/whichever direction
//     decrementing the row represents).
//  3. @0x0042e9d3-0x0042ea17: if delta_col < -1: nudge cam_col right by 2 (+ refill) unless
//     already within 1 tile of the right map edge (`*v.map_width - *v.win_w/32 - 1`), else force
//     delta_col = 0.
//  4. @0x0042ea17-0x0042ea5a: same shape for delta_row / cam_row against the bottom edge
//     (`*v.map_height - own.window_height()/24 - 1`).
//  5. @0x0042ea5a-0x0042ea80: if abs(delta_col) < 2 AND abs(delta_row) < 2 (the LOCAL, possibly
//     already-zeroed values from steps 1-4, not a fresh read): own.cam_follow_selection() = 0.
void cam_follow_selection_tick(const tact_view &v, tact_store &own,
                               const cam_follow_selection_tick_calls &c);

} // namespace detail

void cam_follow_selection_tick();

// Declared here per the module convention; DEFINED in tact_cam_follow_selection_tick.cpp, CALLED
// from install_shadow() by the conductor (not this TU).
namespace detail {
}

} // namespace mh::tact
