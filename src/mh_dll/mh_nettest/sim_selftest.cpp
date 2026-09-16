//
// sim_selftest.cpp -- `net_selftest.exe simtest`: the strategic sim's logic over heap buffers
// (RI-SIM / SIM0).
//
// The sibling of `aitest`, and worth more here than it was there. AI0's evidence: an offline oracle
// moved most of a 215-function cluster off the rig entirely and made per-function iteration a
// seconds-long loop instead of a four-minute one. The sim's rig runs are the expensive ones, so the
// same lever applied to 307 functions is the largest single saving available to RI-SIM.
//
// HOW IT WORKS. Every translated sim function is split in two (sim/sim_state.h): a `detail::` body
// that takes its state as a PARAMETER, and a thin wrapper that applies it to `state()`. Production
// binds the parameters from the region registry; this file binds them to `std::vector`s. There is no
// game, no rig, no DLL and no injection -- so the branches a rig cannot reach (an empty roster, a
// cost list that runs to seven entries, a bit index of 20) are ordinary test cases here.
//
// THE FIXTURE IS THE ONE PLACE THIS CAN LIE, so two rules it follows:
//
//   SEED WITH DISTINCT, NON-DEFAULT, NON-SYMMETRIC VALUES. If two fields that a translation could
//   have swapped hold the same number, the swap passes. aitest learned this the expensive way (its
//   promo_add/promo_sub pair is seeded 3 and 7 even though the shipped AI.SCR ships both as 1).
//
//   SIZE BUFFERS WITH PARENTHESES, NOT BRACES. `std::vector<uint32_t> v{128}` is a ONE-element
//   vector holding 128, not 128 elements -- legal overload resolution, invisible to clang-tidy and
//   to /analyze, and it cost aitest a 0xC0000374 on ~60% of runs before anyone found it. Every
//   sizing below is `(n)`.
//
// Set SIMTEST_TRACE=1 to print and flush every check as it is reached -- same reason aitest has it:
// stdout is fully buffered, so a mutant that corrupts the heap takes the whole transcript down and
// the run looks like "no failures" rather than "crashed".
//
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "sim/sim_bldg_alive.h"
#include "sim/sim_group_scratch_centroid.h"
#include "sim/sim_order_enqueue.h"
#include "sim/sim_resource_spend.h"
#include "sim/sim_state.h"
#include "sim/sim_unit_status_bits.h"
#include "sim/sim_unit_weapons.h"

#include "sim_test_support.h" // the fixture + ck/ck_eq, shared with the other sim test TUs

#include <utility>

// The dispatcher's cases live in their own TUs -- 49 switch arms is more than one file wants, and
// separate files are also what let several authors write them at once. Each adds into the same
// mh::sim::test counters, so run_simtest still prints one total.
namespace mh::sim::test {
void run_dispatch_spine_tests();
void run_dispatch_bldg_tests();
void run_dispatch_bldg2_tests();
void run_dispatch_admin_tests();
void run_debug_roll_random_tests();
// SIM1-V (2026-08-23): llm_fx_anim_chain_find_tail -- armed, only 2 calls; pins the cycle guard and the [idx+1] read offset.
void run_fx_anim_chain_find_tail_tests();
// SIM1-V (2026-08-23): llm_fx_anim_seq_cancel -- armed, only 2 calls; pins the live-count budget, the inclusive [start,tail] filter and the double bookkeeping write.
void run_fx_anim_seq_cancel_tests();
// SIM1-V (2026-08-23): llm_strat_order_issue_0xf_adjacent_by_offset -- NOT armed; pins its own torus-masked new_x/new_y before the direct tail call.
void run_order_issue_0xf_adjacent_tests();
// SIM1-V (2026-08-23): llm_strat_storage_get_approach_tile -- 400+ calls on a VACUOUS site (void + 2 out-params, 0 compared regions), so the count was never evidence.
void run_storage_get_approach_tile_tests();
// SIM1-V debt drain (2026-08-23): llm_strat_prod_shuttle_slot_release -- armed, 0 calls (needs a slot-holder destroyed).
void run_prod_shuttle_slot_release_tests();
// SIM1-V debt drain (2026-08-23): llm_storage_cancel_pending_docked -- armed, 0 calls; never reimpl-verified either.
void run_storage_cancel_pending_docked_tests();
// SIM1-V debt drain (2026-08-23): llm_strat_storage_release_door_held_by_unit -- armed, 0 calls; only original callers.
void run_storage_release_door_held_by_unit_tests();
// SIM1-V debt drain (2026-08-23): llm_strat_unit_select_weapon -- armed on its return value, 0 calls; pins the fixed 4/0x64 status.
void run_unit_select_weapon_tests();
// SIM1-V debt drain (2026-08-23): llm_prod_shuttle_slot_bind_default -- armed, 0 calls over 15k+30k (SESSION_SP dispatch cases).
void run_prod_shuttle_slot_bind_default_tests();
// SIM1-V debt drain (2026-08-23): llm_prod_shuttle_load_resource -- PROVEN UNARMABLE from any skirmish rig (G23); sole evidence.
void run_prod_shuttle_load_resource_tests();
// SIM1-V debt drain (2026-08-23): llm_strat_storage_launch_parked_to_orbit -- armed, 0 calls; DO-NOT-ARM caller.
void run_storage_launch_parked_to_orbit_tests();
// SIM1-V debt drain (2026-08-23): llm_strat_bldg_shuttle_slot_is_free -- armed, 0 calls over 15k+30k; UI-only + G23-gated callers.
void run_bldg_shuttle_slot_is_free_tests();
// SIM1-V debt drain (2026-08-23): llm_strat_prod_bind_planet -- armed with 5 siblings, 0 calls.
void run_prod_bind_planet_tests();
// SIM1-V debt drain (2026-08-23): llm_prod_planet_distance_factor -- pure query, armed, 0 calls; DO-NOT-ARM caller.
void run_prod_planet_distance_factor_tests();
// SIM1-V debt drain (2026-08-23): llm_strat_unit_type_group_index -- armed, 0 calls; pins the UNSIGNED-comparison bug fix.
void run_unit_type_group_index_tests();
// SIM1-V debt drain (2026-08-23): llm_prod_shuttle_fuel_apply -- armed, 0 calls over 15k+30k; pins the corrected fuel[7] @+0x6a9.
void run_prod_shuttle_fuel_apply_tests();
// SIM1-V debt drain (2026-08-23): llm_unit_bldg_apply_scaled_damage -- debug-console order 0xf7, armed, 0 calls; *0.5 vs /3.0.
void run_unit_bldg_apply_scaled_damage_tests();
// SIM1-V debt drain (2026-08-23): llm_unit_bldg_apply_lethal_damage -- debug-console order 0xf8, armed, 0 calls.
void run_unit_bldg_apply_lethal_damage_tests();
// SIM1-V debt drain (2026-08-23): llm_map_fow_reveal_full -- order 0xf9, armed, 0 calls.
void run_map_fow_reveal_full_tests();
// SIM1-V debt drain (2026-08-23): llm_bldg_load_resource_tail_noop -- confirmed-vacuous body; the oracle pins the NOTHING.
void run_bldg_load_resource_tail_noop_tests();
void run_bldg_finish_order_tests();
void run_unit_predicates_tests();
void run_unit_passive_engage_tests();
void run_unit_ctrl_group_tests();
void run_unit_target_tracking_tests();
void run_unit_recruit_tests();
void run_unit_update_anim_tests();
void run_unit_set_state_tests();
void run_unit_path_free_slot_tests();
void run_unit_housing_count_tests();
void run_facing24_from_points_tests();
void run_unit_predict_coords_tests();
void run_unit_ctrlgroup_member_tests();
void run_unit_purge_unregistered_tests();
// SIM1A verify pass (2026-08-12): offline coverage for the reviewed leaves the soak cannot reach
void run_unit_soldier_chain_tests();
void run_unit_soldier_anim_tests();
void run_unit_status_bit_tests();
void run_unit_refund_tests();
void run_unit_creation_tests();
// SIM1A batch-A tail (2026-08-12): offline coverage for the three OUT-pointer functions whose shadow
// sites are vacuous (write only through out-params, nothing to compare) -- simtest is their only oracle.
void run_unit_facing24_delta_tests();
void run_unit_fine_pos_tests();
void run_unit_soldier_screen_pos_tests();
// SIM1B (2026-08-12): offline coverage for the two vacuous-shadow OUT-pointer/caller-buffer functions
// from the placement/roster-queries slice.
void run_bldg_placement_corner_tests();
void run_bldg_connectivity_flood_tests();
// SIM1-G4 (2026-08-22): debt-drain offline oracles for the 6 DO-NOT-ARM/no-shadow-site
// rows the left as reviewed-not-verified.
void run_bldg_placement_corner_by_type_tests();
void run_turret_acquire_target_tests();
void run_turret_fire_tests();
void run_prod_unload_cargo_manifest_tests();
void run_prod_try_start_unit_tests();
void run_bldg_completion_dispatch_tests();
void run_bldg_done_handlers_tests();
void run_bldg_anim_tick_tests();          // SIM1-BLDGCB batch H second slice (2026-08-23)
void run_bldg_anim_state_helipad_tests(); // SIM1-BLDGCB batch H second slice (2026-08-23)
// SIM1-BLDGCB batch H, verification-debt drain (2026-08-23):
void run_bldg_anim_state_helipad_a_tests();
void run_bldg_anim_state_online_a_tests();
void run_pathtrace_greedy_tests();
void run_bldg_start_special_anim_tests();
// SIM1-BLDGCB batch H closing slice, verification-debt drain (2026-08-23):
void run_bldg_anim_state_port_tests();
// SIM1B batch-B closing session (2026-08-13): offline oracles for the remaining vacuous/unreached
// building-tick functions. The power-network pair is done_when clause 2 (the flood fill).
void run_bldg_get_coords_tests();
void run_bldg_find_mothership_position_tests();
void run_bldg_footprint_set_passable_tests();
void run_bldg_energy_refill_full_tests();
void run_bldg_scrap_stored_units_tests();
void run_bldg_power_network_tests();
// The construct/destroy transition handlers (offline orchestration + direct-write oracle; their
// per-call shadow closures are unbounded/effectful, so simtest is the oracle -- G13/G19/G21 class).
void run_bldg_construct_finalize_tests();
void run_bldg_unmap_footprint_tests();
void run_bldg_state_destroyed_tests();
// SIM1-G4 verification debt (2026-08-22): the eleven first-slice building-state/turret/charge
// handlers that closed `reviewed` with no rig evidence -- all DO-NOT-ARM (notify_ui/game_SetEvent's
// broad static closure, or a shared `turrets` write region), plus deploy_anim_wait whose rig arm got
// zero calls. Offline oracles are the only evidence path for all eleven. See the
// SIM1-G4 progress.
void run_bldg_state_default_reset_tests();
void run_bldg_state_idle_activate_tests();
void run_bldg_state_deploy_anim_wait_tests();
void run_bldg_state_land_activate_tests();
void run_bldg_state_deploy_start_tests();
void run_bldg_state_to_unit_tests();
void run_bldg_state_turret_scan_tests();
void run_bldg_state_turret_attack_tests();
void run_bldg_state_construction_tests();
void run_bldg_state_charge_gate_tests();
void run_bldg_state_charge_step_tests();
// SIM1-G4 (2026-08-22): eight DO-NOT-ARM building-state handlers (hangar recharge,
// upgrade/research progress, dismantle, production pick-next/working) -- all reach the
// game_SetEvent UI/gfx/snd/menu-teardown over-approximation or carry writes_shared breadth, so
// offline is their only evidence. See SIM1-G4 progress.
void run_bldg_state_hangar_recharge_check_tests();
void run_bldg_state_hangar_recharge_units_tests();
void run_bldg_state_upgrading_tests();
void run_bldg_state_researching_tests();
void run_bldg_state_dismantling_tests();
void run_bldg_state_dismantle_finish_tests();
void run_bldg_state_prod_pick_next_tests();
void run_bldg_state_prod_working_tests();
// SIM1-G4 / fourth-slice verification debt drain (2026-08-22): power_primary_check's oracle
// was authored in the but never registered; the other eight are the's own
// remaining unproven rows (mine check-deposits/extracting, shuttle/mother liftoff-anim, the shared
// construction-complete dispatcher, the energy-scaled resource refund, and the hangar any-unit-needs-
// energy/recharge-pulse pair) -- all DO-NOT-ARM or zero-call on the rig run, offline is their evidence.
// See SIM1-G4 progress.
void run_bldg_state_power_primary_check_tests();
void run_bldg_state_mine_check_deposits_tests();
void run_bldg_state_mine_extracting_tests();
void run_bldg_start_liftoff_anim_shuttle_tests();
void run_bldg_start_liftoff_anim_mother_tests();
void run_bldg_construction_complete_tests();
void run_bldg_refund_resources_scaled_by_energy_tests();
void run_hangar_any_unit_needs_energy_tests();
void run_hangar_recharge_pulse_tests();
void run_dmp_enqueue_scripted_order_tests();
void run_reason_to_housing_bldg_tests();
void run_bldg_register_online_tests();
void run_bldg_sprite_anchor_tests();
void run_load_base_layout_dmp_tests();
void run_prod_shuttle_unload_tests();
void run_prod_completion_pipeline_tests();
void run_weapon_damage_tests();
// SIM1-G4 / SIM1-G5 (2026-08-22)
void run_bldg_online_secondary_tests();
// SIM1D-V (2026-08-16): verification debt -- the twelve batch-D functions SIM1D closed at
// state=reviewed, none armable (unbounded/effectful closures or a documented arm-crash), all
// covered offline instead. See SIM1D-V progress for the per-function plan.
void run_bldg_instant_construct_tests();
void run_bldg_queue_construction_thunk_tests();
void run_prod_shuttle_depart_tests();
void run_prod_bldg_depart_finalize_tests();
void run_prod_shuttle_load_passengers_tests();
void run_prod_unload_cargo_unit_tests();
void run_unit_load_into_shuttle_cargo_tests();
void run_unit_transport_unload_tests();
void run_unit_apply_production_completion_tests();
void run_storage_purge_dead_docked_tests();
void run_game_notify_system_available_tests();
void run_fog_sight_circle_tests();
void run_weapon_scatter_offset_tests();
void run_unit_fire_weapon_tests();
void run_unit_chase_check_tests();               // RI-SIM / SIM1-G5 second slice (2026-08-22)
void run_unit_try_pay_action_cost_tests();       // RI-SIM / SIM1-G5 second slice (2026-08-22)
void run_unit_calc_range_approach_point_tests(); // RI-SIM / SIM1-G5 second slice (2026-08-22)
void run_projectile_tick_tests();
void run_bldg_apply_damage_tests();
void run_unit_apply_damage_tests();
void run_unit_state_die_explode_tests();
// SIM1-G1 (2026-08-19): unit MOVE state handlers
void run_unit_state_budget_noop_tests();
void run_unit_state_group_step_tests();
void run_unit_state_stop_to_default_tests();
void run_unit_state_patrol_swap_tests();
void run_unit_state_hover_engage_tests();
void run_unit_state_flight_tests();
void run_unit_state_corpse_fow_decay_tests();
void run_unit_state_move_walker_tests();
void run_unit_state_group_marshal_tests();
void run_unit_state_attack_building_tests();
void run_unit_state_attack_unit_tests();
void run_unit_state_remove_silent_tests();
void run_unit_state_idle_scatter_tests();
void run_group_move_order_commit_tests();
void run_unit_group_step_plane_tests();
void run_unit_set_state_order_of_tests();
void run_unit_teardown_mapped_tests();
void run_unit_free_slot_tests();
void run_unit_change_proto_and_energy_tests();
void run_squad_pick_free_formation_anchor_tests();
void run_unit_group_step_ground_tests();
// SIM1-G1 tail slice (2026-08-20): the two group-scratch registration helpers, arm_ready=false per
// its caller-owned in/out counter pointer, unreachable by the shadow restore --
// this offline oracle is their only evidence.
void run_group_scratch_helpers_tests();
void run_combat_conquest_credit_tests();
void run_unit_estimate_weapon_damage_tests();
// SIM1F (2026-08-18): the batch-F done_when's population-add boundary oracle, plus the
// landing count/set accessors' offline oracles.
void run_population_add_tests();
void run_landing_queries_tests();
// SIM1F (2026-08-18): offline oracles promoting two batch-F translations to T1 --
// llm_strat_revoke_invention (scenario-gapped: zero live calls) and game_InsertItemInPlayerArray.
void run_revoke_invention_tests();
void run_insert_item_in_player_array_tests();
// SIM1F (2026-08-18): game_SetEvent, the strategic-HUD panel/notification dispatcher.
void run_game_set_event_tests();
// SIM1F (2026-08-18): llm_strat_player_presence_lost, the elimination / win-loss resolver.
// RI-SIM / sim_resid batches A+B, first translation slice (2026-08-31).
void run_clock_resync_tests();
void run_session_clear_presence_flag_tests();
// sim_resid rows whose bodies never descend into an intra-slice sibling's live mh::call:: thunks
// and are therefore callable offline.
void run_advisor_tick_tests();
void run_planet_transition_finalize_tests();
void run_planet_map_session_init_tests();
void run_new_game_init_tests();
void run_land_players_on_planet_tests();
void run_planet_session_begin_tests();
void run_session_begin_multi_tests();
void run_session_state_reset_tests();
void run_player_presence_lost_tests();
// SIM1F (2026-08-18): llm_strat_spawn_invasion_force, the AI per-player region init (the
// LAST function of the whole SIM migration).
void run_spawn_invasion_force_tests();
// SIM1F FINAL SLICE (2026-08-19): offline oracles promoting the last batch-F reviewed rows to verified.
void run_tile_midpoint_wrapped_tests();
void run_tile_delta_wrapped_tests();
void run_pixel_delta_wrapped_tests();
void run_map_wrapped_delta_tests();
void run_dir_sector_to_tests();
void run_tile_neighbor_reverse_dir_tests();
void run_planet_distance_tests();
void run_locate_active_port_tests();
void run_region_add_adjacency_edge_tests();
void run_region_recompute_adjacency_tests();
void run_merge_small_regions_tests();
void run_region_split_tests();
void run_add_to_available_buildings_tests();
void run_add_project_to_available_tests();
void run_add_project_to_available_with_check_tests();
void run_try_start_project_tests();
void run_apply_project_resources_tests();
void run_update_progress_tests();
void run_invasion_chance_roll_tests();
void run_handle_invasion_tests();
void run_invasion_alert_arm_tests();
void run_spawn_enemy_landing_tests();
void run_sp_outcome_announce_tests();
void run_speed_increase_tests();
void run_speed_decrease_tests();
void run_player_set_ai_tests();
// SIM1F FINAL SLICE cont. (2026-08-19): the _calls-indirection-unblocked batch-F functions.
void run_apply_area_to_map_tests();
void run_region_apply_area_tests();
void run_map_create_building_tests();
// SIM1F: the domain root llm_strat_sim_step (offline orchestration/rollover oracle -- last of the 305).
void run_sim_step_tests();
// SIM1-DISPATCH (2026-08-22): OUR registrar for the two state-machine dispatch tables.
void run_register_state_handlers_tests();
// SIM1-BLDGCB (2026-08-23): OUR registrar for the SECOND registry -- the two per-building-TYPE
// callback tables (BLDG_DONE_FUNCS / BLDG_TICK2_FUNCS).
void run_register_bldg_type_callbacks_tests();
// SIM1-G1 tail slice (2026-08-20): llm_strat_group_plan_formation_positions, the batch's LAST
// unverified function -- arm_ready=false (writes heap-allocated region-graph nodes' ->route_mark, an
// address escape no shadow snapshot/restore can reach), so this offline oracle is its only evidence.
void run_group_plan_formation_positions_tests();
// SIM1-G2 (2026-08-20): llm_strat_dir_step_factor / llm_strat_tile_neighbor_in_dir /
// llm_strat_heading_candidate_find_slot -- all three NOT SHADOWABLE (0 tracked write cells), so
// this is their only evidence. reimpl-verify: 3/3 reviewers clean, 0 findings.
void run_pathfind_grid_geometry_tests();
// SIM1-G2 (2026-08-20): llm_strat_unit_path_step_blocked -- NOT SHADOWABLE (0 tracked write cells).
void run_unit_path_step_blocked_tests();
// SIM1-G2 (2026-08-20): llm_strat_path_step_check_and_request_detour -- 0 DIRECT write cells (writes
// only through its callee llm_strat_unit_path_detour). Also rig-verified T1 (12276
// calls / 0 divergences, all-AI soak); this offline oracle is supplementary, not the sole evidence.
void run_path_step_check_and_request_detour_tests();
// SIM1-G2 (2026-08-21): llm_strat_pathfind_mark_group_member_regions /
// llm_strat_pathfind_target_hook_stub / llm_strat_dir_step_toroidal_dist / llm_strat_pathtrace_dirs_get
// / llm_strat_path_alloc_slot -- all NOT SHADOWABLE (0 tracked write cells; path_alloc_slot is
// shadowable only through its out-of-scope callee). reimpl-verify: all 5 clean, 0 real findings.
void run_pathfind_geom_misc_tests();
// SIM1-G2 (2026-08-21): llm_map_region_neighbor_edge_value / _first_shared_node /
// _route_mark_shared_nodes / _route_prepass -- all NOT SHADOWABLE (0 tracked write cells;
// route_mark_shared_nodes's only write is a heap-node field, route_prepass writes nothing at all).
void run_map_region_route_marking_tests();
// SIM1-G2 (2026-08-21): llm_map_wrap_delta_x / llm_map_wrap_delta_y -- both NOT
// SHADOWABLE (0 tracked write cells, pure).
void run_map_wrap_delta_xy_tests();
// SIM1-G2 (2026-08-21): llm_strat_pathfind_dir_code_from_delta / llm_strat_tiles_adjacent
// -- both NOT SHADOWABLE (0 tracked write cells, pure).
void run_pathfind_dir_code_and_adjacency_tests();
// SIM1-G2 (2026-08-20): llm_strat_path_find_free_slot -- NOT SHADOWABLE (0 tracked write
// cells). llm_strat_path_write_from_solver (same TU) is rig-verified T1 separately (2682 calls).
void run_path_solver_tests();
// SIM1-G2 (2026-08-20): llm_strat_slot_dist_to_ref -- NOT SHADOWABLE (0 tracked write
// cells). llm_strat_claim_free_slots_within_dist (same TU) is rig-verified T1 separately (1277 calls).
void run_path_slot_dist_tests();
// SIM1-G2 (2026-08-20): llm_map_region_find_nearest_valid_tile,
// llm_strat_pathfind_find_closer_visible_tile, llm_map_region_walk_to_valid_tile -- all three NOT
// SHADOWABLE (0 tracked write cells). Case (d) is a regression test for a real ADD-vs-OR passable[]
// indexing bug reimpl-verify.js caught and fixed.
void run_map_region_tile_find_tests();
// SIM1-G2 (2026-08-20): llm_strat_unit_path_queue_count -- NOT SHADOWABLE (0 tracked
// write cells). Case (b) confirms the early-exit return value is 2*flat_index, not the slot count.
void run_unit_path_queue_count_tests();
// SIM1-G2 (2026-08-20): llm_strat_unit_state_plot_turn_path -- also rig-verified T1 (
// 1148 calls / 0 divergences, all-AI soak); this offline oracle is supplementary, not the sole
// evidence.
void run_unit_state_plot_turn_path_tests();
// SIM1-G2 (2026-08-20): llm_strat_unit_state_move_path -- PERMANENTLY un-shadowable
// (llm_strat_unit_chase_check's transitive closure reaches the SIM-CUT effectful UI/snd/net set,
// same root cause as SIM1-G1's llm_strat_unit_state_hover_engage), so this offline oracle is its
// ONLY EVER evidence -- see dll_shadow_manifest.json's why_extra for the full derivation.
void run_unit_state_move_path_tests();
// SIM1-G3 (2026-08-21): llm_strat_unit_state_enter_arrival_check -- NOT SHADOWABLE (0
// tracked write cells; its only effect is inside llm_strat_unit_set_state's own closure), so this
// offline oracle is its ONLY EVER evidence.
void run_unit_state_enter_arrival_check_tests();
// SIM1-G3 (2026-08-21): llm_strat_unit_state_exit_cancel -- arm_ready:true but this
// slice's rig run reached 0 calls (all-AI soak never happened to cancel a pending exit); this
// offline oracle supplements the (still pending) rig evidence.
void run_unit_state_exit_cancel_tests();
// SIM1-G3 (2026-08-21): llm_strat_storage_can_land -- NOT SHADOWABLE (0 direct AND 0
// transitive write cells), so this offline oracle is its ONLY EVER evidence.
void run_storage_can_land_tests();
// SIM1-G3 (2026-08-21): llm_strat_storage_exit_tile_is_clear -- NOT SHADOWABLE (0
// direct AND 0 transitive write cells), so this offline oracle is its ONLY EVER evidence.
void run_storage_exit_tile_is_clear_tests();
// SIM1-G3 (2026-08-21): llm_strat_storage_scrap_home_docked_units -- VACUOUS under a
// per-call shadow arm (writes nothing directly), so this offline oracle is its ONLY EVER evidence.
void run_storage_scrap_home_docked_units_tests();
// SIM1-G3 (2026-08-21): llm_strat_storage_accept_landing -- VACUOUS under a per-call
// shadow arm (writes nothing directly), so this offline oracle is its ONLY EVER evidence.
void run_storage_accept_landing_tests();
// SIM1-G3 (2026-08-21): llm_strat_storage_scrap_docked_units_of_type -- ARMABLE but NOT
// COVERED after two all-AI soak runs; this offline oracle is this slice's evidence in the meantime.
void run_storage_scrap_docked_units_of_type_tests();
// SIM1-G3 (2026-08-21): llm_strat_storage_can_enter -- ARMABLE but NOT COVERED after
// four all-AI soak runs; this offline oracle is this slice's evidence in the meantime.
void run_storage_can_enter_tests();
// SIM1-G3 (2026-08-21): llm_strat_unit_state_enter_wait -- ARMABLE (arm_ready flipped
// true this slice) but NOT COVERED after five all-AI soak runs; this offline oracle is this slice's
// evidence in the meantime -- a future rig run with a targeted save can still add T1 on top.
void run_unit_state_enter_wait_tests();
// SIM1-G3 (2026-08-21): llm_strat_unit_state_enter_walk_in -- ARMABLE (arm_ready:true
// since the) but NOT COVERED after six all-AI soak runs; this offline oracle is this
// slice's evidence in the meantime -- a future rig run with a targeted save can still add T1 on top.
void run_unit_state_enter_walk_in_tests();
// SIM1-G3 (2026-08-21): llm_strat_unit_state_enter_storage_begin -- arm_ready:false
// (broad closure via llm_strat_storage_board_unit); this offline oracle is its evidence. Also caught
// and fixed a real PRESERVE-BUG regression (a translator-added early `return` that "fixed" the
// original's fallthrough-after-abort quirk) -- see the .cpp's own note at the fix site.
void run_unit_state_enter_storage_begin_tests();
// SIM1-G3 (2026-08-21): llm_strat_unit_takeoff_finalize -- arm_ready:false (broad
// closure); this offline oracle is its evidence. No branches -- straight-line, so exhaustive.
void run_unit_takeoff_finalize_tests();
// SIM1-G3 (2026-08-21): llm_strat_unit_state_landing_request -- arm_ready:false (broad
// closure, same escape as the rest of this batch's remaining rows); this offline oracle is its evidence.
void run_unit_state_landing_request_tests();
// SIM1-G3 (2026-08-21): llm_strat_unit_state_production_ready -- arm_ready:false (broad
// closure, same escape); this offline oracle is its evidence.
void run_unit_state_production_ready_tests();
// SIM1-G3 (2026-08-21): llm_strat_unit_state_deploy_approach -- arm_ready:false (broad
// closure, same escape); this offline oracle is its evidence, incl. the two-out-pointer-pair CORRECTION.
void run_unit_state_deploy_approach_tests();
// SIM1-G3 (2026-08-21): llm_strat_unit_state_takeoff_taxi -- arm_ready:false (broad
// closure, same escape); this offline oracle is its evidence.
void run_h_switch_to_planet_tests();
void run_h_group_issue_orders_tests();
void run_h_shuttle_and_diplomacy_tests();
void run_unit_state_takeoff_taxi_tests();
// SIM1-G3 (2026-08-21): llm_strat_unit_state_dock_taxi_in -- arm_ready:false (broad
// closure, same escape); this offline oracle is its evidence, incl. the shuttle_slot preserved-bug.
void run_unit_state_dock_taxi_in_tests();
// SIM1-G3 (2026-08-21): the batch's last 6 rows, all arm_ready:false (closures confirmed
// broad, 777-801 functions each) -- these offline oracles are their evidence.
void run_unit_state_exit_storage_begin_tests();
void run_storage_board_unit_tests();
void run_unit_state_deploy_to_building_tests();
void run_unit_state_squad_merge_tests();
void run_unit_state_takeoff_landing_tests();
void run_storage_place_exit_ground_tests();
void run_player_init_tests();
void run_table_resets_tests();
void run_unit_path_release_tests();
void run_unit_path_store_tests();
// SIM-RESID-C and the tail of SIM-RESID-E: the four rows batch C had DEFERRED on the
// state interface, plus the two rows batch_overrides scheduled into C and the two E rows the slice
// grew to reach. Same offline-only reason as the four above.
void run_tech_tables_reset_tests();
void run_init_human_player_data_tests();
void run_pathfinder_init_tests();
void run_map_fill_defaults_tests();
void run_scenario_planet_clone_tests();
void run_invasion_alert_reset_tests();
void run_bldg_try_begin_placement_tests();
void run_landing_spots_reroll_tests();
// sim_resid batch E/F.
void run_rng_seed_channel_tests();
void run_time_resync_tests();
void run_invasion_due_check_tests();
void run_tutorial_step_driver_tests();
void run_start_tutorial_tests();
void run_try_enter_tactical_mission_tests();
// sim_resid batch E verification debt, drained 2026-09-01.
void run_prod_set_transfer_destination_tests();
void run_bldg_gather_nearby_squad_status_tests();
void run_bldg_is_network_critical_tests();
void run_sort_sites_by_dist_tests();
void run_spawn_ai_base_tests();
// X-TL-DRAIN step 4 (2026-09-12). The three original CALLERS whose translation dissolves the inbound
// surface's X-TL exclusion class. Two are offline-only by necessity and one is offline-as-well:
// llm_unit_order_disembark_soldiers' shadow site is VACUOUS by construction (void return, no writes,
// its one outward call must be inert), llm_strat_bldg_instant_construct_find_slot's armable site is
// reachable only by a player confirming a building placement, and llm_strat_bldg_init_all's site is
// reachable once per boot and compares only the callee's region. See each file's own banner.
void run_bldg_init_all_tests();
void run_bldg_instant_construct_find_slot_tests();
void run_unit_order_disembark_soldiers_tests();
} // namespace mh::sim::test

namespace {

using namespace mh::sim;
using mh::sim::test::ck;
using mh::sim::test::ck_eq;

// ---- W1/W2 wiring -------------------------------------------------------------------------------
//
// Not a behaviour test: it asserts the fixture binds the SAME region to both halves, which is the
// assumption every case below rests on. If store() and view() ever addressed different buffers, a
// write test would pass by writing somewhere nothing reads.
void test_fixture_halves_agree() {
    sim_fixture f;
    sim_view    v   = f.view();
    sim_store   own = f.store();

    own.unit_at(3, 7).ai_group_index = 0x1234;
    ck(unit_of(v, 3, 7).ai_group_index == 0x1234,
       "fixture: a write through the store is visible through the view (one copy of the state)");

    own.player_at(2).resource_spend_total[1] = 99;
    ck(player_of(v, 2).resource_spend_total[1] == 99,
       "fixture: same for player_data, which the two halves also share");

    // The row stride, asserted once rather than assumed by every later case.
    ck(&own.unit_at(1, 0) == &f.units[UNITS_PER_PLAYER],
       "fixture: unit_at's row stride is UNITS_PER_PLAYER");
}

// ---- llm_strat_bldg_is_alive @0x004d3d45 --------------------------------------------------------
void test_bldg_is_alive() {
    sim_fixture f;
    sim_view    v = f.view();

    // Both guards satisfied.
    f.b(2, 5).energy = 1.0;
    f.b(2, 5).state  = 100; // 100 = under construction, the game's own init value
    ck(detail::bldg_is_alive(v, 2, 5) == 1, "is_alive: positive energy + a live state -> 1");

    // ENERGY IS THE FIRST GUARD, and it is a strict `> 0` -- exactly zero is dead.
    f.b(2, 5).energy = 0.0;
    ck(detail::bldg_is_alive(v, 2, 5) == 0, "is_alive: energy == 0.0 is DEAD (strict >, not >=)");
    f.b(2, 5).energy = -1.0;
    ck(detail::bldg_is_alive(v, 2, 5) == 0, "is_alive: negative energy is dead");

    // THE STATE GUARD IS AN EQUALITY AGAINST 4, NOT A RANGE. State 3 and state 5 are both alive;
    // only RUBBLE_SIGHT_DECAY is rejected. A translation using `< 4` or `!= 2` passes a one-value
    // test and fails here.
    f.b(2, 5).energy = 1.0;
    f.b(2, 5).state  = BLDG_STATE_RUBBLE_SIGHT_DECAY;
    ck(detail::bldg_is_alive(v, 2, 5) == 0, "is_alive: state == 4 (RUBBLE_SIGHT_DECAY) -> 0");
    f.b(2, 5).state = 3;
    ck(detail::bldg_is_alive(v, 2, 5) == 1, "is_alive: state 3 is alive (the guard is == 4, not < 4)");
    f.b(2, 5).state = 5;
    ck(detail::bldg_is_alive(v, 2, 5) == 1, "is_alive: state 5 is alive too");
    f.b(2, 5).state = 2; // 2 = destroyed, per the field comment -- and STILL alive by this predicate
    ck(detail::bldg_is_alive(v, 2, 5) == 1,
       "is_alive: state 2 ('destroyed') passes -- this predicate tests 4 only, and that is the "
       "original's behaviour, not an oversight to fix");

    // The row stride: player 2 slot 5 must not be player 0 slot 205.
    f.reset();
    f.b(0, 5).energy = 1.0;
    f.b(0, 5).state  = 1;
    ck(detail::bldg_is_alive(v, 2, 5) == 0,
       "is_alive: reads its OWN player's row (a stride error would find player 0's building)");
}

// ---- the weapon queries -------------------------------------------------------------------------
void test_unit_weapon_predicates() {
    sim_fixture f;
    sim_view    v = f.view();

    // Weapon 7 is air-only, weapon 9 is ground-only, weapon 11 is both. Distinct ids and distinct
    // masks so a mask/id mix-up cannot pass.
    f.cfg_weapons[7].target  = WEAPON_TARGET_AIR;
    f.cfg_weapons[9].target  = WEAPON_TARGET_GROUND;
    f.cfg_weapons[11].target = (uint8_t)(WEAPON_TARGET_AIR | WEAPON_TARGET_GROUND);

    unit &u                = f.u(1, 4);
    u.weapons[0].weapon_id = 7;
    u.weapons[0].enabled_2 = 1;

    ck(detail::unit_has_aa_weapon(v, 1, 4) == 1, "has_aa: an enabled air weapon -> 1");
    ck(detail::unit_has_ground_weapon(v, 1, 4) == 0,
       "has_ground: an air-only weapon does NOT satisfy the ground query");

    u.weapons[0].weapon_id = 9;
    ck(detail::unit_has_aa_weapon(v, 1, 4) == 0, "has_aa: a ground-only weapon -> 0");
    ck(detail::unit_has_ground_weapon(v, 1, 4) == 1, "has_ground: a ground weapon -> 1");

    u.weapons[0].weapon_id = 11;
    ck(detail::unit_has_aa_weapon(v, 1, 4) == 1 && detail::unit_has_ground_weapon(v, 1, 4) == 1,
       "both queries accept a weapon whose target mask has both bits");

    // THE ENABLED FLAG IS A SEPARATE GATE FROM THE ID. A disabled slot is skipped even though its
    // weapon would qualify.
    u.weapons[0].enabled_2 = 0;
    ck(detail::unit_has_aa_weapon(v, 1, 4) == 0, "has_aa: a DISABLED slot is skipped");

    // ...and an enabled slot with weapon_id 0 is skipped too, which is the guard that stops slot 0
    // of a freshly-zeroed unit from indexing Weapon[0].
    u.weapons[0].enabled_2  = 1;
    u.weapons[0].weapon_id  = 0;
    f.cfg_weapons[0].target = (uint8_t)(WEAPON_TARGET_AIR | WEAPON_TARGET_GROUND);
    ck(detail::unit_has_aa_weapon(v, 1, 4) == 0,
       "has_aa: weapon_id == 0 is skipped even when Weapon[0] would qualify");

    // ALL FOUR SLOTS ARE SCANNED, and the last one counts -- an off-by-one bound would miss it.
    f.reset();
    v                       = f.view();
    f.cfg_weapons[7].target = WEAPON_TARGET_AIR;
    unit &u2                = f.u(1, 4);
    u2.weapons[3].weapon_id = 7;
    u2.weapons[3].enabled_2 = 1;
    ck(detail::unit_has_aa_weapon(v, 1, 4) == 1, "has_aa: slot 3 (the last) is scanned");

    // THE PLAYER COMES FROM THE LOW NIBBLE OF unit_ref, and the high nibble is ignored.
    ck(detail::unit_has_aa_weapon(v, 0xa1u, 4) == 1,
       "has_aa: unit_ref 0xa1 indexes player 1 -- the high nibble is masked off");
    ck(detail::unit_has_aa_weapon(v, 2, 4) == 0,
       "has_aa: a DIFFERENT player's row does not see player 1's weapon");
}

void test_unit_max_weapon_range() {
    sim_fixture f;
    sim_view    v = f.view();

    // THE INSTANCE AND THE TYPE ARE DIFFERENT ARRAYS, and this is the case that proves this
    // function reads the TYPE. The unit's own slots are left empty on purpose; if the translation
    // read them, every assertion below would return 0.
    f.u(1, 4).unit_proto_id = 12;

    cfg_unit &cu = f.cfg_units[12];
    // slot 0: weapon 3, enabled. slot 1: weapon 5, DISABLED. slot 2: weapon 8, enabled.
    cu.weapons[0] = 3;
    cu.weapons[1] = 1;
    cu.weapons[2] = 5;
    cu.weapons[3] = 0;
    cu.weapons[4] = 8;
    cu.weapons[5] = 1;

    // range_max is PER PLAYER. Player 1 and player 2 get deliberately different rows so a
    // translation that indexed with a constant, or with the wrong operand, cannot pass both.
    f.cfg_weapons[3].range_max[1] = 40;
    f.cfg_weapons[5].range_max[1] = 900; // disabled slot -- must NOT win
    f.cfg_weapons[8].range_max[1] = 70;
    f.cfg_weapons[3].range_max[2] = 5;
    f.cfg_weapons[8].range_max[2] = 6;

    ck(detail::unit_max_weapon_range(v, 1, 4) == 70,
       "max_range: the largest ENABLED slot's range for this player");
    ck(detail::unit_max_weapon_range(v, 1, 4) != 900,
       "max_range: a disabled slot is excluded even when it holds the largest range");

    // Same unit record, different player nibble -> a different range_max column.
    f.u(2, 4).unit_proto_id = 12;
    ck(detail::unit_max_weapon_range(v, 2, 4) == 6,
       "max_range: range_max is indexed by the PLAYER nibble, not by a constant");

    // No enabled slot at all -> 0, not a stale maximum.
    cu.weapons[1] = 0;
    cu.weapons[5] = 0;
    ck(detail::unit_max_weapon_range(v, 1, 4) == 0, "max_range: no enabled slot -> 0");

    // THE PAIR PACKING. cfg_unit::weapons is rendered flat as uint8_t[8]; slot i is (id, enabled) at
    // [2i], [2i+1]. Writing the id into the ENABLED byte must not be read as an enabled weapon of
    // that id -- which is what a translation indexing `weapons[i]` instead of `weapons[2i]` does.
    f.reset();
    v                             = f.view();
    f.u(1, 4).unit_proto_id       = 12;
    f.cfg_weapons[6].range_max[1] = 33;
    f.cfg_units[12].weapons[6]    = 6; // slot 3's ID
    f.cfg_units[12].weapons[7]    = 1; // slot 3's ENABLED
    ck(detail::unit_max_weapon_range(v, 1, 4) == 33,
       "max_range: slot 3 of the flattened (id, enabled) pair array is read correctly");
}

// ---- llm_unit_status_bit_set / _clear -----------------------------------------------------------
void test_unit_status_bits() {
    sim_fixture f;
    sim_view    v   = f.view();
    sim_store   own = f.store();

    detail::unit_status_bit_set(own, 3, 9, 0);
    ck(unit_of(v, 3, 9).ai_group_index == 0x0001, "bit_set: bit 0");
    detail::unit_status_bit_set(own, 3, 9, 5);
    ck(unit_of(v, 3, 9).ai_group_index == 0x0021,
       "bit_set: ORs in -- it does not overwrite the word");
    detail::unit_status_bit_set(own, 3, 9, 15);
    ck(unit_of(v, 3, 9).ai_group_index == 0x8021, "bit_set: bit 15, the top of the WORD");

    detail::unit_status_bit_clear(own, 3, 9, 5);
    ck(unit_of(v, 3, 9).ai_group_index == 0x8001, "bit_clear: clears exactly the named bit");
    detail::unit_status_bit_clear(own, 3, 9, 7);
    ck(unit_of(v, 3, 9).ai_group_index == 0x8001,
       "bit_clear: clearing an already-clear bit changes nothing");

    // THE >= 16 CASES, which no rig scenario would produce and which the two functions handle
    // ASYMMETRICALLY. The set path stores only the low word of a 32-bit shift, so bit 16 is a
    // no-op; the clear path XORs the whole dword with 0xffff first, leaving the low word 0xffff, so
    // its AND is a no-op too -- for a DIFFERENT reason. Both are the original's; see the header.
    //
    // THE FIELD IS RE-SEEDED FIRST, AND BIT 0 IS DELIBERATELY LEFT CLEAR. Running this on the value
    // built above (0x8001) made the case UNFALSIFIABLE: the plausible mutant is a shift masked to
    // four bits instead of five, which turns "set bit 16" into "set bit 0" -- and bit 0 was already
    // set, so the wrong function produced the right bytes. Measured, not hypothesised: mutant M4 of
    // tools/oneoff/2026-08-08-mutate-sim0-pilot.py was MISSED on the first run for exactly this
    // reason. That is AI0's precedent restated -- a mutation only tests an assertion where the two
    // behaviours DIFFER on the fixture.
    f.reset();
    own = f.store();
    v   = f.view();
    detail::unit_status_bit_set(own, 3, 9, 5);
    detail::unit_status_bit_set(own, 3, 9, 15);
    const uint16_t before = unit_of(v, 3, 9).ai_group_index;
    ck_eq(before, 0x8020, "bit_set: the >= 16 fixture starts at 0x8020, with bit 0 CLEAR");
    detail::unit_status_bit_set(own, 3, 9, 16);
    ck_eq(unit_of(v, 3, 9).ai_group_index, before,
          "bit_set: bit 16 falls off the WORD store -- a no-op, not a set of bit 0");
    detail::unit_status_bit_clear(own, 3, 9, 20);
    ck_eq(unit_of(v, 3, 9).ai_group_index, before,
          "bit_clear: bit 20 leaves the mask 0xffff -- a no-op, NOT a clear of bit 4");

    // THE SHIFT IS MASKED TO 5 BITS BY THE HARDWARE, so 32 is bit 0 and not a no-op. This is the
    // case that separates `1u << (n & 31)` from `n < 32 ? 1u << n : 0`.
    f.reset();
    own = f.store();
    v   = f.view();
    detail::unit_status_bit_set(own, 3, 9, 32);
    ck(unit_of(v, 3, 9).ai_group_index == 0x0001,
       "bit_set: bit_index 32 wraps to bit 0 (SHL masks CL to 5 bits) -- not a no-op");

    // The row stride again: player 3 slot 9 is not player 0 slot 309.
    ck(unit_of(v, 0, 9).ai_group_index == 0, "bit_set: wrote its OWN player's row");
}

// ---- the two resource tallies -------------------------------------------------------------------
void test_resource_spend() {
    sim_fixture f;
    sim_view    v   = f.view();
    sim_store   own = f.store();

    const int32_t player                   = 2;
    f.players[player].ai_spend_ring_cursor = 3;

    // Three costs and a terminator, with DISTINCT ids and DISTINCT values so nothing can pass by
    // coincidence. Note id 4 in slot 2 with a value that differs from its id.
    cfg_unit &cu       = f.cfg_units[17];
    cu.resource[0].id  = 1;
    cu.resource[0].val = 10;
    cu.resource[1].id  = 3;
    cu.resource[1].val = 25;
    cu.resource[2].id  = 4;
    cu.resource[2].val = 7;
    cu.resource[3].id  = 0; // terminator
    cu.resource[4].id  = 2; // BEYOND the terminator -- must never be reached
    cu.resource[4].val = 1000;

    detail::econ_track_unit_resource_spend(v, own, player, 17);

    const player_data &pd = player_of(v, (uint32_t)player);
    ck(pd.resource_spend_total[1] == 10 && pd.resource_spend_total[3] == 25 &&
           pd.resource_spend_total[4] == 7,
       "spend: each (id, val) lands in resource_spend_total[id]");
    ck(pd.resource_spend_total[2] == 0,
       "spend: the walk STOPS at the first id == 0 -- slot 4 is never read");
    ck(pd.resource_spend_total[0] == 0, "spend: nothing is written to the id-0 slot");

    // THE RING BIAS. The ring is int[32][4] flattened, the cursor selects the slot, and the column
    // is `id - 1` because resource ids are one-based and the array is not. This is the assertion
    // that fails if a translation indexes `[cursor * 4 + id]`.
    const int32_t base = 3 * SPEND_RING_RESOURCE_COLS;
    ck(pd.ai_spend_rate_numer_ring[base + 0] == 10, "spend ring: id 1 -> column 0 of the slot");
    ck(pd.ai_spend_rate_numer_ring[base + 2] == 25, "spend ring: id 3 -> column 2");
    ck(pd.ai_spend_rate_numer_ring[base + 3] == 7, "spend ring: id 4 -> column 3");
    ck(pd.ai_spend_rate_numer_ring[base + 1] == 0,
       "spend ring: id 2 was never spent, so its column stays 0");
    // And the neighbouring slots are untouched, which is what says the CURSOR selected the row.
    ck(pd.ai_spend_rate_numer_ring[base - 1] == 0 && pd.ai_spend_rate_numer_ring[base + 4] == 0,
       "spend ring: only the cursor's own slot is written");

    // ACCUMULATION, not assignment: a second call adds.
    detail::econ_track_unit_resource_spend(v, own, player, 17);
    ck(pd.resource_spend_total[1] == 20 && pd.ai_spend_rate_numer_ring[base + 0] == 20,
       "spend: a second call ACCUMULATES into both tallies");

    // A different player's record is untouched -- the 0x288fc row stride.
    ck(player_of(v, 0).resource_spend_total[1] == 0, "spend: wrote its OWN player's record");

    // THE SEVEN-SLOT BOUND. No terminator at all: the walk must consume exactly seven entries and
    // stop, not run off the record. Slot 6 must land and nothing past it may.
    f.reset();
    v                                 = f.view();
    own                               = f.store();
    f.players[1].ai_spend_ring_cursor = 0;
    for (int32_t i = 0; i < CFG_RESOURCE_SLOTS; ++i) {
        f.cfg_units[5].resource[i].id  = 1;
        f.cfg_units[5].resource[i].val = 1;
    }
    // resource_2 immediately follows resource in the record; a walk that ran past seven would eat
    // it. Seeded with a value that would be unmistakable in the tally.
    f.cfg_units[5].resource_2[0].id  = 1;
    f.cfg_units[5].resource_2[0].val = 500;
    detail::econ_track_unit_resource_spend(v, own, 1, 5);
    ck(player_of(v, 1).resource_spend_total[1] == CFG_RESOURCE_SLOTS,
       "spend: exactly SEVEN slots are walked when nothing terminates the list");

    // THE BUILDING SIBLING reads the OTHER cfg table, and that is the thing to prove -- a
    // translation that reused Unit[] would pass every assertion above and fail this one.
    f.reset();
    v                                  = f.view();
    own                                = f.store();
    f.players[4].ai_spend_ring_cursor  = 31; // the LAST ring slot, so a wrap bug is visible
    f.cfg_buildings[9].resource[0].id  = 2;
    f.cfg_buildings[9].resource[0].val = 60;
    f.cfg_buildings[9].resource[1].id  = 0;
    // Same index in the UNIT table, different numbers: if the building path read Unit[], the totals
    // below would be these instead.
    f.cfg_units[9].resource[0].id  = 3;
    f.cfg_units[9].resource[0].val = 77;

    detail::bldg_record_resource_expenditure_stats(v, own, 4, 9);
    ck(player_of(v, 4).resource_spend_total[2] == 60,
       "bldg spend: reads the BUILDING cfg table, not the unit one");
    ck(player_of(v, 4).resource_spend_total[3] == 0,
       "bldg spend: ...and the same index in Unit[] is not what it read");
    ck(player_of(v, 4).ai_spend_rate_numer_ring[31 * SPEND_RING_RESOURCE_COLS + 1] == 60,
       "bldg spend: the last ring slot (cursor 31) is addressable");
}

// ---- llm_strat_group_scratch_compute_centroid @0x0048d36f ---------------------------------------
//
// Migrated onto sim_view by SIM0 (it used to bind its own four-member `group_view`). The body did
// not change, so these cases are here to prove the MIGRATION did not change it either -- the
// function has no aitest coverage of its own and its shadow site compares nothing.
void test_group_scratch_centroid() {
    sim_fixture f;
    sim_view    v = f.view();

    // Three members clustered near (10, 20): the mean of the deltas over members 1..2 is added to
    // member 0's position.
    f.group_move_scratch[0].unit_idx = 5;
    f.group_move_scratch[1].unit_idx = 6;
    f.group_move_scratch[2].unit_idx = 7;
    f.u(1, 5).x                      = 10;
    f.u(1, 5).y                      = 20;
    f.u(1, 6).x                      = 12;
    f.u(1, 6).y                      = 20;
    f.u(1, 7).x                      = 14;
    f.u(1, 7).y                      = 26;

    int32_t cx = -1, cy = -1;
    detail::group_scratch_compute_centroid(v, 1, 3, &cx, &cy);
    // deltas (2,0) and (4,6); sums (6,6); divided by member_count-1 == 2 -> (3,3); anchor + that.
    ck(cx == 13 && cy == 23, "centroid: anchor + mean of the member deltas");

    // MEMBER COUNT 1: no division, and member 0's own row is still the answer.
    detail::group_scratch_compute_centroid(v, 1, 1, &cx, &cy);
    ck(cx == 10 && cy == 20, "centroid: member_count 1 returns the anchor, with no divide");

    // MEMBER COUNT 0 IS NOT AN EARLY RETURN -- row 0 is read anyway. That is the documented
    // difference from the AI's sibling and the case a rig cannot produce on demand.
    cx = cy = -1;
    detail::group_scratch_compute_centroid(v, 1, 0, &cx, &cy);
    ck(cx == 10 && cy == 20,
       "centroid: member_count 0 still reads row 0 -- no empty-group special case");

    // THE TORUS. A member near the far edge is closer the short way round; the wrap band is a
    // signed half-extent per axis and the final step is a TRUE modulo, not a power-of-two mask --
    // which is why the fixture's extents are 200 x 120 and not 256 x 256.
    f.group_move_scratch[1].unit_idx = 6;
    f.u(1, 5).x                      = 2;
    f.u(1, 5).y                      = 2;
    f.u(1, 6).x                      = 198;
    f.u(1, 6).y                      = 118;
    detail::group_scratch_compute_centroid(v, 1, 2, &cx, &cy);
    // dx = 196 -> > 100 -> -4; dy = 116 -> > 60 -> -4. Sum / 1 = -4. (2 - 4 + 200) % 200 = 198.
    ck(cx == 198 && cy == 118,
       "centroid: wraps the SHORT way and the final modulo is true modulo, not a mask");
}

// ---- SIM1C: the order-dispatch enqueue handlers -------------------------------------------------
//
// mh::sim::calls holds plain function pointers (it has to: production binds mh::call::* thunks), so
// the recorder is a file-scope singleton, same shape as orders_selftest.cpp's game_calls recorder.
// Every query call's RETURN is controllable through the log so a test can drive both sides of a
// branch without a real roster.
struct sim_call_log {
    int                                      resets = 0, enqueues = 0;
    std::vector<std::pair<int32_t, int32_t>> set_fields;
    struct enqueue_call {
        uint16_t unit_index, owner_and_kind;
        int16_t  param0;
        uint16_t order_code;
    };
    std::vector<enqueue_call> enqueue_calls;

    int32_t  coord_fine_x = 0, coord_fine_y = 0;
    int      get_coords_calls     = 0;
    uint8_t  select_weapon_return = 5;
    int      select_weapon_calls  = 0;
    uint32_t last_target_mask     = 0;
    int32_t  target_class_return  = 0;
    int      target_class_calls   = 0;
    uint32_t in_range_return      = 1;
    int      in_range_calls       = 0;
    int32_t  is_boarding_return   = 0;
    int      is_boarding_calls    = 0;
    int32_t  footprint_fine_x = 0, footprint_fine_y = 0;
    int      footprint_calls     = 0;
    int32_t  accepts_unit_return = 1;
    int      accepts_calls       = 0;
    int32_t  approach_fine_x = 0, approach_fine_y = 0;
    int      approach_calls                         = 0;
    int      notify_calls                           = 0;
    int      move_enqueue_calls                     = 0;
    uint32_t last_move_flag                         = 0;
    int      collect_available_projects_calls       = 0;
    uint16_t last_collect_available_projects_player = 0;

    int      move_auto_calls         = 0;
    uint16_t last_move_auto_player   = 0;
    int32_t  last_move_auto_unit_idx = 0;
    uint32_t last_move_auto_x = 0, last_move_auto_y = 0;

    int      dispatch_calls_count  = 0;
    uint16_t last_dispatch_unit_id = 0;
    uint32_t last_dispatch_player  = 0;
    uint16_t last_dispatch_op_code = 0, last_dispatch_arg = 0;
    int32_t  dispatch_return = 1;

    void reset() { *this = sim_call_log{}; }
};
sim_call_log g_sim_log;

const mh::sim::calls &recording_sim_calls() {
    static const mh::sim::calls gc = {
        []() { ++g_sim_log.resets; },
        [](int32_t idx, int32_t val) {
            g_sim_log.set_fields.push_back({idx, val});
        },
        [](uint16_t ui, uint16_t ok, int16_t p0, uint16_t oc) -> int32_t {
            ++g_sim_log.enqueues;
            g_sim_log.enqueue_calls.push_back({ui, ok, p0, oc});
            return 1;
        },
        [](uint16_t, int32_t, int32_t *ox, int32_t *oy) {
            ++g_sim_log.get_coords_calls;
            *ox = g_sim_log.coord_fine_x;
            *oy = g_sim_log.coord_fine_y;
        },
        [](uint16_t, int32_t, int32_t *ox, int32_t *oy) {
            ++g_sim_log.get_coords_calls;
            *ox = g_sim_log.coord_fine_x;
            *oy = g_sim_log.coord_fine_y;
        },
        [](uint16_t, int32_t, uint32_t mask) -> uint8_t {
            ++g_sim_log.select_weapon_calls;
            g_sim_log.last_target_mask = mask;
            return g_sim_log.select_weapon_return;
        },
        [](uint32_t, int32_t) -> int32_t {
            ++g_sim_log.target_class_calls;
            return g_sim_log.target_class_return;
        },
        [](int32_t, int32_t, int32_t, int32_t, int32_t) -> uint32_t {
            ++g_sim_log.in_range_calls;
            return g_sim_log.in_range_return;
        },
        [](int32_t) -> int32_t {
            ++g_sim_log.is_boarding_calls;
            return g_sim_log.is_boarding_return;
        },
        [](uint32_t, uint32_t, int32_t, int32_t, uint32_t *ox, uint32_t *oy) {
            ++g_sim_log.footprint_calls;
            *ox = (uint32_t)g_sim_log.footprint_fine_x;
            *oy = (uint32_t)g_sim_log.footprint_fine_y;
        },
        [](uint32_t, uint16_t) -> int32_t {
            ++g_sim_log.accepts_calls;
            return g_sim_log.accepts_unit_return;
        },
        [](uint16_t, uint16_t, uint32_t *ox, uint32_t *oy, uint32_t) {
            ++g_sim_log.approach_calls;
            *ox = (uint32_t)g_sim_log.approach_fine_x;
            *oy = (uint32_t)g_sim_log.approach_fine_y;
        },
        [](uint32_t, int32_t, uint32_t) { ++g_sim_log.notify_calls; },
        [](uint16_t, int32_t, uint32_t, uint32_t, uint32_t move_flag) {
            ++g_sim_log.move_enqueue_calls;
            g_sim_log.last_move_flag = move_flag;
        },
        [](uint16_t player) {
            ++g_sim_log.collect_available_projects_calls;
            g_sim_log.last_collect_available_projects_player = player;
        },
        [](uint16_t player, int32_t unit_idx, uint32_t x, uint32_t y) {
            ++g_sim_log.move_auto_calls;
            g_sim_log.last_move_auto_player   = player;
            g_sim_log.last_move_auto_unit_idx = unit_idx;
            g_sim_log.last_move_auto_x        = x;
            g_sim_log.last_move_auto_y        = y;
        },
        [](uint16_t unit_id, uint32_t player, uint16_t op_code, uint16_t arg) -> int32_t {
            ++g_sim_log.dispatch_calls_count;
            g_sim_log.last_dispatch_unit_id = unit_id;
            g_sim_log.last_dispatch_player  = player;
            g_sim_log.last_dispatch_op_code = op_code;
            g_sim_log.last_dispatch_arg     = arg;
            return g_sim_log.dispatch_return;
        },
    };
    return gc;
}

void test_order_recruit_unit_enqueue() {
    g_sim_log.reset();
    const int32_t rc = detail::order_recruit_unit_enqueue(recording_sim_calls(), 42, 3);
    ck(rc == 1, "recruit_unit_enqueue: always returns 1");
    ck(g_sim_log.resets == 1, "recruit_unit_enqueue: scratch_reset called once");
    ck(g_sim_log.set_fields.size() == 1 && g_sim_log.set_fields[0] == std::make_pair(1, 42),
       "recruit_unit_enqueue: sets field 1 = unit_id");
    ck(g_sim_log.enqueues == 1, "recruit_unit_enqueue: enqueue called once");
    const auto &e = g_sim_log.enqueue_calls[0];
    ck(e.unit_index == 0 && e.owner_and_kind == 3 && e.param0 == 0xf3 && e.order_code == 0xf3,
       "recruit_unit_enqueue: enqueue(0, player, 0xf3, 0xf3)");
}

void test_order_queue_construction_enqueue() {
    g_sim_log.reset();
    const int32_t rc =
        detail::order_queue_construction_enqueue(recording_sim_calls(), 11, 22, 33, 7);
    ck(rc == 1, "queue_construction_enqueue: always returns 1");
    ck(g_sim_log.resets == 1, "queue_construction_enqueue: scratch_reset called once");
    ck(g_sim_log.set_fields.size() == 3, "queue_construction_enqueue: three scratch fields set");
    ck(g_sim_log.set_fields[0] == std::make_pair(4, 11) &&
           g_sim_log.set_fields[1] == std::make_pair(5, 22) &&
           g_sim_log.set_fields[2] == std::make_pair(0, 33),
       "queue_construction_enqueue: fields 4/5/0 = arg4/arg5/arg0, in that order");
    const auto &e = g_sim_log.enqueue_calls[0];
    ck(e.unit_index == 0 && e.owner_and_kind == 7 && e.param0 == 0xf2 && e.order_code == 0xf2,
       "queue_construction_enqueue: enqueue(0, player, 0xf2, 0xf2)");
}

void test_bldg_order_production_add_enqueue() {
    sim_fixture f;
    sim_view    v = f.view();

    // Guard: online_state == 0 -> no scratch calls, no enqueue at all.
    f.b(2, 5).online_state = 0;
    g_sim_log.reset();
    detail::bldg_order_production_add_enqueue(v, recording_sim_calls(), 2, 5, 99);
    ck(g_sim_log.resets == 0 && g_sim_log.set_fields.empty() && g_sim_log.enqueues == 0,
       "repair_start_enqueue: offline building -> no order at all");

    // Guard satisfied: NO scratch_reset (unlike its siblings -- the plate is explicit about this),
    // field[0] = the passed production slot, field[1] = 1 (a fixed count), order 0x6d/0x6d, target
    // kind 0x40 (building).
    f.b(2, 5).online_state = 1;
    g_sim_log.reset();
    detail::bldg_order_production_add_enqueue(v, recording_sim_calls(), 2, 5, 99);
    ck(g_sim_log.resets == 0, "repair_start_enqueue: NO scratch_reset -- faithful to the original");
    ck(g_sim_log.set_fields.size() == 2 && g_sim_log.set_fields[0] == std::make_pair(0, 99) &&
           g_sim_log.set_fields[1] == std::make_pair(1, 1),
       "repair_start_enqueue: field[0] = unit_type, field[1] = 1");
    const auto &e = g_sim_log.enqueue_calls[0];
    ck(e.unit_index == 5 && e.owner_and_kind == (2 | 0x40) && e.param0 == 0x6d &&
           e.order_code == 0x6d,
       "repair_start_enqueue: enqueue(bldg, player|0x40, 0x6d, 0x6d)");
}

void test_bldg_order_assign_unassign_workers_enqueue() {
    g_sim_log.reset();
    detail::bldg_order_assign_workers_enqueue(recording_sim_calls(), 3, 9, 4);
    ck(g_sim_log.resets == 0, "assign_workers_enqueue: NO scratch_reset");
    ck(g_sim_log.set_fields.size() == 1 && g_sim_log.set_fields[0] == std::make_pair(1, 4),
       "assign_workers_enqueue: field[1] = worker_count");
    auto e = g_sim_log.enqueue_calls[0];
    ck(e.unit_index == 9 && e.owner_and_kind == (3 | 0x40) && e.param0 == 0x7e &&
           e.order_code == 0x7e,
       "assign_workers_enqueue: enqueue(bldg, player|0x40, 0x7e, 0x7e)");

    // THE ONE-LINE DIFFERENCE FROM ITS SIBLING: order 0x7f, not 0x7e -- guards against a copy-paste
    // that leaves both siblings issuing the same order.
    g_sim_log.reset();
    detail::bldg_order_unassign_workers_enqueue(recording_sim_calls(), 3, 9, 4);
    e = g_sim_log.enqueue_calls[0];
    ck(e.param0 == 0x7f && e.order_code == 0x7f,
       "unassign_workers_enqueue: order 0x7f/0x7f -- distinct from assign's 0x7e/0x7e");
}

void test_unit_order_exit_storage_enqueue() {
    sim_fixture f;
    sim_view    v   = f.view();
    sim_store   own = f.store();

    f.storage[2 * STORAGE_PER_PLAYER + 4].b_index     = 7;
    f.storage[2 * STORAGE_PER_PLAYER + 4].exit_tile_x = 111;
    f.storage[2 * STORAGE_PER_PLAYER + 4].exit_tile_y = 222;
    f.b(2, 7).online_state                            = 1;
    f.u(2, 3).unit_proto_id                           = 0;

    // ACCEPTED, TELEPORT ARM: move_op_arg == 10 -> order 0x24/0x38 at the storage's own exit tile;
    // field[2] = storage_idx; NO fallback move, NO seq-id write.
    // g_sim_log.reset() FIRST, every time -- it clobbers the controllable return fields back to
    // their (arbitrary) default member initializers, so setting them before reset() is a no-op
    // that silently drives the wrong branch. Caught by this test's own first run (2026-08-08).
    f.cfg_units[0].move_op_arg = 10;
    g_sim_log.reset();
    g_sim_log.accepts_unit_return = 1;
    detail::unit_order_exit_storage_enqueue(v, own, recording_sim_calls(), 2, 3, 4, 0xdeadbeef,
                                            0xdeadbeef);
    ck(g_sim_log.resets == 1, "exit_storage: accepted arm resets scratch");
    ck(g_sim_log.set_fields.size() == 3, "exit_storage: teleport arm sets 3 fields (2, 0, 1)");
    ck(g_sim_log.set_fields[0] == std::make_pair(2, 4), "exit_storage: field[2] = storage_idx");
    ck(g_sim_log.set_fields[1] == std::make_pair(0, 111) &&
           g_sim_log.set_fields[2] == std::make_pair(1, 222),
       "exit_storage: teleport arm uses the storage's OWN exit tile, no approach-tile call");
    ck(g_sim_log.approach_calls == 0, "exit_storage: teleport arm never calls storage_get_approach_tile");
    const auto &e1 = g_sim_log.enqueue_calls[0];
    ck(e1.unit_index == 3 && e1.owner_and_kind == (2 | 0x80) && e1.param0 == 0x24 &&
           e1.order_code == 0x38,
       "exit_storage: teleport arm enqueues 0x24/0x38");
    ck(g_sim_log.notify_calls == 1, "exit_storage: accepted arm notifies the unit");
    ck(g_sim_log.move_enqueue_calls == 0, "exit_storage: accepted arm never falls back to move_enqueue");

    // ACCEPTED, APPROACH-TILE ARM: move_op_arg != 10 -> calls storage_get_approach_tile and scratch
    // fields 0/1 come from ITS output, not the storage's exit tile.
    f.cfg_units[0].move_op_arg = 1;
    g_sim_log.reset();
    g_sim_log.approach_fine_x = 333;
    g_sim_log.approach_fine_y = 444;
    detail::unit_order_exit_storage_enqueue(v, own, recording_sim_calls(), 2, 3, 4, 0, 0);
    ck(g_sim_log.approach_calls == 1, "exit_storage: non-teleport arm calls storage_get_approach_tile");
    ck(g_sim_log.set_fields[1] == std::make_pair(0, 333) &&
           g_sim_log.set_fields[2] == std::make_pair(1, 444),
       "exit_storage: non-teleport arm uses the approach tile, not the storage's exit tile");
    const auto &e2 = g_sim_log.enqueue_calls[0];
    ck(e2.param0 == 0x29 && e2.order_code == 0xb, "exit_storage: non-teleport arm enqueues 0x29/0xb");

    // REJECTED: storage no longer accepts the unit -> falls back to move_enqueue at the storage's
    // OWN exit tile (not an approach tile), stamped with the CURRENT seq id (0 first time), and
    // THIS FUNCTION is the one in the whole batch that writes sim state: the seq id increments,
    // skipping 0 on wraparound.
    own.order_seq_id_by_player(2) = 0;
    g_sim_log.reset();
    g_sim_log.accepts_unit_return = 0;
    detail::unit_order_exit_storage_enqueue(v, own, recording_sim_calls(), 2, 3, 4, 0, 0);
    ck(g_sim_log.resets == 0 && g_sim_log.enqueues == 0,
       "exit_storage: rejected arm never touches the order container directly");
    ck(g_sim_log.move_enqueue_calls == 1, "exit_storage: rejected arm falls back to move_enqueue");
    ck(g_sim_log.last_move_flag == 0, "exit_storage: rejected arm passes the seq id BEFORE incrementing");
    ck(own.order_seq_id_by_player(2) == 1, "exit_storage: rejected arm increments the seq id");

    // WRAPAROUND SKIPS ZERO -- the one behaviour a rig is very unlikely to reach (255 exits by one
    // player's units in one session). accepts_unit_return is still 0 from the block above (no
    // reset() call here) -- deliberately, to keep taking the rejected path.
    own.order_seq_id_by_player(2) = 0xff;
    detail::unit_order_exit_storage_enqueue(v, own, recording_sim_calls(), 2, 3, 4, 0, 0);
    ck(own.order_seq_id_by_player(2) == 1,
       "exit_storage: seq id wraps 0xff -> 0x00 -> 0x01, never settling on 0");
}

void test_unit_order_attack_target_enqueue() {
    sim_fixture f;
    sim_view    v = f.view();

    f.u(0, 6).unit_proto_id = 0; // attacker
    f.u(1, 9).elevation     = 0; // target, ground

    // BRANCH 1 -- out of range: enqueues param0=tag, order_code=move_op_arg, and notifies.
    // move_op_arg is intentionally NOT 10 here (0x14, arbitrary) so this branch is forced by BOTH
    // in_range_return==0 AND move_op_arg!=10 independently -- belt and braces against either check
    // alone being satisfied by a fixture default.
    f.cfg_units[0].move_op_arg = 0x14;
    f.cfg_units[0].type        = 1; // not a plane
    g_sim_log.reset();
    g_sim_log.in_range_return = 0;
    g_sim_log.coord_fine_x    = 320; // -> tile 10
    g_sim_log.coord_fine_y    = 64;  // -> tile 2
    detail::unit_order_attack_target_enqueue(v, recording_sim_calls(), 0, 6, 1, 9, 0xffffffffu,
                                             0x1a);
    ck(g_sim_log.select_weapon_calls == 1 && g_sim_log.last_target_mask == 1,
       "attack_target: auto-select with elevation==0 uses target_mask=1 (ground)");
    ck(g_sim_log.set_fields[0] == std::make_pair(0, 10) &&
           g_sim_log.set_fields[1] == std::make_pair(1, 2),
       "attack_target: tile fields are the fine coords truncating-divided by 32");
    const auto &e1 = g_sim_log.enqueue_calls[0];
    ck(e1.param0 == 0x1a && e1.order_code == 0x14,
       "attack_target: out-of-range arm -> param0=tag(0x1a), order_code=move_op_arg");
    ck(g_sim_log.notify_calls == 1, "attack_target: out-of-range arm notifies the unit");

    // BRANCH 1 with tag=0x1b (the _alt sibling) -- SAME shape, only the tag changes.
    g_sim_log.reset();
    g_sim_log.in_range_return = 0;
    detail::unit_order_attack_target_enqueue(v, recording_sim_calls(), 0, 6, 1, 9, 5, 0x1b);
    ck(g_sim_log.select_weapon_calls == 0,
       "attack_target: an explicit (non-sentinel) weapon_id skips weapon auto-select");
    ck(g_sim_log.enqueue_calls[0].param0 == 0x1b,
       "attack_target_alt: out-of-range arm carries tag 0x1b, not target's 0x1a");

    // BRANCH 2 -- HELICOPTER: order is FIXED 0x1a/0x2e regardless of `tag`. This is the one branch
    // the target/target_alt pair do NOT differ on.
    f.cfg_units[0].move_op_arg = 10;
    f.cfg_units[0].type        = UNIT_TYPE_A_HELI;
    g_sim_log.reset();
    g_sim_log.in_range_return    = 1;
    g_sim_log.is_boarding_return = 0;
    detail::unit_order_attack_target_enqueue(v, recording_sim_calls(), 0, 6, 1, 9, 5, 0x1b);
    const auto &e2 = g_sim_log.enqueue_calls[0];
    ck(e2.param0 == 0x1a && e2.order_code == 0x2e,
       "attack_target_alt: heli arm is FIXED 0x1a/0x2e even though tag is 0x1b");
    ck(g_sim_log.notify_calls == 0, "attack_target: heli arm does not notify");

    // BRANCH 3 -- direct attack (ground mover, in range, not boarding): param0=1, order_code=tag.
    f.cfg_units[0].type = 2; // not a plane, not a heli
    g_sim_log.reset();
    detail::unit_order_attack_target_enqueue(v, recording_sim_calls(), 0, 6, 1, 9, 5, 0x1a);
    const auto &e3 = g_sim_log.enqueue_calls[0];
    ck(e3.param0 == 1 && e3.order_code == 0x1a,
       "attack_target: direct-attack arm -> param0=1, order_code=tag");
}

void test_unit_order_attack_unit_enqueue() {
    sim_fixture f;
    sim_view    v       = f.view();
    f.u(6, 2).elevation = 5; // TARGET (player=6, idx=2) elevated -> air mask

    g_sim_log.reset();
    detail::unit_order_attack_unit_enqueue(v, recording_sim_calls(), 4, 1, 6, 2, 0xffffffffu);
    ck(g_sim_log.last_target_mask == 2, "attack_unit: elevated target -> target_mask=2 (air)");
    ck(g_sim_log.set_fields[2] == std::make_pair(10, (int32_t)(6u | 0x80u)),
       "attack_unit: field[10] = target_player|0x80 (unit kind)");
    ck(g_sim_log.set_fields[3] == std::make_pair(0xb, 2), "attack_unit: field[0xb] = target index");
    const auto &e = g_sim_log.enqueue_calls[0];
    ck(e.unit_index == 1 && e.owner_and_kind == (4 | 0x80) && e.param0 == 0x1e &&
           e.order_code == 0x1e,
       "attack_unit: always enqueue(unit, player|0x80, 0x1e, 0x1e) -- no branching");
}

void test_unit_order_attack_building_enqueue() {
    sim_fixture f;
    sim_view    v = f.view();

    g_sim_log.reset();
    detail::unit_order_attack_building_enqueue(v, recording_sim_calls(), 4, 1, 6, 3, 0xffffffffu);
    ck(g_sim_log.last_target_mask == 1,
       "attack_building: auto-select ALWAYS uses target_mask=1 -- buildings have no elevation");
    ck(g_sim_log.footprint_calls == 1, "attack_building: always jitters onto the footprint");
    ck(g_sim_log.set_fields[2] == std::make_pair(10, (int32_t)(6u | 0x40u)),
       "attack_building: field[10] = target_player|0x40 (BUILDING kind)");
    const auto &e = g_sim_log.enqueue_calls[0];
    ck(e.param0 == 0x1d && e.order_code == 0x1d, "attack_building: enqueue(.., 0x1d, 0x1d)");
}

void test_unit_order_attack_building_reposition_alt_enqueue() {
    sim_fixture f;
    sim_view    v           = f.view();
    f.u(0, 6).unit_proto_id = 0;

    // OUT OF RANGE: the initial aim point comes from bldg_get_coords, NOT footprint_random_offset --
    // that call is confined to the in-range arm below, so this arm's footprint_calls stays ZERO.
    // move_op_arg deliberately != 10 (0x14) so the branch is forced by BOTH conditions
    // independently, not by whichever the fixture happened to default to.
    f.cfg_units[0].move_op_arg = 0x14;
    g_sim_log.reset();
    g_sim_log.in_range_return = 0;
    g_sim_log.coord_fine_x    = 320; // tile 10
    g_sim_log.coord_fine_y    = 64;  // tile 2
    detail::unit_order_attack_building_reposition_alt_enqueue(v, recording_sim_calls(), 0, 6, 1, 3,
                                                              5);
    ck(g_sim_log.footprint_calls == 0,
       "reposition_alt: out-of-range arm never jitters -- the initial point is bldg_get_coords");
    ck(g_sim_log.set_fields.size() == 7, "reposition_alt: out-of-range arm writes only the initial 7 fields");
    const auto &e1 = g_sim_log.enqueue_calls[0];
    ck(e1.param0 == 0x1c && e1.order_code == 0x14,
       "reposition_alt: out-of-range arm -> param0=0x1c, order_code=move_op_arg");
    ck(g_sim_log.notify_calls == 1, "reposition_alt: out-of-range arm notifies");

    // IN RANGE: footprint_random_offset is called EXACTLY ONCE (re-jittering the initial aim
    // point), and tile/fine fields are OVERWRITTEN with the fresh point -- owner/index/weapon
    // (fields 5/6/7, set only on the initial pass) are NOT re-set. 7 initial + 4 more = 11 total.
    f.cfg_units[0].move_op_arg = 10;
    g_sim_log.reset();
    g_sim_log.in_range_return    = 1;
    g_sim_log.is_boarding_return = 0;
    g_sim_log.coord_fine_x       = 320; // tile 10 -- the INITIAL point (from bldg_get_coords)
    g_sim_log.coord_fine_y       = 64;  // tile 2
    g_sim_log.footprint_fine_x   = 640; // tile 20 -- the JITTERED point (from footprint_random_offset)
    g_sim_log.footprint_fine_y   = 96;  // tile 3
    detail::unit_order_attack_building_reposition_alt_enqueue(v, recording_sim_calls(), 0, 6, 1, 3,
                                                              5);
    ck(g_sim_log.footprint_calls == 1, "reposition_alt: in-range arm jitters EXACTLY ONCE");
    ck(g_sim_log.set_fields.size() == 11,
       "reposition_alt: 11 field writes total -- 7 initial + 4 more on the in-range pass");
    ck(g_sim_log.set_fields[0] == std::make_pair(0, 10) && g_sim_log.set_fields[1] == std::make_pair(1, 2),
       "reposition_alt: the INITIAL tile fields come from bldg_get_coords, not the jitter");
    ck(g_sim_log.set_fields[7] == std::make_pair(0, 20) &&
           g_sim_log.set_fields[8] == std::make_pair(1, 3) &&
           g_sim_log.set_fields[9] == std::make_pair(3, 640) &&
           g_sim_log.set_fields[10] == std::make_pair(4, 96),
       "reposition_alt: the in-range pass overwrites tile_x/tile_y/fine_x/fine_y with the FRESH jitter");
    const auto &e2 = g_sim_log.enqueue_calls[0];
    ck(e2.param0 == 1 && e2.order_code == 0x1c,
       "reposition_alt: in-range arm -> param0=1, order_code=0x1c");
    ck(g_sim_log.notify_calls == 0, "reposition_alt: in-range arm does not notify");
}

void test_unit_order_auto_launch_from_storage_enqueue() {
    sim_fixture f;
    sim_view    v                                     = f.view();
    f.storage[3 * STORAGE_PER_PLAYER + 2].b_index     = 8;
    f.storage[3 * STORAGE_PER_PLAYER + 2].exit_tile_x = 50;
    f.storage[3 * STORAGE_PER_PLAYER + 2].exit_tile_y = 70;
    f.u(3, 1).home_storage_slot                       = 2;
    f.cfg_units[0].default_op_code                    = 0x13;

    // PARKED + operational + EXPLICIT x/y: masked against width/height, storage's own tile unused.
    f.u(3, 1).state         = UNIT_STATE_PARKED;
    f.u(3, 1).unit_proto_id = 0;
    f.b(3, 8).built_flags   = BUILT_FLAGS_OPERATIONAL;
    g_sim_log.reset();
    detail::unit_order_auto_launch_from_storage_enqueue(v, recording_sim_calls(), 3, 1, 300, 500);
    ck(g_sim_log.resets == 1, "auto_launch: PARKED+operational resets scratch");
    ck(g_sim_log.set_fields[0] == std::make_pair(0, (int32_t)(300u & 0xffu)) &&
           g_sim_log.set_fields[1] == std::make_pair(1, (int32_t)(500u & 0x3fu)),
       "auto_launch: explicit x/y masked against width_mask/height_mask (0xff/0x3f in this fixture)");
    const auto &e1 = g_sim_log.enqueue_calls[0];
    ck(e1.param0 == 0x13 && e1.order_code == 0x20,
       "auto_launch: enqueue(.., default_op_code, 0x20)");

    // PARKED + operational + SENTINEL x==0xffffffff: falls back to the storage's OWN exit tile,
    // ADD-THEN-MASK (`(exit_tile + 2) & mask`, not `(exit_tile & mask) + 2`).
    g_sim_log.reset();
    detail::unit_order_auto_launch_from_storage_enqueue(v, recording_sim_calls(), 3, 1, 0xffffffffu,
                                                        0xffffffffu);
    ck(g_sim_log.set_fields[0] == std::make_pair(0, (int32_t)((50u + 2u) & 0xffu)) &&
           g_sim_log.set_fields[1] == std::make_pair(1, (int32_t)((70u + 2u) & 0x3fu)),
       "auto_launch: sentinel x/y -> (storage exit_tile + 2) & mask, add BEFORE mask");

    // NOT OPERATIONAL: no order at all.
    f.b(3, 8).built_flags = 0x1; // connected but not staffed
    g_sim_log.reset();
    detail::unit_order_auto_launch_from_storage_enqueue(v, recording_sim_calls(), 3, 1, 300, 500);
    ck(g_sim_log.resets == 0 && g_sim_log.enqueues == 0,
       "auto_launch: PARKED but not fully operational (built_flags != 3) -> no-op");

    // EXIT_WAIT: unconditional order 0x1f/0x23, independent of the storage/building state above.
    f.u(3, 1).state = UNIT_STATE_EXIT_WAIT;
    g_sim_log.reset();
    detail::unit_order_auto_launch_from_storage_enqueue(v, recording_sim_calls(), 3, 1, 300, 500);
    const auto &e2 = g_sim_log.enqueue_calls[0];
    ck(e2.param0 == 0x1f && e2.order_code == 0x23, "auto_launch: EXIT_WAIT -> enqueue(.., 0x1f, 0x23)");

    // ANY OTHER STATE: neither gate matches -> no-op.
    f.u(3, 1).state = 0x11; // MOVE_PATH, neither PARKED nor EXIT_WAIT
    g_sim_log.reset();
    detail::unit_order_auto_launch_from_storage_enqueue(v, recording_sim_calls(), 3, 1, 300, 500);
    ck(g_sim_log.resets == 0 && g_sim_log.enqueues == 0,
       "auto_launch: neither PARKED nor EXIT_WAIT -> no-op");
}

// ---- SIM1C: activate/upgrade/restart_construction/cancel_construction, the two raw
// order-scratch enqueues, the unit-order-settled predicate, and the collect-available-projects thunk
// -------------------------------------------------------------------------------------------------
void test_bldg_order_activate_upgrade_restart_construction_enqueue() {
    // Byte-identical shape, one order code apiece -- no scratch, no gate. Distinct player/bldg_idx
    // per call so a copy-paste that swapped the two arguments would be visible in the enqueue record.
    g_sim_log.reset();
    detail::bldg_order_activate_enqueue(recording_sim_calls(), 3, 9);
    ck(g_sim_log.resets == 0 && g_sim_log.set_fields.empty(),
       "activate_enqueue: no scratch_reset, no scratch fields");
    auto e = g_sim_log.enqueue_calls[0];
    ck(e.unit_index == 9 && e.owner_and_kind == (3 | 0x40) && e.param0 == 0x80 && e.order_code == 0x80,
       "activate_enqueue: enqueue(bldg, player|0x40, 0x80, 0x80)");

    g_sim_log.reset();
    detail::bldg_order_upgrade_enqueue(recording_sim_calls(), 3, 9);
    e = g_sim_log.enqueue_calls[0];
    ck(e.param0 == 0x82 && e.order_code == 0x82,
       "upgrade_enqueue: order 0x82/0x82 -- distinct from activate's 0x80/0x80");

    g_sim_log.reset();
    detail::bldg_order_restart_construction_enqueue(recording_sim_calls(), 3, 9);
    e = g_sim_log.enqueue_calls[0];
    ck(e.param0 == 0x6b && e.order_code == 0x6b,
       "restart_construction_enqueue: order 0x6b/0x6b -- distinct from its two siblings");
}

// ---- llm_strat_bldg_order_repair_cycle_start_enqueue @0x0046e409 -------------------------------
// DESPITE THE NAME NOTHING IS CANCELLED (the order-dispatch notes): order 0x6a starts a
// charge/work cycle, gated on the building being BELOW its cfg max energy AND not a main-base type.
void test_bldg_order_repair_cycle_start_enqueue() {
    sim_fixture f;
    sim_view    v = f.view();

    f.b(2, 5).building_id      = 17;
    f.cfg_buildings[17].energy = 100.0;
    f.cfg_buildings[17].type   = 3; // neither main-base variant

    // BELOW max, ordinary type -> enqueues.
    f.b(2, 5).energy = 40.0;
    g_sim_log.reset();
    detail::bldg_order_repair_cycle_start_enqueue(v, recording_sim_calls(), 2, 5);
    ck(g_sim_log.enqueues == 1, "cancel_construction: energy below cfg max -> enqueues");
    const auto &e = g_sim_log.enqueue_calls[0];
    ck(e.unit_index == 5 && e.owner_and_kind == (2 | 0x40) && e.param0 == 0x6a && e.order_code == 0x6a,
       "cancel_construction: enqueue(bldg, player|0x40, 0x6a, 0x6a)");

    // AT max energy -- the guard is a STRICT '<', so equal does NOT enqueue.
    f.b(2, 5).energy = 100.0;
    g_sim_log.reset();
    detail::bldg_order_repair_cycle_start_enqueue(v, recording_sim_calls(), 2, 5);
    ck(g_sim_log.enqueues == 0, "cancel_construction: energy == cfg max -> no order (strict <, not <=)");

    // ABOVE max -- also no order.
    f.b(2, 5).energy = 150.0;
    g_sim_log.reset();
    detail::bldg_order_repair_cycle_start_enqueue(v, recording_sim_calls(), 2, 5);
    ck(g_sim_log.enqueues == 0, "cancel_construction: energy above cfg max -> no order");

    // BELOW max but a H_MAIN_BASE -- excluded regardless of energy.
    f.b(2, 5).energy         = 40.0;
    f.cfg_buildings[17].type = BUILDING_TYPE_H_MAIN_BASE;
    g_sim_log.reset();
    detail::bldg_order_repair_cycle_start_enqueue(v, recording_sim_calls(), 2, 5);
    ck(g_sim_log.enqueues == 0, "cancel_construction: H_MAIN_BASE is excluded even when below max");

    // BELOW max but an A_MAIN_BASE -- excluded too (the SECOND exclusion, not just the first).
    f.cfg_buildings[17].type = BUILDING_TYPE_A_MAIN_BASE;
    g_sim_log.reset();
    detail::bldg_order_repair_cycle_start_enqueue(v, recording_sim_calls(), 2, 5);
    ck(g_sim_log.enqueues == 0, "cancel_construction: A_MAIN_BASE is excluded too");

    // THE cfg TABLE, NOT THE INSTANCE: building_id indexes cfg_buildings, and a DIFFERENT building_id
    // reads a different max -- proves the lookup goes through building_id, not the roster slot itself.
    f.reset();
    v                         = f.view();
    f.b(4, 1).building_id     = 3;
    f.cfg_buildings[3].energy = 10.0;
    f.cfg_buildings[3].type   = 1;
    f.b(4, 1).energy          = 40.0; // ABOVE cfg[3]'s max
    g_sim_log.reset();
    detail::bldg_order_repair_cycle_start_enqueue(v, recording_sim_calls(), 4, 1);
    ck(g_sim_log.enqueues == 0,
       "cancel_construction: reads Building[building_id], not a fixed/instance-indexed max");
}

// ---- llm_strat_order_grant_resource_raw @0x0046f592 ---------------------------------------------
void test_order_grant_resource_raw() {
    g_sim_log.reset();
    detail::order_grant_resource_raw(recording_sim_calls(), 5, 3, 77);
    ck(g_sim_log.resets == 1, "grant_resource_raw: scratch_reset called once");
    ck(g_sim_log.set_fields.size() == 2 && g_sim_log.set_fields[0] == std::make_pair(3, 3) &&
           g_sim_log.set_fields[1] == std::make_pair(2, 77),
       "grant_resource_raw: field 3 = res_type, field 2 = amount, in that order");
    const auto &e = g_sim_log.enqueue_calls[0];
    ck(e.unit_index == 0 && e.owner_and_kind == 5 && e.param0 == (int16_t)0xed &&
           e.order_code == 0xed,
       "grant_resource_raw: enqueue(0, player, 0xed, 0xed)");
}

// ---- llm_strat_order_population_delta_enqueue @0x0046f63a ---------------------------------------
void test_order_population_delta_enqueue() {
    g_sim_log.reset();
    detail::order_population_delta_enqueue(recording_sim_calls(), 6, -4);
    ck(g_sim_log.resets == 1, "population_delta_enqueue: scratch_reset called once");
    ck(g_sim_log.set_fields.size() == 1 && g_sim_log.set_fields[0] == std::make_pair(1, -4),
       "population_delta_enqueue: field 1 = population_delta (negative values pass through unchanged)");
    const auto &e = g_sim_log.enqueue_calls[0];
    ck(e.unit_index == 0 && e.owner_and_kind == 6 && e.param0 == (int16_t)0xec &&
           e.order_code == 0xec,
       "population_delta_enqueue: enqueue(0, player, 0xec, 0xec)");
}

// ---- llm_strat_unit_order_state_is_settled @0x004d4158 -------------------------------------------
// Pure predicate over (order, state); no callee at all. Four settled/transition pairs, and each half
// of each pair is tested independently so a translation that swapped `order`/`state` on one arm, or
// mismatched a pair's two values, is caught -- not just "some pair matched, some pair didn't".
void test_unit_order_state_is_settled() {
    sim_fixture f;
    sim_view    v = f.view();

    auto set = [&](int32_t player, int32_t idx, uint16_t order, uint16_t state) {
        f.u(player, idx).order = order;
        f.u(player, idx).state = state;
    };

    set(1, 0, UNIT_STATE_STOP_TO_DEFAULT, UNIT_STATE_STOP_TO_DEFAULT);
    ck(detail::unit_order_state_is_settled(v, 1, 0) == 1,
       "state_is_settled: pair 1 (STOP_TO_DEFAULT, STOP_TO_DEFAULT) -> settled");

    set(1, 1, UNIT_STATE_PATROL_SWAP, UNIT_STATE_MOVE_PATH);
    ck(detail::unit_order_state_is_settled(v, 1, 1) == 1,
       "state_is_settled: pair 2 (PATROL_SWAP, MOVE_PATH) -> settled");

    set(1, 2, UNIT_STATE_PATROL_SWAP, UNIT_STATE_IDLE_SCATTER);
    ck(detail::unit_order_state_is_settled(v, 1, 2) == 1,
       "state_is_settled: pair 3 (PATROL_SWAP, IDLE_SCATTER) -> settled");

    set(1, 3, UNIT_STATE_IDLE_SCATTER, UNIT_STATE_HOVER_ENGAGE);
    ck(detail::unit_order_state_is_settled(v, 1, 3) == 1,
       "state_is_settled: pair 4 (IDLE_SCATTER, HOVER_ENGAGE) -> settled");

    // ORDER AND STATE ARE NOT INTERCHANGEABLE -- swapping pair 2's two values must NOT match, which
    // is what separates a translation comparing the right FIELD from one that got order/state backwards.
    set(1, 4, UNIT_STATE_MOVE_PATH, UNIT_STATE_PATROL_SWAP);
    ck(detail::unit_order_state_is_settled(v, 1, 4) == 0,
       "state_is_settled: (MOVE_PATH, PATROL_SWAP) -- pair 2 REVERSED -- is NOT settled");

    // A near-miss: order matches pair 3's order half, but state matches NEITHER of pair 2's or pair
    // 3's state halves.
    set(1, 5, UNIT_STATE_PATROL_SWAP, UNIT_STATE_STOP_TO_DEFAULT);
    ck(detail::unit_order_state_is_settled(v, 1, 5) == 0,
       "state_is_settled: PATROL_SWAP order with an unmatched state -> not settled");

    // Both fields zero (a freshly-zeroed unit) -> not settled.
    set(1, 6, 0, 0);
    ck(detail::unit_order_state_is_settled(v, 1, 6) == 0,
       "state_is_settled: (0, 0) does not coincidentally match any of the four pairs");

    // The row stride: player 1 slot 0 must not read player 0 slot 100.
    ck(detail::unit_order_state_is_settled(v, 0, 0) == 0,
       "state_is_settled: reads its OWN player's row, not a stride-off neighbour");
}

// ---- llm_strat_order_collect_available_projects_thunk @0x004e611f --------------------------------
// Stack-probe thunk with no logic of its own -- forwards `player`, narrowed to uint16_t, to the
// (still-original) frontier callee.
void test_order_collect_available_projects_thunk() {
    g_sim_log.reset();
    detail::order_collect_available_projects_thunk(recording_sim_calls(), 0x10007); // high bits set
    ck(g_sim_log.collect_available_projects_calls == 1,
       "collect_available_projects_thunk: forwards to the frontier callee exactly once");
    ck(g_sim_log.last_collect_available_projects_player == 7,
       "collect_available_projects_thunk: narrows player to uint16_t before the call (0x10007 -> 7)");
}

// ---- batch C's remaining plain order-issue wrappers ---------------------------------

// ---- llm_strat_order_collect_available_projects_enqueue @0x0046f6c1 -------------------------------
void test_order_collect_available_projects_enqueue() {
    g_sim_log.reset();
    detail::order_collect_available_projects_enqueue(recording_sim_calls(), 5);
    ck(g_sim_log.resets == 0, "collect_available_projects_enqueue: no scratch_reset");
    ck(g_sim_log.set_fields.empty(), "collect_available_projects_enqueue: no scratch fields set");
    ck(g_sim_log.enqueues == 1, "collect_available_projects_enqueue: enqueue called once");
    const auto &e = g_sim_log.enqueue_calls[0];
    ck(e.unit_index == 0 && e.owner_and_kind == 5 && e.param0 == (int16_t)0xee &&
           e.order_code == 0xee,
       "collect_available_projects_enqueue: enqueue(0, player, 0xee, 0xee)");
}

// ---- llm_strat_unit_issue_default_order @0x00469e72 ---------------------------------------------
void test_unit_issue_default_order() {
    sim_fixture f;
    sim_view    v = f.view();

    f.u(2, 9).unit_proto_id        = 5;
    f.cfg_units[5].default_op_code = 0x13; // air

    g_sim_log.reset();
    detail::unit_issue_default_order(v, recording_sim_calls(), 2, 9);
    ck(g_sim_log.resets == 1, "issue_default_order: scratch_reset called once");
    ck(g_sim_log.set_fields.empty(), "issue_default_order: no scratch fields set");
    const auto &e = g_sim_log.enqueue_calls[0];
    ck(e.unit_index == 9 && e.owner_and_kind == (2 | 0x80) && e.param0 == 0x13 && e.order_code == 0x13,
       "issue_default_order: enqueue(unit, player|0x80, default_op_code, default_op_code) -- BOTH "
       "op_code and arg carry the SAME cfg value");
}

// ---- llm_strat_unit_order_move_enqueue @0x0046a0d5 -----------------------------------------------
void test_unit_order_move_enqueue() {
    sim_fixture f;
    sim_view    v = f.view();

    f.u(3, 4).unit_proto_id     = 6;
    f.cfg_units[6].move_op_code = 0x11;
    f.cfg_units[6].move_op_arg  = 0xb;

    g_sim_log.reset();
    detail::unit_order_move_enqueue(v, recording_sim_calls(), 3, 4, 100, 200, 42);
    ck(g_sim_log.resets == 1, "move_enqueue: scratch_reset called once");
    ck(g_sim_log.set_fields.size() == 3 && g_sim_log.set_fields[0] == std::make_pair(0, 100) &&
           g_sim_log.set_fields[1] == std::make_pair(1, 200) &&
           g_sim_log.set_fields[2] == std::make_pair(0xc, 42),
       "move_enqueue: fields 0/1/0xc = x/y/move_flag, in that order");
    const auto &e = g_sim_log.enqueue_calls[0];
    ck(e.unit_index == 4 && e.owner_and_kind == (3 | 0x80) && e.param0 == 0x11 && e.order_code == 0xb,
       "move_enqueue: enqueue(unit, player|0x80, move_op_code, move_op_arg)");
    ck(g_sim_log.notify_calls == 1, "move_enqueue: unit_notify_status called once");
}

// ---- llm_strat_unit_order_move_auto @0x0046a56d --------------------------------------------------
// Same shape as move_enqueue, minus the move_flag scratch field -- that omission is the whole
// difference between the two, so it gets its own assertion (a copy-paste that added field 0xc back
// in would pass every OTHER check here).
void test_unit_order_move_auto() {
    sim_fixture f;
    sim_view    v = f.view();

    f.u(1, 2).unit_proto_id     = 8;
    f.cfg_units[8].move_op_code = 0x12;
    f.cfg_units[8].move_op_arg  = 0xb;

    g_sim_log.reset();
    detail::unit_order_move_auto(v, recording_sim_calls(), 1, 2, 55, 66);
    ck(g_sim_log.resets == 1, "move_auto: scratch_reset called once");
    ck(g_sim_log.set_fields.size() == 2 && g_sim_log.set_fields[0] == std::make_pair(0, 55) &&
           g_sim_log.set_fields[1] == std::make_pair(1, 66),
       "move_auto: ONLY fields 0/1 = x/y -- no move_flag field, unlike move_enqueue");
    const auto &e = g_sim_log.enqueue_calls[0];
    ck(e.unit_index == 2 && e.owner_and_kind == (1 | 0x80) && e.param0 == 0x12 && e.order_code == 0xb,
       "move_auto: enqueue(unit, player|0x80, move_op_code, move_op_arg)");
    ck(g_sim_log.notify_calls == 1, "move_auto: unit_notify_status called once");
}

// ---- llm_strat_unit_order_exit_storage_auto @0x0046ab08 ------------------------------------------
// The "_auto" twin of unit_order_exit_storage_enqueue -- same two-gate acceptance test and
// teleport/approach-tile branch, but the rejected-storage fallback goes to move_auto (via the
// `calls` table, NOT the sibling detail:: function directly) rather than move_enqueue, and does NOT
// stamp an order-sequence id.
void test_unit_order_exit_storage_auto() {
    sim_fixture f;
    sim_view    v = f.view();

    f.storage[2 * STORAGE_PER_PLAYER + 4].b_index     = 7;
    f.storage[2 * STORAGE_PER_PLAYER + 4].exit_tile_x = 111;
    f.storage[2 * STORAGE_PER_PLAYER + 4].exit_tile_y = 222;
    f.b(2, 7).online_state                            = 1;
    f.u(2, 3).unit_proto_id                           = 0;

    // ACCEPTED, TELEPORT ARM: move_op_arg == 10 -> order 0x24/0x38 at the storage's own exit tile;
    // field[2] = storage_idx; NO fallback move.
    f.cfg_units[0].move_op_arg = 10;
    g_sim_log.reset();
    detail::unit_order_exit_storage_auto(v, recording_sim_calls(), 2, 3, 4, 0xdeadbeef, 0xcafef00d);
    ck(g_sim_log.resets == 1 && g_sim_log.move_auto_calls == 0,
       "exit_storage_auto: accepted, teleport arm -- scratch_reset, no move_auto fallback");
    ck(g_sim_log.set_fields.size() == 3 && g_sim_log.set_fields[0] == std::make_pair(2, 4) &&
           g_sim_log.set_fields[1] == std::make_pair(0, 111) &&
           g_sim_log.set_fields[2] == std::make_pair(1, 222),
       "exit_storage_auto: field[2] = storage_idx, field[0]/[1] = the storage's own exit tile");
    const auto &e = g_sim_log.enqueue_calls[0];
    ck(e.unit_index == 3 && e.owner_and_kind == (2 | 0x80) && e.param0 == 0x24 && e.order_code == 0x38,
       "exit_storage_auto: enqueue(unit, player|0x80, 0x24, 0x38) on the teleport arm");
    ck(g_sim_log.notify_calls == 1, "exit_storage_auto: unit_notify_status called once on accept");

    // ACCEPTED, APPROACH-TILE ARM: move_op_arg != 10 -> storage_get_approach_tile, order 0x29/0xb.
    f.cfg_units[0].move_op_arg = 5;
    g_sim_log.reset();
    g_sim_log.approach_fine_x = 333; // reset() clobbers controllable fields -- set AFTER, always
    g_sim_log.approach_fine_y = 444;
    detail::unit_order_exit_storage_auto(v, recording_sim_calls(), 2, 3, 4, 0, 0);
    ck(g_sim_log.approach_calls == 1, "exit_storage_auto: approach arm calls storage_get_approach_tile");
    const auto &e2 = g_sim_log.enqueue_calls[0];
    ck(e2.param0 == 0x29 && e2.order_code == 0xb, "exit_storage_auto: enqueue(..., 0x29, 0xb) on the approach arm");
    ck(g_sim_log.set_fields[1] == std::make_pair(0, 333) && g_sim_log.set_fields[2] == std::make_pair(1, 444),
       "exit_storage_auto: field[0]/[1] = the computed approach tile, not the storage's own exit tile");

    // REJECTED: offline storage building -> falls back to move_auto (via `calls`, not the sibling
    // detail:: function) at the storage's own exit tile. NO order-sequence stamp (unlike the
    // _enqueue twin) -- move_auto's own committed signature has no move_flag slot for one.
    f.b(2, 7).online_state = 0;
    g_sim_log.reset();
    detail::unit_order_exit_storage_auto(v, recording_sim_calls(), 2, 3, 4, 0, 0);
    ck(g_sim_log.resets == 0 && g_sim_log.enqueues == 0,
       "exit_storage_auto: rejected -- no direct order-triad call from THIS function");
    ck(g_sim_log.move_auto_calls == 1, "exit_storage_auto: rejected -- falls back to move_auto exactly once");
    ck(g_sim_log.last_move_auto_player == 2 && g_sim_log.last_move_auto_unit_idx == 3 &&
           g_sim_log.last_move_auto_x == 111 && g_sim_log.last_move_auto_y == 222,
       "exit_storage_auto: move_auto(player, unit, storage's exit_tile_x, exit_tile_y)");
}

// ---- llm_strat_order_ctrlgrp_select_member / _flash_member @0x0046f1de / 0x0046f278 --------------
// Identical shape apart from the order code (0x34 vs 0x35) -- both call order_dispatch (bound to
// mh::call::llm_strat_order_dispatch, ALREADY reimplemented+promoted under RI-ORDERS, NOT a body
// this test exercises) rather than the order triad directly.
void test_order_ctrlgrp_select_flash_member() {
    g_sim_log.reset();
    detail::order_ctrlgrp_select_member(recording_sim_calls(), 3, 9, 77);
    ck(g_sim_log.resets == 1, "ctrlgrp_select_member: scratch_reset called once");
    ck(g_sim_log.set_fields.size() == 1 && g_sim_log.set_fields[0] == std::make_pair(0, 77),
       "ctrlgrp_select_member: field 0 = a2");
    ck(g_sim_log.dispatch_calls_count == 1 && g_sim_log.last_dispatch_unit_id == 9 &&
           g_sim_log.last_dispatch_player == 3 && g_sim_log.last_dispatch_op_code == 0x34 &&
           g_sim_log.last_dispatch_arg == 0x34,
       "ctrlgrp_select_member: order_dispatch(unit_id, player, 0x34, 0x34)");

    g_sim_log.reset();
    detail::order_ctrlgrp_flash_member(recording_sim_calls(), 3, 9, 77);
    ck(g_sim_log.dispatch_calls_count == 1 && g_sim_log.last_dispatch_op_code == 0x35 &&
           g_sim_log.last_dispatch_arg == 0x35,
       "ctrlgrp_flash_member: order 0x35/0x35 -- distinct from select_member's 0x34/0x34");
}

// ---- llm_unit_set_order_param @0x0048696f ---------------------------------------------------------
// Direct sim_store write, no callees at all.
void test_unit_set_order_param() {
    sim_fixture f;
    sim_store   own = f.store();

    detail::unit_set_order_param(own, 4, 6, (int16_t)0x2b);
    ck(f.u(4, 6).order == 0x2b, "set_order_param: writes units[player][unit_index].order, nothing else");
    ck(f.u(4, 6).state == 0, "set_order_param: does NOT touch the neighbouring `state` field");
}

} // namespace

int run_simtest() {
    printf("=== simtest (SIM0: the strategic sim's logic over heap buffers) ===\n");
    test_fixture_halves_agree();
    test_bldg_is_alive();
    test_unit_weapon_predicates();
    test_unit_max_weapon_range();
    test_unit_status_bits();
    test_resource_spend();
    test_group_scratch_centroid();
    test_order_recruit_unit_enqueue();
    test_order_queue_construction_enqueue();
    test_bldg_order_production_add_enqueue();
    test_bldg_order_assign_unassign_workers_enqueue();
    test_unit_order_exit_storage_enqueue();
    test_unit_order_attack_target_enqueue();
    test_unit_order_attack_unit_enqueue();
    test_unit_order_attack_building_enqueue();
    test_unit_order_attack_building_reposition_alt_enqueue();
    test_unit_order_auto_launch_from_storage_enqueue();
    test_bldg_order_activate_upgrade_restart_construction_enqueue();
    test_bldg_order_repair_cycle_start_enqueue();
    test_order_grant_resource_raw();
    test_order_population_delta_enqueue();
    test_unit_order_state_is_settled();
    test_order_collect_available_projects_thunk();

    test_order_collect_available_projects_enqueue();
    test_unit_issue_default_order();
    test_unit_order_move_enqueue();
    test_unit_order_move_auto();
    test_unit_order_exit_storage_auto();
    test_order_ctrlgrp_select_flash_member();
    test_unit_set_order_param();
    mh::sim::test::run_debug_roll_random_tests();

    // SIM1-V (2026-08-23): the four substantive rows of the own-body-scan
    // residue. Two others in that residue (unit_queue_advance, transfer_notify_noop) were
    // deliberately NOT given oracles -- a pure forward and an empty body have nothing to test.
    mh::sim::test::run_fx_anim_chain_find_tail_tests();
    mh::sim::test::run_fx_anim_seq_cancel_tests();
    mh::sim::test::run_order_issue_0xf_adjacent_tests();
    mh::sim::test::run_storage_get_approach_tile_tests();

    mh::sim::test::run_prod_shuttle_slot_release_tests();
    mh::sim::test::run_storage_cancel_pending_docked_tests();
    mh::sim::test::run_storage_release_door_held_by_unit_tests();
    mh::sim::test::run_unit_select_weapon_tests();
    mh::sim::test::run_prod_shuttle_slot_bind_default_tests();
    mh::sim::test::run_prod_shuttle_load_resource_tests();
    mh::sim::test::run_storage_launch_parked_to_orbit_tests();
    mh::sim::test::run_bldg_shuttle_slot_is_free_tests();
    mh::sim::test::run_prod_bind_planet_tests();
    mh::sim::test::run_prod_planet_distance_factor_tests();
    mh::sim::test::run_unit_type_group_index_tests();
    mh::sim::test::run_prod_shuttle_fuel_apply_tests();
    mh::sim::test::run_unit_bldg_apply_scaled_damage_tests();
    mh::sim::test::run_unit_bldg_apply_lethal_damage_tests();
    mh::sim::test::run_map_fow_reveal_full_tests();
    mh::sim::test::run_bldg_load_resource_tail_noop_tests();
    mh::sim::test::run_bldg_finish_order_tests();

    // SIM1C: llm_strat_order_queue_dispatch, three TUs mirroring the three translation units.
    mh::sim::test::run_dispatch_spine_tests();
    mh::sim::test::run_dispatch_bldg_tests();
    mh::sim::test::run_dispatch_bldg2_tests();
    mh::sim::test::run_dispatch_admin_tests();

    mh::sim::test::run_unit_predicates_tests();
    mh::sim::test::run_unit_passive_engage_tests();

    mh::sim::test::run_unit_ctrl_group_tests();
    mh::sim::test::run_unit_target_tracking_tests();
    mh::sim::test::run_unit_recruit_tests();
    mh::sim::test::run_unit_update_anim_tests();

    mh::sim::test::run_unit_set_state_tests();
    mh::sim::test::run_unit_path_free_slot_tests();
    mh::sim::test::run_unit_housing_count_tests();
    mh::sim::test::run_facing24_from_points_tests();
    mh::sim::test::run_unit_predict_coords_tests();
    mh::sim::test::run_unit_ctrlgroup_member_tests();
    mh::sim::test::run_unit_purge_unregistered_tests();

    // SIM1A verify pass (2026-08-12)
    mh::sim::test::run_unit_soldier_chain_tests();
    mh::sim::test::run_unit_soldier_anim_tests();
    mh::sim::test::run_unit_status_bit_tests();
    mh::sim::test::run_unit_refund_tests();
    mh::sim::test::run_unit_creation_tests();

    // SIM1A batch-A tail (2026-08-12): the three vacuous-shadow OUT-pointer functions
    mh::sim::test::run_unit_facing24_delta_tests();
    mh::sim::test::run_unit_fine_pos_tests();
    mh::sim::test::run_unit_soldier_screen_pos_tests();

    // SIM1B (2026-08-12): the two vacuous-shadow functions from the placement/roster-queries slice.
    mh::sim::test::run_bldg_placement_corner_tests();
    mh::sim::test::run_bldg_connectivity_flood_tests();

    // SIM1B batch-B closing session (2026-08-13): the remaining vacuous/unreached building-tick
    // functions, plus the power-network flood-fill + driver (done_when clause 2).
    mh::sim::test::run_bldg_get_coords_tests();
    mh::sim::test::run_bldg_find_mothership_position_tests();
    mh::sim::test::run_bldg_footprint_set_passable_tests();
    mh::sim::test::run_bldg_energy_refill_full_tests();
    mh::sim::test::run_bldg_scrap_stored_units_tests();
    mh::sim::test::run_bldg_power_network_tests();

    // The construct/destroy transition handlers.
    mh::sim::test::run_bldg_construct_finalize_tests();
    mh::sim::test::run_bldg_unmap_footprint_tests();
    mh::sim::test::run_bldg_state_destroyed_tests();

    // SIM1-G4 verification debt (2026-08-22): eleven first-slice building-state/turret/charge
    // handlers, all DO-NOT-ARM or with a zero-call rig arm -- offline is their only evidence.
    mh::sim::test::run_bldg_state_default_reset_tests();
    mh::sim::test::run_bldg_state_idle_activate_tests();
    mh::sim::test::run_bldg_state_deploy_anim_wait_tests();
    mh::sim::test::run_bldg_state_land_activate_tests();
    mh::sim::test::run_bldg_state_deploy_start_tests();
    mh::sim::test::run_bldg_state_to_unit_tests();
    mh::sim::test::run_bldg_state_turret_scan_tests();
    mh::sim::test::run_bldg_state_turret_attack_tests();
    mh::sim::test::run_bldg_state_construction_tests();
    mh::sim::test::run_bldg_state_charge_gate_tests();
    mh::sim::test::run_bldg_state_charge_step_tests();

    // SIM1-G4 (2026-08-22): eight DO-NOT-ARM handlers -- offline is their only evidence.
    mh::sim::test::run_bldg_state_hangar_recharge_check_tests();
    mh::sim::test::run_bldg_state_hangar_recharge_units_tests();
    mh::sim::test::run_bldg_state_upgrading_tests();
    mh::sim::test::run_bldg_state_researching_tests();
    mh::sim::test::run_bldg_state_dismantling_tests();
    mh::sim::test::run_bldg_state_dismantle_finish_tests();
    mh::sim::test::run_bldg_state_prod_pick_next_tests();
    mh::sim::test::run_bldg_state_prod_working_tests();

    // SIM1-G4 / fourth-slice verification debt drain (2026-08-22): nine offline oracles
    // (power_primary_check registered late; the other eight are the's remaining unproven
    // rows). See SIM1-G4 progress.
    mh::sim::test::run_bldg_state_power_primary_check_tests();
    mh::sim::test::run_bldg_state_mine_check_deposits_tests();
    mh::sim::test::run_bldg_state_mine_extracting_tests();
    mh::sim::test::run_bldg_start_liftoff_anim_shuttle_tests();
    mh::sim::test::run_bldg_start_liftoff_anim_mother_tests();
    mh::sim::test::run_bldg_construction_complete_tests();
    mh::sim::test::run_bldg_refund_resources_scaled_by_energy_tests();
    mh::sim::test::run_hangar_any_unit_needs_energy_tests();
    mh::sim::test::run_hangar_recharge_pulse_tests();

    // SIM1-G4 (debt drain, 2026-08-22): the 7 rows the left `reviewed`
    // with no rig/oracle evidence. See SIM1-G4 progress.
    mh::sim::test::run_dmp_enqueue_scripted_order_tests();
    mh::sim::test::run_reason_to_housing_bldg_tests();
    mh::sim::test::run_bldg_register_online_tests();
    mh::sim::test::run_load_base_layout_dmp_tests();
    mh::sim::test::run_bldg_sprite_anchor_tests();

    // SIM1-G4 (2026-08-22): the 5 of 7 register_online callees a 60000-step all-AI soak
    // (game_speed_pct=1000) never constructed (airfield_h/helipad_h_or_misc got real rig coverage
    // instead -- see SIM1-G4 progress).
    mh::sim::test::run_bldg_online_secondary_tests();

    mh::sim::test::run_prod_shuttle_unload_tests();

    // SIM1D verification push (2026-08-14): production_complete/prod_deliver_arrivals/
    // prod_spawn_arrived_unit are all DO-NOT-ARM (unbounded shadow closures) -- verified offline here
    // instead, per SIM1D's own closed done_when clause 4 note that a future offline oracle could
    // close it for real.
    mh::sim::test::run_prod_completion_pipeline_tests();
    mh::sim::test::run_weapon_damage_tests();

    // SIM1D-V (2026-08-16): verification debt -- twelve batch-D functions closed `reviewed` with
    // no execution evidence (none arm-ready: unbounded/effectful closures, a SESSION_SP-only
    // reachability gate, one documented arm-crash G22, or a vacuous shadow site). Offline oracles
    // for all twelve.
    mh::sim::test::run_bldg_instant_construct_tests();
    mh::sim::test::run_bldg_queue_construction_thunk_tests();
    mh::sim::test::run_prod_shuttle_depart_tests();
    mh::sim::test::run_prod_bldg_depart_finalize_tests();
    mh::sim::test::run_prod_shuttle_load_passengers_tests();
    mh::sim::test::run_prod_unload_cargo_unit_tests();
    mh::sim::test::run_unit_load_into_shuttle_cargo_tests();
    mh::sim::test::run_unit_transport_unload_tests();
    mh::sim::test::run_unit_apply_production_completion_tests();
    mh::sim::test::run_storage_purge_dead_docked_tests();
    mh::sim::test::run_game_notify_system_available_tests();

    // ---- SIM1E verification debt (2026-08-16) --------------------------------------------------
    // Batch E's last twelve rows. Same shape as SIM1D-V above: translated + adversarially reviewed,
    // zero execution evidence, and NONE of them can ever be armed on the rig (see the forward
    // declarations near the top of this file for the per-function reason). Offline is the only
    // oracle they will get, in this migration or any later one.
    mh::sim::test::run_fog_sight_circle_tests();
    mh::sim::test::run_weapon_scatter_offset_tests();
    mh::sim::test::run_unit_fire_weapon_tests();
    printf("-- llm_strat_unit_chase_check --\n"); // RI-SIM / SIM1-G5 second slice (2026-08-22)
    mh::sim::test::run_unit_chase_check_tests();
    printf("-- llm_strat_unit_try_pay_action_cost --\n"); // RI-SIM / SIM1-G5 second slice (2026-08-22)
    mh::sim::test::run_unit_try_pay_action_cost_tests();
    mh::sim::test::run_unit_calc_range_approach_point_tests(); // RI-SIM / SIM1-G5 second slice (2026-08-22), prints its own banner
    mh::sim::test::run_projectile_tick_tests();
    mh::sim::test::run_bldg_apply_damage_tests();
    mh::sim::test::run_unit_apply_damage_tests();
    mh::sim::test::run_unit_state_die_explode_tests();
    mh::sim::test::run_unit_state_budget_noop_tests();
    mh::sim::test::run_unit_state_group_step_tests();
    mh::sim::test::run_unit_state_stop_to_default_tests();
    mh::sim::test::run_unit_state_patrol_swap_tests();
    mh::sim::test::run_unit_state_hover_engage_tests();
    mh::sim::test::run_unit_state_flight_tests();
    mh::sim::test::run_unit_state_corpse_fow_decay_tests();
    mh::sim::test::run_unit_state_move_walker_tests();
    mh::sim::test::run_unit_state_group_marshal_tests();
    mh::sim::test::run_unit_state_attack_building_tests();
    mh::sim::test::run_unit_state_attack_unit_tests();
    mh::sim::test::run_unit_state_remove_silent_tests();
    mh::sim::test::run_unit_state_idle_scatter_tests();
    mh::sim::test::run_group_move_order_commit_tests();
    mh::sim::test::run_unit_group_step_plane_tests();
    mh::sim::test::run_unit_set_state_order_of_tests();
    mh::sim::test::run_unit_teardown_mapped_tests();
    mh::sim::test::run_unit_free_slot_tests();
    mh::sim::test::run_unit_change_proto_and_energy_tests();
    mh::sim::test::run_squad_pick_free_formation_anchor_tests();
    mh::sim::test::run_unit_group_step_ground_tests();
    mh::sim::test::run_group_scratch_helpers_tests();
    mh::sim::test::run_combat_conquest_credit_tests();
    mh::sim::test::run_unit_estimate_weapon_damage_tests();
    mh::sim::test::run_population_add_tests();
    mh::sim::test::run_landing_queries_tests();
    mh::sim::test::run_revoke_invention_tests();
    mh::sim::test::run_insert_item_in_player_array_tests();
    mh::sim::test::run_game_set_event_tests();
    // RI-SIM / sim_resid batches A+B, first translation slice (2026-08-31).
    mh::sim::test::run_clock_resync_tests();
    mh::sim::test::run_session_clear_presence_flag_tests();
    mh::sim::test::run_advisor_tick_tests();
    mh::sim::test::run_planet_transition_finalize_tests();
    mh::sim::test::run_planet_map_session_init_tests();
    mh::sim::test::run_new_game_init_tests();
    mh::sim::test::run_land_players_on_planet_tests();
    mh::sim::test::run_planet_session_begin_tests();
    mh::sim::test::run_session_begin_multi_tests();
    mh::sim::test::run_session_state_reset_tests();
    mh::sim::test::run_player_presence_lost_tests();
    mh::sim::test::run_spawn_invasion_force_tests();
    mh::sim::test::run_tile_midpoint_wrapped_tests();
    mh::sim::test::run_tile_delta_wrapped_tests();
    mh::sim::test::run_pixel_delta_wrapped_tests();
    mh::sim::test::run_map_wrapped_delta_tests();
    mh::sim::test::run_dir_sector_to_tests();
    mh::sim::test::run_tile_neighbor_reverse_dir_tests();
    mh::sim::test::run_planet_distance_tests();
    mh::sim::test::run_locate_active_port_tests();
    mh::sim::test::run_region_add_adjacency_edge_tests();
    mh::sim::test::run_region_recompute_adjacency_tests();
    mh::sim::test::run_merge_small_regions_tests();
    mh::sim::test::run_region_split_tests();
    mh::sim::test::run_add_to_available_buildings_tests();
    mh::sim::test::run_add_project_to_available_tests();
    mh::sim::test::run_add_project_to_available_with_check_tests();
    mh::sim::test::run_try_start_project_tests();
    mh::sim::test::run_apply_project_resources_tests();
    mh::sim::test::run_update_progress_tests();
    mh::sim::test::run_invasion_chance_roll_tests();
    mh::sim::test::run_handle_invasion_tests();
    mh::sim::test::run_invasion_alert_arm_tests();
    mh::sim::test::run_spawn_enemy_landing_tests();
    mh::sim::test::run_sp_outcome_announce_tests();
    mh::sim::test::run_speed_increase_tests();
    mh::sim::test::run_speed_decrease_tests();
    mh::sim::test::run_player_set_ai_tests();
    mh::sim::test::run_apply_area_to_map_tests();
    mh::sim::test::run_region_apply_area_tests();
    mh::sim::test::run_map_create_building_tests();
    mh::sim::test::run_sim_step_tests();
    mh::sim::test::run_group_plan_formation_positions_tests();
    mh::sim::test::run_pathfind_grid_geometry_tests();
    mh::sim::test::run_unit_path_step_blocked_tests();
    mh::sim::test::run_pathfind_geom_misc_tests();
    mh::sim::test::run_map_region_route_marking_tests();
    mh::sim::test::run_map_wrap_delta_xy_tests();
    mh::sim::test::run_pathfind_dir_code_and_adjacency_tests();
    mh::sim::test::run_path_step_check_and_request_detour_tests();
    mh::sim::test::run_path_solver_tests();
    mh::sim::test::run_path_slot_dist_tests();
    mh::sim::test::run_map_region_tile_find_tests();
    mh::sim::test::run_unit_path_queue_count_tests();
    mh::sim::test::run_unit_state_plot_turn_path_tests();
    mh::sim::test::run_unit_state_move_path_tests();

    // SIM1-G3 (2026-08-21): the storage enter/exit/dock/taxi/landing lifecycle.
    mh::sim::test::run_unit_state_enter_arrival_check_tests();
    mh::sim::test::run_unit_state_exit_cancel_tests();

    // SIM1-G3 (2026-08-21): storage_can_land, NOT SHADOWABLE.
    mh::sim::test::run_storage_can_land_tests();

    // SIM1-G3 (2026-08-21): storage_exit_tile_is_clear, NOT SHADOWABLE.
    mh::sim::test::run_storage_exit_tile_is_clear_tests();

    // SIM1-G3 (2026-08-21): storage_scrap_home_docked_units, VACUOUS under shadow.
    mh::sim::test::run_storage_scrap_home_docked_units_tests();
    mh::sim::test::run_storage_accept_landing_tests();
    mh::sim::test::run_storage_scrap_docked_units_of_type_tests();
    mh::sim::test::run_storage_can_enter_tests();
    mh::sim::test::run_unit_state_enter_wait_tests();

    // SIM1-G3 (2026-08-21): unit_state_enter_walk_in, ARMABLE but NOT COVERED.
    mh::sim::test::run_unit_state_enter_walk_in_tests();

    // SIM1-G3 (2026-08-21): unit_state_enter_storage_begin, arm_ready:false.
    mh::sim::test::run_unit_state_enter_storage_begin_tests();

    // SIM1-G3 (2026-08-21): unit_takeoff_finalize, arm_ready:false.
    mh::sim::test::run_unit_takeoff_finalize_tests();

    // SIM1-G3 (2026-08-21): unit_state_landing_request, arm_ready:false.
    mh::sim::test::run_unit_state_landing_request_tests();

    // SIM1-G3 (2026-08-21): unit_state_production_ready, arm_ready:false.
    mh::sim::test::run_unit_state_production_ready_tests();

    // SIM1-G3 (2026-08-21): unit_state_deploy_approach, arm_ready:false.
    mh::sim::test::run_unit_state_deploy_approach_tests();

    mh::sim::test::run_h_switch_to_planet_tests();

    // SIM1-H (2026-09-10): the two ctrl-group order issuers -- driven by player input, which no gate fixture produces.
    mh::sim::test::run_h_group_issue_orders_tests();

    // SIM1-H (2026-09-10): the shuttle-arrival dialog handler (UI callback) and diplomacy_restore_relations (planet transition) -- neither reachable from a gate fixture.
    mh::sim::test::run_h_shuttle_and_diplomacy_tests();

    mh::sim::test::run_unit_state_takeoff_taxi_tests();

    // SIM1-G3 (2026-08-21): unit_state_dock_taxi_in, arm_ready:false.
    mh::sim::test::run_unit_state_dock_taxi_in_tests();

    // SIM1-G3 (2026-08-21): the batch's last 6 rows, all arm_ready:false.
    mh::sim::test::run_unit_state_exit_storage_begin_tests();
    mh::sim::test::run_storage_board_unit_tests();
    mh::sim::test::run_unit_state_deploy_to_building_tests();
    mh::sim::test::run_unit_state_squad_merge_tests();
    mh::sim::test::run_unit_state_takeoff_landing_tests();
    mh::sim::test::run_storage_place_exit_ground_tests();

    // SIM1-G4 (2026-08-22): debt-drain offline oracles for the 6 DO-NOT-ARM/no-shadow-
    // site rows the left as reviewed-not-verified.
    mh::sim::test::run_bldg_placement_corner_by_type_tests();
    mh::sim::test::run_turret_acquire_target_tests();
    mh::sim::test::run_turret_fire_tests();
    mh::sim::test::run_prod_unload_cargo_manifest_tests();
    mh::sim::test::run_prod_try_start_unit_tests();
    mh::sim::test::run_bldg_completion_dispatch_tests();
    mh::sim::test::run_bldg_done_handlers_tests();
    mh::sim::test::run_bldg_anim_tick_tests();
    mh::sim::test::run_bldg_anim_state_helipad_tests();
    mh::sim::test::run_bldg_anim_state_helipad_a_tests();
    mh::sim::test::run_bldg_anim_state_online_a_tests();
    mh::sim::test::run_pathtrace_greedy_tests();
    mh::sim::test::run_bldg_start_special_anim_tests();
    mh::sim::test::run_bldg_anim_state_port_tests();

    // SIM1-DISPATCH (2026-08-22): the (state -> handler) pairing both tick sites dispatch through.
    // A table check, because the golden A/B cannot name a swapped pair -- see the oracle's header.
    mh::sim::test::run_register_state_handlers_tests();

    // SIM1-BLDGCB (2026-08-23): the (building type -> callback) pairing, plus the cfg SCAN that
    // turns it into a per-building-id table. Offline for the same reason as its sibling above, and
    // for one more: the scan's input is cfg data a golden run cannot vary.
    mh::sim::test::run_register_bldg_type_callbacks_tests();

    // SIM-RESID-C/-D: the residual sim writers -- the path store and the player-table initialisers.
    mh::sim::test::run_player_init_tests();
    mh::sim::test::run_table_resets_tests();
    mh::sim::test::run_unit_path_release_tests();
    mh::sim::test::run_unit_path_store_tests();

    // SIM-RESID-C and the tail of SIM-RESID-E.
    mh::sim::test::run_tech_tables_reset_tests();
    mh::sim::test::run_init_human_player_data_tests();
    mh::sim::test::run_pathfinder_init_tests();
    mh::sim::test::run_map_fill_defaults_tests();
    mh::sim::test::run_scenario_planet_clone_tests();
    mh::sim::test::run_invasion_alert_reset_tests();
    mh::sim::test::run_bldg_try_begin_placement_tests();
    mh::sim::test::run_landing_spots_reroll_tests();
    // sim_resid batch E/F.
    mh::sim::test::run_rng_seed_channel_tests();
    mh::sim::test::run_time_resync_tests();
    mh::sim::test::run_invasion_due_check_tests();
    mh::sim::test::run_tutorial_step_driver_tests();
    mh::sim::test::run_start_tutorial_tests();
    mh::sim::test::run_try_enter_tactical_mission_tests();
    // sim_resid batch E verification debt, drained 2026-09-01.
    mh::sim::test::run_prod_set_transfer_destination_tests();
    mh::sim::test::run_bldg_gather_nearby_squad_status_tests();
    mh::sim::test::run_bldg_is_network_critical_tests();
    mh::sim::test::run_sort_sites_by_dist_tests();
    mh::sim::test::run_spawn_ai_base_tests();
    // X-TL-DRAIN step 4 (2026-09-12).
    mh::sim::test::run_bldg_init_all_tests();
    mh::sim::test::run_bldg_instant_construct_find_slot_tests();
    mh::sim::test::run_unit_order_disembark_soldiers_tests();

    printf("%d checks, %d failures\n", mh::sim::test::g_checks, mh::sim::test::g_fails);
    return mh::sim::test::g_fails ? 1 : 0;
}
