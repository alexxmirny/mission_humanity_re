//
// ai/ai_state.cpp -- binding the AI state view to the live game (RI-AI / AI0).
//
// Two bindings and nothing else: addresses come from the region registry, outward calls come from
// the generated thunks. The logic lives in ai_engage.cpp / ai_build.cpp and never sees either.
//
#include "ai/ai_state.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring> // memcpy/memset -- the AI1-P island move below

#include "state/host_bind.h"    // SB-HOSTFREE: host_relocated() -- the island/host-move interlock
#include "state/region_owner.h" // ST4 claim/region_provider + state_sink -- the island L1 provider

#include "addr/mh_calls.gen.h"
#include "ai/ai_promote.h"                  // AI1-P: promotion_active(), consulted by install_shadow
#include "ai/ai_scan_target_sort_cmp.h"     // LIB-CRT: the standalone qsort comparator
#include "ai/ai_spiral_table_init.h"        // LIB-CRT: the standalone qsort comparator
#include "sim/sim_group_scratch_centroid.h" // its shadow site moved out of the AI tree 2026-08-07
#include "state/host_api.h"
#include "state/host_events.h"
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "fp/st0_call.h"        // CRT-X87: the ST0-argument naked thunk, hoisted out of this body
#include "crt/crt_math.h"       // LIB-REF-SPLIT: the vendored ST0 sin/cos, standalone arm
#include "state/rebind_targets.gen.h"
#include "crt/crt_select.h"        // LIB-CRT: MH_CRT() picks the vendored CRT in the standalone build
#include "state/promoted_select.h" // LIB-REF-SPLIT: MH_PROMOTED

namespace mh::ai {

using namespace mh::state;

ai_state state() {
    ai_state s{};
    // ST2/Law 3: live_base, not the constexpr .bss address. Re-resolved per call -- see ai_state.h.
    s.read.players   = ptr<const player_data>(RID_PLAYER_DATA);
    s.read.units     = ptr<const unit>(RID_UNITS);
    s.read.buildings = ptr<const building>(RID_BUILDINGS);
    // SB-BIND T2: derived row capacities, same per-call posture as the pointers above. The view and
    // the store MUST agree -- they index the same rosters.
    s.read.caps                 = live_roster_caps();
    s.read.engage_scratch       = ptr<const engage_candidate>(RID_STRAT_AI_ENGAGE_CANDIDATE_SCRATCH);
    s.read.engage_scratch_count = ptr<const int32_t>(RID_STRAT_AI_ENGAGE_CANDIDATE_SCRATCH_COUNT);
    s.read.cfg_buildings        = ptr<const cfg_building>(RID_BUILDING);
    s.read.cfg_weapons          = ptr<const cfg_weapon>(RID_WEAPON);
    s.read.cfg_units            = ptr<const cfg_unit>(RID_UNIT);
    s.read.map_width            = ptr<const int32_t>(RID_WIDTH);
    s.read.map_height           = ptr<const int32_t>(RID_HEIGHT);
    s.read.active_player_count  = ptr<const int32_t>(RID_STRAT_AI_ACTIVE_PLAYER_COUNT);
    // ---- batch C layer 1 ----
    s.read.strat_players = ptr<const player_profile>(RID_STRAT_PLAYERS);
    // ---- batch A layer 2 ----
    s.read.tile_objects            = ptr<const tile_object>(RID_TILE_OBJECTS);
    s.read.fog_visible_by_count    = ptr<const uint8_t>(RID_FOG_OF_WAR);
    s.read.map_width_mask          = ptr<const uint32_t>(RID_WIDTH_M);
    s.read.map_height_mask         = ptr<const uint32_t>(RID_HEIGHT_M);
    s.read.spiral_offsets          = ptr<const spiral_offset>(RID_STRAT_AI_TILE_SPIRAL_OFFSETS);
    s.read.spiral_ring_cell_counts = ptr<const uint32_t>(RID_STRAT_AI_TILE_SPIRAL_RING_CELL_COUNTS);
    s.read.grid_wrap_mask          = ptr<const uint32_t>(RID_STRAT_AI_GRID_WRAP_MASK);
    s.read.attack_candidates       = ptr<const attack_candidate>(RID_STRAT_AI_ATTACK_CANDIDATES);
    s.read.attack_candidate_count  = ptr<const int32_t>(RID_STRAT_AI_ATTACK_CANDIDATE_COUNT);
    s.read.attack_strength_table   = ptr<const int32_t>(RID_STRAT_AI_ATTACK_STRENGTH_TABLE);
    s.read.attack_time_table       = ptr<const float>(RID_STRAT_AI_ATTACK_TIME_TABLE);
    s.read.attack_milestone_count  = ptr<const int32_t>(RID_STRAT_AI_ATTACK_MILESTONE_COUNT);
    s.read.repair_ratio            = ptr<const float>(RID_STRAT_AI_REPAIR_RATIO);
    s.read.ai_strategy_period      = ptr<const float>(RID_STRAT_AI_STRATEGY_PERIOD);
    s.read.ai_tactic_period        = ptr<const float>(RID_STRAT_AI_TACTIC_PERIOD);
    s.read.ai_move_period          = ptr<const float>(RID_STRAT_AI_MOVE_PERIOD);
    // ---- batch C layer 2 ----
    s.read.patrol_group_size       = ptr<const int32_t>(RID_STRAT_AI_PATROL_GROUP_SIZE);
    s.read.patrol_group_max        = ptr<const int32_t>(RID_STRAT_AI_PATROL_GROUP_MAX);
    s.read.patrol_wander_bounces   = ptr<const int32_t>(RID_STRAT_AI_PATROL_WANDER_BOUNCES);
    s.read.surplus_group_min_pool4 = ptr<const int32_t>(RID_STRAT_AI_SURPLUS_GROUP_MIN_POOL4);
    s.read.home_guard_target_pct   = ptr<const int32_t>(RID_STRAT_AI_HOME_GUARD_TARGET_PCT);
    s.read.player_side             = ptr<const uint16_t>(RID_PLAYERSIDE);

    s.own.players                = ptr<player_data>(RID_PLAYER_DATA);
    s.own.engage_scratch         = ptr<engage_candidate>(RID_STRAT_AI_ENGAGE_CANDIDATE_SCRATCH);
    s.own.engage_scratch_count   = ptr<int32_t>(RID_STRAT_AI_ENGAGE_CANDIDATE_SCRATCH_COUNT);
    s.own.attack_candidates      = ptr<attack_candidate>(RID_STRAT_AI_ATTACK_CANDIDATES);
    s.own.attack_candidate_count = ptr<int32_t>(RID_STRAT_AI_ATTACK_CANDIDATE_COUNT);
    // ---- batch A layer 3 ----
    s.read.scan_targets              = ptr<const scan_target_entry>(RID_STRAT_AI_SCAN_TARGETS);
    s.read.scan_target_count         = ptr<const int32_t>(RID_STRAT_AI_SCAN_TARGET_COUNT);
    s.read.expand_site_x             = ptr<const uint8_t>(RID_STRAT_AI_EXPAND_SITE_X);
    s.read.expand_site_y             = ptr<const uint8_t>(RID_STRAT_AI_EXPAND_SITE_Y);
    s.read.expand_site_count         = ptr<const int32_t>(RID_STRAT_AI_EXPAND_SITE_COUNT);
    s.read.passable                  = ptr<const uint8_t>(RID_PASSABLE);
    s.read.expand_min_ticks          = ptr<const int32_t>(RID_STRAT_AI_EXPAND_MIN_TICKS);
    s.read.expand_min_building_count = ptr<const int32_t>(RID_STRAT_AI_EXPAND_MIN_BUILDING_COUNT);
    s.read.relay_to_mine_ratio_pct   = ptr<const int32_t>(RID_STRAT_AI_RELAY_TO_MINE_RATIO_PCT);
    s.read.quadrant_dx2              = ptr<const int32_t>(RID_STRAT_AI_QUADRANT_DX2);
    s.read.quadrant_dy2              = ptr<const int32_t>(RID_STRAT_AI_QUADRANT_DY2);

    // ---- batch A layer 4 ----
    s.read.productions  = ptr<const production>(RID_PRODUCTIONS);
    s.read.cfg_unit_sec = ptr<const cfg_unit_section>(RID_UNIT_00E5F638);

    s.own.grid_wrap_mask    = ptr<uint32_t>(RID_STRAT_AI_GRID_WRAP_MASK);
    s.own.scan_targets      = ptr<scan_target_entry>(RID_STRAT_AI_SCAN_TARGETS);
    s.own.scan_target_count = ptr<int32_t>(RID_STRAT_AI_SCAN_TARGET_COUNT);
    s.own.expand_site_x     = ptr<uint8_t>(RID_STRAT_AI_EXPAND_SITE_X);
    s.own.expand_site_y     = ptr<uint8_t>(RID_STRAT_AI_EXPAND_SITE_Y);
    s.own.expand_site_count = ptr<int32_t>(RID_STRAT_AI_EXPAND_SITE_COUNT);

    // ---- batch A layer 5 ----
    s.read.site_candidates      = ptr<const site_candidate>(RID_STRAT_AI_SITE_CANDIDATES);
    s.read.site_candidate_count = ptr<const int32_t>(RID_STRAT_AI_SITE_CANDIDATE_COUNT);
    s.own.site_candidates       = ptr<site_candidate>(RID_STRAT_AI_SITE_CANDIDATES);
    s.own.site_candidate_count  = ptr<int32_t>(RID_STRAT_AI_SITE_CANDIDATE_COUNT);

    // ---- batch B layer 0 ----
    s.read.foreign_bldg_change_flag = ptr<const int32_t>(RID_STRAT_AI_FOREIGN_BLDG_CHANGE_FLAG);

    // ---- batch B layer 2 ----
    s.read.spend_weights = ptr<const int32_t>(RID_STRAT_AI_RESOURCE_SPEND_WEIGHTS);
    // RID_PROGRESS_00E162E4 is the CFG Invention table (Ghidra label `Progress`); RID_PROGRESS is
    // the per-player acquisition state (label `progress`). The ids differ only by the address the
    // registry appended to break the case-insensitive name collision, so bind them adjacently and
    // never one from memory.
    s.read.cfg_inventions   = ptr<const cfg_invention>(RID_PROGRESS_00E162E4);
    s.read.progress         = ptr<const player_progress>(RID_PROGRESS);
    s.read.cfg_building_sec = ptr<const cfg_building_section>(RID_BUILDING_00E5C9E4);
    s.read.cfg_progress_sec = ptr<const cfg_progress_section>(RID_PROGRESS_00E5C0D8);

    // ---- batch B layer 3 ----
    s.read.unit_housing             = ptr<const housing_stats>(RID_STRAT_UNIT_HOUSING_STATS);
    s.read.train_queue_per_unit_cap = ptr<const uint32_t>(RID_STRAT_AI_TRAIN_QUEUE_PER_UNIT_CAP);
    s.read.promo_add                = ptr<const int32_t>(RID_STRAT_AI_CFG_PROMO_ADD);
    s.read.promo_sub                = ptr<const int32_t>(RID_STRAT_AI_CFG_PROMO_SUB);

    // ---- batch B layer 3, the storage/mine planners ----
    s.read.storage                  = ptr<const storage_stats>(RID_STRAT_STORAGE_STATS);
    s.read.player_resources         = ptr<const int32_t>(RID_PLAYER_RESOURCES);
    s.read.silo_ratio               = ptr<const float>(RID_STRAT_AI_SILO_RATIO);
    s.read.mine_rebalance_min_count = ptr<const int32_t>(RID_STRAT_AI_MINE_REBALANCE_MIN_COUNT);
    s.own.mine_rebalance_min_count  = ptr<int32_t>(RID_STRAT_AI_MINE_REBALANCE_MIN_COUNT);

    // ---- batch B layer 3, the turret / worker planners ----
    s.read.pop                     = ptr<const pop_stats>(RID_STRAT_POP_STATS);
    s.read.unemployed_min          = ptr<const int32_t>(RID_STRAT_AI_UNEMPLOYED_MIN);
    s.read.unemployed_ratio        = ptr<const float>(RID_STRAT_AI_UNEMPLOYED_RATIO);
    s.read.max_fuck_ratio          = ptr<const float>(RID_STRAT_AI_MAX_FUCK_RATIO);
    s.read.extra_space             = ptr<const float>(RID_STRAT_AI_EXTRA_SPACE);
    s.read.bldg_connectivity_base  = ptr<const uint8_t>(RID_STRAT_AI_BLDG_CONNECTIVITY_BASE);
    s.read.bldg_connectivity_trial = ptr<const uint8_t>(RID_STRAT_AI_BLDG_CONNECTIVITY_TRIAL);
    s.own.bldg_connectivity_base   = ptr<uint8_t>(RID_STRAT_AI_BLDG_CONNECTIVITY_BASE);
    s.own.bldg_connectivity_trial  = ptr<uint8_t>(RID_STRAT_AI_BLDG_CONNECTIVITY_TRIAL);

    // ---- batch B layers 4 and 6, the mine-worth gate ----
    s.read.resources              = ptr<const map_resources>(RID_RESOURCES);
    s.read.mine_worth             = ptr<const int32_t>(RID_STRAT_AI_CFG_MINE_WORTH);
    s.read.resource_value_weights = ptr<const int32_t>(RID_STRAT_AI_RESOURCE_VALUE_WEIGHTS);

    // ---- batch B layer 4, the mine portfolio rebalance ----
    s.read.mine_roster_index   = ptr<const int32_t>(RID_STRAT_AI_MINE_ROSTER_INDEX);
    s.read.mine_quality_hist   = ptr<const mine_quality>(RID_STRAT_AI_MINE_QUALITY_HIST);
    s.read.mine_yield_estimate = ptr<const mine_yield>(RID_STRAT_AI_MINE_YIELD_ESTIMATE);
    s.read.mine_yield_kernel   = ptr<const mine_kernel_cell>(RID_STRAT_AI_MINE_YIELD_KERNEL);
    s.read.mine_low_share_pct_threshold =
        ptr<const int32_t>(RID_STRAT_AI_MINE_LOW_SHARE_PCT_THRESHOLD);
    s.own.mine_roster_index   = ptr<int32_t>(RID_STRAT_AI_MINE_ROSTER_INDEX);
    s.own.mine_quality_hist   = ptr<mine_quality>(RID_STRAT_AI_MINE_QUALITY_HIST);
    s.own.mine_yield_estimate = ptr<mine_yield>(RID_STRAT_AI_MINE_YIELD_ESTIMATE);

    // ---- RI-AI batch C layer 3 (2026-08-06) ----
    s.read.unit_storage               = ptr<const unit_storage_slot>(RID_UNIT_STORAGE);
    s.read.reposition_small_group_max = ptr<const int32_t>(RID_STRAT_AI_REPOSITION_SMALL_GROUP_MAX);
    s.read.spread_tiles               = ptr<const spread_tile>(RID_STRAT_AI_SPREAD_TILES);
    s.read.spread_tile_count          = ptr<const int32_t>(RID_STRAT_AI_SPREAD_TILE_COUNT);
    s.read.building_candidate_scratch_list =
        ptr<const int32_t>(RID_STRAT_AI_BUILDING_CANDIDATE_SCRATCH_LIST);
    s.read.building_candidate_scratch_count =
        ptr<const int32_t>(RID_STRAT_AI_BUILDING_CANDIDATE_SCRATCH_COUNT);

    s.own.group_relocation_scratch_list  = ptr<int32_t>(RID_STRAT_AI_GROUP_UNIT_SCRATCH_LIST);
    s.own.group_relocation_scratch_count = ptr<int32_t>(RID_STRAT_AI_GROUP_UNIT_SCRATCH_COUNT);
    s.own.spread_tiles                   = ptr<spread_tile>(RID_STRAT_AI_SPREAD_TILES);
    s.own.spread_tile_count              = ptr<int32_t>(RID_STRAT_AI_SPREAD_TILE_COUNT);
    s.own.building_candidate_scratch_count =
        ptr<int32_t>(RID_STRAT_AI_BUILDING_CANDIDATE_SCRATCH_COUNT);

    // ---- RI-AI batch C layer 3 (2026-08-06) ----
    s.read.loiter_angle_scale   = ptr<const double>(RID_STRAT_AI_LOITER_ANGLE_SCALE);
    s.read.loiter_jitter_radius = ptr<const int32_t>(RID_STRAT_AI_LOITER_JITTER_RADIUS);

    // ---- RI-AI batch C layer 4 (2026-08-06) ----
    s.read.turrets = ptr<const turret>(RID_TURRETS);
    s.own.building_candidate_scratch_list =
        ptr<int32_t>(RID_STRAT_AI_BUILDING_CANDIDATE_SCRATCH_LIST);

    // ---- RI-AI batch B layer 2 (2026-08-06) ----
    s.read.energy_ratio        = ptr<const float>(RID_STRAT_AI_ENERGY_RATIO);
    s.read.attack_overkill_pct = ptr<const int32_t>(RID_STRAT_AI_ATTACK_OVERKILL_PCT);
    s.read.min_attack_str      = ptr<const int32_t>(RID_STRAT_AI_MIN_ATTACK_STRENGTH);

    // ---- RI-AI batch E (2026-08-07) ----
    s.read.ai_scr_keyword_table = ptr<const int32_t>(RID_STRAT_AI_SCR_KEYWORD_TABLE);

    s.own.spiral_offsets            = ptr<spiral_offset>(RID_STRAT_AI_TILE_SPIRAL_OFFSETS);
    s.own.spiral_ring_cell_counts   = ptr<uint32_t>(RID_STRAT_AI_TILE_SPIRAL_RING_CELL_COUNTS);
    s.own.ai_tile_spiral_cell_count = ptr<int32_t>(RID_STRAT_AI_TILE_SPIRAL_CELL_COUNT);

    s.own.ai_enabled                       = ptr<int32_t>(RID_STRAT_AI_ENABLED);
    s.own.ai_attack_hq_unit_id             = ptr<uint32_t>(RID_STRAT_AI_ATTACK_HQ_UNIT_ID);
    s.own.tutorial_hq_attack_scenario_done = ptr<bool>(RID_TUTORIAL_HQ_ATTACK_SCENARIO_DONE);

    // ---- RI-AI batch D / AI1D (2026-08-28) ----
    // The roster window resolves the SAME two RIDs the read view does, through mh::state::ptr, so a
    // rebase moves both arms together (the ST1 hazard is a binding that does not come from the
    // registry, not a second registry binding -- see sim_store::order_at's note).
    s.own.roster.bind(ptr<unit>(RID_UNITS), ptr<building>(RID_BUILDINGS));
    s.own.map_width_mask  = ptr<uint32_t>(RID_WIDTH_M);
    s.own.map_height_mask = ptr<uint32_t>(RID_HEIGHT_M);

    return s;
}

namespace detail {

// RI-AI batch C layer 3 (2026-08-06). llm_math_fsin_reduce_loop/llm_math_cos_impl take
// their argument IMPLICITLY on ST0 in the original -- Ghidra's watcomcpp cspec models a float/double
// RETURN via ST0 (fixed 2026-07-07) but has no concept of an ST0 PARAMETER, so no committed
// prototype and no mh_calls.gen.h shape can express "call this with the argument already on the FPU
// stack". This is the ONLY ST0-argument call site in the whole committed call surface (checked
// tools/data/dll_call_protos.json: 0 other functions use ST0 as a param storage), so a hand-written
// naked thunk here is the right-sized fix -- extending gen_dll_calls.py's shape-derivation engine
// for a population of one would be the wrong direction per the repo's own script policy (extend a
// persistent engine for a RECURRING shape; this one is not recurring).
//
// Shape modelled directly on mh::call::detail::s_f64_S4/S8 (mh_calls.gen.cpp) -- same prologue/
// epilogue, same ESP-discipline-agnostic restore via `lea esp, [ebp-12]` -- except the argument is
// FLD'd onto the FPU stack instead of written to an outgoing frame slot. The callee's own return is
// already ST0, which the naked thunk's __cdecl double return leaves untouched, exactly as
// mh::call::detail::s_f64 does for the zero-argument case.
// The thunk itself now lives in fp/st0_call.h (CRT-X87: no inline asm in a translated body).
// Its two callers stay here, because the VAs they name are this domain's, not the helper's.

// LIB-REF-SPLIT: hosted keeps the literal-VA thunk -- unchanged, so the shipping path is the same
// machine code it has always been -- and standalone calls the vendored transcription in
// crt/crt_math.h. Same shape and same reason as crt/crt_select.h's MH_CRT, spelled out here because
// these two sites are the only ones and the hosted arm is a raw address rather than an mh::call::
// name, so MH_CRT itself does not fit.
#ifdef MH_LIBMH_BUILD
double math_fsin_reduce_loop(double angle) { return ::mh::crt::llm_math_fsin_reduce_loop(angle); }
double math_cos_impl(double angle) { return ::mh::crt::llm_math_cos_impl(angle); }
#else
double math_fsin_reduce_loop(double angle) { return ::mh::fp::call_f64_st0_arg(0x004dabc6u, angle); }
double math_cos_impl(double angle) { return ::mh::fp::call_f64_st0_arg(0x004dabbcu, angle); }
#endif

} // namespace detail

const ai_calls &live_calls() {
    static const ai_calls c = {
        MH_LIBMH_BIND(llm_strat_ai_engage_candidate_add),
        MH_LIBMH_BIND(llm_strat_unit_has_ground_weapon),
        MH_LIBMH_BIND(llm_strat_unit_has_aa_weapon),
        MH_LIBMH_BIND(llm_strat_ai_target_ref_is_alive),
        MH_LIBMH_BIND(llm_strat_ai_engage_sort_candidates_by_dist),
        MH_LIBMH_BIND(llm_strat_ai_engage_partition_turret_candidates),
        MH_LIBMH_BIND(llm_strat_ai_commit_attack_order),
        MH_LIBMH_BIND(llm_strat_ai_commit_attack_order_alt),
        MH_LIBMH_BIND(llm_strat_unit_order_attack_building_enqueue),
        MH_LIBMH_BIND(llm_strat_unit_order_attack_unit_enqueue),
        MH_LIBMH_BIND(llm_strat_bldg_find_by_ai_build_and_type),
        MH_LIBMH_BIND(llm_strat_ai_grid_clear_threat_bit),
        MH_LIBMH_BIND(llm_strat_ai_grid_stamp_threat_ring),
        MH_LIBMH_BIND(llm_strat_bldg_is_alive),
        MH_LIBMH_BIND(llm_strat_unit_get_ai_group_index),
        MH_LIBMH_BIND(llm_strat_unit_get_sight),
        MH_LIBMH_BIND(llm_strat_unit_max_weapon_range),
        MH_LIBMH_BIND(llm_strat_ai_scan_spiral_ring_for_engage_candidates),
        MH_LIBMH_BIND(llm_strat_toroidal_dist_sq),
        // ---- batch A layer 2 ----
        MH_LIBMH_BIND(llm_strat_unit_is_idle_or_parked),
        MH_LIBMH_BIND(llm_strat_unit_attack_target_is_dead),
        MH_LIBMH_BIND(llm_strat_ai_target_ref_has_engageable_weapon),
        MH_LIBMH_BIND(llm_strat_ai_target_dist_sq),
        MH_LIBMH_BIND(llm_strat_ai_unit_squad_firepower_value),
        MH_LIBMH_BIND(llm_strat_ai_scan_target_list_add),
        MH_LIBMH_BIND(llm_strat_ai_group_create),
        MH_LIBMH_BIND(llm_strat_ai_group_member_move),
        MH_LIBMH_BIND(llm_strat_ai_group_task_enqueue),
        MH_LIBMH_BIND(llm_strat_ai_group_task_preempt),
        MH_LIBMH_BIND(llm_strat_ai_unit_launch_from_storage_enqueue),
        MH_LIBMH_BIND(llm_strat_ai_pick_owned_tile_or_home),
        MH_LIBMH_BIND(llm_strat_ai_queue_bldg_repair),
        MH_LIBMH_BIND(llm_strat_ai_queue_bldg_upgrade),
        MH_LIBMH_BIND(llm_strat_ai_queue_rotate_newest_to_front),
        MH_LIBMH_BIND(llm_strat_ai_queue_remove_at),
        &MH_CRT(qsort),
        MH_CRT_CMP(llm_strat_ai_scan_target_sort_cmp, &::mh::ai::detail::scan_target_sort_cmp),
        // ---- batch A layer 3 ----
        MH_LIBMH_BIND(llm_strat_ai_group_classify_target_object),
        MH_LIBMH_BIND(llm_strat_ai_target_ref_has_ground_weapon),
        MH_LIBMH_BIND(llm_strat_ai_target_ref_has_aa_weapon),
        MH_LIBMH_BIND(llm_rand_below_ai),
        MH_LIBMH_BIND(llm_scan_masked_table_for_empty_cell),
        MH_LIBMH_BIND(llm_strat_ai_bldg_type_queue_has_pending),
        MH_LIBMH_BIND(llm_strat_bldg_queue_construction),
        MH_LIBMH_BIND(llm_strat_ai_notify_map_changed),
        // ---- batch A layer 4 ----
        MH_LIBMH_BIND(llm_strat_bldg_has_aa_weapon),
        // ---- batch A layer 5 ----
        MH_LIBMH_BIND(llm_strat_ai_grid_match_stencil),
        MH_LIBMH_BIND(llm_strat_ai_grid_stencil_all_near_unthreatened),
        MH_LIBMH_BIND(llm_strat_ai_resource_site_meets_threshold),
        MH_LIBMH_BIND(llm_strat_bldg_check_placement_encloses_neighbors),
        // ---- batch B layer 0 ----
        MH_LIBMH_BIND(llm_strat_unit_is_aircraft),
        MH_LIBMH_BIND(llm_strat_ai_target_list_add),
        // ---- batch B layer 1 ----
        MH_LIBMH_BIND(llm_strat_ai_grid_fill_below_threshold),
        MH_LIBMH_BIND(llm_strat_ai_grid_flood_step),
        MH_LIBMH_BIND(llm_strat_ai_queue_flush_unit_train_entries_2),
        MH_LIBMH_BIND(llm_strat_bldg_total_resource_cost),
        // ---- batch B layer 2 (the rest of it) ----
        MH_LIBMH_BIND(llm_strat_ai_count_unit_build_sources),
        MH_LIBMH_BIND(llm_strat_ai_queue_train_unit),
        MH_LIBMH_BIND(llm_strat_order_grant_resource_raw),
        MH_LIBMH_BIND(llm_strat_ai_bldg_queue_handle_recruit_state),
        MH_LIBMH_BIND(llm_strat_ai_bldg_queue_process_entry),
        MH_LIBMH_BIND(llm_strat_ai_bldg_queue_handle_state2_empty),
        MH_LIBMH_BIND(llm_strat_ai_bldg_queue_handle_upgrade_or_cancel),
        // ---- batch B layer 3 (the dispatch arms' own callees) ----
        MH_LIBMH_BIND(llm_strat_bldg_find_idle_producer_for_unit),
        MH_LIBMH_BIND(llm_strat_order_recruit_unit_enqueue),
        MH_LIBMH_BIND(llm_strat_bldg_order_production_add_enqueue),
        MH_LIBMH_BIND(llm_strat_econ_track_unit_resource_spend),
        MH_LIBMH_BIND(llm_strat_ai_bldg_production_type_dispatch),
        MH_LIBMH_BIND(llm_strat_order_queue_construction_enqueue),
        MH_LIBMH_BIND(llm_strat_bldg_instant_construct_find_slot_enqueue),
        MH_LIBMH_BIND(llm_strat_bldg_record_resource_expenditure_stats),
        MH_LIBMH_BIND(llm_strat_order_population_delta_enqueue),
        MH_LIBMH_BIND(llm_strat_ai_notify_map_changed_2),
        MH_LIBMH_BIND(llm_strat_bldg_order_upgrade_enqueue),
        MH_LIBMH_BIND(llm_strat_bldg_order_repair_cycle_start_enqueue),
        // ---- batch B layer 3, the build-category / housing planners' callees ----
        MH_LIBMH_BIND(llm_strat_bldg_count_by_id),
        MH_LIBMH_BIND(llm_strat_bldg_count_by_type),
        MH_LIBMH_BIND(llm_strat_ai_bldg_count_by_category),
        MH_LIBMH_BIND(llm_strat_ai_bldg_type_already_queued),
        MH_LIBMH_BIND(llm_strat_ai_bldg_has_heli_unit),
        MH_LIBMH_BIND(llm_strat_bldg_side_has_aircraft_producer),
        MH_LIBMH_BIND(llm_strat_ai_mine_portfolio_rebalance),
        // ---- batch B layer 3, the turret / worker planners' callees ----
        MH_LIBMH_BIND(llm_strat_bldg_connectivity_flood_fill),
        MH_LIBMH_BIND(llm_strat_ai_find_nearest_flagged_building),
        MH_LIBMH_BIND(llm_strat_tile_midpoint_wrapped),
        MH_LIBMH_BIND(llm_strat_bldg_order_restart_construction_enqueue),
        MH_LIBMH_BIND(llm_strat_bldg_uses_workers),
        MH_LIBMH_BIND(llm_strat_ai_is_worker_priority_candidate),
        MH_LIBMH_BIND(llm_strat_bldg_order_activate_enqueue),
        MH_LIBMH_BIND(llm_strat_bldg_order_assign_workers_enqueue),
        MH_LIBMH_BIND(llm_strat_bldg_order_unassign_workers_enqueue),
        // ---- batch B layer 4, the site-scanner dispatch ----
        MH_LIBMH_BIND(llm_strat_ai_bldg_scan_resource_site_candidates),
        MH_LIBMH_BIND(llm_strat_ai_scan_build_site_candidates),
        MH_LIBMH_BIND(llm_strat_ai_bldg_scan_grid_candidates),
        MH_LIBMH_BIND(llm_strat_ai_sort_site_candidates_by_dist),

        // ---- batch B layer 4, the storage-launch adapter ----
        MH_LIBMH_BIND(llm_strat_unit_order_auto_launch_from_storage_enqueue),

        // ---- batch B layer 4/5, the mine portfolio rebalance ----
        MH_LIBMH_BIND(llm_strat_ai_calc_mine_yield_estimate),
        MH_LIBMH_BIND(llm_strat_ai_queue_flush_unit_train_entries),

        // ---- batch C layer 0, the object-removed notification hook ----
        MH_LIBMH_BIND(llm_strat_ai_grid_stamp_seeds),
        MH_LIBMH_BIND(llm_strat_ai_target_list_remove),
        MH_LIBMH_BIND(llm_strat_ai_queue_reconcile_bldg_change),
        MH_LIBMH_BIND(llm_strat_ai_group_member_unlink),
        MH_LIBMH_BIND(llm_strat_ai_group_member_link),
        MH_LIBMH_BIND(llm_strat_ai_group_remove),
        // ---- batch C layer 1, the unit-group task machine ----
        MH_LIBMH_BIND(llm_strat_ai_group_task_activate),
        MH_LIBMH_BIND(llm_strat_ai_group_task_step),
        MH_LIBMH_BIND(llm_strat_ai_group_task_rally_formup),
        MH_LIBMH_BIND(llm_strat_ai_group_task_advance_to_anchor),
        MH_LIBMH_BIND(llm_strat_ai_group_task_disperse_passable),
        MH_LIBMH_BIND(llm_strat_ai_group_task_patrol_shuttle),
        MH_LIBMH_BIND(llm_strat_ai_group_task_nudge_stragglers),
        MH_LIBMH_BIND(llm_strat_ai_group_task_wait),
        MH_LIBMH_BIND(llm_strat_ai_group_task_recall_home),
        MH_LIBMH_BIND(llm_strat_ai_group_task_recruit_from_storage),
        MH_LIBMH_BIND(llm_strat_ai_group_task_hold),
        MH_LIBMH_BIND(llm_strat_ai_group_task_scatter_random),
        MH_LIBMH_BIND(llm_strat_ai_group_task_loiter_wander),
        MH_LIBMH_BIND(llm_strat_ai_group_task_muster_from_pool),
        MH_LIBMH_BIND(llm_strat_ai_group_task_recruit_from_pool3),
        MH_LIBMH_BIND(llm_strat_ai_group_task_recruit_from_pool4),
        MH_LIBMH_BIND(llm_strat_ai_group_task_disband),
        MH_LIBMH_BIND(llm_strat_ai_group_task_attack_nearest_defended),
        MH_LIBMH_BIND(llm_strat_ai_group_task_engage_target),
        MH_LIBMH_BIND(llm_strat_ai_group_task_drain_reserve_attack),
        MH_LIBMH_BIND(llm_strat_ai_group_task_attack_random_target),
        MH_LIBMH_BIND(llm_strat_ai_group_task_dequeue),
        MH_LIBMH_BIND(llm_strat_dock_slot_is_busy),
        MH_LIBMH_BIND(llm_strat_ai_group_check_arrival_status),
        &MH_PROMOTED_ROW(llm_strat_ai_group_no_member_near_centroid), // renamed 2026-08-06 from
                                                                      // ..._check_unit_near_centroid
        MH_LIBMH_BIND(llm_strat_ai_group_area_scan_hostile),
        MH_LIBMH_BIND(llm_strat_ai_group_all_units_settled),

        // ---- llm_strat_ai_group_home_guard_replenish's three own callees ----
        MH_LIBMH_BIND(llm_strat_ai_group1_drain_to_group0),
        MH_LIBMH_BIND(llm_strat_ai_route_unit_to_home_storage),
        MH_LIBMH_BIND(llm_strat_ai_group_reposition_members),

        // ---- batch C layer 2, the 2026-08-05 slice ----
        MH_LIBMH_BIND(llm_strat_ai_group_split_off_create),
        MH_LIBMH_BIND(llm_strat_ai_group_has_split_group_link),
        MH_LIBMH_BIND(llm_strat_ai_group_compute_centroid),
        MH_LIBMH_BIND(llm_strat_ai_unit_flag_and_move),
        MH_LIBMH_BIND(llm_strat_ai_group_split_excess_members),
        MH_LIBMH_BIND(llm_strat_bldg_find_mother_position_indexed),
        MH_LIBMH_BIND(llm_strat_order_scratch_reset),
        MH_LIBMH_BIND(llm_strat_order_scratch_set_field),
        MH_LIBMH_BIND(llm_strat_order_enqueue),
        MH_LIBMH_BIND(llm_strat_unit_notify_status),
        MH_LIBMH_BIND(llm_bldg_calc_placement_corner_from_center),
        MH_LIBMH_BIND(llm_bldg_placement_check_and_preview),
        mh::state::evt::snd_play,

        // ---- RI-AI batch C layer 3 (2026-08-06) ----
        MH_LIBMH_BIND(map_unit_IsAiSoldier),
        MH_LIBMH_BIND(map_unit_IsAiGround),
        MH_LIBMH_BIND(map_unit_IsAiPlane),
        MH_LIBMH_BIND(map_unit_IsAiHeli),
        MH_LIBMH_BIND(llm_strat_unit_order_state_is_settled),
        MH_LIBMH_BIND(llm_strat_ai_unit_is_order_pending),
        MH_LIBMH_BIND(llm_strat_unit_state_is_in_transit),
        MH_LIBMH_BIND(llm_strat_unit_state_is_in_storage_transit),
        MH_LIBMH_BIND(llm_strat_unit_order_exit_storage_enqueue),
        MH_LIBMH_BIND(llm_strat_ai_group_scatter_to_passable_tile),
        MH_LIBMH_BIND(llm_strat_ai_group_find_nearest_building_of_types),
        MH_LIBMH_BIND(llm_strat_ai_group_collect_buildings_of_types),

        // ---- RI-AI batch C layer 3 (2026-08-06) ----
        MH_LIBMH_BIND(llm_strat_ai_group_find_slowest_unit),
        MH_LIBMH_BIND(llm_strat_ai_group_pick_best_weapon_unit),
        MH_LIBMH_BIND(llm_strat_unit_issue_default_order),
        MH_LIBMH_BIND(llm_strat_ai_unit_group_assign_by_type),
        MH_LIBMH_BIND(llm_strat_ai_group_rally_formup_worker),
        MH_LIBMH_BIND(llm_strat_ai_group_scatter_random_worker),
        MH_LIBMH_BIND(llm_strat_ai_random_point_near),
        MH_LIBMH_BIND(llm_rand_state_advance),
        &detail::math_fsin_reduce_loop,
        &detail::math_cos_impl,

        // ---- RI-AI batch C layer 3 (2026-08-06) ----
        MH_LIBMH_BIND(llm_strat_ai_group_move_formation_rotating),

        // ---- RI-AI batch B layer 2 (2026-08-06) ----
        MH_LIBMH_BIND(llm_strat_ai_player_score_tier),
        MH_LIBMH_BIND(llm_strat_ai_score_build_categories),
        MH_LIBMH_BIND(llm_strat_ai_unit_weapon_power),
        MH_LIBMH_BIND(llm_strat_ai_rebalance_building_workers),
        MH_LIBMH_BIND(llm_strat_ai_plan_turret_upgrade),
        MH_LIBMH_BIND(llm_strat_ai_calc_power_supply_ratio),
        MH_LIBMH_BIND(llm_strat_ai_plan_mine_construction),
        MH_LIBMH_BIND(llm_strat_ai_storage_capacity_short_and_cap_check),
        MH_LIBMH_BIND(llm_strat_ai_react_resource_shortage),
        MH_LIBMH_BIND(llm_strat_ai_maintain_unit_housing),
        MH_LIBMH_BIND(llm_strat_ai_scan_construction_sites),
        // ---- RI-AI batch E (2026-08-07). Neither start_hq_attack_scenario nor scr_parse is
        // shadow-armed (see their .cpp files), so these eight are never reached through THIS call
        // set in practice -- wired for calls_are_complete() and so the pointer is never null if a
        // future session's judgment on arming changes.
        MH_LIBMH_BIND(llm_unit_create_soldier),
        MH_LIBMH_BIND(llm_diplomacy_set_relation),
        MH_LIBMH_BIND(llm_strat_ai_unit_commit_attack_on_enemy_hq),
        mh::host().asset_size,
        mh::host().asset_read,
        &MH_CRT(utils_malloc),
        &MH_CRT(utils_free),
        &MH_CRT(utils_str_cmp_ci),
        MH_CRT_CMP(spiral_offset_sort_cmp_004dc546, &::mh::ai::detail::spiral_offset_sort_cmp),
        // ---- RI-AI batch E (2026-08-07) -- player_tick's phase dispatch ----
        MH_LIBMH_BIND(game_GetStartingUnit),
        MH_LIBMH_BIND(llm_strat_unit_create),
        MH_LIBMH_BIND(llm_strat_bldg_max_defense_radius_sq),
        MH_LIBMH_BIND(llm_strat_ai_resource_spend_rate_update),
        MH_LIBMH_BIND(llm_strat_ai_update_opponent_relations),
        MH_LIBMH_BIND(llm_strat_ai_recompute_shortage_state),
        MH_LIBMH_BIND(llm_strat_ai_plan_unit_training),
        MH_LIBMH_BIND(llm_strat_ai_plan_construction),
        MH_LIBMH_BIND(llm_strat_ai_scan_bldg_repair_upgrade),
        MH_LIBMH_BIND(llm_strat_order_collect_available_projects_thunk),
        MH_LIBMH_BIND(llm_strat_ai_bldg_queue_process),
        MH_LIBMH_BIND(llm_strat_ai_group_home_guard_replenish),
        MH_LIBMH_BIND(llm_strat_ai_group_form_patrol),
        MH_LIBMH_BIND(llm_strat_ai_group_form_surplus_from_pool4),
        MH_LIBMH_BIND(llm_strat_ai_group_form_standby_from_pool3),
        MH_LIBMH_BIND(llm_strat_ai_group_expansion_form_or_repurpose),
        MH_LIBMH_BIND(llm_strat_ai_army_milestone_advance_or_attack),
        MH_LIBMH_BIND(llm_strat_ai_group_redistribute_units),
        MH_LIBMH_BIND(llm_strat_ai_invasion_launch_attack_group),
        MH_LIBMH_BIND(llm_strat_ai_unit_order_move_with_bump),
        // ---- active_unit_tick's engage loop ----
        MH_LIBMH_BIND(llm_strat_ai_holding_pen_scan_targets),
        MH_LIBMH_BIND(llm_strat_ai_target_list_invalidate_by_id),
        MH_LIBMH_BIND(llm_strat_ai_target_list_refresh_mothers),
        MH_LIBMH_BIND(llm_strat_ai_target_list_scan_visible_enemies),
        MH_LIBMH_BIND(llm_strat_ai_group_seed_resolved_target),
        MH_LIBMH_BIND(llm_strat_ai_attack_candidate_add),
        MH_LIBMH_BIND(llm_strat_ai_building_defense_weapon_range),
        MH_LIBMH_BIND(llm_strat_ai_scan_target_list_sort),
        MH_LIBMH_BIND(llm_strat_ai_group_enter_hold),
        // ---- unit_commit_attack_on_enemy_hq's three ----
        MH_LIBMH_BIND(llm_strat_unit_select_weapon),
        MH_LIBMH_BIND(llm_strat_unit_order_attack_building_reposition),
        MH_LIBMH_BIND(llm_strat_unit_order_move),
        // ---- the invasion-reinforcement family ----
        MH_LIBMH_BIND(llm_strat_ai_score_reinforcement_unit),
        MH_LIBMH_BIND(llm_strat_ai_create_reinforcement_unit),
        MH_LIBMH_BIND(llm_strat_ai_invasion_spawn_reinforcements),
        MH_LIBMH_BIND(llm_strat_ai_unit_should_abandon_target),
        // ---- RI-AI batch D / AI1D (2026-08-28), appended in ai_calls' declaration order ----
        MH_LIBMH_BIND(llm_strat_unit_estimate_weapon_damage),
        MH_LIBMH_BIND(llm_strat_unit_order_attack_target_enqueue),
        MH_LIBMH_BIND(llm_strat_unit_order_attack_target_alt_enqueue),
        MH_LIBMH_BIND(llm_strat_unit_order_attack_building_reposition_alt_enqueue),
        MH_LIBMH_BIND(llm_strat_ai_recompute_map_influence),
        MH_LIBMH_BIND(llm_strat_ai_turret_threat_rescan),
        MH_LIBMH_BIND(llm_strat_ai_player_tick),
        MH_LIBMH_BIND(llm_strat_ai_unit_group_tick),
        MH_LIBMH_BIND(llm_strat_ai_active_unit_tick),
        MH_LIBMH_BIND(llm_strat_unit_passive_engage_tick),
    };
    return c;
}


// live_calls() is a POSITIONAL aggregate initialiser over a struct of nothing but pointers, and C++
// SILENTLY ZERO-FILLS a short one. So adding a member to ai_calls and forgetting to bind it installs
// a null pointer that misbehaves only once that callee is actually reached -- i.e. it looks like a
// crash in the body under test rather than a wiring bug. `calls_are_complete()` walks the set as a
// pointer array and reports the first hole.
//
// This used to compare TWO sets (live_calls() and the shadow arm's shadow_calls()), and the hazard
// it named was a member wired into only one of them. The second set went with the differential
// oracle (F2D); the remaining sweep is narrower but still the cheap place to catch a null slot.
namespace {

constexpr int AI_CALLS_SLOTS = int(sizeof(ai_calls) / sizeof(void *));
static_assert(sizeof(ai_calls) == AI_CALLS_SLOTS * sizeof(void *),
              "ai_calls must stay an array of pointers for the completeness sweep to be valid");

int first_null_slot(const ai_calls &c) {
    const void *const *p = reinterpret_cast<const void *const *>(&c);
    for (int i = 0; i < AI_CALLS_SLOTS; ++i)
        if (!p[i]) return i;
    return -1;
}

} // namespace

bool calls_are_complete() {
    const int a = first_null_slot(live_calls());
    if (a >= 0) ai_say("; ai_calls: live_calls() slot %d of %d is NULL\n", a, AI_CALLS_SLOTS);
    return a < 0;
}

namespace {
void (*g_log)(const char *) = nullptr;
int g_trace                 = 0;
} // namespace

void set_logger(void (*fn)(const char *)) { g_log = fn; }

void ai_say(const char *fmt, ...) {
    if (!g_log) return;
    char    b[400];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(b, sizeof(b), _TRUNCATE, fmt, ap);
    va_end(ap);
    g_log(b);
}

// The BUDGET, not a countdown: each arm keeps its own call counter, so one busy arm cannot consume
// another's trace and a site that is never reached stays visibly silent rather than looking spent.
int trace_budget() { return g_trace; }

// LT1C (2026-09-02): the grid_wrap_mask OWNER accessor -- see the header comment. Returns the
// module's own writable binding so no second binding of RID_STRAT_AI_GRID_WRAP_MASK exists.
uint32_t *grid_wrap_mask() { return state().own.grid_wrap_mask; }


namespace {

constexpr mh::state::region_id ISLAND[] = {
    mh::state::RID_STRAT_AI_ATTACK_CANDIDATES,
    mh::state::RID_STRAT_AI_ATTACK_CANDIDATE_COUNT,
    mh::state::RID_STRAT_AI_ATTACK_HQ_UNIT_ID,
    mh::state::RID_STRAT_AI_ATTACK_MILESTONE_COUNT,
    mh::state::RID_STRAT_AI_ATTACK_OVERKILL_PCT,
    mh::state::RID_STRAT_AI_ATTACK_STRENGTH_TABLE,
    mh::state::RID_STRAT_AI_ATTACK_TIME_TABLE,
    mh::state::RID_STRAT_AI_BLDG_CONNECTIVITY_BASE,
    mh::state::RID_STRAT_AI_BLDG_CONNECTIVITY_TRIAL,
    mh::state::RID_STRAT_AI_BUILDING_CANDIDATE_SCRATCH_COUNT,
    mh::state::RID_STRAT_AI_BUILDING_CANDIDATE_SCRATCH_LIST,
    mh::state::RID_STRAT_AI_CFG_PROMO_ADD,
    mh::state::RID_STRAT_AI_CFG_PROMO_SUB,
    mh::state::RID_STRAT_AI_ENERGY_RATIO,
    mh::state::RID_STRAT_AI_EXPAND_SITE_COUNT,
    mh::state::RID_STRAT_AI_EXPAND_SITE_X,
    mh::state::RID_STRAT_AI_EXPAND_SITE_Y,
    mh::state::RID_STRAT_AI_EXTRA_SPACE,
    mh::state::RID_STRAT_AI_HOME_GUARD_TARGET_PCT,
    mh::state::RID_STRAT_AI_LOITER_ANGLE_SCALE,
    mh::state::RID_STRAT_AI_LOITER_JITTER_RADIUS,
    mh::state::RID_STRAT_AI_MAX_FUCK_RATIO,
    mh::state::RID_STRAT_AI_MINE_LOW_SHARE_PCT_THRESHOLD,
    mh::state::RID_STRAT_AI_MINE_QUALITY_HIST,
    mh::state::RID_STRAT_AI_MINE_REBALANCE_MIN_COUNT,
    mh::state::RID_STRAT_AI_MINE_ROSTER_INDEX,
    mh::state::RID_STRAT_AI_MINE_YIELD_ESTIMATE,
    mh::state::RID_STRAT_AI_MINE_YIELD_KERNEL,
    mh::state::RID_STRAT_AI_MIN_ATTACK_STRENGTH,
    mh::state::RID_STRAT_AI_PATROL_GROUP_MAX,
    mh::state::RID_STRAT_AI_PATROL_GROUP_SIZE,
    mh::state::RID_STRAT_AI_PATROL_WANDER_BOUNCES,
    mh::state::RID_STRAT_AI_QUADRANT_DX2,
    mh::state::RID_STRAT_AI_QUADRANT_DY2,
    mh::state::RID_STRAT_AI_REPAIR_RATIO,
    mh::state::RID_STRAT_AI_REPOSITION_SMALL_GROUP_MAX,
    mh::state::RID_STRAT_AI_RESOURCE_SPEND_WEIGHTS,
    mh::state::RID_STRAT_AI_SCR_KEYWORD_TABLE,
    mh::state::RID_STRAT_AI_SILO_RATIO,
    mh::state::RID_STRAT_AI_SITE_CANDIDATES,
    mh::state::RID_STRAT_AI_SITE_CANDIDATE_COUNT,
    mh::state::RID_STRAT_AI_SPREAD_TILES,
    mh::state::RID_STRAT_AI_SPREAD_TILE_COUNT,
    mh::state::RID_STRAT_AI_SURPLUS_GROUP_MIN_POOL4,
    mh::state::RID_STRAT_AI_TILE_SPIRAL_CELL_COUNT,
    mh::state::RID_STRAT_AI_TRAIN_QUEUE_PER_UNIT_CAP,
    mh::state::RID_STRAT_AI_UNEMPLOYED_MIN,
    mh::state::RID_STRAT_AI_UNEMPLOYED_RATIO,
};
constexpr int ISLAND_N = static_cast<int>(sizeof(ISLAND) / sizeof(ISLAND[0]));

// Static, not VirtualAlloc: 80 KB of DLL .bss, deterministic, no failure path to handle from
// inside an install hook. 16-byte aligned so any SSE-copied member keeps the alignment it had.
alignas(16) uint8_t g_island_store[81920];
bool g_island_moved = false;

constexpr uint8_t POISON = 0xCD;

// The L1 provider (ST4): serve the region's bytes from wherever live_base says they are. One
// generic pair for the whole island -- these are raw scratch/knob bytes with no codec, so the
// canonical stream IS the memory image at its moved location.
void island_emit(mh::state::region_id rid, mh::state::state_sink &s) {
    s.raw(reinterpret_cast<const void *>(static_cast<uintptr_t>(mh::state::live_base(rid))),
          mh::state::REGIONS[rid].size);
}
void island_load(mh::state::region_id rid, mh::state::state_source &src) {
    src.raw(reinterpret_cast<void *>(static_cast<uintptr_t>(mh::state::live_base(rid))),
            mh::state::REGIONS[rid].size);
}

} // namespace

int island_move() {
#ifdef MH_LIBMH_BUILD
    // LIB-REF-SPLIT: THE ISLAND MOVE IS A HOSTED-ONLY MECHANISM, and the reason is in its own
    // name. It exists to lift 48 AI regions OUT of the original image's .bss and POISON what
    // they leave behind, so that island_verify_tick can later prove no ORIGINAL body still
    // reaches them. Standalone there is no .bss to move out of, no original body to catch, and
    // -- once the stock base column goes -- REGIONS[rid].base is 0, so the memcpy below would
    // read through a null pointer and the memset would write through one. A standalone host
    // binds every region to its own allocation at init, which reaches the same place by the
    // supported route.
    //
    // -1 rather than 0: 0 already means `refused, store too small, NOTHING moved`, and a caller
    // that cannot tell those apart would read a configuration fact as a sizing failure. -1 is
    // the existing `cannot move` code (already returned for a second call).
    return -1;
#else
    if (g_island_moved) return -1; // the registry cannot be rebased twice; refuse loudly
    using namespace mh::state;
    static const region_provider me     = {&island_emit, &island_load, "mh::ai (island)"};
    uint32_t                     off    = 0;
    int                          hosted = 0;
    for (int i = 0; i < ISLAND_N; ++i) {
        const region_id rid = ISLAND[i];
        const uint32_t  sz  = REGIONS[rid].size;

        // THE HOST GOT HERE FIRST (SB-HOSTFREE). A relocating host binds every relocatable region,
        // and all 48 of these qualify -- so under that arrangement the bytes are ALREADY out of
        // .bss and the stock range is already poisoned. Moving them again would copy the poison
        // into this store and hand the AI 0xCD for its whole island.
        //
        // THE MOVE AND THE CLAIM ARE SEPARABLE, which is what makes this a two-line interlock
        // rather than a conflict. `island_emit`/`island_load` read `live_base`, so they serve the
        // bytes correctly wherever the host put them; all this path owes is the ST4 claim, so the
        // AI still answers for the region with a canonical stream. The offset is not advanced --
        // the host's arena holds these, not g_island_store.
        if (host_relocated(rid)) {
            claim(rid, &me);
            ++hosted;
            continue;
        }

        off = (off + 15u) & ~15u;
        if (off + sz > sizeof(g_island_store)) {
            ai_say("; [ai-island] REFUSED at %s -- store too small (%u + %u > %u); NOTHING moved\n",
                   REGIONS[rid].name, off, sz, (unsigned)sizeof(g_island_store));
            return 0; // partial rebase would be worse than none; nothing rebased yet on this path
        }
        std::memcpy(g_island_store + off,
                    reinterpret_cast<const void *>(static_cast<uintptr_t>(REGIONS[rid].base)), sz);
        rebase(rid, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_island_store + off)), sz);
        claim(rid, &me); // ST4: the module now answers for these bytes, by region id
        std::memset(reinterpret_cast<void *>(static_cast<uintptr_t>(REGIONS[rid].base)), POISON, sz);
        off += sz;
    }
    g_island_moved = true;
    ai_say("; [ai-island] MOVED %d region(s), %u bytes, out of .bss -- old ranges poisoned 0x%02x, "
           "L1-claimed by mh::ai (%d already relocated by the HOST: claimed, not moved)\n",
           ISLAND_N - hosted, off, POISON, hosted);
    return ISLAND_N;
#endif // MH_LIBMH_BUILD
}

// The done_when's runtime check: READ the original .bss VAs while the game plays and find them
// untouched. Poison makes untouched OBSERVABLE; called from the promoted players_tick adapter at
// widening call milestones so the claim covers the trajectory, not just the install.
void island_verify_tick(long call_no) {
#ifdef MH_LIBMH_BUILD
    // LIB-REF-SPLIT: the check READS the original .bss VAs looking for poison. Standalone those
    // addresses are not mapped and REGIONS[rid].base is 0, so the scan below would dereference
    // null -- and it could never find anything either way, because what it looks for is an
    // ORIGINAL body writing into the island, and there are none. island_move() refuses
    // standalone, so g_island_moved is false and this was already a runtime no-op; the guard
    // makes it one the compiler can see. The symbol STAYS: ai_promote.cpp's promoted
    // players_tick wrapper calls it unconditionally, and that wrapper is compiled into libmh.
    (void)call_no;
    return;
#else
    if (!g_island_moved) return;
    if (call_no != 100 && call_no != 1000 && call_no != 10000 && call_no != 50000) return;
    using namespace mh::state;
    for (int i = 0; i < ISLAND_N; ++i) {
        const region_id rid = ISLAND[i];
        const uint8_t  *p =
            reinterpret_cast<const uint8_t *>(static_cast<uintptr_t>(REGIONS[rid].base));
        for (uint32_t b = 0; b < REGIONS[rid].size; ++b) {
            if (p[b] != POISON) {
                ai_say("; [ai-island] TOUCHED -- %s old VA +0x%x holds 0x%02x at players_tick call "
                       "#%ld: an ORIGINAL body still reaches the island\n",
                       REGIONS[rid].name, b, p[b], call_no);
                return;
            }
        }
    }
    ai_say("; [ai-island] old .bss VAs UNTOUCHED (%d regions, players_tick call #%ld)\n", ISLAND_N,
           call_no);
#endif // MH_LIBMH_BUILD
}

} // namespace mh::ai
