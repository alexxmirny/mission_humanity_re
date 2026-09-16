//
// tact_write_negative.cpp -- the checked-in proof that TACT0's W1-W3 hold (RI-TACT / TACT0).
//
// Driven by tools/check_const_view.py, target `tact`. Each case is compiled with its own macro and
// must FAIL with the diagnostic class its `// EXPECT` marker declares; then the file is compiled
// with NO macro and must SUCCEED. Both arms matter: a negative test that only asks "did the
// compiler say no?" is satisfied by a typo or a renamed field, every one of which looks exactly like
// the architecture rule holding while proving nothing about it.
//
// WHY THE EXPECTED CLASS IS PER CASE. W1 is const-ness and fails with the assign-through-const
// family; W2 and W3 are ACCESS CONTROL and fail with C2248. A case that fails for the other rule's
// reason is a broken test, and one wide accept-set would report it green.
//
// WHAT IS DELIBERATELY NOT HERE: a case asserting that a tactical write to `tile_objects` does not
// compile. TACT0's item text anticipated that clause -- "if excepted, a write attempt through the
// tactical store FAILS TO COMPILE" -- and the measurement retired it. The planes are the MAP's
// substrate with two mutually-exclusive mode tenants, not the sim's property being borrowed, so a
// tactical plane write is legal BY DESIGN and case 5 below asserts the opposite: that it compiles
// only through the shared binding, never by fabricating one. Refusing plane writes outright would
// have made 22 of the 99 migration members untranslatable.
//
#include <cstdint>

#include "state/mode_planes.h"
#include "tact/tact_state.h"

namespace {

// ---- W1: the read view is const, and its MEMBERSHIP is the claim -----------------------------
//
// The four strategic members are the load-bearing ones: `units`, `Unit` (cfg), `_G_LLM_STRAT_PLAYERS`
// and `G_PLANET_INDEX` each have a tactical READER and no tactical writer (measured across all 99
// members), so "a mission does not write strategic rosters" is a compile error rather than a
// convention.

#if defined(MH_TACT_NEGATIVE_CASE_1) // EXPECT const
// A mission writing a STRATEGIC unit. This is the one W1 exists for.
void case_1(mh::tact::tact_view &v) { v.strat_units[3].energy = 0.0; }
#endif

#if defined(MH_TACT_NEGATIVE_CASE_2) // EXPECT const
// A mission writing the frozen cfg TYPE table.
void case_2(mh::tact::tact_view &v) { v.cfg_units[1].energy = 0.0; }
#endif

#if defined(MH_TACT_NEGATIVE_CASE_3) // EXPECT const
// A mission writing a shared plane through the READ path. The write itself is legal tactical
// behaviour -- but not through the view, which is the read half. It must go through
// `own.planes()`, where it is greppable and countable.
void case_3(mh::tact::tact_view &v) { v.tile_objects[0].class_owner = 7; }
#endif

// ---- W2: the store hands out no address ------------------------------------------------------

#if defined(MH_TACT_NEGATIVE_CASE_4) // EXPECT access
// Reaching for the roster base to do pointer arithmetic past a record -- the exact move that goes
// stale the moment a region is rebased, and the reason accessors return ONE record by reference.
void case_4(mh::tact::tact_store &own) {
    mh::tact::tact_unit *base = own.units_;
    (void)base;
}
#endif

// ---- W3: neither the store nor the shared planes can be constructed outside their module ------

#if defined(MH_TACT_NEGATIVE_CASE_5) // EXPECT access
// Forging a `mode_planes` over buffers of one's own. If this compiled, every guarantee about the
// shared substrate would be one struct literal away -- a tactical TU could bind the planes to
// anything and the ownership ledger would describe a fiction.
void case_5() {
    static mh::state::tile_object   tiles[4];
    static uint8_t                  pass[4];
    static mh::state::path_waypoint wp[4];
    static uint8_t                  flags[4];
    static int32_t                  freecnt[4];
    mh::state::mode_planes          forged(tiles, pass, wp, flags, freecnt);
    (void)forged;
}
#endif

#if defined(MH_TACT_NEGATIVE_CASE_6) // EXPECT access
// Forging a `tact_store`. W3's own case: the friend list is the rule, and a third binder has to be
// written into it rather than constructed on the spot.
void case_6(mh::state::mode_planes planes) {
    static mh::tact::tact_unit                units[4];
    static int32_t                            mx, my;
    static mh::tact::character_type           char_types[mh::tact::TACT_CHARACTER_TYPE_SLOTS];
    static int32_t                            map_w, map_h;
    static mh::game::mh_llm_squad_status_slot squad_status[mh::tact::TACT_SQUAD_STATUS_SLOTS];
    static int32_t                            occupancy_rebuild_map_id;
    static int32_t                            move_path_cache_valid;
    static int32_t                            move_path_slot_id;
    static mh::tact::teleport_zone            teleport_zones[mh::tact::TACT_TELEPORT_ZONE_SLOTS];
    static uint8_t                            flood_start_col, flood_start_row;
    static uint8_t                            flood_goal_col, flood_goal_row;
    static uint8_t                            flood_result_col, flood_result_row;
    // THE ARGUMENT LIST HAS TO STAY CURRENT WITH THE PRIVATE CONSTRUCTOR. A stale list still fails
    // to compile, but with C2661 "no overloaded function takes N arguments" instead of C2248
    // "cannot access private member" -- and a test that fails for the wrong reason has stopped
    // testing the rule it names. check_const_view.py asserts the DIAGNOSTIC CLASS, not just that
    // the compile failed, which is what caught this when TACT1A added the character-type binding
    // (and again when it added map_width/map_height/squad_status, 2026-08-26, and again when it
    // added occupancy_rebuild_map_id the same day, and again when TACT1B added
    // move_path_cache_valid/move_path_slot_id, and again when a later slice added teleport_zones and
    // the flood_start/goal/result pair, and again when llm_tact_move_path_build's own private
    // solver scratch was added, and again when llm_tact_unit_despawn added unit_active_count, and
    // again when llm_tact_unit_mine_arm_tick/llm_tact_mission_end_return_to_strategic added
    // mine_blast_time_end/blast_marker_col/blast_marker_row/game_mode/planet_status, and again when
    // the TACT1A/TACT1C central batch added mission_start's write set + the fx-pool write path).
    static uint32_t              move_path_coord_mask, move_path_goal_packed, move_path_start_packed;
    static uint32_t              move_path_tile_cost, move_path_trace_tile, move_path_trace_base;
    static uint32_t              move_path_trace_diag_cand, move_path_trace_best_tile, move_path_trace_cost_sum;
    static uint32_t              move_path_rle_count, move_path_queue_cur_addr;
    static uint8_t               move_path_trace_best_dir;
    static uint16_t              move_path_queue_a[4096], move_path_queue_b[4096];
    static uint32_t              move_path_cost_map[0x10000];
    static uint16_t              tile_height_sprites_base;
    static uint16_t             *tile_height_sprites_ptr = &tile_height_sprites_base;
    static int32_t               unit_active_count;
    static double                mine_blast_time_end;
    static int32_t               blast_marker_col, blast_marker_row;
    static uint8_t               game_mode;
    static int32_t               planet_status[32];
    static int32_t               bank_sprite_base[47];
    static int32_t               window_w, window_h, saved_window_w, view_tile_height_px;
    static int32_t               map_cam_col, map_cam_row, grid_w, grid_h, view_tiles_w, view_tiles_h;
    static double                cam_col_f, cam_row_f;
    static int32_t               click_action_taken, scroll_cmd;
    static uint8_t               framebuffer_byte;
    static uint8_t              *framebuffer_ptr = &framebuffer_byte;
    static int32_t               move_cur_col, move_cur_row;
    static mh::tact::fx_entry    fx_pool_arr[1];
    static int32_t               fx_live_count;
    static mh::tact::fx_type     fx_type_arr[1];
    static mh::tact::door_record door_arr[1];
    static int32_t               squad_size_w, enemy_count, see_enemy_flag;
    static int32_t               mine_first_frame, mine_frame_count, mine_sound_id;
    static double                mine_duration;
    static int32_t               quit_col, quit_row, target_col, target_row;
    static void                 *panel_gfx[16];
    static void                 *file_ptr_v;
    // TACT1D batch (2026-08-27).
    static int32_t fov_col_w, fov_row_w, fov_angle_base_w, fov_angle_width_w, fov_dist_w;
    static uint8_t fov_stencil_arr[64 * 64];
    static int16_t fov_nearest_hibit_cell_w, fov_nearest_low_cell_w;
    static int32_t fov_nearest_hibit_dist_w, fov_nearest_low_dist_w;
    static uint8_t tile_vis_map_arr[300];
    // TACT1E batch (2026-08-27): added snd_channel0_retrigger_time, then the frame pump + selection/
    // sidebar/panel residue's write set below.
    static double   snd_channel0_retrigger_time_w;
    static int32_t  cam_scroll_up_w, cam_scroll_down_w, cam_scroll_left_w, cam_scroll_right_w;
    static int32_t  exit_confirm_w, click_scan_scratch_w;
    static int32_t  drag_anchor_x_w, drag_anchor_y_w, drag_select_active_w;
    static int32_t  hovered_unit_w, active_unit_count_w, active_unit_count_cached_w;
    static uint8_t  tile_drawn_map_arr[300];
    static int32_t  cam_follow_selection_w;
    static void    *sel_panel_icon_gfx_arr[41];
    static int32_t  ui_sel_panel_multi_mode_w;
    static int32_t  sel_panel_icon_slot_state_arr[4];
    static int32_t  sidebar_active_group_id_w, sidebar_slot_scroll_w;
    static int32_t  sidebar_scrollbtn_state_arr[8];
    static int32_t  sidebar_ui_hit_code_w, sidebar_highlighted_unit_id_w;
    static int32_t  sidebar_slot_unit_ids_arr[64];
    static int32_t  squad_bb_target_energy_pct_w;
    static uint8_t  tile_vis_map_ptr_byte, los_cache_ptr_byte;
    static uint8_t *tile_vis_map_ptr_w = &tile_vis_map_ptr_byte;
    static uint8_t *los_cache_ptr_w    = &los_cache_ptr_byte;
    // TACT1E batch (2026-08-28).
    static int32_t       sidebar_unassigned_scroll_row_w, sidebar_group_scroll_row_w;
    static uint8_t       unassigned_unit_roster_arr[256], group_unit_roster_arr[2048];
    static uint8_t       mouse_buttons_cur_w;
    static wchar_t       text_scratch_arr[256];
    mh::tact::tact_store forged(
        units, &mx, &my, char_types, &map_w, &map_h, &tile_height_sprites_ptr, squad_status, planes,
        &occupancy_rebuild_map_id, &move_path_cache_valid, &move_path_slot_id, teleport_zones,
        &flood_start_col, &flood_start_row, &flood_goal_col, &flood_goal_row, &flood_result_col,
        &flood_result_row, &move_path_coord_mask, &move_path_goal_packed, &move_path_start_packed,
        &move_path_tile_cost, &move_path_trace_tile, &move_path_trace_base,
        &move_path_trace_diag_cand, &move_path_trace_best_dir, &move_path_trace_best_tile,
        &move_path_trace_cost_sum, &move_path_rle_count, &move_path_queue_cur_addr,
        move_path_queue_a, move_path_queue_b, move_path_cost_map, &unit_active_count,
        &mine_blast_time_end, &blast_marker_col, &blast_marker_row, &game_mode, planet_status,
        bank_sprite_base, &window_w, &window_h, &saved_window_w, &view_tile_height_px, &map_cam_col,
        &map_cam_row, &grid_w, &grid_h, &view_tiles_w, &view_tiles_h, &cam_col_f, &cam_row_f,
        &click_action_taken, &scroll_cmd, &framebuffer_ptr, &move_cur_col, &move_cur_row,
        fx_pool_arr, &fx_live_count, fx_type_arr, door_arr, &squad_size_w, &enemy_count,
        &see_enemy_flag, &mine_first_frame, &mine_frame_count, &mine_duration, &mine_sound_id,
        &quit_col, &quit_row, &target_col, &target_row, panel_gfx, &file_ptr_v, &fov_col_w,
        &fov_row_w, &fov_angle_base_w, &fov_angle_width_w, &fov_dist_w, fov_stencil_arr,
        &fov_nearest_hibit_cell_w, &fov_nearest_low_cell_w, &fov_nearest_hibit_dist_w,
        &fov_nearest_low_dist_w, tile_vis_map_arr, &snd_channel0_retrigger_time_w,
        &cam_scroll_up_w, &cam_scroll_down_w, &cam_scroll_left_w, &cam_scroll_right_w,
        &exit_confirm_w, &click_scan_scratch_w, &drag_anchor_x_w, &drag_anchor_y_w,
        &drag_select_active_w, &hovered_unit_w, &active_unit_count_w,
        &active_unit_count_cached_w, tile_drawn_map_arr, &cam_follow_selection_w,
        sel_panel_icon_gfx_arr, &ui_sel_panel_multi_mode_w, sel_panel_icon_slot_state_arr,
        &sidebar_active_group_id_w, &sidebar_slot_scroll_w, sidebar_scrollbtn_state_arr,
        &sidebar_ui_hit_code_w, &sidebar_highlighted_unit_id_w, sidebar_slot_unit_ids_arr,
        &squad_bb_target_energy_pct_w, &tile_vis_map_ptr_w, &los_cache_ptr_w,
        &sidebar_unassigned_scroll_row_w, &sidebar_group_scroll_row_w, unassigned_unit_roster_arr,
        group_unit_roster_arr, &mouse_buttons_cur_w, text_scratch_arr);
    (void)forged;
}
#endif

} // namespace

// ---- the POSITIVE arm ---------------------------------------------------------------------------
//
// Compiled with NO case macro and REQUIRED to succeed. If this broke, every negative case above
// would keep "passing" for entirely the wrong reason.
//
// It is also the readable statement of what the interface DOES allow, so it deliberately exercises
// all three legal paths: a const read through the view, a write to tactical's own arena, and a write
// to a shared plane through the one binding both modes use.
namespace {

int32_t positive_read(const mh::tact::tact_view &v) {
    // W1's read path: strategic state is READABLE by a mission, just not writable.
    return (int32_t)v.strat_units[0].energy + *v.planet_index + (int32_t)v.cfg_units[0].energy +
           (int32_t)mh::tact::tile_at(v, 1, 2).class_owner + mh::tact::passable_at(v, 1, 2);
}

void positive_write(mh::tact::tact_store &own) {
    // W2's accessor: one record by reference, no base.
    own.unit_at(1).hp     = 0;
    own.sidebar_mouse_x() = 4;

    // The shared substrate, through the binding both modes share. LEGAL, and the whole point of
    // TACT0: this is a tactical write to a Map/geometry-owned plane, and it is expressible.
    mh::state::mode_planes &p           = own.planes();
    p.tile_object_at(3, 4).class_owner  = 1;
    p.passable_at(3, 4)                 = mh::state::PASSABLE_BLOCKED;
    p.path_waypoint_at(0, 1, 2).heading = 5;
    p.path_slot_flag_at(0, 1)           = 1;
    p.path_free_slot_count_at(0)        = 99;
    mh::state::tile_overlay(p, 3, 4)    = 0;
}

} // namespace

extern "C" int tact_write_negative_positive_arm(void);
extern "C" int tact_write_negative_positive_arm(void) {
    mh::tact::tact_state st = mh::tact::state();
    positive_write(st.own);
    return positive_read(st.read);
}
