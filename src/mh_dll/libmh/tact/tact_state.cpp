//
// tact/tact_state.cpp -- binding the tactical state interface to the live game (RI-TACT / TACT0),
// plus the domain's shadow-arm install point (the conductor-owned aggregator; the per-site
// installers live in their own TUs).
//
// The binding names no logic and the logic names no address: addresses come from the region
// registry here; the translations in the other libmh/tact/ TUs never see one.
//
// THIS IS THE ONLY FILE UNDER libmh/tact/ ALLOWED TO NAME mh::state::ptr. tools/check_sim_addresses.py
// (in lint_repo) enforces that for libmh/sim/ and, since TACT0, for libmh/tact/ too -- see tact_state.h's
// W2. If a translation needs a region, the answer is a new member here plus an accessor, not a
// `ptr<>` at the use site.
//
#include "tact/tact_state.h"

#include "tact/tact_dir24_approach.h"
#include "tact/tact_ambient_sound.h"
#include "tact/tact_door.h"
#include "tact/tact_fov.h"
#include "tact/tact_fx_spawn.h"
#include "tact/tact_fx_splash_damage.h"
#include "tact/tact_fx_update_projectile.h"
#include "tact/tact_group_issue_order.h"
#include "tact/tact_map_bounds.h"
#include "tact/tact_map_reset.h"
#include "tact/tact_mission_end_return_to_strategic.h"
#include "tact/tact_mission_load.h"
#include "tact/tact_mission_start.h"
#include "tact/tact_move_path_build.h"
#include "tact/tact_move_path_preview_walk.h"
#include "tact/tact_move_step_attempt.h"
#include "tact/tact_pilot.h"
#include "tact/tact_squad_assault_resolve.h"
#include "tact/tact_squad_status.h"
#include "tact/tact_teleport_cmdqueue_jump.h"
#include "tact/tact_teleport_zone.h"
#include "tact/tact_tile_occupancy.h"
#include "tact/tact_unit_cmd_advance.h"
#include "tact/tact_unit_cmd_advance_with_defstat.h"
#include "tact/tact_unit_cmd_queue_advance.h"
#include "tact/tact_unit_cmd_queue_resubmit_run.h"
#include "tact/tact_unit_cmd_stance.h"
#include "tact/tact_unit_cmd_teleport_jump_tick.h"
#include "tact/tact_unit_death_tick.h"
#include "tact/tact_unit_despawn.h"
#include "tact/tact_unit_destroy.h"
#include "tact/tact_unit_enqueue_command.h"
#include "tact/tact_unit_fire_weapon.h"
#include "tact/tact_unit_kneel_tick.h"
#include "tact/tact_unit_mine_arm_tick.h"
#include "tact/tact_unit_move_advance.h"
#include "tact/tact_unit_move_tick.h"
#include "tact/tact_unit_owner_tick.h"
#include "tact/tact_unit_rotate_step.h"
#include "tact/tact_unit_rotate_tick.h"
#include "tact/tact_unit_set_anim_state.h"
#include "tact/tact_unit_spawn.h"
#include "tact/tact_unit_stand_tick.h"
#include "tact/tact_unit_teleport.h"
#include "tact/tact_unit_update_anim.h"
#include "tact/tact_unit_vision.h"
#include "tact/tact_unit_weapon_in_range.h"
#include "tact/tact_unit_weapons_tick.h"
#include "tact/tact_ui_sel_panel_init.h"
#include "tact/tact_update_units_and_fx.h"
#include "tact/tact_cam_follow_selection_tick.h"
#include "tact/tact_select_next_unit.h"
#include "tact/tact_sidebar_dispatch.h"
#include "tact/tact_scroll_target_proximity_tick.h"
#include "tact/tact_view_shift.h"
#include "tact/tact_frame.h"
#include "tact/tact_ui_sel_panel_multi_mode_tick.h"
#include "tact/tact_ui_sel_panel_single_mode_tick.h"
#include "tact/tact_camera_center_on_tile.h"
#include "tact/tact_selection_clear_unless_ctrl.h"
#include "tact/tact_active_unit_count_hud_draw.h"
#include "tact/tact_squad_roster_refresh.h"
#include "tact/tact_selection_panel_refresh.h"
#include "tact/tact_ui_order_buttons_minimap_tick.h"

namespace mh::tact {

using namespace mh::state;

tact_state state() {
    tact_view v{};
    // ST2/Law 3: live_base, not the constexpr .bss address. Re-resolved per call -- nothing here is
    // cached and nothing is static, which is what lets `tacttest`'s rebase arm mean anything.
    v.units                             = ptr<const tact_unit>(RID_TACT_UNITS);
    v.map_width                         = ptr<const int32_t>(RID_TACT_MAP_WIDTH_CACHE);
    v.map_height                        = ptr<const int32_t>(RID_TACT_MAP_HEIGHT_CACHE);
    v.sidebar_mouse_x                   = ptr<const int32_t>(RID_TACT_SIDEBAR_MOUSE_X);
    v.sidebar_mouse_y                   = ptr<const int32_t>(RID_TACT_SIDEBAR_MOUSE_Y);
    v.map_tile_height_sprites           = ptr<const uint16_t *>(RID_TACT_MAP_TILE_HEIGHT_SPRITES);
    v.squad_size                        = ptr<const int32_t>(RID_TACT_SQUAD_SIZE);
    v.move_path_slot_id                 = ptr<const int32_t>(RID_TACT_MOVE_PATH_SLOT_ID);
    v.dir24_approach_delta              = ptr<const dir24_delta>(RID_TACT_DIR24_APPROACH_DELTA);
    v.dir8_delta_table                  = ptr<const dir8_delta>(RID_TACT_DIR8_DELTA_TABLE);
    v.fov_delta_table_72                = ptr<const int8_t>(RID_TACT_FOV_DELTA_TABLE_72);
    v.fov_delta_table_120               = ptr<const int8_t>(RID_TACT_FOV_DELTA_TABLE_120);
    v.fov_delta_table_360               = ptr<const int8_t>(RID_TACT_FOV_DELTA_TABLE_360);
    v.unit_wander_retry_interval        = ptr<const double>(RID_TACT_UNIT_WANDER_RETRY_INTERVAL);
    v.move_cur_col                      = ptr<const int32_t>(RID_TACT_MOVE_CUR_COL);
    v.move_cur_row                      = ptr<const int32_t>(RID_TACT_MOVE_CUR_ROW);
    v.move_flood_result_col             = ptr<const uint8_t>(RID_TACT_MOVE_FLOOD_RESULT_COL);
    v.move_flood_result_row             = ptr<const uint8_t>(RID_TACT_MOVE_FLOOD_RESULT_ROW);
    v.mine_blast_time_end               = ptr<const double>(RID_TACT_MINE_BLAST_TIME_END);
    v.mine_blast_duration               = ptr<const double>(RID_TACT_MINE_BLAST_DURATION);
    v.view_size_mode                    = ptr<const int32_t>(RID_VIEW_SIZE_MODE);
    v.last_game_time                    = ptr<const double>(RID_LAST_GAME_TIME);
    v.move_flood_start_col              = ptr<const uint8_t>(RID_TACT_MOVE_FLOOD_START_COL);
    v.move_flood_start_row              = ptr<const uint8_t>(RID_TACT_MOVE_FLOOD_START_ROW);
    v.move_flood_goal_col               = ptr<const uint8_t>(RID_TACT_MOVE_FLOOD_GOAL_COL);
    v.move_flood_goal_row               = ptr<const uint8_t>(RID_TACT_MOVE_FLOOD_GOAL_ROW);
    v.grid_width                        = ptr<const int32_t>(RID_WIDTH);
    v.grid_height                       = ptr<const int32_t>(RID_HEIGHT);
    v.squad_bb_scan_player              = ptr<const int32_t>(RID_SQUAD_BB_SCAN_PLAYER);
    v.squad_bb_target_owner             = ptr<const int32_t>(RID_SQUAD_BB_TARGET_OWNER);
    v.squad_bb_target_building_idx      = ptr<const int32_t>(RID_SQUAD_BB_TARGET_BUILDING_IDX);
    v.squad_bb_target_energy_pct        = ptr<const int32_t>(RID_SQUAD_BB_TARGET_ENERGY_PCT);
    v.squad_assault_power_percent_scale = ptr<const double>(RID_STRAT_SQUAD_ASSAULT_POWER_PERCENT_SCALE);
    v.bldg_energy_to_percent_scale      = ptr<const double>(RID_STRAT_BLDG_ENERGY_TO_PERCENT_SCALE);
    v.bldg_energy_percent_to_abs_scale  = ptr<const double>(RID_STRAT_BLDG_ENERGY_PERCENT_TO_ABS_SCALE);

    // W1's load-bearing members: strategic state a mission reads and must never write.
    v.strat_units   = ptr<const strat_unit>(RID_UNITS);
    v.cfg_units     = ptr<const cfg_unit>(RID_UNIT);
    v.strat_players = ptr<const player_profile>(RID_STRAT_PLAYERS);
    v.planet_index  = ptr<const int32_t>(RID_G_PLANET_INDEX);

    // TACT1A/TACT1C central batch (2026-08-26).
    v.current_system                = ptr<const int32_t>(RID_CURRENTSYSTEM);
    v.gfx_draw_surface              = ptr<const uint8_t *>(RID_GFX_DRAW_SURFACE);
    v.key_rshift_held               = ptr<const uint8_t>(RID_KEY_RSHIFT_HELD);
    v.key_lshift_held               = ptr<const uint8_t>(RID_KEY_LSHIFT_HELD);
    v.unit_anim_step_sec_1          = ptr<const double>(RID_TACT_UNIT_ANIM_STEP_SEC_1);
    v.unit_anim_jitter_scale_1      = ptr<const double>(RID_TACT_UNIT_ANIM_JITTER_SCALE_1);
    v.unit_anim_step_sec_2          = ptr<const double>(RID_TACT_UNIT_ANIM_STEP_SEC_2);
    v.unit_anim_jitter_scale_2      = ptr<const double>(RID_TACT_UNIT_ANIM_JITTER_SCALE_2);
    v.unit_anim_frame_interval_base = ptr<const double>(RID_TACT_UNIT_ANIM_FRAME_INTERVAL_BASE);
    v.mines_enabled                 = ptr<const int32_t>(RID_TACT_MINES_ENABLED);
    v.who_xor_key                   = ptr<const int32_t>(RID_TACT_WHO_XOR_KEY);
    v.colision1_bias                = ptr<const double>(RID_TACT_COLISION1_BIAS);
    v.colision2_bias                = ptr<const double>(RID_TACT_COLISION2_BIAS);
    v.teleport_death_bias           = ptr<const double>(RID_TACT_TELEPORT_DEATH_BIAS);
    v.door_open_hold_time_sec       = ptr<const double>(RID_TACT_DOOR_OPEN_HOLD_TIME_SEC);

    // TACT1C batch (2026-08-26): unit_fire_weapon / fx_splash_damage / unit_get_muzzle_offset.
    v.hovered_unit_id               = ptr<const int32_t>(RID_TACT_HOVERED_UNIT_ID);
    v.sprite_meta_table             = ptr<const sprite_meta>(RID_SPRITE_META);
    v.fx_splash_spare_nonzero_owner = ptr<const int32_t>(RID_TACT_FX_SPLASH_SPARE_NONZERO_OWNER);

    // TACT1D batch (2026-08-27). Same two bindings sim_view already uses (sim_state.cpp).
    v.gfx_bank_pixels    = ptr<uint8_t *const>(RID_GFX_BANK_PIXELS);
    v.sprite_pix_offsets = ptr<const uint32_t>(RID_SPRITE_PIX_OFFSETS);
    v.fov_candidate_dist = ptr<const int32_t>(RID_TACT_FOV_CANDIDATE_DIST);

    // TACT1E (2026-08-27): llm_tact_ambient_sound_tick's /sound-subsystem reads. Raw literal
    // addresses -- see tact_view's own comment on these six members -- not RIDs; the first
    // tactical translation to touch this state, and it is read-only for all but the retrigger-time
    // channel bound separately (tact_store::snd_channel0_retrigger_time, via the existing
    // RID_SND_CHANNEL_NEXT_RETRIGGER_TIME).
    // LIB-REF-SPLIT (2026-09-11): snd_enabled is bound THROUGH THE REGISTRY, unlike its five
    // siblings below. It was the last live original-image VA in the standalone artifact, and it was
    // the only one of the six the byte scan could even see -- the other five are addresses nothing
    // NAMES, so they are absent from mh_addrs.gen.h and therefore from the scan's candidate set.
    // That invisibility is the argument for converting them too, not for leaving them; the work is
    // filed as SND-LITERALS, because they have no registry entry yet and the ambient-sound tick
    // needs an is-it-hashed measurement first.
    //
    // HOSTED BEHAVIOUR CANNOT MOVE, and it is pinned rather than asserted in prose: the registry
    // already carries `static_assert(REGIONS[RID_SND_ENABLED].base == mh::addr::_G_LLM_SND_ENABLED)`
    // (addr/mh_regions.gen.h), and hosted `ptr<T>(rid)` reads live_base(rid), which is seeded from
    // that same base and is only ever changed by a bind to a different address. So this yields the
    // identical pointer the literal did -- the local assert below says so at the site, in the arm
    // where the stock column exists. Standalone it becomes host-bindable like every other region,
    // which is the whole difference.
#ifndef MH_LIBMH_BUILD
    static_assert(base_of(RID_SND_ENABLED) == 0x00ae2ab0u,
                  "RID_SND_ENABLED no longer resolves to the literal this binding replaced");
#endif
    v.snd_enabled                  = ptr<const int32_t>(RID_SND_ENABLED);
    v.snd_master_volume            = reinterpret_cast<const int32_t *>(0x00ae2ab4u);
    v.snd_cfg_table                = reinterpret_cast<const snd_cfg_entry *>(0x0070d5c0u);
    v.ambient_snd_min_interval_sec = reinterpret_cast<const double *>(0x0050046au);
    v.ambient_snd_zone_count       = reinterpret_cast<const int32_t *>(0x00558cacu);
    v.ambient_snd_zone_table       = reinterpret_cast<const int32_t *>(0x00558cb0u);

    // TACT1E (2026-08-27): the frame pump + selection/sidebar/panel residue.
    v.sel_panel_icon_count       = ptr<const int32_t>(RID_TACT_SEL_PANEL_ICON_COUNT);
    v.sel_panel_icon_names       = ptr<const uint8_t>(RID_TACT_SEL_PANEL_ICON_NAMES);
    v.cam_follow_target_col      = ptr<const int32_t>(RID_TACT_CAM_FOLLOW_TARGET_COL);
    v.cam_follow_target_row      = ptr<const int32_t>(RID_TACT_CAM_FOLLOW_TARGET_ROW);
    v.cursor_x                   = ptr<const int32_t>(RID_CURSOR_X);
    v.cursor_y                   = ptr<const int32_t>(RID_CURSOR_Y);
    v.mouse_buttons_cur          = ptr<const uint8_t>(RID_MOUSE_BUTTONS_CUR);
    v.win_w                      = ptr<const int32_t>(RID_G_WIN_W);
    v.sidebar_icon_slot_unit_lut = ptr<const uint8_t>(RID_TACT_SIDEBAR_ICON_SLOT_UNIT_LUT);
    v.unassigned_unit_roster     = ptr<const uint8_t>(RID_TACT_UNASSIGNED_UNIT_ROSTER);
    v.group_unit_roster          = ptr<const uint8_t>(RID_TACT_GROUP_UNIT_ROSTER);
    v.gfx_panel_row_skip         = ptr<const int32_t>(RID_GFX_PANEL_ROW_SKIP);
    v.text_ptrs                  = ptr<const wchar_t *const>(RID_G_TEXT_PTRS);

    // TACT1E batch (2026-08-28): the remaining frame-pump/sidebar residue.
    v.key_lctrl_held                   = ptr<const uint8_t>(RID_KEY_LCTRL_HELD);
    v.key_lalt_held                    = ptr<const uint8_t>(RID_KEY_LALT_HELD);
    v.tact_ui_minimap_origin_x         = ptr<const int32_t>(RID_TACT_UI_MINIMAP_ORIGIN_X);
    v.tact_ui_minimap_origin_y         = ptr<const int32_t>(RID_TACT_UI_MINIMAP_ORIGIN_Y);
    v.sidebar_multi_panel_visible_rows = ptr<const int32_t>(RID_TACT_SIDEBAR_MULTI_PANEL_VISIBLE_ROWS);
    v.sidebar_slot_visible_count       = ptr<const int32_t>(RID_TACT_SIDEBAR_SLOT_VISIBLE_COUNT);
    v.sidebar_fmt_unit_id              = ptr<const wchar_t>(RID_TACT_SIDEBAR_FMT_UNIT_ID);
    v.sidebar_fmt_unit_name            = ptr<const wchar_t>(RID_TACT_SIDEBAR_FMT_UNIT_NAME);

    // The mission's unit CLASS table: read path (the parsers' write path is tact_store's).
    v.character_types = ptr<const character_type>(RID_TACT_CHARACTER_TYPES);

    // Mission-loaded gun/explosion definitions. No tactical writer.
    v.fx_type_table = ptr<const fx_type>(RID_TACT_FX_TYPE_TABLE);

    // The shared planes, read path.
    v.tile_objects = ptr<const tile_object>(RID_TILE_OBJECTS);
    v.passable     = ptr<const uint8_t>(RID_PASSABLE);

    v.fx_pool = ptr<const fx_entry>(RID_TACT_FX_POOL);

    // The shared planes, write path -- ONE binding, the same five regions sim_store binds.
    mode_planes planes(ptr<tile_object>(RID_TILE_OBJECTS), ptr<uint8_t>(RID_PASSABLE),
                       ptr<path_waypoint>(RID_STRAT_PATH_BUFFERS),
                       ptr<uint8_t>(RID_STRAT_PATH_SLOT_FLAGS),
                       ptr<int32_t>(RID_STRAT_PATH_FREE_SLOT_COUNT));

    tact_store own(ptr<tact_unit>(RID_TACT_UNITS), ptr<int32_t>(RID_TACT_SIDEBAR_MOUSE_X),
                   ptr<int32_t>(RID_TACT_SIDEBAR_MOUSE_Y),
                   ptr<character_type>(RID_TACT_CHARACTER_TYPES), ptr<int32_t>(RID_TACT_MAP_WIDTH_CACHE),
                   ptr<int32_t>(RID_TACT_MAP_HEIGHT_CACHE),
                   ptr<uint16_t *>(RID_TACT_MAP_TILE_HEIGHT_SPRITES),
                   ptr<mh::game::mh_llm_squad_status_slot>(RID_SQUAD_STATUS), planes,
                   ptr<int32_t>(RID_TACT_OCCUPANCY_REBUILD_MAP_ID),
                   ptr<int32_t>(RID_TACT_MOVE_PATH_CACHE_VALID),
                   ptr<int32_t>(RID_TACT_MOVE_PATH_SLOT_ID),
                   ptr<teleport_zone>(RID_TACT_TELEPORT_TABLE),
                   ptr<uint8_t>(RID_TACT_MOVE_FLOOD_START_COL),
                   ptr<uint8_t>(RID_TACT_MOVE_FLOOD_START_ROW),
                   ptr<uint8_t>(RID_TACT_MOVE_FLOOD_GOAL_COL),
                   ptr<uint8_t>(RID_TACT_MOVE_FLOOD_GOAL_ROW),
                   ptr<uint8_t>(RID_TACT_MOVE_FLOOD_RESULT_COL),
                   ptr<uint8_t>(RID_TACT_MOVE_FLOOD_RESULT_ROW),
                   ptr<uint32_t>(RID_TACT_MOVE_PATH_COORD_MASK),
                   ptr<uint32_t>(RID_TACT_MOVE_PATH_GOAL_PACKED),
                   ptr<uint32_t>(RID_TACT_MOVE_PATH_START_PACKED),
                   ptr<uint32_t>(RID_TACT_MOVE_PATH_TILE_COST),
                   ptr<uint32_t>(RID_TACT_MOVE_PATH_TRACE_TILE),
                   ptr<uint32_t>(RID_TACT_MOVE_PATH_TRACE_BASE),
                   ptr<uint32_t>(RID_TACT_MOVE_PATH_TRACE_DIAG_CAND),
                   ptr<uint8_t>(RID_TACT_MOVE_PATH_TRACE_BEST_DIR),
                   ptr<uint32_t>(RID_TACT_MOVE_PATH_TRACE_BEST_TILE),
                   ptr<uint32_t>(RID_TACT_MOVE_PATH_TRACE_COST_SUM),
                   ptr<uint32_t>(RID_TACT_MOVE_PATH_RLE_COUNT),
                   ptr<uint32_t>(RID_TACT_MOVE_PATH_QUEUE_CUR),
                   ptr<uint16_t>(RID_TACT_MOVE_PATH_QUEUE_A),
                   ptr<uint16_t>(RID_TACT_MOVE_PATH_QUEUE_B),
                   ptr<uint32_t>(RID_TACT_MOVE_COST_MAP),
                   ptr<int32_t>(RID_TACT_UNIT_ACTIVE_COUNT),
                   ptr<double>(RID_TACT_MINE_BLAST_TIME_END), ptr<int32_t>(RID_TACT_BLAST_MARKER_COL),
                   ptr<int32_t>(RID_TACT_BLAST_MARKER_ROW), ptr<uint8_t>(RID_GAME_MODE),
                   ptr<int32_t>(RID_G_PLANET_STATUS),
                   // TACT1A/TACT1C central batch (2026-08-26).
                   ptr<int32_t>(RID_BANK_SPRITE_BASE), ptr<int32_t>(RID_WINDOWWIDTH),
                   ptr<int32_t>(RID_WINDOWHEIGHT), ptr<int32_t>(RID_TACT_SAVED_WINDOW_WIDTH),
                   ptr<int32_t>(RID_GFX_VIEW_TILE_HEIGHT_PX), ptr<int32_t>(RID_MAP_CAM_COL),
                   ptr<int32_t>(RID_MAP_CAM_ROW), ptr<int32_t>(RID_WIDTH),
                   ptr<int32_t>(RID_HEIGHT), ptr<int32_t>(RID_VIEW_TILES_W),
                   ptr<int32_t>(RID_VIEW_TILES_H), ptr<double>(RID_TACT_CAM_COL_F),
                   ptr<double>(RID_TACT_CAM_ROW_F), ptr<int32_t>(RID_TACT_CLICK_ACTION_TAKEN),
                   ptr<int32_t>(RID_TACT_SCROLL_CMD), ptr<uint8_t *>(RID_FRAMEBUFFER),
                   ptr<int32_t>(RID_TACT_MOVE_CUR_COL), ptr<int32_t>(RID_TACT_MOVE_CUR_ROW),
                   ptr<fx_entry>(RID_TACT_FX_POOL), ptr<int32_t>(RID_TACT_FX_LIVE_COUNT),
                   // llm_tact_mission_load's write set (2026-08-26).
                   ptr<fx_type>(RID_TACT_FX_TYPE_TABLE), ptr<door_record>(RID_TACT_DOOR_TABLE),
                   ptr<int32_t>(RID_TACT_SQUAD_SIZE), ptr<int32_t>(RID_TACT_ENEMY_COUNT),
                   ptr<int32_t>(RID_TACT_SEE_ENEMY_FLAG),
                   ptr<int32_t>(RID_TACT_MINE_BLAST_FIRST_FRAME),
                   ptr<int32_t>(RID_TACT_MINE_BLAST_FRAME_COUNT),
                   ptr<double>(RID_TACT_MINE_BLAST_DURATION),
                   ptr<int32_t>(RID_TACT_MINE_BLAST_SOUND_ID),
                   ptr<int32_t>(RID_TACT_QUIT_TILE_COL), ptr<int32_t>(RID_TACT_QUIT_TILE_ROW),
                   ptr<int32_t>(RID_TACT_TARGET_TILE_COL), ptr<int32_t>(RID_TACT_TARGET_TILE_ROW),
                   ptr<void *>(RID_TACT_CHAR_PANEL_GFX), ptr<void *>(RID_FILE_PTR),
                   // TACT1D batch (2026-08-27): the FOV raycaster's cross-function scratch.
                   ptr<int32_t>(RID_TACT_FOV_COL), ptr<int32_t>(RID_TACT_FOV_ROW),
                   ptr<int32_t>(RID_TACT_FOV_ANGLE_BASE), ptr<int32_t>(RID_TACT_FOV_ANGLE_WIDTH),
                   ptr<int32_t>(RID_TACT_FOV_DIST), ptr<uint8_t>(RID_TACT_FOV_STENCIL),
                   ptr<int16_t>(RID_TACT_FOV_NEAREST_HIBIT_CELL),
                   ptr<int16_t>(RID_TACT_FOV_NEAREST_LOW_CELL),
                   ptr<int32_t>(RID_TACT_FOV_NEAREST_HIBIT_DIST),
                   ptr<int32_t>(RID_TACT_FOV_NEAREST_LOW_DIST), ptr<uint8_t>(RID_TILE_VIS_MAP),
                   ptr<double>(RID_SND_CHANNEL_NEXT_RETRIGGER_TIME),
                   // TACT1E batch (2026-08-27).
                   ptr<int32_t>(RID_TACT_CAM_SCROLL_UP_HELD),
                   ptr<int32_t>(RID_TACT_CAM_SCROLL_DOWN_HELD),
                   ptr<int32_t>(RID_TACT_CAM_SCROLL_LEFT_HELD),
                   ptr<int32_t>(RID_TACT_CAM_SCROLL_RIGHT_HELD),
                   ptr<int32_t>(RID_TACT_EXIT_CONFIRM_OPEN), ptr<int32_t>(RID_TACT_CLICK_SCAN_SCRATCH),
                   ptr<int32_t>(RID_TACT_DRAG_ANCHOR_X), ptr<int32_t>(RID_TACT_DRAG_ANCHOR_Y),
                   ptr<int32_t>(RID_TACT_DRAG_SELECT_ACTIVE), ptr<int32_t>(RID_TACT_HOVERED_UNIT_ID),
                   ptr<int32_t>(RID_TACT_ACTIVE_UNIT_COUNT),
                   ptr<int32_t>(RID_TACT_ACTIVE_UNIT_COUNT_CACHED), ptr<uint8_t>(RID_TILE_DRAWN_MAP),
                   ptr<int32_t>(RID_TACT_CAM_FOLLOW_SELECTION), ptr<void *>(RID_TACT_SEL_PANEL_ICON_GFX),
                   ptr<int32_t>(RID_TACT_UI_SEL_PANEL_MULTI_MODE),
                   ptr<int32_t>(RID_TACT_SEL_PANEL_ICON_SLOT_STATE),
                   ptr<int32_t>(RID_TACT_SIDEBAR_ACTIVE_GROUP_ID),
                   ptr<int32_t>(RID_TACT_SIDEBAR_SLOT_SCROLL),
                   ptr<int32_t>(RID_TACT_SIDEBAR_SCROLLBTN_STATE),
                   ptr<int32_t>(RID_TACT_SIDEBAR_UI_HIT_CODE),
                   ptr<int32_t>(RID_TACT_SIDEBAR_HIGHLIGHTED_UNIT_ID),
                   ptr<int32_t>(RID_TACT_SIDEBAR_SLOT_UNIT_IDS),
                   ptr<int32_t>(RID_SQUAD_BB_TARGET_ENERGY_PCT), ptr<uint8_t *>(RID_TILE_VIS_MAP_PTR),
                   ptr<uint8_t *>(RID_TACT_LOS_CACHE_PTR),
                   // TACT1E batch (2026-08-28).
                   ptr<int32_t>(RID_TACT_SIDEBAR_UNASSIGNED_SCROLL_ROW),
                   ptr<int32_t>(RID_TACT_SIDEBAR_GROUP_SCROLL_ROW),
                   ptr<uint8_t>(RID_TACT_UNASSIGNED_UNIT_ROSTER), ptr<uint8_t>(RID_TACT_GROUP_UNIT_ROSTER),
                   ptr<uint8_t>(RID_MOUSE_BUTTONS_CUR), ptr<wchar_t>(RID_G_TEXT_TMP));
    return tact_state{v, own};
}


} // namespace mh::tact
