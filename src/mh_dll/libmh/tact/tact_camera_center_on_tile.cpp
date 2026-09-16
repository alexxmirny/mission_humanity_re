//
// tact/tact_camera_center_on_tile.cpp -- see tact_camera_center_on_tile.h. Translated from the
// DISASSEMBLY (tmp/decomp_tact/llm_tact_camera_center_on_tile_0042e889.asm), not from Ghidra's C.
//
#include "tact/tact_camera_center_on_tile.h"

#include "addr/mh_calls.gen.h" // frontier callee (Law 4): llm_tact_vis_map_fill_default
#include "state/host_api.h"
#include "state/host_events.h"

namespace mh::tact {

const camera_center_on_tile_calls &live_camera_center_on_tile_calls() {
    static const camera_center_on_tile_calls c = {
        mh::state::evt::inv_tact_vis_map,
    };
    return c;
}

namespace detail {

namespace {
// @0x0042e923-0x0042e949: the tactical camera's own clamp bounds -- no existing shared name found
// for either (grepped mh/tact for 0x6f/0x6a); local to this translation unit.
inline constexpr int32_t CAM_COL_MAX = 0x6f; // 111
inline constexpr int32_t CAM_ROW_MAX = 0x6a; // 106
} // namespace

void camera_center_on_tile(const tact_view &v, tact_store &own,
                           const camera_center_on_tile_calls &c, int32_t target_col,
                           int32_t target_row) {
    // @0x0042e8a6-0x0042e8cc: own.map_cam_col() = target_col - (G_WIN_W/32)/2, both divisions
    // truncating toward zero -- see the header banner for the shift-idiom derivation (identical to
    // tact_cam_follow_selection_tick.h's already-verified `G_WIN_W / 32`).
    own.map_cam_col() = target_col - (*v.win_w / 32) / 2;

    // @0x0042e8d2-0x0042e8f7: own.map_cam_row() = target_row - (WindowHeight/24)/2, `/24` a literal
    // IDIV, `/2` the same bias-then-shift idiom.
    own.map_cam_row() = target_row - (own.window_height() / 24) / 2;

    // @0x0042e8fd-0x0042e923: clamp both axes up to >= 0.
    if (own.map_cam_col() < 0) {
        own.map_cam_col() = 0;
    }
    if (own.map_cam_row() < 0) {
        own.map_cam_row() = 0;
    }

    // @0x0042e923-0x0042e949: clamp both axes down to their map-bound maxima.
    if (own.map_cam_col() > CAM_COL_MAX) {
        own.map_cam_col() = CAM_COL_MAX;
    }
    if (own.map_cam_row() > CAM_ROW_MAX) {
        own.map_cam_row() = CAM_ROW_MAX;
    }

    // @0x0042e949: unconditional on every path.
    c.vis_map_fill_default();
}

} // namespace detail

void camera_center_on_tile(int32_t target_col, int32_t target_row) {
    tact_state st = state();
    detail::camera_center_on_tile(st.read, st.own, live_camera_center_on_tile_calls(), target_col,
                                  target_row);
}


} // namespace mh::tact
