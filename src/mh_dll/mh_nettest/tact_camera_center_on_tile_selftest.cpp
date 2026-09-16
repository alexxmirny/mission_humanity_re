//
// tact_camera_center_on_tile_selftest.cpp -- offline oracle for llm_tact_camera_center_on_tile
// (TACT1E, 2026-08-28). See tact/tact_camera_center_on_tile.h for the derivation.
//
// WHY OFFLINE: the write closure reaches the frontier llm_tact_vis_map_fill_default, a
// shared/ungated state-class callee (same posture as tact_cam_follow_selection_tick.h) -- proven
// here via the mockable camera_center_on_tile_calls seam, not rig-armed.
//
#include "tact/tact_camera_center_on_tile.h"
#include "tact_test_support.h"

namespace mh::tact::test {

namespace {

int g_fill_default_calls = 0;

void mock_vis_map_fill_default() { ++g_fill_default_calls; }

camera_center_on_tile_calls mock_calls() { return {mock_vis_map_fill_default}; }

} // namespace

void run_camera_center_on_tile_tests() {
    // T1: no clamp needed. win_w=640 -> (640/32)/2 = 20/2 = 10; window_height=600 -> (600/24)/2 =
    // 25/2 = 12 (IDIV truncates toward zero). target_col=50 -> cam_col=40; target_row=50 ->
    // cam_row=38. Both inside [0,111]x[0,106]. 0x0042e8a6-0x0042e8f7.
    {
        tact_fixture fx;
        fx.win_w             = 640;
        fx.window_height     = 600;
        g_fill_default_calls = 0;

        tact_store own = fx.store();
        detail::camera_center_on_tile(fx.view(), own, mock_calls(), 50, 50);

        ck_eq((uint32_t)own.map_cam_col(), 40u, "T1: cam_col = 50 - (640/32)/2 = 40, 0x0042e8c7");
        ck_eq((uint32_t)own.map_cam_row(), 38u, "T1: cam_row = 50 - (600/24)/2 = 38, 0x0042e8f2");
        ck_eq((uint32_t)g_fill_default_calls, 1u, "T1: fill_default() fires unconditionally, 0x0042e949");
    }

    // T2: col clamp to >= 0 -- a strongly negative target drives the raw result negative.
    // cam_col = -100 - 10 = -110 -> clamped to 0. Row kept centered (no clamp) as a control.
    {
        tact_fixture fx;
        fx.win_w             = 640;
        fx.window_height     = 600;
        g_fill_default_calls = 0;

        tact_store own = fx.store();
        detail::camera_center_on_tile(fx.view(), own, mock_calls(), -100, 50);

        ck_eq((uint32_t)own.map_cam_col(), 0u, "T2: negative col clamped to 0, 0x0042e8fd");
        ck_eq((uint32_t)own.map_cam_row(), 38u, "T2: row unaffected by the col clamp");
        ck_eq((uint32_t)g_fill_default_calls, 1u, "T2: fill_default() still fires");
    }

    // T3: row clamp to >= 0, mirrors T2 for the row axis. cam_row = -100 - 12 = -112 -> clamp 0.
    {
        tact_fixture fx;
        fx.win_w             = 640;
        fx.window_height     = 600;
        g_fill_default_calls = 0;

        tact_store own = fx.store();
        detail::camera_center_on_tile(fx.view(), own, mock_calls(), 50, -100);

        ck_eq((uint32_t)own.map_cam_col(), 40u, "T3: col unaffected by the row clamp");
        ck_eq((uint32_t)own.map_cam_row(), 0u, "T3: negative row clamped to 0, 0x0042e91b");
    }

    // T4: col clamp to <= CAM_COL_MAX (0x6f = 111). target_col=200 -> raw 190 -> clamp 111.
    {
        tact_fixture fx;
        fx.win_w             = 640;
        fx.window_height     = 600;
        g_fill_default_calls = 0;

        tact_store own = fx.store();
        detail::camera_center_on_tile(fx.view(), own, mock_calls(), 200, 50);

        ck_eq((uint32_t)own.map_cam_col(), 111u, "T4: col clamped to CAM_COL_MAX=111, 0x0042e923");
        ck_eq((uint32_t)own.map_cam_row(), 38u, "T4: row unaffected");
    }

    // T5: row clamp to <= CAM_ROW_MAX (0x6a = 106). target_row=200 -> raw 188 -> clamp 106.
    {
        tact_fixture fx;
        fx.win_w             = 640;
        fx.window_height     = 600;
        g_fill_default_calls = 0;

        tact_store own = fx.store();
        detail::camera_center_on_tile(fx.view(), own, mock_calls(), 50, 200);

        ck_eq((uint32_t)own.map_cam_col(), 40u, "T5: col unaffected");
        ck_eq((uint32_t)own.map_cam_row(), 106u, "T5: row clamped to CAM_ROW_MAX=106, 0x0042e93b");
    }

    // T6: exact boundary values -- neither clamp should perturb a result already sitting ON the
    // bound (>, not >=, per the header's own CMP/Jcc re-derivation).
    {
        tact_fixture fx;
        fx.win_w             = 32; // (32/32)/2 = 0
        fx.window_height     = 24; // (24/24)/2 = 0
        g_fill_default_calls = 0;

        tact_store own = fx.store();
        detail::camera_center_on_tile(fx.view(), own, mock_calls(), 111, 106);

        ck_eq((uint32_t)own.map_cam_col(), 111u, "T6: col lands exactly on CAM_COL_MAX, not clamped away");
        ck_eq((uint32_t)own.map_cam_row(), 106u, "T6: row lands exactly on CAM_ROW_MAX, not clamped away");
        ck_eq((uint32_t)g_fill_default_calls, 1u, "T6: still exactly one fill_default() call");
    }
}

} // namespace mh::tact::test
