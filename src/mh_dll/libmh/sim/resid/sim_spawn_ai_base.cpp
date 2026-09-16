//
// sim/resid/sim_spawn_ai_base.cpp -- see sim_spawn_ai_base.h. Translated from the DISASSEMBLY
// (tmp/decomp_sim_resid/llm_strat_spawn_ai_base_004dcd0e.asm,
// tmp/decomp_sim_resid/llm_strat_sort_sites_by_dist_004dc117.asm), the Ghidra .c drafts being
// field-name reading aids only (see the header banner's FIELD MAPPING note).
//
#include "sim/resid/sim_spawn_ai_base.h"

#include <utility> // std::swap

#include "addr/mh_calls.gen.h"  // typed callables for the frontier originals we call OUT to
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"
#include "crt/crt_select.h" // LIB-CRT: MH_CRT() picks the vendored CRT in the standalone build

namespace mh::sim {

const spawn_ai_base_calls &live_spawn_ai_base_calls() {
    static const spawn_ai_base_calls c = {
        MH_LIBMH_BIND(llm_strat_ai_scr_parse),
        MH_CRT(utils_sprintf__vi),
        MH_LIBMH_BIND(llm_strat_ai_group_create),
        MH_LIBMH_BIND(llm_strat_ai_group_task_enqueue),
        MH_LIBMH_BIND(llm_strat_ai_init_build_candidate_priorities),
        MH_LIBMH_BIND(llm_strat_ai_build_plan_push),
        MH_LIBMH_BIND(llm_strat_toroidal_dist_sq),
    };
    return c;
}

namespace {

// player_profile.status_flags bit 1 (E_STRAT_PLAYER_STATUS). libmh/sim/ keeps its own file-local copy
// rather than depending on ai/ai_state.h, exactly like sim_spawn_invasion_force.cpp /
// sim_player_presence_lost.cpp.
inline constexpr uint32_t STATUS_ALIVE = 0x2u;

// The per-planet AI-script format string, byte-identical to the s_init_AI_02d_SCR constant at
// 0x00506d6c (Ghidra auto-label spelling, uppercase AI/SCR, single backslash). The fixed
// "init\AI.SCR" boot string is at 0x00506d60, parsed unconditionally before this one -- see the
// header banner's TWO SCRIPTS, NO BRANCH note.
constexpr const char *SCR_PLANET_FMT = "init\\AI%02d.SCR";

} // namespace

namespace detail {

// ---- llm_strat_sort_sites_by_dist @0x004dc117 ------------------------------------------------------
void sort_sites_by_dist(sim_store &own, const spawn_ai_base_calls &c, int32_t player_id) {
    player_data &pd = own.player_at(static_cast<uint32_t>(player_id));

    // 0x004dc144-0x004dc14c: nothing to sort.
    const int32_t count = pd.ai_resource_site_count;
    if (count == 0) return;

    // 0x004dc152-0x004dc18c: one toroidal distance-squared per site, home-tile to the site's fine
    // tile coords (coarse grid coord <<2). grid_x/grid_y are read UNSIGNED (MOVZX word) even though
    // the struct types them int16_t. Scratch sized to the array's max extent (1024), matching the
    // original's `utils_assert_stack_capacity(0x103c)` (~1024 dwords) -- no bound beyond `count` is
    // added.
    uint32_t dist[1024];
    for (int32_t i = 0; i < count; ++i) {
        const auto &site = pd.ai_resource_sites[i];
        dist[i]          = c.toroidal_dist_sq(pd.ai_home_tile_x, pd.ai_home_tile_y,
                                              static_cast<int32_t>(static_cast<uint16_t>(site.grid_x)) << 2,
                                              static_cast<int32_t>(static_cast<uint16_t>(site.grid_y)) << 2);
    }

    // 0x004dc1b1-0x004dc25a: bubble sort. The original tracks "no swap this pass" in a register
    // (ESI) and exits the outer pass loop the instant a full inner pass makes none; a plain bool
    // here is the same early-exit shape.
    bool swapped = true;
    while (swapped) {
        swapped = false;
        for (int32_t i = 0; i < count - 1; ++i) {
            if (dist[i + 1] < dist[i]) {
                // 0x004dc1d9-0x004dc22c: the original moves each record through a 10-byte stack
                // temp (MOVSD;MOVSD;MOVSW) -- a whole-record swap, all 5 fields together -- then
                // separately swaps the two cached distance values. Equivalent to:
                std::swap(pd.ai_resource_sites[i + 1], pd.ai_resource_sites[i]);
                std::swap(dist[i + 1], dist[i]);
                swapped = true;
            }
        }
    }
}

// ---- llm_strat_spawn_ai_base @0x004dcd0e ------------------------------------------------------------
void spawn_ai_base(const sim_view &v, sim_store &own, const spawn_ai_base_calls &c, int32_t player,
                   int32_t is_alien, int32_t x, int32_t y) {
    // 0x004dcd16: utils_assert_stack_capacity(0x138) -- inert Watcom stack probe, omitted.

    // 0x004dcd24-0x004dcd50: TWO SCRIPTS, NO BRANCH (see header banner) -- the global default first,
    // then the per-planet override, both unconditional.
    char scr_boot[] = "init\\AI.SCR"; // @0x00506d60
    c.ai_scr_parse(scr_boot);
    char scr_planet[256];
    c.sprintf_ai_scr(scr_planet, SCR_PLANET_FMT, *v.planet_index); // "init\AI%02d.SCR" % G_PLANET_INDEX
    c.ai_scr_parse(scr_planet);

    // 0x004dcd55-0x004dcd69: raise the active-player high-water mark to player+1 if this player is
    // new (unsigned compare in the original: JA skips the raise when count > player).
    if (*v.ai_active_player_count <= player) own.ai_active_player_count() = player + 1;

    // 0x004dcd6e-0x004dcd7f: mark the profile ALIVE (bit 0x2). player*0x740 stride, offset 0 (LSB of
    // status_flags) -- a byte-wide OR in the original, behaviourally identical to OR-ing the dword.
    own.profile_at(player).status_flags |= STATUS_ALIVE;

    player_data &pd = own.player_at(static_cast<uint32_t>(player));

    // 0x004dcd9b-0x004dcdad: the header scalars, in the original's write order.
    pd.is_alien_race              = is_alien;
    pd.ai_home_tile_x             = x;
    pd.ai_home_tile_y             = y;
    pd.ai_resource_shortage_state = 0;
    pd.ai_resource_need_score     = 0;
    pd.ai_resource_need_threshold = 6;
    pd.ai_established             = 0;
    // 0x004dcdd5: a DWORD store of 7 at ai_phase_flags -- the field is a byte at +0x14 followed by 3
    // pad bytes (_pad_0x15) nothing reads, so the plain byte write is behaviourally identical.
    pd.ai_phase_flags            = 7;
    pd.ai_enabled                = 1;
    pd.ai_invasion_force         = 0;
    pd.ai_patrol_quadrant_cursor = 0;
    pd.ai_group_count            = 0;
    pd.ai_target_list_count      = 0;
    // 0x004dce11-0x004dce1b: ai_resource_need_score / ai_resource_shortage_state stored 0 a SECOND
    // time here -- transcribed as-is (see header PRESERVED-AS-IS note).
    pd.ai_resource_need_score     = 0;
    pd.ai_resource_shortage_state = 0;
    pd.next_group_serial          = 0;
    pd.ai_bldg_queue_count        = 0;

    // 0x004dce39-0x004dce8f: the AI clock base 0.001f, then the three per-player staggered
    // think-clocks. The x87 sequence is FILD player / FST float / FMUL float-period / FMUL
    // double-stagger / FSTP float; the operand order below reproduces it (for an integer player this
    // is exact regardless of intermediate precision).
    pd.ai_clock    = 0.001f;
    const float pf = static_cast<float>(player);
    pd.ai_clock_m  = static_cast<float>(static_cast<double>(pf) * *v.ai_move_period *
                                        *v.ai_base_spawn_timer_stagger_scale);
    pd.ai_clock_t  = static_cast<float>(static_cast<double>(pf) * *v.ai_tactic_period *
                                        *v.ai_base_spawn_timer_stagger_scale);
    pd.ai_clock_s  = static_cast<float>(static_cast<double>(pf) * *v.ai_strategy_period *
                                        *v.ai_base_spawn_timer_stagger_scale);

    // 0x004dce95-0x004def8: the remaining header scalars, in the original's write order (promo
    // credit before attack-milestone index before attack-milestone clock).
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

    // 0x004dcf01-0x004dcf34: resource_spent[0..3] = 0.
    for (int32_t i = 0; i < 4; ++i) pd.resource_spent[i] = 0;

    // 0x004dcf3d-0x004dcf8c: the two spend rings, logically int[32][4] (flat index slot*4 + res).
    // Both zeroed together, inner-then-outer per the original's nested loop.
    for (int32_t slot = 0; slot < 0x20; ++slot)
        for (int32_t res = 0; res < 4; ++res) {
            pd.ai_spend_rate_denom_ring[slot * 4 + res] = 0;
            pd.ai_spend_rate_numer_ring[slot * 4 + res] = 0;
        }

    // 0x004dcf86: ai_spend_ring_cursor = 0 (single store after the ring loops).
    pd.ai_spend_ring_cursor = 0;

    // 0x004dcfb5-0x004dcfe2: resource_spend_total[0..7] = 1 (a divide-safe denominator default, not
    // a zero-init).
    for (int32_t i = 0; i < 8; ++i) pd.resource_spend_total[i] = 1;

    // 0x004dcfed-0x004dd048: the AI tile-flag grid, seeded from map::g::passable. OUTER x runs to
    // map width, INNER y to map height (confirmed by which global each loop bound compares against:
    // 0x00825084==width bounds the outer counter, 0x00825064==height the inner one). An impassable
    // tile (passable byte == 0) gets flag 8, a passable one gets 0.
    for (int32_t xt = 0; xt < *v.map_width; ++xt)
        for (int32_t yt = 0; yt < *v.map_height; ++yt) {
            const int32_t idx          = (xt << 8) | yt;
            pd.ai_tile_flags_grid[idx] = (v.passable[idx] == 0) ? 8 : 0;
        }

    // 0x004dd04c-0x004dd087: every one of the 32 ai_groups gets member_count = 0 (word) and
    // current_param = 3 (dword) -- see header PRESERVED-AS-IS note. The rest of each record stays
    // whatever ai_group_create leaves it (below).
    for (int32_t g = 0; g < 0x20; ++g) {
        pd.ai_groups[g].member_count  = 0;
        pd.ai_groups[g].current_param = 3;
    }

    // 0x004dd08d-0x004dd0e6: the three per-opponent int[8] rows. seen_count/flags zeroed; relation
    // is +1 toward self, -1 toward everyone else.
    for (int32_t i = 0; i < 8; ++i) {
        pd.ai_intel_seen_count[i] = 0;
        pd.ai_intel_flags[i]      = 0;
        pd.ai_player_relation[i]  = (i == player) ? 1 : -1;
    }

    // 0x004dd0ec-0x004dd11c: ai_train_queued_by_unit_type[0 .. UNIT.total] = 0 (INCLUSIVE, unsigned
    // bound -- JBE per mh_addrs.gen.h's own comment on G_UNIT_COUNT_TOTAL).
    for (uint32_t i = 0; i <= v.cfg_unit_sec->total; ++i) pd.ai_train_queued_by_unit_type[i] = 0;

    // 0x004dd122-0x004dd14f: ai_train_queued_by_ai_unit[0..11] = 0 (12 elements).
    for (int32_t i = 0; i < 12; ++i) pd.ai_train_queued_by_ai_unit[i] = 0;

    // 0x004dd151: ai_resource_site_count = 0 -- no bound is ever checked against it again below
    // (see header PRESERVED-AS-IS note).
    pd.ai_resource_site_count = 0;

    // 0x004dd162-0x004dd296: the coarse resource-grid scan. OUTER `edi` (fine height coordinate
    // divided by 4) runs to map_height/4, INNER `x4` (fine width coordinate divided by 4) to
    // map_width/4 -- confirmed by which global each loop's bound check compares against, same as
    // the passable loop above. The /4 in the original is a shift-with-sign-correction sequence
    // (SAR/SHL/SBB/SAR) that implements exact truncate-toward-zero division -- bit-identical to
    // plain C `/` for these always-non-negative map extents, so plain `/4` is used here.
    const int32_t coarse_h = *v.map_height / 4;
    const int32_t coarse_w = *v.map_width / 4;
    for (int32_t edi = 0; edi < coarse_h; ++edi) {
        for (int32_t x4 = 0; x4 < coarse_w; ++x4) {
            // 0x004dd174-0x004dd193: sum this coarse cell's 8 resource values. v.resources is
            // [64][64] row-major with x as the row (resources_at(x,y) == resources_mut_[x*64+y]),
            // read UNSIGNED (MOVZX) despite the field being int16_t.
            uint32_t sum = 0;
            for (int32_t r = 0; r < 8; ++r)
                sum += static_cast<uint16_t>(v.resources[x4 * 64 + edi].value[r]);

            // 0x004dd193-0x004dd211: strictly greater than the live mine-worth threshold (UNSIGNED
            // compare per mh_addrs.gen.h's own comment on _G_LLM_STRAT_AI_CFG_MINE_WORTH) records a
            // new candidate site. status=0 (unresolved), build_tile_x/y=-1 (0xffff, "unset").
            if (sum > static_cast<uint32_t>(*v.mine_worth)) {
                auto &site        = pd.ai_resource_sites[pd.ai_resource_site_count];
                site.status       = 0;
                site.grid_x       = static_cast<int16_t>(x4);
                site.grid_y       = static_cast<int16_t>(edi);
                site.build_tile_x = -1;
                site.build_tile_y = -1;
                ++pd.ai_resource_site_count;
            }

            // 0x004dd211-0x004dd25d: any nonzero resource presence (not gated by the mine-worth
            // threshold) OR-stamps bit 0x80 across this coarse cell's whole 4x4 fine-tile footprint.
            if (sum != 0) {
                for (int32_t sub_x = 0; sub_x < 4; ++sub_x)
                    for (int32_t sub_y = 0; sub_y < 4; ++sub_y) {
                        const int32_t idx = ((x4 * 4 + sub_x) << 8) | (edi * 4 + sub_y);
                        pd.ai_tile_flags_grid[idx] |= 0x80;
                    }
            }
        }
    }

    // 0x004dd296-0x004dd29c: re-order the freshly-populated site list nearest-home-first.
    // INTRA-UNIT sibling (G21): a plain detail:: call, never mh::call::.
    sort_sites_by_dist(own, c, player);

    // 0x004dd2a1-0x004dd318: create 5 AI task groups (goals 1/2/6/6/6). The goal is written to
    // groups 0..4 by FIXED index (not via ai_group_create's return value) -- see header
    // PRESERVED-AS-IS note.
    c.ai_group_create(player);
    pd.ai_groups[0].goal = 1;
    c.ai_group_create(player);
    pd.ai_groups[1].goal = 2;
    c.ai_group_create(player);
    pd.ai_groups[2].goal = 6;
    c.ai_group_create(player);
    pd.ai_groups[3].goal = 6;
    c.ai_group_create(player);
    pd.ai_groups[4].goal = 6;

    // 0x004dd321-0x004dd3ba: the matching task enqueues (groups 0..4; group 0 param 0x1d3, group 1
    // param 0x183, groups 2..4 param 0). All trailing args are 0.
    c.ai_group_task_enqueue(player, 0, 0, 0x1d3, 0, 0, 0, 0, 0);
    c.ai_group_task_enqueue(player, 1, 0, 0x183, 0, 0, 0, 0, 0);
    c.ai_group_task_enqueue(player, 2, 0, 0, 0, 0, 0, 0, 0);
    c.ai_group_task_enqueue(player, 3, 0, 0, 0, 0, 0, 0, 0);
    c.ai_group_task_enqueue(player, 4, 0, 0, 0, 0, 0, 0, 0);

    // 0x004dd3ba-0x004dd3cf: seed the build-candidate priorities, then reset the build-plan
    // cursor/length.
    c.ai_init_build_candidate_priorities(player);
    pd.ai_build_plan_cursor       = 0;
    pd.ai_build_plan_len_and_flag = 0;

    // 0x004dd3d9-0x004dd43a: six build-plan pushes. The first two both read ai_mine_candidate_tier1
    // (duplicate, transcribed as-is). The last reads player+1's ai_housing_candidate_soldier -- the
    // ALIASED FIELD documented in the header banner, not a translation bug.
    c.ai_build_plan_push(player, pd.ai_mine_candidate_tier1);
    c.ai_build_plan_push(player, pd.ai_mine_candidate_tier1);
    c.ai_build_plan_push(player, pd.ai_build_candidate_primary);
    c.ai_build_plan_push(player, pd.ai_build_candidate_secondary);
    c.ai_build_plan_push(player, pd.ai_resource_shortage_candidates[0]);
    c.ai_build_plan_push(player, own.player_at(static_cast<uint32_t>(player) + 1).ai_housing_candidate_soldier);
}

} // namespace detail

// ---- the public wrappers ---------------------------------------------------------------------------

void sort_sites_by_dist(int32_t player_id) {
    sim_state st = state();
    detail::sort_sites_by_dist(st.own, live_spawn_ai_base_calls(), player_id);
}

void spawn_ai_base(int32_t player, int32_t is_alien, int32_t x, int32_t y) {
    sim_state st = state();
    detail::spawn_ai_base(st.read, st.own, live_spawn_ai_base_calls(), player, is_alien, x, y);
}

} // namespace mh::sim
