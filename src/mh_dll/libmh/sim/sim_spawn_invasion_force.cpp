//
// sim/sim_spawn_invasion_force.cpp -- see sim_spawn_invasion_force.h. Translated from the
// DISASSEMBLY (tmp/decomp/llm_strat_spawn_invasion_force_004dd446.asm), the Ghidra .c being a draft.
//
#include "sim/sim_spawn_invasion_force.h"

#include "addr/mh_calls.gen.h"  // typed callables for the delegated/effectful originals we call OUT to
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"
#include "crt/crt_select.h" // LIB-CRT: MH_CRT() picks the vendored CRT in the standalone build

namespace mh::sim {

const spawn_invasion_force_calls &live_spawn_invasion_force_calls() {
    static const spawn_invasion_force_calls c = {
        MH_LIBMH_BIND(llm_strat_ai_scr_parse),
        MH_CRT(utils_sprintf__vi),
        MH_LIBMH_BIND(llm_strat_ai_group_create),
        MH_LIBMH_BIND(llm_strat_ai_group_task_enqueue),
        MH_LIBMH_BIND(llm_strat_ai_init_build_candidate_priorities),
        MH_LIBMH_BIND(llm_strat_ai_invasion_spawn_reinforcements),
    };
    return c;
}

namespace {

// player_profile.status_flags bit 1 (E_STRAT_PLAYER_STATUS). libmh/sim/ keeps its own copy rather than
// depending on ai/ai_state.h, exactly like sim_player_presence_lost.
inline constexpr uint32_t STATUS_ALIVE = 0x2u;

// The two AI-script filenames the original passes, byte-identical to the s_init_ai_scr /
// s_init_ai_02d_scr constants at 0x00506d84 / 0x00506d90 (read-memory-confirmed this slice: a single
// backslash). scr_boot is a mutable buffer because llm_strat_ai_scr_parse takes a char* (it does not
// modify it, but the callable's type is char*).
constexpr const char *SCR_PLANET_FMT = "init\\ai%02d.scr";

} // namespace

namespace detail {

// ---- llm_strat_spawn_invasion_force @0x004dd446 --------------------------------------------------
uint32_t spawn_invasion_force(const sim_view &v, sim_store &own, const spawn_invasion_force_calls &c,
                              uint32_t player, int32_t is_alien_race, int32_t home_tile_x,
                              int32_t home_tile_y, int32_t invasion_points) {
    // 0x004dd44e: utils_assert_stack_capacity(0x134) -- a Watcom stack probe with no observable
    // effect; omitted.

    // 0x004dd45f-0x004dd489: parse the global AI script, then the per-planet one (both DELEGATED --
    // llm_strat_ai_scr_parse loads the AI.SCR config that populates the period constants read below).
    char scr_boot[] = "init\\ai.scr";
    c.ai_scr_parse(scr_boot);
    char scr_planet[256];
    c.sprintf_ai_scr(scr_planet, SCR_PLANET_FMT, *v.planet_index);
    c.ai_scr_parse(scr_planet);

    // 0x004dd48e: raise the active-player high-water mark to player+1 if this player is beyond it.
    if (*v.ai_active_player_count <= static_cast<int32_t>(player))
        own.ai_active_player_count() = static_cast<int32_t>(player) + 1;

    // 0x004dd49e: mark the profile ALIVE (bit 0x2).
    own.profile_at(static_cast<int32_t>(player)).status_flags |= STATUS_ALIVE;

    player_data &pd = own.player_at(player);

    // 0x004dd4c1-0x004dd51c: the header scalars (in the original's write order).
    pd.is_alien_race              = is_alien_race;
    pd.ai_home_tile_x             = home_tile_x;
    pd.ai_home_tile_y             = home_tile_y;
    pd.ai_resource_shortage_state = 0; // (the original stores this twice; identical value)
    pd.ai_resource_need_score     = 0; // (likewise)
    pd.ai_resource_need_threshold = 6;
    pd.ai_established             = 1;
    // 0x004dd4fb: a DWORD store of 4 at ai_phase_flags -- the field is a byte at +0x14 followed by 3
    // pad bytes (_pad_0x15) that nothing reads, so the plain byte write is behaviourally identical.
    pd.ai_phase_flags            = 4;
    pd.ai_enabled                = 1;
    pd.ai_invasion_force         = 1;
    pd.ai_invasion_points        = invasion_points;
    pd.ai_patrol_quadrant_cursor = 0;
    pd.ai_group_count            = 0;
    pd.ai_target_list_count      = 0;
    // (0xe9633c / 0xe96340 stored 0 a second time here in the original; same values as above.)
    pd.next_group_serial   = 0;
    pd.ai_bldg_queue_count = 0;

    // 0x004dd568: the AI clock base 0.001f, then the three per-player staggered think-clocks. The x87
    // sequence is FILD player / FST float / FMUL float-period / FMUL double-stagger / FSTP float; on
    // this /fp:precise x87 build the float/double multiplies happen in 80-bit and round to float on
    // store, which the operand order below reproduces (for an integer player this is exact anyway).
    pd.ai_clock    = 0.001f;
    const float pf = static_cast<float>(static_cast<int32_t>(player));
    pd.ai_clock_m  = static_cast<float>(static_cast<double>(pf) * *v.ai_move_period *
                                        *v.ai_invasion_clock_stagger);
    pd.ai_clock_t  = static_cast<float>(static_cast<double>(pf) * *v.ai_tactic_period *
                                        *v.ai_invasion_clock_stagger);
    pd.ai_clock_s  = static_cast<float>(static_cast<double>(pf) * *v.ai_strategy_period *
                                        *v.ai_invasion_clock_stagger);

    // 0x004dd5be-0x004dd620: the remaining header scalars.
    pd.ai_attack_orders_issued           = 0;
    pd.ai_labor_utilization              = 0.0; // (original: two dword stores of 0 = the double 0.0)
    pd.ai_reposition_cached_member_count = 0;
    pd.ai_start_units_remaining          = *v.ai_cfg_start_units;
    pd.ai_start_units_pending_spawn      = *v.ai_cfg_start_units;
    pd.ai_promo_credit                   = 0;
    pd.ai_attack_milestone_index         = 0;
    pd.ai_attack_milestone_clock         = 0.0f;
    pd.ai_map_changed_pending            = 0;
    pd.ai_turret_rescan_pending          = 0;

    // 0x004dd62e: resource_spent[0..3] = 0.
    for (int32_t i = 0; i < 4; ++i) pd.resource_spent[i] = 0;

    // 0x004dd660: the two spend rings, logically int[32][4] (flat index slot*4 + res). Both zeroed.
    for (int32_t slot = 0; slot < 0x20; ++slot)
        for (int32_t res = 0; res < 4; ++res) {
            pd.ai_spend_rate_denom_ring[slot * 4 + res] = 0;
            pd.ai_spend_rate_numer_ring[slot * 4 + res] = 0;
        }
    // 0x004dd6bd: ai_spend_ring_cursor = 0 (a single store after the ring loops).
    pd.ai_spend_ring_cursor = 0;

    // 0x004dd6cc: resource_spend_total[0..7] = 1 (initialised to 1, not 0 -- it is a divide-safe
    // denominator; see the field comment).
    for (int32_t i = 0; i < 8; ++i) pd.resource_spend_total[i] = 1;

    // 0x004dd702: the AI tile-flag grid, seeded from map::g::passable. Column-major (x is the high
    // byte): an IMPASSABLE tile (passable byte == 0) gets flag 8, a passable one gets 0. Outer x runs
    // to map width, inner y to map height.
    for (int32_t x = 0; x < *v.map_width; ++x)
        for (int32_t y = 0; y < *v.map_height; ++y) {
            const int32_t idx          = (x << 8) | y;
            pd.ai_tile_flags_grid[idx] = (v.passable[idx] == 0) ? 8 : 0;
        }

    // 0x004dd757: every one of the 32 ai_groups gets member_count = 0 (word) and current_param = 3
    // (dword). The rest of each record stays as group_create left it (zero, offline).
    for (int32_t g = 0; g < 0x20; ++g) {
        pd.ai_groups[g].member_count  = 0;
        pd.ai_groups[g].current_param = 3;
    }

    // 0x004dd792: the three per-opponent int[8] rows. seen_count/flags zeroed; relation is +1 toward
    // self, -1 toward everyone else.
    for (int32_t i = 0; i < 8; ++i) {
        pd.ai_intel_seen_count[i] = 0;
        pd.ai_intel_flags[i]      = 0;
        pd.ai_player_relation[i]  = (i == static_cast<int32_t>(player)) ? 1 : -1;
    }

    // 0x004dd7e7: ai_train_queued_by_unit_type[0 .. UNIT.total] = 0 (INCLUSIVE, unsigned bound).
    for (uint32_t i = 0; i <= v.cfg_unit_sec->total; ++i) pd.ai_train_queued_by_unit_type[i] = 0;

    // 0x004dd813: ai_resource_site_count = 0.
    pd.ai_resource_site_count = 0;

    // 0x004dd81d-0x004dd864: create 5 AI task groups (goals 1/2/6/6/6). The goal is written to groups
    // 0..4 by fixed index (NOT via group_create's return value or ai_group_count).
    const int32_t p = static_cast<int32_t>(player);
    c.ai_group_create(p);
    pd.ai_groups[0].goal = 1;
    c.ai_group_create(p);
    pd.ai_groups[1].goal = 2;
    c.ai_group_create(p);
    pd.ai_groups[2].goal = 6;
    c.ai_group_create(p);
    pd.ai_groups[3].goal = 6;
    c.ai_group_create(p);
    pd.ai_groups[4].goal = 6;

    // 0x004dd86d-0x004dd8ed: the matching task enqueues (groups 0..4; group 0 param 0x1d3, group 1
    // param 0x183, groups 2..4 param 0). All the trailing args are 0.
    c.ai_group_task_enqueue(p, 0, 0, 0x1d3, 0, 0, 0, 0, 0);
    c.ai_group_task_enqueue(p, 1, 0, 0x183, 0, 0, 0, 0, 0);
    c.ai_group_task_enqueue(p, 2, 0, 0, 0, 0, 0, 0, 0);
    c.ai_group_task_enqueue(p, 3, 0, 0, 0, 0, 0, 0, 0);
    c.ai_group_task_enqueue(p, 4, 0, 0, 0, 0, 0, 0, 0);

    // 0x004dd8f4: seed the build-candidate priorities, THEN reset the build-plan cursor/length (the
    // order matters -- init_build_candidate_priorities is what would otherwise leave them set).
    c.ai_init_build_candidate_priorities(p);
    pd.ai_build_plan_cursor       = 0;
    pd.ai_build_plan_len_and_flag = 0;

    // 0x004dd90f: launch the first reinforcement wave and return its result.
    return static_cast<uint32_t>(c.ai_invasion_spawn_reinforcements(p));
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

uint32_t spawn_invasion_force(uint32_t player, int32_t is_alien_race, int32_t home_tile_x,
                              int32_t home_tile_y, int32_t invasion_points) {
    sim_state st = state();
    return detail::spawn_invasion_force(st.read, st.own, live_spawn_invasion_force_calls(), player,
                                        is_alien_race, home_tile_x, home_tile_y, invasion_points);
}

} // namespace mh::sim
