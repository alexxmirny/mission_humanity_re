//
// seams/move_carrier.h -- mods:M1H lane A: the HONEST carrier report for the movement / combat keys.
//
// THE BUG THIS REPLACES. Every setter of the [net] improved_movement / group_movement / improved_combat keys used to ask
// `promoted_owner_of(site)` ("is the function JMP-patched at its entry by install_export?") and log "active" when the
// answer was no. libmh does not take over the movement functions that way: it fills the unit-state dispatch table
// (_G_LLM_STRAT_UNIT_STATE_FUNCS, 0x00e15a3c) with its own handlers at strategic-mode init, and its bodies call each other
// through the rebind (MH_LIBMH_BIND). Neither notes anything, so in the libmh configuration every retail splice was dead
// code while the log still said "active".
//
// THE FIX. One report, evaluated LATE (the first present after the table is filled -- the table is filled at strategic-mode
// init, after install_move_proto, so reading it at install time would still say "retail"). For each key the carrier says
// exactly one of:  retail splice | libmh twin | splice + twin | NOT ACTIVE (displaced, no twin) | NOT ACTIVE (MISMATCHED).
//
// A SITE IS REACHED by the retail code iff the retail handler that roots its call chain is still in the dispatch table, OR a
// link of the chain from a libmh root down to the site is UNARMED in the rebind (the call then lands in retail code, and
// everything below a retail function is retail). The pure rule is below so a net_selftest arm can drive it with fake tables.
//
#pragma once
#include <stdint.h>

namespace mh::seams::mv {

// ---- the splice sites (one per patched address; the stubs' hit counters are separate, see mv_hit_id) ------------------------
enum site_id : int {
    SITE_CENT_X,       // 0x0041caf2  pathfind: centroid wrap (x)
    SITE_CENT_Y,       // 0x0041cb51  pathfind: centroid wrap (y)
    SITE_SLOT_X,       // 0x004cbdb0  slot_dist_to_ref: wrap (x)
    SITE_SLOT_Y,       // 0x004cbdc3  slot_dist_to_ref: wrap (y)
    SITE_DETOUR,       // 0x0042172e  unit_path_detour: occupant slot guard
    SITE_FLOOD_Q,      // 0x00424f62  flood_reachable: queue guard
    SITE_FLOOD_C,      // 0x00424f7f  flood_reachable: cost table
    SITE_CORR,         // 0x0041d06c  pathfind: corridor widening
    SITE_ARR_COMMIT,   // 0x0041c9d3  commit: distinct goals
    SITE_ARR_RETRY,    // 0x00482b5b  marshal: near enough
    SITE_MERGE_GOAL,   // 0x0041cc69  pathfind: squad merge goal
    SITE_BLOCKED,      // 0x0047d50d  walker: blocked-unit protocol
    SITE_ND,           // 0x0047d508  walker: nudge / claim / swap
    SITE_MK,           // 0x00482954  marshal: no-path nudge / swap
    SITE_MK2,          // 0x004829e2  marshal: exhausted-path nudge / swap
    SITE_RT,           // 0x0041c9ce  commit: route through idle friends
    SITE_SPEED_COMMIT, // 0x00482897  marshal: group speed (after commit)
    SITE_SPEED_WALK,   // 0x0047d3d1  walker: group speed (averaging)
    SITE_ATK_HEADING,  // 0x0041ec1d  build_steps: attack heading (a runtime-flipped byte, no stub)
    SITE_SQM,          // 0x0047ebe1  squad_merge: state compare
    SITE_PFF,          // 0x00441424  projectile_tick: facing delta
    SITE_ARF_ISSUE_REPOS,
    SITE_ARF_ISSUE_ALT,
    SITE_ARF_ISSUE_ENQ,
    SITE_ARF_ISSUE_ATK, // attack_range_footprint, the issuers
    SITE_ARF_JIT_REPOS,
    SITE_ARF_JIT_ENQ,
    SITE_ARF_JIT_ATK,
    SITE_ARF_JIT_ALT,
    SITE_ARF_JIT_WALK, // ... the aim jitter
    SITE_ARF_WALK,
    SITE_ARF_FIRE,
    SITE_ARF_REAIM,
    SITE_ARF_STEP,      // ... walker / fire / re-aim / group step
    SITE_FLOOD_X,       // 0x00424f3a  flood_reachable: phase 2 replaced by the bucket queue (route_flood_exact)
    SITE_FLOOD_END,     // 0x00425072  flood_reachable: result tail (the cost measurement's end stamp)
    SITE_FLOOD_BEGIN,   // 0x00424ee8  flood_reachable: first instruction after the prologue (the cost measurement's start stamp)
    SITE_REVERSE,       // 0x00466bfe  dispatcher retain path: slide back at once (mods:M1G 4c)
    SITE_TURN,          // 0x0047d5b4  walker turn branch: turn cost x 2^-k (mods:M1G 4c)
    SITE_RTM,           // 0x0041c9ce  the SAME splice as SITE_RT, counted separately (move_route_through_moving)
    SITE_RTR,           // 0x0041c9ce  the SAME splice again, counted separately (move_route_through_replanning, mods:M1DT)
    SITE_CAPOK,         // 0x00482cee  marshal good-path branch: refuse a block-wait detour route (move_replan_detour_cap, mods:M1DT)
    SITE_FC,            // 0x0041c9ce  the SAME splice again: formation from the click runs after the retail plan, inside the bracket (move_formation_click, mods:M1G 4b)
    SITE_DOOR_SOLDIERS, // 0x00477075  soldiers_h anim state, case 2 (door open): keep it open for an entering friend (storage_door_hold, mods:M1BE)
    SITE_DOOR_VEHICLES, // 0x00476b81  vehicles_h anim state, case 2
    SITE_DOOR_BARRACKS, // 0x004767ed  barracks_garage_a anim state, case 2
    SITE_COUNT
};

} // namespace mh::seams::mv

// ---- the stubs' hit counters (a stub bumps its counter when its DECISION path runs: key on, relevant case) --------------
enum mv_hit_id : int {
    MVH_CENT_X,
    MVH_CENT_Y,
    MVH_SLOT_X,
    MVH_SLOT_Y,
    MVH_DETOUR,
    MVH_FLOOD_Q,
    MVH_FLOOD_C,
    MVH_CORR,
    MVH_ARR_COMMIT,
    MVH_ARR_RETRY,
    MVH_MERGE_GOAL,
    MVH_BLOCKED,
    MVH_ND,
    MVH_MK,
    MVH_MK2,
    MVH_RT,
    MVH_SPEED_COMMIT,
    MVH_SPEED_WALK,
    MVH_SQM,
    MVH_PFF,
    MVH_ARF_ISSUE,
    MVH_ARF_JIT_POINT,
    MVH_ARF_JIT_OFFSET,
    MVH_ARF_WALK,
    MVH_ARF_FIRE,
    MVH_ARF_REAIM,
    MVH_ARF_STEP,
    MVH_FLOOD_EXACT,
    MVH_REVERSE,
    MVH_TURN,
    MVH_RTM,
    MVH_RTR,
    MVH_CAPOK,
    MVH_FC,
    MVH_EQ, // mods:M1BE move_enter_queue: an entering unit waited behind an entering friend
    MVH_DH, // mods:M1BE storage_door_hold: an open door was kept open
    MVH_COUNT
};

// The counters themselves. extern "C" so the naked stubs can `inc dword ptr [g_mv_hit + 4*MVH_X]`.
extern "C" uint32_t g_mv_hit[MVH_COUNT];

namespace mh::seams::mv {

// ---- the keys ------------------------------------------------------------------------------------------------------------
enum key_id : int {
    KEY_CENTROID_WRAP,
    KEY_DETOUR_GUARD,
    KEY_FLOOD_GUARD,
    KEY_ROUTE_COST_MODE,
    KEY_ROUTE_COST_BASE,
    KEY_ROUTE_COST_RING4,
    KEY_ROUTE_COST_RING8,
    KEY_ROUTE_COST_RING16,
    KEY_ROUTE_GAP_WAVE_FULL,
    KEY_ROUTE_CORRIDOR_MODE,
    KEY_BLOCK_WAIT_MS,
    KEY_KEEP_SIDESTEP,
    KEY_REPATH_COOLDOWN_MS,
    KEY_DISTINCT_GOALS,
    KEY_NEAR_ENOUGH,
    KEY_GROUP_SPEED,
    KEY_NUDGE_IDLE,
    KEY_CLAIM_LEADER,
    KEY_SWAP_HEADON,
    KEY_ROUTE_THROUGH_IDLE,
    KEY_ATTACK_HEADING,
    KEY_SQUAD_MERGE,
    KEY_MERGE_GOAL,
    KEY_ATTACK_RANGE_FOOTPRINT,
    KEY_PROJECTILE_FACING,
    KEY_ROUTE_FLOOD_EXACT,
    KEY_REVERSE_NOW,
    KEY_TURN_RATE,
    KEY_ROUTE_THROUGH_MOVING,
    KEY_ROUTE_THROUGH_REPLANNING,
    KEY_BLOCK_RESET_MOVING,
    KEY_REPLAN_DETOUR_CAP,
    KEY_ROUTE_THROUGH_MOVING_COST,
    KEY_FORMATION_CLICK,
    KEY_ENTER_QUEUE,
    KEY_STORAGE_DOOR_HOLD,
    KEY_COUNT
};

// ---- the PURE rules (header-only: net_selftest drives them with fake tables) ----------------------------------------------

// Is `va` one of the `slots` entries of a dispatch table?
inline bool table_has(const uint32_t *table, int slots, uint32_t va) {
    if (!table || va == 0) return false;
    for (int i = 0; i < slots; ++i)
        if (table[i] == va) return true;
    return false;
}

// Is the table filled yet? (strategic-mode init writes the walker's and the marshal's slots, 0x0f and 0x0a; BSS reads 0 before.)
inline bool table_filled(const uint32_t *table, int slots) {
    return table && slots > 0x0f && table[0x0f] != 0 && table[0x0a] != 0;
}

// A site below `root_retail` (true = the retail handler is dispatched) with a chain of rebind rows (true = the row is armed):
// reached by retail code iff the root is retail, or a link of the chain is unarmed.
inline bool chain_reaches_retail(bool root_retail, const bool *row_armed, int rows) {
    if (root_retail) return true;
    for (int i = 0; i < rows; ++i)
        if (!row_armed[i]) return true;
    return false;
}

// Per-site facts the key verdict is made of. state: 1 carried (patched), -1 MISMATCHED, -2 DISPLACED (entry claimed), 0 not installed.
struct site_fact {
    int  state;
    bool reached; // meaningful for state 1: the retail code at this site runs in this configuration
};

enum class verdict : int { splice,
                           twin,
                           splice_and_twin,
                           no_twin,
                           mismatched,
                           not_installed };

struct verdict_info {
    verdict v;
    int     sites, reached, displaced, mismatched, missing;
};

inline verdict_info key_verdict(const site_fact *f, int n, bool has_twin) {
    verdict_info r{verdict::not_installed, n, 0, 0, 0, 0};
    for (int i = 0; i < n; ++i) {
        switch (f[i].state) {
            case 1:
                if (f[i].reached) ++r.reached;
                else ++r.displaced;
                break;
            case -2: ++r.displaced; break;
            case -1: ++r.mismatched; break;
            default: ++r.missing; break;
        }
    }
    if (r.reached > 0) r.v = (r.displaced > 0 && has_twin) ? verdict::splice_and_twin : verdict::splice;
    else if (r.displaced > 0) r.v = has_twin ? verdict::twin : verdict::no_twin;
    else if (r.mismatched > 0) r.v = verdict::mismatched;
    else r.v = verdict::not_installed;
    return r;
}

// ---- the live side (move_carrier.cpp) ----------------------------------------------------------------------------------------
// The retail unit-state handler `va` is in the live dispatch table (false until the table is filled).
bool retail_handler_dispatched(uintptr_t handler_va);
// mods:M1BE: the retail building anim-state handler `handler_va` is in the live building tick2 table (false until libmh or the game filled it).
bool retail_tick2_dispatched(uintptr_t handler_va);
// The rebind row (a mh::rebind::ROW_* value) is armed. Absent libmh: false (retail), which is correct.
bool rebind_row_armed(int row);

// Seams call this once per patched site right after installing it (state codes as site_fact::state).
void note_site(site_id s, int state);
// Setters call this: the value the host asked for and the retail default of that key (a key at its default logs nothing).
void request(key_id k, int value, int retail_value);
// Per-frame (net_lockstep on_present): the late report + the throttled hit counters.
void on_present();
// The sum of a key's stub counters (-1 when the key has no counter, e.g. a runtime-flipped byte).
int64_t     key_hits(key_id k);
const char *key_name(key_id k);
// Does libmh carry the key (a twin)? Must agree with the manifest: some row of tools/data/dll_patch_manifest.json carries
// `carrier: "migrated:<key_name>"` exactly when this is true (net_selftest `gameplaytest` checks it).
bool key_has_twin(key_id k);

} // namespace mh::seams::mv

// Increment a hit counter from C++.
inline void mv_hit(mv_hit_id h) { ++g_mv_hit[h]; }
