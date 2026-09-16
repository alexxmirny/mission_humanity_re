//
// sim/sim_state.cpp -- binding the sim state interface to the live game (RI-SIM / SIM0), plus the
// domain's shadow-arm install point (the conductor-owned aggregator; the per-site installers live in
// their own TUs).
//
// The binding names no logic and the logic names no address: addresses come from the region registry
// here; the translations in the other libmh/sim/ TUs never see one.
//
// THIS IS THE ONLY FILE UNDER libmh/sim/ ALLOWED TO NAME mh::state::ptr. tools/lint_repo.py's
// sim-raw-address check enforces that -- see sim_state.h's W2. If a translation needs a region,
// the answer is a new member here plus an accessor, not a `ptr<>` at the use site.
//
#include "sim/sim_state.h"

#include "addr/mh_bldg_type_callbacks.gen.h" // the extracted per-building-TYPE registry (SIM1-BLDGCB)

#include "sim/sim_ai_bldg_queue_construction.h"
#include "sim/sim_bldg_add_workers.h"
#include "sim/sim_bldg_anim_state_online.h"
#include "sim/sim_bldg_anim_state_port.h"
#include "sim/sim_bldg_anim_tick.h"
#include "sim/sim_bldg_anim_trigger.h"
#include "sim/sim_bldg_apply_damage.h"
#include "sim/sim_bldg_clear_flag_bit0_notify.h"
#include "sim/sim_bldg_completion_dispatch.h"
#include "sim/sim_bldg_construct_finalize.h"
#include "sim/sim_bldg_construction_complete.h"
#include "sim/sim_bldg_defense_cost.h"
#include "sim/sim_bldg_done_handlers.h"
#include "sim/sim_bldg_economy.h"
#include "sim/sim_bldg_find_idle_producer.h"
#include "sim/sim_bldg_find_mothership_position.h"
#include "sim/sim_bldg_finish_order.h"
#include "sim/sim_bldg_flush_cargo_hold.h"
#include "sim/sim_bldg_footprint_clear_passable.h"
#include "sim/sim_bldg_footprint_is_clear.h"
#include "sim/sim_bldg_footprint_random_offset.h"
#include "sim/sim_bldg_footprint_set_passable.h"
#include "sim/sim_bldg_free_record.h"
#include "sim/sim_bldg_get_coords.h"
#include "sim/sim_bldg_grant_type_resources.h"
#include "sim/sim_bldg_has_aa_weapon.h"
#include "sim/sim_bldg_init_all.h"
#include "sim/sim_bldg_instant_construct.h"
#include "sim/sim_bldg_instant_construct_find_slot.h"
#include "sim/sim_bldg_liftoff_anim.h"
#include "sim/sim_bldg_link_to_network.h"
#include "sim/sim_bldg_load_resource_tail_noop.h"
#include "sim/sim_bldg_mother_reelect_primary.h"
#include "sim/sim_bldg_mothership_alive.h"
#include "sim/sim_bldg_notify_state_change.h"
#include "sim/sim_bldg_notify_ui.h"
#include "sim/sim_bldg_online_secondary.h"
#include "sim/sim_bldg_pay_costs.h"
#include "sim/sim_bldg_placement_corner.h"
#include "sim/sim_bldg_placement_enclosure.h"
#include "sim/sim_bldg_placement_preview.h"
#include "sim/sim_bldg_population_layoff_workers.h"
#include "sim/sim_bldg_power_network_recompute.h"
#include "sim/sim_bldg_power_recompute.h"
#include "sim/sim_bldg_propagate_network_connectivity.h"
#include "sim/sim_bldg_refresh_all_buildings.h"
#include "sim/sim_bldg_refund_resources_scaled_by_energy.h"
#include "sim/sim_bldg_register_online.h"
#include "sim/sim_bldg_remove_workers.h"
#include "sim/sim_bldg_reset_construction_anim.h"
#include "sim/sim_bldg_roster_queries.h"
#include "sim/sim_bldg_scrap_stored_units.h"
#include "sim/sim_bldg_set_connected_flag.h"
#include "sim/sim_bldg_shuttle_slot_is_free.h"
#include "sim/sim_bldg_side_has_aircraft_producer.h"
#include "sim/sim_bldg_sprite_anchor.h"
#include "sim/sim_bldg_staffed_flag.h"
#include "sim/sim_bldg_state_charge.h"
#include "sim/sim_bldg_state_deploy.h"
#include "sim/sim_bldg_state_destroyed.h"
#include "sim/sim_bldg_state_dismantle.h"
#include "sim/sim_bldg_state_hangar.h"
#include "sim/sim_bldg_state_mine.h"
#include "sim/sim_bldg_state_mine_scan.h"
#include "sim/sim_bldg_state_power.h"
#include "sim/sim_bldg_state_prod.h"
#include "sim/sim_bldg_state_reset_idle.h"
#include "sim/sim_bldg_state_rubble_decay.h"
#include "sim/sim_bldg_state_to_unit.h"
#include "sim/sim_bldg_state_turret.h"
#include "sim/sim_bldg_state_upgrade_research.h"
#include "sim/sim_bldg_tick_animation_state.h"
#include "sim/sim_bldg_tick_pip_anim.h"
#include "sim/sim_bldg_transfer_notify_noop.h"
#include "sim/sim_bldg_turret_combat.h"
#include "sim/sim_bldg_turret_reload_tick.h"
#include "sim/sim_bldg_unmap_footprint.h"
#include "sim/sim_bldg_update_charge_pips.h"
#include "sim/sim_bldg_uses_workers.h"
#include "sim/sim_bldg_worker_assign.h"
#include "sim/sim_building_tick.h"
#include "sim/sim_cfg_anim_frame_at_progress.h"
#include "sim/sim_cfg_apply_project_resources.h"
#include "sim/sim_check_storage_overflow.h"
#include "sim/sim_combat_credit_planet_conquest_kills.h"
#include "sim/sim_combat_kill_credit.h"
#include "sim/sim_debug_roll_random.h"
#include "sim/sim_diplomacy_ai_relation_swap.h"
#include "sim/sim_diplomacy_set_relation.h"
#include "sim/sim_dir_headings.h"
#include "sim/sim_dist_out_of_range.h"
#include "sim/sim_dmp_enqueue_scripted_order.h"
#include "sim/sim_dock_slot_is_busy.h"
#include "sim/sim_fog_of_war.h"
#include "sim/sim_fx_anim_chain_find_tail.h"
#include "sim/sim_fx_anim_seq_cancel.h"
#include "sim/sim_fx_anim_tick.h"
#include "sim/sim_fx_debris_burst.h"
#include "sim/sim_game_add_planet_to_available.h"
#include "sim/sim_game_add_project_to_available.h"
#include "sim/sim_game_add_to_available_buildings.h"
#include "sim/sim_game_get_starting_unit.h"
#include "sim/sim_game_handle_progress.h"
#include "sim/sim_game_handle_upgrade.h"
#include "sim/sim_game_insert_item_in_player_array.h"
#include "sim/sim_game_notify_system_available.h"
#include "sim/sim_game_player_set_side.h"
#include "sim/sim_game_set_event.h"
#include "sim/sim_game_sp_outcome_announce.h"
#include "sim/sim_game_speed_adjust.h"
#include "sim/sim_game_speed_recompute.h"
#include "sim/sim_game_try_start_project.h"
#include "sim/sim_game_update_progress.h"
#include "sim/sim_game_update_resource_stats.h"
#include "sim/sim_geom_toroidal.h"
#include "sim/sim_group_move_order_commit.h"
#include "sim/sim_group_move_order_pathfind.h"
#include "sim/sim_group_move_register_member.h"
#include "sim/sim_group_path_step_append.h"
#include "sim/sim_group_plan_formation_positions.h"
#include "sim/sim_group_scratch_add_unit.h"
#include "sim/sim_hangar_energy.h"
#include "sim/sim_invasion.h"
#include "sim/sim_invasion_alert_arm.h"
#include "sim/sim_landing_queries.h"
#include "sim/sim_landing_spot.h"
#include "sim/sim_load_base_layout_dmp.h"
#include "sim/sim_locate_active_port.h"
#include "sim/sim_map_apply_area.h"
#include "sim/sim_map_create_building.h"
#include "sim/sim_map_fog_of_war_recompute.h"
#include "sim/sim_map_fow_reveal_full.h"
#include "sim/sim_map_region_helpers.h"
#include "sim/sim_map_region_recompute_adjacency.h"
#include "sim/sim_map_region_routing.h"
#include "sim/sim_map_region_split.h"
#include "sim/sim_map_unit_add.h"
#include "sim/sim_map_wrap_delta_row.h"
#include "sim/sim_map_zoom_scale_x_get.h"
#include "sim/sim_mine_scan_deposit_slot.h"
#include "sim/sim_order_dispatch.h"
#include "sim/sim_order_enqueue.h"
#include "sim/sim_order_issue_0xf_adjacent.h"
#include "sim/sim_path_attach_slot.h"
#include "sim/sim_path_group_steps.h"
#include "sim/sim_path_make_single_step.h"
#include "sim/sim_path_slot_dist.h"
#include "sim/sim_path_solver.h"
#include "sim/sim_path_step_check_and_request_detour.h"
#include "sim/sim_pathfind_geom_misc.h"
#include "sim/sim_pathfind_route.h"
#include "sim/sim_pathfind_route_leg_group_and_sort.h"
#include "sim/sim_pathfind_route_leg_reconcile.h"
#include "sim/sim_pathtrace_dir_table.h"
#include "sim/sim_pathtrace_greedy.h"
#include "sim/sim_planet_distance.h"
#include "sim/sim_player_teardown_hook_stub.h"
#include "sim/sim_population_add.h"
#include "sim/sim_population_change.h"
#include "sim/sim_prod_bind_planet.h"
#include "sim/sim_prod_bldg_depart_finalize.h"
#include "sim/sim_prod_cargo.h"
#include "sim/sim_prod_deliver_arrivals.h"
#include "sim/sim_prod_planet_distance_factor.h"
#include "sim/sim_prod_shuttle_bay_unload_all.h"
#include "sim/sim_prod_shuttle_complete.h"
#include "sim/sim_prod_shuttle_depart.h"
#include "sim/sim_prod_shuttle_fuel_apply.h"
#include "sim/sim_prod_shuttle_fuel_check.h"
#include "sim/sim_prod_shuttle_load_passengers.h"
#include "sim/sim_prod_shuttle_load_resource.h"
#include "sim/sim_prod_shuttle_slot_bind_default.h"
#include "sim/sim_prod_shuttle_slot_release.h"
#include "sim/sim_prod_shuttle_unload_passengers.h"
#include "sim/sim_prod_shuttle_unload_resource.h"
#include "sim/sim_prod_spawn_arrived_unit.h"
#include "sim/sim_prod_unbind_planet.h"
#include "sim/sim_prod_unload_cargo_unit.h"
#include "sim/sim_projectile_tick.h"
#include "sim/sim_reason_to_housing_bldg.h"
#include "sim/sim_refresh_building.h"
#include "sim/sim_region_adjacency_edge.h"
#include "sim/sim_resource_add_spend.h"
#include "sim/sim_resource_decay.h"
#include "sim/sim_revoke_invention.h"
#include "sim/sim_rng_next.h"
// LT1A (2026-09-02): the lib_trans RNG-family wrappers over rng_next/rng_seed_channel + the adopted
// tick-slot step.
#include "sim/libtrans/sim_lt_rng_draws.h"
#include "sim/libtrans/sim_lt_rng_raw_step.h"
#include "sim/libtrans/sim_lt_rng_seed.h"
#include "sim/libtrans/sim_lt_anim_time.h"
#include "sim/libtrans/sim_lt_bldg_cell_grid.h"
#include "sim/libtrans/sim_lt_bldg_query.h"
#include "sim/libtrans/sim_lt_diplomacy.h"
#include "sim/libtrans/sim_lt_invasion_alert.h"
#include "sim/libtrans/sim_lt_map_setup_dimensions.h"
#include "sim/libtrans/sim_lt_math.h"
#include "sim/libtrans/sim_lt_prod_transfer.h"
#include "sim/libtrans/sim_lt_progress_planet_sweep.h"
#include "sim/libtrans/sim_lt_progress_unlock_fixpoint.h"
#include "sim/libtrans/sim_lt_scan_masked_table.h"
#include "sim/sim_step.h"
#include "sim/sim_storage_accepts_unit_type.h"
#include "sim/sim_storage_can.h"
#include "sim/sim_storage_cancel_pending_docked.h"
#include "sim/sim_storage_dock.h"
#include "sim/sim_storage_dock_unit_at_building.h"
#include "sim/sim_storage_exit_placement.h"
#include "sim/sim_storage_find_home_for_unit.h"
#include "sim/sim_storage_get_approach_tile.h"
#include "sim/sim_storage_launch_parked_to_orbit.h"
#include "sim/sim_storage_path.h"
#include "sim/sim_storage_purge_dead_docked.h"
#include "sim/sim_storage_release_door_held_by_unit.h"
#include "sim/sim_storage_remove_docked_unit.h"
#include "sim/sim_storage_scrap.h"
#include "sim/sim_storage_type_accepts_unit.h"
#include "sim/sim_target_class.h"
#include "sim/sim_target_release_ref.h"
#include "sim/sim_tile_delta_wrapped.h"
#include "sim/sim_tile_neighbor_reverse_dir.h"
#include "sim/sim_tile_pixel_wrap_delta.h"
#include "sim/sim_unit_add_docked.h"
#include "sim/sim_unit_apply_damage.h"
#include "sim/sim_unit_apply_production_completion.h"
#include "sim/sim_unit_bldg_apply_lethal_damage.h"
#include "sim/sim_unit_bldg_apply_scaled_damage.h"
#include "sim/sim_unit_bldg_energy_refill_full.h"
#include "sim/sim_unit_change_proto_and_energy.h"
#include "sim/sim_unit_chase_check.h"
#include "sim/sim_unit_create.h"
#include "sim/sim_unit_create_soldier.h"
#include "sim/sim_unit_ctrl_group.h"
#include "sim/sim_unit_ctrlgroup_leave.h"
#include "sim/sim_unit_ctrlgroup_member.h"
#include "sim/sim_unit_facing24_delta.h"
#include "sim/sim_unit_fine_pos.h"
#include "sim/sim_unit_fire_at_target.h"
#include "sim/sim_unit_fire_at_target2_if_aimed.h"
#include "sim/sim_unit_fire_weapon.h"
#include "sim/sim_unit_force_disembark.h"
#include "sim/sim_unit_free_slot.h"
#include "sim/sim_unit_get_ready_home_building.h"
#include "sim/sim_unit_get_sight.h"
#include "sim/sim_unit_goal_in_weapon_range.h"
#include "sim/sim_unit_group_step_ground.h"
#include "sim/sim_unit_group_step_plane.h"
#include "sim/sim_unit_housing_count.h"
#include "sim/sim_unit_hover_tile_crowded.h"
#include "sim/sim_unit_idle_state.h"
#include "sim/sim_unit_in_weapon_range.h"
#include "sim/sim_unit_init_record.h"
#include "sim/sim_unit_is_boarding.h"
#include "sim/sim_unit_load_into_shuttle_cargo.h"
#include "sim/sim_unit_mount_pos.h"
#include "sim/sim_unit_notify.h"
#include "sim/sim_unit_on_destroyed.h"
#include "sim/sim_unit_order_disembark_soldiers.h"
#include "sim/sim_unit_passive_engage.h"
#include "sim/sim_unit_path_detour.h"
#include "sim/sim_unit_path_free_slot.h"
#include "sim/sim_unit_population_remove.h"
#include "sim/sim_unit_purge_unregistered.h"
#include "sim/sim_unit_put_on_map.h"
#include "sim/sim_unit_queue_advance.h"
#include "sim/sim_unit_recruit.h"
#include "sim/sim_unit_refund.h"
#include "sim/sim_unit_remove_from_map.h"
#include "sim/sim_unit_select_weapon.h"
#include "sim/sim_unit_set_state.h"
#include "sim/sim_unit_set_state_of.h"
#include "sim/sim_unit_set_state_order.h"
#include "sim/sim_unit_set_state_order_of.h"
#include "sim/sim_unit_soldier_anim.h"
#include "sim/sim_unit_soldier_chain.h"
#include "sim/sim_unit_soldier_screen_pos.h"
#include "sim/sim_unit_spawn_docked.h"
#include "sim/sim_unit_spawn_on_tile.h"
#include "sim/sim_unit_state_attack_building.h"
#include "sim/sim_unit_state_attack_unit.h"
#include "sim/sim_unit_state_budget_noop.h"
#include "sim/sim_unit_state_corpse_fow_decay.h"
#include "sim/sim_unit_state_deploy.h"
#include "sim/sim_unit_state_die_explode.h"
#include "sim/sim_unit_state_enter.h"
#include "sim/sim_unit_state_exit.h"
#include "sim/sim_unit_state_flight.h"
#include "sim/sim_unit_state_group_marshal.h"
#include "sim/sim_unit_state_group_step.h"
#include "sim/sim_unit_state_hover_engage.h"
#include "sim/sim_unit_state_idle_scatter.h"
#include "sim/sim_unit_state_misc2.h"
#include "sim/sim_unit_state_move_path.h"
#include "sim/sim_unit_state_move_walker.h"
#include "sim/sim_unit_state_patrol_swap.h"
#include "sim/sim_unit_state_plot_turn_path.h"
#include "sim/sim_unit_state_predicates.h"
#include "sim/sim_unit_state_remove_silent.h"
#include "sim/sim_unit_state_squad_merge.h"
#include "sim/sim_unit_state_stop_to_default.h"
#include "sim/sim_unit_state_takeoff.h"
#include "sim/sim_unit_state_taxi_dock.h"
#include "sim/sim_unit_storage_transit.h"
#include "sim/sim_unit_target_tracking.h"
#include "sim/sim_unit_teardown.h"
#include "sim/sim_unit_teardown_mapped.h"
#include "sim/sim_unit_tick.h"
#include "sim/sim_unit_transport_unload.h"
#include "sim/sim_unit_try_pay_action_cost.h"
#include "sim/sim_unit_type_group_index.h"
#include "sim/sim_unit_type_predicates.h"
#include "sim/sim_unit_unlink_tile.h"
#include "sim/sim_unit_update_anim.h"
#include "sim/sim_unit_update_damage_smoke.h"
#include "sim/sim_unit_update_rotation.h"
#include "sim/sim_unit_update_soldiers.h"
#include "sim/sim_unit_walk_step_allowed.h"
#include "sim/sim_unit_weapon_reload_tick.h"
#include "sim/sim_unit_weapons.h"
#include "sim/sim_weapon_damage_calc.h"
#include "sim/sim_weapon_projectile_spawn.h"

namespace mh::sim {

using namespace mh::state;

sim_state state() {
    sim_view v{};
    // ST2/Law 3: live_base, not the constexpr .bss address. Re-resolved per call -- see
    // sim_state.h. Nothing here is cached and nothing is static.
    v.units     = ptr<const unit>(RID_UNITS);
    v.buildings = ptr<const building>(RID_BUILDINGS);
    v.players   = ptr<const player_data>(RID_PLAYER_DATA);
    // SB-BIND T2: the row capacities, derived from the sizes the HOST bound. Same posture as the
    // pointers above -- resolved per call, nothing cached -- so a rebase between calls is followed.
    v.caps = live_roster_caps();

    v.cfg_units      = ptr<const cfg_unit>(RID_UNIT);
    v.cfg_buildings  = ptr<const cfg_building>(RID_BUILDING);
    v.cfg_weapons    = ptr<const cfg_weapon>(RID_WEAPON);
    v.cfg_projects   = ptr<const cfg_project>(RID_PROJECTS);
    v.cfg_inventions = ptr<const cfg_invention>(RID_PROGRESS_00E162E4);
    v.cfg_upgrades   = ptr<const cfg_upgrade>(RID_UPGRADES);

    v.upgrade_unit_speed_pct_divisor  = ptr<const double>(RID_STRAT_UPGRADE_SPEED_PERCENT_DIVISOR);
    v.upgrade_weapon_pct_divisor      = ptr<const double>(RID_100F);
    v.upgrade_msg_sep_before_category = ptr<const wchar_t>(RID_STRAT_UPGRADE_MSG_SEP_BEFORE_CATEGORY);
    v.upgrade_msg_sep_before_name     = ptr<const wchar_t>(RID_STRAT_UPGRADE_MSG_SEP_BEFORE_NAME);
    v.upgrade_msg_clause_sep_first    = ptr<const wchar_t>(RID_STRAT_UPGRADE_MSG_CLAUSE_SEP_FIRST);
    v.upgrade_msg_clause_sep_next     = ptr<const wchar_t>(RID_STRAT_UPGRADE_MSG_CLAUSE_SEP_NEXT);
    v.upgrade_msg_trailer             = ptr<const wchar_t>(RID_STRAT_UPGRADE_MSG_TRAILER);

    v.invasion_roll_base_time = ptr<const double>(RID_STRAT_INVASION_ROLL_BASE_TIME);
    v.local_player_slot       = ptr<const uint16_t>(RID_STRAT_LOCAL_PLAYER_SLOT);

    v.dir24_rad2deg_num    = ptr<const double>(RID_STRAT_DIR24_RAD2DEG_NUM);
    v.dir24_rad2deg_den    = ptr<const double>(RID_STRAT_DIR24_RAD2DEG_DEN);
    v.dir24_half_turn_deg  = ptr<const double>(RID_STRAT_DIR24_HALF_TURN_DEG);
    v.dir24_bias_deg       = ptr<const double>(RID_STRAT_DIR24_BIAS_DEG);
    v.dir24_wrap_add_deg   = ptr<const double>(RID_STRAT_DIR24_WRAP_ADD_DEG);
    v.dir24_wrap_limit_deg = ptr<const double>(RID_STRAT_DIR24_WRAP_LIMIT_DEG);
    v.dir24_wrap_sub_deg   = ptr<const double>(RID_STRAT_DIR24_WRAP_SUB_DEG);
    v.dir24_sector_deg     = ptr<const double>(RID_STRAT_DIR24_SECTOR_DEG);

    v.dir128_rad2deg_num               = ptr<const double>(RID_STRAT_DIR128_RAD2DEG_NUM);
    v.dir128_rad2deg_den               = ptr<const double>(RID_STRAT_DIR128_RAD2DEG_DEN);
    v.dir128_half_turn_deg             = ptr<const double>(RID_STRAT_DIR128_HALF_TURN_DEG);
    v.dir128_bias_deg                  = ptr<const double>(RID_STRAT_DIR128_BIAS_DEG);
    v.dir128_wrap_add_deg              = ptr<const double>(RID_STRAT_DIR128_WRAP_ADD_DEG);
    v.dir128_wrap_limit_deg            = ptr<const double>(RID_STRAT_DIR128_WRAP_LIMIT_DEG);
    v.dir128_wrap_sub_deg              = ptr<const double>(RID_STRAT_DIR128_WRAP_SUB_DEG);
    v.dir128_sector_deg                = ptr<const double>(RID_STRAT_DIR128_SECTOR_DEG);
    v.bldg_count_offline_decrement     = ptr<const double>(RID_STRAT_BLDG_COUNT_OFFLINE_DECREMENT);
    v.unit_energy_status_percent_scale = ptr<const double>(RID_STRAT_UNIT_ENERGY_STATUS_PERCENT_SCALE);
    v.bldg_energy_status_percent_scale = ptr<const double>(RID_STRAT_BLDG_ENERGY_STATUS_PERCENT_SCALE);
    v.ai_base_spawn_timer_stagger_scale =
        ptr<const double>(RID_STRAT_AI_BASE_SPAWN_TIMER_STAGGER_SCALE);
    v.mine_worth  = ptr<const int32_t>(RID_STRAT_AI_CFG_MINE_WORTH);
    v.sprite_meta = ptr<const sprite_meta_entry>(RID_SPRITE_META);

    // SIM1A; REBASED LT1E (2026-09-02). facing_step_offset used to alias
    // RID_CURSOR_ANIM_STATES + 0x100 -- which, since that region's v312 retype shrank it to 256 B,
    // pointed one byte PAST its declared extent, leaving these 200 dereferenced bytes inside no
    // registered region. The table now has its own row (RID_STRAT_FACING_STEP_SIGN @0xae4768,
    // llm_vec2i[25]); bound at offset 0. Same bytes, honest extent.
    v.facing_step_offset =
        reinterpret_cast<const facing_step_offset_pair *>(ptr<const uint8_t>(RID_STRAT_FACING_STEP_SIGN));

    v.sprite_pix_offsets = ptr<const uint32_t>(RID_SPRITE_PIX_OFFSETS);
    v.gfx_bank_pixels    = ptr<uint8_t *const>(RID_GFX_BANK_PIXELS);

    // LT1 lib_trans const singles (2026-09-02) -- see the sim_view block comment.
    v.rng_norm_divisor         = ptr<const double>(RID_STRAT_RNG_NORM_DIVISOR);
    v.invasion_alert_interval  = ptr<const double>(RID_STRAT_INVASION_ALERT_INTERVAL);
    v.math_percent_divisor     = ptr<const double>(RID_MATH_PERCENT_DIVISOR);
    v.deploy_formation_stencil = ptr<const uint8_t>(RID_STRAT_DEPLOY_FORMATION_STENCIL);

    v.map_width  = ptr<const int32_t>(RID_WIDTH);
    v.map_height = ptr<const int32_t>(RID_HEIGHT);

    v.zoom_scale_x = ptr<const double>(RID_MAP_ZOOM_SCALE_X);

    v.region_list_head       = ptr<llm_map_region *const>(RID_MAP_REGION_LIST_HEAD);
    v.region_merge_threshold = ptr<const uint32_t>(RID_MAP_REGION_MERGE_THRESHOLD);

    v.group_move_scratch = ptr<const group_scratch_member>(RID_STRAT_GROUP_MOVE_SCRATCH);
    v.path_buffers       = ptr<const path_waypoint>(RID_STRAT_PATH_BUFFERS);

    v.heading_candidates           = ptr<const heading_slot>(RID_STRAT_HEADING_CANDIDATE_TABLE);
    v.group_step_heading_remap     = ptr<const int32_t>(RID_STRAT_GROUP_STEP_HEADING_REMAP);
    v.pathfinder_air_mode_flag     = ptr<const int32_t>(RID_STRAT_PATHFINDER_AIR_MODE_FLAG);
    v.squad_placement_offset_table = ptr<const uint8_t>(RID_STRAT_SQUAD_PLACEMENT_OFFSET_TABLE);
    v.squad_anchor_scratch         = ptr<const squad_formation_anchor_scratch>(RID_STRAT_SQUAD_FORMATION_ANCHOR_SCRATCH);

    v.group_route_steps  = ptr<const route_step>(RID_STRAT_GROUP_ROUTE_STEPS);
    v.group_members      = ptr<const group_member>(RID_STRAT_GROUP_MEMBERS);
    v.group_member_tile  = ptr<const uint8_t>(RID_STRAT_GROUP_MEMBER_TILE);
    v.path_wrap_mask     = ptr<const uint32_t>(RID_STRAT_PATH_WRAP_MASK);
    v.group_order_goal_x = ptr<const int32_t>(RID_STRAT_GROUP_ORDER_GOAL_X);
    v.group_order_goal_y = ptr<const int32_t>(RID_STRAT_GROUP_ORDER_GOAL_Y);
    v.group_anchor_x     = ptr<const int32_t>(RID_STRAT_GROUP_ANCHOR_X);
    v.group_anchor_y     = ptr<const int32_t>(RID_STRAT_GROUP_ANCHOR_Y);
    v.group_order_owner  = ptr<const int32_t>(RID_STRAT_GROUP_ORDER_OWNER);
    v.group_member_count = ptr<const int32_t>(RID_STRAT_GROUP_MEMBER_COUNT);
    v.pathtrace_len      = ptr<const uint32_t>(RID_STRAT_PATHTRACE_LEN);

    // SIM1-G2 (2026-08-20). Read-only pathfinding/heading constants -- no writer in the closure.
    v.map_dir_step_deltas         = ptr<const uint8_t>(RID_MAP_DIR_STEP_DELTAS);
    v.group_move_wave_dist_scale  = ptr<const double>(RID_STRAT_GROUP_MOVE_WAVE_DIST_SCALE);
    v.group_move_wave_dist_bias   = ptr<const double>(RID_STRAT_GROUP_MOVE_WAVE_DIST_BIAS);
    v.move_path_heading_scale_far = ptr<const double>(RID_STRAT_MOVE_PATH_HEADING_SCALE_FAR);
    v.move_path_heading_scale_mid = ptr<const double>(RID_STRAT_MOVE_PATH_HEADING_SCALE_MID);

    // SIM1-G4 (2026-08-22). Nine boot-constant doubles -- see sim_state.h's comment.
    v.hangar_recharge_period          = ptr<const double>(RID_STRAT_HANGAR_RECHARGE_PERIOD);
    v.hangar_recharge_period_neg      = ptr<const double>(RID_STRAT_HANGAR_RECHARGE_PERIOD_NEG);
    v.dismantle_progress_divisor      = ptr<const double>(RID_STRAT_DISMANTLE_PROGRESS_DIVISOR);
    v.bldg_dismantle_hq_energy_credit = ptr<const double>(RID_STRAT_BLDG_DISMANTLE_HQ_ENERGY_CREDIT);
    v.rubble_sight_decay_period       = ptr<const double>(RID_STRAT_RUBBLE_SIGHT_DECAY_PERIOD);
    v.rubble_sight_decay_period_neg   = ptr<const double>(RID_STRAT_RUBBLE_SIGHT_DECAY_PERIOD_NEG);
    v.rubble_cleanup_period           = ptr<const double>(RID_STRAT_RUBBLE_CLEANUP_PERIOD);
    v.prod_retry_period               = ptr<const double>(RID_STRAT_PROD_RETRY_PERIOD);
    v.prod_retry_period_neg           = ptr<const double>(RID_STRAT_PROD_RETRY_PERIOD_NEG);

    // SIM1-G4 (2026-08-22). Same gap class -- see sim_state.h's comment.
    v.refund_energy_factor    = ptr<const double>(RID_STRAT_REFUND_ENERGY_FACTOR);
    v.mine_extract_period     = ptr<const double>(RID_STRAT_MINE_EXTRACT_PERIOD);
    v.mine_extract_period_neg = ptr<const double>(RID_STRAT_MINE_EXTRACT_PERIOD_NEG);
    v.mine_rescan_period      = ptr<const double>(RID_STRAT_MINE_RESCAN_PERIOD);
    v.mine_rescan_period_neg  = ptr<const double>(RID_STRAT_MINE_RESCAN_PERIOD_NEG);

    // SIM1-G4 (2026-08-22). llm_strat_bldg_completion_dispatch.
    v.mother_lost_escalation_interval = ptr<const double>(RID_STRAT_MOTHER_LOST_ESCALATION_INTERVAL);
    v.bldg_completion_slot_count      = ptr<const uint8_t>(RID_STRAT_BLDG_COMPLETION_SLOT_COUNT);
    v.resources                       = ptr<const map_resources>(RID_RESOURCES);

    // SIM1-G2 (2026-08-20). The pathtrace tracer's read-only game-data tables.
    v.dir_bitmask_table       = ptr<const uint32_t>(RID_STRAT_DIR_BITMASK_TABLE);
    v.coord_sign_lut          = ptr<const uint8_t>(RID_STRAT_COORD_SIGN_LUT);
    v.move_dir_table          = ptr<const move_dir_step>(RID_STRAT_MOVE_DIR_TABLE);
    v.dir8_step_offsets       = ptr<const int8_t>(RID_STRAT_DIR8_STEP_OFFSETS);
    v.pathtrace_dir_merge_lut = ptr<const uint8_t>(RID_STRAT_PATHTRACE_DIR_MERGE_LUT);

    // SIM1-G-PREP (2026-08-20): the batch-G write set, pre-registered so no batch wires a region
    // inline (R8). See sim_state.h's matching block, and the dll_addr_manifest note at each address.
    v.group_centroid_x     = ptr<const int32_t>(RID_STRAT_GROUP_CENTROID_X);
    v.group_centroid_y     = ptr<const int32_t>(RID_STRAT_GROUP_CENTROID_Y);
    v.group_path_build_idx = ptr<const int32_t>(RID_STRAT_GROUP_PATH_BUILD_IDX);

    v.region_route_cand_scratch = ptr<const uint8_t>(RID_MAP_REGION_ROUTE_CAND_SCRATCH);
    v.region_flood_tile_queue   = ptr<const uint32_t>(RID_MAP_REGION_FLOOD_TILE_QUEUE);
    v.region_route_bfs_queue    = ptr<llm_map_region *const>(RID_MAP_REGION_ROUTE_BFS_QUEUE);
    v.region_coord_wrap_mask    = ptr<const uint32_t>(RID_MAP_REGION_COORD_WRAP_MASK);

    // SIM1-G2 (2026-08-21). llm_map_region_route_search's step/output tables plus
    // llm_strat_pathtrace_normalize_repeat_dir_table's diagonal-merge lookup -- all three already
    // named+typed in Ghidra, only the binding was missing.
    v.region_route_step_deltas = ptr<const uint8_t>(RID_MAP_REGION_ROUTE_STEP_DELTAS);
    // SIM1-H wave 2 (2026-09-10): the 13x13 obstacle-proximity stencil (read-only boot data).
    v.proximity_stencil         = ptr<const proximity_stencil_entry>(RID_MAP_PROXIMITY_STENCIL);
    v.region_route_dir_codes    = ptr<const int32_t>(RID_MAP_REGION_ROUTE_DIR_CODES);
    v.pathtrace_dir_split_table = ptr<const llm_strat_pathtrace_dir_split>(RID_STRAT_PATHTRACE_DIR_SPLIT_TABLE);

    v.bldg_completion_accum = ptr<const int32_t>(RID_STRAT_BLDG_COMPLETION_ACCUM);

    v.unitq_cur_tile       = ptr<const uint32_t>(RID_UNITQ_CUR_TILE);
    v.unitq_closed_count   = ptr<const int32_t>(RID_UNITQ_CLOSED_COUNT);
    v.unitq_iter           = ptr<const int32_t>(RID_UNITQ_ITER);
    v.unitq_frontier_count = ptr<const int32_t>(RID_UNITQ_FRONTIER_COUNT);
    v.unitq_closed         = ptr<const unitq_search_node>(RID_UNITQ_CLOSED);
    v.unitq_frontier       = ptr<const unitq_search_node>(RID_UNITQ_FRONTIER);
    v.unitq_neighbor_dx    = ptr<const int32_t>(RID_UNITQ_NEIGHBOR_DX);
    v.unitq_neighbor_dy    = ptr<const int32_t>(RID_UNITQ_NEIGHBOR_DY);

    v.pathtrace_coord_mask   = ptr<const uint32_t>(RID_STRAT_PATHTRACE_COORD_MASK);
    v.pathtrace_col_mask     = ptr<const uint32_t>(RID_STRAT_PATHTRACE_COL_MASK);
    v.pathtrace_row_mask     = ptr<const uint32_t>(RID_STRAT_PATHTRACE_ROW_MASK);
    v.pathtrace_map_w        = ptr<const uint32_t>(RID_STRAT_PATHTRACE_MAP_W);
    v.pathtrace_map_h        = ptr<const uint32_t>(RID_STRAT_PATHTRACE_MAP_H);
    v.pathtrace_half_w       = ptr<const uint32_t>(RID_STRAT_PATHTRACE_HALF_W);
    v.pathtrace_half_h       = ptr<const uint32_t>(RID_STRAT_PATHTRACE_HALF_H);
    v.pathtrace_half_w_m1    = ptr<const uint32_t>(RID_STRAT_PATHTRACE_HALF_W_M1);
    v.pathtrace_half_h_m1    = ptr<const uint32_t>(RID_STRAT_PATHTRACE_HALF_H_M1);
    v.pathtrace_neg_half_w   = ptr<const int32_t>(RID_STRAT_PATHTRACE_NEG_HALF_W);
    v.pathtrace_neg_half_h   = ptr<const int32_t>(RID_STRAT_PATHTRACE_NEG_HALF_H);
    v.pathtrace_start_col    = ptr<const uint32_t>(RID_STRAT_PATHTRACE_START_COL);
    v.pathtrace_start_row    = ptr<const uint32_t>(RID_STRAT_PATHTRACE_START_ROW);
    v.pathtrace_walk_dir     = ptr<const uint32_t>(RID_STRAT_PATHTRACE_WALK_DIR);
    v.pathtrace_goal_col     = ptr<const uint32_t>(RID_STRAT_PATHTRACE_GOAL_COL);
    v.pathtrace_goal_row     = ptr<const uint32_t>(RID_STRAT_PATHTRACE_GOAL_ROW);
    v.pathtrace_goal_packed  = ptr<const uint32_t>(RID_STRAT_PATHTRACE_GOAL_PACKED);
    v.pathtrace_approach_dir = ptr<const uint32_t>(RID_STRAT_PATHTRACE_APPROACH_DIR);
    v.pathtrace_dirs         = ptr<const uint8_t>(RID_STRAT_PATHTRACE_DIRS);
    v.pathtrace_pos          = ptr<const uint16_t>(RID_STRAT_PATHTRACE_POS);
    v.pathtrace_best_dir     = ptr<const uint32_t>(RID_STRAT_PATHTRACE_BEST_DIR);
    v.pathtrace_best_dist    = ptr<const int32_t>(RID_STRAT_PATHTRACE_BEST_DIST);
    v.pathtrace_forbid_cells = ptr<const uint32_t>(RID_STRAT_PATHTRACE_FORBID_CELLS);

    v.geom    = ptr<const map_geom>(RID_GENERAL);
    v.storage = ptr<const unit_storage>(RID_UNIT_STORAGE);

    v.tile_objects = ptr<const tile_object>(RID_TILE_OBJECTS);

    // ---- SIM1C (llm_strat_order_queue_dispatch). See sim_state.h for the two-group split: the
    // first three are written by the sim elsewhere and only read on the dispatch path; the rest have
    // no writer in the closure at all.
    v.profiles     = ptr<const player_profile>(RID_STRAT_PLAYERS);
    v.population   = ptr<const pop_stats>(RID_STRAT_POP_STATS);
    v.session_mode = ptr<const int32_t>(RID_GAME_SESSION_MODE);

    v.move_microsteps               = ptr<const move_microstep>(RID_STRAT_MOVE_MICROSTEPS);
    v.sim_active                    = ptr<const int32_t>(RID_STRAT_SIM_ACTIVE);
    v.foreign_bldg_change_flag      = ptr<const int32_t>(RID_STRAT_AI_FOREIGN_BLDG_CHANGE_FLAG);
    v.tutorial_step                 = ptr<const int32_t>(RID_GAME_TUTORIAL_STEP);
    v.player_side                   = ptr<const int16_t>(RID_PLAYERSIDE);
    v.planet_index                  = ptr<const int32_t>(RID_G_PLANET_INDEX);
    v.planet_status                 = ptr<const int32_t>(RID_G_PLANET_STATUS);
    v.game_clock                    = ptr<const double>(RID_STRAT_GAME_CLOCK);
    v.lockstep_step_size            = ptr<const double>(RID_STRAT_LOCKSTEP_STEP_SIZE);
    v.current_system                = ptr<const int32_t>(RID_CURRENTSYSTEM);
    v.planet_available_notify_delay = ptr<const double>(RID_GAME_PLANET_AVAILABLE_NOTIFY_DELAY);
    v.pop_decay_factor              = ptr<const double>(RID_STRAT_POP_DECAY_FACTOR);
    v.pop_growth_factor             = ptr<const double>(RID_STRAT_POP_GROWTH_FACTOR);

    // SIM1-G3 (2026-08-21). See sim_state.h's comment above.
    v.exit_wait_operational_activity_bump  = ptr<const double>(RID_STRAT_STORAGE_EXIT_WAIT_OPERATIONAL_BUMP);
    v.exit_wait_default_activity_bump      = ptr<const double>(RID_STRAT_STORAGE_EXIT_WAIT_DEFAULT_BUMP);
    v.exit_walk_out_soldier_step_scale     = ptr<const double>(RID_STRAT_UNIT_EXIT_WALK_SOLDIER_STEP_SCALE);
    v.taxi_takeoff_step_speed_mult         = ptr<const double>(RID_STRAT_UNIT_TAKEOFF_TAXI_STEP_SPEED_MULT);
    v.taxi_dock_step_speed_mult            = ptr<const double>(RID_STRAT_UNIT_DOCK_TAXI_STEP_SPEED_MULT);
    v.enter_wait_activity_backoff_seconds  = ptr<const double>(RID_STRAT_UNIT_ENTER_WAIT_ACTIVITY_BACKOFF);
    v.enter_walk_in_soldier_transport_mult = ptr<const double>(RID_STRAT_UNIT_ENTER_WALK_SOLDIER_STEP_SCALE);

    // SIM1-G3 (2026-08-21). See sim_state.h's comment above.
    v.takeoff_landing_step_cost_scale         = ptr<const double>(RID_STRAT_UNIT_TAKEOFF_LANDING_STEP_COST_SCALE);
    v.takeoff_landing_a_helipad_activity_bump = ptr<const double>(RID_STRAT_UNIT_TAKEOFF_LANDING_HELIPAD_ACTIVITY_BUMP);
    v.step_adjacent_budget_gate               = ptr<const double>(RID_STRAT_UNIT_STEP_ADJACENT_BUDGET_GATE);

    // SIM1F (2026-08-17). See sim_state.h's comment above.
    v.cam_col   = ptr<const int32_t>(RID_MAP_CAM_COL);
    v.cam_row   = ptr<const int32_t>(RID_MAP_CAM_ROW);
    v.text_ptrs = ptr<const wchar_t *const>(RID_G_TEXT_PTRS);

    // SIM1F (2026-08-18): llm_strat_player_presence_lost's read-only inputs. See
    // sim_state.h for each. No writer in the sim closure, so no sim_store siblings for these four.
    v.debug_campaign_cheat       = ptr<const int32_t>(RID_STRAT_DEBUG_CAMPAIGN_CHEAT);
    v.mp_ally_victory_rule_flag  = ptr<const int32_t>(RID_STRAT_MP_ALLY_VICTORY_RULE_FLAG);
    v.system_lost_msg_shown_flag = ptr<const int32_t>(RID_STRAT_SYSTEM_LOST_MSG_SHOWN_FLAG);
    v.cheat_cmd_table            = ptr<char *const>(RID_CHEAT_CMD_TABLE);

    // ---- SIM1A (llm_strat_unit_tick and neighbours) ------------------------------
    //
    // SB-BIND T3: the stored pointer is TRANSLATED -- it names a stock address, which is the right
    // address only while the host binds stock bases. translate_slot is the identity in the hosted
    // configuration; the enumeration of every slot of this shape is tools/data/pointer_slots.json.
    // _G_LLM_STRAT_CUR_UNIT is a POINTER-VALUED global (map_object_unit*), not a row base -- resolve
    // it ONCE here (still per-call, same as every other member; nothing is cached across calls) and
    // bind both sim_view::cur_unit and sim_store::cur_unit_ from the SAME resolved live pointer, so
    // a read through one and a write through the other name the same record.
    unit *cur_unit_live = translate_slot(*ptr<unit *>(RID_STRAT_CUR_UNIT));
    v.cur_unit          = cur_unit_live;
    v.cur_player        = ptr<const uint16_t>(RID_STRAT_CUR_PLAYER);
    v.cur_index         = ptr<const uint16_t>(RID_STRAT_CUR_INDEX);

    v.tick_budget      = ptr<const double>(RID_STRAT_TICK_BUDGET);
    v.unit_state_funcs = ptr<const unit_state_fn>(RID_STRAT_UNIT_STATE_FUNCS);
    v.ctrl_groups      = ptr<const ctrl_group>(RID_STRAT_CTRL_GROUPS);
    // SIM1-H (2026-09-10): the four modifier-key bytes attack_order branches on.
    v.key_lctrl_held  = ptr<const uint8_t>(RID_KEY_LCTRL_HELD);
    v.key_lshift_held = ptr<const uint8_t>(RID_KEY_LSHIFT_HELD);
    v.key_rshift_held = ptr<const uint8_t>(RID_KEY_RSHIFT_HELD);
    v.key_lalt_held   = ptr<const uint8_t>(RID_KEY_LALT_HELD);
    v.anim_frames     = ptr<const anim_frame>(RID_ANIM);
    v.player_race     = ptr<const int32_t>(RID_STRAT_PLAYER_RACE);
    v.passable        = ptr<const uint8_t>(RID_PASSABLE);
    v.soldiers        = ptr<const soldier>(RID_STRAT_SOLDIERS);
    v.unit_housing    = ptr<const housing_stats>(RID_STRAT_UNIT_HOUSING_STATS);

    // ---- SIM1A (llm_strat_path_free_slot / _remove_from_map / _on_destroyed) --------
    v.click_select_target_flags = ptr<const uint16_t>(RID_CLICK_SELECT_TARGET_FLAGS);

    // ---- SIM1B (llm_strat_bldg_instant_construct_find_slot_enqueue's free-slot scan) ------------
    v.turrets            = ptr<const turret>(RID_TURRETS);
    v.productions        = ptr<const production>(RID_PRODUCTIONS);
    v.labs               = ptr<const lab>(RID_LABS);
    v.mines              = ptr<const mine>(RID_MINES);
    v.cfg_building_sec   = ptr<const cfg_building_section>(RID_BUILDING_00E5C9E4);
    v.cfg_unit_sec       = ptr<const cfg_unit_section>(RID_UNIT_00E5F638);
    v.cfg_planets        = ptr<const cfg_planet>(RID_PLANETS);
    v.width_m            = ptr<const uint32_t>(RID_WIDTH_M);
    v.height_m           = ptr<const uint32_t>(RID_HEIGHT_M);
    v.anim_place_seq_ids = ptr<const int32_t>(RID_ANIM_PLACE_DENIED);

    // SIM1B (2026-08-12; building_tick machinery slice). Second independent bindings of the SAME
    // regions ai/ai_state.h already binds -- see sim_state.h's `player_progress`/PLAYER_RESOURCE_SLOTS
    // comments.
    v.progress             = ptr<const player_progress>(RID_PROGRESS);
    v.player_resources     = ptr<const int32_t>(RID_PLAYER_RESOURCES);
    v.pip_fire_anim_frames = ptr<const int32_t>(RID_A_OGIEN);

    // SIM1B (2026-08-13; building_tick machinery). _G_LLM_STRAT_CUR_BUILDING is a
    // POINTER-VALUED global (map_object_building*), same resolve-once-bind-both shape as
    // cur_unit_live above.
    building *cur_building_live = translate_slot(*ptr<building *>(RID_STRAT_CUR_BUILDING));
    v.cur_building              = cur_building_live;

    // SIM1B building_tick promotion-oracle session (2026-08-13, G19). The three original building
    // dispatch tables -- see sim_state.h's bldg_state_fn/bldg_done_fn/bldg_tick2_fn for shape/index.
    v.bldg_state_funcs = ptr<const bldg_state_fn>(RID_STRAT_BLDG_STATE_FUNCS);
    v.bldg_done_funcs  = ptr<const bldg_done_fn>(RID_STRAT_BLDG_DONE_FUNCS);
    v.bldg_tick2_funcs = ptr<const bldg_tick2_fn>(RID_STRAT_BLDG_TICK2_FUNCS);

    v.death_anim_table = ptr<const int32_t>(RID_STRAT_DEATH_ANIM_TABLE);

    v.ui_panel_mode = ptr<const int32_t>(RID_STRAT_UI_PANEL_MODE);
    v.ui_panel_page = ptr<const int32_t>(RID_STRAT_UI_PANEL_PAGE);
    // SIM1F (2026-08-18): game_SetEvent's read-only inputs.
    v.ui_event_defer_active      = ptr<const int32_t>(RID_STRAT_UI_EVENT_DEFER_ACTIVE);
    v.ui_bldg_tab_select_blocked = ptr<const uint8_t>(RID_STRAT_UI_BLDG_TAB_SELECT_BLOCKED);
    v.ui_panel_fallback_table    = ptr<const int32_t>(RID_STRAT_UI_PANEL_FALLBACK_TABLE);

    // SIM1B (2026-08-13; building_tick machinery). Read-only sibling of
    // sim_store::power_stats_at() below -- same RID, same live pointer resolution.
    v.power_stats = ptr<const power_stats>(RID_STRAT_POWER_STATS);

    // SIM1D (2026-08-13). See sim_state.h's prod_shuttle_slot/dir8_offset/storage_stats
    // alias comments.
    v.prod_shuttle_slots = ptr<const prod_shuttle_slot>(RID_PROD_SHUTTLE_SLOTS);
    v.dir8_offsets       = ptr<const dir8_offset>(RID_STRAT_DIR8_OFFSET_TABLE);
    v.storage_stats      = ptr<const storage_stats>(RID_STRAT_STORAGE_STATS);

    // SIM1D (2026-08-14). See sim_state.h's comments above.
    v.system_define_index_base          = ptr<const int32_t>(RID_SYSTEM);
    v.shuttle_board_activity_bump       = ptr<const double>(RID_PROD_SHUTTLE_BOARD_ACTIVITY_BUMP);
    v.shuttle_duration_mult_dead_branch = ptr<const double>(RID_PROD_SHUTTLE_DURATION_MULT_DEAD_BRANCH);

    // SIM1D / SIM1E opening (2026-08-14). See sim_state.h's comment above.
    v.unit_death_hq_energy_credit = ptr<const double>(RID_STRAT_UNIT_DEATH_HQ_ENERGY_CREDIT);

    v.dir_step_offsets = ptr<const dir_step_offset>(RID_STRAT_DIR_STEP_OFFSET_TABLE);
    v.dir_remap_table  = ptr<const dir_remap_row>(RID_STRAT_DIR_REMAP_TABLE);
    v.landing_spots    = ptr<const landing_spot>(RID_STRAT_LANDING_SPOTS);

    // SIM1E (2026-08-15). See sim_state.h's comments above. NOTE debris_scale_divisor
    // is a FLOAT (4 bytes) while every other constant in this block is a double -- that asymmetry
    // is measured, not a typo; see its comment.
    v.unit_lost_feedback_cooldown = ptr<const double>(RID_STRAT_UNIT_LOST_FEEDBACK_COOLDOWN);
    v.facing_trig_table           = ptr<const facing_trig>(RID_STRAT_FACING_TRIG_TABLE);
    v.bldg_main_base_damage_mult  = ptr<const double>(RID_STRAT_BLDG_MAIN_BASE_DAMAGE_MULT);
    v.bldg_lost_feedback_cooldown_mother =
        ptr<const double>(RID_STRAT_BLDG_LOST_FEEDBACK_COOLDOWN_MOTHER);
    v.bldg_lost_feedback_cooldown_other =
        ptr<const double>(RID_STRAT_BLDG_LOST_FEEDBACK_COOLDOWN_OTHER);
    v.debris_scale_divisor  = ptr<const float>(RID_FX_DEBRIS_SCALE_DIVISOR);
    v.projectile_dist_scale = ptr<const double>(RID_STRAT_PROJECTILE_DIST_SCALE);
    v.projectile_dir_y_sign = ptr<const double>(RID_STRAT_PROJECTILE_DIR_Y_SIGN);

    // SIM1D (2026-08-14; opening SIM1E). _G_LLM_STRAT_CUR_PROJECTILE is a
    // POINTER-VALUED global (llm_strat_projectile*), same resolve-once-bind-both shape as
    // cur_unit_live/cur_building_live above.
    projectile *cur_projectile_live = translate_slot(*ptr<projectile *>(RID_STRAT_CUR_PROJECTILE));
    v.cur_projectile                = cur_projectile_live;
    v.projectile_pool               = ptr<const projectile>(RID_STRAT_PROJECTILE_POOL);

    // SIM1E third batch (2026-08-14). _G_LLM_STRAT_CUR_FX_ANIM is a POINTER-VALUED global
    // (llm_strat_fx_anim*), same resolve-once-bind-both shape as cur_projectile_live above.
    fx_anim *cur_fx_anim_live = translate_slot(*ptr<fx_anim *>(RID_STRAT_CUR_FX_ANIM));
    v.cur_fx_anim             = cur_fx_anim_live;
    v.fx_anim_pool            = ptr<const fx_anim>(RID_STRAT_FX_ANIMS);
    v.a_dym_pojazd            = ptr<const int32_t>(RID_A_DYM_POJAZD);
    v.dmg_smoke_level_scale   = ptr<const double>(RID_STRAT_DMG_SMOKE_LEVEL_SCALE);

    // SIM1E: ONE region, TWO bindings. _G_LLM_CAM_JUMP_QUEUE's first 30 entries are
    // llm_vec2i offsets and the 30 that follow are raw doubles -- see sim_state.h's cam_jump_offset
    // alias for the measurement. Resolved here rather than inline in the ctor call below because
    // that argument list is parsed POSITIONALLY by the translation lint, and a comment
    // containing a comma inside it is counted as extra arguments (which is exactly what happened
    // the first time this was written).
    cam_jump_offset *cam_jump_offsets_live = ptr<cam_jump_offset>(RID_CAM_JUMP_QUEUE);
    double          *cam_jump_scales_live =
        reinterpret_cast<double *>(cam_jump_offsets_live + CAM_JUMP_QUEUE_SLOTS);

    // SIM1E fog/sight family (2026-08-16). See sim_state.h's comments above.
    v.sight_area[0] = ptr<const map_t_tile_coord>(RID_SIGHT_AREA_1);
    v.sight_area[1] = ptr<const map_t_tile_coord>(RID_SIGHT_AREA_2);
    v.sight_area[2] = ptr<const map_t_tile_coord>(RID_SIGHT_AREA_3);
    v.sight_area[3] = ptr<const map_t_tile_coord>(RID_SIGHT_AREA_4);
    v.sight_area[4] = ptr<const map_t_tile_coord>(RID_SIGHT_AREA_5);
    v.sight_area[5] = ptr<const map_t_tile_coord>(RID_SIGHT_AREA_6);
    v.sight_area[6] = ptr<const map_t_tile_coord>(RID_SIGHT_AREA_7);
    v.sight_area[7] = ptr<const map_t_tile_coord>(RID_SIGHT_AREA_8);
    v.sight_area[8] = ptr<const map_t_tile_coord>(RID_SIGHT_AREA_9);
    v.sight_area[9] = ptr<const map_t_tile_coord>(RID_SIGHT_AREA_10);
    v.is_human      = ptr<const uint32_t>(RID_IS_HUMAN);

    // SIM1E sim_step slice (2026-08-16). Second, independent, read-only bindings of regions other
    // modules already bind mutably/elsewhere -- see sim_state.h's comments above.
    v.ai_enabled      = ptr<const int32_t>(RID_STRAT_AI_ENABLED);
    v.game_time_delta = ptr<const double>(RID_GAME_TIME_DELTA);

    // Seven boot-constant doubles -- see sim_state.h's comment above for the RID-name caveat (they
    // were already named in Ghidra under their real _G_LLM_STRAT_* names, not the bare names this
    // slice's manifest entries proposed).
    v.subtick_a_period      = ptr<const double>(RID_STRAT_SUBTICK_A_PERIOD);
    v.subtick_a_period_pos  = ptr<const double>(RID_STRAT_SUBTICK_A_PERIOD_POS);
    v.subtick_a_period_neg  = ptr<const double>(RID_STRAT_SUBTICK_A_PERIOD_NEG);
    v.subtick_b_period      = ptr<const double>(RID_STRAT_SUBTICK_B_PERIOD);
    v.subtick_b_period_pos  = ptr<const double>(RID_STRAT_SUBTICK_B_PERIOD_POS);
    v.subtick_b_period_neg  = ptr<const double>(RID_STRAT_SUBTICK_B_PERIOD_NEG);
    v.prod_check_period_neg = ptr<const double>(RID_STRAT_PROD_CHECK_PERIOD_NEG);

    v.resource_decay_rate         = ptr<const double>(RID_STRAT_RESOURCE_DECAY_RATE);
    v.game_speed_factor_max       = ptr<const double>(RID_GAME_SPEED_FACTOR_MAX);
    v.game_speed_factor_step_up   = ptr<const double>(RID_GAME_SPEED_FACTOR_STEP_UP);
    v.game_speed_factor_min       = ptr<const double>(RID_GAME_SPEED_FACTOR_MIN);
    v.game_speed_factor_step_down = ptr<const double>(RID_GAME_SPEED_FACTOR_STEP_DOWN);
    v.game_speed_player_factor    = ptr<const double>(RID_GAME_SPEED_PLAYER_FACTOR);

    // SIM1F (llm_strat_spawn_invasion_force, the LAST SIM function). The three AI
    // think-cycle periods, the per-player clock stagger, the start-unit budget, and the read side of
    // the active-player high-water counter. No writer for the first four -- read-only.
    v.ai_move_period            = ptr<const float>(RID_STRAT_AI_MOVE_PERIOD);
    v.ai_tactic_period          = ptr<const float>(RID_STRAT_AI_TACTIC_PERIOD);
    v.ai_strategy_period        = ptr<const float>(RID_STRAT_AI_STRATEGY_PERIOD);
    v.ai_invasion_clock_stagger = ptr<const double>(RID_STRAT_AI_INVASION_CLOCK_STAGGER);
    v.ai_cfg_start_units        = ptr<const int32_t>(RID_STRAT_AI_CFG_START_UNITS);
    v.ai_active_player_count    = ptr<const int32_t>(RID_STRAT_AI_ACTIVE_PLAYER_COUNT);

    // SIM1E fog/sight family (2026-08-16). fog_of_war is a second, independent MF_VIEW binding of an
    // already-matrix-discovered region (RID_FOG_OF_WAR) -- resolved here as a raw uint8_t* so
    // sim_store's two accessors can index the visible_by_count/discovered halves the way the
    // assembly actually does (tile-major, not the committed struct's player-major field order; see
    // sim_state.h's fog_visible_by_count_at() comment). discovered starts 0x80000 bytes in (measured
    // struct layout: visible_by_count is 0x80000 bytes, discovered immediately follows).
    uint8_t *fog_of_war_base           = ptr<uint8_t>(RID_FOG_OF_WAR);
    uint8_t *fog_visible_by_count_live = fog_of_war_base;
    uint8_t *fog_discovered_live       = fog_of_war_base + 0x80000;

    // SIM1E sim_step slice (2026-08-16). The four ambient POINTER-VALUED globals' SLOTS themselves
    // (not their pointed-to record) -- see sim_state.h's set_cur_unit_ptr()/etc. comment. Resolved
    // as T** (the slot's address), distinct from cur_unit_live/cur_building_live/etc. above (which
    // dereference the SAME slots once for the read-only/consumer view members).
    unit       **cur_unit_slot_live       = ptr<unit *>(RID_STRAT_CUR_UNIT);
    building   **cur_building_slot_live   = ptr<building *>(RID_STRAT_CUR_BUILDING);
    projectile **cur_projectile_slot_live = ptr<projectile *>(RID_STRAT_CUR_PROJECTILE);
    fx_anim    **cur_fx_anim_slot_live    = ptr<fx_anim *>(RID_STRAT_CUR_FX_ANIM);

    // SIM-RESID-IF re-close (2026-08-31). See the sim_view block for what each one is.
    v.advisor_due_delay            = ptr<const double>(RID_STRAT_ADVISOR_DUE_DELAY);
    v.advisor_staff_threshold      = ptr<const double>(RID_STRAT_ADVISOR_STAFF_THRESHOLD);
    v.advisor_interval             = ptr<const double>(RID_STRAT_ADVISOR_INTERVAL);
    v.empty_name_str               = ptr<const char>(RID_STRAT_EMPTY_NAME_STR);
    v.storage_stats_init_delay     = ptr<const double>(RID_STRAT_STORAGE_STATS_INIT_DELAY);
    v.win_w                        = ptr<const int32_t>(RID_G_WIN_W);
    v.win_h                        = ptr<const int32_t>(RID_G_WIN_H);
    v.player_desc_slots            = ptr<const player_desc>(RID_PLAYERS);
    v.net_local_player_slot        = ptr<const uint16_t>(RID_NET_LOCAL_PLAYER_SLOT);
    v.net_lobby_scan_host_count    = ptr<const uint32_t>(RID_NET_LOBBY_SCAN_HOST_COUNT);
    v.lockstep_session_adapt_delay = ptr<const double>(RID_STRAT_LOCKSTEP_SESSION_ADAPT_DELAY);

    // SIM-RESID-IF reopen (2026-08-31). Two read-only constants, one blocked translation each --
    // see the sim_view block for why ai_clock_stagger_fraction is NOT ai_invasion_clock_stagger.
    v.ai_clock_stagger_fraction  = ptr<const double>(RID_STRAT_AI_CLOCK_STAGGER_FRACTION);
    v.invention_name_placeholder = ptr<const char>(RID_STRAT_INVENTION_NAME_PLACEHOLDER);

    // SIM-RESID-F (2026-09-01). The six read-only regions the tutorial pair reads; none is written
    // anywhere in the sim closure, so all six are view-only.
    v.tutorial_steps           = ptr<const tutorial_step_record>(RID_TUTORIAL_STEPS);
    v.tutorial_step_count      = ptr<const int32_t>(RID_TUTORIAL_STEP_COUNT);
    v.tutorial_colors_rgb      = ptr<const int32_t>(RID_TUTORIAL_COLORS_RGB);
    v.ui_menu_async_callback_b = ptr<const void *const>(RID_UI_MENU_ASYNC_CALLBACK_B);
    v.ui_screen_main_menu_id   = ptr<const int32_t>(RID_UI_SCREEN_MAIN_MENU_ID);
    v.ui_screen_racebck_id     = ptr<const int32_t>(RID_UI_SCREEN_RACEBCK_ID);

    // The write half. Same region ids as the read half above, deliberately: the two must address
    // one copy of the state, which is what statetest's rebase consumer checks.
    sim_store own(ptr<unit>(RID_UNITS), ptr<player_data>(RID_PLAYER_DATA),
                  ptr<uint8_t>(RID_STRAT_ORDER_SEQ_ID_BY_PLAYER), ptr<building>(RID_BUILDINGS),
                  ptr<turret>(RID_TURRETS), ptr<production>(RID_PRODUCTIONS), ptr<lab>(RID_LABS),
                  ptr<order>(RID_STRAT_ORDER_QUEUE), ptr<int32_t>(RID_STRAT_ORDER_QUEUE_COUNT),
                  ptr<double>(RID_STRAT_LOCKSTEP_HORIZON),
                  ptr<uint8_t>(RID_NET_LOCKSTEP_STATUS_FLAGS), ptr<wchar_t>(RID_G_TEXT_TMP),
                  ptr<int32_t>(RID_STRAT_AI_ENGAGE_CANDIDATE_SCRATCH_COUNT), cur_unit_live,
                  ptr<double>(RID_STRAT_TICK_BUDGET), ptr<int32_t>(RID_STRAT_STATE_LOOP_GUARD),
                  ptr<ctrl_group>(RID_STRAT_CTRL_GROUPS), ptr<tile_object>(RID_TILE_OBJECTS),
                  ptr<uint8_t>(RID_PASSABLE), ptr<pop_stats>(RID_STRAT_POP_STATS),
                  ptr<soldier>(RID_STRAT_SOLDIERS),
                  ptr<housing_stats>(RID_STRAT_UNIT_HOUSING_STATS),
                  ptr<uint8_t>(RID_STRAT_PATH_SLOT_FLAGS),
                  ptr<int32_t>(RID_STRAT_PATH_FREE_SLOT_COUNT),
                  ptr<uint16_t>(RID_CLICK_SELECT_TARGET_ID), ptr<player_profile>(RID_STRAT_PLAYERS),
                  ptr<map_geom>(RID_GENERAL),
                  ptr<uint8_t>(RID_STRAT_BLDG_ENCLOSURE_SCRATCH),
                  ptr<power_stats>(RID_STRAT_POWER_STATS), cur_building_live,
                  ptr<int32_t>(RID_STRAT_CAM_PAN_TARGET_COL),
                  ptr<int32_t>(RID_STRAT_CAM_PAN_TARGET_ROW),
                  ptr<double>(RID_STRAT_PLANET_MOTHER_LOST_TIME), ptr<mine>(RID_MINES),
                  ptr<unit_storage>(RID_UNIT_STORAGE),
                  ptr<uint16_t>(RID_STRAT_UI_SELECTED_BLDG_INDEX),
                  ptr<prod_shuttle_slot>(RID_PROD_SHUTTLE_SLOTS),
                  ptr<uint8_t>(RID_STRAT_PROD_COMPLETE_THROTTLE), cur_projectile_live,
                  ptr<projectile>(RID_STRAT_PROJECTILE_POOL), cur_fx_anim_live,
                  ptr<fx_anim>(RID_STRAT_FX_ANIMS), cam_jump_offsets_live, cam_jump_scales_live,
                  ptr<int32_t>(RID_CAM_JUMP_QUEUE_COUNT), fog_visible_by_count_live,
                  fog_discovered_live, ptr<int32_t>(RID_G_TMP_PLAYER), ptr<int32_t>(RID_G_TMP_X),
                  ptr<int32_t>(RID_G_TMP_Y), ptr<int32_t>(RID_G_TMP_SIGHT),
                  ptr<uint8_t>(RID_G_OTHER_PLAYERS_MASK), ptr<uint16_t>(RID_STRAT_CUR_PLAYER),
                  ptr<uint16_t>(RID_STRAT_CUR_INDEX), cur_unit_slot_live, cur_building_slot_live,
                  cur_projectile_slot_live, cur_fx_anim_slot_live,
                  ptr<storage_stats>(RID_STRAT_STORAGE_STATS),
                  ptr<double>(RID_GAME_SPEED_PLAYER_FACTOR), ptr<uint32_t>(RID_STRAT_RNG_STATE),
                  ptr<uint32_t>(RID_IS_HUMAN), ptr<uint8_t>(RID_GAME_HUMAN_PLAYER_MASK),
                  ptr<uint8_t>(RID_PLAYERS), ptr<player_progress>(RID_PROGRESS),
                  ptr<double>(RID_GAME_SPEED), ptr<double>(RID_PLANET_TIME),
                  ptr<int32_t>(RID_AVAILABLEBUILDINGS), ptr<int32_t>(RID_AVAILABLEPROJECTS),
                  ptr<cfg_unit>(RID_UNIT), ptr<cfg_weapon>(RID_WEAPON), ptr<int32_t>(RID_WIDTH),
                  ptr<int32_t>(RID_HEIGHT), ptr<double>(RID_STRAT_INVASION_TIME),
                  ptr<int32_t>(RID_PLAYER_RESOURCES), ptr<int32_t>(RID_G_PLANET_STATUS),
                  ptr<llm_map_region_cell>(RID_MAP_REGION_GRID),
                  ptr<llm_map_bfs_entry>(RID_MAP_BFS_QUEUE_REGIONSPLIT),
                  ptr<int32_t>(RID_STRAT_FOREIGN_BLDG_EVENT_PENDING),
                  ptr<double>(RID_STRAT_INVASION_ALERT_TIME),
                  ptr<landing_spot>(RID_STRAT_LANDING_SPOTS),
                  // SIM1F (2026-08-18): game_SetEvent's UI-panel/event-queue/chat state.
                  ptr<int32_t>(RID_STRAT_UI_PANEL_MODE), ptr<int32_t>(RID_STRAT_UI_PANEL_PAGE),
                  ptr<int32_t>(RID_STRAT_UI_PANEL_SWITCH_PENDING),
                  ptr<int32_t>(RID_STRAT_UI_BLDG_PANEL_REFRESH_PENDING),
                  ptr<int32_t>(RID_STRAT_UI_UNIT_PANEL_REFRESH_PENDING),
                  ptr<int32_t>(RID_STRAT_UI_MAINPANEL_REFRESH_PENDING),
                  ptr<int32_t>(RID_STRAT_UI_MAINPANEL_TAB_INDEX),
                  ptr<int32_t>(RID_STRAT_UI_UNIT_TAB_TOGGLE),
                  ptr<int32_t>(RID_STRAT_UI_VIEW_RESIZE_PENDING),
                  ptr<int32_t>(RID_STRAT_UI_EVENT_QUEUE_POS),
                  ptr<int32_t>(RID_STRAT_UI_EVENT_QUEUE_BUF), ptr<int32_t>(RID_CHAT_INPUT_ACTIVE),
                  ptr<int32_t>(RID_CHAT_INPUT_LEN), ptr<int32_t>(RID_CHAT_INPUT_CURSOR),
                  ptr<char>(RID_STRAT_CHAT_INPUT_LINE),
                  // SIM1F (2026-08-18): llm_strat_player_presence_lost's mutable writes.
                  ptr<double>(RID_STRAT_DEBUG_RESOURCE_YIELD_CUT),
                  ptr<int32_t>(RID_GAME_SESSION_MODE),
                  // SIM1F: llm_strat_spawn_invasion_force's active-player-count bump.
                  ptr<int32_t>(RID_STRAT_AI_ACTIVE_PLAYER_COUNT),
                  // SIM1-G1 (2026-08-19): the move state handlers' path buffers + group-move scratch.
                  ptr<path_waypoint>(RID_STRAT_PATH_BUFFERS),
                  ptr<group_scratch_member>(RID_STRAT_GROUP_MOVE_SCRATCH),
                  // SIM1-G1 (2026-08-20): unit_group_step_ground/_plane's pathfinder-mode flag.
                  ptr<int32_t>(RID_STRAT_PATHFINDER_AIR_MODE_FLAG),
                  // SIM1-G1 (2026-08-20): group_move_order_commit's mutable writes.
                  ptr<route_step>(RID_STRAT_GROUP_ROUTE_STEPS), ptr<group_member>(RID_STRAT_GROUP_MEMBERS),
                  ptr<uint32_t>(RID_STRAT_PATH_WRAP_MASK), ptr<int32_t>(RID_STRAT_GROUP_ORDER_GOAL_X),
                  ptr<int32_t>(RID_STRAT_GROUP_ORDER_GOAL_Y), ptr<int32_t>(RID_STRAT_GROUP_ANCHOR_X),
                  ptr<int32_t>(RID_STRAT_GROUP_ANCHOR_Y), ptr<int32_t>(RID_STRAT_GROUP_ORDER_OWNER),
                  ptr<int32_t>(RID_STRAT_GROUP_MEMBER_COUNT),
                  // SIM1-G1 tail slice (2026-08-20): group_plan_formation_positions's mutable write.
                  ptr<uint8_t>(RID_STRAT_GROUP_MEMBER_TILE),
                  // SIM1-G-PREP (2026-08-20): the batch-G write set. SAME ORDER as the ctor's
                  // parameter list in sim_state.h and as its member-init list -- the three lists are
                  // one sequence, and nothing but that agreement binds these correctly, since most
                  // of them are same-typed pointers. (a) group-move working state.
                  ptr<int32_t>(RID_STRAT_GROUP_CENTROID_X),
                  ptr<int32_t>(RID_STRAT_GROUP_CENTROID_Y),
                  ptr<int32_t>(RID_STRAT_GROUP_PATH_BUILD_IDX),
                  // (b) map-region routing scratch.
                  ptr<uint8_t>(RID_MAP_REGION_ROUTE_CAND_SCRATCH),
                  ptr<uint32_t>(RID_MAP_REGION_FLOOD_TILE_QUEUE),
                  ptr<llm_map_region *>(RID_MAP_REGION_ROUTE_BFS_QUEUE),
                  // (c) the invasion-pacing completion counter.
                  ptr<int32_t>(RID_STRAT_BLDG_COMPLETION_ACCUM),
                  // (d) the unit-queue tile search.
                  ptr<uint32_t>(RID_UNITQ_CUR_TILE), ptr<int32_t>(RID_UNITQ_CLOSED_COUNT),
                  ptr<int32_t>(RID_UNITQ_ITER), ptr<int32_t>(RID_UNITQ_FRONTIER_COUNT),
                  ptr<unitq_search_node>(RID_UNITQ_CLOSED),
                  ptr<unitq_search_node>(RID_UNITQ_FRONTIER),
                  // (e) the pathtrace working state.
                  ptr<uint32_t>(RID_STRAT_PATHTRACE_COORD_MASK),
                  ptr<uint32_t>(RID_STRAT_PATHTRACE_COL_MASK),
                  ptr<uint32_t>(RID_STRAT_PATHTRACE_ROW_MASK),
                  ptr<uint32_t>(RID_STRAT_PATHTRACE_MAP_W),
                  ptr<uint32_t>(RID_STRAT_PATHTRACE_MAP_H),
                  ptr<uint32_t>(RID_STRAT_PATHTRACE_HALF_W),
                  ptr<uint32_t>(RID_STRAT_PATHTRACE_HALF_H),
                  ptr<uint32_t>(RID_STRAT_PATHTRACE_HALF_W_M1),
                  ptr<uint32_t>(RID_STRAT_PATHTRACE_HALF_H_M1),
                  ptr<int32_t>(RID_STRAT_PATHTRACE_NEG_HALF_W),
                  ptr<int32_t>(RID_STRAT_PATHTRACE_NEG_HALF_H),
                  ptr<uint32_t>(RID_STRAT_PATHTRACE_START_COL),
                  ptr<uint32_t>(RID_STRAT_PATHTRACE_START_ROW),
                  ptr<uint32_t>(RID_STRAT_PATHTRACE_WALK_DIR),
                  ptr<uint32_t>(RID_STRAT_PATHTRACE_GOAL_COL),
                  ptr<uint32_t>(RID_STRAT_PATHTRACE_GOAL_ROW),
                  ptr<uint32_t>(RID_STRAT_PATHTRACE_GOAL_PACKED),
                  ptr<uint32_t>(RID_STRAT_PATHTRACE_APPROACH_DIR),
                  ptr<uint8_t>(RID_STRAT_PATHTRACE_DIRS), ptr<uint16_t>(RID_STRAT_PATHTRACE_POS),
                  ptr<uint32_t>(RID_STRAT_PATHTRACE_BEST_DIR),
                  ptr<int32_t>(RID_STRAT_PATHTRACE_BEST_DIST),
                  ptr<uint32_t>(RID_STRAT_PATHTRACE_FORBID_CELLS),
                  ptr<uint32_t>(RID_STRAT_PATHTRACE_LEN),
                  // SIM1-G2 (2026-08-20). Appended at the tail -- see sim_state.h's matching comment
                  // on the ctor. (f) group-move distance scratch, (g) chase-check result cache.
                  ptr<int32_t>(RID_STRAT_GROUP_MOVE_DIST_REF_X),
                  ptr<int32_t>(RID_STRAT_GROUP_MOVE_DIST_REF_Y),
                  ptr<int32_t>(RID_STRAT_GROUP_MOVE_DIST_HALF_WIDTH),
                  ptr<int32_t>(RID_STRAT_GROUP_MOVE_DIST_HALF_HEIGHT),
                  ptr<int32_t>(RID_STRAT_UNIT_CHASE_RESULT),
                  // SIM1-G3 (2026-08-21). Appended at the tail -- see sim_state.h's
                  // matching comment on the ctor.
                  ptr<squad_formation_anchor_scratch>(RID_STRAT_SQUAD_FORMATION_ANCHOR_SCRATCH),
                  // SIM1-G4 (2026-08-22). Appended at the tail per the same positional-
                  // swap-avoidance precedent as the SIM1-G2/G3 groups above.
                  ptr<map_resources>(RID_RESOURCES),
                  ptr<int32_t>(RID_STRAT_AI_FOREIGN_BLDG_CHANGE_FLAG),
                  ptr<ui_base_marker_coord>(RID_STRAT_UI_BASE_MARKER_COORDS),
                  ptr<char>(RID_STRAT_DMP_PATH_SCRATCH),
                  ptr<double>(RID_CURRENT_GAME_TIME), ptr<double>(RID_LAST_GAME_TIME),
                  ptr<double>(RID_TOTAL_GAME_TIME), ptr<int32_t>(RID_CHEAT_PENALTY_SCORE),
                  ptr<int32_t>(RID_DEBUG_TAP_FLAG), ptr<int32_t>(RID_NET_BW_STAT),
                  ptr<uint8_t>(RID_STRAT_FLOATING_MSG_SUPPRESS_FLAG),
                  ptr<uint8_t>(RID_STRAT_LOCKSTEP_STEP_MULT),
                  ptr<int32_t>(RID_STRAT_PLANET_INT_TABLE),
                  ptr<double>(RID_STRAT_SIM_STEP_INTERVAL),
                  ptr<int32_t>(RID_STRAT_SAVE_MISC_DWORD),
                  ptr<int32_t>(RID_GAME_LAND_NO_START_UNIT_FLAG),
                  ptr<int32_t>(RID_STRAT_OUTER_PLANET_LANDED_FLAG),
                  ptr<int32_t>(RID_STRAT_OUTER_PLANET_LAND_STATE),
                  ptr<uint8_t>(RID_STRAT_PLANET_TRANSITION_STATE),
                  ptr<int32_t>(RID_STRAT_SHOW_UNIT_FLAGS),
                  ptr<uint8_t>(RID_STRAT_RNG_SEED_BYTE),
                  ptr<double>(RID_STRAT_LOCKSTEP_ADAPT_NEXT_TIME),
                  ptr<uint8_t>(RID_CHAT_TARGET_MASK),
                  ptr<uint16_t>(RID_STRAT_PLANET_MAP_PAL4_WHITE),
                  ptr<uint16_t>(RID_STRAT_PLANET_MAP_PAL4_BLACK),
                  ptr<uint16_t>(RID_STRAT_PLANET_MAP_PAL4_MAGENTA),
                  ptr<uint16_t>(RID_STRAT_PLANET_MAP_PAL4_YELLOW),
                  ptr<uint16_t>(RID_STRAT_PLANET_MAP_PAL5_BLACK),
                  ptr<uint16_t>(RID_STRAT_PLANET_MAP_PAL5_MAGENTA),
                  ptr<uint16_t>(RID_STRAT_PLANET_MAP_PAL5_GREEN),
                  ptr<uint16_t>(RID_STRAT_PLANET_MAP_PAL5_RED),
                  ptr<uint16_t>(RID_STRAT_PLANET_MAP_PAL5_BLUE),
                  ptr<int32_t>(RID_SQUAD_BB_SCAN_PLAYER),
                  ptr<int32_t>(RID_SQUAD_BB_TARGET_OWNER),
                  ptr<int32_t>(RID_SQUAD_BB_TARGET_BUILDING_ID),
                  ptr<int32_t>(RID_SQUAD_BB_TARGET_ENERGY_PCT),
                  ptr<int32_t>(RID_SQUAD_BB_TARGET_BUILDING_IDX),
                  ptr<squad_status_slot>(RID_SQUAD_STATUS),
                  ptr<int32_t>(RID_SQUAD_STATUS_COUNT),
                  ptr<int32_t>(RID_STRAT_ORDER_PENDING_COUNT),
                  ptr<int32_t>(RID_STRAT_ORDER_STAGING_COUNT),
                  ptr<double>(RID_NET_PEER_HORIZON), ptr<double>(RID_NET_PEER_HORIZON_PENDING),
                  ptr<int32_t>(RID_BUILD_PLACEMENT_ID),
                  ptr<uint32_t>(RID_BLDG_FOOTPRINT_PASSABLE_SAVE_SLOT),
                  ptr<int32_t>(RID_STRAT_ADVISOR_PHASE),
                  ptr<int32_t>(RID_STRAT_FLOATING_MSG_QUEUE_ACTIVE),
                  ptr<job_result_entry>(RID_STRAT_PATH_JOB_RESULT_TABLE),
                  ptr<uint8_t>(RID_GAME_MODE), ptr<int32_t>(RID_TUTORIAL_BUILD_TYPE_FILTER),
                  ptr<uint32_t>(RID_TUTORIAL_FORCED_BLDG_SELECTION),
                  ptr<uint8_t>(RID_TUTORIAL_HQ_ATTACK_SCENARIO_DONE),
                  ptr<int32_t>(RID_TUTORIAL_PENDING_BUILD_PLACEMENT_ID),
                  ptr<int32_t>(RID_TUTORIAL_RMB_LIMIT_FLAG),
                  ptr<int32_t>(RID_TUTORIAL_RESET_SLOT_0050A678),
                  ptr<int32_t>(RID_DLG_STATE_FLAGS), ptr<uint8_t>(RID_PLAYER_CONTROL_MASK),
                  ptr<int32_t>(RID_GFX_UI_COLORS_RGB),
                  ptr<int32_t>(RID_UI_RACE_SEL_PENDING_GFX_IDX),
                  ptr<ui_fade_transition_state>(RID_UI_FADE_TRANSITION), ptr<void *>(RID_UI_MENU_ASYNC_CALLBACK_A), ptr<widget_list *>(RID_UI_MENU_WIDGET_LIST),
                  ptr<ui_widget>(RID_UI_TUTORIAL_HINT_WIDGET),
                  ptr<ui_widget>(RID_UI_WGT_TUTORIAL_WELCOME),
                  ptr<ui_widget>(RID_UI_WGT_MENU_SCREEN_TITLE),
                  ptr<ui_widget>(RID_UI_OUTCOME_DLG_TITLE_WIDGET),
                  ptr<ui_widget>(RID_UI_OUTCOME_DLG_MESSAGE_WIDGET),
                  ptr<ui_widget>(RID_UI_WGT_FRAME_MENU_PANEL),
                  ptr<uint8_t>(RID_STRAT_INJECTED_MAP_PLANET_SLOT),
                  ptr<int32_t>(RID_VIEW_SIZE_MODE), ptr<int32_t>(RID_VIEW_SIZE_MODE_SAVE),
                  ptr<map_header>(RID_CURRENT_MAP_DATA),
                  // SIM-RESID-IF re-close (2026-08-31): the write halves. Same region ids as the
                  // read half above, deliberately -- one copy of the state.
                  ptr<int32_t>(RID_CURRENTSYSTEM), ptr<int32_t>(RID_G_PLANET_INDEX),
                  ptr<double>(RID_STRAT_GAME_CLOCK), ptr<double>(RID_GAME_TIME_DELTA),
                  ptr<int16_t>(RID_PLAYERSIDE), ptr<uint16_t>(RID_STRAT_LOCAL_PLAYER_SLOT),
                  ptr<int32_t>(RID_STRAT_PLAYER_RACE), ptr<int32_t>(RID_STRAT_SIM_ACTIVE),
                  ptr<int32_t>(RID_STRAT_MP_ALLY_VICTORY_RULE_FLAG),
                  ptr<int32_t>(RID_STRAT_SYSTEM_LOST_MSG_SHOWN_FLAG),
                  ptr<uint8_t>(RID_STRAT_UI_BLDG_TAB_SELECT_BLOCKED),
                  ptr<uint8_t>(RID_STRAT_BLDG_COMPLETION_SLOT_COUNT),
                  ptr<uint32_t>(RID_WIDTH_M), ptr<uint32_t>(RID_HEIGHT_M),
                  ptr<int32_t>(RID_GAME_TUTORIAL_STEP), ptr<int32_t>(RID_STRAT_AI_ENABLED),
                  ptr<int32_t>(RID_STRAT_UI_PANEL_FALLBACK_TABLE), ptr<cfg_planet>(RID_PLANETS),
                  ptr<cfg_invention>(RID_PROGRESS_00E162E4), ptr<cfg_project>(RID_PROJECTS),
                  ptr<cfg_upgrade>(RID_UPGRADES), ptr<int32_t>(RID_SYSTEM),
                  ptr<double>(RID_STRAT_ADVISOR_NEXT_TIME), ptr<uint8_t>(RID_MOUSE_BUTTONS_PREV),
                  ptr<char>(RID_STRAT_LAND_DMP_PATH_SCRATCH), fog_of_war_base,
                  // SIM-RESID-IF reopen (2026-08-31): map_FillDefaults' three remaining
                  // whole-region fill destinations, plus the mutable half of the table its
                  // PRESERVE-BUG store lands in. See sim_state.h for the shape argument.
                  ptr<uint8_t>(RID_MAP_OBJECTS), ptr<uint8_t>(RID_MAP_OBJECT_TABLE),
                  ptr<uint8_t>(RID_MAP_HALFRES_GRID), ptr<int32_t>(RID_STRAT_DEATH_ANIM_TABLE),
                  // SIM-RESID-C (2026-08-31): llm_strat_scenario_planet_clone's widening
                  // destination -- an address escape, like land_dmp_scratch above it.
                  ptr<wchar_t>(RID_STRAT_SCENARIO_PLANET_NAME_W),
                  // The write half of sim_view::text_ptrs, same RID -- llm_strat_scenario_planet_clone
                  // is the domain's only writer of the string table.
                  ptr<text_slot>(RID_G_TEXT_PTRS),
                  // SIM-RESID-F (2026-09-01): the mutable half of sim_view::player_desc_slots,
                  // same RID -- llm_game_start_tutorial writes the fixed 1v1 slot descriptors.
                  ptr<player_desc>(RID_PLAYERS),
                  // LT1 lib_trans (2026-09-02): the map-region pool slots (LT1C -- the head slots
                  // are pointer VALUES at fixed VAs, so bind pointer-to-slot), the menu-teardown
                  // writes and the ambient table escape (LT1D).
                  reinterpret_cast<llm_map_region **>(ptr<void *>(RID_MAP_REGION_LIST_HEAD)),
                  reinterpret_cast<llm_map_region **>(ptr<void *>(RID_MAP_REGION_POOL_FREE_HEAD)),
                  reinterpret_cast<llm_map_region **>(ptr<void *>(RID_MAP_REGION_BY_INDEX)),
                  ptr<int32_t>(RID_COUNTER), ptr<int32_t>(RID_G_LAST_MAP_INDEX),
                  ptr<void *>(RID_UI_MENU_ASYNC_CALLBACK), ptr<uint8_t>(RID_UI_MENU_STATE),
                  ptr<int32_t>(RID_GAME_QUIT_TEARDOWN_FORCED_FLAG),
                  ptr<uint8_t>(RID_SND_AMBIENT_BY_PLANET),
                  // LT1C c4 (2026-09-02): recompute_cell_grid's two island outputs.
                  ptr<uint8_t>(RID_STRAT_BLDG_CELL_GRID),
                  ptr<int32_t>(RID_STRAT_BLDG_CELL_GRID_ROW_SHIFT),
                  // SIM1-H (2026-09-10): the write halves of Building[] and the region wrap mask.
                  ptr<cfg_building>(RID_BUILDING),
                  ptr<uint32_t>(RID_MAP_REGION_COORD_WRAP_MASK),
                  // SIM1-H wave 2 (2026-09-10): the nav-region pipeline's write set.
                  ptr<uint32_t>(RID_MAP_REGION_MERGE_THRESHOLD),
                  ptr<uint8_t>(RID_MAP_REGION_ROUTE_STEP_DELTAS),
                  ptr<uint8_t>(RID_MAP_REGION_ROUTE_STEP_DELTA_WRAP),
                  ptr<llm_map_bfs_entry>(RID_PATH));

    // SB-BIND T2. Set after construction rather than threaded through the 35-argument ctor above;
    // `state()` is a friend, which is what makes this reachable. It MUST match v.caps -- the view
    // and the store index the same rosters, and a disagreement between them would put a read and a
    // write on the same (player, slot) into different records.
    own.caps_ = v.caps;

    return sim_state{v, own};
}

// ---- SIM1-DISPATCH (2026-08-22): the two state tables, for the registrar only -------------------
//
// See sim_state.h's sim_state_tables comment for why this is a free binder and not a sim_store
// accessor. Same ST2/Law 3 posture as state() above: live_base, re-resolved per call, nothing
// cached and nothing static. The sizes are read from the registry rather than from the generated
// table, so the two can DISAGREE and the caller can refuse -- which is the point of returning them.
sim_state_tables writable_state_tables() {
    sim_state_tables t{};
    t.unit       = ptr<unit_state_fn>(RID_STRAT_UNIT_STATE_FUNCS);
    t.bldg       = ptr<bldg_state_fn>(RID_STRAT_BLDG_STATE_FUNCS);
    t.unit_slots = static_cast<int32_t>(live_size(RID_STRAT_UNIT_STATE_FUNCS) / sizeof(unit_state_fn));
    t.bldg_slots = static_cast<int32_t>(live_size(RID_STRAT_BLDG_STATE_FUNCS) / sizeof(bldg_state_fn));
    return t;
}

// ---- SIM1-BLDGCB (2026-08-23): the two per-building-type callback tables + the cfg they scan -----
//
// THE DRIFT ASSERTS ARE THE POINT OF PUTTING THIS HERE. The generated header carries three
// constants read out of the original setters' own listings -- the address the type byte is read
// from, the cfg record stride, and each table's base. Our registrar reproduces that scan through a
// C++ struct and the region registry, so those two descriptions of the same memory must agree.
// Below they are made to agree at COMPILE time: a cfg record that gains a field, a `type` field that
// moves, or a region that is re-based without the extraction being re-run is a build error here
// rather than a registrar quietly matching on the wrong byte and filling the tables for the wrong
// buildings. This is the only file under libmh/sim/ that may name the registry, so it is also the only
// place these asserts can be written.
// THE STRIDE ASSERT IS CONFIGURATION-FREE and stays in both arms: a record size is a record size,
// and it is the one of the four that can catch a cfg layout change on its own.
static_assert(sizeof(cfg_building) == mh::addr::BLDG_TYPE_CFG_STRIDE,
              "the cfg Building record size disagrees with the stride the setters multiply by");
// The other three compare an ORIGINAL-IMAGE ADDRESS against REGIONS[].base, so they are hosted-only
// (LIB-REF-SPLIT): standalone MH_STOCK_BASE makes that column 0 and all three would fail for a
// configuration reason rather than a drift. Split from the stride assert rather than guarded
// wholesale -- same treatment the hash-slice asserts get in addr/mh_regions.gen.h, and for the same
// reason: losing a check that still works standalone to a guard aimed at one that cannot is a
// silent narrowing of what the build verifies.
#ifndef MH_LIBMH_BUILD
static_assert(mh::addr::BLDG_TYPE_CFG_SCAN_BASE ==
                  REGIONS[RID_BUILDING].base + offsetof(cfg_building, type),
              "the setters' cfg type-byte address is not &Building[0].type as this code assumes");
static_assert(mh::addr::BLDG_DONE_TABLE_BASE == REGIONS[RID_STRAT_BLDG_DONE_FUNCS].base,
              "the extracted done-table base disagrees with the region registry");
static_assert(mh::addr::BLDG_TICK2_TABLE_BASE == REGIONS[RID_STRAT_BLDG_TICK2_FUNCS].base,
              "the extracted tick2-table base disagrees with the region registry");
#endif // !MH_LIBMH_BUILD

sim_bldg_callback_tables writable_bldg_callback_tables() {
    sim_bldg_callback_tables t{};
    t.done  = ptr<bldg_done_fn>(RID_STRAT_BLDG_DONE_FUNCS);
    t.tick2 = ptr<bldg_tick2_fn>(RID_STRAT_BLDG_TICK2_FUNCS);
    t.cfg   = ptr<const cfg_building>(RID_BUILDING);
    t.done_slots =
        static_cast<int32_t>(live_size(RID_STRAT_BLDG_DONE_FUNCS) / sizeof(bldg_done_fn));
    t.tick2_slots =
        static_cast<int32_t>(live_size(RID_STRAT_BLDG_TICK2_FUNCS) / sizeof(bldg_tick2_fn));
    t.cfg_count = static_cast<int32_t>(live_size(RID_BUILDING) / sizeof(cfg_building));
    return t;
}


} // namespace mh::sim

// THE STRIDED-REGION ASSERTIONS, last because they are about the binder above: every region a
// sim body indexes as [row * STRIDE + i] must be longer than one row. Generated from THIS file's
// `ptr<T>(RID_X)` lines crossed with the index expressions in libmh/sim/ -- see the header, and
// tools/lint_strided_regions.py for why only the compiler can evaluate them.
#include "addr/mh_strided_regions.gen.h"
