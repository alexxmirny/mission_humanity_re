//
// sim/libtrans/sim_lt_promote.cpp -- see sim_lt_promote.h.
//
#include "sim/libtrans/sim_lt_promote.h"

#include "addr/mh_export.gen.h" // sig_<name> typedefs, MH_EXPORT_REPLACE, mh_export_install_<name>()

// LIB-REF-IN: the adapters forward through the INBOUND C ENTRY, not into the C++ wrapper. See
// the banner block in sim/sim_hostreach_promote.cpp for the mechanism and the ordering hazard.
// SEVEN of this module's ten covered rows route; the other three are PRINTED EXCEPTIONS that
// gen_libmh_inbound.py re-derives every run -- all three are served by libmh_import_world, a
// REPLAY-SPINE entry LIB-REF implements, so there is nothing here yet to route to.
#include "../../../libmh/include/libmh_host_in.h"
#include "ai/ai_state.h" // ai_say -- the shared trace sink every mh::sim promoted_arm TU logs through

// F4D-PRE: C9(c)/C4 entry_owner_of and the harness's wall-clock-pin question, both through the
// host's hook table. The two per-build arms this replaced (a guarded hook/ + harness include, and a
// hand-written `#else` stub apiece) are now one unguarded include: mh::hosthook answers "unowned"
// and "no pin" when nothing is bound, which is exactly what those stubs answered.
#include "state/hook_api.h"

#include "sim/libtrans/sim_lt_ambient.h"
#include "sim/libtrans/sim_lt_anim_time.h"
#include "sim/libtrans/sim_lt_available_projects_clear.h"
#include "sim/libtrans/sim_lt_available_remove.h"
#include "sim/libtrans/sim_lt_bldg_cell_grid.h"
#include "sim/libtrans/sim_lt_bldg_query.h"
#include "sim/libtrans/sim_lt_cfg_planet.h"
#include "sim/libtrans/sim_lt_deploy_squad.h"
#include "sim/libtrans/sim_lt_diplomacy.h"
#include "sim/libtrans/sim_lt_invasion_alert.h"
#include "sim/libtrans/sim_lt_map_region_pool.h"
#include "sim/libtrans/sim_lt_map_setup_dimensions.h"
#include "sim/libtrans/sim_lt_math.h"
#include "sim/libtrans/sim_lt_menu_teardown.h"
#include "sim/libtrans/sim_lt_placement_offsets.h"
#include "sim/libtrans/sim_lt_prod_transfer.h"
#include "sim/libtrans/sim_lt_progress_finalize.h"
#include "sim/libtrans/sim_lt_progress_planet_sweep.h"
#include "sim/libtrans/sim_lt_progress_unlock_fixpoint.h"
#include "sim/libtrans/sim_lt_rng_draws.h"
#include "sim/libtrans/sim_lt_rng_raw_step.h"
#include "sim/libtrans/sim_lt_rng_seed.h"
#include "sim/libtrans/sim_lt_scan_masked_table.h"
#include "sim/libtrans/sim_lt_str.h"

#include "lockstep/lt_chat_ally_mask.h"
#include "lockstep/lt_peer_timing_reset.h"
#include "lockstep/lt_player_by_side_id.h"
#include "lockstep/lt_reload_snapshot_resync.h"
#include "lockstep/lt_time_query.h"

#include <cstdint>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

namespace mh::sim {

namespace {

// PER-ROW FIRST-CALL LIVENESS (see the header banner). `fired` is a static local owned by the calling
// adapter, so each of the 43 adapters below gets its own one-shot latch. The "(OURS is live)" suffix
// is exact -- tools/test_ui.py greps it -- do not reword it. Named `lt_mark_first_call` (not anonymous
// like sim/resid/resid_promote.cpp's own `mark_first_call`) purely for grep clarity across the two
// sibling files -- internal linkage (this anonymous namespace) already rules out any ODR collision
// between them; logs through mh::ai::ai_say, the shared trace sink every other mh::sim::promoted_arm
// TU in this codebase already uses (see the header banner) -- no module-local say()/set_logger() pair.
void lt_mark_first_call(bool &fired, const char *orig_name) {
    if (fired) return;
    fired = true;
    mh::ai::ai_say("; [promote] lib_trans: %s call #1 (OURS is live)\n", orig_name);
}

} // namespace

// ---- PROMOTION (LIB-TRANS-P): install this domain's 43 verified rows as the LIVE implementation ----
//
// One named adapter per row, matching its `::mh::exp::sig_<orig>` function-pointer type exactly (the
// generated naked entry thunk assigns straight into a `static sig_<orig>` -- a mismatched signature is
// a compile error, not a runtime one). Every one of the 43 wrappers below was already written to match
// its own sig_ typedef exactly (checked row by row against addr/mh_export.gen.h while writing this
// file) -- unlike sim/resid/resid_promote.cpp's exemplar, NONE of the 43 needs a static_cast at this
// boundary. Two rows carry a narrower width/ordering note worth calling out at their own adapter
// rather than a cast: llm_map_region_free's uint32_t<->pointer boundary (Ghidra gap G1, see the header
// banner) and llm_strat_squad_placement_offset_lookup's table_col/table_row-vs-soldier_count/slot
// naming (a straight positional forward, not a swap -- see that adapter).
namespace promoted_arm {

// `mh::sim::promoted_arm` is ALSO used by sim/sim_order_dispatch.cpp and sim/libtrans/sim_lt_frame.cpp
// (their own promoted arms) -- namespaces merge across translation units, so this flag is kept
// internal-linkage (an anonymous sub-namespace, matching resid_promote.cpp's own convention for this
// exact namespace) rather than a bare namespace-scope global, to avoid a second TU ever colliding on
// the name at link time. Named with the `lt_` prefix (matching sim_lt_frame.cpp's own
// `g_lt_frame_installed`) for grep clarity, even though the anonymous namespace alone already rules
// out any collision. Qualified access (`promoted_arm::g_lt_promote_installed`, used below by
// lib_trans_promotion_active()) still resolves within this TU -- the using-directive an anonymous
// namespace injects into its enclosing namespace applies to qualified lookup too.
namespace {
bool g_lt_promote_installed = false;
} // namespace

// clang-format off

// ---- sim_lt_ambient.h -------------------------------------------------------------------------------
void lt_ambient_reseed_planet_event_times(int32_t planet_index, double current_time) {
    static bool fired = false;
    lt_mark_first_call(fired, "llm_snd_ambient_reseed_planet_event_times");
    ::libmh_ambient_reseed(planet_index, current_time);
}

// ---- sim_lt_anim_time.h -----------------------------------------------------------------------------
double lt_cfg_GetAnimTime(int32_t index_) {
    static bool fired = false;
    lt_mark_first_call(fired, "cfg_GetAnimTime");
    return ::mh::sim::cfg_get_anim_time(index_);
}

// ---- sim_lt_available_projects_clear.h ---------------------------------------------------------------
void lt_game_ClearAvailableProjects() {
    static bool fired = false;
    lt_mark_first_call(fired, "game_ClearAvailableProjects");
    ::mh::sim::available_projects_clear();
}

// ---- sim_lt_available_remove.h -----------------------------------------------------------------------
// The two LT1B ADOPTIONS (2026-09-02) -- game_RemoveFromAvailableBuildings/Projects, the Add* family's
// exact Remove mirrors (sim_lt_available_remove.h's own banner).
void lt_game_RemoveFromAvailableBuildings(uint32_t player, int32_t b_i) {
    static bool fired = false;
    lt_mark_first_call(fired, "game_RemoveFromAvailableBuildings");
    ::mh::sim::remove_from_available_buildings(player, b_i);
}

void lt_game_RemoveFromAvailableProjects(uint32_t player, uint32_t p_i) {
    static bool fired = false;
    lt_mark_first_call(fired, "game_RemoveFromAvailableProjects");
    ::mh::sim::remove_from_available_projects(player, p_i);
}

// ---- sim_lt_bldg_cell_grid.h -------------------------------------------------------------------------
void lt_bldg_recompute_cell_grid() {
    static bool fired = false;
    lt_mark_first_call(fired, "llm_strat_bldg_recompute_cell_grid");
    ::mh::sim::bldg_recompute_cell_grid();
}

// ---- sim_lt_bldg_query.h -----------------------------------------------------------------------------
int32_t lt_bldg_first_occupied_unit_slot_has_soldiers(int32_t building_index) {
    static bool fired = false;
    lt_mark_first_call(fired, "llm_bldg_first_occupied_unit_slot_has_soldiers");
    return ::libmh_bldg_slot_has_soldiers(building_index);
}

// ---- sim_lt_cfg_planet.h -----------------------------------------------------------------------------
void lt_cfg_final_planet_Construct(int32_t define_index, int32_t invention_index, char *map_name,
                                   mh::game::mh_cfg_pre_struct_Planet *planet_data, char *path_unc) {
    static bool fired = false;
    lt_mark_first_call(fired, "cfg_final_planet_Construct");
    ::mh::sim::cfg_final_planet_Construct(define_index, invention_index, map_name, planet_data,
                                          path_unc);
}

// ---- sim_lt_deploy_squad.h ---------------------------------------------------------------------------
int32_t lt_deploy_starting_squad() {
    static bool fired = false;
    lt_mark_first_call(fired, "llm_strat_deploy_starting_squad");
    return ::libmh_deploy_starting_squad();
}

// ---- sim_lt_diplomacy.h ------------------------------------------------------------------------------
void lt_diplomacy_init_multiplayer() {
    static bool fired = false;
    lt_mark_first_call(fired, "llm_diplomacy_init_multiplayer");
    ::mh::sim::diplomacy_init_multiplayer();
}

void lt_diplomacy_init_skirmish() {
    static bool fired = false;
    lt_mark_first_call(fired, "llm_diplomacy_init_skirmish");
    ::mh::sim::diplomacy_init_skirmish();
}

// ---- sim_lt_invasion_alert.h -------------------------------------------------------------------------
void lt_invasion_alert_clear(int32_t planet) {
    static bool fired = false;
    lt_mark_first_call(fired, "llm_strat_invasion_alert_clear");
    ::mh::sim::invasion_alert_clear(planet);
}

int32_t lt_invasion_alert_poll(double now) {
    static bool fired = false;
    lt_mark_first_call(fired, "llm_strat_invasion_alert_poll");
    return ::mh::sim::invasion_alert_poll(now);
}

llm_map_region *lt_map_block_8_GetNextBlock() {
    static bool fired = false;
    lt_mark_first_call(fired, "map_block_8_GetNextBlock");
    return ::mh::sim::get_next_block();
}

void lt_map_region_free(uint32_t param_1) {
    static bool fired = false;
    lt_mark_first_call(fired, "llm_map_region_free");
    // Ghidra gap G1 (sim_lt_map_region_pool.h): the committed export parameter is uint32_t, NOT
    // llm_map_region* -- the pointer<->uint32_t cast happens INSIDE mh::sim::region_free, at that
    // boundary, not here. Forwarded untouched; matches sig_llm_map_region_free exactly.
    ::mh::sim::region_free(param_1);
}

void lt_map_region_pool_reset() {
    static bool fired = false;
    lt_mark_first_call(fired, "llm_map_region_pool_reset");
    ::mh::sim::pool_reset();
}

// ---- sim_lt_map_setup_dimensions.h -------------------------------------------------------------------
void lt_map_setup_dimensions() {
    static bool fired = false;
    lt_mark_first_call(fired, "llm_map_setup_dimensions");
    ::mh::sim::map_setup_dimensions();
}

// ---- sim_lt_math.h -----------------------------------------------------------------------------------
double lt_math_scale_pct(double value, int32_t pct) {
    static bool fired = false;
    lt_mark_first_call(fired, "llm_math_scale_pct");
    return ::mh::sim::scale_pct(value, pct);
}

int32_t lt_math_manhattan_dist(int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    static bool fired = false;
    lt_mark_first_call(fired, "llm_math_manhattan_dist");
    return ::mh::sim::manhattan_dist(x0, y0, x1, y1);
}

// ---- sim_lt_menu_teardown.h --------------------------------------------------------------------------
void lt_menu_force_return_to_main() {
    static bool fired = false;
    lt_mark_first_call(fired, "llm_menu_force_return_to_main");
    ::mh::sim::menu_force_return_to_main();
}

// ---- sim_lt_placement_offsets.h ----------------------------------------------------------------------
void lt_facing_step_apply(char *out_x, char *out_y, int32_t cursor_state_index) {
    static bool fired = false;
    lt_mark_first_call(fired, "llm_strat_facing_step_apply");
    ::mh::sim::facing_step_apply(out_x, out_y, cursor_state_index);
}

void lt_squad_placement_offset_lookup(int32_t table_col, int32_t table_row, char *out_a, char *out_b) {
    static bool fired = false;
    lt_mark_first_call(fired, "llm_strat_squad_placement_offset_lookup");
    // sig_llm_strat_squad_placement_offset_lookup's (table_col, table_row) IS positionally
    // (soldier_count, slot): sim_lt_placement_offsets.h derives table_col==soldier_count,
    // table_row==slot. A straight positional forward, not a swap.
    ::mh::sim::squad_placement_offset_lookup(table_col, table_row, out_a, out_b);
}

// ---- sim_lt_prod_transfer.h --------------------------------------------------------------------------
double lt_prod_transfer_progress(int32_t slot) {
    static bool fired = false;
    lt_mark_first_call(fired, "llm_strat_prod_transfer_progress");
    return ::libmh_prod_transfer_progress(slot);
}

// ---- sim_lt_progress_finalize.h ----------------------------------------------------------------------
void lt_progress_finalize_acquire(uint16_t player, uint16_t pid) {
    static bool fired = false;
    lt_mark_first_call(fired, "llm_progress_finalize_acquire");
    ::mh::sim::progress_finalize_acquire(player, pid);
}

// ---- sim_lt_progress_planet_sweep.h ------------------------------------------------------------------
void lt_progress_recheck_planet_system_all_players() {
    static bool fired = false;
    lt_mark_first_call(fired, "llm_progress_recheck_planet_system_all_players");
    ::libmh_progress_recheck_all();
}

void lt_game_UpdatePlanetProgress() {
    static bool fired = false;
    lt_mark_first_call(fired, "game_UpdatePlanetProgress");
    ::mh::sim::game_update_planet_progress();
}

// ---- sim_lt_progress_unlock_fixpoint.h -----------------------------------------------------------------
void lt_progress_propagate_unlocks(uint16_t player) {
    static bool fired = false;
    lt_mark_first_call(fired, "llm_progress_propagate_unlocks");
    ::mh::sim::progress_propagate_unlocks(player);
}

void lt_progress_recheck_projects(uint32_t player) {
    static bool fired = false;
    lt_mark_first_call(fired, "llm_progress_recheck_projects");
    ::mh::sim::progress_recheck_projects(player);
}

void lt_progress_recheck_buildings(uint32_t player) {
    static bool fired = false;
    lt_mark_first_call(fired, "llm_progress_recheck_buildings");
    ::mh::sim::progress_recheck_buildings(player);
}

void lt_progress_collect_available_projects(uint32_t player) {
    static bool fired = false;
    lt_mark_first_call(fired, "llm_progress_collect_available_projects");
    ::mh::sim::progress_collect_available_projects(player);
}

// ---- sim_lt_rng_draws.h ------------------------------------------------------------------------------
int32_t lt_rand_below(int32_t upper_bound) {
    static bool fired = false;
    lt_mark_first_call(fired, "llm_rand_below");
    return ::mh::sim::rand_below(upper_bound);
}

int32_t lt_rand_below_fx(uint32_t upper_bound) {
    static bool fired = false;
    lt_mark_first_call(fired, "llm_rand_below_fx");
    return ::mh::sim::rand_below_fx(upper_bound);
}

// ---- sim_lt_rng_raw_step.h ---------------------------------------------------------------------------
int32_t lt_rand_below_ai(uint32_t range) {
    static bool fired = false;
    lt_mark_first_call(fired, "llm_rand_below_ai");
    return ::mh::sim::rand_below_ai(range);
}

double lt_rand_state_advance(int32_t rng_index) {
    static bool fired = false;
    lt_mark_first_call(fired, "llm_rand_state_advance");
    return ::mh::sim::rand_state_advance(rng_index);
}

// ---- sim_lt_rng_seed.h -------------------------------------------------------------------------------
void lt_rng_seed_ch0(uint32_t seed_value) {
    static bool fired = false;
    lt_mark_first_call(fired, "llm_strat_rng_seed_ch0");
    ::mh::sim::rng_seed_ch0(seed_value);
}

void lt_rng_seed_ch1(uint32_t seed_value) {
    static bool fired = false;
    lt_mark_first_call(fired, "llm_strat_rng_seed_ch1");
    ::mh::sim::rng_seed_ch1(seed_value);
}

// ---- sim_lt_scan_masked_table.h ----------------------------------------------------------------------
int32_t lt_scan_masked_table_for_empty_cell(uint8_t *grid, int32_t grid_width, int32_t grid_height,
                                            uint8_t *footprint_mask, int32_t span_x, int32_t span_y,
                                            int32_t start_x, int32_t start_y) {
    static bool fired = false;
    lt_mark_first_call(fired, "llm_scan_masked_table_for_empty_cell");
    return ::mh::sim::scan_masked_table_for_empty_cell(grid, grid_width, grid_height, footprint_mask,
                                                       span_x, span_y, start_x, start_y);
}

// ---- sim_lt_str.h ------------------------------------------------------------------------------------
void lt_str_char_subst(char *str, uint8_t mode, char c1, char c2) {
    static bool fired = false;
    lt_mark_first_call(fired, "llm_str_char_subst");
    ::mh::sim::str_char_subst(str, mode, c1, c2);
}

// ---- lockstep/lt_chat_ally_mask.h ----------------------------------------------------------------------
void lt_net_chat_ally_mask_rebuild() {
    static bool fired = false;
    lt_mark_first_call(fired, "llm_net_chat_ally_mask_rebuild");
    ::mh::lockstep::chat_ally_mask_rebuild();
}

// ---- lockstep/lt_peer_timing_reset.h ---------------------------------------------------------------------
void lt_net_lockstep_peer_timing_reset() {
    static bool fired = false;
    lt_mark_first_call(fired, "llm_net_lockstep_peer_timing_reset");
    ::mh::lockstep::peer_timing_reset();
}

// ---- lockstep/lt_player_by_side_id.h ---------------------------------------------------------------------
int32_t lt_strat_player_by_side_id(int32_t side_id) {
    static bool fired = false;
    lt_mark_first_call(fired, "llm_strat_player_by_side_id");
    return ::libmh_player_index_by_side(side_id);
}

// ---- lockstep/lt_reload_snapshot_resync.h ------------------------------------------------------------------
void lt_game_reload_snapshot_resync_clocks(double now) {
    static bool fired = false;
    lt_mark_first_call(fired, "llm_game_reload_snapshot_resync_clocks");
    ::libmh_clock_reload_resync(now);
}

// ---- lockstep/lt_time_query.h ------------------------------------------------------------------------------
// EXPECTED REFUSAL under [harness] pin_wallclock=1 -- see this file's own header banner. The pin
// whole-body-replaces this entry BEFORE MH_Seam_Init runs, so this row's install() call below returns
// false in every harnessed/determinism run, with the pin named as the entry's owner -- that is the
// deterministic replacement working as intended, not a build problem.
double lt_time_GetCurrentTime() {
    static bool fired = false;
    lt_mark_first_call(fired, "time_GetCurrentTime");
    return ::mh::lockstep::time_GetCurrentTime();
}

// clang-format on

} // namespace promoted_arm

} // namespace mh::sim

// The 43 verified lib_trans seams this file owns (tools/data/lib_trans_migration.json, "state":
// "verified", MINUS the frame pair -- see the header banner; that pair has its own installer in
// sim/libtrans/sim_lt_frame.cpp). MH_EXPORT_REPLACE expands at file scope (its generated
// `mh_export_install_*` is a TU-local `static` function, so it cannot live inside a namespace block)
// -- same placement as mh::sim::resid_promote.cpp's own P0-EXPORT-shaped block.
MH_EXPORT_REPLACE(llm_snd_ambient_reseed_planet_event_times, mh::sim::promoted_arm::lt_ambient_reseed_planet_event_times)
MH_EXPORT_REPLACE(cfg_GetAnimTime, mh::sim::promoted_arm::lt_cfg_GetAnimTime)
MH_EXPORT_REPLACE(game_ClearAvailableProjects, mh::sim::promoted_arm::lt_game_ClearAvailableProjects)
MH_EXPORT_REPLACE(game_RemoveFromAvailableBuildings, mh::sim::promoted_arm::lt_game_RemoveFromAvailableBuildings)
MH_EXPORT_REPLACE(game_RemoveFromAvailableProjects, mh::sim::promoted_arm::lt_game_RemoveFromAvailableProjects)
MH_EXPORT_REPLACE(llm_strat_bldg_recompute_cell_grid, mh::sim::promoted_arm::lt_bldg_recompute_cell_grid)
MH_EXPORT_REPLACE(llm_bldg_first_occupied_unit_slot_has_soldiers, mh::sim::promoted_arm::lt_bldg_first_occupied_unit_slot_has_soldiers)
MH_EXPORT_REPLACE(cfg_final_planet_Construct, mh::sim::promoted_arm::lt_cfg_final_planet_Construct)
MH_EXPORT_REPLACE(llm_strat_deploy_starting_squad, mh::sim::promoted_arm::lt_deploy_starting_squad)
MH_EXPORT_REPLACE(llm_diplomacy_init_multiplayer, mh::sim::promoted_arm::lt_diplomacy_init_multiplayer)
MH_EXPORT_REPLACE(llm_diplomacy_init_skirmish, mh::sim::promoted_arm::lt_diplomacy_init_skirmish)
MH_EXPORT_REPLACE(llm_strat_invasion_alert_clear, mh::sim::promoted_arm::lt_invasion_alert_clear)
MH_EXPORT_REPLACE(llm_strat_invasion_alert_poll, mh::sim::promoted_arm::lt_invasion_alert_poll)
MH_EXPORT_REPLACE(map_block_8_GetNextBlock, mh::sim::promoted_arm::lt_map_block_8_GetNextBlock)
MH_EXPORT_REPLACE(llm_map_region_free, mh::sim::promoted_arm::lt_map_region_free)
MH_EXPORT_REPLACE(llm_map_region_pool_reset, mh::sim::promoted_arm::lt_map_region_pool_reset)
MH_EXPORT_REPLACE(llm_map_setup_dimensions, mh::sim::promoted_arm::lt_map_setup_dimensions)
MH_EXPORT_REPLACE(llm_math_scale_pct, mh::sim::promoted_arm::lt_math_scale_pct)
MH_EXPORT_REPLACE(llm_math_manhattan_dist, mh::sim::promoted_arm::lt_math_manhattan_dist)
MH_EXPORT_REPLACE(llm_menu_force_return_to_main, mh::sim::promoted_arm::lt_menu_force_return_to_main)
MH_EXPORT_REPLACE(llm_strat_facing_step_apply, mh::sim::promoted_arm::lt_facing_step_apply)
MH_EXPORT_REPLACE(llm_strat_squad_placement_offset_lookup, mh::sim::promoted_arm::lt_squad_placement_offset_lookup)
MH_EXPORT_REPLACE(llm_strat_prod_transfer_progress, mh::sim::promoted_arm::lt_prod_transfer_progress)
MH_EXPORT_REPLACE(llm_progress_finalize_acquire, mh::sim::promoted_arm::lt_progress_finalize_acquire)
MH_EXPORT_REPLACE(llm_progress_recheck_planet_system_all_players, mh::sim::promoted_arm::lt_progress_recheck_planet_system_all_players)
MH_EXPORT_REPLACE(game_UpdatePlanetProgress, mh::sim::promoted_arm::lt_game_UpdatePlanetProgress)
MH_EXPORT_REPLACE(llm_progress_propagate_unlocks, mh::sim::promoted_arm::lt_progress_propagate_unlocks)
MH_EXPORT_REPLACE(llm_progress_recheck_projects, mh::sim::promoted_arm::lt_progress_recheck_projects)
MH_EXPORT_REPLACE(llm_progress_recheck_buildings, mh::sim::promoted_arm::lt_progress_recheck_buildings)
MH_EXPORT_REPLACE(llm_progress_collect_available_projects, mh::sim::promoted_arm::lt_progress_collect_available_projects)
MH_EXPORT_REPLACE(llm_rand_below, mh::sim::promoted_arm::lt_rand_below)
MH_EXPORT_REPLACE(llm_rand_below_fx, mh::sim::promoted_arm::lt_rand_below_fx)
MH_EXPORT_REPLACE(llm_rand_below_ai, mh::sim::promoted_arm::lt_rand_below_ai)
MH_EXPORT_REPLACE(llm_rand_state_advance, mh::sim::promoted_arm::lt_rand_state_advance)
MH_EXPORT_REPLACE(llm_strat_rng_seed_ch0, mh::sim::promoted_arm::lt_rng_seed_ch0)
MH_EXPORT_REPLACE(llm_strat_rng_seed_ch1, mh::sim::promoted_arm::lt_rng_seed_ch1)
MH_EXPORT_REPLACE(llm_scan_masked_table_for_empty_cell, mh::sim::promoted_arm::lt_scan_masked_table_for_empty_cell)
MH_EXPORT_REPLACE(llm_str_char_subst, mh::sim::promoted_arm::lt_str_char_subst)
MH_EXPORT_REPLACE(llm_net_chat_ally_mask_rebuild, mh::sim::promoted_arm::lt_net_chat_ally_mask_rebuild)
MH_EXPORT_REPLACE(llm_net_lockstep_peer_timing_reset, mh::sim::promoted_arm::lt_net_lockstep_peer_timing_reset)
MH_EXPORT_REPLACE(llm_strat_player_by_side_id, mh::sim::promoted_arm::lt_strat_player_by_side_id)
MH_EXPORT_REPLACE(llm_game_reload_snapshot_resync_clocks, mh::sim::promoted_arm::lt_game_reload_snapshot_resync_clocks)
MH_EXPORT_REPLACE(time_GetCurrentTime, mh::sim::promoted_arm::lt_time_GetCurrentTime)

namespace mh::sim {

bool lib_trans_promotion_active() { return promoted_arm::g_lt_promote_installed; }

// C8-f-shaped: `default_on` is the SHIPPING DEFAULT for `[promote] lib_trans`, PASSED IN by the seams
// layer (mh/seams/reimpl_probe.cpp, SHIP_PROMOTE_LIB_TRANS) rather than read from a header here --
// this module must not include a seams header (the layering lint keeps binary-bound seam code out of
// the reimplementation), so the one call site is where "what ships" is answerable, same discipline as
// mh::sim::install_promotion_resid / mh::orders::install_promotion.
int install_promotion_lib_trans(int default_on) {
#ifdef MH_LIBMH_BUILD
    // LIB-REF-SPLIT: PROMOTION IS INJECTION, so the whole installer is hosted. Its `seams[]` table
    // pairs each callee's ORIGINAL ENTRY ADDRESS with the generated installer, and standalone every
    // one of those installers already refuses (addr/mh_export.gen.h's standalone arm) -- but the
    // table is built on the stack before any of them is called, so the addresses reached the object
    // anyway: 76 distinct original VAs across the three installers that still carry one, measured
    // 2026-09-11 after the macro-level arms had taken everything else.
    //
    // 0, not a partial attempt: the existing contract is "0 = not promoted, treat this run as
    // unpromoted", and a standalone build is exactly a run where nothing is promoted because there
    // is nothing to promote OVER. The ini read below would also be meaningless -- there is no ini.
    (void)default_on;
    return 0;
#else
    if (default_on == 0) return 0;

    struct entry {
        const char *name;
        uintptr_t   addr;
        bool (*install)();
    };
    // clang-format off
    const entry seams[] = {
        {"llm_snd_ambient_reseed_planet_event_times",     mh::exp::addr_llm_snd_ambient_reseed_planet_event_times,     mh_export_install_llm_snd_ambient_reseed_planet_event_times},
        {"cfg_GetAnimTime",                               mh::exp::addr_cfg_GetAnimTime,                               mh_export_install_cfg_GetAnimTime},
        {"game_ClearAvailableProjects",                   mh::exp::addr_game_ClearAvailableProjects,                   mh_export_install_game_ClearAvailableProjects},
        {"game_RemoveFromAvailableBuildings",              mh::exp::addr_game_RemoveFromAvailableBuildings,             mh_export_install_game_RemoveFromAvailableBuildings},
        {"game_RemoveFromAvailableProjects",               mh::exp::addr_game_RemoveFromAvailableProjects,              mh_export_install_game_RemoveFromAvailableProjects},
        {"llm_strat_bldg_recompute_cell_grid",             mh::exp::addr_llm_strat_bldg_recompute_cell_grid,            mh_export_install_llm_strat_bldg_recompute_cell_grid},
        {"llm_bldg_first_occupied_unit_slot_has_soldiers", mh::exp::addr_llm_bldg_first_occupied_unit_slot_has_soldiers, mh_export_install_llm_bldg_first_occupied_unit_slot_has_soldiers},
        {"cfg_final_planet_Construct",                     mh::exp::addr_cfg_final_planet_Construct,                    mh_export_install_cfg_final_planet_Construct},
        {"llm_strat_deploy_starting_squad",                mh::exp::addr_llm_strat_deploy_starting_squad,               mh_export_install_llm_strat_deploy_starting_squad},
        {"llm_diplomacy_init_multiplayer",                 mh::exp::addr_llm_diplomacy_init_multiplayer,                mh_export_install_llm_diplomacy_init_multiplayer},
        {"llm_diplomacy_init_skirmish",                    mh::exp::addr_llm_diplomacy_init_skirmish,                   mh_export_install_llm_diplomacy_init_skirmish},
        {"llm_strat_invasion_alert_clear",                 mh::exp::addr_llm_strat_invasion_alert_clear,                mh_export_install_llm_strat_invasion_alert_clear},
        {"llm_strat_invasion_alert_poll",                  mh::exp::addr_llm_strat_invasion_alert_poll,                 mh_export_install_llm_strat_invasion_alert_poll},
        {"map_block_8_GetNextBlock",                       mh::exp::addr_map_block_8_GetNextBlock,                      mh_export_install_map_block_8_GetNextBlock},
        {"llm_map_region_free",                            mh::exp::addr_llm_map_region_free,                           mh_export_install_llm_map_region_free},
        {"llm_map_region_pool_reset",                      mh::exp::addr_llm_map_region_pool_reset,                     mh_export_install_llm_map_region_pool_reset},
        {"llm_map_setup_dimensions",                       mh::exp::addr_llm_map_setup_dimensions,                      mh_export_install_llm_map_setup_dimensions},
        {"llm_math_scale_pct",                             mh::exp::addr_llm_math_scale_pct,                            mh_export_install_llm_math_scale_pct},
        {"llm_math_manhattan_dist",                        mh::exp::addr_llm_math_manhattan_dist,                       mh_export_install_llm_math_manhattan_dist},
        {"llm_menu_force_return_to_main",                  mh::exp::addr_llm_menu_force_return_to_main,                 mh_export_install_llm_menu_force_return_to_main},
        {"llm_strat_facing_step_apply",                    mh::exp::addr_llm_strat_facing_step_apply,                   mh_export_install_llm_strat_facing_step_apply},
        {"llm_strat_squad_placement_offset_lookup",        mh::exp::addr_llm_strat_squad_placement_offset_lookup,       mh_export_install_llm_strat_squad_placement_offset_lookup},
        {"llm_strat_prod_transfer_progress",               mh::exp::addr_llm_strat_prod_transfer_progress,              mh_export_install_llm_strat_prod_transfer_progress},
        {"llm_progress_finalize_acquire",                  mh::exp::addr_llm_progress_finalize_acquire,                 mh_export_install_llm_progress_finalize_acquire},
        {"llm_progress_recheck_planet_system_all_players", mh::exp::addr_llm_progress_recheck_planet_system_all_players, mh_export_install_llm_progress_recheck_planet_system_all_players},
        {"game_UpdatePlanetProgress",                      mh::exp::addr_game_UpdatePlanetProgress,                     mh_export_install_game_UpdatePlanetProgress},
        {"llm_progress_propagate_unlocks",                 mh::exp::addr_llm_progress_propagate_unlocks,                mh_export_install_llm_progress_propagate_unlocks},
        {"llm_progress_recheck_projects",                  mh::exp::addr_llm_progress_recheck_projects,                 mh_export_install_llm_progress_recheck_projects},
        {"llm_progress_recheck_buildings",                 mh::exp::addr_llm_progress_recheck_buildings,                mh_export_install_llm_progress_recheck_buildings},
        {"llm_progress_collect_available_projects",        mh::exp::addr_llm_progress_collect_available_projects,       mh_export_install_llm_progress_collect_available_projects},
        {"llm_rand_below",                                 mh::exp::addr_llm_rand_below,                                mh_export_install_llm_rand_below},
        {"llm_rand_below_fx",                              mh::exp::addr_llm_rand_below_fx,                             mh_export_install_llm_rand_below_fx},
        {"llm_rand_below_ai",                              mh::exp::addr_llm_rand_below_ai,                             mh_export_install_llm_rand_below_ai},
        {"llm_rand_state_advance",                         mh::exp::addr_llm_rand_state_advance,                        mh_export_install_llm_rand_state_advance},
        {"llm_strat_rng_seed_ch0",                         mh::exp::addr_llm_strat_rng_seed_ch0,                        mh_export_install_llm_strat_rng_seed_ch0},
        {"llm_strat_rng_seed_ch1",                         mh::exp::addr_llm_strat_rng_seed_ch1,                        mh_export_install_llm_strat_rng_seed_ch1},
        {"llm_scan_masked_table_for_empty_cell",           mh::exp::addr_llm_scan_masked_table_for_empty_cell,          mh_export_install_llm_scan_masked_table_for_empty_cell},
        {"llm_str_char_subst",                             mh::exp::addr_llm_str_char_subst,                            mh_export_install_llm_str_char_subst},
        {"llm_net_chat_ally_mask_rebuild",                 mh::exp::addr_llm_net_chat_ally_mask_rebuild,                mh_export_install_llm_net_chat_ally_mask_rebuild},
        {"llm_net_lockstep_peer_timing_reset",             mh::exp::addr_llm_net_lockstep_peer_timing_reset,            mh_export_install_llm_net_lockstep_peer_timing_reset},
        {"llm_strat_player_by_side_id",                    mh::exp::addr_llm_strat_player_by_side_id,                   mh_export_install_llm_strat_player_by_side_id},
        {"llm_game_reload_snapshot_resync_clocks",         mh::exp::addr_llm_game_reload_snapshot_resync_clocks,        mh_export_install_llm_game_reload_snapshot_resync_clocks},
        {"time_GetCurrentTime",                            mh::exp::addr_time_GetCurrentTime,                           mh_export_install_time_GetCurrentTime},
    };
    // clang-format on

    int attempted = 0;
    int ok        = 0;
    for (const entry &e : seams) {
        // THE WALLCLOCK-PIN YIELD (measured 2026-09-02, first SP-oracle run of this installer):
        // the banner's "expected refusal" model was BACKWARDS -- the pin arms AFTER MH_Seam_Init's
        // promotions (LateArm side), so OUR install won the entry and the pin reported NOT ARMED,
        // which P0-SPDET correctly failed as "time_tick read the REAL clock". Instruments own
        // entries (the C4/C6 doctrine); the promotion side yields when the instrument declares it
        // will need this one. Non-harness runs still promote it -- the query answers 0 there.
        if (e.addr == mh::exp::addr_time_GetCurrentTime && mh::hosthook::harness_wants_wallclock_pin()) {
            mh::ai::ai_say("; [promote] lib_trans: time_GetCurrentTime YIELDED to [harness] "
                           "pin_wallclock -- the pin IS the deterministic replacement for this row; "
                           "ours promotes only in unharnessed runs\n");
            continue;
        }
        ++attempted;
        if (e.install()) {
            ++ok;
            // The done_when's "promoted set named + size reported" evidence -- one line per
            // installed row, not just a count.
            mh::ai::ai_say("; [promote] lib_trans: + %s\n", e.name);
        } else {
            // C4/C9(c): distinguish "another detour already owns this entry" (name it -- the fix is a
            // rebind, not a rebuild) from "the entry bytes don't match this build" (wrong image) --
            // same discipline sim_order_dispatch.cpp's install_promotion_dispatch already uses.
            // time_GetCurrentTime is the row EXPECTED to take this branch under
            // [harness] pin_wallclock=1 (this file's header banner) -- a PARTIAL count caused only by
            // that row is healthy, not a regression.
            if (const char *owner = mh::hosthook::entry_owner_of(e.addr))
                mh::ai::ai_say("; [promote] lib_trans: seam %s REFUSED -- the entry is held by %s. "
                               "NOT necessarily a build problem: an owned entry means another detour "
                               "got there first (rebind it, or accept this row stays original)\n",
                               e.name, owner);
            else
                mh::ai::ai_say(
                    "; [promote] lib_trans: seam %s REFUSED -- entry guard mismatch, NOT promoted\n",
                    e.name);
        }
    }
    if (ok != attempted) {
        mh::ai::ai_say(
            "; [promote] lib_trans: %d/%d seams installed -- PARTIAL, treat this run as invalid "
            "(time_GetCurrentTime under pin_wallclock is a YIELD before counting, never a miss "
            "here -- see the header banner)\n",
            ok, attempted);
    } else {
        mh::ai::ai_say("; [promote] lib_trans: ALL %d seams installed -- the lib_trans domain is "
                       "LIVE\n",
                       ok);
    }
    promoted_arm::g_lt_promote_installed = ok > 0;
    return ok;
#endif // MH_LIBMH_BUILD
}

} // namespace mh::sim
