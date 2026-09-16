//
// tact_cam_follow_selection_tick_selftest.cpp -- offline oracle for llm_tact_cam_follow_selection_tick
// (TACT1E, 2026-08-27/28). See tact/tact_cam_follow_selection_tick.h for the derivation.
//
// WHY OFFLINE: 2 of its 3 own write regions (_G_LLM_MAP_CAM_COL, _G_LLM_MAP_CAM_ROW) are OWN_SHARED
// (cross-mode), and its frontier callee llm_tact_vis_map_fill_default is a class:"state" shared/
// ungated callee the closure tool cannot fully attribute through (tools/data/tact_shared_callees.json)
// -- arm_ready:false, proven here only (proof:OFFLINE(tacttest, shared:2) per tact_migration.json).
//
#include "tact/tact_cam_follow_selection_tick.h"
#include "tact_test_support.h"

namespace mh::tact::test {

namespace {

int g_fill_default_calls = 0;

void mock_vis_map_fill_default() { ++g_fill_default_calls; }

cam_follow_selection_tick_calls mock_calls() { return {mock_vis_map_fill_default}; }

} // namespace

void run_cam_follow_selection_tick_tests() {
    // T1: col nudge-left. target_col=100, cam_col=105 -> delta_col=5 (>1); cam_col(105)>1 so nudge:
    // cam_col -= 2 -> 103, fill_default() fires. Row axis kept inert (delta_row in [-1,1]) so only
    // the col branch is exercised. 0x0042e98b-0x0042e9a6.
    {
        tact_fixture fx;
        fx.map_cam_col           = 105;
        fx.cam_follow_target_col = 100;
        fx.map_cam_row           = 50;
        fx.cam_follow_target_row = 50; // delta_row = 0
        fx.cam_follow_selection  = 1;
        g_fill_default_calls     = 0;

        tact_store own = fx.store();
        detail::cam_follow_selection_tick(fx.view(), own, mock_calls());

        ck_eq((uint32_t)own.map_cam_col(), 103u, "T1: col nudge-left, cam_col -= 2, 0x0042e99a");
        ck_eq((uint32_t)g_fill_default_calls, 1u, "T1: exactly one fill_default() call, 0x0042e9a1");
        // stale delta_col stays 5 (never recomputed) -> abs(5)>=2 -> gate false -> flag untouched.
        ck_eq((uint32_t)own.cam_follow_selection(), 1u,
              "T1: follow flag untouched -- final gate reads the STALE delta_col (5), not a fresh "
              "recompute against the new cam_col, 0x0042ea5a-0x0042ea65");
    }

    // T2: col edge-left. delta_col>1 (105-100... use cam_col=3,target=0 -> delta=3>1) but
    // cam_col(1) is NOT >1 (boundary: JLE fires at cam_col<=1) -> delta_col forced 0, no fill call,
    // cam_col left unchanged. 0x0042e991-0x0042e9a8 (LAB_0042e9a8).
    {
        tact_fixture fx;
        fx.map_cam_col           = 1;
        fx.cam_follow_target_col = -2; // delta_col = 1 - (-2) = 3 > 1
        fx.map_cam_row           = 50;
        fx.cam_follow_target_row = 50;
        fx.cam_follow_selection  = 1;
        g_fill_default_calls     = 0;

        tact_store own = fx.store();
        detail::cam_follow_selection_tick(fx.view(), own, mock_calls());

        ck_eq((uint32_t)own.map_cam_col(), 1u, "T2: edge case, cam_col left unchanged, 0x0042e9a8");
        ck_eq((uint32_t)g_fill_default_calls, 0u, "T2: edge case never calls fill_default");
        // delta_col forced to 0, delta_row=0 -> both abs<2 -> gate clears the flag.
        ck_eq((uint32_t)own.cam_follow_selection(), 0u,
              "T2: edge-forced delta_col=0 combined with delta_row=0 clears the follow flag, 0x0042ea76");
    }

    // T3: row nudge-up (delta_row>1, cam_row>1), mirrors T1 for the row axis. 0x0042e9af-0x0042e9ca.
    {
        tact_fixture fx;
        fx.map_cam_row           = 40;
        fx.cam_follow_target_row = 10; // delta_row = 30 > 1
        fx.map_cam_col           = 50;
        fx.cam_follow_target_col = 50; // delta_col = 0
        fx.cam_follow_selection  = 1;
        g_fill_default_calls     = 0;

        tact_store own = fx.store();
        detail::cam_follow_selection_tick(fx.view(), own, mock_calls());

        ck_eq((uint32_t)own.map_cam_row(), 38u, "T3: row nudge, cam_row -= 2, 0x0042e9be");
        ck_eq((uint32_t)g_fill_default_calls, 1u, "T3: exactly one fill_default() call, 0x0042e9c5");
        ck_eq((uint32_t)own.cam_follow_selection(), 1u,
              "T3: stale delta_row (30) keeps the gate false, follow flag untouched");
    }

    // T4: row edge-up (delta_row>1 but cam_row<=1) -> delta_row forced 0, no call, unchanged.
    // 0x0042e9b5-0x0042e9cc.
    {
        tact_fixture fx;
        fx.map_cam_row           = 1;
        fx.cam_follow_target_row = -5; // delta_row = 6 > 1
        fx.map_cam_col           = 50;
        fx.cam_follow_target_col = 50;
        fx.cam_follow_selection  = 1;
        g_fill_default_calls     = 0;

        tact_store own = fx.store();
        detail::cam_follow_selection_tick(fx.view(), own, mock_calls());

        ck_eq((uint32_t)own.map_cam_row(), 1u, "T4: edge case, cam_row left unchanged, 0x0042e9cc");
        ck_eq((uint32_t)g_fill_default_calls, 0u, "T4: edge case never calls fill_default");
        ck_eq((uint32_t)own.cam_follow_selection(), 0u,
              "T4: edge-forced delta_row=0 combined with delta_col=0 clears the follow flag");
    }

    // T5: col nudge-right. right_edge = map_width - (win_w/32) - 1 = 128 - (640/32) - 1 = 107.
    // cam_col=50 < right_edge(107) -> nudge fires: cam_col += 2. delta_col = 50-90 = -40 < -1.
    // 0x0042e9d9-0x0042ea0e.
    {
        tact_fixture fx;
        fx.map_width             = 128;
        fx.win_w                 = 640; // visible_cols = 20, right_edge = 128-20-1 = 107
        fx.map_cam_col           = 50;
        fx.cam_follow_target_col = 90; // delta_col = 50-90 = -40 < -1
        fx.map_cam_row           = 50;
        fx.cam_follow_target_row = 50;
        fx.cam_follow_selection  = 1;
        g_fill_default_calls     = 0;

        tact_store own = fx.store();
        detail::cam_follow_selection_tick(fx.view(), own, mock_calls());

        ck_eq((uint32_t)own.map_cam_col(), 52u,
              "T5: col nudge-right, cam_col += 2 (right_edge=107 > cam_col=50), 0x0042ea02");
        ck_eq((uint32_t)g_fill_default_calls, 1u, "T5: exactly one fill_default() call, 0x0042ea09");
        ck_eq((uint32_t)own.cam_follow_selection(), 1u,
              "T5: stale delta_col (-40) keeps the gate false, follow flag untouched");
    }

    // T6: col edge-right. Same right_edge=107, but cam_col=107 -> right_edge(107) is NOT > cam_col
    // (JLE fires) -> delta_col forced 0, no call, cam_col unchanged. 0x0042e9fa-0x0042ea10.
    {
        tact_fixture fx;
        fx.map_width             = 128;
        fx.win_w                 = 640; // right_edge = 107
        fx.map_cam_col           = 107;
        fx.cam_follow_target_col = 200; // delta_col = 107-200 = -93 < -1
        fx.map_cam_row           = 50;
        fx.cam_follow_target_row = 50;
        fx.cam_follow_selection  = 1;
        g_fill_default_calls     = 0;

        tact_store own = fx.store();
        detail::cam_follow_selection_tick(fx.view(), own, mock_calls());

        ck_eq((uint32_t)own.map_cam_col(), 107u, "T6: edge case, cam_col left unchanged, 0x0042ea10");
        ck_eq((uint32_t)g_fill_default_calls, 0u, "T6: edge case never calls fill_default");
        ck_eq((uint32_t)own.cam_follow_selection(), 0u,
              "T6: edge-forced delta_col=0 combined with delta_row=0 clears the follow flag");
    }

    // T7: row nudge-down. bottom_edge = map_height - (window_height/24) - 1 = 128 - (600/24) - 1 = 102.
    // cam_row=50 < bottom_edge(102) -> nudge fires: cam_row += 2. 0x0042ea1d-0x0042ea51.
    {
        tact_fixture fx;
        fx.map_height            = 128;
        fx.window_height         = 600; // visible_rows = 25, bottom_edge = 128-25-1 = 102
        fx.map_cam_row           = 50;
        fx.cam_follow_target_row = 90; // delta_row = 50-90 = -40 < -1
        fx.map_cam_col           = 50;
        fx.cam_follow_target_col = 50;
        fx.cam_follow_selection  = 1;
        g_fill_default_calls     = 0;

        tact_store own = fx.store();
        detail::cam_follow_selection_tick(fx.view(), own, mock_calls());

        ck_eq((uint32_t)own.map_cam_row(), 52u,
              "T7: row nudge-down, cam_row += 2 (bottom_edge=102 > cam_row=50), 0x0042ea45");
        ck_eq((uint32_t)g_fill_default_calls, 1u, "T7: exactly one fill_default() call, 0x0042ea4c");
    }

    // T8: row edge-down. Same bottom_edge=102, cam_row=102 -> bottom_edge NOT > cam_row (JLE fires)
    // -> delta_row forced 0, no call, unchanged. 0x0042ea3d-0x0042ea53.
    {
        tact_fixture fx;
        fx.map_height            = 128;
        fx.window_height         = 600; // bottom_edge = 102
        fx.map_cam_row           = 102;
        fx.cam_follow_target_row = 200; // delta_row = 102-200 = -98 < -1
        fx.map_cam_col           = 50;
        fx.cam_follow_target_col = 50;
        fx.cam_follow_selection  = 1;
        g_fill_default_calls     = 0;

        tact_store own = fx.store();
        detail::cam_follow_selection_tick(fx.view(), own, mock_calls());

        ck_eq((uint32_t)own.map_cam_row(), 102u, "T8: edge case, cam_row left unchanged, 0x0042ea53");
        ck_eq((uint32_t)g_fill_default_calls, 0u, "T8: edge case never calls fill_default");
        ck_eq((uint32_t)own.cam_follow_selection(), 0u,
              "T8: edge-forced delta_row=0 combined with delta_col=0 clears the follow flag");
    }

    // T9: final-gate clear -- neither axis nudges this tick (deltas in [-1,1]), both abs<2 ->
    // flag clears, zero fill_default() calls. delta_col=1 (not >1, not <-1), delta_row=-1 (same).
    // 0x0042ea5a-0x0042ea80.
    {
        tact_fixture fx;
        fx.map_cam_col           = 11;
        fx.cam_follow_target_col = 10; // delta_col = 1
        fx.map_cam_row           = 9;
        fx.cam_follow_target_row = 10; // delta_row = -1
        fx.cam_follow_selection  = 1;
        g_fill_default_calls     = 0;

        tact_store own = fx.store();
        detail::cam_follow_selection_tick(fx.view(), own, mock_calls());

        ck_eq((uint32_t)g_fill_default_calls, 0u, "T9: neither axis nudges -> zero calls");
        ck_eq((uint32_t)own.map_cam_col(), 11u, "T9: cam_col untouched (no nudge branch taken)");
        ck_eq((uint32_t)own.map_cam_row(), 9u, "T9: cam_row untouched (no nudge branch taken)");
        ck_eq((uint32_t)own.cam_follow_selection(), 0u,
              "T9: abs(1)<2 && abs(-1)<2 -> flag clears, 0x0042ea76");
    }

    // T10: gate boundary -- abs(delta_col)==2 EXACTLY must NOT clear (JGE, not JG, at 0x0042ea65).
    // Also the distinguishing case for "reads the STALE local, never re-reads the accessor": a col
    // nudge this tick lands cam_col exactly ON the target (fresh delta would read 0, abs<2), but the
    // real code holds the pre-move delta_col=2 in the local, so the gate must still read false.
    {
        tact_fixture fx;
        fx.map_cam_col           = 102;
        fx.cam_follow_target_col = 100; // delta_col = 2 (>1, so nudge fires)
        fx.map_cam_row           = 50;
        fx.cam_follow_target_row = 50; // delta_row = 0
        fx.cam_follow_selection  = 1;
        g_fill_default_calls     = 0;

        tact_store own = fx.store();
        detail::cam_follow_selection_tick(fx.view(), own, mock_calls());

        ck_eq((uint32_t)own.map_cam_col(), 100u, "T10: nudge lands cam_col exactly on target, 0x0042e99a");
        ck_eq((uint32_t)g_fill_default_calls, 1u, "T10: nudge branch calls fill_default once");
        ck_eq((uint32_t)own.cam_follow_selection(), 1u,
              "T10: gate reads STALE delta_col=2 (>=2, JGE at 0x0042ea65) not a fresh recompute of "
              "cam_col(100)-target(100)=0 -- a re-read bug would wrongly clear this flag");
    }

    // T11: both axes nudge in the same tick -- verifies the call count accumulates correctly (2, not
    // 1 or 0) and each axis's own branch fires independently.
    {
        tact_fixture fx;
        fx.map_cam_col           = 105;
        fx.cam_follow_target_col = 100; // delta_col = 5 > 1, cam_col(105)>1 -> nudge
        fx.map_cam_row           = 40;
        fx.cam_follow_target_row = 10; // delta_row = 30 > 1, cam_row(40)>1 -> nudge
        fx.cam_follow_selection  = 1;
        g_fill_default_calls     = 0;

        tact_store own = fx.store();
        detail::cam_follow_selection_tick(fx.view(), own, mock_calls());

        ck_eq((uint32_t)own.map_cam_col(), 103u, "T11: col nudge fires alongside row nudge");
        ck_eq((uint32_t)own.map_cam_row(), 38u, "T11: row nudge fires alongside col nudge");
        ck_eq((uint32_t)g_fill_default_calls, 2u, "T11: TWO fill_default() calls, one per axis");
    }
}

} // namespace mh::tact::test
