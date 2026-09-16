//
// sim_write_negative.cpp -- THE NEGATIVE COMPILE TEST for the sim state interface (RI-SIM / SIM0).
//
// SIM0's acceptance asks that "the chosen compile-time property is stated in the module header AND
// proven by a checked-in NEGATIVE test that is demonstrated to fail with the expected diagnostic --
// not merely asserted -- on the AI0 pattern (each case compiled with its own macro, plus a POSITIVE
// arm required to compile, because a driver misconfiguration makes every case 'fail' and a naive
// did-it-fail check reads that as a pass)". This is that test; `python tools/check_const_view.py` is
// what demonstrates it.
//
// THE PROPERTY IS NOT THE AI'S. ai_const_negative.cpp proves "the AI cannot write sim state", which
// works because the AI writes almost nothing. The sim writes the rosters -- that is its job -- so
// the same test here would be a lie. sim/sim_state.h states three rules instead, and the cases below
// are one per rule plus the two places each rule is most likely to leak:
//
//   W1  THE READ VIEW IS CONST, AND ITS MEMBERSHIP IS THE CLAIM.
//       case 1 -- a CFG table through the view. Building[]/Unit[]/Weapon[] are loaded at boot and
//                 read-only after; a sim function writing one is always a bug, and this is the case
//                 that carries W1's actual content.
//       case 2 -- a ROSTER through the view. The sim may write `units` -- but only through the
//                 store, so a write must name `own` and is therefore greppable and countable. This
//                 is the AI0 case restated for a subsystem that legitimately writes.
//       case 5 -- const has to survive a NESTED record array (`players[p].ai_target_list[i].f`),
//                 not just a scalar field. A scalar field holding does not prove an array of
//                 structs does.
//
//   W2  THE WRITE STORE HAS NO REACHABLE ADDRESS.
//       case 3 -- reaching for the store's roster base. If this compiled, a translation could cache
//                 a `unit *` and keep it across a rebase, which is the exact silent staleness the
//                 whole re-resolve-per-call arrangement exists to remove.
//
//   W3  THE STORE CANNOT BE CONSTRUCTED OUTSIDE THE MODULE.
//       case 4 -- a non-sim TU building its own store over addresses of its choosing. The friend
//                 list (state(), sim_fixture) is the sanctioned set; anything else has to edit it.
//
// Cases are mutually exclusive on purpose (one macro per compile) so a failure can only be
// attributed to the construct it names. Note the driver checks a DIFFERENT diagnostic per case:
// 1/2/5 must fail with the assign-through-const family, 3/4 with C2248 (inaccessible member). A
// case that fails with the other rule's diagnostic is a broken test, not a passing one.
//
#include <cstdint>

#include "sim/sim_state.h"

namespace {

// A view bound to nothing: this file is never linked or run, only compiled. A store cannot be
// declared at namespace scope at all (that is case 4), so the cases that need one take it from
// state() -- which is legal to CALL from anywhere; it is only CONSTRUCTING one that is not.
mh::sim::sim_view g_view{};

void body() {
#if defined(MH_SIM_NEGATIVE_CASE_1) // EXPECT const
    // W1: the cfg tables are read-only after boot. This is the write the sim must never make.
    g_view.cfg_buildings[0].weapon_id = 0;
#elif defined(MH_SIM_NEGATIVE_CASE_2) // EXPECT const
    // W1: the sim writes the rosters -- through `own`, never through the read view.
    mh::sim::unit_of(g_view, 0, 0).ai_group_index = 0;
#elif defined(MH_SIM_NEGATIVE_CASE_3) // EXPECT access
    // W2: no roster base escapes the store, so there is nothing to cache across a rebase.
    mh::sim::sim_state st = mh::sim::state();
    mh::sim::unit     *p  = st.own.units_;
    (void)p;
#elif defined(MH_SIM_NEGATIVE_CASE_4) // EXPECT access
    // W3: a non-sim TU cannot mint a mutable handle over addresses of its own choosing.
    //
    // THE ARGUMENT LIST HAS TO TRACK sim_store's CONSTRUCTOR. It is private, so a caller cannot see
    // its signature -- which means a stale list fails with "no overloaded function takes N
    // arguments" (C2661) instead of "cannot access private member" (C2248), and the case would then
    // be passing for a reason that has nothing to do with W3. check_const_view.py asserts the
    // diagnostic KIND for exactly this reason and caught it when SIM1C widened the constructor from
    // 3 parameters to 12 (2026-08-08), when SIM1A widened it from 12 to 13
    // (2026-08-10, engage_candidate_scratch_count_), and again when SIM1A widened it
    // from 13 to 21 (2026-08-10, cur_unit_/tick_budget_/state_loop_guard_/ctrl_groups_/
    // tile_objects_/passable_/population_/soldiers_), ONCE MORE from 21 to 22 (2026-08-10,
    // SIM1A adding unit_housing_ for llm_strat_unit_housing_count_add and
    // llm_strat_unit_spawn_on_tile), and again from 22 to 26 (2026-08-11, SIM1A adding
    // path_slot_flags_/path_free_slot_count_ for llm_strat_path_free_slot,
    // click_select_target_id_ for llm_strat_unit_remove_from_map/_on_destroyed, and profiles_ for
    // llm_strat_unit_on_destroyed), again from 26 to 27 (2026-08-12, SIM1A
    // adding geom_mut_ for llm_strat_unit_notify_ui's general.change_flag write), and again from 27
    // to 28 (2026-08-12, SIM1B adding bldg_enclosure_scratch_ for llm_strat_bldg_check_placement_
    // encloses_neighbors's private scratch grid), and again from 28 to 29 (2026-08-12, SIM1B's
    // building_tick machinery slice adding power_stats_ for llm_strat_power_recompute), and again
    // from 29 to 36 (2026-08-13, SIM1B's building_tick machinery adding cur_building_ for
    // llm_strat_bldg_state_destroyed, cam_pan_target_col_/cam_pan_target_row_/
    // planet_mother_lost_time_ for the same function's tail, and mines_/storage_/
    // ui_selected_bldg_index_ for llm_strat_bldg_unmap_footprint's first-writer bindings), and again
    // from 36 to 37 (2026-08-13, SIM1D adding prod_shuttle_slots_ for
    // llm_strat_production_complete), and again from 37 to 38 (2026-08-13, same slice, adding
    // prod_complete_throttle_ for the same function), and again from 38 to 40 (2026-08-14, SIM1D
    // adding cur_projectile_/projectile_pool_ for llm_strat_projectile_tick, opening
    // SIM1E), and again from 40 to 42 (2026-08-14, SIM1E third batch adding cur_fx_anim_/
    // fx_anim_pool_ for llm_fx_anim_seq_cancel/llm_strat_fx_anim_tick), and again from 42 to 45
    // (2026-08-15, SIM1E adding cam_jump_offsets_/cam_jump_scales_/
    // cam_jump_queue_count_ for llm_strat_spawn_debris_burst), and again from 45 to 59 (2026-08-16,
    // SIM1E fog/sight family adding fog_visible_by_count_/fog_discovered_/g_tmp_player_/g_tmp_x_/
    // g_tmp_y_/g_tmp_sight_/other_players_mask_, plus the domain-root sim_step slice adding
    // cur_player_mut_/cur_index_mut_/cur_unit_slot_/cur_building_slot_/cur_projectile_slot_/
    // cur_fx_anim_slot_/storage_stats_), and again from 59 to 64 (2026-08-16, SIM1F
    // adding game_speed_player_factor_/rng_state_/is_human_mut_/game_human_player_mask_/
    // players_raw_ for llm_game_speed_increase/_decrease, llm_strat_rng_next,
    // llm_game_player_set_human/_set_ai, and llm_diplomacy_set_relation), and again from 64 to 69
    // (2026-08-16, SIM1F adding progress_ for game_HandleProgress, game_speed_/
    // planet_time_ for llm_game_speed_recompute/game_AddPlanetToAvailable, and
    // available_buildings_/available_projects_ for game_AddToAvailableBuildings/
    // game_AddProjectToAvailable's address-escape accessors), and again from 69 to 73 (2026-08-16,
    // SIM1F adding cfg_units_mut_/cfg_weapons_mut_ for game_HandleUpgrade -- the FIRST
    // writer to either cfg table in the closure -- and map_width_mut_/map_height_mut_ for
    // llm_strat_planet_distance's temporary-override pattern), and again from 73 to 74 (2026-08-16,
    // same slice, adding planet_invasion_time_ for game_HandleInvasion), and again from 74 to 75
    // (2026-08-16, same slice, adding player_resources_mut_ for llm_resource_add/game_SpendResource
    // -- the first DIRECT writer of player_resources; every earlier caller reached it by calling one
    // of these two), and again from 75 to 76 (2026-08-16, same slice, adding planet_status_mut_ for
    // game_HandleInvasion -- the first writer of G_PLANET_STATUS), and again from 76 to 78
    // (2026-08-17, SIM1F, adding region_grid_ for map_ApplyAreaToMap/
    // llm_map_region_apply_area/map_CreateBuilding/llm_map_merge_small_regions/
    // llm_map_region_split and region_bfs_queue_ for llm_map_region_split's own flood-fill scratch),
    // and again from 78 to 79 (2026-08-17, same slice, adding foreign_bldg_event_pending_ for
    // map_CreateBuilding), and again from 79 to 81 (2026-08-17, SIM1F, adding
    // invasion_alert_time_ for llm_strat_invasion_alert_arm and landing_spots_ for
    // llm_strat_claim_landing_spot), and again from 81 to 96 (2026-08-18, SIM1F, adding
    // game_SetEvent's 15 UI-panel/event-queue/chat-input mutable accessors), and again from 96 to 98
    // (2026-08-18, SIM1F, adding debug_resource_yield_cut_ and session_mode_w_ for
    // llm_strat_player_presence_lost), and again from 98 to 99 (2026-08-18, SIM1F, adding
    // ai_active_player_count_w_ for llm_strat_spawn_invasion_force -- the LAST sim function), and
    // again from 154 to 155 (2026-08-21, SIM1-G3, adding squad_anchor_scratch_mut_ for
    // llm_strat_unit_state_squad_merge), and again from 155 to 159 (2026-08-22, SIM1-G4,
    // adding resources_mut_/foreign_bldg_change_flag_mut_/ui_base_marker_coords_mut_/dmp_path_
    // scratch_ for llm_strat_bldg_completion_dispatch), and again from 159 to 227 (2026-08-31,
    // SIM-RESID-IF bind half, adding the 68 residual-writer bindings in one go -- the largest
    // single jump this line has taken, because that item bound a whole domain's write set at once
    // rather than a slice's), and again from 227 to 253 (2026-08-31, SIM-RESID-IF's RE-CLOSE, adding
    // the 24 WRITE halves that first close missed plus the two address escapes -- the first close
    // bound regions without asking which HALF, so 24 of them were read-only and eight finished
    // translations could not be built; see sim_state.h's write-halves block), and again from 253 to 257
    // (2026-08-31, SIM-RESID-IF's THIRD close, adding map_FillDefaults' three whole-region fill
    // bases plus the mutable death_anim_table its PRESERVE-BUG store lands in -- the four the
    // write columns could not report because the fill is performed by utils_fill_data), and again
    // from 257 to 258 (2026-08-31, the write half of G_TEXT_PTRS for
    // llm_strat_scenario_planet_clone, the domain's only writer of the string table), and again from
    // 258 to 259 (2026-08-31, SIM-RESID-C: scenario_planet_name_w, the wchar_t[16] at
    // 0x00e589c0 that the SAME function hands to llm_str_ansi_to_wide as its widening destination --
    // an address escape, so it is a store binding even though we never write a byte of it), and
    // again from 259 to 260 (2026-09-01, SIM-RESID-F: player_desc_slots, the mutable half of
    // `Players` that llm_game_start_tutorial writes the fixed 1v1 slot descriptors through), and
    // again from 260 to 269 (2026-09-02, LT1 lib_trans: the five map-region pool slots for the
    // LT1C allocator, the three menu-teardown writes and the ambient table escape for LT1D), and
    // again from 269 to 271 (2026-09-02, LT1C c4: bldg_cell_grid_ + bldg_cell_grid_row_shift_ for
    // llm_strat_bldg_recompute_cell_grid's two island outputs), and again from 271 to 273
    // (2026-09-10, SIM1-H: cfg_buildings_mut_ + region_coord_wrap_mask_mut_ -- the write halves
    // llm_strat_bldg_init_defaults and llm_map_build_regions need), and again from 273 to 277
    // (2026-09-10, SIM1-H wave 2: the nav-region pipeline's merge threshold, route step
    // deltas, the delta wrap sentinel and the flood fill's own BFS queue). If
    // this line stops compiling, extend it -- do not relax the expected diagnostic.
    //
    // AND NOTE WHAT "STOPS COMPILING" LOOKS LIKE, because it already fooled a session: when the arity
    // drifts, this case still FAILS to compile -- with C2661 "no overloaded function takes N
    // arguments" instead of C2248 "cannot access private member". That is a BROKEN test, not a
    // passing one; check_const_view.py checks the diagnostic KIND for exactly this reason and says
    // so. A checker that only asked "did it fail?" would have reported W3 green while proving
    // nothing.
    mh::sim::sim_store forged(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                              nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                              nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                              nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                              nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                              nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                              nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                              nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                              nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                              nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                              nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                              nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                              nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                              nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                              nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                              nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                              nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                              nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                              nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                              nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                              nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                              nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                              nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                              nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                              nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                              nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                              nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                              nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                              nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                              nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                              nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                              nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                              nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                              nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                              nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                              nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                              nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                              nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                              nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    (void)forged;
#elif defined(MH_SIM_NEGATIVE_CASE_5) // EXPECT const
    // W1: const survives a nested record array, not only a scalar field.
    g_view.players[0].ai_target_list[0].victim_index = 0;
#elif defined(MH_SIM_NEGATIVE_CASE_7) // EXPECT const
    // THE WRITE-HALF ARM (2026-08-31), and it exists because SIM-RESID-IF closed once WITHOUT it.
    //
    // That item's acceptance test folded the domain's write set against the RID set MENTIONED in
    // sim_state.cpp and returned zero unbound -- a fold that cannot tell `v.X = ptr<const T>(RID)`
    // from `ptr<T>(RID)` in the store constructor. 24 regions were bound READ-ONLY and counted as
    // bound; eight complete, linted translations were then unbuildable because each named a
    // sim_store accessor that does not exist. Nothing in this file could see that: every case here
    // asked whether an ILLEGAL write is REJECTED, and none asked whether a LEGAL one is POSSIBLE.
    //
    // So the pair is now explicit, over one of the 24. HERE: writing _G_LLM_STRAT_SIM_ACTIVE
    // through the const view must not compile. IN THE POSITIVE ARM below: writing the SAME region
    // through `own.sim_active_mut()` must compile. Break either half and exactly one of the two
    // goes red -- which is the discrimination the single fold could not make.
    *g_view.sim_active = 1;
#elif defined(MH_SIM_NEGATIVE_CASE_6) // EXPECT access
    // W2 OVER A REGION BOUND BY SIM-RESID-IF (2026-08-31), not by an earlier slice. The bind half of
    // that item added 67 pointers to sim_store in ONE change -- the largest single jump this class
    // has taken -- and a batch that size is exactly where a member could be added public by accident
    // and nothing would notice: case 3 pins `units_`, which was private long before any of this.
    //
    // squad_status_ is the sharpest of the 67 to pin, because it is a SECOND registry binding of a
    // region libmh/tact/tact_state.cpp already binds mutably. Two modules holding the same region is
    // the shape sim_state.h blesses ONLY while neither hands out an address; the moment one does,
    // "same region, two legitimate writers" becomes two aliases nobody can reason about.
    mh::sim::sim_state st = mh::sim::state();
    (void)st.own.squad_status_;
#else
    // THE POSITIVE ARM. Every read below is legal and every write goes through the store's typed
    // accessors; if this does not compile, the negative cases above prove nothing.
    mh::sim::sim_state st = mh::sim::state();

    const mh::sim::unit     &u = mh::sim::unit_of(g_view, 0, 0);
    const mh::sim::building &b = mh::sim::building_of(g_view, 0, 0);
    const uint8_t            t = g_view.cfg_weapons[u.weapons[0].weapon_id].target;

    st.own.unit_at(0, 0).ai_group_index = (uint16_t)(u.ai_group_index | t);
    st.own.player_at(0).resource_spend_total[1] += (int32_t)b.state;

    // SIM-RESID-IF (2026-08-31). One of each KIND of the 67 residual-writer bindings -- scalar,
    // array, record, pointer slot -- so the positive arm proves they are reachable and correctly
    // typed rather than merely declared. A binding that compiles in the header but not at a call
    // site is a binding this file would otherwise pass over in silence.
    st.own.current_game_time()              = st.own.last_game_time() + st.own.sim_step_interval();
    st.own.game_mode()                      = (uint8_t)0;
    st.own.net_peer_horizon_at(0)           = st.own.net_peer_horizon_pending_at(0);
    st.own.squad_status_at(0).unit_proto_id = 0;
    st.own.ui_fade_transition().state       = 0;
    st.own.current_map_data().seed          = (uint8_t)0;
    st.own.ui_menu_widget_list()            = nullptr;
    st.own.ui_menu_async_callback_a()       = nullptr;
    st.own.tutorial_reset_slot_0050a678()   = 0;

    // SIM-RESID-IF RE-CLOSE (2026-08-31): the LEGAL half of case 7, and one of each kind among the
    // 24 write halves that close added -- scalar, record-by-reference, raw-table cell, and an
    // address escape. Every one of these regions was bound READ-ONLY before, so this block is the
    // arm that would have failed to compile on the day SIM-RESID-IF reported itself closed.
    st.own.sim_active_mut()                 = 1;
    st.own.game_clock_mut()                 = st.own.game_time_delta_mut();
    st.own.player_side_mut()                = (int16_t)st.own.local_player_slot_mut();
    st.own.width_m_mut()                    = st.own.height_m_mut();
    st.own.cfg_planet_at(0).system_index    = st.own.current_system_mut();
    st.own.system_define_index_at(0)        = st.own.planet_index_mut();
    st.own.advisor_next_time()              = 0.0;
    st.own.mouse_buttons_prev()             = (uint8_t)0;
    st.own.ui_panel_fallback_table_at(0)    = 0;
    st.own.bldg_completion_slot_count_mut() = (uint8_t)4;
    st.own.ui_bldg_tab_select_blocked_mut() = (uint8_t)0;
    st.own.system_lost_msg_shown_flag_mut() = 0;
    st.own.mp_ally_victory_rule_flag_mut()  = 0;
    st.own.player_race_mut()                = 0;
    st.own.tutorial_step_mut()              = 0;
    st.own.ai_enabled_mut()                 = 0;
    st.own.cfg_invention_at(0).f2           = 0;
    st.own.land_dmp_scratch()[0]            = '\0';
    st.own.fog_of_war_base()[0]             = (uint8_t)0;
#endif
}

} // namespace

// Referenced so the TU is not empty under /W4; never called.
void mh_sim_write_negative_anchor() { body(); }
