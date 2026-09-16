//
// ai/ai_promote.cpp -- AI1-P promotion glue: 166 of the 167 verified AI originals wired live, one
// MH_EXPORT_REPLACE seam apiece (see ai_promote.h's banner for the two that are not, and why).
//
// SHAPE, mirroring libmh/orders/order_queue.cpp's "---- PROMOTION" block (the exemplar this file
// scales up, 9 seams there vs 166 here): a named adapter per row in namespace
// mh::ai::promoted_arm, each matching its sig_<orig> exactly (addr/mh_export.gen.h), each logging a
// FIRST-CALL liveness line through mh::ai::ai_say (the AI module's own logger, ai_state.h/.cpp --
// NOT mh::orders::say, a different module's private logger), then calling the corresponding public
// mh::ai:: live wrapper. Where sig_<orig>'s parameter/return type differs from the wrapper's --
// void* vs a typed pointer, uint32_t vs int32_t, int32_t vs bool -- the adapter casts explicitly at
// the call site rather than relying on an implicit conversion, so the width/signedness change stays
// visible to a reader instead of hiding in an argument list. All these casts are the ORIGINAL's own
// committed prototype meeting a wrapper written a batch earlier with a materially identical but
// differently-spelled type (void* vs uint8_t*, a typedef alias vs its fully-qualified name, a
// narrower-vs-wider registered width) -- none of them changes what value crosses the seam.
//
// UNBINDABLE (see ai_promote.h): ONE row, llm_strat_ai_unit_squad_firepower_value, which has no
// public wrapper at all -- only a private detail:: helper of army_milestone_advance_or_attack. It is
// simply absent from the tables below (no seam, no adapter, no MH_EXPORT_REPLACE line) and its
// pre-patch bytes stay live until a future session resolves the gap.
// llm_strat_ai_engage_sort_candidates_by_dist WAS the second such row and is now wired: REBIND-AI-ESI
// (2026-09-10) declared its ambient ESI as a real third parameter, which made the seam's arity match
// the wrapper's. Its adapter is below, with the reasoning.
//
#include "ai/ai_promote.h"

#include "addr/mh_export.gen.h"

#include "ai/ai_abandon_target.h"
#include "ai/ai_active_unit_tick.h"
#include "ai/ai_army_milestone.h"
#include "ai/ai_attack_candidates.h"
#include "ai/ai_attack_commit.h"
#include "ai/ai_attacker_intel.h"
#include "ai/ai_bldg_queue.h"
#include "ai/ai_bldg_queue_dispatch.h"
#include "ai/ai_bldg_repair.h"
#include "ai/ai_bldg_weapon_range.h"
#include "ai/ai_build.h"
#include "ai/ai_build_plan_push.h"
#include "ai/ai_build_score.h"
#include "ai/ai_build_sources.h"
#include "ai/ai_construction_plan.h"
#include "ai/ai_construction_sites.h"
#include "ai/ai_engage.h"
#include "ai/ai_engage_candidates.h"
#include "ai/ai_engage_scan.h"
#include "ai/ai_grid.h"
#include "ai/ai_grid_stencil.h"
#include "ai/ai_group_building_scan.h"
#include "ai/ai_group_centroid.h"
#include "ai/ai_group_classify_target.h"
#include "ai/ai_group_expansion.h"
#include "ai/ai_group_form.h"
#include "ai/ai_group_hold.h"
#include "ai/ai_group_home_guard.h"
#include "ai/ai_group_member_count.h"
#include "ai/ai_group_member_list.h"
#include "ai/ai_group_membership.h"
#include "ai/ai_group_move_helpers.h"
#include "ai/ai_group_muster_pick.h"
#include "ai/ai_group_redistribute.h"
#include "ai/ai_group_relocation.h"
#include "ai/ai_group_remove.h"
#include "ai/ai_group_split_off.h"
#include "ai/ai_group_task_attack.h"
#include "ai/ai_group_task_formation.h"
#include "ai/ai_group_task_lifecycle.h"
#include "ai/ai_group_task_machine.h"
#include "ai/ai_group_task_movement.h"
#include "ai/ai_group_task_predicates.h"
#include "ai/ai_group_task_queue_ops.h"
#include "ai/ai_group_task_recruit.h"
#include "ai/ai_group_task_workers.h"
#include "ai/ai_group_tick.h"
#include "ai/ai_holding_pen.h"
#include "ai/ai_hq_attack_commit.h"
#include "ai/ai_hq_attack_scenario.h"
#include "ai/ai_invasion.h"
#include "ai/ai_launch_storage.h"
#include "ai/ai_map_influence.h"
#include "ai/ai_mine_plan.h"
#include "ai/ai_mine_rebalance.h"
#include "ai/ai_mine_yield.h"
#include "ai/ai_nearest_flagged.h"
#include "ai/ai_notify_bldg_constructed.h"
#include "ai/ai_notify_map_changed.h"
#include "ai/ai_notify_removed.h"
#include "ai/ai_notify_unit_lifecycle.h"
#include "ai/ai_opponent_relations.h"
#include "ai/ai_player_tick.h"
#include "ai/ai_players_tick.h"
#include "ai/ai_queue_enqueue.h"
#include "ai/ai_queue_reconcile.h"
#include "ai/ai_queue_release.h"
#include "ai/ai_queue_remove.h"
#include "ai/ai_queue_rotate.h"
#include "ai/ai_queue_type_query.h"
#include "ai/ai_reinforcements.h"
#include "ai/ai_resource_sites.h"
#include "ai/ai_scan_target_add.h"
#include "ai/ai_scan_target_sort_cmp.h"
#include "ai/ai_scan_targets.h"
#include "ai/ai_scan_visible.h"
#include "ai/ai_scr_parse.h"
#include "ai/ai_shortage_gate.h"
#include "ai/ai_shortage_react.h"
#include "ai/ai_shortage_state.h"
#include "ai/ai_site_dispatch.h"
#include "ai/ai_site_scan.h"
#include "ai/ai_site_sort.h"
#include "ai/ai_site_worth.h"
#include "ai/ai_spend_rate.h"
#include "ai/ai_spiral_scan.h"
#include "ai/ai_spiral_table_init.h"
#include "ai/ai_target.h"
#include "ai/ai_target_query.h"
#include "ai/ai_target_ref_predicates.h"
#include "ai/ai_train_flush.h"
#include "ai/ai_train_plan.h"
#include "ai/ai_turret_plan.h"
#include "ai/ai_turret_threat.h"
#include "ai/ai_unit_housing.h"
#include "ai/ai_unit_move_bump.h"
#include "ai/ai_worker_priority.h"
#include "ai/ai_worker_rebalance.h"

#include <windows.h>

// The FIRST-CALL proof (ai_promote.h banner, and the P0-EXPORT lesson order_queue.cpp's own
// promotion block records): a milestone counter can go silent for an entire run if the real call
// volume undercuts whatever thresholds it was seeded with. One line, once, per row, is what a
// ">=15000-step asymmetric run" can grep for and trust. A plain function-local static bool -- not a
// counter -- is enough: this is "was this adapter ever entered", not "how often".
namespace mh::ai::promoted_arm {
namespace {
inline void first_call(bool &seen, const char *orig_name) {
    if (seen) return;
    seen = true;
    ::mh::ai::ai_say("; [promote] ai: %s call #1 (OURS is live)\n", orig_name);
}
} // namespace
} // namespace mh::ai::promoted_arm

#define MH_AI_FIRST_CALL(ORIG_NAME_STR)                            \
    do {                                                           \
        static bool s_seen = false;                                \
        ::mh::ai::promoted_arm::first_call(s_seen, ORIG_NAME_STR); \
    } while (0)

namespace mh::ai::promoted_arm {

void unit_order_move_with_bump(uint32_t player, int32_t unit_idx, uint32_t order_arg0, uint32_t order_arg1) {
    MH_AI_FIRST_CALL("llm_strat_ai_unit_order_move_with_bump");
    mh::ai::unit_order_move_with_bump(player, unit_idx, order_arg0, order_arg1);
}

void create_reinforcement_unit(uint32_t x, uint32_t y, uint32_t unit_proto_id, uint16_t param_4) {
    MH_AI_FIRST_CALL("llm_strat_ai_create_reinforcement_unit");
    mh::ai::create_reinforcement_unit(x, y, unit_proto_id, param_4);
}

void grid_fill_below_threshold(uint8_t *grid_base, int32_t width, int32_t height, int32_t threshold, int32_t fill_value) {
    MH_AI_FIRST_CALL("llm_strat_ai_grid_fill_below_threshold");
    mh::ai::grid_fill_below_threshold(grid_base, width, height, threshold, fill_value);
}

void grid_flood_step(uint8_t *grid_base, int32_t width, int32_t height, int32_t source_level, int32_t fill_value) {
    MH_AI_FIRST_CALL("llm_strat_ai_grid_flood_step");
    mh::ai::grid_flood_step(grid_base, width, height, source_level, fill_value);
}

void grid_stamp_seeds(uint8_t *grid, int32_t map_width, int32_t map_height, uint8_t *stencil, int32_t span_x, int32_t span_y, int32_t origin_x, int32_t origin_y, int32_t seed_value) {
    MH_AI_FIRST_CALL("llm_strat_ai_grid_stamp_seeds");
    mh::ai::grid_stamp_seeds(grid, map_width, map_height, stencil, span_x, span_y, origin_x, origin_y, seed_value);
}

int32_t grid_match_stencil(uint8_t *grid, int32_t grid_width, int32_t grid_height, uint8_t *footprint_mask, int32_t span_x, int32_t span_y, int32_t start_x, int32_t start_y, int32_t target_byte) {
    MH_AI_FIRST_CALL("llm_strat_ai_grid_match_stencil");
    return mh::ai::grid_match_stencil(grid, grid_width, grid_height, footprint_mask, span_x, span_y, start_x, start_y, target_byte);
}

int32_t grid_stencil_all_near_unthreatened(uint8_t *grid, int32_t grid_width, int32_t grid_height, uint8_t *footprint_mask, int32_t span_x, int32_t span_y, int32_t start_x, int32_t start_y) {
    MH_AI_FIRST_CALL("llm_strat_ai_grid_stencil_all_near_unthreatened");
    return mh::ai::grid_stencil_all_near_unthreatened(grid, grid_width, grid_height, footprint_mask, span_x, span_y, start_x, start_y);
}

void grid_stamp_threat_ring(uint8_t *grid_base, int32_t grid_height, int32_t grid_width, int32_t center_y, int32_t center_x, int32_t radius) {
    MH_AI_FIRST_CALL("llm_strat_ai_grid_stamp_threat_ring");
    mh::ai::grid_stamp_threat_ring(grid_base, grid_height, grid_width, center_y, center_x, radius);
}

void grid_clear_threat_bit(uint8_t *grid_base, int32_t row_count, int32_t col_count) {
    MH_AI_FIRST_CALL("llm_strat_ai_grid_clear_threat_bit");
    mh::ai::grid_clear_threat_bit(grid_base, row_count, col_count);
}

void unit_commit_attack_on_enemy_hq() {
    MH_AI_FIRST_CALL("llm_strat_ai_unit_commit_attack_on_enemy_hq");
    mh::ai::unit_commit_attack_on_enemy_hq();
}

void start_hq_attack_scenario() {
    MH_AI_FIRST_CALL("llm_strat_ai_start_hq_attack_scenario");
    mh::ai::start_hq_attack_scenario();
}

uint32_t score_reinforcement_unit(int32_t player, int32_t unit_proto_id) {
    MH_AI_FIRST_CALL("llm_strat_ai_score_reinforcement_unit");
    return mh::ai::score_reinforcement_unit(player, unit_proto_id);
}

int32_t bldg_count_by_category(int32_t player, uint32_t category) {
    MH_AI_FIRST_CALL("llm_strat_ai_bldg_count_by_category");
    return mh::ai::bldg_count_by_category(player, category);
}

int32_t bldg_type_queue_has_pending(int32_t player_idx, uint32_t bldg_type) {
    MH_AI_FIRST_CALL("llm_strat_ai_bldg_type_queue_has_pending");
    return mh::ai::bldg_type_queue_has_pending(player_idx, bldg_type);
}

int32_t bldg_type_already_queued(int32_t player, uint32_t building_type) {
    MH_AI_FIRST_CALL("llm_strat_ai_bldg_type_already_queued");
    return mh::ai::bldg_type_already_queued(player, building_type);
}

int32_t group_has_split_group_link(int32_t player_id, uint32_t target_group_id) {
    MH_AI_FIRST_CALL("llm_strat_ai_group_has_split_group_link");
    return mh::ai::group_has_split_group_link(player_id, target_group_id);
}

int32_t target_ref_has_engageable_weapon(int32_t target_ref_kind, int32_t target_ref_index, uint32_t attacker_weapon_flags) {
    MH_AI_FIRST_CALL("llm_strat_ai_target_ref_has_engageable_weapon");
    return mh::ai::target_ref_has_engageable_weapon(target_ref_kind, target_ref_index, attacker_weapon_flags);
}

int32_t target_ref_is_alive(uint32_t target_ref_packed, int32_t target_index) {
    MH_AI_FIRST_CALL("llm_strat_ai_target_ref_is_alive");
    return mh::ai::target_ref_is_alive(target_ref_packed, target_index);
}

uint32_t unit_is_order_pending(uint32_t player, uint32_t unit_id) {
    MH_AI_FIRST_CALL("llm_strat_ai_unit_is_order_pending");
    return mh::ai::unit_is_order_pending(player, unit_id);
}

void group_member_unlink(uint32_t player, int32_t ai_group_index, uint32_t unit_id) {
    MH_AI_FIRST_CALL("llm_strat_ai_group_member_unlink");
    mh::ai::group_member_unlink(player, ai_group_index, unit_id);
}

void group_member_link(uint32_t player, int32_t ai_group_index, uint32_t unit_id) {
    MH_AI_FIRST_CALL("llm_strat_ai_group_member_link");
    mh::ai::group_member_link(player, ai_group_index, unit_id);
}

void group_member_move(uint32_t player, int32_t src_group, int32_t dst_group, int32_t unit_id) {
    MH_AI_FIRST_CALL("llm_strat_ai_group_member_move");
    mh::ai::group_member_move(player, src_group, dst_group, unit_id);
}

void group_task_preempt(int32_t param_1, int32_t param_2, int16_t a2, int32_t param_4, int32_t param_5, int32_t param_6, int32_t param_7, int32_t param_8, int16_t param_9) {
    MH_AI_FIRST_CALL("llm_strat_ai_group_task_preempt");
    mh::ai::group_task_preempt(param_1, param_2, a2, param_4, param_5, param_6, param_7, param_8, param_9);
}

void group_task_enqueue(int32_t param_1, int32_t param_2, uint16_t a2, uint32_t param_4, uint32_t param_5, uint32_t param_6, uint32_t param_7, uint32_t param_8, uint16_t param_9) {
    MH_AI_FIRST_CALL("llm_strat_ai_group_task_enqueue");
    mh::ai::group_task_enqueue(param_1, param_2, a2, param_4, param_5, param_6, param_7, param_8, param_9);
}

void group_task_dequeue(int32_t player_id, int32_t group_index) {
    MH_AI_FIRST_CALL("llm_strat_ai_group_task_dequeue");
    mh::ai::group_task_dequeue(player_id, group_index);
}

int32_t group_create(int32_t player_id) {
    MH_AI_FIRST_CALL("llm_strat_ai_group_create");
    return mh::ai::group_create(player_id);
}

void group_remove(int32_t player, uint32_t group_index) {
    MH_AI_FIRST_CALL("llm_strat_ai_group_remove");
    mh::ai::group_remove(player, group_index);
}

void engage_candidate_add(uint32_t target_ref, int32_t target_index) {
    MH_AI_FIRST_CALL("llm_strat_ai_engage_candidate_add");
    mh::ai::engage_candidate_add(target_ref, target_index);
}

void engage_partition_turret_candidates() {
    MH_AI_FIRST_CALL("llm_strat_ai_engage_partition_turret_candidates");
    mh::ai::engage_partition_turret_candidates();
}

// WAS UNBINDABLE UNTIL REBIND-AI-ESI (2026-09-10) and is the reason this file's banner used to name
// two exceptions instead of one. The original's third input is the register ESI, read as the bubble
// sort's "swapped" flag before anything writes it; with only two declared arguments no seam could
// carry it and the arm would have had to invent a value. The Ghidra prototype now declares it as a
// custom-storage ESI parameter, so `sig_` is three arguments wide and the generated entry thunk
// (MH_EXPORT_THUNK_e_void_EAX_EDX_ESI__callee) pushes the CALLER'S OWN ESI as the third one. That is
// strictly better than the constant a promoted caller passes: this seam sees whatever a still-
// original caller really had in the register, so an original->ours edge behaves as production does.
void engage_sort_candidates_by_dist(uint32_t source_ref, int32_t source_index,
                                    int32_t inherited_sorted_flag) {
    MH_AI_FIRST_CALL("llm_strat_ai_engage_sort_candidates_by_dist");
    mh::ai::engage_sort_candidates_by_dist(source_ref, source_index, inherited_sorted_flag);
}

void commit_attack_order(uint32_t param_1, int32_t param_2, uint32_t a2, int32_t param_4) {
    MH_AI_FIRST_CALL("llm_strat_ai_commit_attack_order");
    mh::ai::commit_attack_order(param_1, param_2, a2, param_4);
}

void commit_attack_order_alt(uint32_t param_1, int32_t param_2, uint32_t a2, int32_t param_4) {
    MH_AI_FIRST_CALL("llm_strat_ai_commit_attack_order_alt");
    mh::ai::commit_attack_order_alt(param_1, param_2, a2, param_4);
}

void unit_group_assign_by_type(uint32_t player, int32_t group_index, int32_t unit_id, int32_t exit_param) {
    MH_AI_FIRST_CALL("llm_strat_ai_unit_group_assign_by_type");
    mh::ai::unit_group_assign_by_type(player, group_index, unit_id, exit_param);
}

void unit_launch_from_storage_enqueue(uint8_t player, int32_t unit_id, uint32_t target_x, uint32_t target_y) {
    MH_AI_FIRST_CALL("llm_strat_ai_unit_launch_from_storage_enqueue");
    mh::ai::unit_launch_from_storage_enqueue(player, unit_id, target_x, target_y);
}

void route_unit_to_home_storage(uint32_t player, int32_t unit_id) {
    MH_AI_FIRST_CALL("llm_strat_ai_route_unit_to_home_storage");
    mh::ai::route_unit_to_home_storage(player, unit_id);
}

void group_compute_centroid(int32_t player, int32_t group_index, uint32_t *out_x, uint32_t *out_y) {
    MH_AI_FIRST_CALL("llm_strat_ai_group_compute_centroid");
    mh::ai::group_compute_centroid(player, group_index, out_x, out_y);
}

void group_rally_formup_worker(int32_t player_id, int32_t group_index) {
    MH_AI_FIRST_CALL("llm_strat_ai_group_rally_formup_worker");
    mh::ai::group_rally_formup_worker(player_id, group_index);
}

void random_point_near(int32_t x, int32_t y, int32_t radius, int32_t *out_x, int32_t *out_y) {
    MH_AI_FIRST_CALL("llm_strat_ai_random_point_near");
    mh::ai::random_point_near(x, y, radius, out_x, out_y);
}

void group_scatter_random_worker(int32_t param_1, int32_t param_2, uint32_t a2) {
    MH_AI_FIRST_CALL("llm_strat_ai_group_scatter_random_worker");
    mh::ai::group_scatter_random_worker(param_1, param_2, a2);
}

void group_reposition_members(uint32_t player, int32_t group_idx) {
    MH_AI_FIRST_CALL("llm_strat_ai_group_reposition_members");
    mh::ai::group_reposition_members(player, group_idx);
}

int32_t group_no_member_near_centroid(int32_t player, int32_t group_index) {
    MH_AI_FIRST_CALL("llm_strat_ai_group_no_member_near_centroid");
    return mh::ai::group_no_member_near_centroid(player, group_index);
}

int32_t group_area_scan_hostile(uint32_t player, int32_t group_index, uint8_t owner_mask) {
    MH_AI_FIRST_CALL("llm_strat_ai_group_area_scan_hostile");
    return mh::ai::group_area_scan_hostile(player, group_index, owner_mask);
}

int32_t group_all_units_settled(int32_t player_id, int32_t group_index) {
    MH_AI_FIRST_CALL("llm_strat_ai_group_all_units_settled");
    return mh::ai::group_all_units_settled(player_id, group_index);
}

uint32_t group_find_slowest_unit(int32_t player_id, int32_t group_index) {
    MH_AI_FIRST_CALL("llm_strat_ai_group_find_slowest_unit");
    return mh::ai::group_find_slowest_unit(player_id, group_index);
}

uint32_t group_pick_best_weapon_unit(int32_t player_id, int32_t group_index) {
    MH_AI_FIRST_CALL("llm_strat_ai_group_pick_best_weapon_unit");
    return mh::ai::group_pick_best_weapon_unit(player_id, group_index);
}

void target_list_add(uint32_t player, int32_t victim_ref, int32_t victim_index, uint32_t aggressor_ref, int32_t aggressor_index) {
    MH_AI_FIRST_CALL("llm_strat_ai_target_list_add");
    mh::ai::target_list_add(static_cast<int32_t>(player), victim_ref, victim_index, aggressor_ref, aggressor_index);
}

void target_list_remove(int32_t player, uint32_t aggressor_ref, int32_t aggressor_index) {
    MH_AI_FIRST_CALL("llm_strat_ai_target_list_remove");
    mh::ai::target_list_remove(player, aggressor_ref, aggressor_index);
}

int32_t unit_weapon_power(uint32_t player, uint32_t unit_id) {
    MH_AI_FIRST_CALL("llm_strat_ai_unit_weapon_power");
    return mh::ai::unit_weapon_power(player, unit_id);
}

int32_t target_ref_has_ground_weapon(uint32_t target_ref_packed, int32_t target_index) {
    MH_AI_FIRST_CALL("llm_strat_ai_target_ref_has_ground_weapon");
    return mh::ai::target_ref_has_ground_weapon(target_ref_packed, target_index);
}

int32_t target_ref_has_aa_weapon(uint32_t target_ref_packed, int32_t target_index) {
    MH_AI_FIRST_CALL("llm_strat_ai_target_ref_has_aa_weapon");
    return mh::ai::target_ref_has_aa_weapon(target_ref_packed, target_index);
}

void update_opponent_relations(uint32_t assessed_player, mh::game::mh_llm_strat_ai_opponent_assessment *out) {
    MH_AI_FIRST_CALL("llm_strat_ai_update_opponent_relations");
    mh::ai::update_opponent_relations(assessed_player, out);
}

void recompute_map_influence(int32_t player) {
    MH_AI_FIRST_CALL("llm_strat_ai_recompute_map_influence");
    mh::ai::recompute_map_influence(player);
}

uint32_t target_dist_sq(uint32_t ref_a, int32_t idx_a, uint32_t ref_b, int32_t idx_b) {
    MH_AI_FIRST_CALL("llm_strat_ai_target_dist_sq");
    return mh::ai::target_dist_sq(ref_a, idx_a, ref_b, idx_b);
}

void notify_map_changed(int32_t builder_player, int32_t building_type, int32_t tile_x, int32_t tile_y) {
    MH_AI_FIRST_CALL("llm_strat_ai_notify_map_changed");
    mh::ai::notify_map_changed(builder_player, building_type, tile_x, tile_y);
}

void notify_map_changed_2(int32_t builder_player, int32_t building_type, int32_t tile_x, int32_t tile_y) {
    MH_AI_FIRST_CALL("llm_strat_ai_notify_map_changed_2");
    mh::ai::notify_map_changed_2(builder_player, building_type, tile_x, tile_y);
}

int32_t building_defense_weapon_range(int32_t player, int32_t building_index) {
    MH_AI_FIRST_CALL("llm_strat_ai_building_defense_weapon_range");
    return mh::ai::building_defense_weapon_range(player, building_index);
}

void turret_threat_rescan(uint32_t player) {
    MH_AI_FIRST_CALL("llm_strat_ai_turret_threat_rescan");
    mh::ai::turret_threat_rescan(player);
}

int32_t bldg_has_heli_unit(int32_t player) {
    MH_AI_FIRST_CALL("llm_strat_ai_bldg_has_heli_unit");
    return mh::ai::bldg_has_heli_unit(player);
}

void notify_bldg_constructed(uint32_t player, uint32_t x_b, uint32_t param_3, uint32_t building_id, uint32_t y_b, uint32_t param_6) {
    MH_AI_FIRST_CALL("llm_strat_ai_notify_bldg_constructed");
    mh::ai::notify_bldg_constructed(player, x_b, param_3, building_id, y_b, param_6);
}

void register_attacker_damage(int32_t victim_index, uint32_t victim_ref, uint32_t aggressor_unit_index, uint32_t aggressor_ref, int32_t victim_destroyed) {
    MH_AI_FIRST_CALL("llm_strat_ai_bldg_register_visible_building");
    mh::ai::register_attacker_damage(victim_index, victim_ref, aggressor_unit_index, aggressor_ref, victim_destroyed);
}

void group_member_count_adjust(uint32_t player, uint32_t unit_index, uint32_t group_or_type, uint32_t mode) {
    MH_AI_FIRST_CALL("llm_strat_ai_group_member_count_adjust");
    mh::ai::group_member_count_adjust(player, unit_index, group_or_type, mode);
}

void notify_object_removed(uint32_t flags, uint32_t object_index, int32_t hard_remove) {
    MH_AI_FIRST_CALL("llm_strat_ai_notify_object_removed");
    mh::ai::notify_object_removed(flags, object_index, hard_remove);
}

void players_tick(double dt) {
    MH_AI_FIRST_CALL("llm_strat_ai_players_tick");
    // AI1-P island move: the untouched-check over the poisoned old .bss VAs rides the closure
    // root's call count, so the claim covers the played trajectory (ai_state.cpp, island_verify_tick
    // logs at 100/1000/10000/50000 and is a no-op when the island has not moved).
    static long s_calls = 0;
    mh::ai::island_verify_tick(++s_calls);
    mh::ai::players_tick(dt);
}

void notify_unit_lifecycle(uint16_t player_, uint16_t unit_type, uint32_t unit_id, uint32_t param_4) {
    MH_AI_FIRST_CALL("llm_strat_ai_notify_unit_lifecycle");
    mh::ai::notify_unit_lifecycle(player_, unit_type, unit_id, param_4);
}

void queue_release_order(int32_t player, int32_t building_index, int32_t mode) {
    MH_AI_FIRST_CALL("llm_strat_ai_queue_release_order");
    mh::ai::queue_release_order(player, building_index, mode);
}

void spiral_table_init() {
    MH_AI_FIRST_CALL("llm_strat_ai_spiral_table_init");
    mh::ai::spiral_table_init();
}

void build_plan_push(int32_t player, int32_t ai_build_id) {
    MH_AI_FIRST_CALL("llm_strat_ai_build_plan_push");
    mh::ai::build_plan_push(player, ai_build_id);
}

void init_build_candidate_priorities(int32_t player_index) {
    MH_AI_FIRST_CALL("llm_strat_ai_init_build_candidate_priorities");
    mh::ai::init_build_candidate_priorities(player_index);
}

void scr_parse(char *filename) {
    MH_AI_FIRST_CALL("llm_strat_ai_scr_parse");
    mh::ai::scr_parse(filename);
}

void count_unit_build_sources(int32_t player, int32_t *out_counts) {
    MH_AI_FIRST_CALL("llm_strat_ai_count_unit_build_sources");
    mh::ai::count_unit_build_sources(player, out_counts);
}

int32_t queue_train_unit(int32_t player, uint32_t unit_id) {
    MH_AI_FIRST_CALL("llm_strat_ai_queue_train_unit");
    return mh::ai::queue_train_unit(player, unit_id);
}

int32_t queue_bldg_repair(int32_t player, int32_t building_index) {
    MH_AI_FIRST_CALL("llm_strat_ai_queue_bldg_repair");
    return mh::ai::queue_bldg_repair(player, building_index);
}

int32_t queue_bldg_upgrade(int32_t player, uint32_t building_index) {
    MH_AI_FIRST_CALL("llm_strat_ai_queue_bldg_upgrade");
    return mh::ai::queue_bldg_upgrade(player, building_index);
}

void queue_flush_unit_train_entries(int32_t player) {
    MH_AI_FIRST_CALL("llm_strat_ai_queue_flush_unit_train_entries");
    mh::ai::queue_flush_unit_train_entries(player);
}

void queue_flush_unit_train_entries_2(int32_t player) {
    MH_AI_FIRST_CALL("llm_strat_ai_queue_flush_unit_train_entries_2");
    mh::ai::queue_flush_unit_train_entries_2(player);
}

void queue_rotate_newest_to_front(int32_t player) {
    MH_AI_FIRST_CALL("llm_strat_ai_queue_rotate_newest_to_front");
    mh::ai::queue_rotate_newest_to_front(player);
}

void queue_reconcile_bldg_change(uint32_t player, uint32_t building_index) {
    MH_AI_FIRST_CALL("llm_strat_ai_queue_reconcile_bldg_change");
    mh::ai::queue_reconcile_bldg_change(player, building_index);
}

void calc_mine_yield_estimate(int32_t building_id, uint32_t tile_x, uint32_t tile_y, int32_t *out_yield, int32_t *out_quality) {
    MH_AI_FIRST_CALL("llm_strat_ai_calc_mine_yield_estimate");
    mh::ai::calc_mine_yield_estimate(building_id, tile_x, tile_y, out_yield, out_quality);
}

void mine_portfolio_rebalance(uint32_t player, int32_t *out_yield) {
    MH_AI_FIRST_CALL("llm_strat_ai_mine_portfolio_rebalance");
    mh::ai::mine_portfolio_rebalance(player, out_yield);
}

int32_t storage_capacity_short_and_cap_check(int32_t player) {
    MH_AI_FIRST_CALL("llm_strat_ai_storage_capacity_short_and_cap_check");
    return mh::ai::storage_capacity_short_and_cap_check(player);
}

double calc_power_supply_ratio(int32_t player) {
    MH_AI_FIRST_CALL("llm_strat_ai_calc_power_supply_ratio");
    return mh::ai::calc_power_supply_ratio(player);
}

int32_t is_worker_priority_candidate(int32_t player, int32_t site_index) {
    MH_AI_FIRST_CALL("llm_strat_ai_is_worker_priority_candidate");
    return static_cast<int32_t>(mh::ai::is_worker_priority_candidate(player, site_index));
}

int32_t rebalance_building_workers(uint32_t player) {
    MH_AI_FIRST_CALL("llm_strat_ai_rebalance_building_workers");
    return mh::ai::rebalance_building_workers(player);
}

int32_t scan_construction_sites(int32_t player, int32_t category) {
    MH_AI_FIRST_CALL("llm_strat_ai_scan_construction_sites");
    return mh::ai::scan_construction_sites(player, category);
}

int32_t find_nearest_flagged_building(int32_t player, int32_t x, int32_t y) {
    MH_AI_FIRST_CALL("llm_strat_ai_find_nearest_flagged_building");
    return mh::ai::find_nearest_flagged_building(player, x, y);
}

void plan_turret_upgrade(uint32_t player) {
    MH_AI_FIRST_CALL("llm_strat_ai_plan_turret_upgrade");
    mh::ai::plan_turret_upgrade(player);
}

void plan_mine_construction(int32_t player) {
    MH_AI_FIRST_CALL("llm_strat_ai_plan_mine_construction");
    mh::ai::plan_mine_construction(player);
}

void score_build_categories(int32_t player) {
    MH_AI_FIRST_CALL("llm_strat_ai_score_build_categories");
    mh::ai::score_build_categories(player);
}

int32_t player_score_tier(int32_t player_idx) {
    MH_AI_FIRST_CALL("llm_strat_ai_player_score_tier");
    return mh::ai::player_score_tier(player_idx);
}

void react_resource_shortage(int32_t player) {
    MH_AI_FIRST_CALL("llm_strat_ai_react_resource_shortage");
    mh::ai::react_resource_shortage(player);
}

void maintain_unit_housing(int32_t player) {
    MH_AI_FIRST_CALL("llm_strat_ai_maintain_unit_housing");
    mh::ai::maintain_unit_housing(player);
}

void plan_construction(uint32_t player_idx) {
    MH_AI_FIRST_CALL("llm_strat_ai_plan_construction");
    mh::ai::plan_construction(player_idx);
}

void queue_remove_at(int32_t player, uint32_t slot_index) {
    MH_AI_FIRST_CALL("llm_strat_ai_queue_remove_at");
    mh::ai::queue_remove_at(player, slot_index);
}

void scan_bldg_repair_upgrade(int32_t player) {
    MH_AI_FIRST_CALL("llm_strat_ai_scan_bldg_repair_upgrade");
    mh::ai::scan_bldg_repair_upgrade(player);
}

void sort_site_candidates_by_dist() {
    MH_AI_FIRST_CALL("llm_strat_ai_sort_site_candidates_by_dist");
    mh::ai::sort_site_candidates_by_dist();
}

int32_t resource_site_meets_threshold(int32_t player_idx, int32_t building_type, int32_t fine_x, int32_t fine_y) {
    MH_AI_FIRST_CALL("llm_strat_ai_resource_site_meets_threshold");
    return static_cast<int32_t>(mh::ai::resource_site_meets_threshold(player_idx, building_type, fine_x, fine_y));
}

void bldg_scan_resource_site_candidates(uint32_t player, int32_t building_type) {
    MH_AI_FIRST_CALL("llm_strat_ai_bldg_scan_resource_site_candidates");
    mh::ai::bldg_scan_resource_site_candidates(player, building_type);
}

void bldg_scan_grid_candidates(uint32_t player, int32_t building_idx) {
    MH_AI_FIRST_CALL("llm_strat_ai_bldg_scan_grid_candidates");
    mh::ai::bldg_scan_grid_candidates(player, building_idx);
}

void scan_build_site_candidates(int32_t player_idx, int32_t building_idx) {
    MH_AI_FIRST_CALL("llm_strat_ai_scan_build_site_candidates");
    mh::ai::scan_build_site_candidates(player_idx, building_idx);
}

void group_split_off_create(uint32_t player, int32_t centroid_x, int32_t centroid_y, int32_t source_group_idx, uint32_t member_count) {
    MH_AI_FIRST_CALL("llm_strat_ai_group_split_off_create");
    mh::ai::group_split_off_create(player, centroid_x, centroid_y, source_group_idx, member_count);
}

void group_split_excess_members(uint32_t player, int32_t group_idx) {
    MH_AI_FIRST_CALL("llm_strat_ai_group_split_excess_members");
    mh::ai::group_split_excess_members(player, group_idx);
}

void group_redistribute_units(uint32_t player, int32_t group_index) {
    MH_AI_FIRST_CALL("llm_strat_ai_group_redistribute_units");
    mh::ai::group_redistribute_units(player, group_index);
}

void plan_unit_training(int32_t player) {
    MH_AI_FIRST_CALL("llm_strat_ai_plan_unit_training");
    mh::ai::plan_unit_training(player);
}

void group_home_guard_replenish(int32_t player_id) {
    MH_AI_FIRST_CALL("llm_strat_ai_group_home_guard_replenish");
    mh::ai::group_home_guard_replenish(player_id);
}

void pick_owned_tile_or_home(int32_t param_1, uint32_t *param_2, uint32_t *a2) {
    MH_AI_FIRST_CALL("llm_strat_ai_pick_owned_tile_or_home");
    mh::ai::pick_owned_tile_or_home(param_1, param_2, a2);
}

void form_standby_from_pool3(int32_t player_id) {
    MH_AI_FIRST_CALL("llm_strat_ai_group_form_standby_from_pool3");
    mh::ai::form_standby_from_pool3(player_id);
}

void form_surplus_from_pool4(int32_t player_id) {
    MH_AI_FIRST_CALL("llm_strat_ai_group_form_surplus_from_pool4");
    mh::ai::form_surplus_from_pool4(player_id);
}

void army_milestone_advance_or_attack(uint32_t player) {
    MH_AI_FIRST_CALL("llm_strat_ai_army_milestone_advance_or_attack");
    mh::ai::army_milestone_advance_or_attack(player);
}

void group_expansion_form_or_repurpose(uint32_t player_id) {
    MH_AI_FIRST_CALL("llm_strat_ai_group_expansion_form_or_repurpose");
    mh::ai::group_expansion_form_or_repurpose(player_id);
}

void form_patrol(int32_t player_id) {
    MH_AI_FIRST_CALL("llm_strat_ai_group_form_patrol");
    mh::ai::form_patrol(player_id);
}

void resource_spend_rate_update(int32_t player) {
    MH_AI_FIRST_CALL("llm_strat_ai_resource_spend_rate_update");
    mh::ai::resource_spend_rate_update(player);
}

void bldg_production_type_dispatch(uint32_t player_id, int32_t building_id) {
    MH_AI_FIRST_CALL("llm_strat_ai_bldg_production_type_dispatch");
    mh::ai::bldg_production_type_dispatch(player_id, building_id);
}

void bldg_queue_process_entry(uint32_t player, int32_t queue_index) {
    MH_AI_FIRST_CALL("llm_strat_ai_bldg_queue_process_entry");
    mh::ai::bldg_queue_process_entry(player, queue_index);
}

void bldg_queue_handle_recruit_state(uint32_t player_id, int32_t queue_index) {
    MH_AI_FIRST_CALL("llm_strat_ai_bldg_queue_handle_recruit_state");
    mh::ai::bldg_queue_handle_recruit_state(player_id, queue_index);
}

void bldg_queue_handle_state2_empty() {
    MH_AI_FIRST_CALL("llm_strat_ai_bldg_queue_handle_state2_empty");
    mh::ai::bldg_queue_handle_state2_empty();
}

void bldg_queue_handle_upgrade_or_cancel(uint32_t player_id, int32_t queue_index) {
    MH_AI_FIRST_CALL("llm_strat_ai_bldg_queue_handle_upgrade_or_cancel");
    mh::ai::bldg_queue_handle_upgrade_or_cancel(player_id, queue_index);
}

void bldg_queue_process(uint32_t player_id) {
    MH_AI_FIRST_CALL("llm_strat_ai_bldg_queue_process");
    mh::ai::bldg_queue_process(player_id);
}

void recompute_shortage_state(uint32_t player_idx) {
    MH_AI_FIRST_CALL("llm_strat_ai_recompute_shortage_state");
    mh::ai::recompute_shortage_state(player_idx);
}

int32_t invasion_spawn_reinforcements(int32_t player_idx) {
    MH_AI_FIRST_CALL("llm_strat_ai_invasion_spawn_reinforcements");
    return mh::ai::invasion_spawn_reinforcements(player_idx);
}

void invasion_launch_attack_group(uint32_t player) {
    MH_AI_FIRST_CALL("llm_strat_ai_invasion_launch_attack_group");
    mh::ai::invasion_launch_attack_group(player);
}

void player_tick(int32_t player) {
    MH_AI_FIRST_CALL("llm_strat_ai_player_tick");
    mh::ai::player_tick(player);
}

int32_t group_check_arrival_status(uint32_t player, int32_t ai_group_id) {
    MH_AI_FIRST_CALL("llm_strat_ai_group_check_arrival_status");
    return mh::ai::group_check_arrival_status(player, ai_group_id);
}

uint32_t group_task_step(uint32_t player_id, int32_t group_index) {
    MH_AI_FIRST_CALL("llm_strat_ai_group_task_step");
    return mh::ai::group_task_step(player_id, group_index);
}

int32_t group_find_nearest_building_of_types(int32_t player_id, int32_t query_x, int32_t query_y, uint32_t building_type_1, uint32_t building_type_2, uint32_t building_type_3, uint32_t building_type_4) {
    MH_AI_FIRST_CALL("llm_strat_ai_group_find_nearest_building_of_types");
    return mh::ai::group_find_nearest_building_of_types(player_id, query_x, query_y, building_type_1, building_type_2, building_type_3, building_type_4);
}

void group_collect_buildings_of_types(int32_t player_id, uint32_t unused_edx_slot, uint32_t unused_ebx_slot, uint32_t building_type_1, uint32_t building_type_2, uint32_t building_type_3, uint32_t building_type_4) {
    MH_AI_FIRST_CALL("llm_strat_ai_group_collect_buildings_of_types");
    mh::ai::group_collect_buildings_of_types(player_id, unused_edx_slot, unused_ebx_slot, building_type_1, building_type_2, building_type_3, building_type_4);
}

void group_task_attack_random_target(int32_t player_id, int32_t group_index) {
    MH_AI_FIRST_CALL("llm_strat_ai_group_task_attack_random_target");
    mh::ai::group_task_attack_random_target(player_id, group_index);
}

void group_task_drain_reserve_attack(uint32_t player_id, int32_t group_index) {
    MH_AI_FIRST_CALL("llm_strat_ai_group_task_drain_reserve_attack");
    mh::ai::group_task_drain_reserve_attack(player_id, group_index);
}

void group_task_engage_target(int32_t player_id, int32_t group_index) {
    MH_AI_FIRST_CALL("llm_strat_ai_group_task_engage_target");
    mh::ai::group_task_engage_target(player_id, group_index);
}

void group_task_attack_nearest_defended(int32_t player_id, int32_t group_index) {
    MH_AI_FIRST_CALL("llm_strat_ai_group_task_attack_nearest_defended");
    mh::ai::group_task_attack_nearest_defended(player_id, group_index);
}

void group_task_muster_from_pool(uint32_t player_id, int32_t group_index) {
    MH_AI_FIRST_CALL("llm_strat_ai_group_task_muster_from_pool");
    mh::ai::group_task_muster_from_pool(player_id, group_index);
}

void group_task_recruit_from_pool3(uint32_t player_id, int32_t group_index) {
    MH_AI_FIRST_CALL("llm_strat_ai_group_task_recruit_from_pool3");
    mh::ai::group_task_recruit_from_pool3(player_id, group_index);
}

void group_task_recruit_from_pool4(uint32_t player_id, int32_t group_index) {
    MH_AI_FIRST_CALL("llm_strat_ai_group_task_recruit_from_pool4");
    mh::ai::group_task_recruit_from_pool4(player_id, group_index);
}

void group_task_recruit_from_storage(uint32_t player_id, int32_t group_index) {
    MH_AI_FIRST_CALL("llm_strat_ai_group_task_recruit_from_storage");
    mh::ai::group_task_recruit_from_storage(player_id, group_index);
}

void group_task_disband(uint32_t player_id, int32_t group_index) {
    MH_AI_FIRST_CALL("llm_strat_ai_group_task_disband");
    mh::ai::group_task_disband(player_id, group_index);
}

void group_task_hold() {
    MH_AI_FIRST_CALL("llm_strat_ai_group_task_hold");
    mh::ai::group_task_hold();
}

void group_task_recall_home(int32_t player_id, int32_t group_index) {
    MH_AI_FIRST_CALL("llm_strat_ai_group_task_recall_home");
    mh::ai::group_task_recall_home(player_id, group_index);
}

void group_task_patrol_shuttle(int32_t player_id, int32_t group_index) {
    MH_AI_FIRST_CALL("llm_strat_ai_group_task_patrol_shuttle");
    mh::ai::group_task_patrol_shuttle(player_id, group_index);
}

void group_task_loiter_wander(int32_t player_id, int32_t group_index) {
    MH_AI_FIRST_CALL("llm_strat_ai_group_task_loiter_wander");
    mh::ai::group_task_loiter_wander(player_id, group_index);
}

void group_task_rally_formup(int32_t player_id, int32_t group_index) {
    MH_AI_FIRST_CALL("llm_strat_ai_group_task_rally_formup");
    mh::ai::group_task_rally_formup(player_id, group_index);
}

void group_task_scatter_random(int32_t player_id, int32_t group_index) {
    MH_AI_FIRST_CALL("llm_strat_ai_group_task_scatter_random");
    mh::ai::group_task_scatter_random(player_id, group_index);
}

void group_task_nudge_stragglers(uint32_t player_id, int32_t group_index) {
    MH_AI_FIRST_CALL("llm_strat_ai_group_task_nudge_stragglers");
    mh::ai::group_task_nudge_stragglers(player_id, group_index);
}

void group_task_wait() {
    MH_AI_FIRST_CALL("llm_strat_ai_group_task_wait");
    mh::ai::group_task_wait();
}

void group_task_advance_to_anchor(int32_t player_id, int32_t group_index) {
    MH_AI_FIRST_CALL("llm_strat_ai_group_task_advance_to_anchor");
    mh::ai::group_task_advance_to_anchor(player_id, group_index);
}

void group_task_disperse_passable(uint32_t player_id, int32_t group_index) {
    MH_AI_FIRST_CALL("llm_strat_ai_group_task_disperse_passable");
    mh::ai::group_task_disperse_passable(player_id, group_index);
}

void group_task_activate(int32_t player_id, int32_t group_index) {
    MH_AI_FIRST_CALL("llm_strat_ai_group_task_activate");
    mh::ai::group_task_activate(player_id, group_index);
}

void group_enter_hold(int32_t player_id, int32_t group_index) {
    MH_AI_FIRST_CALL("llm_strat_ai_group_enter_hold");
    mh::ai::group_enter_hold(player_id, group_index);
}

void group1_drain_to_group0(uint32_t player_id) {
    MH_AI_FIRST_CALL("llm_strat_ai_group1_drain_to_group0");
    mh::ai::group1_drain_to_group0(player_id);
}

void unit_group_tick(int32_t player) {
    MH_AI_FIRST_CALL("llm_strat_ai_unit_group_tick");
    mh::ai::unit_group_tick(player);
}

void group_classify_target_object(uint32_t player_id, mh::game::mh_llm_strat_ai_scan_target_entry *target_record) {
    MH_AI_FIRST_CALL("llm_strat_ai_group_classify_target_object");
    mh::ai::group_classify_target_object(player_id, static_cast<scan_target_entry *>(target_record));
}

void scan_target_list_add(int32_t player, uint32_t target_ref, int32_t target_index) {
    MH_AI_FIRST_CALL("llm_strat_ai_scan_target_list_add");
    mh::ai::scan_target_list_add(static_cast<uint32_t>(player), target_ref, target_index);
}

int32_t holding_pen_scan_targets(int32_t player, uint32_t pen_index) {
    MH_AI_FIRST_CALL("llm_strat_ai_holding_pen_scan_targets");
    return mh::ai::holding_pen_scan_targets(player, pen_index);
}

int32_t target_list_invalidate_by_id(int32_t player, int32_t target_id) {
    MH_AI_FIRST_CALL("llm_strat_ai_target_list_invalidate_by_id");
    return mh::ai::target_list_invalidate_by_id(player, target_id);
}

int32_t target_list_refresh_mothers(int32_t player) {
    MH_AI_FIRST_CALL("llm_strat_ai_target_list_refresh_mothers");
    return mh::ai::target_list_refresh_mothers(player);
}

int32_t target_list_scan_visible_enemies(uint32_t player) {
    MH_AI_FIRST_CALL("llm_strat_ai_target_list_scan_visible_enemies");
    return mh::ai::target_list_scan_visible_enemies(player);
}

uint8_t group_seed_resolved_target(int32_t player_id, int32_t group_index) {
    MH_AI_FIRST_CALL("llm_strat_ai_group_seed_resolved_target");
    return mh::ai::group_seed_resolved_target(player_id, group_index);
}

int32_t scan_target_sort_cmp(void *a, void *b) {
    MH_AI_FIRST_CALL("llm_strat_ai_scan_target_sort_cmp");
    return mh::ai::scan_target_sort_cmp(a, b);
}

void scan_target_list_sort(void *base, uint32_t count) {
    MH_AI_FIRST_CALL("llm_strat_ai_scan_target_list_sort");
    mh::ai::scan_target_list_sort(base, count);
}

void attack_candidate_add(int32_t player, int32_t unit_index) {
    MH_AI_FIRST_CALL("llm_strat_ai_attack_candidate_add");
    mh::ai::attack_candidate_add(player, unit_index);
}

int32_t unit_should_abandon_target(uint32_t player, int32_t unit_index) {
    MH_AI_FIRST_CALL("llm_strat_ai_unit_should_abandon_target");
    return static_cast<int32_t>(mh::ai::unit_should_abandon_target(player, unit_index));
}

int32_t scan_targets_for_engage(int32_t player_idx, int32_t target_unit_id) {
    MH_AI_FIRST_CALL("llm_strat_ai_scan_targets_for_engage");
    return mh::ai::scan_targets_for_engage(player_idx, target_unit_id);
}

int32_t scan_target_list_for_engage_candidates(int32_t player, uint16_t ai_group_mask) {
    MH_AI_FIRST_CALL("llm_strat_ai_scan_target_list_for_engage_candidates");
    return mh::ai::scan_target_list_for_engage_candidates(player, ai_group_mask);
}

int32_t scan_spiral_ring_for_engage_candidates(int32_t player, int32_t x, int32_t y, int32_t ring_index, uint32_t target_mask) {
    MH_AI_FIRST_CALL("llm_strat_ai_scan_spiral_ring_for_engage_candidates");
    return mh::ai::scan_spiral_ring_for_engage_candidates(player, x, y, ring_index, target_mask);
}

int32_t unit_scan_engage_candidates_in_range(int32_t player, int32_t unit_id, uint32_t target_mask) {
    MH_AI_FIRST_CALL("llm_strat_ai_unit_scan_engage_candidates_in_range");
    return mh::ai::unit_scan_engage_candidates_in_range(player, unit_id, target_mask);
}

int32_t engage_select_and_commit(int32_t player, int32_t unit_index, int32_t use_alt_commit) {
    MH_AI_FIRST_CALL("llm_strat_ai_engage_select_and_commit");
    return static_cast<int32_t>(mh::ai::engage_select_and_commit(player, unit_index, use_alt_commit));
}

int32_t engage_filter_and_commit_target(uint32_t attacker_ref, uint32_t context) {
    MH_AI_FIRST_CALL("llm_strat_ai_engage_filter_and_commit_target");
    return mh::ai::engage_filter_and_commit_target(attacker_ref, static_cast<int32_t>(context));
}

void active_unit_tick(int32_t player) {
    MH_AI_FIRST_CALL("llm_strat_ai_active_unit_tick");
    mh::ai::active_unit_tick(player);
}

} // namespace mh::ai::promoted_arm

// The 166 live seams. ONE row of the 167-row verified set is absent -- see the banner above.
MH_EXPORT_REPLACE(llm_strat_ai_unit_order_move_with_bump, mh::ai::promoted_arm::unit_order_move_with_bump)
MH_EXPORT_REPLACE(llm_strat_ai_create_reinforcement_unit, mh::ai::promoted_arm::create_reinforcement_unit)
MH_EXPORT_REPLACE(llm_strat_ai_grid_fill_below_threshold, mh::ai::promoted_arm::grid_fill_below_threshold)
MH_EXPORT_REPLACE(llm_strat_ai_grid_flood_step, mh::ai::promoted_arm::grid_flood_step)
MH_EXPORT_REPLACE(llm_strat_ai_grid_stamp_seeds, mh::ai::promoted_arm::grid_stamp_seeds)
MH_EXPORT_REPLACE(llm_strat_ai_grid_match_stencil, mh::ai::promoted_arm::grid_match_stencil)
MH_EXPORT_REPLACE(llm_strat_ai_grid_stencil_all_near_unthreatened, mh::ai::promoted_arm::grid_stencil_all_near_unthreatened)
MH_EXPORT_REPLACE(llm_strat_ai_grid_stamp_threat_ring, mh::ai::promoted_arm::grid_stamp_threat_ring)
MH_EXPORT_REPLACE(llm_strat_ai_grid_clear_threat_bit, mh::ai::promoted_arm::grid_clear_threat_bit)
MH_EXPORT_REPLACE(llm_strat_ai_unit_commit_attack_on_enemy_hq, mh::ai::promoted_arm::unit_commit_attack_on_enemy_hq)
MH_EXPORT_REPLACE(llm_strat_ai_start_hq_attack_scenario, mh::ai::promoted_arm::start_hq_attack_scenario)
MH_EXPORT_REPLACE(llm_strat_ai_score_reinforcement_unit, mh::ai::promoted_arm::score_reinforcement_unit)
MH_EXPORT_REPLACE(llm_strat_ai_bldg_count_by_category, mh::ai::promoted_arm::bldg_count_by_category)
MH_EXPORT_REPLACE(llm_strat_ai_bldg_type_queue_has_pending, mh::ai::promoted_arm::bldg_type_queue_has_pending)
MH_EXPORT_REPLACE(llm_strat_ai_bldg_type_already_queued, mh::ai::promoted_arm::bldg_type_already_queued)
MH_EXPORT_REPLACE(llm_strat_ai_group_has_split_group_link, mh::ai::promoted_arm::group_has_split_group_link)
MH_EXPORT_REPLACE(llm_strat_ai_target_ref_has_engageable_weapon, mh::ai::promoted_arm::target_ref_has_engageable_weapon)
MH_EXPORT_REPLACE(llm_strat_ai_target_ref_is_alive, mh::ai::promoted_arm::target_ref_is_alive)
MH_EXPORT_REPLACE(llm_strat_ai_unit_is_order_pending, mh::ai::promoted_arm::unit_is_order_pending)
MH_EXPORT_REPLACE(llm_strat_ai_group_member_unlink, mh::ai::promoted_arm::group_member_unlink)
MH_EXPORT_REPLACE(llm_strat_ai_group_member_link, mh::ai::promoted_arm::group_member_link)
MH_EXPORT_REPLACE(llm_strat_ai_group_member_move, mh::ai::promoted_arm::group_member_move)
MH_EXPORT_REPLACE(llm_strat_ai_group_task_preempt, mh::ai::promoted_arm::group_task_preempt)
MH_EXPORT_REPLACE(llm_strat_ai_group_task_enqueue, mh::ai::promoted_arm::group_task_enqueue)
MH_EXPORT_REPLACE(llm_strat_ai_group_task_dequeue, mh::ai::promoted_arm::group_task_dequeue)
MH_EXPORT_REPLACE(llm_strat_ai_group_create, mh::ai::promoted_arm::group_create)
MH_EXPORT_REPLACE(llm_strat_ai_group_remove, mh::ai::promoted_arm::group_remove)
MH_EXPORT_REPLACE(llm_strat_ai_engage_candidate_add, mh::ai::promoted_arm::engage_candidate_add)
MH_EXPORT_REPLACE(llm_strat_ai_engage_partition_turret_candidates, mh::ai::promoted_arm::engage_partition_turret_candidates)
MH_EXPORT_REPLACE(llm_strat_ai_engage_sort_candidates_by_dist, mh::ai::promoted_arm::engage_sort_candidates_by_dist)
MH_EXPORT_REPLACE(llm_strat_ai_commit_attack_order, mh::ai::promoted_arm::commit_attack_order)
MH_EXPORT_REPLACE(llm_strat_ai_commit_attack_order_alt, mh::ai::promoted_arm::commit_attack_order_alt)
MH_EXPORT_REPLACE(llm_strat_ai_unit_group_assign_by_type, mh::ai::promoted_arm::unit_group_assign_by_type)
MH_EXPORT_REPLACE(llm_strat_ai_unit_launch_from_storage_enqueue, mh::ai::promoted_arm::unit_launch_from_storage_enqueue)
MH_EXPORT_REPLACE(llm_strat_ai_route_unit_to_home_storage, mh::ai::promoted_arm::route_unit_to_home_storage)
MH_EXPORT_REPLACE(llm_strat_ai_group_compute_centroid, mh::ai::promoted_arm::group_compute_centroid)
MH_EXPORT_REPLACE(llm_strat_ai_group_rally_formup_worker, mh::ai::promoted_arm::group_rally_formup_worker)
MH_EXPORT_REPLACE(llm_strat_ai_random_point_near, mh::ai::promoted_arm::random_point_near)
MH_EXPORT_REPLACE(llm_strat_ai_group_scatter_random_worker, mh::ai::promoted_arm::group_scatter_random_worker)
MH_EXPORT_REPLACE(llm_strat_ai_group_reposition_members, mh::ai::promoted_arm::group_reposition_members)
MH_EXPORT_REPLACE(llm_strat_ai_group_no_member_near_centroid, mh::ai::promoted_arm::group_no_member_near_centroid)
MH_EXPORT_REPLACE(llm_strat_ai_group_area_scan_hostile, mh::ai::promoted_arm::group_area_scan_hostile)
MH_EXPORT_REPLACE(llm_strat_ai_group_all_units_settled, mh::ai::promoted_arm::group_all_units_settled)
MH_EXPORT_REPLACE(llm_strat_ai_group_find_slowest_unit, mh::ai::promoted_arm::group_find_slowest_unit)
MH_EXPORT_REPLACE(llm_strat_ai_group_pick_best_weapon_unit, mh::ai::promoted_arm::group_pick_best_weapon_unit)
MH_EXPORT_REPLACE(llm_strat_ai_target_list_add, mh::ai::promoted_arm::target_list_add)
MH_EXPORT_REPLACE(llm_strat_ai_target_list_remove, mh::ai::promoted_arm::target_list_remove)
MH_EXPORT_REPLACE(llm_strat_ai_unit_weapon_power, mh::ai::promoted_arm::unit_weapon_power)
MH_EXPORT_REPLACE(llm_strat_ai_target_ref_has_ground_weapon, mh::ai::promoted_arm::target_ref_has_ground_weapon)
MH_EXPORT_REPLACE(llm_strat_ai_target_ref_has_aa_weapon, mh::ai::promoted_arm::target_ref_has_aa_weapon)
MH_EXPORT_REPLACE(llm_strat_ai_update_opponent_relations, mh::ai::promoted_arm::update_opponent_relations)
MH_EXPORT_REPLACE(llm_strat_ai_recompute_map_influence, mh::ai::promoted_arm::recompute_map_influence)
MH_EXPORT_REPLACE(llm_strat_ai_target_dist_sq, mh::ai::promoted_arm::target_dist_sq)
MH_EXPORT_REPLACE(llm_strat_ai_notify_map_changed, mh::ai::promoted_arm::notify_map_changed)
MH_EXPORT_REPLACE(llm_strat_ai_notify_map_changed_2, mh::ai::promoted_arm::notify_map_changed_2)
MH_EXPORT_REPLACE(llm_strat_ai_building_defense_weapon_range, mh::ai::promoted_arm::building_defense_weapon_range)
MH_EXPORT_REPLACE(llm_strat_ai_turret_threat_rescan, mh::ai::promoted_arm::turret_threat_rescan)
MH_EXPORT_REPLACE(llm_strat_ai_bldg_has_heli_unit, mh::ai::promoted_arm::bldg_has_heli_unit)
MH_EXPORT_REPLACE(llm_strat_ai_notify_bldg_constructed, mh::ai::promoted_arm::notify_bldg_constructed)
MH_EXPORT_REPLACE(llm_strat_ai_bldg_register_visible_building, mh::ai::promoted_arm::register_attacker_damage)
MH_EXPORT_REPLACE(llm_strat_ai_group_member_count_adjust, mh::ai::promoted_arm::group_member_count_adjust)
MH_EXPORT_REPLACE(llm_strat_ai_notify_object_removed, mh::ai::promoted_arm::notify_object_removed)
MH_EXPORT_REPLACE(llm_strat_ai_players_tick, mh::ai::promoted_arm::players_tick)
MH_EXPORT_REPLACE(llm_strat_ai_notify_unit_lifecycle, mh::ai::promoted_arm::notify_unit_lifecycle)
MH_EXPORT_REPLACE(llm_strat_ai_queue_release_order, mh::ai::promoted_arm::queue_release_order)
MH_EXPORT_REPLACE(llm_strat_ai_spiral_table_init, mh::ai::promoted_arm::spiral_table_init)
MH_EXPORT_REPLACE(llm_strat_ai_build_plan_push, mh::ai::promoted_arm::build_plan_push)
MH_EXPORT_REPLACE(llm_strat_ai_init_build_candidate_priorities, mh::ai::promoted_arm::init_build_candidate_priorities)
MH_EXPORT_REPLACE(llm_strat_ai_scr_parse, mh::ai::promoted_arm::scr_parse)
MH_EXPORT_REPLACE(llm_strat_ai_count_unit_build_sources, mh::ai::promoted_arm::count_unit_build_sources)
MH_EXPORT_REPLACE(llm_strat_ai_queue_train_unit, mh::ai::promoted_arm::queue_train_unit)
MH_EXPORT_REPLACE(llm_strat_ai_queue_bldg_repair, mh::ai::promoted_arm::queue_bldg_repair)
MH_EXPORT_REPLACE(llm_strat_ai_queue_bldg_upgrade, mh::ai::promoted_arm::queue_bldg_upgrade)
MH_EXPORT_REPLACE(llm_strat_ai_queue_flush_unit_train_entries, mh::ai::promoted_arm::queue_flush_unit_train_entries)
MH_EXPORT_REPLACE(llm_strat_ai_queue_flush_unit_train_entries_2, mh::ai::promoted_arm::queue_flush_unit_train_entries_2)
MH_EXPORT_REPLACE(llm_strat_ai_queue_rotate_newest_to_front, mh::ai::promoted_arm::queue_rotate_newest_to_front)
MH_EXPORT_REPLACE(llm_strat_ai_queue_reconcile_bldg_change, mh::ai::promoted_arm::queue_reconcile_bldg_change)
MH_EXPORT_REPLACE(llm_strat_ai_calc_mine_yield_estimate, mh::ai::promoted_arm::calc_mine_yield_estimate)
MH_EXPORT_REPLACE(llm_strat_ai_mine_portfolio_rebalance, mh::ai::promoted_arm::mine_portfolio_rebalance)
MH_EXPORT_REPLACE(llm_strat_ai_storage_capacity_short_and_cap_check, mh::ai::promoted_arm::storage_capacity_short_and_cap_check)
MH_EXPORT_REPLACE(llm_strat_ai_calc_power_supply_ratio, mh::ai::promoted_arm::calc_power_supply_ratio)
MH_EXPORT_REPLACE(llm_strat_ai_is_worker_priority_candidate, mh::ai::promoted_arm::is_worker_priority_candidate)
MH_EXPORT_REPLACE(llm_strat_ai_rebalance_building_workers, mh::ai::promoted_arm::rebalance_building_workers)
MH_EXPORT_REPLACE(llm_strat_ai_scan_construction_sites, mh::ai::promoted_arm::scan_construction_sites)
MH_EXPORT_REPLACE(llm_strat_ai_find_nearest_flagged_building, mh::ai::promoted_arm::find_nearest_flagged_building)
MH_EXPORT_REPLACE(llm_strat_ai_plan_turret_upgrade, mh::ai::promoted_arm::plan_turret_upgrade)
MH_EXPORT_REPLACE(llm_strat_ai_plan_mine_construction, mh::ai::promoted_arm::plan_mine_construction)
MH_EXPORT_REPLACE(llm_strat_ai_score_build_categories, mh::ai::promoted_arm::score_build_categories)
MH_EXPORT_REPLACE(llm_strat_ai_player_score_tier, mh::ai::promoted_arm::player_score_tier)
MH_EXPORT_REPLACE(llm_strat_ai_react_resource_shortage, mh::ai::promoted_arm::react_resource_shortage)
MH_EXPORT_REPLACE(llm_strat_ai_maintain_unit_housing, mh::ai::promoted_arm::maintain_unit_housing)
MH_EXPORT_REPLACE(llm_strat_ai_plan_construction, mh::ai::promoted_arm::plan_construction)
MH_EXPORT_REPLACE(llm_strat_ai_queue_remove_at, mh::ai::promoted_arm::queue_remove_at)
MH_EXPORT_REPLACE(llm_strat_ai_scan_bldg_repair_upgrade, mh::ai::promoted_arm::scan_bldg_repair_upgrade)
MH_EXPORT_REPLACE(llm_strat_ai_sort_site_candidates_by_dist, mh::ai::promoted_arm::sort_site_candidates_by_dist)
MH_EXPORT_REPLACE(llm_strat_ai_resource_site_meets_threshold, mh::ai::promoted_arm::resource_site_meets_threshold)
MH_EXPORT_REPLACE(llm_strat_ai_bldg_scan_resource_site_candidates, mh::ai::promoted_arm::bldg_scan_resource_site_candidates)
MH_EXPORT_REPLACE(llm_strat_ai_bldg_scan_grid_candidates, mh::ai::promoted_arm::bldg_scan_grid_candidates)
MH_EXPORT_REPLACE(llm_strat_ai_scan_build_site_candidates, mh::ai::promoted_arm::scan_build_site_candidates)
MH_EXPORT_REPLACE(llm_strat_ai_group_split_off_create, mh::ai::promoted_arm::group_split_off_create)
MH_EXPORT_REPLACE(llm_strat_ai_group_split_excess_members, mh::ai::promoted_arm::group_split_excess_members)
MH_EXPORT_REPLACE(llm_strat_ai_group_redistribute_units, mh::ai::promoted_arm::group_redistribute_units)
MH_EXPORT_REPLACE(llm_strat_ai_plan_unit_training, mh::ai::promoted_arm::plan_unit_training)
MH_EXPORT_REPLACE(llm_strat_ai_group_home_guard_replenish, mh::ai::promoted_arm::group_home_guard_replenish)
MH_EXPORT_REPLACE(llm_strat_ai_pick_owned_tile_or_home, mh::ai::promoted_arm::pick_owned_tile_or_home)
MH_EXPORT_REPLACE(llm_strat_ai_group_form_standby_from_pool3, mh::ai::promoted_arm::form_standby_from_pool3)
MH_EXPORT_REPLACE(llm_strat_ai_group_form_surplus_from_pool4, mh::ai::promoted_arm::form_surplus_from_pool4)
MH_EXPORT_REPLACE(llm_strat_ai_army_milestone_advance_or_attack, mh::ai::promoted_arm::army_milestone_advance_or_attack)
MH_EXPORT_REPLACE(llm_strat_ai_group_expansion_form_or_repurpose, mh::ai::promoted_arm::group_expansion_form_or_repurpose)
MH_EXPORT_REPLACE(llm_strat_ai_group_form_patrol, mh::ai::promoted_arm::form_patrol)
MH_EXPORT_REPLACE(llm_strat_ai_resource_spend_rate_update, mh::ai::promoted_arm::resource_spend_rate_update)
MH_EXPORT_REPLACE(llm_strat_ai_bldg_production_type_dispatch, mh::ai::promoted_arm::bldg_production_type_dispatch)
MH_EXPORT_REPLACE(llm_strat_ai_bldg_queue_process_entry, mh::ai::promoted_arm::bldg_queue_process_entry)
MH_EXPORT_REPLACE(llm_strat_ai_bldg_queue_handle_recruit_state, mh::ai::promoted_arm::bldg_queue_handle_recruit_state)
MH_EXPORT_REPLACE(llm_strat_ai_bldg_queue_handle_state2_empty, mh::ai::promoted_arm::bldg_queue_handle_state2_empty)
MH_EXPORT_REPLACE(llm_strat_ai_bldg_queue_handle_upgrade_or_cancel, mh::ai::promoted_arm::bldg_queue_handle_upgrade_or_cancel)
MH_EXPORT_REPLACE(llm_strat_ai_bldg_queue_process, mh::ai::promoted_arm::bldg_queue_process)
MH_EXPORT_REPLACE(llm_strat_ai_recompute_shortage_state, mh::ai::promoted_arm::recompute_shortage_state)
MH_EXPORT_REPLACE(llm_strat_ai_invasion_spawn_reinforcements, mh::ai::promoted_arm::invasion_spawn_reinforcements)
MH_EXPORT_REPLACE(llm_strat_ai_invasion_launch_attack_group, mh::ai::promoted_arm::invasion_launch_attack_group)
MH_EXPORT_REPLACE(llm_strat_ai_player_tick, mh::ai::promoted_arm::player_tick)
MH_EXPORT_REPLACE(llm_strat_ai_group_check_arrival_status, mh::ai::promoted_arm::group_check_arrival_status)
MH_EXPORT_REPLACE(llm_strat_ai_group_task_step, mh::ai::promoted_arm::group_task_step)
MH_EXPORT_REPLACE(llm_strat_ai_group_find_nearest_building_of_types, mh::ai::promoted_arm::group_find_nearest_building_of_types)
MH_EXPORT_REPLACE(llm_strat_ai_group_collect_buildings_of_types, mh::ai::promoted_arm::group_collect_buildings_of_types)
MH_EXPORT_REPLACE(llm_strat_ai_group_task_attack_random_target, mh::ai::promoted_arm::group_task_attack_random_target)
MH_EXPORT_REPLACE(llm_strat_ai_group_task_drain_reserve_attack, mh::ai::promoted_arm::group_task_drain_reserve_attack)
MH_EXPORT_REPLACE(llm_strat_ai_group_task_engage_target, mh::ai::promoted_arm::group_task_engage_target)
MH_EXPORT_REPLACE(llm_strat_ai_group_task_attack_nearest_defended, mh::ai::promoted_arm::group_task_attack_nearest_defended)
MH_EXPORT_REPLACE(llm_strat_ai_group_task_muster_from_pool, mh::ai::promoted_arm::group_task_muster_from_pool)
MH_EXPORT_REPLACE(llm_strat_ai_group_task_recruit_from_pool3, mh::ai::promoted_arm::group_task_recruit_from_pool3)
MH_EXPORT_REPLACE(llm_strat_ai_group_task_recruit_from_pool4, mh::ai::promoted_arm::group_task_recruit_from_pool4)
MH_EXPORT_REPLACE(llm_strat_ai_group_task_recruit_from_storage, mh::ai::promoted_arm::group_task_recruit_from_storage)
MH_EXPORT_REPLACE(llm_strat_ai_group_task_disband, mh::ai::promoted_arm::group_task_disband)
MH_EXPORT_REPLACE(llm_strat_ai_group_task_hold, mh::ai::promoted_arm::group_task_hold)
MH_EXPORT_REPLACE(llm_strat_ai_group_task_recall_home, mh::ai::promoted_arm::group_task_recall_home)
MH_EXPORT_REPLACE(llm_strat_ai_group_task_patrol_shuttle, mh::ai::promoted_arm::group_task_patrol_shuttle)
MH_EXPORT_REPLACE(llm_strat_ai_group_task_loiter_wander, mh::ai::promoted_arm::group_task_loiter_wander)
MH_EXPORT_REPLACE(llm_strat_ai_group_task_rally_formup, mh::ai::promoted_arm::group_task_rally_formup)
MH_EXPORT_REPLACE(llm_strat_ai_group_task_scatter_random, mh::ai::promoted_arm::group_task_scatter_random)
MH_EXPORT_REPLACE(llm_strat_ai_group_task_nudge_stragglers, mh::ai::promoted_arm::group_task_nudge_stragglers)
MH_EXPORT_REPLACE(llm_strat_ai_group_task_wait, mh::ai::promoted_arm::group_task_wait)
MH_EXPORT_REPLACE(llm_strat_ai_group_task_advance_to_anchor, mh::ai::promoted_arm::group_task_advance_to_anchor)
MH_EXPORT_REPLACE(llm_strat_ai_group_task_disperse_passable, mh::ai::promoted_arm::group_task_disperse_passable)
MH_EXPORT_REPLACE(llm_strat_ai_group_task_activate, mh::ai::promoted_arm::group_task_activate)
MH_EXPORT_REPLACE(llm_strat_ai_group_enter_hold, mh::ai::promoted_arm::group_enter_hold)
MH_EXPORT_REPLACE(llm_strat_ai_group1_drain_to_group0, mh::ai::promoted_arm::group1_drain_to_group0)
MH_EXPORT_REPLACE(llm_strat_ai_unit_group_tick, mh::ai::promoted_arm::unit_group_tick)
MH_EXPORT_REPLACE(llm_strat_ai_group_classify_target_object, mh::ai::promoted_arm::group_classify_target_object)
MH_EXPORT_REPLACE(llm_strat_ai_scan_target_list_add, mh::ai::promoted_arm::scan_target_list_add)
MH_EXPORT_REPLACE(llm_strat_ai_holding_pen_scan_targets, mh::ai::promoted_arm::holding_pen_scan_targets)
MH_EXPORT_REPLACE(llm_strat_ai_target_list_invalidate_by_id, mh::ai::promoted_arm::target_list_invalidate_by_id)
MH_EXPORT_REPLACE(llm_strat_ai_target_list_refresh_mothers, mh::ai::promoted_arm::target_list_refresh_mothers)
MH_EXPORT_REPLACE(llm_strat_ai_target_list_scan_visible_enemies, mh::ai::promoted_arm::target_list_scan_visible_enemies)
MH_EXPORT_REPLACE(llm_strat_ai_group_seed_resolved_target, mh::ai::promoted_arm::group_seed_resolved_target)
MH_EXPORT_REPLACE(llm_strat_ai_scan_target_sort_cmp, mh::ai::promoted_arm::scan_target_sort_cmp)
MH_EXPORT_REPLACE(llm_strat_ai_scan_target_list_sort, mh::ai::promoted_arm::scan_target_list_sort)
MH_EXPORT_REPLACE(llm_strat_ai_attack_candidate_add, mh::ai::promoted_arm::attack_candidate_add)
MH_EXPORT_REPLACE(llm_strat_ai_unit_should_abandon_target, mh::ai::promoted_arm::unit_should_abandon_target)
MH_EXPORT_REPLACE(llm_strat_ai_scan_targets_for_engage, mh::ai::promoted_arm::scan_targets_for_engage)
MH_EXPORT_REPLACE(llm_strat_ai_scan_target_list_for_engage_candidates, mh::ai::promoted_arm::scan_target_list_for_engage_candidates)
MH_EXPORT_REPLACE(llm_strat_ai_scan_spiral_ring_for_engage_candidates, mh::ai::promoted_arm::scan_spiral_ring_for_engage_candidates)
MH_EXPORT_REPLACE(llm_strat_ai_unit_scan_engage_candidates_in_range, mh::ai::promoted_arm::unit_scan_engage_candidates_in_range)
MH_EXPORT_REPLACE(llm_strat_ai_engage_select_and_commit, mh::ai::promoted_arm::engage_select_and_commit)
MH_EXPORT_REPLACE(llm_strat_ai_engage_filter_and_commit_target, mh::ai::promoted_arm::engage_filter_and_commit_target)
MH_EXPORT_REPLACE(llm_strat_ai_active_unit_tick, mh::ai::promoted_arm::active_unit_tick)

namespace mh::ai {

namespace {
bool g_any_installed = false;
} // namespace

bool promotion_active() { return g_any_installed; }

// D18/C8-f, same contract as mh::orders::install_promotion and mh::sim's sibling: `default_on` is
// the SHIPPING DEFAULT, PASSED IN by the caller (mh/seams/net_internal.h's SHIP_PROMOTE_* at the one
// call site) rather than read from a header here -- this module must not include a seams header, so
// "what ships" stays answerable from one place instead of two.
//
// [promote_skip] is the per-function red-ladder override this domain's ai_migration.json review
// process assumes exists: a single row can be pulled back to its pre-patch bytes (ini value 1)
// without touching the other 165, e.g. while a fresh divergence on that one row is under
// investigation.
int install_promotion(int default_on) {
    if (default_on == 0) return 0;

    struct entry {
        const char *name;
        bool (*install)();
    };
    static const entry seams[] = {
        {"llm_strat_ai_unit_order_move_with_bump", mh_export_install_llm_strat_ai_unit_order_move_with_bump},
        {"llm_strat_ai_create_reinforcement_unit", mh_export_install_llm_strat_ai_create_reinforcement_unit},
        {"llm_strat_ai_grid_fill_below_threshold", mh_export_install_llm_strat_ai_grid_fill_below_threshold},
        {"llm_strat_ai_grid_flood_step", mh_export_install_llm_strat_ai_grid_flood_step},
        {"llm_strat_ai_grid_stamp_seeds", mh_export_install_llm_strat_ai_grid_stamp_seeds},
        {"llm_strat_ai_grid_match_stencil", mh_export_install_llm_strat_ai_grid_match_stencil},
        {"llm_strat_ai_grid_stencil_all_near_unthreatened", mh_export_install_llm_strat_ai_grid_stencil_all_near_unthreatened},
        {"llm_strat_ai_grid_stamp_threat_ring", mh_export_install_llm_strat_ai_grid_stamp_threat_ring},
        {"llm_strat_ai_grid_clear_threat_bit", mh_export_install_llm_strat_ai_grid_clear_threat_bit},
        {"llm_strat_ai_unit_commit_attack_on_enemy_hq", mh_export_install_llm_strat_ai_unit_commit_attack_on_enemy_hq},
        {"llm_strat_ai_start_hq_attack_scenario", mh_export_install_llm_strat_ai_start_hq_attack_scenario},
        {"llm_strat_ai_score_reinforcement_unit", mh_export_install_llm_strat_ai_score_reinforcement_unit},
        {"llm_strat_ai_bldg_count_by_category", mh_export_install_llm_strat_ai_bldg_count_by_category},
        {"llm_strat_ai_bldg_type_queue_has_pending", mh_export_install_llm_strat_ai_bldg_type_queue_has_pending},
        {"llm_strat_ai_bldg_type_already_queued", mh_export_install_llm_strat_ai_bldg_type_already_queued},
        {"llm_strat_ai_group_has_split_group_link", mh_export_install_llm_strat_ai_group_has_split_group_link},
        {"llm_strat_ai_target_ref_has_engageable_weapon", mh_export_install_llm_strat_ai_target_ref_has_engageable_weapon},
        {"llm_strat_ai_target_ref_is_alive", mh_export_install_llm_strat_ai_target_ref_is_alive},
        {"llm_strat_ai_unit_is_order_pending", mh_export_install_llm_strat_ai_unit_is_order_pending},
        {"llm_strat_ai_group_member_unlink", mh_export_install_llm_strat_ai_group_member_unlink},
        {"llm_strat_ai_group_member_link", mh_export_install_llm_strat_ai_group_member_link},
        {"llm_strat_ai_group_member_move", mh_export_install_llm_strat_ai_group_member_move},
        {"llm_strat_ai_group_task_preempt", mh_export_install_llm_strat_ai_group_task_preempt},
        {"llm_strat_ai_group_task_enqueue", mh_export_install_llm_strat_ai_group_task_enqueue},
        {"llm_strat_ai_group_task_dequeue", mh_export_install_llm_strat_ai_group_task_dequeue},
        {"llm_strat_ai_group_create", mh_export_install_llm_strat_ai_group_create},
        {"llm_strat_ai_group_remove", mh_export_install_llm_strat_ai_group_remove},
        {"llm_strat_ai_engage_candidate_add", mh_export_install_llm_strat_ai_engage_candidate_add},
        {"llm_strat_ai_engage_partition_turret_candidates", mh_export_install_llm_strat_ai_engage_partition_turret_candidates},
        {"llm_strat_ai_engage_sort_candidates_by_dist", mh_export_install_llm_strat_ai_engage_sort_candidates_by_dist},
        {"llm_strat_ai_commit_attack_order", mh_export_install_llm_strat_ai_commit_attack_order},
        {"llm_strat_ai_commit_attack_order_alt", mh_export_install_llm_strat_ai_commit_attack_order_alt},
        {"llm_strat_ai_unit_group_assign_by_type", mh_export_install_llm_strat_ai_unit_group_assign_by_type},
        {"llm_strat_ai_unit_launch_from_storage_enqueue", mh_export_install_llm_strat_ai_unit_launch_from_storage_enqueue},
        {"llm_strat_ai_route_unit_to_home_storage", mh_export_install_llm_strat_ai_route_unit_to_home_storage},
        {"llm_strat_ai_group_compute_centroid", mh_export_install_llm_strat_ai_group_compute_centroid},
        {"llm_strat_ai_group_rally_formup_worker", mh_export_install_llm_strat_ai_group_rally_formup_worker},
        {"llm_strat_ai_random_point_near", mh_export_install_llm_strat_ai_random_point_near},
        {"llm_strat_ai_group_scatter_random_worker", mh_export_install_llm_strat_ai_group_scatter_random_worker},
        {"llm_strat_ai_group_reposition_members", mh_export_install_llm_strat_ai_group_reposition_members},
        {"llm_strat_ai_group_no_member_near_centroid", mh_export_install_llm_strat_ai_group_no_member_near_centroid},
        {"llm_strat_ai_group_area_scan_hostile", mh_export_install_llm_strat_ai_group_area_scan_hostile},
        {"llm_strat_ai_group_all_units_settled", mh_export_install_llm_strat_ai_group_all_units_settled},
        {"llm_strat_ai_group_find_slowest_unit", mh_export_install_llm_strat_ai_group_find_slowest_unit},
        {"llm_strat_ai_group_pick_best_weapon_unit", mh_export_install_llm_strat_ai_group_pick_best_weapon_unit},
        {"llm_strat_ai_target_list_add", mh_export_install_llm_strat_ai_target_list_add},
        {"llm_strat_ai_target_list_remove", mh_export_install_llm_strat_ai_target_list_remove},
        {"llm_strat_ai_unit_weapon_power", mh_export_install_llm_strat_ai_unit_weapon_power},
        {"llm_strat_ai_target_ref_has_ground_weapon", mh_export_install_llm_strat_ai_target_ref_has_ground_weapon},
        {"llm_strat_ai_target_ref_has_aa_weapon", mh_export_install_llm_strat_ai_target_ref_has_aa_weapon},
        {"llm_strat_ai_update_opponent_relations", mh_export_install_llm_strat_ai_update_opponent_relations},
        {"llm_strat_ai_recompute_map_influence", mh_export_install_llm_strat_ai_recompute_map_influence},
        {"llm_strat_ai_target_dist_sq", mh_export_install_llm_strat_ai_target_dist_sq},
        {"llm_strat_ai_notify_map_changed", mh_export_install_llm_strat_ai_notify_map_changed},
        {"llm_strat_ai_notify_map_changed_2", mh_export_install_llm_strat_ai_notify_map_changed_2},
        {"llm_strat_ai_building_defense_weapon_range", mh_export_install_llm_strat_ai_building_defense_weapon_range},
        {"llm_strat_ai_turret_threat_rescan", mh_export_install_llm_strat_ai_turret_threat_rescan},
        {"llm_strat_ai_bldg_has_heli_unit", mh_export_install_llm_strat_ai_bldg_has_heli_unit},
        {"llm_strat_ai_notify_bldg_constructed", mh_export_install_llm_strat_ai_notify_bldg_constructed},
        {"llm_strat_ai_bldg_register_visible_building", mh_export_install_llm_strat_ai_bldg_register_visible_building},
        {"llm_strat_ai_group_member_count_adjust", mh_export_install_llm_strat_ai_group_member_count_adjust},
        {"llm_strat_ai_notify_object_removed", mh_export_install_llm_strat_ai_notify_object_removed},
        {"llm_strat_ai_players_tick", mh_export_install_llm_strat_ai_players_tick},
        {"llm_strat_ai_notify_unit_lifecycle", mh_export_install_llm_strat_ai_notify_unit_lifecycle},
        {"llm_strat_ai_queue_release_order", mh_export_install_llm_strat_ai_queue_release_order},
        {"llm_strat_ai_spiral_table_init", mh_export_install_llm_strat_ai_spiral_table_init},
        {"llm_strat_ai_build_plan_push", mh_export_install_llm_strat_ai_build_plan_push},
        {"llm_strat_ai_init_build_candidate_priorities", mh_export_install_llm_strat_ai_init_build_candidate_priorities},
        {"llm_strat_ai_scr_parse", mh_export_install_llm_strat_ai_scr_parse},
        {"llm_strat_ai_count_unit_build_sources", mh_export_install_llm_strat_ai_count_unit_build_sources},
        {"llm_strat_ai_queue_train_unit", mh_export_install_llm_strat_ai_queue_train_unit},
        {"llm_strat_ai_queue_bldg_repair", mh_export_install_llm_strat_ai_queue_bldg_repair},
        {"llm_strat_ai_queue_bldg_upgrade", mh_export_install_llm_strat_ai_queue_bldg_upgrade},
        {"llm_strat_ai_queue_flush_unit_train_entries", mh_export_install_llm_strat_ai_queue_flush_unit_train_entries},
        {"llm_strat_ai_queue_flush_unit_train_entries_2", mh_export_install_llm_strat_ai_queue_flush_unit_train_entries_2},
        {"llm_strat_ai_queue_rotate_newest_to_front", mh_export_install_llm_strat_ai_queue_rotate_newest_to_front},
        {"llm_strat_ai_queue_reconcile_bldg_change", mh_export_install_llm_strat_ai_queue_reconcile_bldg_change},
        {"llm_strat_ai_calc_mine_yield_estimate", mh_export_install_llm_strat_ai_calc_mine_yield_estimate},
        {"llm_strat_ai_mine_portfolio_rebalance", mh_export_install_llm_strat_ai_mine_portfolio_rebalance},
        {"llm_strat_ai_storage_capacity_short_and_cap_check", mh_export_install_llm_strat_ai_storage_capacity_short_and_cap_check},
        {"llm_strat_ai_calc_power_supply_ratio", mh_export_install_llm_strat_ai_calc_power_supply_ratio},
        {"llm_strat_ai_is_worker_priority_candidate", mh_export_install_llm_strat_ai_is_worker_priority_candidate},
        {"llm_strat_ai_rebalance_building_workers", mh_export_install_llm_strat_ai_rebalance_building_workers},
        {"llm_strat_ai_scan_construction_sites", mh_export_install_llm_strat_ai_scan_construction_sites},
        {"llm_strat_ai_find_nearest_flagged_building", mh_export_install_llm_strat_ai_find_nearest_flagged_building},
        {"llm_strat_ai_plan_turret_upgrade", mh_export_install_llm_strat_ai_plan_turret_upgrade},
        {"llm_strat_ai_plan_mine_construction", mh_export_install_llm_strat_ai_plan_mine_construction},
        {"llm_strat_ai_score_build_categories", mh_export_install_llm_strat_ai_score_build_categories},
        {"llm_strat_ai_player_score_tier", mh_export_install_llm_strat_ai_player_score_tier},
        {"llm_strat_ai_react_resource_shortage", mh_export_install_llm_strat_ai_react_resource_shortage},
        {"llm_strat_ai_maintain_unit_housing", mh_export_install_llm_strat_ai_maintain_unit_housing},
        {"llm_strat_ai_plan_construction", mh_export_install_llm_strat_ai_plan_construction},
        {"llm_strat_ai_queue_remove_at", mh_export_install_llm_strat_ai_queue_remove_at},
        {"llm_strat_ai_scan_bldg_repair_upgrade", mh_export_install_llm_strat_ai_scan_bldg_repair_upgrade},
        {"llm_strat_ai_sort_site_candidates_by_dist", mh_export_install_llm_strat_ai_sort_site_candidates_by_dist},
        {"llm_strat_ai_resource_site_meets_threshold", mh_export_install_llm_strat_ai_resource_site_meets_threshold},
        {"llm_strat_ai_bldg_scan_resource_site_candidates", mh_export_install_llm_strat_ai_bldg_scan_resource_site_candidates},
        {"llm_strat_ai_bldg_scan_grid_candidates", mh_export_install_llm_strat_ai_bldg_scan_grid_candidates},
        {"llm_strat_ai_scan_build_site_candidates", mh_export_install_llm_strat_ai_scan_build_site_candidates},
        {"llm_strat_ai_group_split_off_create", mh_export_install_llm_strat_ai_group_split_off_create},
        {"llm_strat_ai_group_split_excess_members", mh_export_install_llm_strat_ai_group_split_excess_members},
        {"llm_strat_ai_group_redistribute_units", mh_export_install_llm_strat_ai_group_redistribute_units},
        {"llm_strat_ai_plan_unit_training", mh_export_install_llm_strat_ai_plan_unit_training},
        {"llm_strat_ai_group_home_guard_replenish", mh_export_install_llm_strat_ai_group_home_guard_replenish},
        {"llm_strat_ai_pick_owned_tile_or_home", mh_export_install_llm_strat_ai_pick_owned_tile_or_home},
        {"llm_strat_ai_group_form_standby_from_pool3", mh_export_install_llm_strat_ai_group_form_standby_from_pool3},
        {"llm_strat_ai_group_form_surplus_from_pool4", mh_export_install_llm_strat_ai_group_form_surplus_from_pool4},
        {"llm_strat_ai_army_milestone_advance_or_attack", mh_export_install_llm_strat_ai_army_milestone_advance_or_attack},
        {"llm_strat_ai_group_expansion_form_or_repurpose", mh_export_install_llm_strat_ai_group_expansion_form_or_repurpose},
        {"llm_strat_ai_group_form_patrol", mh_export_install_llm_strat_ai_group_form_patrol},
        {"llm_strat_ai_resource_spend_rate_update", mh_export_install_llm_strat_ai_resource_spend_rate_update},
        {"llm_strat_ai_bldg_production_type_dispatch", mh_export_install_llm_strat_ai_bldg_production_type_dispatch},
        {"llm_strat_ai_bldg_queue_process_entry", mh_export_install_llm_strat_ai_bldg_queue_process_entry},
        {"llm_strat_ai_bldg_queue_handle_recruit_state", mh_export_install_llm_strat_ai_bldg_queue_handle_recruit_state},
        {"llm_strat_ai_bldg_queue_handle_state2_empty", mh_export_install_llm_strat_ai_bldg_queue_handle_state2_empty},
        {"llm_strat_ai_bldg_queue_handle_upgrade_or_cancel", mh_export_install_llm_strat_ai_bldg_queue_handle_upgrade_or_cancel},
        {"llm_strat_ai_bldg_queue_process", mh_export_install_llm_strat_ai_bldg_queue_process},
        {"llm_strat_ai_recompute_shortage_state", mh_export_install_llm_strat_ai_recompute_shortage_state},
        {"llm_strat_ai_invasion_spawn_reinforcements", mh_export_install_llm_strat_ai_invasion_spawn_reinforcements},
        {"llm_strat_ai_invasion_launch_attack_group", mh_export_install_llm_strat_ai_invasion_launch_attack_group},
        {"llm_strat_ai_player_tick", mh_export_install_llm_strat_ai_player_tick},
        {"llm_strat_ai_group_check_arrival_status", mh_export_install_llm_strat_ai_group_check_arrival_status},
        {"llm_strat_ai_group_task_step", mh_export_install_llm_strat_ai_group_task_step},
        {"llm_strat_ai_group_find_nearest_building_of_types", mh_export_install_llm_strat_ai_group_find_nearest_building_of_types},
        {"llm_strat_ai_group_collect_buildings_of_types", mh_export_install_llm_strat_ai_group_collect_buildings_of_types},
        {"llm_strat_ai_group_task_attack_random_target", mh_export_install_llm_strat_ai_group_task_attack_random_target},
        {"llm_strat_ai_group_task_drain_reserve_attack", mh_export_install_llm_strat_ai_group_task_drain_reserve_attack},
        {"llm_strat_ai_group_task_engage_target", mh_export_install_llm_strat_ai_group_task_engage_target},
        {"llm_strat_ai_group_task_attack_nearest_defended", mh_export_install_llm_strat_ai_group_task_attack_nearest_defended},
        {"llm_strat_ai_group_task_muster_from_pool", mh_export_install_llm_strat_ai_group_task_muster_from_pool},
        {"llm_strat_ai_group_task_recruit_from_pool3", mh_export_install_llm_strat_ai_group_task_recruit_from_pool3},
        {"llm_strat_ai_group_task_recruit_from_pool4", mh_export_install_llm_strat_ai_group_task_recruit_from_pool4},
        {"llm_strat_ai_group_task_recruit_from_storage", mh_export_install_llm_strat_ai_group_task_recruit_from_storage},
        {"llm_strat_ai_group_task_disband", mh_export_install_llm_strat_ai_group_task_disband},
        {"llm_strat_ai_group_task_hold", mh_export_install_llm_strat_ai_group_task_hold},
        {"llm_strat_ai_group_task_recall_home", mh_export_install_llm_strat_ai_group_task_recall_home},
        {"llm_strat_ai_group_task_patrol_shuttle", mh_export_install_llm_strat_ai_group_task_patrol_shuttle},
        {"llm_strat_ai_group_task_loiter_wander", mh_export_install_llm_strat_ai_group_task_loiter_wander},
        {"llm_strat_ai_group_task_rally_formup", mh_export_install_llm_strat_ai_group_task_rally_formup},
        {"llm_strat_ai_group_task_scatter_random", mh_export_install_llm_strat_ai_group_task_scatter_random},
        {"llm_strat_ai_group_task_nudge_stragglers", mh_export_install_llm_strat_ai_group_task_nudge_stragglers},
        {"llm_strat_ai_group_task_wait", mh_export_install_llm_strat_ai_group_task_wait},
        {"llm_strat_ai_group_task_advance_to_anchor", mh_export_install_llm_strat_ai_group_task_advance_to_anchor},
        {"llm_strat_ai_group_task_disperse_passable", mh_export_install_llm_strat_ai_group_task_disperse_passable},
        {"llm_strat_ai_group_task_activate", mh_export_install_llm_strat_ai_group_task_activate},
        {"llm_strat_ai_group_enter_hold", mh_export_install_llm_strat_ai_group_enter_hold},
        {"llm_strat_ai_group1_drain_to_group0", mh_export_install_llm_strat_ai_group1_drain_to_group0},
        {"llm_strat_ai_unit_group_tick", mh_export_install_llm_strat_ai_unit_group_tick},
        {"llm_strat_ai_group_classify_target_object", mh_export_install_llm_strat_ai_group_classify_target_object},
        {"llm_strat_ai_scan_target_list_add", mh_export_install_llm_strat_ai_scan_target_list_add},
        {"llm_strat_ai_holding_pen_scan_targets", mh_export_install_llm_strat_ai_holding_pen_scan_targets},
        {"llm_strat_ai_target_list_invalidate_by_id", mh_export_install_llm_strat_ai_target_list_invalidate_by_id},
        {"llm_strat_ai_target_list_refresh_mothers", mh_export_install_llm_strat_ai_target_list_refresh_mothers},
        {"llm_strat_ai_target_list_scan_visible_enemies", mh_export_install_llm_strat_ai_target_list_scan_visible_enemies},
        {"llm_strat_ai_group_seed_resolved_target", mh_export_install_llm_strat_ai_group_seed_resolved_target},
        {"llm_strat_ai_scan_target_sort_cmp", mh_export_install_llm_strat_ai_scan_target_sort_cmp},
        {"llm_strat_ai_scan_target_list_sort", mh_export_install_llm_strat_ai_scan_target_list_sort},
        {"llm_strat_ai_attack_candidate_add", mh_export_install_llm_strat_ai_attack_candidate_add},
        {"llm_strat_ai_unit_should_abandon_target", mh_export_install_llm_strat_ai_unit_should_abandon_target},
        {"llm_strat_ai_scan_targets_for_engage", mh_export_install_llm_strat_ai_scan_targets_for_engage},
        {"llm_strat_ai_scan_target_list_for_engage_candidates", mh_export_install_llm_strat_ai_scan_target_list_for_engage_candidates},
        {"llm_strat_ai_scan_spiral_ring_for_engage_candidates", mh_export_install_llm_strat_ai_scan_spiral_ring_for_engage_candidates},
        {"llm_strat_ai_unit_scan_engage_candidates_in_range", mh_export_install_llm_strat_ai_unit_scan_engage_candidates_in_range},
        {"llm_strat_ai_engage_select_and_commit", mh_export_install_llm_strat_ai_engage_select_and_commit},
        {"llm_strat_ai_engage_filter_and_commit_target", mh_export_install_llm_strat_ai_engage_filter_and_commit_target},
        {"llm_strat_ai_active_unit_tick", mh_export_install_llm_strat_ai_active_unit_tick},
    };
    constexpr int total = (int)(sizeof(seams) / sizeof(seams[0]));

    int ok        = 0;
    int attempted = 0;
    for (const entry &e : seams) {
        ++attempted;
        if (e.install()) {
            ++ok;
            ai_say("; [promote] ai: + %s\n", e.name);
        } else {
            // install_export_ok already logged WHY (entry-byte guard mismatch = the DLL was built
            // against a different image). Refusing loudly beats a half-promoted closure -- and this
            // closure is more half-promoted than most: the ai_calls internal-call wiring means a
            // hole here is not just one row missing, it is every OTHER row's calls into this row
            // still hitting the pre-patch bytes underneath our own seams.
            ai_say("; [promote] ai: seam %s REFUSED -- container is NOT fully promoted\n", e.name);
        }
    }

    if (ok != total) {
        ai_say("; [promote] ai: %d/%d seams installed (%d skipped) -- PARTIAL, treat this run as "
               "invalid\n",
               ok, total, total - attempted);
    } else {
        ai_say("; [promote] ai: ALL %d seams installed -- the AI closure is LIVE\n", ok);
    }

    g_any_installed = ok > 0;
    return ok;
}

} // namespace mh::ai
