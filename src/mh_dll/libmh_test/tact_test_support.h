//
// tact_test_support.h -- the shared fixture and check helpers for `net_selftest.exe tacttest`
// (RI-TACT). The tactical sibling of sim_test_support.h, and created for the same reason that one
// exists.
//
// WHY IT WAS SPLIT OUT (2026-08-25, TACT1A -- the domain's first BATCH session). Until now the
// whole suite was one file, tact_selftest.cpp, with the fixture and the `ck` helpers in its
// anonymous namespace. That shape works exactly until the first session that owes more than one
// oracle: authoring is the only expensive part of writing an oracle that parallelises, and the
// migration-session skill's fan-out is safe ONLY because each writer creates its own new file. A
// single shared suite file is precisely the collision `translate.js`'s guard refuses to spawn into
// -- which is why `ai`, still on one 12k-line ai_selftest.cpp, has to author its oracles serially.
// Splitting the support out now, with 1 translated unit, is a ten-minute job; splitting it at
// TACT1E would be the sim suite's 159-file migration.
//
// So the shape from here on is sim's: this header holds what every oracle shares, each translation
// unit gets its own `tact_<stem>_selftest.cpp` defining one `run_<stem>_tests()`, and
// tact_selftest.cpp is the runner plus the instrument-audit checks (T1-T5, T10-T13) that are about
// the suite itself rather than about any translated function.
//
// ---------------------------------------------------------------------------------------------
// FIXTURE RULES -- both of these have already cost this project a debugging session
// ---------------------------------------------------------------------------------------------
//
// SEED DISTINCT, NON-SYMMETRIC VALUES. Two fields holding the same number make a swap of those two
// fields pass. Where a body writes a pair of out-pointers, give the pair deliberately different
// values so consuming the wrong one fails.
//
// SIZE EVERY VECTOR OF A SCALAR ELEMENT WITH PARENTHESES, NOT BRACES. `std::vector<uint8_t>{65536}`
// is a ONE-element vector holding 65536 -- legal overload resolution, invisible to every warning,
// and it cost `aitest` a heap corruption on ~60% of runs.
//
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "state/mode_planes.h"
#include "tact/tact_state.h"

namespace mh::tact::test {

inline int g_checks = 0;
inline int g_fails  = 0;

// Per-check tracing is OFF by default and env-gated, because this suite's instrument half alone is
// ~370 checks and a verbose default buries the one line that matters. `TACTTEST_TRACE=1` turns it
// on, matching sim's `SIMTEST_TRACE`.
inline int trace_on() {
    static const int on = [] {
        const char *e = getenv("TACTTEST_TRACE");
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
// rebuild with a printf in it -- and the answer is usually visible in the number.
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

// Doubles compare EXACTLY, deliberately -- a tolerance would only hide a wrong operand order. The
// mission parsers make this load-bearing: `SPEED 0.010` is accumulated digit by digit as
// `acc*10 + d` and then divided by 10 per fractional place, so the expected value in a case must be
// written the same way rather than as the decimal literal a reader would guess.
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

} // namespace mh::tact::test

// ---- the fixture --------------------------------------------------------------------------------
//
// `tact_fixture` is declared a friend of BOTH `tact_store` and `mode_planes` and is one of the two
// sanctioned ways to obtain each (the other is `state()`). That is the whole of W3: the exception
// list is short enough to read, and adding to it is a diff a reviewer sees.
//
// It must be `mh::tact::tact_fixture` -- the same name in the same namespace the headers befriend --
// so it cannot live in an anonymous namespace.
namespace mh::tact {

struct tact_fixture {
    // Real extents, so an index the original computes out of range lands in the fixture's own
    // memory and can be OBSERVED rather than corrupting the heap. +1 slack (TACT1E,
    // llm_tact_select_next_unit): loop B's first read can legitimately be unit_at(TACT_UNIT_SLOTS)
    // -- one slot past the declared 129 -- when `candidate` reaches TACT_UNIT_LAST_SLOT (128), a
    // real OOB hazard in the original game reproduced literally (see that header's own derivation).
    std::vector<tact_unit>  units       = std::vector<tact_unit>((size_t)TACT_UNIT_SLOTS + 1);
    std::vector<strat_unit> strat_units = std::vector<strat_unit>(800);
    std::vector<cfg_unit>   cfg_units   = std::vector<cfg_unit>(100);
    // llm_strat_player_profile[8] -- the RID_STRAT_PLAYERS record type (retyped 2026-08-26 with
    // tact_view::strat_players; see tact_state.h's `player_profile` using).
    std::vector<player_profile> strat_players   = std::vector<player_profile>(8);
    int32_t                     planet_index    = 0;
    int32_t                     map_width       = mh::tact::TACT_MAP_DIM;
    int32_t                     map_height      = mh::tact::TACT_MAP_DIM;
    int32_t                     sidebar_mouse_x = 0;
    int32_t                     sidebar_mouse_y = 0;
    // The tile-height-sprite table llm_tact_map_compute_bounds scans: 128 cols * 128 rows * 8
    // uint16_t slots, at its real stride (col*1024 + row*8 + k in uint16_t units).
    std::vector<uint16_t> tile_height_sprites =
        std::vector<uint16_t>((size_t)(mh::tact::TACT_MAP_DIM * mh::tact::TACT_MAP_DIM * 8));
    // Declared AFTER tile_height_sprites so its default member initializer (evaluated in
    // declaration order) sees a valid .data(). Two bindings of the SAME data() pointer -- one
    // const (the view), one mutable (the store, llm_tact_map_reset's write path) -- exactly the
    // shape tact_state.h documents for this region's real binding.
    const uint16_t *tile_height_sprites_base     = tile_height_sprites.data();
    uint16_t       *tile_height_sprites_base_mut = tile_height_sprites.data();
    int32_t         squad_size                   = 0;
    // The mission's unit CLASS table, at its REAL 16-slot extent (RID_TACT_CHARACTER_TYPES is
    // 1728 B / 0x6c). Real extent so a CHARACTER number the parser accepts but should not lands
    // in the fixture's own memory and can be OBSERVED rather than corrupting the heap.
    std::vector<mh::tact::character_type> character_types =
        std::vector<mh::tact::character_type>((size_t)mh::tact::TACT_CHARACTER_TYPE_SLOTS);
    // Mission-loaded gun/explosion definitions, real 64-slot extent (RID_TACT_FX_TYPE_TABLE is
    // 4736 B / 0x4a). llm_tact_unit_weapon_in_range's only reads.
    std::vector<mh::tact::fx_type> fx_type_table =
        std::vector<mh::tact::fx_type>((size_t)mh::tact::TACT_FX_TYPE_SLOTS);
    // The squad-status blackboard, at its real 64-slot extent (RID_SQUAD_STATUS is 1024 B / 0x10).
    std::vector<mh::game::mh_llm_squad_status_slot> squad_status =
        std::vector<mh::game::mh_llm_squad_status_slot>((size_t)mh::tact::TACT_SQUAD_STATUS_SLOTS);

    // llm_tact_tile_rebuild_occupancy_layer_for_map's own scratch global, and the FX pool the
    // inner function reads for its occupancy stamp (both TACT1A batch A, real 1024-slot extent).
    int32_t occupancy_rebuild_map_id = 0;
    // +1 slack: mission_load's init-clear loop writes fx_at(1024) -- one past the 1024 real slots
    // (JLE 0x400, preserved literally); the overrun must land in owned fixture memory.
    std::vector<mh::tact::fx_entry> fx_pool =
        std::vector<mh::tact::fx_entry>((size_t)(mh::tact::TACT_FX_POOL_SLOTS + 1));

    // TACT1B (llm_tact_unit_move_tick / llm_tact_unit_owner_tick). Real extent for the
    // octant table; the other three are scalars.
    std::vector<mh::tact::dir8_delta> dir8_delta_table =
        std::vector<mh::tact::dir8_delta>((size_t)8);

    // TACT1D: the FOV raycaster's three ray-delta tables, seedable per case. Sized to the real
    // tables (short[72]/[120]/[360] -> 144/240/720 BYTES); a case seeds the {dx,dy} pair it needs at
    // 2*angle_index. Default all-zero = every ray's accumulator never carries, i.e. a cast that
    // marks only its centre cell -- a deliberate, inert default, not a realistic one.
    std::vector<int8_t> fov_delta_table_72         = std::vector<int8_t>(144);
    std::vector<int8_t> fov_delta_table_120        = std::vector<int8_t>(240);
    std::vector<int8_t> fov_delta_table_360        = std::vector<int8_t>(720);
    double              unit_wander_retry_interval = 2.0; // matches the live constant (get-data confirmed)
    int32_t             move_cur_col               = 0;
    int32_t             move_cur_row               = 0;
    int32_t             move_path_cache_valid      = 0;
    uint8_t             move_flood_result_col      = 0;
    uint8_t             move_flood_result_row      = 0;
    int32_t             move_path_slot_id          = -1;
    double              mine_blast_time_end        = 0.0; // 0 -> "now" always clears the op==9 gate by default
    double              mine_blast_duration        = 5.0; // an arbitrary non-zero mission-loaded constant
    int32_t             blast_marker_col           = 0;
    int32_t             blast_marker_row           = 0;
    int32_t             view_size_mode             = 0;
    double              last_game_time             = 0.0;
    uint8_t             game_mode                  = 6; // live default: tactical, matches a real excursion
    // E_PLANET_STATUS[32] (/Manual/game/E_PLANET_STATUS), real 32-planet extent.
    std::vector<int32_t> planet_status = std::vector<int32_t>((size_t)32);

    // llm_tact_teleport_cmdqueue_jump's scratch-slot write path (TACT1B), real 66-slot extent.
    std::vector<mh::tact::teleport_zone> teleport_zones =
        std::vector<mh::tact::teleport_zone>((size_t)mh::tact::TACT_TELEPORT_ZONE_SLOTS);

    // The mode-shared planes, at their REAL [256][256] extent -- not the 128x128 sub-block a mission
    // uses. That difference is the point of the preview-clear case: a fixture sized to the
    // sub-block could not tell a correct clear from one that ran off the end.
    std::vector<mh::state::tile_object> tile_objects =
        std::vector<mh::state::tile_object>(256u * 256u);
    std::vector<uint8_t>                  passable = std::vector<uint8_t>(256u * 256u);
    std::vector<mh::state::path_waypoint> path_buffers =
        std::vector<mh::state::path_waypoint>((size_t)(8 * mh::state::PATH_WAYPOINTS_PER_OWNER));
    std::vector<uint8_t> path_slot_flags =
        std::vector<uint8_t>((size_t)(8 * mh::state::PATH_SLOTS_PER_OWNER));
    std::vector<int32_t> path_free_slot_count = std::vector<int32_t>(8);

    mh::state::mode_planes planes() {
        return mh::state::mode_planes(tile_objects.data(), passable.data(), path_buffers.data(),
                                      path_slot_flags.data(), path_free_slot_count.data());
    }

    // llm_tact_move_step_attempt's (TACT1B) flood-fill scratch START/GOAL/RESULT bytes, previously
    // frontier-owned.
    uint8_t move_flood_start_col_v = 0;
    uint8_t move_flood_start_row_v = 0;
    uint8_t move_flood_goal_col_v  = 0;
    uint8_t move_flood_goal_row_v  = 0;

    // The SHARED (both-mode) map extent, RID_WIDTH/RID_HEIGHT -- llm_tact_move_path_build's own
    // read (TACT1A/B). Matches TACT_MAP_DIM, same as the tactical-only map_width/map_height cache
    // above (a coincidence of this fixture's defaults, not a shared binding).
    int32_t grid_width  = mh::tact::TACT_MAP_DIM;
    int32_t grid_height = mh::tact::TACT_MAP_DIM;

    // llm_strat_squad_assault_resolve's (TACT1A batch A) blackboard reads, real scalars.
    int32_t squad_bb_scan_player              = 0;
    int32_t squad_bb_target_owner             = 0;
    int32_t squad_bb_target_building_idx      = 0;
    int32_t squad_bb_target_energy_pct        = 0;
    double  squad_assault_power_percent_scale = 100.0; // matches the live constant
    double  bldg_energy_to_percent_scale      = 100.0; // matches the live constant
    double  bldg_energy_percent_to_abs_scale  = 100.0; // matches the live constant

    // llm_tact_move_path_build's (TACT1A/B) own private solver scratch -- real extents for the two
    // BFS wave arrays (4096 uint16_t = 8192 B each) and the cost map (65536 uint32_t = 262144 B),
    // matching the live region sizes exactly.
    uint32_t              move_path_coord_mask      = 0;
    uint32_t              move_path_goal_packed     = 0;
    uint32_t              move_path_start_packed    = 0;
    uint32_t              move_path_tile_cost       = 0;
    uint32_t              move_path_trace_tile      = 0;
    uint32_t              move_path_trace_base      = 0;
    uint32_t              move_path_trace_diag_cand = 0;
    uint8_t               move_path_trace_best_dir  = 0;
    uint32_t              move_path_trace_best_tile = 0;
    uint32_t              move_path_trace_cost_sum  = 0;
    uint32_t              move_path_rle_count       = 0;
    uint32_t              move_path_queue_cur_addr  = 0;
    std::vector<uint16_t> move_path_queue_a         = std::vector<uint16_t>((size_t)4096);
    std::vector<uint16_t> move_path_queue_b         = std::vector<uint16_t>((size_t)4096);
    std::vector<uint32_t> move_path_cost_map        = std::vector<uint32_t>((size_t)0x10000);
    int32_t               unit_active_count         = 0;

    // TACT1A/TACT1C central batch (2026-08-26): llm_tact_mission_start's write set, the
    // weapons_tick move cursor's write half, and fx_update_projectile's pool write path.
    std::vector<int32_t> bank_sprite_base = std::vector<int32_t>((size_t)47); // 188 B / 4
    int32_t              window_width = 800, window_height = 600, saved_window_width = 0;
    int32_t              gfx_view_tile_height_px = 0;
    int32_t              map_cam_col = 0, map_cam_row = 0;
    int32_t              view_tiles_w = 0, view_tiles_h = 0;
    double               cam_col_f = 0.0, cam_row_f = 0.0;
    int32_t              click_action_taken = 0, scroll_cmd = 0;
    // The framebuffer + draw surface behind their pointer VARIABLES (real-shape rows so
    // mission_start's 0x140-byte row copies land in observable fixture memory).
    std::vector<uint8_t> framebuffer       = std::vector<uint8_t>((size_t)(1024u * 768u));
    uint8_t             *framebuffer_base  = framebuffer.data();
    std::vector<uint8_t> draw_surface      = std::vector<uint8_t>((size_t)(1024u * 768u));
    const uint8_t       *draw_surface_base = draw_surface.data();
    int32_t              current_system    = 1;
    uint8_t              key_rshift_held = 0, key_lshift_held = 0;
    // Distinct non-symmetric values per the fixture rules -- a family swap must fail.
    double  unit_anim_step_sec_1 = 0.25, unit_anim_jitter_scale_1 = 0.5;
    double  unit_anim_step_sec_2 = 0.125, unit_anim_jitter_scale_2 = 0.75;
    double  unit_anim_frame_interval_base = 1.0;
    int32_t mines_enabled                 = 0;
    int32_t fx_live_count                 = 0;

    // llm_tact_mission_load's write set (2026-08-26). The fx-type table carries ONE EXTRA slack
    // record: mission_load's init-clear loop deliberately overruns (fx pool JLE 0x400 = 1025 writes
    // -- preserved literally per Law 2), and in the fixture that overrun must land in owned,
    // observable memory, not the heap (ASan).
    std::vector<mh::tact::fx_type> fx_type_table_mut =
        std::vector<mh::tact::fx_type>((size_t)(mh::tact::TACT_FX_TYPE_SLOTS + 1));
    // Door table: TACT1D (2026-08-27) found a SECOND, LARGER overrun beyond mission_load's own
    // (JLE 0x10 = 17 writes). llm_tact_door_tick/llm_tact_door_apply_to_map both loop slots
    // 1..0x1f inclusive (CMP ...,0x20), touching index 31 -- 15 slots past the Ghidra-declared
    // `llm_tact_door[16]` (RID_TACT_DOOR_TABLE's registered extent is still 16*0x68 = 1664 B;
    // widening the LIVE region is deferred). Confirmed benign in the real
    // binary (no named symbol anywhere in the extra 15*0x68 bytes past the declared array,
    // en_addr_index.json), but the offline fixture's OWN buffer must cover every index either
    // function walks or a garbage `.id`/`.state` collision at slot 17-31 is a heap-buffer overflow,
    // not a game-state bug. Sized to 32 (covers slots 0..31 exactly the loops reach).
    std::vector<mh::tact::door_record> door_table             = std::vector<mh::tact::door_record>((size_t)32);
    int32_t                            enemy_count            = 0;
    int32_t                            see_enemy_flag         = 0;
    int32_t                            mine_blast_first_frame = 0;
    int32_t                            mine_blast_frame_count = 0;
    int32_t                            mine_blast_sound_id    = 0;
    int32_t                            quit_tile_col = 0, quit_tile_row = 0;
    int32_t                            target_tile_col = 0, target_tile_row = 0;
    std::vector<void *>                char_panel_gfx = std::vector<void *>((size_t)16);
    void                              *file_ptr_v     = nullptr;
    // mission_load's read-only inputs (distinct values per the fixture rules; the live .rdata
    // biases are all 32.0 -- oracles that pin the bias arithmetic should seed distinct values).
    int32_t who_xor_key             = 0;
    double  colision1_bias          = 32.0;
    double  colision2_bias          = 32.0;
    double  teleport_death_bias     = 32.0;
    double  door_open_hold_time_sec = 2.0; // matches the live constant (byte-verified)

    // TACT1C batch (2026-08-26): unit_fire_weapon / fx_splash_damage / unit_get_muzzle_offset.
    int32_t                            hovered_unit_id = -1; // -1 = no hovered unit, matches the live default
    std::vector<mh::tact::sprite_meta> sprite_meta_table =
        std::vector<mh::tact::sprite_meta>((size_t)64); // small real-shaped extent, enough headroom
    int32_t fx_splash_spare_nonzero_owner = 0;          // live value is ALWAYS 0 (no writer)

    // TACT1D batch (2026-08-27): the FOV raycaster's cross-function scratch. Distinct non-symmetric
    // seed values per the fixture rules -- the two CELL/DIST families must not swap silently.
    int32_t              fov_col = 3, fov_row = 5, fov_angle_base = 7, fov_angle_width = 11, fov_dist = 13;
    std::vector<uint8_t> fov_stencil            = std::vector<uint8_t>((size_t)(64 * 64));
    int16_t              fov_nearest_hibit_cell = 0, fov_nearest_low_cell = 0;
    int32_t              fov_nearest_hibit_dist = 0, fov_nearest_low_dist = 0;
    int32_t              fov_candidate_dist = 17; // DEAD READ (ghidra_findings.json 2026-08-27-0017-1): no
                                                  // writer anywhere in the real program either -- seeded
                                                  // non-zero so a case that (wrongly) treats it as live is
                                                  // observable rather than degenerating to the 0-init case.
    // RID_TILE_VIS_MAP real 300 B extent.
    std::vector<uint8_t> tile_vis_map = std::vector<uint8_t>((size_t)300);
    // Sprite-bank pixel lookup (RID_GFX_BANK_PIXELS / RID_SPRITE_PIX_OFFSETS), view-only (no
    // tactical writer): real-shaped so a frame lookup lands in owned, observable memory instead of
    // the heap (ASan). GFX_BANK_PIXELS is itself a POINTER VARIABLE (same shape as
    // tile_height_sprites_base above), hence the extra indirection.
    std::vector<uint8_t>  gfx_bank_pixels_data    = std::vector<uint8_t>((size_t)4096);
    const uint8_t        *gfx_bank_pixels_base    = gfx_bank_pixels_data.data();
    std::vector<uint32_t> sprite_pix_offsets_data = std::vector<uint32_t>((size_t)128);

    // TACT1E (2026-08-27): llm_tact_ambient_sound_tick's /sound-subsystem reads (first tactical
    // reader of any of these). Distinct non-symmetric values per the fixture rules.
    int32_t              snd_enabled                       = 1;
    int32_t              snd_master_volume                 = 10000;
    double               ambient_snd_min_interval_sec      = 20.0; // matches the live constant (byte-verified)
    int32_t              ambient_snd_zone_count            = 3;
    std::vector<int32_t> ambient_snd_zone_table            = std::vector<int32_t>((size_t)10);
    double               snd_channel_next_retrigger_time_v = 0.0;
    // llm_snd_cfg_entry[200], PADDED with 4 extra entries BEFORE index 0: one real call site
    // (tact_ambient_sound.cpp's header banner, the G28 stack-probe-watermark derivation) reads
    // table[-4] -- a genuine, deterministic OUT-OF-BOUNDS read in the ORIGINAL game, reproduced
    // literally (Law 2). The padding makes that read land in owned, observable fixture memory
    // instead of a heap-buffer-underflow (ASan). snd_cfg_table_base() below is entry 0, i.e.
    // snd_cfg_table_storage.data() + 4.
    std::vector<mh::tact::snd_cfg_entry> snd_cfg_table_storage =
        std::vector<mh::tact::snd_cfg_entry>((size_t)204);
    mh::tact::snd_cfg_entry *snd_cfg_table_base() { return snd_cfg_table_storage.data() + 4; }

    // TACT1E (2026-08-27): the frame pump + selection/sidebar/panel residue. Distinct non-symmetric
    // seed values per the fixture rules.
    int32_t cam_scroll_up_held = 0, cam_scroll_down_held = 0, cam_scroll_left_held = 0,
            cam_scroll_right_held          = 0;
    int32_t              exit_confirm_open = 0, click_scan_scratch = 0;
    int32_t              drag_anchor_x = 0, drag_anchor_y = 0, drag_select_active = 0;
    int32_t              active_unit_count = 0, active_unit_count_cached = 0;
    std::vector<uint8_t> tile_drawn_map        = std::vector<uint8_t>((size_t)300); // real RID_TILE_DRAWN_MAP extent
    int32_t              cam_follow_selection  = 1;                                 // live default: following
    int32_t              cam_follow_target_col = 21, cam_follow_target_row = 23;
    int32_t              cursor_x = 0, cursor_y = 0;
    uint8_t              mouse_buttons_cur           = 0;
    int32_t              win_w                       = 640;                             // matches a stock 640x480-ish viewport
    std::vector<void *>  sel_panel_icon_gfx          = std::vector<void *>((size_t)41); // real RID_TACT_SEL_PANEL_ICON_GFX extent
    int32_t              ui_sel_panel_multi_mode     = 0;
    std::vector<int32_t> sel_panel_icon_slot_state   = std::vector<int32_t>((size_t)4); // real extent
    int32_t              sel_panel_icon_count        = 0;
    std::vector<uint8_t> sel_panel_icon_names        = std::vector<uint8_t>((size_t)410); // real byte extent
    int32_t              sidebar_active_group_id     = 0;
    int32_t              sidebar_slot_scroll         = 0;
    std::vector<int32_t> sidebar_scrollbtn_state     = std::vector<int32_t>((size_t)8); // real extent
    int32_t              sidebar_ui_hit_code         = 0;
    int32_t              sidebar_highlighted_unit_id = 0;
    std::vector<int32_t> sidebar_slot_unit_ids       = std::vector<int32_t>((size_t)64); // real RID extent (retyped 2026-08-27)
    // Sized 160 B, not the LUT's OWN declared 40 B (10 int32 slots): TACT1E's sidebar_dispatch
    // reads byte offsets [0x50,0x9c) from this base (hit_code in [0x14,0x28), *4), past the LUT's
    // declared extent and into what the real binary's adjacent _G_LLM_TACT_UNASSIGNED_UNIT_ROSTER
    // occupies (tact_sidebar_dispatch.h's own derivation). The two are SEPARATE vectors here, so
    // sizing this one to cover the real read range keeps it inside OWNED, observable memory (ASan)
    // rather than reproducing the real cross-region adjacency byte-for-byte.
    std::vector<uint8_t>         sidebar_icon_slot_unit_lut = std::vector<uint8_t>((size_t)160);
    std::vector<uint8_t>         unassigned_unit_roster     = std::vector<uint8_t>((size_t)256);  // real extent
    std::vector<uint8_t>         group_unit_roster          = std::vector<uint8_t>((size_t)2048); // real extent
    int32_t                      gfx_panel_row_skip         = 0;
    std::vector<const wchar_t *> text_ptrs{806}; // real RID_G_TEXT_PTRS extent, matches sim_test_support.h
    // Two POINTER VARIABLES, same shape as framebuffer_base above -- real-shaped backing buffers so
    // the view-shift row/column copies land in owned, observable memory.
    std::vector<uint8_t> tile_vis_map_buf   = std::vector<uint8_t>((size_t)(1024u * 8u));
    uint8_t             *tile_vis_map_ptr_v = tile_vis_map_buf.data();
    std::vector<uint8_t> los_cache_buf      = std::vector<uint8_t>((size_t)(1024u * 8u));
    uint8_t             *los_cache_ptr_v    = los_cache_buf.data();

    // TACT1E batch (2026-08-28).
    uint8_t              key_lctrl_held                   = 0;
    uint8_t              key_lalt_held                    = 0;
    int32_t              tact_ui_minimap_origin_x         = 400;
    int32_t              tact_ui_minimap_origin_y         = 300;
    int32_t              sidebar_multi_panel_visible_rows = 10; // real image default (byte-verified: 0x0a)
    int32_t              sidebar_slot_visible_count       = 5;  // real image default (byte-verified: 0x05)
    const wchar_t       *sidebar_fmt_unit_id_v            = L"%2d";
    const wchar_t       *sidebar_fmt_unit_name_v          = L"%s";
    int32_t              sidebar_unassigned_scroll_row    = 0;
    int32_t              sidebar_group_scroll_row         = 0;
    std::vector<wchar_t> text_scratch_buf                 = std::vector<wchar_t>((size_t)256); // real RID_G_TEXT_TMP extent (512 B)

    tact_store store() {
        return tact_store(units.data(), &sidebar_mouse_x, &sidebar_mouse_y, character_types.data(),
                          &map_width, &map_height, &tile_height_sprites_base_mut,
                          squad_status.data(), planes(),
                          &occupancy_rebuild_map_id, &move_path_cache_valid, &move_path_slot_id,
                          teleport_zones.data(), &move_flood_start_col_v, &move_flood_start_row_v,
                          &move_flood_goal_col_v, &move_flood_goal_row_v, &move_flood_result_col,
                          &move_flood_result_row, &move_path_coord_mask, &move_path_goal_packed,
                          &move_path_start_packed, &move_path_tile_cost, &move_path_trace_tile,
                          &move_path_trace_base, &move_path_trace_diag_cand,
                          &move_path_trace_best_dir, &move_path_trace_best_tile,
                          &move_path_trace_cost_sum, &move_path_rle_count,
                          &move_path_queue_cur_addr, move_path_queue_a.data(),
                          move_path_queue_b.data(), move_path_cost_map.data(),
                          &unit_active_count, &mine_blast_time_end, &blast_marker_col,
                          &blast_marker_row, &game_mode, planet_status.data(),
                          bank_sprite_base.data(), &window_width, &window_height,
                          &saved_window_width, &gfx_view_tile_height_px, &map_cam_col,
                          &map_cam_row, &grid_width, &grid_height, &view_tiles_w, &view_tiles_h,
                          &cam_col_f, &cam_row_f, &click_action_taken, &scroll_cmd,
                          &framebuffer_base, &move_cur_col, &move_cur_row, fx_pool.data(),
                          &fx_live_count, fx_type_table_mut.data(), door_table.data(), &squad_size,
                          &enemy_count, &see_enemy_flag, &mine_blast_first_frame,
                          &mine_blast_frame_count, &mine_blast_duration, &mine_blast_sound_id,
                          &quit_tile_col, &quit_tile_row, &target_tile_col, &target_tile_row,
                          char_panel_gfx.data(), &file_ptr_v,
                          // TACT1D batch (2026-08-27).
                          &fov_col, &fov_row, &fov_angle_base, &fov_angle_width, &fov_dist,
                          fov_stencil.data(), &fov_nearest_hibit_cell, &fov_nearest_low_cell,
                          &fov_nearest_hibit_dist, &fov_nearest_low_dist, tile_vis_map.data(),
                          // TACT1E batch (2026-08-27).
                          &snd_channel_next_retrigger_time_v, &cam_scroll_up_held,
                          &cam_scroll_down_held, &cam_scroll_left_held, &cam_scroll_right_held,
                          &exit_confirm_open, &click_scan_scratch, &drag_anchor_x, &drag_anchor_y,
                          &drag_select_active, &hovered_unit_id, &active_unit_count,
                          &active_unit_count_cached, tile_drawn_map.data(), &cam_follow_selection,
                          sel_panel_icon_gfx.data(), &ui_sel_panel_multi_mode,
                          sel_panel_icon_slot_state.data(), &sidebar_active_group_id,
                          &sidebar_slot_scroll, sidebar_scrollbtn_state.data(),
                          &sidebar_ui_hit_code, &sidebar_highlighted_unit_id,
                          sidebar_slot_unit_ids.data(), &squad_bb_target_energy_pct,
                          &tile_vis_map_ptr_v, &los_cache_ptr_v,
                          // TACT1E batch (2026-08-28).
                          &sidebar_unassigned_scroll_row, &sidebar_group_scroll_row,
                          unassigned_unit_roster.data(), group_unit_roster.data(),
                          &mouse_buttons_cur, text_scratch_buf.data());
    }

    tact_view view() {
        tact_view v{};
        v.units                             = units.data();
        v.map_width                         = &map_width;
        v.map_height                        = &map_height;
        v.sidebar_mouse_x                   = &sidebar_mouse_x;
        v.sidebar_mouse_y                   = &sidebar_mouse_y;
        v.map_tile_height_sprites           = &tile_height_sprites_base;
        v.squad_size                        = &squad_size;
        v.strat_units                       = strat_units.data();
        v.cfg_units                         = cfg_units.data();
        v.strat_players                     = strat_players.data();
        v.planet_index                      = &planet_index;
        v.character_types                   = character_types.data();
        v.fx_type_table                     = fx_type_table.data();
        v.tile_objects                      = tile_objects.data();
        v.passable                          = passable.data();
        v.fx_pool                           = fx_pool.data();
        v.dir8_delta_table                  = dir8_delta_table.data();
        v.fov_delta_table_72                = fov_delta_table_72.data();
        v.fov_delta_table_120               = fov_delta_table_120.data();
        v.fov_delta_table_360               = fov_delta_table_360.data();
        v.unit_wander_retry_interval        = &unit_wander_retry_interval;
        v.move_cur_col                      = &move_cur_col;
        v.move_cur_row                      = &move_cur_row;
        v.move_flood_result_col             = &move_flood_result_col;
        v.move_flood_result_row             = &move_flood_result_row;
        v.move_path_slot_id                 = &move_path_slot_id;
        v.mine_blast_time_end               = &mine_blast_time_end;
        v.mine_blast_duration               = &mine_blast_duration;
        v.view_size_mode                    = &view_size_mode;
        v.last_game_time                    = &last_game_time;
        v.move_flood_start_col              = &move_flood_start_col_v;
        v.move_flood_start_row              = &move_flood_start_row_v;
        v.move_flood_goal_col               = &move_flood_goal_col_v;
        v.move_flood_goal_row               = &move_flood_goal_row_v;
        v.grid_width                        = &grid_width;
        v.grid_height                       = &grid_height;
        v.squad_bb_scan_player              = &squad_bb_scan_player;
        v.squad_bb_target_owner             = &squad_bb_target_owner;
        v.squad_bb_target_building_idx      = &squad_bb_target_building_idx;
        v.squad_bb_target_energy_pct        = &squad_bb_target_energy_pct;
        v.squad_assault_power_percent_scale = &squad_assault_power_percent_scale;
        v.bldg_energy_to_percent_scale      = &bldg_energy_to_percent_scale;
        v.bldg_energy_percent_to_abs_scale  = &bldg_energy_percent_to_abs_scale;
        // TACT1A/TACT1C central batch (2026-08-26).
        v.current_system                = &current_system;
        v.gfx_draw_surface              = &draw_surface_base;
        v.key_rshift_held               = &key_rshift_held;
        v.key_lshift_held               = &key_lshift_held;
        v.unit_anim_step_sec_1          = &unit_anim_step_sec_1;
        v.unit_anim_jitter_scale_1      = &unit_anim_jitter_scale_1;
        v.unit_anim_step_sec_2          = &unit_anim_step_sec_2;
        v.unit_anim_jitter_scale_2      = &unit_anim_jitter_scale_2;
        v.unit_anim_frame_interval_base = &unit_anim_frame_interval_base;
        v.mines_enabled                 = &mines_enabled;
        v.who_xor_key                   = &who_xor_key;
        v.colision1_bias                = &colision1_bias;
        v.colision2_bias                = &colision2_bias;
        v.teleport_death_bias           = &teleport_death_bias;
        v.door_open_hold_time_sec       = &door_open_hold_time_sec;
        // TACT1C batch (2026-08-26).
        v.hovered_unit_id               = &hovered_unit_id;
        v.sprite_meta_table             = sprite_meta_table.data();
        v.fx_splash_spare_nonzero_owner = &fx_splash_spare_nonzero_owner;
        // TACT1D batch (2026-08-27).
        v.gfx_bank_pixels    = &gfx_bank_pixels_base;
        v.sprite_pix_offsets = sprite_pix_offsets_data.data();
        v.fov_candidate_dist = &fov_candidate_dist;
        // TACT1E batch (2026-08-27).
        v.snd_enabled                  = &snd_enabled;
        v.snd_master_volume            = &snd_master_volume;
        v.snd_cfg_table                = snd_cfg_table_base();
        v.ambient_snd_min_interval_sec = &ambient_snd_min_interval_sec;
        v.ambient_snd_zone_count       = &ambient_snd_zone_count;
        v.ambient_snd_zone_table       = ambient_snd_zone_table.data();
        // TACT1E batch (2026-08-27).
        v.sel_panel_icon_count       = &sel_panel_icon_count;
        v.sel_panel_icon_names       = sel_panel_icon_names.data();
        v.cam_follow_target_col      = &cam_follow_target_col;
        v.cam_follow_target_row      = &cam_follow_target_row;
        v.cursor_x                   = &cursor_x;
        v.cursor_y                   = &cursor_y;
        v.mouse_buttons_cur          = &mouse_buttons_cur;
        v.win_w                      = &win_w;
        v.sidebar_icon_slot_unit_lut = sidebar_icon_slot_unit_lut.data();
        v.unassigned_unit_roster     = unassigned_unit_roster.data();
        v.group_unit_roster          = group_unit_roster.data();
        v.gfx_panel_row_skip         = &gfx_panel_row_skip;
        v.text_ptrs                  = text_ptrs.data();
        // TACT1E batch (2026-08-28).
        v.key_lctrl_held                   = &key_lctrl_held;
        v.key_lalt_held                    = &key_lalt_held;
        v.tact_ui_minimap_origin_x         = &tact_ui_minimap_origin_x;
        v.tact_ui_minimap_origin_y         = &tact_ui_minimap_origin_y;
        v.sidebar_multi_panel_visible_rows = &sidebar_multi_panel_visible_rows;
        v.sidebar_slot_visible_count       = &sidebar_slot_visible_count;
        v.sidebar_fmt_unit_id              = sidebar_fmt_unit_id_v;
        v.sidebar_fmt_unit_name            = sidebar_fmt_unit_name_v;
        return v;
    }
};

} // namespace mh::tact
