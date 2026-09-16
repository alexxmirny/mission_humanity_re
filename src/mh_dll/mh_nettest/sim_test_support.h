//
// sim_test_support.h -- the `simtest` fixture and check helpers, shared by every sim test TU.
//
// Extracted from sim_selftest.cpp when llm_strat_order_queue_dispatch arrived: 49 switch arms is
// more test code than one file wants, and splitting it across files is also what lets several
// authors work on it at once. Nothing here is new -- the fixture and the two check helpers are the
// ones sim_selftest.cpp has had since SIM0, moved verbatim.
//
// THE COUNTERS ARE INLINE VARIABLES, i.e. ONE PER PROGRAM, not one per TU. That is the point: every
// TU's cases add into the same totals, and `run_simtest` prints one number at the end. A `static`
// here would silently give each TU its own counter and the summary would report only the last one.
//
// THE FIXTURE IS THE ONE PLACE THIS CAN LIE, so two rules it follows:
//
//   SEED WITH DISTINCT, NON-DEFAULT, NON-SYMMETRIC VALUES. If two fields that a translation could
//   have swapped hold the same number, the swap passes. aitest learned this the expensive way (its
//   promo_add/promo_sub pair is seeded 3 and 7 even though the shipped AI.SCR ships both as 1).
//
//   SIZE BUFFERS WITH PARENTHESES, NOT BRACES. `std::vector<uint32_t> v{128}` is a ONE-element
//   vector holding 128, not 128 elements -- legal overload resolution, invisible to clang-tidy and
//   to /analyze, and it cost aitest a 0xC0000374 on ~60% of runs before anyone found it.
//
#pragma once
#include <cmath> // sim_fixture::reset() rebuilds the facing trig table with cos/sin
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator> // std::begin/std::end over the " wynalazek " literal
#include <vector>

#include "sim/sim_state.h"

namespace mh::sim::test {

inline int g_checks = 0;
inline int g_fails  = 0;

inline int trace_on() {
    static const int on = [] {
        const char *e = getenv("SIMTEST_TRACE");
        return (e != nullptr && *e != '\0' && *e != '0') ? 1 : 0;
    }();
    return on;
}

inline void ck(bool ok, const char *what) {
    ++g_checks;
    if (trace_on()) {
        printf("  [%3d] %s: %s\n", g_checks, ok ? "ok  " : "FAIL", what);
        fflush(stdout);
    }
    if (!ok) {
        ++g_fails;
        if (!trace_on()) printf("  FAIL: %s\n", what);
        fflush(stdout);
    }
}

// The same check for an integral comparison, printing WHAT WAS ACTUALLY THERE. `ck(x == k, "...")`
// reduces the observation to one bit before anyone sees it, which turns "why did this fail" into a
// rebuild with a printf in it -- and the answer is usually visible in the number (a value one column
// high, a stale value, a byte-swapped one).
inline void ck_eq(uint32_t got, uint32_t want, const char *what) {
    ++g_checks;
    const bool ok = (got == want);
    if (trace_on()) {
        printf("  [%3d] %s: %s\n", g_checks, ok ? "ok  " : "FAIL", what);
        fflush(stdout);
    }
    if (!ok) {
        ++g_fails;
        printf("  FAIL: %s  [got 0x%08x, want 0x%08x]\n", what, got, want);
        fflush(stdout);
    }
}

// Doubles compare EXACTLY here, deliberately. Every FP value the sim computes is either copied
// verbatim or produced by one arithmetic step from fixture inputs chosen to be exactly
// representable, so a tolerance would only hide a wrong operand order.
inline void ck_eq_d(double got, double want, const char *what) {
    ++g_checks;
    const bool ok = (got == want);
    if (trace_on()) {
        printf("  [%3d] %s: %s\n", g_checks, ok ? "ok  " : "FAIL", what);
        fflush(stdout);
    }
    if (!ok) {
        ++g_fails;
        printf("  FAIL: %s  [got %.17g, want %.17g]\n", what, got, want);
        fflush(stdout);
    }
}

} // namespace mh::sim::test

// ---- the fixture --------------------------------------------------------------------------------
//
// `sim_fixture` is declared a friend of `sim_store` in sim/sim_state.h and is one of the TWO
// sanctioned ways to obtain a mutable handle (the other is `state()`). That is deliberate and it is
// the whole of W3: the exception list is short enough to read, and adding to it is a diff.
//
// It must be `mh::sim::sim_fixture` -- the same name in the same namespace the header befriends --
// so it cannot live in an anonymous namespace.
namespace mh::sim {

struct sim_fixture {
    // Real extents, so an index the original computes out of range lands in the fixture's own
    // memory and can be OBSERVED rather than corrupting the heap.
    std::vector<unit>          units{(size_t)(MAX_PLAYERS * UNITS_PER_PLAYER)};
    std::vector<building>      buildings{(size_t)(MAX_PLAYERS * BUILDINGS_PER_PLAYER)};
    std::vector<player_data>   players{(size_t)MAX_PLAYERS};
    std::vector<cfg_unit>      cfg_units{100};
    std::vector<cfg_building>  cfg_buildings{100};
    std::vector<cfg_weapon>    cfg_weapons{32};
    std::vector<cfg_project>   cfg_projects{100};   // SIM1F second slice
    std::vector<cfg_invention> cfg_inventions{300}; // SIM1F second slice
    // Owned by SIM1F; RESIZED 99 -> 100 by SIM-RESID-C.
    //
    // 100 IS THE REACH, NOT THE CANONICAL SIZE, and the difference is a real bug in the original
    // that Law 2 says to reproduce. llm_strat_tech_tables_reset's third loop is bounded
    // `CMP dword ptr [EBP+-0x18],0x64` / JL @0x00455d21, so it writes Upgrades[0..99] INCLUSIVE --
    // one 0x68-byte entry past the 99 that mh_addrs.gen.h documents. mh_regions.gen.h's own
    // RID_UPGRADES entry already records exactly this: size=10296 (99*0x68) vs reach=10400
    // (100*0x68).
    //
    // At 99 the fixture was a guaranteed heap-buffer-overflow on EVERY call to
    // detail::tech_tables_reset, not only in the case that inspects entry 99 -- an ASan abort, or in
    // a plain build the delayed, non-local corruption ASan exists to catch. A fixture must
    // model the region's REACH so a translation that faithfully reproduces an overrun can run at
    // all; clamping the loop instead would have been "fixing" the bug.
    std::vector<cfg_upgrade> cfg_upgrades{100};
    // SIM1F. Boot constants + message-separator literals game_HandleUpgrade needs.
    double  upgrade_unit_speed_pct_divisor  = 100.0;
    double  upgrade_weapon_pct_divisor      = 100.0;
    wchar_t upgrade_msg_sep_before_category = L' ';
    wchar_t upgrade_msg_sep_before_name     = L':';
    wchar_t upgrade_msg_clause_sep_first    = L' ';
    wchar_t upgrade_msg_clause_sep_next     = L',';
    wchar_t upgrade_msg_trailer             = L')';
    // SIM1F. llm_strat_invasion_chance_roll / game_HandleInvasion's own boot constants.
    double   invasion_roll_base_time = 300.0;
    uint16_t local_player_slot       = 0;
    // SIM1F. llm_strat_dir_from_to / llm_strat_dir_sector_to's boot constants -- real
    // read-memory-confirmed values so an offline case exercises the actual trig-to-sector math.
    double dir24_rad2deg_num = 180.0, dir24_rad2deg_den = 3.1415926536, dir24_half_turn_deg = 180.0,
           dir24_bias_deg = 7.0, dir24_wrap_add_deg = 360.0, dir24_wrap_limit_deg = 360.0,
           dir24_wrap_sub_deg = -360.0, dir24_sector_deg = 15.0;
    double dir128_rad2deg_num = 180.0, dir128_rad2deg_den = 3.1415926536, dir128_half_turn_deg = 180.0,
           dir128_bias_deg = 1.4062, dir128_wrap_add_deg = 360.0, dir128_wrap_limit_deg = 360.0,
           dir128_wrap_sub_deg = -360.0, dir128_sector_deg = 2.8125;
    // sim_resid batch E (2026-08-31). _G_LLM_STRAT_BLDG_COUNT_OFFLINE_DECREMENT @0x0050160e, the
    // REAL shipped value read straight off the image (ReVa get-data, hexBytes 000000000000f0bf).
    double bldg_count_offline_decrement = -1.0;
    // sim_resid batch E (2026-08-31). Three more read-only boot constants + the AI mine-worth
    // threshold. DISTINCT, non-symmetric seeds so a swap between the two energy scales cannot pass.
    double                            unit_energy_status_percent_scale  = 100.0;
    double                            bldg_energy_status_percent_scale  = 200.0;
    double                            ai_base_spawn_timer_stagger_scale = 0.25;
    int32_t                           mine_worth                        = 10;
    std::vector<group_scratch_member> group_move_scratch{100};
    // SIM1-G1 (2026-08-19). _G_LLM_STRAT_PATH_BUFFERS[240000] = 8 players * 100 path slots * 300
    // waypoints, MUTABLE (move_walker decrements .run_length). PARENS, not braces: a braced
    // std::vector<path_waypoint>{(size_t)240000} would pick the ONE-element initializer_list ctor.
    std::vector<path_waypoint> path_buffers =
        std::vector<path_waypoint>((size_t)(MAX_PLAYERS * PATH_WAYPOINTS_PER_PLAYER));
    // SIM1-G1 (2026-08-20). _G_LLM_STRAT_HEADING_CANDIDATE_TABLE[72] (24 headings * 3),
    // read-only in this closure; _G_LLM_STRAT_GROUP_STEP_HEADING_REMAP[24], read-only.
    std::vector<heading_slot> heading_candidates{72};
    std::vector<int32_t>      group_step_heading_remap = std::vector<int32_t>(24);
    // _G_LLM_STRAT_PATHFINDER_AIR_MODE_FLAG, MUTABLE (unit_group_step_ground/_plane write it).
    int32_t pathfinder_air_mode_flag = 0;
    // llm_strat_squad_pick_free_formation_anchor's two read-only inputs -- see sim_state.h's alias
    // comments. REBASED 2026-09-02 (LT0): the full squad-placement offset table (byte[288] =
    // llm_squad_placement_offset[6][6]); the col-5 column the picker reads sits at 0x28+row*0x30.
    std::vector<uint8_t>                        squad_placement_offset_table = std::vector<uint8_t>(288);
    std::vector<squad_formation_anchor_scratch> squad_anchor_scratch{5};
    // SIM1-G1 (2026-08-20). llm_strat_group_move_order_commit's globals.
    std::vector<route_step>   group_route_steps{256};
    std::vector<group_member> group_members{256};
    std::vector<uint8_t>      group_member_tile  = std::vector<uint8_t>(512); // [256][2] flat
    uint32_t                  path_wrap_mask     = 0;
    int32_t                   group_order_goal_x = 0;
    int32_t                   group_order_goal_y = 0;
    int32_t                   group_anchor_x     = 0;
    int32_t                   group_anchor_y     = 0;
    int32_t                   group_order_owner  = 0;
    int32_t                   group_member_count = 0;
    uint32_t                  pathtrace_len      = 0;
    // SIM1-G-PREP (2026-08-20). The batch-G write set, so an offline oracle for any of those
    // functions has backing storage the day its batch opens. PARENS, not braces, on every sized
    // vector of a SCALAR element: `std::vector<uint32_t>{4096}` is a ONE-element vector holding
    // 4096, not 4096 zeroes (the same trap path_buffers documents above).
    int32_t group_centroid_x = 0, group_centroid_y = 0, group_path_build_idx = 0;
    // route_cand_scratch is byte-addressed with a 4-byte slot stride -- see sim_state.h. WIDENED to 36
    // (SIM1-G2, 2026-08-21): llm_map_region_route_search's shipped direction-7 corner-cut
    // check legitimately reads/writes ONE SLOT (bytes 32-35) past the tracked 32-byte array, into what
    // is _G_LLM_MAP_REGION_ROUTE_BEST_CAND in the live game image (real adjacent memory there, but a
    // genuine heap-buffer-overflow against an exactly-32-byte fixture vector under ASan). The extra 4
    // bytes are otherwise-unused fixture padding, not a second tracked region.
    std::vector<uint8_t>          region_route_cand_scratch = std::vector<uint8_t>(36);
    std::vector<uint32_t>         region_flood_tile_queue   = std::vector<uint32_t>(4096);
    std::vector<llm_map_region *> region_route_bfs_queue    = std::vector<llm_map_region *>(512);
    uint32_t                      region_coord_wrap_mask    = 0; // SIM1-H: WRITTEN by llm_map_build_regions (was read-only before batch H)
    // SIM1-H wave 2 (2026-09-10). The nav-region pipeline's own state. `path_` is 2048 entries in
    // the game; the fixture sizes it the same so a flood-fill case cannot pass by staying inside a
    // smaller buffer. PARENTHESES, not braces -- a braced vector<T>(n) is a ONE-element vector.
    std::vector<proximity_stencil_entry> proximity_stencil            = std::vector<proximity_stencil_entry>((size_t)169);
    std::vector<uint8_t>                 region_route_step_deltas     = std::vector<uint8_t>((size_t)32);
    std::vector<uint8_t>                 region_route_step_delta_wrap = std::vector<uint8_t>((size_t)4);
    std::vector<llm_map_bfs_entry>       region_flood_path_queue      = std::vector<llm_map_bfs_entry>((size_t)2048);
    int32_t                              bldg_completion_accum        = 0;
    uint32_t                             unitq_cur_tile               = 0;
    int32_t                              unitq_closed_count = 0, unitq_iter = 0, unitq_frontier_count = 0;
    std::vector<unitq_search_node>       unitq_closed         = std::vector<unitq_search_node>(1024);
    std::vector<unitq_search_node>       unitq_frontier       = std::vector<unitq_search_node>(128);
    uint32_t                             pathtrace_coord_mask = 0, pathtrace_col_mask = 0, pathtrace_row_mask = 0;
    uint32_t                             pathtrace_map_w = 0, pathtrace_map_h = 0, pathtrace_half_w = 0, pathtrace_half_h = 0;
    uint32_t                             pathtrace_half_w_m1 = 0, pathtrace_half_h_m1 = 0;
    int32_t                              pathtrace_neg_half_w = 0, pathtrace_neg_half_h = 0;
    uint32_t                             pathtrace_start_col = 0, pathtrace_start_row = 0, pathtrace_walk_dir = 0;
    uint32_t                             pathtrace_goal_col = 0, pathtrace_goal_row = 0, pathtrace_goal_packed = 0;
    uint32_t                             pathtrace_approach_dir = 0;
    std::vector<uint8_t>                 pathtrace_dirs         = std::vector<uint8_t>(512);
    std::vector<uint16_t>                pathtrace_pos          = std::vector<uint16_t>(512);
    uint32_t                             pathtrace_best_dir     = 0;
    int32_t                              pathtrace_best_dist    = 0;
    std::vector<uint32_t>                pathtrace_forbid_cells = std::vector<uint32_t>(7);
    // SIM1-G2 (2026-08-20). llm_strat_pathfind_route_leg_group_and_sort's per-call distance scratch
    // (mutable) + read-only wave/heading-scale tuning constants + the dir-step delta table.
    int32_t group_move_dist_ref_x = 0, group_move_dist_ref_y = 0;
    int32_t group_move_dist_half_width = 0, group_move_dist_half_height = 0;
    int32_t unit_chase_result          = 0;
    double  group_move_wave_dist_scale = 1.0, group_move_wave_dist_bias = 0.0;
    double  move_path_heading_scale_far = 1.0, move_path_heading_scale_mid = 1.0;
    // SIM1-G3 (2026-08-21). llm_strat_unit_state_enter_wait's boot-constant backoff
    // (_G_LLM_STRAT_UNIT_ENTER_WAIT_ACTIVITY_BACKOFF, real value 0.05 -- see sim_state.cpp).
    double enter_wait_activity_backoff_seconds = 0.05;
    // SIM1-G3 (2026-08-21). llm_strat_unit_state_enter_walk_in's soldier-transport step-cost
    // multiplier (_G_LLM_STRAT_UNIT_ENTER_WALK_SOLDIER_STEP_SCALE, real value 0.5 -- see sim_state.cpp).
    double enter_walk_in_soldier_transport_mult = 0.5;
    // SIM1-G3 (2026-08-21). llm_strat_unit_state_{takeoff_taxi,dock_taxi_in}'s
    // boot-constant step-cost multipliers (_G_LLM_STRAT_UNIT_{TAKEOFF_TAXI,DOCK_TAXI}_STEP_SPEED_MULT,
    // real value 2.0 for both -- DIFFERENT cells, same value -- see sim_state.cpp).
    double taxi_takeoff_step_speed_mult = 2.0;
    double taxi_dock_step_speed_mult    = 2.0;
    // SIM1-G3 (2026-08-21). llm_strat_unit_state_takeoff_landing's boot-constant step-cost
    // scale and A_HELIPAD landing-complete activity bump (_G_LLM_STRAT_UNIT_TAKEOFF_LANDING_STEP_COST_
    // SCALE=2.0, _...HELIPAD_ACTIVITY_BUMP=3.0 -- see sim_state.cpp).
    double               takeoff_landing_step_cost_scale         = 2.0;
    double               takeoff_landing_a_helipad_activity_bump = 3.0;
    std::vector<uint8_t> map_dir_step_deltas                     = std::vector<uint8_t>(56); // [28][2] flat, unsigned
    // SIM1-G2 (2026-08-20). llm_strat_trace_greedy_path / pathtrace_remove_loops's
    // read-only supporting tables. Zero-initialized like map_dir_step_deltas above -- a test that
    // needs specific real content sets the relevant entries itself (the real game values, captured
    // via read-memory 2026-08-20: dir_bitmask_table = {1,2,4,8,16,32,64,128}; coord_sign_lut[i] =
    // sign(int8_t(i)) (0 at i=0, 1 for 1..127, 0xff for 128..255); dir8_step_offsets (8 signed
    // (dx,dy) pairs) = {0,-1, 1,-1, 1,0, 1,1, 0,1, -1,1, -1,0, -1,-1}; move_dir_table's first 8
    // records (dir_code 0..7) have dcol/drow matching those same 8 deltas).
    std::vector<uint32_t> dir_bitmask_table       = std::vector<uint32_t>(8);
    std::vector<uint8_t>  coord_sign_lut          = std::vector<uint8_t>(256);
    std::vector<uint8_t>  move_dir_table          = std::vector<uint8_t>(24 * 8); // raw bytes, 24 records
    std::vector<int8_t>   dir8_step_offsets       = std::vector<int8_t>(16);      // [8][2], signed
    std::vector<uint8_t>  pathtrace_dir_merge_lut = std::vector<uint8_t>(64);     // [8][8]
    // SIM1A; 25 entries since LT1E (2026-09-02). The live table's own region
    // (RID_STRAT_FACING_STEP_SIGN) is llm_vec2i[25]: the boot writer llm_strat_unit_facing_offset_init
    // fills indices 1..24 and slot 0 is never written, so a 24-entry vector was one short for any
    // case exercising the writer's full range. See facing_step_offset_pair's comment in sim_state.h.
    std::vector<facing_step_offset_pair> facing_step_offset{25};
    map_geom                             geom{};
    std::vector<unit_storage>            storage{(size_t)(MAX_PLAYERS * STORAGE_PER_PLAYER)};
    // Real extents (256x256), same "out-of-range lands in fixture memory" rule as everything else
    // here -- SIM1A's llm_strat_unit_attack_target_is_dead indexes it with (tile_x << 8) | tile_y.
    std::vector<tile_object> tile_objects{(size_t)(256 * 256)};
    // uint8_t is a scalar element type, so a default member initializer written `{N}` resolves to
    // the initializer_list<uint8_t> constructor (a ONE-element vector holding the byte value N)
    // instead of the size constructor -- the trap this file's own header warns about, caught by
    // ASan on this file's first run (heap-buffer-overflow at order_seq_id_by_player(2), 2026-08-08).
    // Bare parentheses are not valid default-member-initializer syntax (C2059), so the fix is an
    // explicit temporary: `= vector<uint8_t>(N)` direct-initializes via the size constructor.
    std::vector<uint8_t> order_seq_id = std::vector<uint8_t>((size_t)MAX_PLAYERS);

    // ---- SIM1C: llm_strat_order_queue_dispatch's regions ---------------------------------------
    // Same "real extents" rule as above -- an out-of-range index the original computes must land in
    // the fixture's own memory to be observable. `text_scratch` gets the game buffer's real 512
    // BYTES, i.e. 256 wchar_t, because a shape that overruns it should overrun here too.
    std::vector<turret>     turrets{(size_t)(MAX_PLAYERS * TURRETS_PER_PLAYER)};
    std::vector<production> productions{(size_t)(MAX_PLAYERS * PRODUCTIONS_PER_PLAYER)};
    std::vector<lab>        labs{(size_t)(MAX_PLAYERS * LABS_PER_PLAYER)};
    std::vector<order>      order_queue{(size_t)ORDER_QUEUE_CAP};
    // SIM1B. mines has no mutable sibling in sim_store -- llm_strat_bldg_instant_construct_find_
    // slot_enqueue only reads it via sim_view.
    std::vector<mine>    mines{(size_t)(MAX_PLAYERS * MINES_PER_PLAYER)};
    std::vector<wchar_t> text_scratch = std::vector<wchar_t>(256);

    // The profile/population/text tables the dispatch path READS (sim_view group 1 and 2).
    std::vector<player_profile> profiles{(size_t)MAX_PLAYERS};
    // AN ARRAY, ONE RECORD PER PLAYER -- not a single struct. sim_state.cpp binds
    // v.population from RID_STRAT_POP_STATS, 416 bytes over MAX_PLAYERS = 52 per record, and three
    // building arms read `v.population[x.player].human`. A one-record fixture makes that an
    // out-of-bounds read past the whole sim_fixture object for any player but 0 -- silent in the
    // plain build, an ASan report in the checked one, and a WRONG BRANCH either way. Found while
    // writing the arm-1/2/12 cases, 2026-08-08.
    std::vector<pop_stats> population{(size_t)MAX_PLAYERS};
    // [heading][MICROSTEPS_PER_HEADING]. 8 headings is the dir8 domain; the fixture is what makes an
    // out-of-domain heading land in its own memory rather than the game's.
    std::vector<move_microstep> move_microsteps{(size_t)(8 * MICROSTEPS_PER_HEADING)};
    // The localised string table, as POINTERS. Left null: no case asserts on message TEXT (the whole
    // formatting path is presentation and behind the effect seam), only on which reason id was
    // chosen. A case that needs a real string sets an entry itself.
    // ONE vector, BOTH halves since 2026-08-31: llm_strat_scenario_planet_clone writes one SLOT
    // (sim_store::text_ptr_at). The element stays `const wchar_t *` -- what is mutable is the
    // slot, never the string -- and `const wchar_t **` converts to the view's
    // `const wchar_t *const *` implicitly, so the read and write halves address one copy.
    std::vector<const wchar_t *> text_ptrs{806};

    int32_t order_queue_count              = 0;
    double  lockstep_horizon               = 0.0;
    uint8_t net_lockstep_flags             = 0;
    int32_t engage_candidate_scratch_count = 0;
    int32_t sim_active                     = 0;
    int32_t foreign_bldg_change_flag       = 0;
    int32_t tutorial_step                  = 0;
    int16_t player_side                    = 0;
    int32_t planet_index                   = 0;
    int32_t session_mode                   = 0;
    double  game_clock                     = 0.0;
    double  lockstep_step_size             = 0.0;
    int32_t current_system                 = 0;   // SIM1F second slice
    double  game_speed                     = 1.0; // SIM1F second slice, MUTABLE
    // SIM1F, MUTABLE. PARENS, NOT BRACES -- see planet_status's comment above.
    std::vector<double> planet_time                     = std::vector<double>((size_t)32);
    double              planet_available_notify_delay_v = 60.0; // SIM1F second slice
    int32_t             cam_col                         = 0;    // SIM1F fifth slice
    int32_t             cam_row                         = 0;    // SIM1F fifth slice
    // SIM1F, MUTABLE ADDRESS ESCAPES -- see sim_store::available_buildings_row/
    // available_projects_bucket's comments. PARENS, NOT BRACES.
    std::vector<int32_t> available_buildings = std::vector<int32_t>((size_t)(8 * AVAILABLE_BUILDINGS_ROW_INTS));
    std::vector<int32_t> available_projects =
        std::vector<int32_t>((size_t)(8 * AVAILABLE_PROJECTS_TYPES * AVAILABLE_PROJECTS_BUCKET_INTS));
    std::vector<double> planet_invasion_time = std::vector<double>((size_t)32); // SIM1F third slice, MUTABLE

    int32_t map_width    = 0;
    int32_t map_height   = 0;
    double  zoom_scale_x = 1.0; // SIM1F third slice
    // SIM1F. The region-graph plane + BFS scratch -- real EXTENTS, same "an
    // out-of-range index lands in fixture memory" rule as tile_objects/etc above. PARENS.
    std::vector<llm_map_region_cell> region_grid                = std::vector<llm_map_region_cell>((size_t)(MAP_GRID_DIM * MAP_GRID_DIM));
    std::vector<llm_map_bfs_entry>   region_bfs_queue           = std::vector<llm_map_bfs_entry>((size_t)2048);
    int32_t                          foreign_bldg_event_pending = 0;
    // SIM1F. See sim_state.h's comments above.
    std::vector<double> invasion_alert_time = std::vector<double>((size_t)32);
    // Sized 17, not 16: llm_strat_count_landing_spots reads landing_spots[i].status BEFORE its i<16
    // bound, so an all-16-active table dereferences [16] once. In the live binary that lands in the
    // region's neighbour bytes; the 17th element here gives the offline oracle the same valid read
    // (slot 16 is never a live landing spot -- landing_spot_at only indexes 0..15).
    std::vector<landing_spot> landing_spots = std::vector<landing_spot>((size_t)17);
    // The ACTIVE region list head. nullptr (no live regions) unless a case builds a fixture chain via
    // llm_map_region fixture nodes (owned by each region TU's own selftest, not this shared fixture --
    // region NODES are heap objects in production, so there is no "real extent" for them here).
    llm_map_region *region_list_head       = nullptr;
    uint32_t        region_merge_threshold = 4;

    // LT1C (2026-09-02). The rest of the region-pool slots -- the allocator is now in the closure
    // (map_block_8_GetNextBlock / llm_map_region_free / llm_map_region_pool_reset), and since the
    // pool trio is UNARMABLE (malloc/free in the bodies), this fixture IS its oracle. Node ARENAS
    // stay each TU's own selftest's job (region nodes are heap objects, no real extent here);
    // these are just the five fixed slots. region_alloc_counter starts at 1 -- index 0 is never
    // allocated, the live pool_reset writes exactly 1.
    llm_map_region               *region_pool_free_head = nullptr;
    std::vector<llm_map_region *> region_by_index       = std::vector<llm_map_region *>((size_t)4096);
    int32_t                       region_alloc_counter  = 1;
    int32_t                       last_map_index        = 0; // live-node COUNT despite the name

    // LT1D (2026-09-02). llm_menu_force_return_to_main's teardown slots + the ambient event table
    // (whole-region byte escape -- the record layout overlaps by design, see
    // sim_store::snd_ambient_by_planet_base()).
    void                *ui_menu_async_callback_base = nullptr;
    uint8_t              ui_menu_state               = 0;
    int32_t              quit_teardown_forced_flag   = 0;
    std::vector<uint8_t> snd_ambient_by_planet       = std::vector<uint8_t>((size_t)23168);

    // LT1C c4 (2026-09-02). recompute_cell_grid's two island outputs: byte[100][8][8] + int32[100].
    std::vector<uint8_t> bldg_cell_grid           = std::vector<uint8_t>((size_t)6400);
    std::vector<int32_t> bldg_cell_grid_row_shift = std::vector<int32_t>((size_t)100);

    // LT1 const singles, seeded with the REAL image values so offline cases compute what the
    // binary computes (the advisor_interval seeding precedent above).
    double               rng_norm_divisor         = 65535.0;
    double               invasion_alert_interval  = 25.0;
    double               math_percent_divisor     = 100.0;
    std::vector<uint8_t> deploy_formation_stencil = std::vector<uint8_t>((size_t)49);

    // SIM1B. The cfg Building section record -- only `.total` is read.
    cfg_building_section cfg_building_sec{};
    // SIM1D. The cfg Unit section record -- only `.total` is read.
    cfg_unit_section cfg_unit_sec{};
    // SIM1D. Planets[32] -- see sim_view::cfg_planets's comment.
    std::vector<cfg_planet> cfg_planets{(size_t)32};
    // RID_G_PLANET_STATUS, int32 stride per planet -- see sim_view::planet_status's comment. Was
    // bound in sim_state.h/.cpp but never given a fixture member until the production-completion
    // pipeline oracle needed it (llm_strat_prod_deliver_arrivals' local-player ctrl-group gate).
    // PARENS, NOT BRACES: {(size_t)32} on an int32_t element type silently picks the
    // initializer_list<int32_t> ctor (a ONE-element vector valued 32) instead of the size-32 ctor --
    // the exact `ring_counts{128}` bug class build_selftest.bat's own --asan banner documents. Caught
    // by ASan on this exact line during this slice.
    std::vector<int32_t> planet_status = std::vector<int32_t>((size_t)32);
    // SIM1B. The torus wrap masks, DISTINCT from geom.width_mask/height_mask above (same "two real
    // masks, not equal to each other or its own square" rule -- see reset()'s note on geom's).
    uint32_t width_m  = 0;
    uint32_t height_m = 0;
    // SIM1B. _G_LLM_ANIM_PLACE_DENIED/_ALLOWED, DISTINCT values so a swapped-index translation
    // disagrees with the fixture.
    int32_t anim_place_seq_ids[2] = {0x1111, 0x2222};

    // ---- SIM1A (llm_strat_unit_tick and neighbours) -------------------------------
    // CUR_UNIT is a POINTER-valued global; the fixture points it into the SAME `units` storage the
    // roster accessors use (index 0 by default), matching the real-game invariant that
    // map_object_unit* always names a roster slot -- see sim_unit_update_soldiers.h's own
    // uncertainty note. A case that needs "the current unit" to be a DIFFERENT record than
    // units[0][0] repoints cur_unit_ptr itself before calling view()/store().
    unit *cur_unit_ptr = nullptr;
    // SIM1B (building_tick machinery). CUR_BUILDING is a SEPARATE pointer-valued global
    // from cur_unit_ptr (see sim_state.h's cur_building comment) -- same "points into the SAME
    // storage the roster accessors use" convention as cur_unit_ptr; a case that needs a different
    // record repoints cur_building_ptr itself before calling view()/store().
    building *cur_building_ptr = nullptr;
    // _G_LLM_STRAT_CUR_PLAYER/_CUR_INDEX are separate ambient globals from cur_unit_ptr (see
    // sim_state.h) -- named `view_*` to avoid colliding with u()/b()'s own `player`/`index` params.
    uint16_t view_cur_player  = 0;
    uint16_t view_cur_index   = 0;
    double   tick_budget      = 0.0;
    int32_t  state_loop_guard = 0;
    // SIM1D. _G_LLM_STRAT_PROD_COMPLETE_THROTTLE -- see sim_state.h's
    // prod_complete_throttle() comment. Not per-player.
    uint8_t prod_complete_throttle = 0;
    // Real extent (10 entries, 0x194 bytes each -- see sim_state.h's ctrl_group alias).
    std::vector<ctrl_group> ctrl_groups{10};
    // Real extent (2501 frames -- see sim_state.h's anim_frame alias). Left zeroed; a case that
    // needs a specific chain shape (time/next) sets its own entries.
    std::vector<anim_frame> anim_frames{2501};
    int32_t                 player_race = 0;
    // Real extent (256x256), same rule as tile_objects above. PARENTHESES, NOT BRACES -- uint8_t is
    // scalar, so `{N}` would resolve to the initializer_list<uint8_t> ctor (see order_seq_id above).
    std::vector<uint8_t> passable = std::vector<uint8_t>((size_t)(256 * 256));
    // Real extent [MAX_PLAYERS][SOLDIERS_PER_PLAYER] -- see sim_state.h's soldier alias.
    std::vector<soldier> soldiers{(size_t)(MAX_PLAYERS * SOLDIERS_PER_PLAYER)};
    // SIM1A: per-sprite metadata table (RID_SPRITE_META, MF_VIEW, no writer in the sim
    // closure). Real extent (22000, see sprite_meta_entry alias in sim_state.h) so a sprite index
    // llm_strat_unit_soldier_get_sprite_screen_pos computes out of range lands in fixture memory
    // rather than the game's. sprite_meta_entry is a struct, so `{(size_t)N}` resolves to the size
    // ctor (the initializer_list<uint8_t> trap this header warns about is scalar-element-only).
    std::vector<sprite_meta_entry> sprite_meta{(size_t)22000};
    // SIM1-G4: llm_strat_bldg_sprite_anchor_offset's per-frame byte-offset table
    // (RID_SPRITE_PIX_OFFSETS, parallel to sprite_meta) and the live pixel-bank base pointer
    // (RID_GFX_BANK_PIXELS, a pointer-to-pointer -- same shape as region_list_head below). A case
    // that needs a real gfx_sprite_hdr sets gfx_bank_pixels_storage bytes and sprite_pix_offsets[idx].
    std::vector<uint32_t>      sprite_pix_offsets = std::vector<uint32_t>((size_t)22000);
    std::vector<uint8_t>       gfx_bank_pixels_storage;
    const uint8_t             *gfx_bank_pixels_base = nullptr;
    std::vector<housing_stats> unit_housing{(size_t)MAX_PLAYERS};
    // ---- SIM1A (llm_strat_path_free_slot / _remove_from_map / _on_destroyed) --------
    // Real extent [MAX_PLAYERS][100] -- see sim_state.h's path_slot_flag_at().
    std::vector<uint8_t> path_slot_flags = std::vector<uint8_t>((size_t)(MAX_PLAYERS * 100));
    // PARENTHESES, NOT BRACES: `std::vector<int32_t> v{(size_t)MAX_PLAYERS}` list-initializes a
    // ONE-element vector holding the VALUE MAX_PLAYERS (int32_t is arithmetic, so the single-size_t
    // argument non-narrowingly converts and matches the initializer_list<int32_t> ctor) -- the exact
    // trap this file's own header banner warns about, caught by ASan on this file's first real use
    // (heap-buffer-overflow in path_free_slot_count_at(3), SIM1A selftest, 2026-08-11).
    std::vector<int32_t> path_free_slot_count = std::vector<int32_t>((size_t)MAX_PLAYERS);
    // Plain scalars, not per-player -- see sim_state.h's click_select_target_id()/
    // sim_view::click_select_target_flags.
    uint16_t click_select_target_id    = 0;
    uint16_t click_select_target_flags = 0;

    // SIM1B. llm_strat_bldg_check_placement_encloses_neighbors's private 256x256 scratch grid --
    // see sim_state.h's bldg_enclosure_scratch_at(). Real extent, same rule as tile_objects/passable.
    std::vector<uint8_t> bldg_enclosure_scratch = std::vector<uint8_t>((size_t)(256 * 256));

    // SIM1B (building_tick machinery slice). Real extents -- see sim_state.h's `player_progress`/
    // PLAYER_RESOURCE_SLOTS/PROGRESS_ROW_COUNT comments.
    std::vector<player_progress> progress{(size_t)(MAX_PLAYERS * PROGRESS_ROW_COUNT)};
    std::vector<int32_t>         player_resources = std::vector<int32_t>((size_t)(MAX_PLAYERS * PLAYER_RESOURCE_SLOTS));
    // SIM1B (building_tick machinery slice). _G_LLM_STRAT_POWER_STATS[MAX_PLAYERS], MUTABLE (see
    // sim_state.h's power_stats_at()). Named `power_stats_rows`, not `power_stats`, so the member
    // does not shadow the `power_stats` TYPE name for the rest of this class body.
    std::vector<power_stats> power_stats_rows{(size_t)MAX_PLAYERS};
    // SIM1B (2026-08-13 fix). A_OGIEN[4] -- see sim_state.h's `pip_fire_anim_frames` comment. Seeded
    // with distinct non-zero values in reset() so a translation that misreads the index is caught.
    int32_t pip_fire_anim_frames[4] = {0};

    // SIM1B (building_tick machinery). Plain int32_t scalars (not per-player) -- see
    // sim_state.h's cam_pan_target_col()/_row().
    int32_t cam_pan_target_col = 0;
    int32_t cam_pan_target_row = 0;
    // _G_LLM_STRAT_PLANET_MOTHER_LOST_TIME[32] -- see sim_state.h's planet_mother_lost_time_at().
    std::vector<double> planet_mother_lost_time = std::vector<double>((size_t)32);
    // _G_LLM_STRAT_DEATH_ANIM_TABLE -- real extent settled 2026-08-21 at 6 rows x 4 entries (see
    // sim_view::death_anim_table's comment). Kept generously over-sized here (64 rows) so an
    // out-of-range `trace`/`debris_anim_row` value in a test still lands in fixture memory rather than
    // corrupting the heap; NOT a claim that the real table has more than 6 rows.
    std::vector<int32_t> death_anim_table = std::vector<int32_t>((size_t)(64 * 4));
    // _G_LLM_STRAT_UI_PANEL_MODE/_PAGE -- see sim_view::ui_panel_mode/_page's comment.
    int32_t ui_panel_mode = 0;
    int32_t ui_panel_page = 0;
    // SIM1F (2026-08-18): game_SetEvent's UI-panel/event-queue/chat-input state.
    int32_t ui_panel_switch_pending       = 0;
    int32_t ui_bldg_panel_refresh_pending = 0;
    int32_t ui_unit_panel_refresh_pending = 0;
    int32_t ui_mainpanel_refresh_pending  = 0;
    int32_t ui_mainpanel_tab_index        = 0;
    int32_t ui_unit_tab_toggle            = 0;
    int32_t ui_view_resize_pending        = 0;
    int32_t ui_event_defer_active         = 0;
    int32_t ui_event_queue_pos            = 0;
    uint8_t ui_bldg_tab_select_blocked    = 0; // 1-byte flag -- see sim_view::ui_bldg_tab_select_blocked
    int32_t chat_input_active             = 0;
    int32_t chat_input_len                = 0;
    int32_t chat_input_cursor             = 0;
    // PARENS, NOT BRACES (initializer_list<int32_t> trap this header's banner documents). QUEUE_BUF is
    // game_e_event[256] in the game; the fallback table is int[4] (0-terminated, empty at boot).
    std::vector<int32_t> ui_event_queue          = std::vector<int32_t>((size_t)256);
    std::vector<int32_t> ui_panel_fallback_table = std::vector<int32_t>((size_t)4);
    std::vector<char>    chat_input_line         = std::vector<char>((size_t)81);
    // SIM1F (2026-08-18): llm_strat_player_presence_lost's state.
    // debug_resource_yield_cut is a MUTABLE double (presence_lost stores 0.5); the three flags +
    // cheat_cmd_table are read-only. system_define_index_base is the System record's leading two
    // int32 fields (invention@+0, name/define_index@+1), stride 0x8c/4 = 35 ints, 32 systems --
    // read-only; presence_lost is the first offline test to touch it (see sim_view's comment).
    double               debug_resource_yield_cut   = 0.0;
    int32_t              debug_campaign_cheat       = 0;
    int32_t              mp_ally_victory_rule_flag  = 0;
    int32_t              system_lost_msg_shown_flag = 0;
    std::vector<char *>  cheat_cmd_table            = std::vector<char *>((size_t)48);
    std::vector<int32_t> system_define_index_base   = std::vector<int32_t>((size_t)(32 * (0x8c / 4)));
    // _G_LLM_STRAT_UI_SELECTED_BLDG_INDEX -- see sim_state.h's ui_selected_bldg_index().
    uint16_t ui_selected_bldg_index = 0;
    // SIM1D. _G_LLM_PROD_SHUTTLE_SLOTS[MAX_PLAYERS][PROD_SHUTTLE_SLOTS_PER_PLAYER],
    // MUTABLE (see sim_state.h's prod_shuttle_slot_at()).
    std::vector<prod_shuttle_slot> prod_shuttle_slots{(size_t)(MAX_PLAYERS * PROD_SHUTTLE_SLOTS_PER_PLAYER)};
    // _G_LLM_STRAT_DIR8_OFFSET_TABLE[8] -- see sim_view::dir8_offsets's comment. Read-only, no
    // sim_store sibling.
    std::vector<dir8_offset> dir8_offsets{(size_t)8};
    // _G_LLM_STRAT_STORAGE_STATS[MAX_PLAYERS] -- see sim_view::storage_stats's comment. Read-only,
    // no sim_store sibling. Named `storage_stats_rows`, not `storage_stats`, so the member does not
    // shadow the `storage_stats` TYPE name (same reasoning as `power_stats_rows` above).
    std::vector<storage_stats> storage_stats_rows{(size_t)MAX_PLAYERS};
    // _G_LLM_STRAT_DIR_STEP_OFFSET_TABLE[40] -- see sim_view::dir_step_offsets's comment.
    // Read-only, no sim_store sibling.
    std::vector<dir_step_offset> dir_step_offsets{(size_t)DIR_STEP_OFFSET_COUNT};
    // SIM1F. _G_LLM_STRAT_POP_GROWTH_FACTOR -- see sim_view::pop_growth_factor's comment.
    // Read-only, no sim_store sibling. Seeded with the REAL boot value (0.1) so population_add's
    // growth branch computes the same product the game does; a case may override it.
    double pop_growth_factor = 0.1;
    // _G_LLM_STRAT_UNIT_LOST_FEEDBACK_COOLDOWN -- see sim_view::unit_lost_feedback_cooldown's
    // comment. Read-only, no sim_store sibling. Seeded with the REAL boot value (60.0) so a case
    // that asserts on a stamped deadline compares against what the game would produce.
    double unit_lost_feedback_cooldown = 60.0;
    // The three sibling boot constants in the same 8-byte-stride run -- see sim_view's comment.
    // Seeded with the REAL values so an offline case compares against what the game produces.
    double bldg_main_base_damage_mult         = 0.0001;
    double bldg_lost_feedback_cooldown_mother = 20.0;
    double bldg_lost_feedback_cooldown_other  = 60.0;
    // FLOAT, not double -- see sim_view::debris_scale_divisor.
    float debris_scale_divisor = 100.0f;
    // RID_STRAT_UNIT_DEATH_HQ_ENERGY_CREDIT (DAT_005013d0) -- see sim_view's comment. Read-only, no
    // sim_store sibling. Bound in sim_state.cpp's live path since 2026-08-14 but MISSING here until
    // 2026-08-16, so `sim_fixture::view()` handed every offline case a NULL pointer for it and the
    // first test that needed it (sim_unit_apply_damage_selftest.cpp) had to patch its own sim_view
    // copy by hand. Seeded with the REAL read-memory-confirmed value, which is -1.0: the HQ credit is
    // a per-death DECREMENT of exactly one on the slot-0 record, NOT a refund -- see the corrected
    // Ghidra plates on llm_strat_unit_apply_damage @0x0047e380 / llm_strat_bldg_apply_damage
    // @0x004710ba. (The name says CREDIT because the region was named before the sign was measured.)
    double unit_death_hq_energy_credit = -1.0;
    // SIM1-G4 (2026-08-22). Nine boot-constant doubles -- see sim_state.h's comment on
    // the matching sim_view members. Read-only, no sim_store sibling. Seeded with the REAL
    // read-memory-confirmed values.
    double hangar_recharge_period          = 20.0;
    double hangar_recharge_period_neg      = -20.0;
    double dismantle_progress_divisor      = 0.2;
    double bldg_dismantle_hq_energy_credit = -1.0;
    double rubble_sight_decay_period       = 5.0;
    double rubble_sight_decay_period_neg   = -5.0;
    double rubble_cleanup_period           = 20.0;
    double prod_retry_period               = 10.0;
    double prod_retry_period_neg           = -10.0;
    // SIM1-G4 (2026-08-22). Same gap class -- see sim_state.h's comment. Read-memory-
    // confirmed value 0.5.
    double refund_energy_factor    = 0.5;
    double mine_extract_period     = 10.0;
    double mine_extract_period_neg = -10.0;
    double mine_rescan_period      = 10.0;
    double mine_rescan_period_neg  = -10.0;
    // SIM1-G4 (2026-08-22). Same gap class -- see sim_state.h's comment. Read-memory-
    // confirmed value 60.0.
    double                            mother_lost_escalation_interval = 60.0;
    uint8_t                           bldg_completion_slot_count      = 4;
    std::vector<map_resources>        resources{64 * 64};
    std::vector<ui_base_marker_coord> ui_base_marker_coords{8};
    std::vector<char>                 dmp_path_scratch{32, '\0'};

    // SIM-RESID-IF (2026-08-31) -- backing storage for the 67 residual-writer bindings.
    // Real extents, so an index the original computes out of range lands in the
    // fixture's own memory and can be OBSERVED rather than corrupting the heap.
    double                         current_game_time                 = 0;
    double                         last_game_time                    = 0;
    double                         total_game_time                   = 0;
    int32_t                        cheat_penalty_score               = 0;
    int32_t                        debug_tap_flag                    = 0;
    std::vector<int32_t>           net_bw_stat                       = std::vector<int32_t>((size_t)2);
    uint8_t                        floating_msg_suppress_flag        = 0;
    uint8_t                        lockstep_step_mult                = 0;
    std::vector<int32_t>           planet_int_table                  = std::vector<int32_t>((size_t)32);
    double                         sim_step_interval                 = 0;
    int32_t                        save_misc_dword                   = 0;
    int32_t                        game_land_no_start_unit_flag      = 0;
    int32_t                        outer_planet_landed_flag          = 0;
    int32_t                        outer_planet_land_state           = 0;
    uint8_t                        planet_transition_state           = 0;
    int32_t                        show_unit_flags                   = 0;
    uint8_t                        rng_seed_byte                     = 0;
    double                         lockstep_adapt_next_time          = 0;
    uint8_t                        chat_target_mask                  = 0;
    uint16_t                       planet_map_pal4_white             = 0;
    uint16_t                       planet_map_pal4_black             = 0;
    uint16_t                       planet_map_pal4_magenta           = 0;
    uint16_t                       planet_map_pal4_yellow            = 0;
    uint16_t                       planet_map_pal5_black             = 0;
    uint16_t                       planet_map_pal5_magenta           = 0;
    uint16_t                       planet_map_pal5_green             = 0;
    uint16_t                       planet_map_pal5_red               = 0;
    uint16_t                       planet_map_pal5_blue              = 0;
    int32_t                        squad_bb_scan_player              = 0;
    int32_t                        squad_bb_target_owner             = 0;
    int32_t                        squad_bb_target_building_id       = 0;
    int32_t                        squad_bb_target_energy_pct        = 0;
    int32_t                        squad_bb_target_building_idx      = 0;
    std::vector<squad_status_slot> squad_status                      = std::vector<squad_status_slot>((size_t)64);
    int32_t                        squad_status_count                = 0;
    int32_t                        order_pending_count               = 0;
    int32_t                        order_staging_count               = 0;
    std::vector<double>            net_peer_horizon                  = std::vector<double>((size_t)8);
    std::vector<double>            net_peer_horizon_pending          = std::vector<double>((size_t)8);
    int32_t                        build_placement_id                = 0;
    uint32_t                       bldg_footprint_passable_save_slot = 0;
    int32_t                        advisor_phase                     = 0;

    // ---- SIM-RESID-IF re-close (2026-08-31): the backing for the 26 new sim_store bindings ------
    // Everything else in this list already existed as a READ-side field (the fixture bound it into
    // sim_view); only these five are new storage, and every one of them is a region that had no
    // binding in either half.
    //
    // System, as a RAW INT TABLE -- the same shape both halves of sim_state use for it. Sized
    // 32 systems * (0x8c / 4) ints, matching system_define_index_base above, and aliased to it in
    // store() so a write through the store is visible through the view.
    //
    // advisor_next_time: the advisor's re-arm clock. mouse_buttons_prev: one byte (NOT the 17-byte
    // save blob the registry names after it). land_dmp_scratch: the 32-byte landing .DMP filename
    // buffer, distinct from dmp_path_scratch.
    double  advisor_next_time  = 0.0;
    uint8_t mouse_buttons_prev = 0;
    // SIM1-H (2026-09-10). The four modifier-key bytes. NOT booleans -- the game's WndProc tap ORs
    // in 5 (plain) or 10 (extended), so a case that means "held" must seed one of those, and a case
    // that means "not held" must seed 0. Seeding 1 would pass a body that tests only bit 0.
    uint8_t           key_lctrl_held   = 0;
    uint8_t           key_lshift_held  = 0;
    uint8_t           key_rshift_held  = 0;
    uint8_t           key_lalt_held    = 0;
    std::vector<char> land_dmp_scratch = std::vector<char>((size_t)32);
    // SIM-RESID-C (2026-08-31). scenario_planet_name_w: the wchar_t[16] UTF-16 destination
    // llm_strat_scenario_planet_clone hands to llm_str_ansi_to_wide, whose returned pointer it
    // then stores into text_ptrs[0xa8]. Sized exactly 16, the region's registered extent, so a
    // case that overruns it is an ASan report rather than a silent pass.
    std::vector<wchar_t> scenario_planet_name_w = std::vector<wchar_t>((size_t)16);

    // ---- the READ surface those same translations declared -------------------------------------
    // Three advisor .rdata doubles, the blank-name string, the storage-stats seed delay, the
    // viewport extent, the lobby's per-slot descriptor array and its two scalars, and the MP
    // lockstep adapt delay. Real values where the original's constant is known and load-bearing;
    // see each sim_view member for what it is.
    double                   advisor_due_delay        = 0.0;
    double                   advisor_staff_threshold  = 0.0;
    double                   advisor_interval         = 0.0;
    char                     empty_name_str           = 0;
    double                   storage_stats_init_delay = 10.0;
    int32_t                  win_w                    = 640;
    int32_t                  win_h                    = 480;
    std::vector<player_desc> player_desc_slots        = std::vector<player_desc>((size_t)8);

    // SIM-RESID-F (2026-09-01): the tutorial pair's read-only surface.
    // _G_LLM_TUTORIAL_STEPS[16] -- the loaded script. Zeroed, so every op-list terminates
    // immediately (opcode 0 = end-of-list) and a case that does not seed a step is not handed a
    // plausible-looking objective. Sized 16, the region's registered extent.
    std::vector<tutorial_step_record> tutorial_steps = std::vector<tutorial_step_record>((size_t)16);
    // The count prefix. 0 = "already past the last step", so an un-seeded case takes the
    // tutorial-DONE arm rather than reading tutorial_steps[N] out of a script that has none.
    int32_t tutorial_step_count = 0;
    // int[5][3] source palette, flattened. Distinct nonzero values so a case can tell a real copy
    // from a zero-fill, and so a copy that runs one row too far is visible.
    std::vector<int32_t> tutorial_colors_rgb = {11, 12, 13, 21, 22, 23, 31, 32, 33,
                                                41, 42, 43, 51, 52, 53};
    // The menu async-callback slot the driver's active/idle gate reads. nullptr = "no menu callback
    // owns the screen", i.e. the driver runs its real body -- the arm worth exercising by default.
    const void *ui_menu_async_callback_b = nullptr;
    // The two screen ids the tutorial entry seeds the fade transition with. Distinct sentinels so a
    // case can tell src from dst and catch the two being swapped.
    int32_t  ui_screen_main_menu_id       = 0x1101;
    int32_t  ui_screen_racebck_id         = 0x1102;
    uint16_t net_local_player_slot        = 0;
    uint32_t net_lobby_scan_host_count    = 0;
    double   lockstep_session_adapt_delay = 0.0;

    // ---- SIM-RESID-IF REOPEN (2026-08-31) -------------------------------------------------------
    // ai_clock_stagger_fraction holds the original's real 0.125, because init_human_player_data's
    // stagger arithmetic is exactly what its oracle asserts. NOT the same storage as
    // ai_invasion_clock_stagger below even though the value matches -- two regions, two members,
    // and a case can retune one to prove the translation reads the right one.
    double ai_clock_stagger_fraction = 0.125;
    // The real 12-byte " wynalazek " blob, NUL included: tech_tables_reset copies it out by value,
    // so a case can compare against the literal.
    std::vector<char> invention_name_placeholder =
        std::vector<char>(std::begin(" wynalazek "), std::end(" wynalazek "));
    // map_FillDefaults' three remaining whole-region fill destinations, at their REAL extents --
    // the fill length is what the translation passes, so a short buffer here would be an overrun
    // rather than a failed assertion.
    std::vector<uint8_t> map_objects_bytes      = std::vector<uint8_t>((size_t)240000);
    std::vector<uint8_t> map_object_table_bytes = std::vector<uint8_t>((size_t)240000);
    std::vector<uint8_t> map_halfres_grid_bytes = std::vector<uint8_t>((size_t)65536);

    int32_t                       floating_msg_queue_active           = 0;
    std::vector<job_result_entry> path_job_result                     = std::vector<job_result_entry>((size_t)100);
    uint8_t                       game_mode                           = 0;
    int32_t                       tutorial_build_type_filter          = 0;
    uint32_t                      tutorial_forced_bldg_selection      = 0;
    uint8_t                       tutorial_hq_attack_scenario_done    = 0;
    int32_t                       tutorial_pending_build_placement_id = 0;
    int32_t                       tutorial_rmb_limit_flag             = 0;
    int32_t                       tutorial_reset_slot_0050a678        = 0;
    int32_t                       dlg_state_flags                     = 0;
    uint8_t                       player_control_mask                 = 0;
    std::vector<int32_t>          gfx_ui_color                        = std::vector<int32_t>((size_t)12);
    int32_t                       ui_race_sel_pending_gfx_idx         = 0;
    ui_fade_transition_state      ui_fade_transition{};
    void                         *ui_menu_async_callback_a = nullptr;
    widget_list                  *ui_menu_widget_list      = nullptr;
    ui_widget                     ui_tutorial_hint_widget{};
    ui_widget                     ui_wgt_tutorial_welcome{};
    ui_widget                     ui_wgt_menu_screen_title{};
    ui_widget                     ui_outcome_dlg_title_widget{};
    ui_widget                     ui_outcome_dlg_message_widget{};
    ui_widget                     ui_wgt_frame_menu_panel{};
    uint8_t                       injected_map_planet_slot = 0;
    int32_t                       view_size_mode           = 0;
    int32_t                       view_size_mode_save      = 0;
    map_header                    current_map_data{};
    // llm_strat_projectile_spawn's launch-offset pair. NOT an (x,y) scale pair: the first feeds both
    // axes, the second is a sign flip applied to the sin component only.
    double projectile_dist_scale = 16.0;
    double projectile_dir_y_sign = -1.0;
    // _G_LLM_STRAT_FACING_TRIG_TABLE[24] -- see sim_view::facing_trig_table's comment. Read-only,
    // no sim_store sibling. reset() fills it from the game's own init shape, INCLUDING the
    // documented off-by-one, so a translation cannot pass here and diverge in the game.
    std::vector<facing_trig> facing_trig_table = std::vector<facing_trig>((size_t)FACING_TRIG_ENTRIES);
    // _G_LLM_CAM_JUMP_QUEUE, MUTABLE, both halves -- see sim_state.h's cam_jump_offset alias for
    // why one region gets two vectors here. PARENS, not braces: `std::vector<double>{(size_t)30}`
    // would silently pick the ONE-element initializer_list ctor (the trap build_selftest.bat's
    // --asan banner documents, and which cost a session in SIM1D).
    std::vector<cam_jump_offset> cam_jump_offsets =
        std::vector<cam_jump_offset>((size_t)CAM_JUMP_QUEUE_SLOTS);
    std::vector<double> cam_jump_scales      = std::vector<double>((size_t)CAM_JUMP_QUEUE_SLOTS);
    int32_t             cam_jump_queue_count = 0;
    // _G_LLM_STRAT_PROJECTILE_POOL[1500], MUTABLE (see sim_state.h's projectile_pool_at()).
    std::vector<projectile> projectile_pool{(size_t)PROJECTILE_POOL_CAP};
    // _G_LLM_STRAT_CUR_PROJECTILE dereferenced -- see cur_unit_ptr's comment above for the same
    // "points into the SAME storage the pool accessor uses" convention. Defaults to slot 0 (the
    // pool's live-count header slot is never a real "current" target in practice, but this just
    // needs a valid address until a case repoints it); a case that needs a specific slot repoints
    // cur_projectile_ptr itself before calling view()/store().
    projectile *cur_projectile_ptr = nullptr;
    // _G_LLM_STRAT_FX_ANIMS[10000], MUTABLE (see sim_state.h's fx_anim_pool_at()).
    std::vector<fx_anim> fx_anim_pool{(size_t)10000};
    // _G_LLM_STRAT_CUR_FX_ANIM dereferenced -- same "points into the SAME storage the pool accessor
    // uses" convention as cur_projectile_ptr above. Defaults to slot 0; a case that needs a specific
    // slot repoints cur_fx_anim_ptr itself before calling view()/store().
    fx_anim *cur_fx_anim_ptr = nullptr;
    // A_DYM_POJAZD[4] -- see sim_view::a_dym_pojazd's comment. Read-only, no sim_store sibling.
    // Seeded with distinct non-zero values in reset() so a translation that misreads the index is
    // caught.
    int32_t a_dym_pojazd[4] = {0};
    // _G_LLM_STRAT_DMG_SMOKE_LEVEL_SCALE -- see sim_view::dmg_smoke_level_scale's comment. Read-only,
    // no sim_store sibling. Real value is 4.0; seeded here so a translation that forgot to apply the
    // scale at all is caught (a level computed with scale=1.0 stays 0 for any HP fraction < 1.0).
    double dmg_smoke_level_scale = 4.0;

    // ---- SIM1E fog/sight family (2026-08-16) ----------------------------------------------------
    // fog_of_war, MUTABLE, ONE buffer for both halves (0x90000 = 0x80000 visible_by_count +
    // 0x10000 discovered) -- see sim_state.h's fog_visible_by_count_at()/fog_discovered_at() for the
    // tile-major indexing that makes this one contiguous span rather than two.
    std::vector<uint8_t> fog_of_war_bytes = std::vector<uint8_t>((size_t)0x90000);
    // The ten precomputed circular-footprint tables, real extents (1/3/5/7/9/11/13/15/17/19 entries
    // -- see sim_view::sight_area's comment), each terminated with the real sentinel (x==0x80) so an
    // unseeded table's walk terminates immediately rather than reading past the vector.
    std::vector<map_t_tile_coord> sight_area_1  = std::vector<map_t_tile_coord>((size_t)1 + 1);
    std::vector<map_t_tile_coord> sight_area_2  = std::vector<map_t_tile_coord>((size_t)3 + 1);
    std::vector<map_t_tile_coord> sight_area_3  = std::vector<map_t_tile_coord>((size_t)5 + 1);
    std::vector<map_t_tile_coord> sight_area_4  = std::vector<map_t_tile_coord>((size_t)7 + 1);
    std::vector<map_t_tile_coord> sight_area_5  = std::vector<map_t_tile_coord>((size_t)9 + 1);
    std::vector<map_t_tile_coord> sight_area_6  = std::vector<map_t_tile_coord>((size_t)11 + 1);
    std::vector<map_t_tile_coord> sight_area_7  = std::vector<map_t_tile_coord>((size_t)13 + 1);
    std::vector<map_t_tile_coord> sight_area_8  = std::vector<map_t_tile_coord>((size_t)15 + 1);
    std::vector<map_t_tile_coord> sight_area_9  = std::vector<map_t_tile_coord>((size_t)17 + 1);
    std::vector<map_t_tile_coord> sight_area_10 = std::vector<map_t_tile_coord>((size_t)19 + 1);
    // Per-player human-controlled bitmask -- see sim_view::is_human's comment. All-human by default
    // (every bit set) so a case does not have to opt in just to reach the human-only branch.
    uint32_t is_human = 0xffu;
    // G_TMP_PLAYER/_x/_y/_SIGHT -- see sim_store::g_tmp_player()/etc.
    int32_t g_tmp_player = 0;
    int32_t g_tmp_x      = 0;
    int32_t g_tmp_y      = 0;
    int32_t g_tmp_sight  = 0;
    // G_OTHER_PLAYERS_MASK -- see sim_store::other_players_mask().
    uint8_t other_players_mask = 0;

    // ---- SIM1E sim_step (the domain root, 2026-08-16) ---------------------------------------------
    // _G_LLM_STRAT_AI_ENABLED -- see sim_view::ai_enabled's comment.
    int32_t ai_enabled = 0;
    // GAME_TIME_DELTA -- see sim_view::game_time_delta's comment.
    double game_time_delta = 0.0;
    // Seven boot-constant doubles, seeded with the REAL values (read-memory-confirmed this slice) --
    // see sim_view's comment above.
    double subtick_a_period      = 5.0;
    double subtick_a_period_pos  = 5.0;
    double subtick_a_period_neg  = -5.0;
    double subtick_b_period      = 5.0;
    double subtick_b_period_pos  = 5.0;
    double subtick_b_period_neg  = -5.0;
    double prod_check_period_neg = -1.0;

    // ---- SIM1F (llm_strat_spawn_invasion_force, the LAST SIM function) ---------------
    // The three AI think-cycle periods + the per-player clock stagger, seeded with the REAL
    // read-memory-confirmed values (distinct so a period-swap in the translation disagrees here) and
    // the start-unit budget. ai_active_player_count is a MUTABLE high-water counter (view read +
    // store write); seeded non-zero and non-symmetric so a wrong compare is caught.
    float   ai_move_period            = 1.0f;
    float   ai_tactic_period          = 5.0f;
    float   ai_strategy_period        = 10.0f;
    double  ai_invasion_clock_stagger = 0.125;
    int32_t ai_cfg_start_units        = 12;
    int32_t ai_active_player_count    = 0;

    // ---- SIM1F (2026-08-16) -----------------------------------------------------------
    // Boot constants, seeded with the REAL read-memory-confirmed values -- see sim_view's comment.
    double resource_decay_rate         = 0.1;
    double game_speed_factor_max       = 2.0;
    double game_speed_factor_step_up   = 1.2;
    double game_speed_factor_min       = 0.25;
    double game_speed_factor_step_down = 1.2;
    // _G_LLM_GAME_SPEED_PLAYER_FACTOR[MAX_PLAYERS], MUTABLE -- see sim_state.h's
    // game_speed_player_factor_at(). Seeded at 1.0 (the game's own default multiplier).
    std::vector<double> game_speed_player_factor = std::vector<double>((size_t)MAX_PLAYERS, 1.0);
    // _G_LLM_STRAT_RNG_STATE[4], MUTABLE -- see sim_state.h's rng_state_at(). Zeroed, matching the
    // live game's pre-seed .bss state; a case that needs a specific channel value sets it directly.
    std::vector<uint32_t> rng_state = std::vector<uint32_t>((size_t)4);
    // _G_LLM_GAME_HUMAN_PLAYER_MASK, MUTABLE -- the narrower byte mirror of `is_human` above, see
    // sim_state.h's game_human_player_mask(). All-human by default, matching `is_human`'s default.
    uint8_t game_human_player_mask = 0xffu;
    // Players[MAX_PLAYERS] flattened raw bytes (0x34 stride, see sim_state.h's player_relation_at()),
    // MUTABLE. NOT ITS OWN BUFFER: an ALIAS over `player_desc_slots` above, because in the game they
    // ARE one region -- sim_state.cpp binds player_desc_slots, player_desc_slots_mut_ and this raw
    // half all from RID_PLAYERS, and player_relation_at()'s `player * 0x34 + 8 + other` lands exactly
    // on player_desc[player].relation[other] (offset 8, uint8_t[8], sizeof(player_desc) == 0x34, both
    // static_asserted in mh_structs.gen.h). It was a SEPARATE vector until 2026-09-01, which meant a
    // case could write a relation and read a descriptor that did not see it -- a post-state the live
    // build cannot produce. llm_game_start_tutorial writes BOTH halves of the same slots, so its
    // oracle is the first that could tell the difference. Zeroed via player_desc_slots' own reset;
    // relation 0 is neither self/ally(2) nor any other named value, so a case that reads a relation
    // before writing it is not handed a plausible-looking default.
    uint8_t *players_raw = reinterpret_cast<uint8_t *>(player_desc_slots.data());

    // The original per-state dispatch table is LIVE GAME CODE -- an offline fixture cannot host real
    // entries (nothing to call). Filled with pointers to a local no-op so a state-machine loop that
    // reaches the dispatch in a test does not crash the process; it will not reproduce a real state
    // handler's behaviour, so any case that needs the loop body to actually run belongs to the shadow
    // arm (a live game process), not this offline oracle. Sized generously past every named state
    // value in llm_strat_unit_state.
    static void                unit_state_noop() {}
    std::vector<unit_state_fn> unit_state_funcs = std::vector<unit_state_fn>(256, &unit_state_noop);
    // The three building dispatch tables (SIM1B building_tick promotion-oracle session, 2026-08-13,
    // G19) -- same "offline fixture cannot host real entries, fill with a local no-op" posture as
    // unit_state_funcs above. Sized to the real registered extents (255 / 100 / 100).
    static void                bldg_state_noop() {}
    static void                bldg_done_noop() {}
    static void                bldg_tick2_noop() {}
    std::vector<bldg_state_fn> bldg_state_funcs = std::vector<bldg_state_fn>(255, &bldg_state_noop);
    std::vector<bldg_done_fn>  bldg_done_funcs  = std::vector<bldg_done_fn>(100, &bldg_done_noop);
    std::vector<bldg_tick2_fn> bldg_tick2_funcs = std::vector<bldg_tick2_fn>(100, &bldg_tick2_noop);

    sim_fixture() {
        cur_unit_ptr       = units.data();
        cur_building_ptr   = buildings.data();
        cur_projectile_ptr = projectile_pool.data();
        cur_fx_anim_ptr    = fx_anim_pool.data();
        reset();
    }

    void reset() {
        memset(units.data(), 0, units.size() * sizeof(unit));
        memset(buildings.data(), 0, buildings.size() * sizeof(building));
        memset(players.data(), 0, players.size() * sizeof(player_data));
        memset(cfg_units.data(), 0, cfg_units.size() * sizeof(cfg_unit));
        memset(cfg_buildings.data(), 0, cfg_buildings.size() * sizeof(cfg_building));
        memset(cfg_weapons.data(), 0, cfg_weapons.size() * sizeof(cfg_weapon));
        memset(cfg_projects.data(), 0, cfg_projects.size() * sizeof(cfg_project));
        memset(cfg_inventions.data(), 0, cfg_inventions.size() * sizeof(cfg_invention));
        memset(cfg_upgrades.data(), 0, cfg_upgrades.size() * sizeof(cfg_upgrade));
        upgrade_unit_speed_pct_divisor  = 100.0;
        upgrade_weapon_pct_divisor      = 100.0;
        upgrade_msg_sep_before_category = L' ';
        upgrade_msg_sep_before_name     = L':';
        upgrade_msg_clause_sep_first    = L' ';
        upgrade_msg_clause_sep_next     = L',';
        upgrade_msg_trailer             = L')';
        invasion_roll_base_time         = 300.0;
        local_player_slot               = 0;
        memset(group_move_scratch.data(), 0,
               group_move_scratch.size() * sizeof(group_scratch_member));
        memset(path_buffers.data(), 0, path_buffers.size() * sizeof(path_waypoint));
        memset(facing_step_offset.data(), 0,
               facing_step_offset.size() * sizeof(facing_step_offset_pair));
        memset(heading_candidates.data(), 0, heading_candidates.size() * sizeof(heading_slot));
        memset(group_step_heading_remap.data(), 0,
               group_step_heading_remap.size() * sizeof(int32_t));
        pathfinder_air_mode_flag = 0;
        memset(squad_placement_offset_table.data(), 0, squad_placement_offset_table.size());
        memset(squad_anchor_scratch.data(), 0,
               squad_anchor_scratch.size() * sizeof(squad_formation_anchor_scratch));
        memset(group_route_steps.data(), 0, group_route_steps.size() * sizeof(route_step));
        memset(group_members.data(), 0, group_members.size() * sizeof(group_member));
        memset(group_member_tile.data(), 0, group_member_tile.size());
        path_wrap_mask     = 0;
        group_order_goal_x = 0;
        group_order_goal_y = 0;
        group_anchor_x     = 0;
        group_anchor_y     = 0;
        group_order_owner  = 0;
        group_member_count = 0;
        pathtrace_len      = 0;
        // SIM1-G-PREP (2026-08-20): the batch-G write set.
        group_centroid_x = group_centroid_y = group_path_build_idx = 0;
        memset(region_route_cand_scratch.data(), 0, region_route_cand_scratch.size());
        memset(region_flood_tile_queue.data(), 0,
               region_flood_tile_queue.size() * sizeof(uint32_t));
        memset(region_route_bfs_queue.data(), 0,
               region_route_bfs_queue.size() * sizeof(llm_map_region *));
        region_coord_wrap_mask = 0;
        bldg_completion_accum  = 0;
        unitq_cur_tile         = 0;
        unitq_closed_count = unitq_iter = unitq_frontier_count = 0;
        memset(unitq_closed.data(), 0, unitq_closed.size() * sizeof(unitq_search_node));
        memset(unitq_frontier.data(), 0, unitq_frontier.size() * sizeof(unitq_search_node));
        pathtrace_coord_mask = pathtrace_col_mask = pathtrace_row_mask = 0;
        pathtrace_map_w = pathtrace_map_h = pathtrace_half_w = pathtrace_half_h = 0;
        pathtrace_half_w_m1 = pathtrace_half_h_m1 = 0;
        pathtrace_neg_half_w = pathtrace_neg_half_h = 0;
        pathtrace_start_col = pathtrace_start_row = pathtrace_walk_dir = 0;
        pathtrace_goal_col = pathtrace_goal_row = pathtrace_goal_packed = 0;
        pathtrace_approach_dir                                          = 0;
        memset(pathtrace_dirs.data(), 0, pathtrace_dirs.size());
        memset(pathtrace_pos.data(), 0, pathtrace_pos.size() * sizeof(uint16_t));
        pathtrace_best_dir  = 0;
        pathtrace_best_dist = 0;
        memset(pathtrace_forbid_cells.data(), 0,
               pathtrace_forbid_cells.size() * sizeof(uint32_t));
        // SIM1-G2 (2026-08-20).
        group_move_dist_ref_x = group_move_dist_ref_y = 0;
        group_move_dist_half_width = group_move_dist_half_height = 0;
        unit_chase_result                                        = 0;
        group_move_wave_dist_scale                               = 1.0;
        group_move_wave_dist_bias                                = 0.0;
        move_path_heading_scale_far                              = 1.0;
        move_path_heading_scale_mid                              = 1.0;
        enter_wait_activity_backoff_seconds                      = 0.05;
        enter_walk_in_soldier_transport_mult                     = 0.5;
        taxi_takeoff_step_speed_mult                             = 2.0;
        taxi_dock_step_speed_mult                                = 2.0;
        takeoff_landing_step_cost_scale                          = 2.0;
        takeoff_landing_a_helipad_activity_bump                  = 3.0;
        // LT1 lib_trans (2026-09-02).
        region_list_head      = nullptr;
        region_pool_free_head = nullptr;
        memset(region_by_index.data(), 0, region_by_index.size() * sizeof(llm_map_region *));
        region_alloc_counter        = 1; // the live pool_reset value; index 0 never allocated
        last_map_index              = 0;
        ui_menu_async_callback_base = nullptr;
        ui_menu_state               = 0;
        quit_teardown_forced_flag   = 0;
        memset(snd_ambient_by_planet.data(), 0, snd_ambient_by_planet.size());
        memset(bldg_cell_grid.data(), 0, bldg_cell_grid.size());
        memset(bldg_cell_grid_row_shift.data(), 0,
               bldg_cell_grid_row_shift.size() * sizeof(int32_t));
        rng_norm_divisor        = 65535.0;
        invasion_alert_interval = 25.0;
        math_percent_divisor    = 100.0;
        memset(deploy_formation_stencil.data(), 0, deploy_formation_stencil.size());
        memset(map_dir_step_deltas.data(), 0, map_dir_step_deltas.size());
        memset(dir_bitmask_table.data(), 0, dir_bitmask_table.size() * sizeof(uint32_t));
        memset(coord_sign_lut.data(), 0, coord_sign_lut.size());
        memset(move_dir_table.data(), 0, move_dir_table.size());
        memset(dir8_step_offsets.data(), 0, dir8_step_offsets.size());
        memset(pathtrace_dir_merge_lut.data(), 0, pathtrace_dir_merge_lut.size());
        // NOT a power of two and NOT equal to each other: the centroid's wrap is a true modulo, so
        // a translation that used a mask instead would agree with a 256x256 fixture and disagree
        // here, and one that swapped the axes would agree with a square one.
        map_width    = 200;
        map_height   = 120;
        zoom_scale_x = 1.0;
        memset(region_grid.data(), 0, region_grid.size() * sizeof(llm_map_region_cell));
        memset(region_bfs_queue.data(), 0, region_bfs_queue.size() * sizeof(llm_map_bfs_entry));
        region_list_head           = nullptr;
        region_merge_threshold     = 4;
        foreign_bldg_event_pending = 0;
        memset(invasion_alert_time.data(), 0, invasion_alert_time.size() * sizeof(double));
        memset(landing_spots.data(), 0, landing_spots.size() * sizeof(landing_spot));
        memset(&geom, 0, sizeof(geom));
        // Distinct from each other AND from width_mask == height_mask-1's tempting symmetry, and
        // both real masks (2^n - 1) so an `& mask` translation and a `% (mask+1)` one agree here but
        // a swapped-axis translation does not.
        geom.width_mask  = 0xff; // 256 - 1
        geom.height_mask = 0x3f; // 64 - 1
        // SIM1B. width_m/height_m are a SEPARATE pair of masks from geom's (see sim_state.h) --
        // deliberately different values from geom.width_mask/height_mask so a translation that
        // reached for the wrong pair disagrees with the fixture.
        width_m               = 0x7f; // 128 - 1
        height_m              = 0x1f; // 32 - 1
        anim_place_seq_ids[0] = 0x1111;
        anim_place_seq_ids[1] = 0x2222;
        memset(&cfg_building_sec, 0, sizeof(cfg_building_sec));
        memset(&cfg_unit_sec, 0, sizeof(cfg_unit_sec));
        memset(cfg_planets.data(), 0, cfg_planets.size() * sizeof(cfg_planet));
        memset(planet_status.data(), 0, planet_status.size() * sizeof(int32_t));
        memset(storage.data(), 0, storage.size() * sizeof(unit_storage));
        memset(tile_objects.data(), 0, tile_objects.size() * sizeof(tile_object));
        memset(order_seq_id.data(), 0, order_seq_id.size());

        // SIM1C
        memset(turrets.data(), 0, turrets.size() * sizeof(turret));
        memset(productions.data(), 0, productions.size() * sizeof(production));
        memset(labs.data(), 0, labs.size() * sizeof(lab));
        memset(mines.data(), 0, mines.size() * sizeof(mine));
        memset(order_queue.data(), 0, order_queue.size() * sizeof(order));
        memset(text_scratch.data(), 0, text_scratch.size() * sizeof(wchar_t));
        memset(profiles.data(), 0, profiles.size() * sizeof(player_profile));
        memset(population.data(), 0, population.size() * sizeof(pop_stats));
        memset(move_microsteps.data(), 0, move_microsteps.size() * sizeof(move_microstep));
        for (auto &p : text_ptrs) p = nullptr;
        order_queue_count              = 0;
        lockstep_horizon               = 0.0;
        net_lockstep_flags             = 0;
        engage_candidate_scratch_count = 0;
        sim_active                     = 0;
        foreign_bldg_change_flag       = 0;
        tutorial_step                  = 0;
        // NOT 0: PlayerSide 0 is a real player id, so a translation that forgot the comparison
        // entirely would agree with the fixture on every "is this the local player" gate. Pick a
        // side no case's order is owned by unless it says so.
        player_side        = 7;
        planet_index       = 0;
        session_mode       = 0;
        pop_growth_factor  = 0.1; // restore the boot value in case a case overrode it
        game_clock         = 0.0;
        lockstep_step_size = 0.0;
        current_system     = 0;
        // SIM1F: presence_lost's state.
        debug_resource_yield_cut   = 0.0;
        debug_campaign_cheat       = 0;
        mp_ally_victory_rule_flag  = 0;
        system_lost_msg_shown_flag = 0;
        for (auto &p : cheat_cmd_table) p = nullptr;
        memset(system_define_index_base.data(), 0, system_define_index_base.size() * sizeof(int32_t));
        game_speed = 1.0;
        memset(planet_time.data(), 0, planet_time.size() * sizeof(double));
        memset(planet_invasion_time.data(), 0, planet_invasion_time.size() * sizeof(double));
        planet_available_notify_delay_v = 60.0;
        cam_col                         = 0;
        cam_row                         = 0;
        memset(available_buildings.data(), 0, available_buildings.size() * sizeof(int32_t));
        memset(available_projects.data(), 0, available_projects.size() * sizeof(int32_t));

        view_cur_player        = 0;
        view_cur_index         = 0;
        tick_budget            = 0.0;
        state_loop_guard       = 0;
        prod_complete_throttle = 0;
        memset(ctrl_groups.data(), 0, ctrl_groups.size() * sizeof(ctrl_group));
        memset(anim_frames.data(), 0, anim_frames.size() * sizeof(anim_frame));
        player_race = 0;
        memset(passable.data(), 0, passable.size());
        memset(soldiers.data(), 0, soldiers.size() * sizeof(soldier));
        memset(sprite_meta.data(), 0, sprite_meta.size() * sizeof(sprite_meta_entry));
        memset(sprite_pix_offsets.data(), 0, sprite_pix_offsets.size() * sizeof(uint32_t));
        gfx_bank_pixels_storage.clear();
        gfx_bank_pixels_base = nullptr;
        memset(unit_housing.data(), 0, unit_housing.size() * sizeof(housing_stats));

        memset(path_slot_flags.data(), 0, path_slot_flags.size());
        memset(path_free_slot_count.data(), 0, path_free_slot_count.size() * sizeof(int32_t));
        click_select_target_id    = 0;
        click_select_target_flags = 0;

        // SIM1B
        memset(bldg_enclosure_scratch.data(), 0, bldg_enclosure_scratch.size());
        memset(progress.data(), 0, progress.size() * sizeof(player_progress));
        memset(player_resources.data(), 0, player_resources.size() * sizeof(int32_t));
        memset(power_stats_rows.data(), 0, power_stats_rows.size() * sizeof(power_stats));
        pip_fire_anim_frames[0] = 100;
        pip_fire_anim_frames[1] = 110;
        pip_fire_anim_frames[2] = 120;
        pip_fire_anim_frames[3] = 130;

        // SIM1B (building_tick machinery). mines/storage are memset above (SIM1B/SIM1C
        // sections); cur_building_ptr is re-pinned here since buildings.data() does not move but the
        // constructor-time assignment happens before reset()'s own memsets run.
        cur_building_ptr   = buildings.data();
        cam_pan_target_col = 0;
        cam_pan_target_row = 0;
        memset(planet_mother_lost_time.data(), 0, planet_mother_lost_time.size() * sizeof(double));
        memset(death_anim_table.data(), 0, death_anim_table.size() * sizeof(int32_t));
        ui_panel_mode          = 0;
        ui_panel_page          = 0;
        ui_selected_bldg_index = 0;
        // SIM1F: game_SetEvent's UI-panel/event-queue/chat-input state.
        ui_panel_switch_pending       = 0;
        ui_bldg_panel_refresh_pending = 0;
        ui_unit_panel_refresh_pending = 0;
        ui_mainpanel_refresh_pending  = 0;
        ui_mainpanel_tab_index        = 0;
        ui_unit_tab_toggle            = 0;
        ui_view_resize_pending        = 0;
        ui_event_defer_active         = 0;
        ui_event_queue_pos            = 0;
        ui_bldg_tab_select_blocked    = 0;
        chat_input_active             = 0;
        chat_input_len                = 0;
        chat_input_cursor             = 0;
        memset(ui_event_queue.data(), 0, ui_event_queue.size() * sizeof(int32_t));
        memset(ui_panel_fallback_table.data(), 0, ui_panel_fallback_table.size() * sizeof(int32_t));
        memset(chat_input_line.data(), 0, chat_input_line.size());

        memset(prod_shuttle_slots.data(), 0, prod_shuttle_slots.size() * sizeof(prod_shuttle_slot));
        memset(dir8_offsets.data(), 0, dir8_offsets.size() * sizeof(dir8_offset));
        memset(storage_stats_rows.data(), 0, storage_stats_rows.size() * sizeof(storage_stats));

        memset(dir_step_offsets.data(), 0, dir_step_offsets.size() * sizeof(dir_step_offset));
        memset(projectile_pool.data(), 0, projectile_pool.size() * sizeof(projectile));
        cur_projectile_ptr = projectile_pool.data();

        // SIM1E third batch
        memset(fx_anim_pool.data(), 0, fx_anim_pool.size() * sizeof(fx_anim));
        cur_fx_anim_ptr = fx_anim_pool.data();
        a_dym_pojazd[0] = 201;
        a_dym_pojazd[1] = 202;
        a_dym_pojazd[2] = 203;
        a_dym_pojazd[3] = 204;

        memset(cam_jump_offsets.data(), 0, cam_jump_offsets.size() * sizeof(cam_jump_offset));
        memset(cam_jump_scales.data(), 0, cam_jump_scales.size() * sizeof(double));
        cam_jump_queue_count               = 0;
        unit_lost_feedback_cooldown        = 60.0;
        bldg_main_base_damage_mult         = 0.0001;
        bldg_lost_feedback_cooldown_mother = 20.0;
        bldg_lost_feedback_cooldown_other  = 60.0;
        debris_scale_divisor               = 100.0f;
        unit_death_hq_energy_credit        = -1.0;
        hangar_recharge_period             = 20.0;
        hangar_recharge_period_neg         = -20.0;
        dismantle_progress_divisor         = 0.2;
        bldg_dismantle_hq_energy_credit    = -1.0;
        rubble_sight_decay_period          = 5.0;
        rubble_sight_decay_period_neg      = -5.0;
        rubble_cleanup_period              = 20.0;
        prod_retry_period                  = 10.0;
        prod_retry_period_neg              = -10.0;
        refund_energy_factor               = 0.5;
        mine_extract_period                = 10.0;
        mine_extract_period_neg            = -10.0;
        mine_rescan_period                 = 10.0;
        mine_rescan_period_neg             = -10.0;
        mother_lost_escalation_interval    = 60.0;
        bldg_completion_slot_count         = 4;
        memset(resources.data(), 0, resources.size() * sizeof(map_resources));
        memset(ui_base_marker_coords.data(), 0,
               ui_base_marker_coords.size() * sizeof(ui_base_marker_coord));
        memset(dmp_path_scratch.data(), 0, dmp_path_scratch.size());
        // SIM-RESID-IF (2026-08-31) -- the 67 residual-writer bindings, back to their construction state so
        // one case cannot see the previous case's writes.
        current_game_time   = 0;
        last_game_time      = 0;
        total_game_time     = 0;
        cheat_penalty_score = 0;
        debug_tap_flag      = 0;
        memset(net_bw_stat.data(), 0, net_bw_stat.size() * sizeof(int32_t));
        floating_msg_suppress_flag = 0;
        lockstep_step_mult         = 0;
        memset(planet_int_table.data(), 0, planet_int_table.size() * sizeof(int32_t));
        sim_step_interval            = 0;
        save_misc_dword              = 0;
        game_land_no_start_unit_flag = 0;
        outer_planet_landed_flag     = 0;
        outer_planet_land_state      = 0;
        planet_transition_state      = 0;
        show_unit_flags              = 0;
        rng_seed_byte                = 0;
        lockstep_adapt_next_time     = 0;
        chat_target_mask             = 0;
        planet_map_pal4_white        = 0;
        planet_map_pal4_black        = 0;
        planet_map_pal4_magenta      = 0;
        planet_map_pal4_yellow       = 0;
        planet_map_pal5_black        = 0;
        planet_map_pal5_magenta      = 0;
        planet_map_pal5_green        = 0;
        planet_map_pal5_red          = 0;
        planet_map_pal5_blue         = 0;
        squad_bb_scan_player         = 0;
        squad_bb_target_owner        = 0;
        squad_bb_target_building_id  = 0;
        squad_bb_target_energy_pct   = 0;
        squad_bb_target_building_idx = 0;
        memset(squad_status.data(), 0, squad_status.size() * sizeof(squad_status_slot));
        squad_status_count  = 0;
        order_pending_count = 0;
        order_staging_count = 0;
        memset(net_peer_horizon.data(), 0, net_peer_horizon.size() * sizeof(double));
        memset(net_peer_horizon_pending.data(), 0, net_peer_horizon_pending.size() * sizeof(double));
        build_placement_id                = 0;
        bldg_footprint_passable_save_slot = 0;
        advisor_phase                     = 0;

        // SIM-RESID-IF re-close (2026-08-31). Reset the new write-half backing to the same values
        // the declarations carry, so a case that does not seed one starts from a known state.
        advisor_next_time  = 0.0;
        mouse_buttons_prev = 0;
        key_lctrl_held = key_lshift_held = key_rshift_held = key_lalt_held = 0;
        memset(proximity_stencil.data(), 0,
               proximity_stencil.size() * sizeof(proximity_stencil_entry));
        memset(region_route_step_deltas.data(), 0, region_route_step_deltas.size());
        memset(region_route_step_delta_wrap.data(), 0, region_route_step_delta_wrap.size());
        memset(region_flood_path_queue.data(), 0,
               region_flood_path_queue.size() * sizeof(llm_map_bfs_entry));
        memset(land_dmp_scratch.data(), 0, land_dmp_scratch.size());
        memset(scenario_planet_name_w.data(), 0, scenario_planet_name_w.size() * sizeof(wchar_t));
        player_desc_slots.assign((size_t)8, player_desc{});
        // assign() at the same size does not reallocate, but re-point the alias anyway rather than
        // depend on that: a stale players_raw would read freed memory, silently.
        players_raw = reinterpret_cast<uint8_t *>(player_desc_slots.data());
        tutorial_steps.assign((size_t)16, tutorial_step_record{});
        tutorial_step_count          = 0;
        tutorial_colors_rgb          = {11, 12, 13, 21, 22, 23, 31, 32, 33, 41, 42, 43, 51, 52, 53};
        ui_menu_async_callback_b     = nullptr;
        ui_screen_main_menu_id       = 0x1101;
        ui_screen_racebck_id         = 0x1102;
        net_local_player_slot        = 0;
        net_lobby_scan_host_count    = 0;
        advisor_due_delay            = 0.0;
        advisor_staff_threshold      = 0.0;
        advisor_interval             = 0.0;
        empty_name_str               = 0;
        storage_stats_init_delay     = 10.0;
        lockstep_session_adapt_delay = 0.0;
        // SIM-RESID-IF reopen (2026-08-31).
        ai_clock_stagger_fraction = 0.125;
        invention_name_placeholder.assign(std::begin(" wynalazek "), std::end(" wynalazek "));
        memset(map_objects_bytes.data(), 0, map_objects_bytes.size());
        memset(map_object_table_bytes.data(), 0, map_object_table_bytes.size());
        memset(map_halfres_grid_bytes.data(), 0, map_halfres_grid_bytes.size());
        win_w                     = 640;
        win_h                     = 480;
        floating_msg_queue_active = 0;
        memset(path_job_result.data(), 0, path_job_result.size() * sizeof(job_result_entry));
        game_mode                           = 0;
        tutorial_build_type_filter          = 0;
        tutorial_forced_bldg_selection      = 0;
        tutorial_hq_attack_scenario_done    = 0;
        tutorial_pending_build_placement_id = 0;
        tutorial_rmb_limit_flag             = 0;
        tutorial_reset_slot_0050a678        = 0;
        dlg_state_flags                     = 0;
        player_control_mask                 = 0;
        memset(gfx_ui_color.data(), 0, gfx_ui_color.size() * sizeof(int32_t));
        ui_race_sel_pending_gfx_idx   = 0;
        ui_fade_transition            = ui_fade_transition_state{};
        ui_menu_async_callback_a      = nullptr;
        ui_menu_widget_list           = nullptr;
        ui_tutorial_hint_widget       = ui_widget{};
        ui_wgt_tutorial_welcome       = ui_widget{};
        ui_wgt_menu_screen_title      = ui_widget{};
        ui_outcome_dlg_title_widget   = ui_widget{};
        ui_outcome_dlg_message_widget = ui_widget{};
        ui_wgt_frame_menu_panel       = ui_widget{};
        injected_map_planet_slot      = 0;
        view_size_mode                = 0;
        view_size_mode_save           = 0;
        current_map_data              = map_header{};
        projectile_dist_scale         = 16.0;
        projectile_dir_y_sign         = -1.0;

        // The facing trig table, reproduced the way llm_strat_unit_facing_offset_init builds it --
        // INCLUDING the off-by-one documented on llm_facing_trig's own field comments (the init
        // loop reads angle[i-1] and writes trig[i], so rec[N].cos is the cosine of rec[N-1]'s angle
        // and rec[0]'s cos/sin are left uninitialised). Seeding the "obvious" aligned table instead
        // would let a translation that indexes off by one pass here and diverge in the game, which
        // is the whole reason this is spelled out rather than generated from `i * 15`.
        {
            const int32_t angles[FACING_TRIG_ENTRIES] = {270, 255, 240, 225, 210, 195, 180, 165,
                                                         150, 135, 120, 105, 90, 75, 60, 45,
                                                         30, 15, 0, 345, 330, 315, 300, 285};
            memset(facing_trig_table.data(), 0, facing_trig_table.size() * sizeof(facing_trig));
            for (int32_t i = 0; i < FACING_TRIG_ENTRIES; ++i) {
                facing_trig_table[(size_t)i].angle_deg = angles[i];
                if (i == 0) continue; // rec[0].cos/.sin: uninitialised in the original, left zero
                const double rad                 = (double)angles[i - 1] * 3.14159265358979323846 / 180.0;
                facing_trig_table[(size_t)i].cos = ::cos(rad);
                facing_trig_table[(size_t)i].sin = ::sin(rad);
            }
        }

        // SIM1E fog/sight family (2026-08-16). Zero the fog buffer; terminate every sight table with
        // the real sentinel (x==0x80) so an unseeded walk stops immediately.
        memset(fog_of_war_bytes.data(), 0, fog_of_war_bytes.size());
        for (auto *tbl : {&sight_area_1, &sight_area_2, &sight_area_3, &sight_area_4, &sight_area_5,
                          &sight_area_6, &sight_area_7, &sight_area_8, &sight_area_9,
                          &sight_area_10}) {
            memset(tbl->data(), 0, tbl->size() * sizeof(map_t_tile_coord));
            tbl->back().x = (uint8_t)0x80;
        }
        is_human              = 0xffu;
        g_tmp_player          = 0;
        g_tmp_x               = 0;
        g_tmp_y               = 0;
        g_tmp_sight           = 0;
        other_players_mask    = 0;
        ai_enabled            = 0;
        game_time_delta       = 0.0;
        subtick_a_period      = 5.0;
        subtick_a_period_pos  = 5.0;
        subtick_a_period_neg  = -5.0;
        subtick_b_period      = 5.0;
        subtick_b_period_pos  = 5.0;
        subtick_b_period_neg  = -5.0;
        prod_check_period_neg = -1.0;

        // SIM1F: spawn_invasion_force's AI clock constants + counters.
        ai_move_period            = 1.0f;
        ai_tactic_period          = 5.0f;
        ai_strategy_period        = 10.0f;
        ai_invasion_clock_stagger = 0.125;
        ai_cfg_start_units        = 12;
        ai_active_player_count    = 0;

        resource_decay_rate         = 0.1;
        game_speed_factor_max       = 2.0;
        game_speed_factor_step_up   = 1.2;
        game_speed_factor_min       = 0.25;
        game_speed_factor_step_down = 1.2;
        for (auto &f : game_speed_player_factor) f = 1.0;
        memset(rng_state.data(), 0, rng_state.size() * sizeof(uint32_t));
        game_human_player_mask = 0xffu;
        // players_raw aliases player_desc_slots, which reset() already cleared above -- clearing it
        // again here would be a second zero of the same bytes, not a second buffer.
    }

    sim_view view() const {
        sim_view v{};
        v.units                             = units.data();
        v.buildings                         = buildings.data();
        v.players                           = players.data();
        v.cfg_units                         = cfg_units.data();
        v.cfg_buildings                     = cfg_buildings.data();
        v.cfg_weapons                       = cfg_weapons.data();
        v.cfg_projects                      = cfg_projects.data();
        v.cfg_inventions                    = cfg_inventions.data();
        v.cfg_upgrades                      = cfg_upgrades.data();
        v.upgrade_unit_speed_pct_divisor    = &upgrade_unit_speed_pct_divisor;
        v.upgrade_weapon_pct_divisor        = &upgrade_weapon_pct_divisor;
        v.upgrade_msg_sep_before_category   = &upgrade_msg_sep_before_category;
        v.upgrade_msg_sep_before_name       = &upgrade_msg_sep_before_name;
        v.upgrade_msg_clause_sep_first      = &upgrade_msg_clause_sep_first;
        v.upgrade_msg_clause_sep_next       = &upgrade_msg_clause_sep_next;
        v.upgrade_msg_trailer               = &upgrade_msg_trailer;
        v.invasion_roll_base_time           = &invasion_roll_base_time;
        v.local_player_slot                 = &local_player_slot;
        v.dir24_rad2deg_num                 = &dir24_rad2deg_num;
        v.dir24_rad2deg_den                 = &dir24_rad2deg_den;
        v.dir24_half_turn_deg               = &dir24_half_turn_deg;
        v.dir24_bias_deg                    = &dir24_bias_deg;
        v.dir24_wrap_add_deg                = &dir24_wrap_add_deg;
        v.dir24_wrap_limit_deg              = &dir24_wrap_limit_deg;
        v.dir24_wrap_sub_deg                = &dir24_wrap_sub_deg;
        v.dir24_sector_deg                  = &dir24_sector_deg;
        v.dir128_rad2deg_num                = &dir128_rad2deg_num;
        v.dir128_rad2deg_den                = &dir128_rad2deg_den;
        v.dir128_half_turn_deg              = &dir128_half_turn_deg;
        v.dir128_bias_deg                   = &dir128_bias_deg;
        v.dir128_wrap_add_deg               = &dir128_wrap_add_deg;
        v.dir128_wrap_limit_deg             = &dir128_wrap_limit_deg;
        v.dir128_wrap_sub_deg               = &dir128_wrap_sub_deg;
        v.dir128_sector_deg                 = &dir128_sector_deg;
        v.bldg_count_offline_decrement      = &bldg_count_offline_decrement;
        v.unit_energy_status_percent_scale  = &unit_energy_status_percent_scale;
        v.bldg_energy_status_percent_scale  = &bldg_energy_status_percent_scale;
        v.ai_base_spawn_timer_stagger_scale = &ai_base_spawn_timer_stagger_scale;
        v.mine_worth                        = &mine_worth;
        v.map_width                         = &map_width;
        v.map_height                        = &map_height;
        v.zoom_scale_x                      = &zoom_scale_x;
        v.region_list_head                  = &region_list_head;
        v.region_merge_threshold            = &region_merge_threshold;
        v.rng_norm_divisor                  = &rng_norm_divisor;
        v.invasion_alert_interval           = &invasion_alert_interval;
        v.math_percent_divisor              = &math_percent_divisor;
        v.deploy_formation_stencil          = deploy_formation_stencil.data();
        v.group_move_scratch                = group_move_scratch.data();
        v.path_buffers                      = path_buffers.data();
        v.heading_candidates                = heading_candidates.data();
        v.group_step_heading_remap          = group_step_heading_remap.data();
        v.pathfinder_air_mode_flag          = &pathfinder_air_mode_flag;
        v.squad_placement_offset_table      = squad_placement_offset_table.data();
        v.squad_anchor_scratch              = squad_anchor_scratch.data();
        v.group_route_steps                 = group_route_steps.data();
        v.group_members                     = group_members.data();
        v.group_member_tile                 = group_member_tile.data();
        v.path_wrap_mask                    = &path_wrap_mask;
        v.group_order_goal_x                = &group_order_goal_x;
        v.group_order_goal_y                = &group_order_goal_y;
        v.group_anchor_x                    = &group_anchor_x;
        v.group_anchor_y                    = &group_anchor_y;
        v.group_order_owner                 = &group_order_owner;
        v.group_member_count                = &group_member_count;
        v.pathtrace_len                     = &pathtrace_len;
        // SIM1-G-PREP (2026-08-20): the batch-G write set (read half).
        v.group_centroid_x          = &group_centroid_x;
        v.group_centroid_y          = &group_centroid_y;
        v.group_path_build_idx      = &group_path_build_idx;
        v.region_route_cand_scratch = region_route_cand_scratch.data();
        v.region_flood_tile_queue   = region_flood_tile_queue.data();
        v.region_route_bfs_queue    = region_route_bfs_queue.data();
        v.region_coord_wrap_mask    = &region_coord_wrap_mask;
        v.bldg_completion_accum     = &bldg_completion_accum;
        v.unitq_cur_tile            = &unitq_cur_tile;
        v.unitq_closed_count        = &unitq_closed_count;
        v.unitq_iter                = &unitq_iter;
        v.unitq_frontier_count      = &unitq_frontier_count;
        v.unitq_closed              = unitq_closed.data();
        v.unitq_frontier            = unitq_frontier.data();
        v.pathtrace_coord_mask      = &pathtrace_coord_mask;
        v.pathtrace_col_mask        = &pathtrace_col_mask;
        v.pathtrace_row_mask        = &pathtrace_row_mask;
        v.pathtrace_map_w           = &pathtrace_map_w;
        v.pathtrace_map_h           = &pathtrace_map_h;
        v.pathtrace_half_w          = &pathtrace_half_w;
        v.pathtrace_half_h          = &pathtrace_half_h;
        v.pathtrace_half_w_m1       = &pathtrace_half_w_m1;
        v.pathtrace_half_h_m1       = &pathtrace_half_h_m1;
        v.pathtrace_neg_half_w      = &pathtrace_neg_half_w;
        v.pathtrace_neg_half_h      = &pathtrace_neg_half_h;
        v.pathtrace_start_col       = &pathtrace_start_col;
        v.pathtrace_start_row       = &pathtrace_start_row;
        v.pathtrace_walk_dir        = &pathtrace_walk_dir;
        v.pathtrace_goal_col        = &pathtrace_goal_col;
        v.pathtrace_goal_row        = &pathtrace_goal_row;
        v.pathtrace_goal_packed     = &pathtrace_goal_packed;
        v.pathtrace_approach_dir    = &pathtrace_approach_dir;
        v.pathtrace_dirs            = pathtrace_dirs.data();
        v.pathtrace_pos             = pathtrace_pos.data();
        v.pathtrace_best_dir        = &pathtrace_best_dir;
        v.pathtrace_best_dist       = &pathtrace_best_dist;
        v.pathtrace_forbid_cells    = pathtrace_forbid_cells.data();
        // SIM1-G2 (2026-08-20): read-only halves of the new pathfinding scratch/constants.
        v.map_dir_step_deltas                     = map_dir_step_deltas.data();
        v.group_move_wave_dist_scale              = &group_move_wave_dist_scale;
        v.group_move_wave_dist_bias               = &group_move_wave_dist_bias;
        v.move_path_heading_scale_far             = &move_path_heading_scale_far;
        v.move_path_heading_scale_mid             = &move_path_heading_scale_mid;
        v.enter_wait_activity_backoff_seconds     = &enter_wait_activity_backoff_seconds;
        v.enter_walk_in_soldier_transport_mult    = &enter_walk_in_soldier_transport_mult;
        v.taxi_takeoff_step_speed_mult            = &taxi_takeoff_step_speed_mult;
        v.taxi_dock_step_speed_mult               = &taxi_dock_step_speed_mult;
        v.takeoff_landing_step_cost_scale         = &takeoff_landing_step_cost_scale;
        v.takeoff_landing_a_helipad_activity_bump = &takeoff_landing_a_helipad_activity_bump;
        // SIM1-G2 (2026-08-20): the pathtrace tracer's read-only tables.
        v.dir_bitmask_table       = dir_bitmask_table.data();
        v.coord_sign_lut          = coord_sign_lut.data();
        v.move_dir_table          = reinterpret_cast<const move_dir_step *>(move_dir_table.data());
        v.dir8_step_offsets       = dir8_step_offsets.data();
        v.pathtrace_dir_merge_lut = pathtrace_dir_merge_lut.data();
        v.facing_step_offset      = facing_step_offset.data();
        v.geom                    = &geom;
        v.storage                 = storage.data();
        v.tile_objects            = tile_objects.data();

        v.profiles                      = profiles.data();
        v.population                    = population.data();
        v.session_mode                  = &session_mode;
        v.move_microsteps               = move_microsteps.data();
        v.sim_active                    = &sim_active;
        v.foreign_bldg_change_flag      = &foreign_bldg_change_flag;
        v.tutorial_step                 = &tutorial_step;
        v.player_side                   = &player_side;
        v.planet_index                  = &planet_index;
        v.game_clock                    = &game_clock;
        v.lockstep_step_size            = &lockstep_step_size;
        v.current_system                = &current_system;
        v.planet_available_notify_delay = &planet_available_notify_delay_v;
        v.cam_col                       = &cam_col;
        v.cam_row                       = &cam_row;
        v.text_ptrs                     = text_ptrs.data();

        // SIM1F: presence_lost's read-only inputs.
        v.debug_campaign_cheat       = &debug_campaign_cheat;
        v.mp_ally_victory_rule_flag  = &mp_ally_victory_rule_flag;
        v.system_lost_msg_shown_flag = &system_lost_msg_shown_flag;
        v.cheat_cmd_table            = cheat_cmd_table.data();
        v.system_define_index_base   = system_define_index_base.data();

        v.cur_unit         = cur_unit_ptr;
        v.cur_player       = &view_cur_player;
        v.cur_index        = &view_cur_index;
        v.tick_budget      = &tick_budget;
        v.unit_state_funcs = unit_state_funcs.data();
        v.ctrl_groups      = ctrl_groups.data();
        // SIM1-H (2026-09-10): the four modifier-key bytes.
        // SIM1-H wave 2: the 13x13 obstacle-proximity stencil (read-only boot data).
        v.proximity_stencil         = proximity_stencil.data();
        v.key_lctrl_held            = &key_lctrl_held;
        v.key_lshift_held           = &key_lshift_held;
        v.key_rshift_held           = &key_rshift_held;
        v.key_lalt_held             = &key_lalt_held;
        v.anim_frames               = anim_frames.data();
        v.player_race               = &player_race;
        v.passable                  = passable.data();
        v.soldiers                  = soldiers.data();
        v.sprite_meta               = sprite_meta.data();
        v.sprite_pix_offsets        = sprite_pix_offsets.data();
        v.gfx_bank_pixels           = &gfx_bank_pixels_base;
        v.unit_housing              = unit_housing.data();
        v.click_select_target_flags = &click_select_target_flags;

        // SIM1B
        v.turrets              = turrets.data();
        v.productions          = productions.data();
        v.labs                 = labs.data();
        v.mines                = mines.data();
        v.cfg_building_sec     = &cfg_building_sec;
        v.cfg_unit_sec         = &cfg_unit_sec;
        v.cfg_planets          = cfg_planets.data();
        v.planet_status        = planet_status.data();
        v.width_m              = &width_m;
        v.height_m             = &height_m;
        v.anim_place_seq_ids   = anim_place_seq_ids;
        v.progress             = progress.data();
        v.player_resources     = player_resources.data();
        v.pip_fire_anim_frames = pip_fire_anim_frames;
        v.cur_building         = cur_building_ptr;
        v.bldg_state_funcs     = bldg_state_funcs.data();
        v.bldg_done_funcs      = bldg_done_funcs.data();
        v.bldg_tick2_funcs     = bldg_tick2_funcs.data();
        v.death_anim_table     = death_anim_table.data();
        v.ui_panel_mode        = &ui_panel_mode;
        v.ui_panel_page        = &ui_panel_page;
        // SIM1F: game_SetEvent's read-only inputs.
        v.ui_event_defer_active      = &ui_event_defer_active;
        v.ui_bldg_tab_select_blocked = &ui_bldg_tab_select_blocked;
        v.ui_panel_fallback_table    = ui_panel_fallback_table.data();

        v.prod_shuttle_slots = prod_shuttle_slots.data();
        v.dir8_offsets       = dir8_offsets.data();
        v.storage_stats      = storage_stats_rows.data();

        v.dir_step_offsets = dir_step_offsets.data();
        v.projectile_pool  = projectile_pool.data();
        v.cur_projectile   = cur_projectile_ptr;

        // SIM1F: population_add's growth constant + the read-only landing-spot view
        // (count_landing_spots). dir_remap_table is intentionally left null -- no offline case reads
        // it (tile_neighbor_reverse_dir is verified via its live-view shadow arm, not offline).
        v.pop_growth_factor = &pop_growth_factor;
        v.landing_spots     = landing_spots.data();

        // SIM1E third batch
        v.fx_anim_pool          = fx_anim_pool.data();
        v.cur_fx_anim           = cur_fx_anim_ptr;
        v.a_dym_pojazd          = a_dym_pojazd;
        v.dmg_smoke_level_scale = &dmg_smoke_level_scale;

        v.unit_lost_feedback_cooldown        = &unit_lost_feedback_cooldown;
        v.facing_trig_table                  = facing_trig_table.data();
        v.bldg_main_base_damage_mult         = &bldg_main_base_damage_mult;
        v.bldg_lost_feedback_cooldown_mother = &bldg_lost_feedback_cooldown_mother;
        v.bldg_lost_feedback_cooldown_other  = &bldg_lost_feedback_cooldown_other;
        v.debris_scale_divisor               = &debris_scale_divisor;
        v.unit_death_hq_energy_credit        = &unit_death_hq_energy_credit;
        v.hangar_recharge_period             = &hangar_recharge_period;
        v.hangar_recharge_period_neg         = &hangar_recharge_period_neg;
        v.dismantle_progress_divisor         = &dismantle_progress_divisor;
        v.bldg_dismantle_hq_energy_credit    = &bldg_dismantle_hq_energy_credit;
        v.rubble_sight_decay_period          = &rubble_sight_decay_period;
        v.rubble_sight_decay_period_neg      = &rubble_sight_decay_period_neg;
        v.rubble_cleanup_period              = &rubble_cleanup_period;
        v.prod_retry_period                  = &prod_retry_period;
        v.prod_retry_period_neg              = &prod_retry_period_neg;
        v.refund_energy_factor               = &refund_energy_factor;
        v.mine_extract_period                = &mine_extract_period;
        v.mine_extract_period_neg            = &mine_extract_period_neg;
        v.mine_rescan_period                 = &mine_rescan_period;
        v.mine_rescan_period_neg             = &mine_rescan_period_neg;
        v.mother_lost_escalation_interval    = &mother_lost_escalation_interval;
        v.bldg_completion_slot_count         = &bldg_completion_slot_count;
        v.resources                          = resources.data();
        v.projectile_dist_scale              = &projectile_dist_scale;
        v.projectile_dir_y_sign              = &projectile_dir_y_sign;

        // SIM1E fog/sight family
        v.sight_area[0] = sight_area_1.data();
        v.sight_area[1] = sight_area_2.data();
        v.sight_area[2] = sight_area_3.data();
        v.sight_area[3] = sight_area_4.data();
        v.sight_area[4] = sight_area_5.data();
        v.sight_area[5] = sight_area_6.data();
        v.sight_area[6] = sight_area_7.data();
        v.sight_area[7] = sight_area_8.data();
        v.sight_area[8] = sight_area_9.data();
        v.sight_area[9] = sight_area_10.data();
        v.is_human      = &is_human;

        // SIM1E sim_step (the domain root)
        v.ai_enabled            = &ai_enabled;
        v.game_time_delta       = &game_time_delta;
        v.subtick_a_period      = &subtick_a_period;
        v.subtick_a_period_pos  = &subtick_a_period_pos;
        v.subtick_a_period_neg  = &subtick_a_period_neg;
        v.subtick_b_period      = &subtick_b_period;
        v.subtick_b_period_pos  = &subtick_b_period_pos;
        v.subtick_b_period_neg  = &subtick_b_period_neg;
        v.prod_check_period_neg = &prod_check_period_neg;

        v.resource_decay_rate         = &resource_decay_rate;
        v.game_speed_factor_max       = &game_speed_factor_max;
        v.game_speed_factor_step_up   = &game_speed_factor_step_up;
        v.game_speed_factor_min       = &game_speed_factor_min;
        v.game_speed_factor_step_down = &game_speed_factor_step_down;
        v.game_speed_player_factor    = game_speed_player_factor.data();

        // SIM1F: spawn_invasion_force's AI clock constants + counters (read side).
        v.ai_move_period            = &ai_move_period;
        v.ai_tactic_period          = &ai_tactic_period;
        v.ai_strategy_period        = &ai_strategy_period;
        v.ai_invasion_clock_stagger = &ai_invasion_clock_stagger;
        v.ai_cfg_start_units        = &ai_cfg_start_units;
        v.ai_active_player_count    = &ai_active_player_count;

        // SIM-RESID-IF re-close (2026-08-31): the read surface the batch A/B translations declared.
        v.advisor_due_delay            = &advisor_due_delay;
        v.advisor_staff_threshold      = &advisor_staff_threshold;
        v.advisor_interval             = &advisor_interval;
        v.empty_name_str               = &empty_name_str;
        v.storage_stats_init_delay     = &storage_stats_init_delay;
        v.win_w                        = &win_w;
        v.win_h                        = &win_h;
        v.player_desc_slots            = player_desc_slots.data();
        v.net_local_player_slot        = &net_local_player_slot;
        v.net_lobby_scan_host_count    = &net_lobby_scan_host_count;
        v.lockstep_session_adapt_delay = &lockstep_session_adapt_delay;

        // SIM-RESID-IF reopen (2026-08-31).
        v.ai_clock_stagger_fraction  = &ai_clock_stagger_fraction;
        v.invention_name_placeholder = invention_name_placeholder.data();

        // SIM-RESID-F (2026-09-01): the tutorial pair's six read-only bindings.
        v.tutorial_steps           = tutorial_steps.data();
        v.tutorial_step_count      = &tutorial_step_count;
        v.tutorial_colors_rgb      = tutorial_colors_rgb.data();
        v.ui_menu_async_callback_b = &ui_menu_async_callback_b;
        v.ui_screen_main_menu_id   = &ui_screen_main_menu_id;
        v.ui_screen_racebck_id     = &ui_screen_racebck_id;
        return v;
    }

    // The write half, field for field the same regions state() binds -- see the note there about
    // the two halves having to address one copy of the state.
    sim_store store() {
        return sim_store(units.data(), players.data(), order_seq_id.data(), buildings.data(),
                         turrets.data(), productions.data(), labs.data(), order_queue.data(),
                         &order_queue_count, &lockstep_horizon, &net_lockstep_flags,
                         text_scratch.data(), &engage_candidate_scratch_count, cur_unit_ptr,
                         &tick_budget, &state_loop_guard, ctrl_groups.data(), tile_objects.data(),
                         passable.data(), population.data(), soldiers.data(),
                         unit_housing.data(), path_slot_flags.data(), path_free_slot_count.data(),
                         &click_select_target_id, profiles.data(), &geom,
                         bldg_enclosure_scratch.data(), power_stats_rows.data(), cur_building_ptr,
                         &cam_pan_target_col, &cam_pan_target_row, planet_mother_lost_time.data(),
                         mines.data(), storage.data(), &ui_selected_bldg_index,
                         prod_shuttle_slots.data(), &prod_complete_throttle, cur_projectile_ptr,
                         projectile_pool.data(), cur_fx_anim_ptr, fx_anim_pool.data(),
                         cam_jump_offsets.data(), cam_jump_scales.data(), &cam_jump_queue_count,
                         fog_of_war_bytes.data(), fog_of_war_bytes.data() + 0x80000, &g_tmp_player,
                         &g_tmp_x, &g_tmp_y, &g_tmp_sight, &other_players_mask, &view_cur_player,
                         &view_cur_index, &cur_unit_ptr, &cur_building_ptr, &cur_projectile_ptr,
                         &cur_fx_anim_ptr, storage_stats_rows.data(),
                         game_speed_player_factor.data(), rng_state.data(), &is_human,
                         &game_human_player_mask, players_raw, progress.data(),
                         &game_speed, planet_time.data(), available_buildings.data(),
                         available_projects.data(), cfg_units.data(), cfg_weapons.data(),
                         &map_width, &map_height, planet_invasion_time.data(),
                         player_resources.data(), planet_status.data(), region_grid.data(),
                         region_bfs_queue.data(), &foreign_bldg_event_pending,
                         invasion_alert_time.data(), landing_spots.data(),
                         // SIM1F: game_SetEvent's UI-panel/event-queue/chat state.
                         &ui_panel_mode, &ui_panel_page, &ui_panel_switch_pending,
                         &ui_bldg_panel_refresh_pending, &ui_unit_panel_refresh_pending,
                         &ui_mainpanel_refresh_pending, &ui_mainpanel_tab_index, &ui_unit_tab_toggle,
                         &ui_view_resize_pending, &ui_event_queue_pos, ui_event_queue.data(),
                         &chat_input_active, &chat_input_len, &chat_input_cursor,
                         chat_input_line.data(),
                         // SIM1F: presence_lost's mutable writes.
                         &debug_resource_yield_cut, &session_mode,
                         // SIM1F: spawn_invasion_force's active-player-count bump.
                         &ai_active_player_count,
                         // SIM1-G1: the move state handlers' path buffers + group-move scratch.
                         path_buffers.data(), group_move_scratch.data(),
                         &pathfinder_air_mode_flag,
                         group_route_steps.data(), group_members.data(), &path_wrap_mask,
                         &group_order_goal_x, &group_order_goal_y, &group_anchor_x, &group_anchor_y,
                         &group_order_owner, &group_member_count, group_member_tile.data(),
                         // SIM1-G-PREP (2026-08-20): the batch-G write set. SAME ORDER as the ctor.
                         &group_centroid_x, &group_centroid_y, &group_path_build_idx,
                         region_route_cand_scratch.data(), region_flood_tile_queue.data(),
                         region_route_bfs_queue.data(), &bldg_completion_accum, &unitq_cur_tile,
                         &unitq_closed_count, &unitq_iter, &unitq_frontier_count,
                         unitq_closed.data(), unitq_frontier.data(), &pathtrace_coord_mask,
                         &pathtrace_col_mask, &pathtrace_row_mask, &pathtrace_map_w,
                         &pathtrace_map_h, &pathtrace_half_w, &pathtrace_half_h,
                         &pathtrace_half_w_m1, &pathtrace_half_h_m1, &pathtrace_neg_half_w,
                         &pathtrace_neg_half_h, &pathtrace_start_col, &pathtrace_start_row,
                         &pathtrace_walk_dir, &pathtrace_goal_col, &pathtrace_goal_row,
                         &pathtrace_goal_packed, &pathtrace_approach_dir, pathtrace_dirs.data(),
                         pathtrace_pos.data(), &pathtrace_best_dir, &pathtrace_best_dist,
                         pathtrace_forbid_cells.data(), &pathtrace_len,
                         // SIM1-G2 (2026-08-20): appended at the tail -- see sim_state.h's ctor.
                         &group_move_dist_ref_x, &group_move_dist_ref_y,
                         &group_move_dist_half_width, &group_move_dist_half_height,
                         &unit_chase_result,
                         // SIM1-G3 (2026-08-21): appended at the tail -- see
                         // sim_state.h's ctor. Same backing vector as the read-only view binding
                         // above (squad_anchor_scratch.data()), like every other dual-bound region.
                         squad_anchor_scratch.data(),
                         // SIM1-G4 (2026-08-22): appended at the tail -- see sim_state.h's
                         // ctor. resources.data() is the SAME backing vector as the read-only view
                         // binding above, like squad_anchor_scratch.
                         resources.data(), &foreign_bldg_change_flag, ui_base_marker_coords.data(),
                         dmp_path_scratch.data(),
                         &current_game_time, &last_game_time, &total_game_time,
                         &cheat_penalty_score, &debug_tap_flag, net_bw_stat.data(),
                         &floating_msg_suppress_flag, &lockstep_step_mult,
                         planet_int_table.data(), &sim_step_interval, &save_misc_dword,
                         &game_land_no_start_unit_flag, &outer_planet_landed_flag,
                         &outer_planet_land_state, &planet_transition_state, &show_unit_flags,
                         &rng_seed_byte, &lockstep_adapt_next_time, &chat_target_mask,
                         &planet_map_pal4_white, &planet_map_pal4_black,
                         &planet_map_pal4_magenta, &planet_map_pal4_yellow,
                         &planet_map_pal5_black, &planet_map_pal5_magenta,
                         &planet_map_pal5_green, &planet_map_pal5_red, &planet_map_pal5_blue,
                         &squad_bb_scan_player, &squad_bb_target_owner,
                         &squad_bb_target_building_id, &squad_bb_target_energy_pct,
                         &squad_bb_target_building_idx, squad_status.data(),
                         &squad_status_count, &order_pending_count, &order_staging_count,
                         net_peer_horizon.data(), net_peer_horizon_pending.data(),
                         &build_placement_id, &bldg_footprint_passable_save_slot,
                         &advisor_phase, &floating_msg_queue_active, path_job_result.data(),
                         &game_mode, &tutorial_build_type_filter,
                         &tutorial_forced_bldg_selection, &tutorial_hq_attack_scenario_done,
                         &tutorial_pending_build_placement_id, &tutorial_rmb_limit_flag,
                         &tutorial_reset_slot_0050a678, &dlg_state_flags, &player_control_mask,
                         gfx_ui_color.data(), &ui_race_sel_pending_gfx_idx, &ui_fade_transition,
                         &ui_menu_async_callback_a, &ui_menu_widget_list,
                         &ui_tutorial_hint_widget, &ui_wgt_tutorial_welcome,
                         &ui_wgt_menu_screen_title, &ui_outcome_dlg_title_widget,
                         &ui_outcome_dlg_message_widget, &ui_wgt_frame_menu_panel,
                         &injected_map_planet_slot, &view_size_mode, &view_size_mode_save,
                         &current_map_data,
                         // SIM-RESID-IF re-close (2026-08-31): the write halves. Each one is the
                         // SAME storage the view above binds -- that identity is the whole point,
                         // and it is what statetest's rebase consumer checks against state().
                         &current_system, &planet_index, &game_clock, &game_time_delta,
                         &player_side, &local_player_slot, &player_race, &sim_active,
                         &mp_ally_victory_rule_flag, &system_lost_msg_shown_flag,
                         &ui_bldg_tab_select_blocked, &bldg_completion_slot_count, &width_m,
                         &height_m, &tutorial_step, &ai_enabled, ui_panel_fallback_table.data(),
                         cfg_planets.data(), cfg_inventions.data(), cfg_projects.data(),
                         cfg_upgrades.data(), system_define_index_base.data(), &advisor_next_time,
                         &mouse_buttons_prev, land_dmp_scratch.data(), fog_of_war_bytes.data(),
                         // SIM-RESID-IF reopen (2026-08-31): map_FillDefaults' three remaining
                         // whole-region fill destinations, plus the MUTABLE half of
                         // death_anim_table -- the SAME vector the view binds above, because the
                         // PRESERVE-BUG write has to be observable through the read side.
                         map_objects_bytes.data(), map_object_table_bytes.data(),
                         map_halfres_grid_bytes.data(), death_anim_table.data(),
                         // SIM-RESID-C (2026-08-31): the scenario planet's widening destination.
                         scenario_planet_name_w.data(), text_ptrs.data(),
                         player_desc_slots.data(),
                         // LT1 lib_trans (2026-09-02): the pool slots, menu-teardown writes and
                         // the ambient escape.
                         &region_list_head, &region_pool_free_head, region_by_index.data(),
                         &region_alloc_counter, &last_map_index, &ui_menu_async_callback_base,
                         &ui_menu_state, &quit_teardown_forced_flag,
                         snd_ambient_by_planet.data(),
                         // LT1C c4: recompute_cell_grid's two island outputs. SAME ORDER as the ctor.
                         bldg_cell_grid.data(), bldg_cell_grid_row_shift.data(),
                         // SIM1-H (2026-09-10): the write halves of Building[] and the region wrap
                         // mask. SAME storage the read half binds -- cfg_buildings.data() is
                         // v.cfg_buildings, so a fixture seeding the read side sees the writes.
                         cfg_buildings.data(), &region_coord_wrap_mask,
                         // SIM1-H wave 2 (2026-09-10): the nav-region pipeline's write set. The
                         // merge threshold and the step deltas are the SAME storage the read half
                         // binds, so a case seeding one side sees the other's writes.
                         &region_merge_threshold, region_route_step_deltas.data(),
                         region_route_step_delta_wrap.data(), region_flood_path_queue.data());
    }

    // A BOUNDS-CHECKED accessor, and the check is not paranoia -- it is the third time this class
    // of bug has cost this project a debugging session, and the first time ASan could not name it.
    //
    // 2026-08-20, SIM1-G1: a case seeded `target_ref = 77` meaning "low nibble 5", but 77 is 0x4d,
    // so the owner nibble was 13 -- past MAX_PLAYERS. `u(13, 88)` is element 1388 of an 800-element
    // vector, i.e. a write ~68 KB PAST THE END. What made it expensive is that **ASan reported
    // nothing**: ASan finds an overrun by its redzone, and an overrun this large clears the redzone
    // entirely and lands inside another live allocation, which is a perfectly valid address. The
    // plain build then said `4823 checks, 0 failures` while quietly smearing another object, and
    // only a run with SIMTEST_TRACE=1 (different heap layout) turned it into a 0xC0000005. So:
    // ASan is necessary and NOT sufficient -- the near-miss it catches is the small overrun, and
    // this guard is what catches the far one. Abort rather than ck(): a corrupted fixture makes
    // every later assertion meaningless, so stopping at the cause beats a cascade at the symptom.
    //
    // For the ONE original that legitimately indexes `units` with an out-of-range owner, use
    // wide_u() below -- it grows the storage so the row really exists.
    // THE CHECK IS ON THE FLAT INDEX, NOT ON player<MAX_PLAYERS && index<UNITS_PER_PLAYER, and the
    // difference is the whole point. In the live image the rosters are one contiguous .bss run, so
    // `units[3][250]` IS `units[5][50]` -- an index past the end of a ROW is ordinary, faithful
    // behaviour that some originals genuinely produce, and sim_prod_unload_cargo_unit's oracle
    // relies on exactly that (a spawn id of 250 for player 3). A row-relative assert would reject
    // it. What is never faithful is leaving the ARRAY, so that is what this rejects.
    unit &u(int32_t player, int32_t index) {
        const long long flat = (long long)player * (long long)UNITS_PER_PLAYER + (long long)index;
        if (flat < 0 || flat >= (long long)units.size()) {
            printf("  FATAL: sim_fixture::u(%d, %d) -> flat element %lld, outside units[%zu] -- "
                   "use wide_u() if the ORIGINAL really indexes this far\n",
                   (int)player, (int)index, flat, units.size());
            fflush(stdout);
            abort();
        }
        return units[(size_t)flat];
    }

    // Roster row `owner` for an owner BEYOND MAX_PLAYERS, growing the storage so the row is really
    // addressable. For the one original that indexes `units` with a value it never computed:
    // llm_strat_unit_estimate_weapon_damage re-reads the [EBP-0x10] stack slot under a gate that did
    // not write it, and that slot holds the Watcom stack-probe imprint -- this function's own frame
    // size, 0x38 == 56. In the live image `units[56]` is simply another
    // part of .bss; in a fixture the row does not exist, so a case walking that path would be a heap
    // overflow and ASan would (correctly) take the run down. Widening is the honest fixture-side
    // answer: it reproduces "there is memory there", which is the only property the original relies on.
    //
    // RESIZING REALLOCATES, so the pointer-valued globals that point INTO this buffer are re-based.
    // Call before view()/store() -- exactly like repointing cur_unit_ptr by hand.
    unit &wide_u(uint32_t owner, int32_t index) {
        const size_t need = ((size_t)owner + 1u) * (size_t)UNITS_PER_PLAYER;
        if (units.size() < need) {
            const ptrdiff_t cur_off = cur_unit_ptr ? (cur_unit_ptr - units.data()) : -1;
            units.resize(need);
            if (cur_off >= 0) cur_unit_ptr = units.data() + cur_off;
        }
        return units[(size_t)owner * (size_t)UNITS_PER_PLAYER + (size_t)index];
    }
    // Same guard, same flat-index reasoning as u() above.
    building &b(int32_t player, int32_t index) {
        const long long flat =
            (long long)player * (long long)BUILDINGS_PER_PLAYER + (long long)index;
        if (flat < 0 || flat >= (long long)buildings.size()) {
            printf("  FATAL: sim_fixture::b(%d, %d) -> flat element %lld, outside buildings[%zu]\n",
                   (int)player, (int)index, flat, buildings.size());
            fflush(stdout);
            abort();
        }
        return buildings[(size_t)flat];
    }
    tile_object &t(int32_t tile_x, int32_t tile_y) { return tile_objects[(tile_x << 8) | tile_y]; }
};

} // namespace mh::sim
