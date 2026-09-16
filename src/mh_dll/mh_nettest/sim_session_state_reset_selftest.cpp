//
// sim_session_state_reset_selftest.cpp -- `simtest` offline oracle for
// llm_strat_session_state_reset @0x00453ee4 (sim/resid/sim_session_state_reset.h/.cpp,
// RI-SIM sim_resid batch). THIS ROW IS THE DOMAIN'S WIDEST WRITER (~35 regions, 735 bytes) --
// see the module header's own derivation for the full region list.
//
// ================================================================================================
// COVERAGE LIMIT -- READ BEFORE ADDING CASES: the FULL-RESET branch (reset_flag == 0 or == 2) is
// OFFLINE-UNTESTABLE through this entry point, and NO CASE HERE CALLS IT. Inside that branch,
// sim_session_state_reset.cpp:101 calls the intra-slice sibling directly and UNMOCKABLY:
//   new_game_init(v, own, live_new_game_init_calls());
// `live_new_game_init_calls()` (sim_new_game_init.cpp) binds straight to the LIVE
// `mh::call::game_ClearAvailableProjects` / `llm_strat_player_profile_init` / ... callables --
// each an `inline` wrapper hardcoding a literal absolute game VA and doing a raw
// `call dword ptr [...]` to it (addr/mh_calls.gen.{h,cpp}; e.g. mh_calls.gen.h:303's
// `0x0041c02eu`). There is no NET_SELFTEST build-variant stub for these thunks -- confirmed by
// grepping mh_calls.gen.{h,cpp} for any such guard: none exists. `session_state_reset`'s own
// `session_state_reset_calls c` parameter (this file's only mock seam) is NOT threaded through to
// `new_game_init` -- the call bypasses it entirely -- so nothing in an offline test process can
// intercept it. Calling `detail::session_state_reset(..., /*reset_flag=*/0)` or `(..., 2)` from
// `net_selftest.exe` therefore executes an indirect call through a foreign/unmapped absolute
// address: at best an access violation, at worst (non-ASLR image-base collision) a silent jump
// into whatever code net_selftest.exe's own image happens to have at that address. See this
// file's final report to the conductor for the fix this implies (thread new_game_init's calls
// through a second `_calls` member on session_state_reset_calls, or an equivalent seam) -- it is
// the reason roughly two-thirds of this function's documented writes (the ENTIRE full-reset-only
// tail: game_speed, cheat_penalty_score, CurrentSystem/G_PLANET_INDEX's full-reset derivation,
// BLDG_COMPLETION_ACCUM, all 32 planet slots' 5-field reset, and the 12-field per-session flag
// run) have NO case in this file and cannot get one without a conductor-side change to the module
// under test.
//
// Every region this file CAN safely reach is covered: the common prologue (unconditional for
// EVERY reset_flag, including the untestable full-reset values -- proven here via reset_flag
// values that are NOT 0 or 2) and the soft path (reset_flag == 1, and -- to prove the dispatch is
// a genuine `else`, not a `== 1` special case -- an arbitrary non-0/2 value too).
// ================================================================================================
//
// EXPECTED BEHAVIOUR, from the module header's own derivation (sim/resid/sim_session_state_reset.h)
// and the DISASSEMBLY (tmp/decomp_sim_resid/llm_strat_session_state_reset_00453ee4.asm):
//   COMMON PROLOGUE (every reset_flag): game_human_player_mask/player_control_mask -> 0
//     (0x00453eff/0x00453f06); _G_LLM_NET_BW_STAT[0..1] -> 0 (0x00453f21-0x00453f31);
//     CURRENT_GAME_TIME -> 0.0 (0x00453f33); LAST_GAME_TIME <- time_get_current_time()
//     (0x00453f47/0x00453f4c); SIM_STEP_INTERVAL -> 0.1 (0x00453f52/0x00453f5c);
//     GAME_TIME_DELTA -> 0.0 (0x00453f66/0x00453f70); TOTAL_GAME_TIME -> 0.0
//     (0x00453f7a/0x00453f84); _G_LLM_STRAT_GAME_CLOCK -> 0.0 (0x00453f8e/0x00453f98);
//     BUILD_PLACEMENT_ID -> 0 (0x00453fa2); CAM_PAN_TARGET_COL -> -1 (0x00453fac); THEN
//     cam_jump_queue_clear() (0x00453fb6) THEN invasion_alert_reset_all() (0x00453fbb), in that
//     order, both unconditional.
//   BRANCH (0x00453fc0-0x0045411f): reset_flag==0 or ==2 -> full reset (UNTESTABLE here, see
//     above); anything else -> soft path.
//   SOFT PATH (0x0045411f-0x004541b7): CurrentSystem += 1 (0x0045411f) THEN G_PLANET_INDEX
//     re-derived from the NEW CurrentSystem via system_define_index_base[CurrentSystem*35+4]
//     (0x0045412c-0x0045413c) -- the increment must land BEFORE this read; prod_reset_system()
//     (0x00454141) THEN all 8 player profiles' mother_established -> 0
//     (0x00454146-0x0045416d, i=0..7) -- prod_reset_system must run BEFORE this loop; then a dead
//     tail (0x0045416d-0x004541b7) this translation does NOT reproduce because it is provably
//     unreachable (a constant-true `0x1bc <= 0x1f4` gate at 0x0045417d) -- see the module header's
//     own derivation. The two regions that tail would have touched -- the per-player-slot table
//     at 0xe4a14d ("Unit" in the batch's own shorthand) and _G_LLM_STRAT_LOCAL_PLAYER_SLOT (read
//     only, to compute that table's index) -- must stay UNCHANGED. LOCAL_PLAYER_SLOT is asserted
//     directly below; the 0xe4a14d table has NO sim_state/fixture binding anywhere in this
//     codebase (it is not a registered region under any name), so there is nothing to assert it
//     against -- its "untouched" status is provable only by the same unreachability argument the
//     header already makes, not by a fixture read.
//
#include "sim/resid/sim_session_state_reset.h"

#include "sim_test_support.h"
#include "sim_resid_sibling_mocks.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- recorder: time_get_current_time (LAST_GAME_TIME's source, 0x00453f47) -----------------------
int    g_time_get_current_time_calls  = 0;
double g_time_get_current_time_return = 0.0;
double rec_time_get_current_time() {
    ++g_time_get_current_time_calls;
    return g_time_get_current_time_return;
}

// ---- recorders: the two unconditional void callees, with a SHARED sequence counter so a case can
// prove ORDER (cam_jump_queue_clear @0x00453fb6 must run BEFORE invasion_alert_reset_all
// @0x00453fbb), not merely that both fired. -1 = not called this case.
int  g_call_seq                       = 0;
int  g_cam_jump_queue_clear_calls     = 0;
int  g_cam_jump_queue_clear_order     = -1;
int  g_invasion_alert_reset_all_calls = 0;
int  g_invasion_alert_reset_all_order = -1;
void rec_cam_jump_queue_clear() {
    ++g_cam_jump_queue_clear_calls;
    g_cam_jump_queue_clear_order = g_call_seq++;
}
void rec_invasion_alert_reset_all() {
    ++g_invasion_alert_reset_all_calls;
    g_invasion_alert_reset_all_order = g_call_seq++;
}

// ---- recorder: prod_reset_system (soft path only, 0x00454141). Carries a MUTATION HOOK: when
// armed, the mock itself writes a player's mother_established field mid-call, so a case can prove
// the mother_established-clearing loop (0x00454146-0x0045416d) runs AFTER this callee returns --
// if the loop ran BEFORE it (or not at all), the mock's write would survive to the final check.
int        g_prod_reset_system_calls         = 0;
int        g_prod_reset_system_order         = -1;
sim_store *g_prod_reset_system_store         = nullptr;
int32_t    g_prod_reset_system_mutate_player = -1;
int32_t    g_prod_reset_system_mutate_value  = 0;
void       rec_prod_reset_system() {
    ++g_prod_reset_system_calls;
    g_prod_reset_system_order = g_call_seq++;
    if (g_prod_reset_system_store != nullptr && g_prod_reset_system_mutate_player >= 0) {
        g_prod_reset_system_store->profile_at(g_prod_reset_system_mutate_player).mother_established =
            g_prod_reset_system_mutate_value;
    }
}

const session_state_reset_calls g_calls = {
    &rec_time_get_current_time,
    &rec_cam_jump_queue_clear,
    &rec_invasion_alert_reset_all,
    &rec_prod_reset_system,
};

void reset_recorders() {
    g_time_get_current_time_calls     = 0;
    g_time_get_current_time_return    = 0.0;
    g_call_seq                        = 0;
    g_cam_jump_queue_clear_calls      = 0;
    g_cam_jump_queue_clear_order      = -1;
    g_invasion_alert_reset_all_calls  = 0;
    g_invasion_alert_reset_all_order  = -1;
    g_prod_reset_system_calls         = 0;
    g_prod_reset_system_order         = -1;
    g_prod_reset_system_store         = nullptr;
    g_prod_reset_system_mutate_player = -1;
    g_prod_reset_system_mutate_value  = 0;
}

// System[] record stride, INT units (0x8c/4 = 35), and the int-index the .asm's two reads land on
// (byte offset 0x10 -> int-index 4) -- same derivation as the module's own anonymous-namespace
// constants (sim_session_state_reset.cpp), duplicated here because those are file-local to the
// module and not exported.
constexpr int32_t SYSTEM_STRIDE_INTS = 0x8c / 4;
constexpr int32_t SYSTEM_FIELD_INDEX = 4;

} // namespace

void run_session_state_reset_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- common-prologue byte/word writes, exercised via reset_flag=1 (the SOFT path) to prove
    // these are NOT gated behind the full-reset branch: game_human_player_mask/player_control_mask
    // -> 0 (0x00453eff/0x00453f06), both NET_BW_STAT slots -> 0 (0x00453f21-0x00453f31, first AND
    // last of the 2-iteration loop), BUILD_PLACEMENT_ID -> 0 (0x00453fa2), CAM_PAN_TARGET_COL -> -1
    // (0x00453fac). All pre-seeded to distinct non-zero/non-target sentinels.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        fx.game_human_player_mask = 0x11;
        fx.player_control_mask    = 0x22;
        fx.net_bw_stat[0]         = 111;
        fx.net_bw_stat[1]         = 222;
        fx.build_placement_id     = 999;
        fx.cam_pan_target_col     = 123;

        sim_store own = fx.store();
        detail::session_state_reset(fx.view(), own, g_calls, /*reset_flag=*/1);

        ck_eq((uint32_t)own.game_human_player_mask(), 0u, "T1: game_human_player_mask -> 0, 0x00453eff");
        ck_eq((uint32_t)own.player_control_mask(), 0u, "T1: player_control_mask -> 0, 0x00453f06");
        ck_eq((uint32_t)own.net_bw_stat_at(0), 0u, "T1: net_bw_stat[0] -> 0, loop first iter, 0x00453f27");
        ck_eq((uint32_t)own.net_bw_stat_at(1), 0u, "T1: net_bw_stat[1] -> 0, loop last iter, 0x00453f27");
        ck_eq((uint32_t)own.build_placement_id(), 0u, "T1: build_placement_id -> 0, 0x00453fa2");
        ck_eq((uint32_t)own.cam_pan_target_col(), 0xffffffffu, "T1: cam_pan_target_col -> -1, 0x00453fac");
    }

    // =================================================================================================
    // T2 -- the session-clock quintet + the two unconditional void callees' ORDER. reset_flag=2 is
    // deliberately AVOIDED (it would take the untestable full-reset branch) -- reset_flag=1 used.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        fx.current_game_time           = 555.5;
        fx.last_game_time              = 111.0; // distinct from the mock's return, below
        fx.sim_step_interval           = 2.5;
        fx.game_time_delta             = 3.5;
        fx.total_game_time             = 4.5;
        fx.game_clock                  = 6.5;
        g_time_get_current_time_return = 8888.25;

        sim_store own = fx.store();
        detail::session_state_reset(fx.view(), own, g_calls, /*reset_flag=*/1);

        ck_eq_d(own.current_game_time(), 0.0, "T2: current_game_time -> 0.0, 0x00453f33");
        ck_eq((uint32_t)g_time_get_current_time_calls, 1u, "T2: time_get_current_time() called once, 0x00453f47");
        ck_eq_d(own.last_game_time(), 8888.25, "T2: last_game_time <- time_get_current_time()'s return, 0x00453f4c");
        ck_eq_d(own.sim_step_interval(), 0.1, "T2: sim_step_interval -> 0.1 (0x3fb999999999999a), 0x00453f52/f5c");
        ck_eq_d(own.game_time_delta_mut(), 0.0, "T2: game_time_delta -> 0.0, 0x00453f66/f70");
        ck_eq_d(own.total_game_time(), 0.0, "T2: total_game_time -> 0.0, 0x00453f7a/f84");
        ck_eq_d(own.game_clock_mut(), 0.0, "T2: game_clock -> 0.0, 0x00453f8e/f98");

        ck_eq((uint32_t)g_cam_jump_queue_clear_calls, 1u, "T2: cam_jump_queue_clear fires unconditionally, 0x00453fb6");
        ck_eq((uint32_t)g_invasion_alert_reset_all_calls, 1u,
              "T2: invasion_alert_reset_all fires unconditionally, 0x00453fbb");
        ck(g_cam_jump_queue_clear_order >= 0 && g_invasion_alert_reset_all_order >= 0 &&
               g_cam_jump_queue_clear_order < g_invasion_alert_reset_all_order,
           "T2: ORDER -- cam_jump_queue_clear (0x00453fb6) runs BEFORE invasion_alert_reset_all "
           "(0x00453fbb), matching the asm sequence");
    }

    // =================================================================================================
    // T3 -- soft path: CurrentSystem += 1 THEN G_PLANET_INDEX re-derived from the NEW CurrentSystem
    // (0x0045411f INC before 0x0045412c IMUL). Seed the table at the OLD (pre-increment) index with
    // a WRONG sentinel and at the NEW (post-increment) index with the RIGHT one: reading the old
    // index would return the wrong value and fail this case.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        fx.current_system                              = 7;
        const int32_t old_index                        = 7 * SYSTEM_STRIDE_INTS + SYSTEM_FIELD_INDEX; // pre-increment, WRONG if read
        const int32_t new_index                        = 8 * SYSTEM_STRIDE_INTS + SYSTEM_FIELD_INDEX; // post-increment, RIGHT
        fx.system_define_index_base[(size_t)old_index] = (int32_t)0xdeadbeef;
        fx.system_define_index_base[(size_t)new_index] = (int32_t)0x1234abcd;

        sim_store own = fx.store();
        detail::session_state_reset(fx.view(), own, g_calls, /*reset_flag=*/1);

        ck_eq((uint32_t)own.current_system_mut(), 8u, "T3: CurrentSystem 7 -> 8, 0x0045411f");
        ck_eq((uint32_t)own.planet_index_mut(), (uint32_t)0x1234abcd,
              "T3: G_PLANET_INDEX <- system_define_index_base[NEW CurrentSystem*35+4] -- reading the "
              "PRE-increment index (0xdeadbeef) would fail this, 0x0045412c-0x0045413c");
    }

    // =================================================================================================
    // T4 -- prod_reset_system() fires on the soft path (0x00454141), and ORDER: the mock mutates a
    // player's mother_established mid-call; if the clearing loop (0x00454146-0x0045416d) really runs
    // AFTER this callee, the mock's write is overwritten back to 0 by the time the function returns.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        sim_store own = fx.store();

        g_prod_reset_system_store         = &own;
        g_prod_reset_system_mutate_player = 3;
        g_prod_reset_system_mutate_value  = 77;

        detail::session_state_reset(fx.view(), own, g_calls, /*reset_flag=*/1);

        ck_eq((uint32_t)g_prod_reset_system_calls, 1u, "T4: prod_reset_system() fires once, 0x00454141");
        ck_eq((uint32_t)own.profile_at(3).mother_established, 0u,
              "T4: ORDER -- mock's mid-call write (77) is overwritten back to 0, proving the "
              "mother_established loop (0x00454146-0x0045416d) runs AFTER prod_reset_system()");
    }

    // =================================================================================================
    // T5 -- the mother_established loop's bounds: first (player 0), an interior player (4), and last
    // (player 7, MAX_PLAYERS-1) all cleared. Distinct pre-seeds so an off-by-one bound (e.g. 0..6)
    // would leave player 7's seed (44) visible.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        fx.profiles[0].mother_established = 11;
        fx.profiles[4].mother_established = 22;
        fx.profiles[7].mother_established = 44;

        sim_store own = fx.store();
        detail::session_state_reset(fx.view(), own, g_calls, /*reset_flag=*/1);

        ck_eq((uint32_t)own.profile_at(0).mother_established, 0u, "T5: profile[0].mother_established -> 0, first iter");
        ck_eq((uint32_t)own.profile_at(4).mother_established, 0u, "T5: profile[4].mother_established -> 0, interior");
        ck_eq((uint32_t)own.profile_at(7).mother_established, 0u,
              "T5: profile[7].mother_established -> 0, last iter (MAX_PLAYERS-1), 0x00454146-0x0045416d");
    }

    // =================================================================================================
    // T6 -- branch separation: every FULL-RESET-ONLY region is left COMPLETELY UNCHANGED by the soft
    // path. Seeds are all non-default so a leak from either branch's writer would be visible.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        fx.game_speed                 = 3.75;
        fx.cheat_penalty_score        = 91;
        fx.bldg_completion_accum      = 17;
        fx.planet_status[0]           = 5;
        fx.planet_status[31]          = 6;
        fx.foreign_bldg_change_flag   = 1;
        fx.debug_tap_flag             = 1;
        fx.prod_complete_throttle     = 9;
        fx.debug_resource_yield_cut   = 12.5;
        fx.ui_bldg_tab_select_blocked = 1;
        fx.bldg_completion_slot_count = 9;
        fx.lockstep_step_mult         = 9;
        fx.planet_transition_state    = 3;
        fx.floating_msg_suppress_flag = 1;
        fx.outer_planet_landed_flag   = 1;
        fx.outer_planet_land_state    = 2;
        fx.system_lost_msg_shown_flag = 1;

        sim_store own = fx.store();
        detail::session_state_reset(fx.view(), own, g_calls, /*reset_flag=*/1);

        ck_eq_d(own.game_speed(), 3.75, "T6: game_speed UNCHANGED by the soft path (full-reset-only region)");
        ck_eq((uint32_t)own.cheat_penalty_score(), 91u, "T6: cheat_penalty_score UNCHANGED (full-reset-only)");
        ck_eq((uint32_t)own.bldg_completion_accum_mut(), 17u, "T6: bldg_completion_accum UNCHANGED (full-reset-only)");
        ck_eq((uint32_t)own.planet_status_at(0), 5u, "T6: planet_status[0] UNCHANGED (full-reset-only loop)");
        ck_eq((uint32_t)own.planet_status_at(31), 6u, "T6: planet_status[31] UNCHANGED (full-reset-only loop)");
        ck_eq((uint32_t)own.foreign_bldg_change_flag_mut(), 1u,
              "T6: foreign_bldg_change_flag UNCHANGED (full-reset-only)");
        ck_eq((uint32_t)own.debug_tap_flag(), 1u, "T6: debug_tap_flag UNCHANGED (full-reset-only)");
        ck_eq((uint32_t)own.prod_complete_throttle(), 9u, "T6: prod_complete_throttle UNCHANGED (full-reset-only)");
        ck_eq_d(own.debug_resource_yield_cut(), 12.5, "T6: debug_resource_yield_cut UNCHANGED (full-reset-only)");
        ck_eq((uint32_t)own.ui_bldg_tab_select_blocked_mut(), 1u,
              "T6: ui_bldg_tab_select_blocked UNCHANGED (full-reset-only)");
        ck_eq((uint32_t)own.bldg_completion_slot_count_mut(), 9u,
              "T6: bldg_completion_slot_count UNCHANGED (full-reset-only)");
        ck_eq((uint32_t)own.lockstep_step_mult(), 9u, "T6: lockstep_step_mult UNCHANGED (full-reset-only)");
        ck_eq((uint32_t)own.planet_transition_state(), 3u, "T6: planet_transition_state UNCHANGED (full-reset-only)");
        ck_eq((uint32_t)own.floating_msg_suppress_flag(), 1u,
              "T6: floating_msg_suppress_flag UNCHANGED (full-reset-only)");
        ck_eq((uint32_t)own.outer_planet_landed_flag(), 1u, "T6: outer_planet_landed_flag UNCHANGED (full-reset-only)");
        ck_eq((uint32_t)own.outer_planet_land_state(), 2u, "T6: outer_planet_land_state UNCHANGED (full-reset-only)");
        ck_eq((uint32_t)own.system_lost_msg_shown_flag_mut(), 1u,
              "T6: system_lost_msg_shown_flag UNCHANGED (full-reset-only)");
    }

    // =================================================================================================
    // T7 -- the branch dispatch is a genuine `else`, not a `== 1` special case: an arbitrary non-0/2
    // value (42) also takes the soft path (CurrentSystem increments, prod_reset_system fires).
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        fx.current_system                              = 2;
        const int32_t new_index                        = 3 * SYSTEM_STRIDE_INTS + SYSTEM_FIELD_INDEX;
        fx.system_define_index_base[(size_t)new_index] = 555;

        sim_store own = fx.store();
        detail::session_state_reset(fx.view(), own, g_calls, /*reset_flag=*/42);

        ck_eq((uint32_t)own.current_system_mut(), 3u,
              "T7: reset_flag=42 (neither 0 nor 2) still takes the soft path, CurrentSystem increments, "
              "0x00453fc6/0x00453fca");
        ck_eq((uint32_t)g_prod_reset_system_calls, 1u, "T7: prod_reset_system() fires for reset_flag=42 too, 0x00454141");
    }

    // =================================================================================================
    // T8 -- the dead tail (0x0045416d-0x004541b7), provably unreachable per the module header's own
    // derivation (a constant-true `0x1bc <= 0x1f4` gate at 0x0045417d): LOCAL_PLAYER_SLOT (the only
    // region it would have READ) is left completely unchanged. The other region that dead code would
    // have WRITTEN -- the per-slot table at 0xe4a14d ("Unit" in the batch's own shorthand) -- has NO
    // sim_state/fixture binding anywhere in this codebase (not a registered region under any name),
    // so there is no accessor to assert it against here; its "untouched" status rests on the same
    // unreachability argument the header already makes, not on an observable check.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();
        fx.local_player_slot = 12345;

        sim_store own = fx.store();
        detail::session_state_reset(fx.view(), own, g_calls, /*reset_flag=*/1);

        ck_eq((uint32_t)fx.local_player_slot, 12345u,
              "T8: LOCAL_PLAYER_SLOT UNCHANGED -- the only reader of it (the dead tail's 100-iteration "
              "loop) is unreachable, 0x0045417d/0x00454184");
    }

    // =================================================================================================
    // T9..T13 -- THE FULL-RESET ARM (0x00453fd0-0x00454119), reachable only with reset_flag 0 or 2.
    //
    // These cases did not exist until 2026-08-31, and the reason is worth keeping: this arm calls the
    // intra-slice sibling new_game_init at 0x004540a2, and that call used to bind
    // `live_new_game_init_calls()` INSIDE the detail body -- a table of `mh::call::` naked thunks
    // against absolute game VAs that `net_selftest.exe` cannot execute. So every case here would have
    // faulted rather than failed, and the row was verified on the SOFT path alone: roughly 11 of the
    // ~35 regions it writes, with the whole full-reset arm asserted by nothing. The sibling's table is
    // now a defaulted parameter (see the header), so the arm is drivable and the sibling body runs for
    // real with its own outward calls mocked -- which is what a composite oracle wants.
    // =================================================================================================

    // T9 -- the arm's scalar head: game_speed, cheat_penalty_score, CurrentSystem and the
    // G_PLANET_INDEX re-derive, all seeded to values the arm must overwrite.
    // 0x00453fd0-0x0045400f.
    {
        fx.reset();
        reset_recorders();
        fx.game_speed            = 7.5;    // must become exactly 1.0
        fx.cheat_penalty_score   = 99;     // must become 0
        fx.current_system        = 6;      // must be PINNED to 1, not incremented
        fx.planet_index          = 0x1234; // must be re-derived from the System table
        fx.bldg_completion_accum = 55;     // must become 0
        // The System table cell the re-derive reads: system 1, field 4 of the 0x8c-byte record.
        // A distinctive value so "read the right cell" is provable and not a coincidence of zero.
        fx.system_define_index_base[1 * (0x8c / 4) + 4] = 0x2b;

        sim_store own = fx.store();
        detail::session_state_reset(fx.view(), own, g_calls, /*reset_flag=*/0, mock_ngi_calls());

        ck_eq_d(own.game_speed(), 1.0, "T9: game_speed pinned to 1.0, 0x00453fd0");
        ck_eq((uint32_t)own.cheat_penalty_score(), 0u, "T9: cheat_penalty_score zeroed, 0x00453fe4");
        ck_eq((uint32_t)own.current_system_mut(), 1u,
              "T9: CurrentSystem PINNED to 1 (not incremented as the soft path does), 0x00453ff5");
        ck_eq((uint32_t)own.planet_index_mut(), 0x2bu,
              "T9: G_PLANET_INDEX re-derived from System[CurrentSystem*0x23 + 4], 0x00453ffe-0x0045400f");
        ck_eq((uint32_t)own.bldg_completion_accum_mut(), 0u,
              "T9: BLDG_COMPLETION_ACCUM zeroed, 0x00454018");
    }

    // T10 -- the 32-slot planet sweep, all five fields. 0x0045401e-0x004540a0.
    // Every slot is seeded NON-zero first, so a loop that ran short, started late, or skipped a field
    // leaves evidence rather than matching a zeroed fixture by accident.
    {
        fx.reset();
        reset_recorders();
        for (int32_t i = 0; i < 32; ++i) {
            fx.planet_status[i]           = 3 + i;
            fx.planet_time[i]             = 100.0 + i;
            fx.planet_mother_lost_time[i] = 200.0 + i;
            fx.planet_invasion_time[i]    = 300.0 + i;
            fx.planet_int_table[i]        = 400 + i;
        }

        sim_store own = fx.store();
        detail::session_state_reset(fx.view(), own, g_calls, /*reset_flag=*/0, mock_ngi_calls());

        int bad_status = 0, bad_time = 0, bad_mother = 0, bad_invasion = 0, bad_int = 0;
        for (int32_t i = 0; i < 32; ++i) {
            if (own.planet_status_at(i) != 0) ++bad_status;
            if (own.planet_time_at(i) != 0.0) ++bad_time;
            if (own.planet_mother_lost_time_at(i) != 0.0) ++bad_mother;
            if (own.planet_invasion_time_at(i) != -1.0) ++bad_invasion;
            if (own.planet_int_table_at(i) != 0) ++bad_int;
        }
        ck_eq((uint32_t)bad_status, 0u, "T10: all 32 planet_status = UNKNOWN(0), 0x0045402a");
        ck_eq((uint32_t)bad_time, 0u, "T10: all 32 planet_time = 0.0, 0x0045403c");
        ck_eq((uint32_t)bad_mother, 0u, "T10: all 32 planet_mother_lost_time = 0.0, 0x00454052");
        ck_eq((uint32_t)bad_invasion, 0u,
              "T10: all 32 planet_invasion_time = -1.0 (0xbff00000 high dword), NOT 0 -- the one "
              "field in this sweep whose reset value is not zero, 0x00454068");
        ck_eq((uint32_t)bad_int, 0u, "T10: all 32 planet_int_table = 0, 0x0045408e");
        // The BOUND, both ends: slot 0 and slot 31 are covered by the counts above; a loop that ran
        // 0..30 or 1..31 would leave exactly one non-zero and show up there. Asserted explicitly so
        // the intent survives a later refactor of the counting.
        ck_eq((uint32_t)own.planet_status_at(0), 0u, "T10: slot 0 reached (loop starts at 0)");
        ck_eq((uint32_t)own.planet_status_at(31), 0u, "T10: slot 31 reached (loop bound is 0x20)");
    }

    // T11 -- the intra-slice sibling FIRES, and only on this arm. 0x004540a2.
    {
        fx.reset();
        reset_recorders();
        sibling_rec() = sibling_record{};

        sim_store own = fx.store();
        detail::session_state_reset(fx.view(), own, g_calls, /*reset_flag=*/0, mock_ngi_calls());
        ck_eq((uint32_t)sibling_rec().ngi_clear_available_projects, 1u,
              "T11: new_game_init reached on the full-reset arm, 0x004540a2");
        ck_eq((uint32_t)sibling_rec().ngi_fill_data, 1u,
              "T11: and its fog-of-war whole-region clear ran (a sibling that no-ops logs the same "
              "as one that works)");

        fx.reset();
        reset_recorders();
        sibling_rec()  = sibling_record{};
        sim_store own2 = fx.store();
        detail::session_state_reset(fx.view(), own2, g_calls, /*reset_flag=*/1, mock_ngi_calls());
        ck_eq((uint32_t)sibling_rec().ngi_clear_available_projects, 0u,
              "T11: new_game_init NOT reached on the soft path -- the sibling is arm-specific");
    }

    // T12 -- the 12-field per-session flag run at the arm's tail. 0x004540a7-0x00454119.
    // Every field seeded non-zero (and each to a DIFFERENT value, so a store that wrote the wrong
    // field cannot be masked by a neighbour that happened to hold the same thing).
    {
        fx.reset();
        reset_recorders();
        fx.foreign_bldg_change_flag   = 0x11;
        fx.debug_tap_flag             = 0x22;
        fx.prod_complete_throttle     = 0x33;
        fx.debug_resource_yield_cut   = 44.5;
        fx.ui_bldg_tab_select_blocked = 0x55;
        fx.bldg_completion_slot_count = 0x66; // must become 4, NOT 0 -- the only non-zero reset here
        fx.lockstep_step_mult         = 0x77; // must become 1, the other non-zero reset
        fx.planet_transition_state    = 0x88;
        fx.floating_msg_suppress_flag = 0x99;
        fx.outer_planet_landed_flag   = 0xaa;
        fx.outer_planet_land_state    = 0xbb;
        fx.system_lost_msg_shown_flag = 0xcc;

        sim_store own = fx.store();
        detail::session_state_reset(fx.view(), own, g_calls, /*reset_flag=*/2, mock_ngi_calls());

        ck_eq((uint32_t)own.foreign_bldg_change_flag_mut(), 0u, "T12: foreign_bldg_change_flag = 0, 0x004540a7");
        ck_eq((uint32_t)own.debug_tap_flag(), 0u, "T12: debug_tap_flag = 0, 0x004540b1");
        ck_eq((uint32_t)own.prod_complete_throttle(), 0u, "T12: prod_complete_throttle = 0, 0x004540bb");
        ck_eq_d(own.debug_resource_yield_cut(), 0.0, "T12: debug_resource_yield_cut = 0.0, 0x004540c2");
        ck_eq((uint32_t)own.ui_bldg_tab_select_blocked_mut(), 0u,
              "T12: ui_bldg_tab_select_blocked = 0, 0x004540d6");
        ck_eq((uint32_t)own.bldg_completion_slot_count_mut(), 4u,
              "T12: bldg_completion_slot_count = 4 -- NOT zero; a blanket-zero sweep would miss this, "
              "0x004540dd");
        ck_eq((uint32_t)own.lockstep_step_mult(), 1u,
              "T12: lockstep_step_mult = 1 -- the second non-zero reset, 0x004540e4");
        ck_eq((uint32_t)own.planet_transition_state(), 0u, "T12: planet_transition_state = 0, 0x004540eb");
        ck_eq((uint32_t)own.floating_msg_suppress_flag(), 0u,
              "T12: floating_msg_suppress_flag = 0, 0x004540f2");
        ck_eq((uint32_t)own.outer_planet_landed_flag(), 0u, "T12: outer_planet_landed_flag = 0, 0x004540fc");
        ck_eq((uint32_t)own.outer_planet_land_state(), 0u, "T12: outer_planet_land_state = 0, 0x00454106");
        ck_eq((uint32_t)own.system_lost_msg_shown_flag_mut(), 0u,
              "T12: system_lost_msg_shown_flag = 0, 0x00454110");
    }

    // T13 -- reset_flag 0 and 2 take the SAME arm. The branch is `== 0 || == 2`, so a translation
    // that wrote `< 2` would pass every case above (0 and 1 differ correctly) and only break here,
    // where 2 must take the FULL arm and not the soft one.
    {
        for (int32_t flag : {0, 2}) {
            fx.reset();
            reset_recorders();
            sibling_rec()     = sibling_record{};
            fx.current_system = 6;

            sim_store own = fx.store();
            detail::session_state_reset(fx.view(), own, g_calls, flag, mock_ngi_calls());

            ck_eq((uint32_t)own.current_system_mut(), 1u,
                  flag == 0 ? "T13: reset_flag=0 takes the FULL arm (CurrentSystem pinned to 1)"
                            : "T13: reset_flag=2 takes the FULL arm too -- `== 0 || == 2`, not `< 2`");
            ck_eq((uint32_t)sibling_rec().ngi_clear_available_projects, 1u,
                  flag == 0 ? "T13: reset_flag=0 reaches new_game_init"
                            : "T13: reset_flag=2 reaches new_game_init");
        }
    }
}

} // namespace mh::sim::test
