//
// seams/move_carrier.cpp -- mods:M1H lane A: the honest carrier report + the splice hit counters. Design and the pure rules:
// seams/move_carrier.h.
//
// WHAT RUNS WHEN. The seams (net_lockstep.cpp, move_proto_*.cpp, combat_proto.cpp) install their splices at DllMain time and
// tell this module each site's state (note_site). The setters (gameplay apply hooks) tell it the requested values (request).
// The report itself waits for the unit-state dispatch table to be filled (strategic-mode init), then prints ONE line per key
// whose value is not its retail default. The stubs bump g_mv_hit[] when their decision path runs; the counters are logged,
// throttled to once a second while they change, as `; move hits ...`.
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>
#include <string.h>

#include "seams/move_carrier.h"

#include "addr/mh_rebind.gen.h" // mh::rebind::armed / ROW_*
#include "addr/mh_regions.gen.h"
#include "hook/promoted.h"        // promoted_owner_of -- a function whose ENTRY libmh claimed
#include "state/region_runtime.h" // mh::state::ptr
#include "seams/net_internal.h"   // seam_log

#pragma comment(lib, "user32.lib") // wsprintfA

extern "C" uint32_t g_mv_hit[MVH_COUNT] = {};

namespace mh::seams::mv {

bool retail_handler_dispatched(uintptr_t handler_va) {
    const uint32_t *t = mh::state::ptr<const uint32_t>(mh::state::RID_STRAT_UNIT_STATE_FUNCS);
    return table_has(t, 255, static_cast<uint32_t>(handler_va));
}

bool rebind_row_armed(int row) { return mh::rebind::armed(row); }

// mods:M1BE: is the retail building anim-state handler `va` still in the live tick2 table (100 type slots)?
bool retail_tick2_dispatched(uintptr_t handler_va) {
    const uint32_t *t = mh::state::ptr<const uint32_t>(mh::state::RID_STRAT_BLDG_TICK2_FUNCS);
    return table_has(t, 100, static_cast<uint32_t>(handler_va));
}

namespace {

// ---- the retail unit-state handlers that root the call chains --------------------------------------------------------------
constexpr uintptr_t H_WALKER = 0x0047c903, H_MARSHAL = 0x00482451, H_SQUAD_MERGE = 0x0047ea62, H_ATTACK_BUILDING = 0x00484e10;

// ---- per-site description --------------------------------------------------------------------------------------------------
struct site_desc {
    const char *name;
    uintptr_t   va;
    uintptr_t   root;    // the retail table handler whose body (transitively) reaches the site; 0 = not table-rooted (entry claim decides)
    int         rows[3]; // the rebind rows on the chain from the root down to the site's function (first rebindable callee first)
    int         nrows;
    int         hit;            // hit_id of the stub's counter, -1 = none (a runtime-flipped byte)
    bool        twin   = true;  // the libmh twin of the function holding this site carries the rule (false = a displaced site would go unfixed)
    uintptr_t   caller = 0;     // root == 0 only: the retail function that calls this site's function; when libmh claims it, the chain rows decide
    bool        tick2  = false; // root is a handler of the BUILDING tick2 table (mods:M1BE), not of the unit-state table
};

constexpr int       R_COMMIT         = static_cast<int>(mh::rebind::ROW_llm_strat_group_move_order_commit);
constexpr int       R_PATHFIND       = static_cast<int>(mh::rebind::ROW_llm_strat_group_move_order_pathfind);
constexpr int       R_FLOOD          = static_cast<int>(mh::rebind::ROW_llm_map_region_flood_reachable);
constexpr int       R_BUILD_STEPS    = static_cast<int>(mh::rebind::ROW_llm_strat_pathfind_build_steps);
constexpr int       R_ROUTE_LEG      = static_cast<int>(mh::rebind::ROW_llm_strat_pathfind_route_leg_group_and_sort);
constexpr int       R_SLOT_DIST      = static_cast<int>(mh::rebind::ROW_llm_strat_slot_dist_to_ref);
constexpr int       R_STEP_CHECK     = static_cast<int>(mh::rebind::ROW_llm_strat_path_step_check_and_request_detour);
constexpr int       R_DETOUR         = static_cast<int>(mh::rebind::ROW_llm_strat_unit_path_detour);
constexpr int       R_ORDER_DISPATCH = static_cast<int>(mh::rebind::ROW_llm_strat_order_queue_dispatch);
constexpr uintptr_t F_SIM_STEP       = 0x0043f512; // llm_strat_sim_step: the dispatcher's only caller

// Chains (xrefs read 2026-10-18 on /eng/mh.exe, tmp/m1h/plan.md section 0): commit <- marshal only; pathfind <- commit only;
// flood <- pathfind (+ formation / plan_group_route); route_leg_group_and_sort <- marshal; slot_dist_to_ref <- route_leg;
// build_steps <- pathfind; step_check <- walker only; unit_path_detour <- step_check (+ itself).
const site_desc k_site[SITE_COUNT] = {
    /* SITE_CENT_X */ {"cent_x", 0x0041caf2, H_MARSHAL, {R_COMMIT, R_PATHFIND}, 2, MVH_CENT_X},
    /* SITE_CENT_Y */ {"cent_y", 0x0041cb51, H_MARSHAL, {R_COMMIT, R_PATHFIND}, 2, MVH_CENT_Y},
    /* SITE_SLOT_X */ {"slot_x", 0x004cbdb0, H_MARSHAL, {R_ROUTE_LEG, R_SLOT_DIST}, 2, MVH_SLOT_X},
    /* SITE_SLOT_Y */ {"slot_y", 0x004cbdc3, H_MARSHAL, {R_ROUTE_LEG, R_SLOT_DIST}, 2, MVH_SLOT_Y},
    /* SITE_DETOUR */ {"detour", 0x0042172e, H_WALKER, {R_STEP_CHECK, R_DETOUR}, 2, MVH_DETOUR},
    /* SITE_FLOOD_Q */ {"flood_q", 0x00424f62, H_MARSHAL, {R_COMMIT, R_PATHFIND, R_FLOOD}, 3, MVH_FLOOD_Q},
    /* SITE_FLOOD_C */ {"flood_c", 0x00424f7f, H_MARSHAL, {R_COMMIT, R_PATHFIND, R_FLOOD}, 3, MVH_FLOOD_C},
    /* SITE_CORR */ {"corr", 0x0041d06c, H_MARSHAL, {R_COMMIT, R_PATHFIND}, 2, MVH_CORR},
    /* SITE_ARR_COMMIT */ {"arr_commit", 0x0041c9d3, H_MARSHAL, {R_COMMIT}, 1, MVH_ARR_COMMIT},
    /* SITE_ARR_RETRY */ {"arr_retry", 0x00482b5b, H_MARSHAL, {}, 0, MVH_ARR_RETRY},
    /* SITE_MERGE_GOAL */ {"merge_goal", 0x0041cc69, H_MARSHAL, {R_COMMIT, R_PATHFIND}, 2, MVH_MERGE_GOAL},
    /* SITE_BLOCKED */ {"blocked", 0x0047d50d, H_WALKER, {}, 0, MVH_BLOCKED},
    /* SITE_ND */ {"nudge", 0x0047d508, H_WALKER, {}, 0, MVH_ND},
    /* SITE_MK */ {"mk", 0x00482954, H_MARSHAL, {}, 0, MVH_MK},
    /* SITE_MK2 */ {"mk2", 0x004829e2, H_MARSHAL, {}, 0, MVH_MK2},
    /* SITE_RT */ {"route_through", 0x0041c9ce, H_MARSHAL, {R_COMMIT}, 1, MVH_RT},
    /* SITE_SPEED_COMMIT */ {"speed_commit", 0x00482897, H_MARSHAL, {}, 0, MVH_SPEED_COMMIT},
    /* SITE_SPEED_WALK */ {"speed_walk", 0x0047d3d1, H_WALKER, {}, 0, MVH_SPEED_WALK},
    /* SITE_ATK_HEADING */ {"atk_heading", 0x0041ec1d, H_MARSHAL, {R_COMMIT, R_PATHFIND, R_BUILD_STEPS}, 3, -1},
    /* SITE_SQM */ {"squad_merge", 0x0047ebe1, H_SQUAD_MERGE, {}, 0, MVH_SQM},
    /* SITE_PFF */ {"proj_facing", 0x00441424, 0, {}, 0, MVH_PFF},
    /* SITE_ARF_ISSUE_REPOS */ {"arf_issue_repos", 0x0046bfca, 0, {}, 0, MVH_ARF_ISSUE},
    /* SITE_ARF_ISSUE_ALT */ {"arf_issue_alt", 0x0046c24a, 0, {}, 0, MVH_ARF_ISSUE},
    /* SITE_ARF_ISSUE_ENQ */ {"arf_issue_enq", 0x0046c49f, 0, {}, 0, MVH_ARF_ISSUE, false},
    /* SITE_ARF_ISSUE_ATK */ {"arf_issue_atk", 0x0046c6f4, 0, {}, 0, MVH_ARF_ISSUE, false},
    /* SITE_ARF_JIT_REPOS */ {"arf_jit_repos", 0x0046c19e, 0, {}, 0, MVH_ARF_JIT_POINT},
    /* SITE_ARF_JIT_ENQ */ {"arf_jit_enq", 0x0046c648, 0, {}, 0, MVH_ARF_JIT_POINT, false},
    /* SITE_ARF_JIT_ATK */ {"arf_jit_atk", 0x0046c731, 0, {}, 0, MVH_ARF_JIT_POINT, false},
    /* SITE_ARF_JIT_ALT */ {"arf_jit_alt", 0x0046c3f3, 0, {}, 0, MVH_ARF_JIT_OFFSET},
    /* SITE_ARF_JIT_WALK */ {"arf_jit_walk", 0x0047d114, H_WALKER, {}, 0, MVH_ARF_JIT_OFFSET},
    /* SITE_ARF_WALK */ {"arf_walk", 0x0047d016, H_WALKER, {}, 0, MVH_ARF_WALK},
    /* SITE_ARF_FIRE */ {"arf_fire", 0x0048bbce, 0, {}, 0, MVH_ARF_FIRE},
    /* SITE_ARF_REAIM */ {"arf_reaim", 0x00484fc6, H_ATTACK_BUILDING, {}, 0, MVH_ARF_REAIM},
    /* SITE_ARF_STEP */ {"arf_step", 0x004831c0, 0, {}, 0, MVH_ARF_STEP},
    /* SITE_FLOOD_X */ {"flood_exact", 0x00424f3a, H_MARSHAL, {R_COMMIT, R_PATHFIND, R_FLOOD}, 3, MVH_FLOOD_EXACT},
    /* SITE_FLOOD_END */ {"flood_end", 0x00425072, H_MARSHAL, {R_COMMIT, R_PATHFIND, R_FLOOD}, 3, -1},
    /* SITE_FLOOD_BEGIN */ {"flood_begin", 0x00424ee8, H_MARSHAL, {R_COMMIT, R_PATHFIND, R_FLOOD}, 3, -1},
    // mods:M1G 4c. The dispatcher has no table handler above it: retail sim_step calls it, so the retail splice is reached unless libmh
    // CLAIMED sim_step (promoted) AND its dispatch call is bound to the libmh body (row armed) -- chain_reaches_retail over {caller, row}.
    /* SITE_REVERSE */ {"reverse", 0x00466bfe, 0, {R_ORDER_DISPATCH}, 1, MVH_REVERSE, true, F_SIM_STEP},
    /* SITE_TURN */ {"turn", 0x0047d5b4, H_WALKER, {}, 0, MVH_TURN},
    /* SITE_RTM */ {"route_moving", 0x0041c9ce, H_MARSHAL, {R_COMMIT}, 1, MVH_RTM},
    /* SITE_RTR */ {"route_replanning", 0x0041c9ce, H_MARSHAL, {R_COMMIT}, 1, MVH_RTR},
    /* SITE_CAPOK */ {"replan_cap", 0x00482cee, H_MARSHAL, {}, 0, MVH_CAPOK},
    /* SITE_FC */ {"formation", 0x0041c9ce, H_MARSHAL, {R_COMMIT}, 1, MVH_FC},
    // mods:M1BE: the three storage door state handlers (building anim states, not unit states: not table-rooted, the entry claim decides).
    // Rooted at the retail handler in the building TICK2 table (_G_LLM_STRAT_BLDG_TICK2_FUNCS): libmh's register_bldg_type_callbacks fills it with
    // its own thunks, which makes the retail body (and the splice in it) dead code.
    /* SITE_DOOR_SOLDIERS */ {"door_soldiers", 0x00477075, 0x00476e9d, {}, 0, MVH_DH, true, 0, true},
    /* SITE_DOOR_VEHICLES */ {"door_vehicles", 0x00476b81, 0x004769a9, {}, 0, MVH_DH, true, 0, true},
    /* SITE_DOOR_BARRACKS */ {"door_barracks", 0x004767ed, 0x00476627, {}, 0, MVH_DH, true, 0, true},
};

const char *const k_hit_name[MVH_COUNT] = {
    "cent_x",
    "cent_y",
    "slot_x",
    "slot_y",
    "detour",
    "flood_q",
    "flood_c",
    "corr",
    "arr_commit",
    "arr_retry",
    "merge_goal",
    "blocked",
    "nudge",
    "mk",
    "mk2",
    "route_through",
    "speed_commit",
    "speed_walk",
    "squad_merge",
    "proj_facing",
    "arf_issue",
    "arf_jit_pt",
    "arf_jit_off",
    "arf_walk",
    "arf_fire",
    "arf_reaim",
    "arf_step",
    "flood_exact",
    "reverse",
    "turn",
    "route_moving",
    "route_replanning",
    "replan_cap",
    "formation",
    "enter_queue",
    "door_hold",
};

// ---- per-key description ---------------------------------------------------------------------------------------------------
struct key_desc {
    const char *name;
    const char *twin; // "<libmh file>:<function>" when libmh carries the key (reimpl_fixes field of the same name), else nullptr.
                      // Lanes B / C set it when they land a twin -- and put `carrier: "migrated:<key>"` on the manifest rows.
    int sites[13];
    int nsites;
};

const key_desc k_key[KEY_COUNT] = {
    /* KEY_CENTROID_WRAP */
    {"move_centroid_wrap_fix", "sim_group_move_order_pathfind.cpp:group_move_order_pathfind + sim_path_slot_dist.cpp:slot_dist_to_ref", {SITE_CENT_X, SITE_CENT_Y, SITE_SLOT_X, SITE_SLOT_Y}, 4},
    /* KEY_DETOUR_GUARD */ {"move_detour_slot_guard", "sim_unit_path_detour.cpp:unit_path_detour", {SITE_DETOUR}, 1},
    /* KEY_FLOOD_GUARD */ {"move_flood_queue_guard", "sim_map_region_routing.cpp:region_flood_reachable", {SITE_FLOOD_Q}, 1},
    /* KEY_ROUTE_COST_MODE */ {"route_cost_mode", "sim_map_region_routing.cpp:region_flood_reachable", {SITE_FLOOD_C}, 1},
    /* KEY_ROUTE_COST_BASE */ {"route_cost_base", "sim_map_region_routing.cpp:region_flood_reachable", {SITE_FLOOD_C}, 1},
    /* KEY_ROUTE_COST_RING4 */ {"route_cost_ring4", "sim_map_region_routing.cpp:region_flood_reachable", {SITE_FLOOD_C}, 1},
    /* KEY_ROUTE_COST_RING8 */ {"route_cost_ring8", "sim_map_region_routing.cpp:region_flood_reachable", {SITE_FLOOD_C}, 1},
    /* KEY_ROUTE_COST_RING16 */ {"route_cost_ring16", "sim_map_region_routing.cpp:region_flood_reachable", {SITE_FLOOD_C}, 1},
    /* KEY_ROUTE_GAP_WAVE_FULL */ {"route_gap_wave_full", "sim_map_region_routing.cpp:region_flood_reachable", {SITE_FLOOD_C}, 1},
    /* KEY_ROUTE_CORRIDOR_MODE */ {"route_corridor_mode", "sim_group_move_order_pathfind.cpp:group_move_order_pathfind (modes 0/1/3)", {SITE_CORR}, 1},
    /* KEY_BLOCK_WAIT_MS */ {"move_block_wait_ms", "sim_unit_state_move_walker.cpp:unit_state_move_walker", {SITE_BLOCKED}, 1},
    /* KEY_KEEP_SIDESTEP */ {"move_keep_sidestep", "sim_unit_state_move_walker.cpp:unit_state_move_walker", {SITE_BLOCKED}, 1},
    /* KEY_REPATH_COOLDOWN_MS */ {"move_repath_cooldown_ms", "sim_unit_state_move_walker.cpp:unit_state_move_walker", {SITE_BLOCKED}, 1},
    /* KEY_DISTINCT_GOALS */ {"move_distinct_goals", "sim_group_move_order_commit.cpp:group_move_order_commit (the marshal's on-goal stop is the retry site: lane C)", {SITE_ARR_COMMIT, SITE_ARR_RETRY}, 2},
    /* KEY_NEAR_ENOUGH */ {"move_near_enough", "sim_unit_state_group_marshal.cpp:unit_state_group_marshal", {SITE_ARR_RETRY}, 1},
    /* KEY_GROUP_SPEED */ {"move_group_speed", "sim_unit_state_move_walker.cpp:unit_state_move_walker + sim_unit_state_group_marshal.cpp:unit_state_group_marshal", {SITE_SPEED_COMMIT, SITE_SPEED_WALK}, 2},
    /* KEY_NUDGE_IDLE */ {"move_nudge_idle", "sim_unit_state_move_walker.cpp:unit_state_move_walker + sim_unit_state_group_marshal.cpp:unit_state_group_marshal", {SITE_ND, SITE_MK, SITE_MK2}, 3},
    /* KEY_CLAIM_LEADER */ {"move_claim_leader", "sim_unit_state_move_walker.cpp:unit_state_move_walker", {SITE_ND}, 1},
    /* KEY_SWAP_HEADON */ {"move_swap_headon", "sim_unit_state_move_walker.cpp:unit_state_move_walker + sim_unit_state_group_marshal.cpp:unit_state_group_marshal", {SITE_ND, SITE_MK, SITE_MK2}, 3},
    /* KEY_ROUTE_THROUGH_IDLE */ {"move_route_through_idle", "sim_group_move_order_commit.cpp:group_move_order_commit (mode 1)", {SITE_RT}, 1},
    /* KEY_ATTACK_HEADING */ {"move_attack_heading_fix", "sim_path_group_steps.cpp:pathfind_build_steps", {SITE_ATK_HEADING}, 1},
    /* KEY_SQUAD_MERGE */ {"move_squad_merge_fix", "sim_unit_state_squad_merge.cpp:unit_state_squad_merge", {SITE_SQM}, 1},
    /* KEY_MERGE_GOAL */ {"move_merge_goal_fix", "sim_group_move_order_pathfind.cpp:group_move_order_pathfind", {SITE_MERGE_GOAL}, 1},
    /* KEY_ATTACK_RANGE_FOOTPRINT */
    {"attack_range_footprint", "sim_attack_range_footprint.h in issue_unit_attack.cpp / sim_unit_fire_weapon.cpp / ...", {SITE_ARF_ISSUE_REPOS, SITE_ARF_ISSUE_ALT, SITE_ARF_ISSUE_ENQ, SITE_ARF_ISSUE_ATK, SITE_ARF_JIT_REPOS, SITE_ARF_JIT_ENQ, SITE_ARF_JIT_ATK, SITE_ARF_JIT_ALT, SITE_ARF_JIT_WALK, SITE_ARF_WALK, SITE_ARF_FIRE, SITE_ARF_REAIM, SITE_ARF_STEP}, 13},
    /* KEY_PROJECTILE_FACING */ {"projectile_facing_fix", "sim_projectile_tick.cpp:projectile_tick", {SITE_PFF}, 1},
    /* KEY_ROUTE_FLOOD_EXACT */ {"route_flood_exact", "sim_map_region_routing.cpp:region_flood_reachable", {SITE_FLOOD_X, SITE_FLOOD_END, SITE_FLOOD_BEGIN}, 3},
    /* KEY_REVERSE_NOW */ {"move_reverse_now", "sim_order_dispatch.cpp:order_queue_dispatch (retain path)", {SITE_REVERSE}, 1},
    /* KEY_TURN_RATE */ {"move_turn_rate", "sim_unit_state_move_walker.cpp:unit_state_move_walker (turn branch)", {SITE_TURN}, 1},
    /* KEY_ROUTE_THROUGH_MOVING */ {"move_route_through_moving", "sim_group_move_order_commit.cpp:group_move_order_commit (rt_unmask_moving)", {SITE_RTM}, 1},
    /* KEY_ROUTE_THROUGH_REPLANNING */ {"move_route_through_replanning", "sim_group_move_order_commit.cpp:group_move_order_commit (rt_unmask_replanning)", {SITE_RTR}, 1},
    /* KEY_BLOCK_RESET_MOVING */ {"move_block_reset_moving", "sim_unit_state_move_walker.cpp:unit_state_move_walker", {SITE_BLOCKED}, 1},
    /* KEY_REPLAN_DETOUR_CAP */ {"move_replan_detour_cap", "sim_unit_state_move_walker.cpp:unit_state_move_walker + sim_unit_state_group_marshal.cpp:unit_state_group_marshal", {SITE_BLOCKED, SITE_CAPOK}, 2},
    /* KEY_ROUTE_THROUGH_MOVING_COST */ {"move_route_through_moving_cost", "sim_group_move_order_commit.cpp:group_move_order_commit (rt_unmask_moving)", {SITE_RTM}, 1},
    /* KEY_FORMATION_CLICK */ {"move_formation_click", "sim_group_move_order_commit.cpp:group_move_order_commit (formation_after_pathfind; the marshal's on-goal stop is the retry site)", {SITE_FC, SITE_ARR_RETRY}, 2},
    /* KEY_ENTER_QUEUE */ {"move_enter_queue", "sim_unit_state_move_walker.cpp:unit_state_move_walker (the nudge site + the blocked block; sim/move/enter_queue.h)", {SITE_ND, SITE_BLOCKED}, 2},
    /* KEY_STORAGE_DOOR_HOLD */ {"storage_door_hold", "sim_bldg_anim_state_online.cpp:bldg_anim_state_soldiers_h / vehicles_h / barracks_garage_a (door_may_close)", {SITE_DOOR_SOLDIERS, SITE_DOOR_VEHICLES, SITE_DOOR_BARRACKS}, 3},
};

// ---- live state ------------------------------------------------------------------------------------------------------------
int           g_state[SITE_COUNT];     // site_fact::state, set by the seams
bool          g_requested[KEY_COUNT];  // a setter has run for the key
int           g_value[KEY_COUNT];      // ... with this value
int           g_retail[KEY_COUNT];     // ... whose retail default is this
bool          g_pending       = false; // a setter ran since the last report
bool          g_reported_once = false;
int           g_verdict[KEY_COUNT]; // last report's verdict (-1 = none)
int           g_reached[KEY_COUNT]; // last report's reached-site count
uint32_t      g_last_hits[MVH_COUNT];
ULONGLONG     g_last_hit_log_ms = 0;
unsigned long g_rep_gen         = 0; // MH_RunDirGeneration() at the last report

bool site_reached(int s) {
    const site_desc &d = k_site[s];
    if (mh::hook::promoted_owner_of(d.va)) return false; // an entry-claimed function: the claim's owner runs, not this splice
    if (d.root == 0) {                                   // not table-rooted: the install-time entry check was the whole question, plus the caller's claim + the chain's rows
        if (d.caller == 0) return true;
        bool armed[3] = {true, true, true};
        for (int i = 0; i < d.nrows && i < 3; ++i) armed[i] = rebind_row_armed(d.rows[i]);
        return chain_reaches_retail(mh::hook::promoted_owner_of(d.caller) == nullptr, armed, d.nrows);
    }
    bool armed[3] = {true, true, true};
    for (int i = 0; i < d.nrows && i < 3; ++i) armed[i] = rebind_row_armed(d.rows[i]);
    if (d.tick2) return chain_reaches_retail(retail_tick2_dispatched(d.root), armed, d.nrows);
    return chain_reaches_retail(retail_handler_dispatched(d.root), armed, d.nrows);
}

const char *handler_owner(uintptr_t retail_va, int slot) {
    const uint32_t *t = mh::state::ptr<const uint32_t>(mh::state::RID_STRAT_UNIT_STATE_FUNCS);
    if (!t) return "?";
    if (t[slot] == static_cast<uint32_t>(retail_va)) return "retail";
    return t[slot] ? "libmh" : "empty";
}

void report() {
    char b[420];
    // Evidence line: who owns the three table slots the chains hang off, and which rebind rows the chains use are armed.
    wsprintfA(b, "; move carrier: unit-state table walker(0x0f)=%s marshal(0x0a)=%s squad_merge(0x33)=%s; rebind armed: commit=%d pathfind=%d flood=%d slot_dist=%d step_check=%d\n",
              handler_owner(H_WALKER, 0x0f), handler_owner(H_MARSHAL, 0x0a), handler_owner(H_SQUAD_MERGE, 0x33), rebind_row_armed(R_COMMIT) ? 1 : 0,
              rebind_row_armed(R_PATHFIND) ? 1 : 0, rebind_row_armed(R_FLOOD) ? 1 : 0, rebind_row_armed(R_SLOT_DIST) ? 1 : 0, rebind_row_armed(R_STEP_CHECK) ? 1 : 0);
    seam_log(b);
    for (int k = 0; k < KEY_COUNT; ++k) {
        g_verdict[k] = -1;
        g_reached[k] = 0;
        if (!g_requested[k] || g_value[k] == g_retail[k]) continue;
        const key_desc &kd = k_key[k];
        site_fact       f[13];
        for (int i = 0; i < kd.nsites; ++i) {
            const int s  = kd.sites[i];
            f[i].state   = g_state[s];
            f[i].reached = g_state[s] == 1 && site_reached(s);
        }
        // A twin is claimed only if it carries the rule at every site that is displaced (state -2, or carried but unreached).
        bool has_twin = kd.twin != nullptr;
        for (int i = 0; i < kd.nsites; ++i)
            if (!k_site[kd.sites[i]].twin && (f[i].state == -2 || (f[i].state == 1 && !f[i].reached))) has_twin = false;
        const verdict_info vi = key_verdict(f, kd.nsites, has_twin);
        g_verdict[k]          = static_cast<int>(vi.v);
        g_reached[k]          = vi.reached;
        switch (vi.v) {
            case verdict::splice:
                if (vi.reached == vi.sites) wsprintfA(b, "; move carrier [net] %s=%d: retail splice\n", kd.name, g_value[k]);
                else
                    wsprintfA(b, "; move carrier [net] %s=%d: retail splice on %d of %d sites (%d displaced, no twin; %d mismatched/not installed)\n", kd.name,
                              g_value[k], vi.reached, vi.sites, vi.displaced, vi.mismatched + vi.missing);
                break;
            case verdict::twin: wsprintfA(b, "; move carrier [net] %s=%d: libmh twin (%s)\n", kd.name, g_value[k], kd.twin); break;
            case verdict::splice_and_twin:
                wsprintfA(b, "; move carrier [net] %s=%d: retail splice on %d of %d sites + libmh twin (%s)\n", kd.name, g_value[k], vi.reached, vi.sites,
                          kd.twin);
                break;
            case verdict::no_twin: wsprintfA(b, "; move carrier [net] %s=%d: NOT ACTIVE: displaced, no twin\n", kd.name, g_value[k]); break;
            case verdict::mismatched: wsprintfA(b, "; move carrier [net] %s=%d: NOT ACTIVE: splice MISMATCHED\n", kd.name, g_value[k]); break;
            default: wsprintfA(b, "; move carrier [net] %s=%d: NOT ACTIVE: splice not installed\n", kd.name, g_value[k]); break;
        }
        seam_log(b);
    }
    g_reported_once = true;
}

void log_hits(bool force) {
    bool changed = false;
    for (int h = 0; h < MVH_COUNT; ++h)
        if (g_mv_hit[h] != g_last_hits[h]) changed = true;
    if (!changed && !force) return;
    char b[900];
    int  n   = wsprintfA(b, "; move hits");
    bool any = false;
    for (int h = 0; h < MVH_COUNT && n < 820; ++h) {
        g_last_hits[h] = g_mv_hit[h];
        if (g_mv_hit[h] == 0) continue;
        any = true;
        n += wsprintfA(b + n, " %s=%u", k_hit_name[h], g_mv_hit[h]);
    }
    if (!any) n += wsprintfA(b + n, " (none)");
    // A key the report said is NOT carried by the retail splice must not show splice hits: say so loudly.
    for (int k = 0; k < KEY_COUNT && n < 860; ++k) {
        if (g_verdict[k] < 0 || g_reached[k] > 0) continue;
        const int64_t kh = key_hits(static_cast<key_id>(k));
        if (kh > 0) n += wsprintfA(b + n, " CARRIER-CONFLICT:%s(reported not reached, hits=%d)", k_key[k].name, static_cast<int>(kh));
    }
    b[n++] = '\n';
    b[n]   = 0;
    seam_log(b);
}

} // namespace

void note_site(site_id s, int state) {
    if (s >= 0 && s < SITE_COUNT) g_state[s] = state;
}

void request(key_id k, int value, int retail_value) {
    if (k < 0 || k >= KEY_COUNT) return;
    g_requested[k] = true;
    g_value[k]     = value;
    g_retail[k]    = retail_value;
    g_pending      = true;
}

int64_t key_hits(key_id k) {
    if (k < 0 || k >= KEY_COUNT) return -1;
    const key_desc &kd              = k_key[k];
    bool            seen[MVH_COUNT] = {};
    int64_t         sum             = 0;
    bool            any             = false;
    for (int i = 0; i < kd.nsites; ++i) {
        const int h = k_site[kd.sites[i]].hit;
        if (h < 0 || seen[h]) continue;
        seen[h] = true;
        any     = true;
        sum += g_mv_hit[h];
    }
    return any ? sum : -1;
}

const char *key_name(key_id k) { return (k >= 0 && k < KEY_COUNT) ? k_key[k].name : "?"; }
// A patch-manifest row has ONE `carrier` field, so the five route-cost knobs that ride the route_cost_mode row (same site, same twin)
// are carried by that row's `migrated:route_cost_mode`; they report the twin but do not own a manifest carrier of their own.
static bool key_rides_another_row(key_id k) {
    return k == KEY_ROUTE_COST_BASE || k == KEY_ROUTE_COST_RING4 || k == KEY_ROUTE_COST_RING8 || k == KEY_ROUTE_COST_RING16 ||
           k == KEY_ROUTE_GAP_WAVE_FULL || k == KEY_ROUTE_THROUGH_MOVING_COST;
}
bool key_has_twin(key_id k) { return k >= 0 && k < KEY_COUNT && k_key[k].twin != nullptr && !key_rides_another_row(k); }

void on_present() {
    if (!g_pending && !g_reported_once) return;
    const uint32_t *t = mh::state::ptr<const uint32_t>(mh::state::RID_STRAT_UNIT_STATE_FUNCS);
    if (!table_filled(t, 255)) return; // strategic-mode init has not run yet
    // "The first sim step": the table is filled at strategic-mode init but a libmh fill could still follow a retail one, so wait until
    // the game clock has moved (zero until the first step of a fresh game).
    if (*mh::state::ptr<const uint64_t>(mh::state::RID_STRAT_GAME_CLOCK) == 0) return;
    // A new MATCH log folder: say it again THERE -- the rows read one session's mh_net.log. (Hit counters stay cumulative per process.)
    const unsigned long gen = MH_RunDirGeneration();
    if (g_reported_once && gen != g_rep_gen && MH_RunDir_SessionActive()) g_pending = true;
    if (g_pending) {
        g_pending = false;
        g_rep_gen = gen;
        report();
    }
    const ULONGLONG now = GetTickCount64();
    if (now - g_last_hit_log_ms >= 1000) {
        g_last_hit_log_ms = now;
        log_hits(false);
    }
}

} // namespace mh::seams::mv
