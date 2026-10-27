//
// seams/move_proto_nudge.cpp -- mods:M1N, the "units make room for each other" movement prototypes (movement plan 3e).
//
//   [net] move_nudge_idle  (3e-1)  a unit whose next tile holds an IDLE friendly ground unit asks it to step to a free neighbour.
//   [net] move_claim_leader (3e-2) a follower takes the tile of a friendly leader that is TURNING in place (it stands settled on the
//                                  tile, its next step needs a turn first) instead of waiting behind it.
//   [net] move_swap_headon (3e-3)  two friendly units that meet head-on (each one's next step is the other's tile) walk through each
//                                  other's tile: both take their next step at once. No sidestep, no replan.
//
// All three are ONE always-installed, runtime-gated call-splice (the M1X carrier pattern, see install_move_proto_blocked): the CALL
// to llm_strat_path_step_check_and_request_detour (0x0049a23d) inside llm_strat_unit_state_move_walker (0x0047d508, 5 bytes
// E8 30 CD 01 00) is retargeted at a stub. The stub asks nd_decide(); -1 = run the retail function unchanged (tail-jump, so every
// key at 0 is byte-for-byte retail behaviour), otherwise nd_decide's value is the function's return value (0 = "wait for the
// occupant": the walker adds 0.05 s to the unit's activity clock and retries on its next tick, the existing M1X block at 0x0047d50d
// then applies move_block_wait_ms to that wait). Retail's alternative for these blockers is the detour request (a sidestep spliced
// into the path and an immediate replan), which these keys replace.
//
// The call's register contract is __watcall: EAX = source x, EDX = source y, EBX = destination x, ECX = destination y (the tile the
// unit is standing on, and the tile it wants). Both tiles hold units (tile_objects[].building = the unit's index, class_owner low
// nibble = its player, high nibble 0x80).
//
// ONE-PLAYER RULE: "friendly" means the SAME player. Allies and enemies keep the retail behaviour (enemies are never nudged,
// claimed or swapped). The AI's units are covered like the human's (the AI's idle units are nudged too).
//
// ---- 3e-1 NUDGE ----------------------------------------------------------------------------------------------------------
// The blocker B is IDLE when state == 1 (STOP_TO_DEFAULT, the ground default op code) and no order is queued for it. It is sent
// through the game's own one-tile move: state 0x36 (llm_strat_unit_state_step_adjacent 0x00485bab), whose goal is the neighbour
// tile. That state tests the goal is adjacent, passable and unoccupied, builds a one-leg path (path_make_single_step) and hands
// over to the walker (state 0xf, order 1); at the end of the leg the retail path-end rule stops the unit (STOP_TO_DEFAULT). So
// the nudged unit turns toward the tile, steps (its old tile is released the moment the step starts) and rests there. No new
// state, no new unit state value. The neighbour is chosen deterministically: among the free neighbours (passable != 0, no
// occupant) the one needing the fewest facing notches from B's current facing, perpendicular to the walker's leg first on ties, the
// tile straight ahead on the walker's route only as a last resort. B's activity clock is pulled back to the game clock if it is
// ahead, so a sleeping unit reacts at once. While B is in state 0x36 (the nudge not yet started) the walker just waits.
//
// ---- 3e-3 SWAP / 3e-2 CLAIM: ONE EXCHANGE -------------------------------------------------------------------------------
// The exchange is the walker's own step done for TWO units in the same tick: each unit's old tile is released and its new tile
// claimed (tile_objects building/class_owner, passable 5 = "a unit is moving into this tile", origin_tile_was_passable handed over
// crosswise because each tile's underlying value is held by the unit standing on it), x/y updated, move_microstep = 0, retry = 0,
// the fog of war moved (the game's own remove_sight / UpdateFoWPlus), the soldier idle-wander flag cleared. From then on each
// unit slides into its new tile through its own 31 microsteps (get_coords draws it from the old tile toward the new one along its
// facing), and at the last microstep settles (passable 0). The two units therefore walk THROUGH each other's tile over the normal
// stepping time; the logical claim of both tiles is the existing mid-step "passable 5" window, never a tile shared beyond it, and
// there is never a one-tick position swap.
//   SWAP  both units are walking (state 0xf), settled, no order queued, and each one's next path step is the other's tile; the
//         other one already FACES that step (its facing_target equals the leg heading). If it is still turning the walker waits
//         for it (the other one's own blocked step would do the exchange too). Both paths stay valid (each unit just takes the
//         step it wanted), so the run lengths of both current legs drop by one, like a normal step.
//   CLAIM the leader L is walking (0xf), settled, no order queued, retry byte 0 (not waiting for its own blocker) and its next
//         step needs a TURN (facing_target != the leg heading) toward a tile that is not the follower's; the follower F's current
//         leg has at least two steps left (F goes straight on, so it does not itself become the turning blocker -- this is the
//         guard against two units leapfrogging each other forever). F steps into L's tile; L is moved to F's old tile and gets
//         ONE new leg in front of its path (heading = F's heading, run 1: back onto the tile F is leaving), the rest of its path
//         unchanged, so it simply follows F instead of having its path invalidated. L is turned to face its slide direction.
//         Needs room for one more path entry (the buffer has 300). Costs F nothing, L loses the turn it was doing.
//
// DETERMINISM: integer arithmetic only (no x87/SSE in the stub: the doubles that are compared are compared as raw bits), units
// handled in the order the walker meets them, fixed neighbour scan order, no clock, no rand. Writes only fields the retail step
// writes (unit record, tile_objects, passable, the path buffer, the fog). No new persistent state, no layout change.
// If libmh owns move_walker the site is DISPLACED and the libmh twin (sim/move/nudge.h, sim_unit_state_move_walker.cpp /
// sim_unit_state_group_marshal.cpp) carries the keys; the late carrier report (move_carrier.cpp) says which.
//
// ---- 3e-4 THE MARSHAL NO-PATH SITE (field fix, user playtest rc10, 2026-10-16) -------------------------------------------------
// The two splices above only run for a unit that is WALKING. In a one-tile corridor the wave planner cuts a unit's route short at an
// idle friend's tile; the unit then sits in group_marshal (state 0x0a) with no path, retrying every 2 s until it gives up, and the
// friend is never asked to move. Two jump-splices into group_marshal's per-member "no path" branches (ADDR_MK_SITE: slot 0xff;
// ADDR_MK2_SITE: path exhausted / first step blocked, right after the slot was freed) ask mk_decide(player, member): the same two
// keys, the same helpers (pick_nudge_tile, do_exchange). See the block comment above mk_decide.
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <cstring>

#include "addr/mh_calls.gen.h"   // mh::call::llm_strat_fow_remove_sight / map_fow_UpdateFoWPlus / llm_strat_unit_soldiers_set_heading
#include "addr/mh_regions.gen.h" // mh::state::live(), RID_*
#include "addr/mh_structs.gen.h"
#include "hook/patch.h"     // patch_bytes_guarded
#include "hook/promoted.h"  // promoted_owner_of
#include "caps/caps.h"      // mh::caps::rec / g: the unit + soldier arrays the game addresses (PT-CAP8)
#include "caps/path_pool.h" // PT-CAP9-U4: the relocated path pool
#include "seams/net_internal.h"
#include "seams/heat_ui.h"          // mods:M1WL v3: the snapshot / traffic / route reads of the ImGui "Stream heat" section (display only)
#include "desync/replay_record.h"   // replay_rec::current_step: the sim step of a plan, for the snapshot
#include "seams/move_carrier.h"     // mods:M1H: site states, requested values, stub hit counters
#include "sim/move/nudge.h"         // mods:M1H lane C: the decision (3e-1..3e-4), shared with the libmh twin
#include "sim/move/queue_flank.h"   // mp:U75: the flank rule + depth cap of move_queue_flank_guard, shared with the libmh twins
#include "sim/move/route_through.h" // mods:M1H lane B: move_route_through_idle's rule (rt_unmask / rt_restore), shared with the libmh twin
#include "sim/move/stream_heat.h"   // mods:M1WL v2: move_stream_heat's rule (sh_open / sh_close), shared with the libmh twin
#include "sim/move/blocked.h"       // mods:M1DT: blocker_info for the walker block site (move_proto_blocked.cpp)
#include "sim/move/replan_cap.h"    // mods:M1DT fix 4b: the marshal check (mv_cap_refuse below)

#pragma comment(lib, "user32.lib") // wsprintfA

namespace {

using mh::game::mh_cfg_final_struct_Unit;
using mh::game::mh_llm_strat_crew_soldier;
using mh::game::mh_llm_strat_map_geom;
using mh::game::mh_map_object_unit;
using mh::game::mh_map_tile_object_data;

constexpr uintptr_t ADDR_ND_CALL   = 0x0047d508; // llm_strat_unit_state_move_walker: CALL llm_strat_path_step_check_and_request_detour
constexpr uintptr_t FN_STEP_CHECK  = 0x0049a23d; // llm_strat_path_step_check_and_request_detour (__watcall EAX,EDX,EBX,ECX; returns EAX)
constexpr uintptr_t ADDR_DIR_DELTA = 0x0051de40; // _G_LLM_MAP_DIR_STEP_DELTAS: int8 {dx, dy} per 24-compass heading
constexpr uintptr_t ADDR_CLOCK     = 0x005d0198; // _G_LLM_STRAT_GAME_CLOCK (double)
const uint8_t       ND_EXPECT[5]   = {0xE8, 0x30, 0xCD, 0x01, 0x00};

// the marshal no-path site (llm_strat_unit_state_group_marshal 0x00482451): the `MOVZX EAX,word [_G_LLM_STRAT_CUR_PLAYER]` that opens the
// "member has no path" branch (CMP path_slot_id,0xff / JNZ just above it), 7 bytes; the stub gets (player, [EBP-0x48] = the member's unit
// index), returns 1 = handled (jump to the loop's per-member continue 0x004828ab, skipping the retail clock bump), 0 = run the displaced
// MOVZX and carry on at 0x0048295b.
constexpr uintptr_t ADDR_MK_SITE = 0x00482954;
constexpr uintptr_t ADDR_MK_RET  = 0x0048295b;
constexpr uintptr_t ADDR_MK_SKIP = 0x004828ab;
const uint8_t       MK_EXPECT[7] = {0x0F, 0xB7, 0x05, 0x44, 0x81, 0xE5, 0x00};
// the second site: the "path exhausted or first step blocked" branch, right after its llm_strat_path_free_slot call (the unit's slot is
// 0xff again from here): `MOVZX EDX,word [_G_LLM_STRAT_CUR_PLAYER]` (0F B7 15 44 81 E5 00), rejoin at 0x004829e9. Same decision; a
// unit the planner shortened to a zero-length path lands here, not at the first site, and counts retries toward giving up.
constexpr uintptr_t ADDR_MK2_SITE  = 0x004829e2;
constexpr uintptr_t ADDR_MK2_RET   = 0x004829e9;
const uint8_t       MK2_EXPECT[7]  = {0x0F, 0xB7, 0x15, 0x44, 0x81, 0xE5, 0x00};
constexpr uintptr_t ADDR_CUR_INDEX = 0x00e58142; // _G_LLM_STRAT_CUR_INDEX (uint16)

// the route planner site (3e-5): the CALL llm_strat_group_move_order_pathfind (0x0041ca37; __watcall EAX = mode, EDX = target mask, no stack
// arguments, void) in llm_strat_group_move_order_commit (0x0041c86c), its only caller; 5 bytes E8 64 00 00 00.
constexpr uintptr_t ADDR_RT_CALL = 0x0041c9ce;
constexpr uintptr_t FN_PATHFIND  = 0x0041ca37;
const uint8_t       RT_EXPECT[5] = {0xE8, 0x64, 0x00, 0x00, 0x00};

constexpr uint16_t ST_IDLE = 0x01, ST_MARSHAL = 0x0a, ST_HOP = 0x0c, ST_WALK = 0x0f, ST_STEP_ADJACENT = 0x36;
constexpr int32_t  MS_SETTLED   = 0x1f;
constexpr int      PATH_ENTRIES = 300;

int g_rt_state  = 0; // the route-planner splice: same codes as g_nd_state
int g_mk2_state = 0;
int g_mk_state  = 0; // the marshal splice: same codes as g_nd_state
int g_nd_state  = 0; // 1 carried, -1 MISMATCHED, -2 DISPLACED, 0 not installed
int g_qf_state  = 0; // the queue-advance search splice (mp:U75): same codes

const char *nd_state_name(int s) {
    return s == 1 ? "carried" : (s == -1 ? "the splice MISMATCHED" : (s == -2 ? "DISPLACED (libmh owns move_walker; its twin carries the keys, see the carrier line)" : "not installed"));
}

// ---- state access (live bases: the hosted exe or the standalone host's own allocation) ---------------------------------------
mh_map_object_unit *unit_rec(uint32_t player, int32_t idx) {
    const auto &u = mh::caps::g(mh::caps::UNITS);
    if (player >= u.players || idx < 0 || (uint32_t)idx >= u.limit) return nullptr;
    return mh::caps::rec<mh_map_object_unit>(mh::caps::UNITS, player, (uint32_t)idx);
}
const mh_cfg_final_struct_Unit *cfg_of(const mh_map_object_unit &u) {
    const auto *c = reinterpret_cast<const mh_cfg_final_struct_Unit *>(mh::state::live().base[mh::state::RID_UNIT]);
    return c ? c + u.unit_proto_id : nullptr;
}
mh_map_tile_object_data *tile_of(uint32_t x, uint32_t y) {
    return reinterpret_cast<mh_map_tile_object_data *>(mh::state::live().base[mh::state::RID_TILE_OBJECTS]) + (((x & 0xffu) << 8) | (y & 0xffu));
}
uint8_t *pass_of(uint32_t x, uint32_t y) {
    return reinterpret_cast<uint8_t *>(mh::state::live().base[mh::state::RID_PASSABLE]) + (((x & 0xffu) << 8) | (y & 0xffu));
}
// One slot's path entries: {heading, run_length} pairs, 300 per slot. The LIVE pool (relocated: the registry's base is only the
// slots-0..99 mirror, and a write there is lost at the next gather).
uint8_t *path_of(uint32_t player, uint32_t slot) { return mh::caps::path::waypoint(player, slot, 0); }

uint64_t raw_bits(const double &d) {
    uint64_t v;
    memcpy(&v, &d, sizeof(v));
    return v;
}
bool unit_alive(const mh_map_object_unit &u) { return (raw_bits(u.energy) >> 63) == 0 && (raw_bits(u.energy) << 1) != 0; }

// The retail splice's WORLD for the shared decision (mh::sim::mvc, sim/move/nudge.h): live game memory + the game's own functions.
struct splice_world {
    mh_map_object_unit                   *unit(uint32_t player, int32_t idx) { return unit_rec(player, idx); }
    const mh_cfg_final_struct_Unit       *cfg(const mh_map_object_unit &u) { return cfg_of(u); }
    mh_map_tile_object_data              &tile(uint32_t x, uint32_t y) { return *tile_of(x, y); }
    uint8_t                              &pass(uint32_t x, uint32_t y) { return *pass_of(x, y); }
    mh::game::mh_llm_strat_path_waypoint *path(uint32_t player, uint32_t slot) {
        return reinterpret_cast<mh::game::mh_llm_strat_path_waypoint *>(path_of(player, slot));
    }
    void dir_delta(int h, int *dx, int *dy) {
        const int8_t *t = reinterpret_cast<const int8_t *>(ADDR_DIR_DELTA);
        *dx             = t[h * 2];
        *dy             = t[h * 2 + 1];
    }
    uint32_t                   mask_x() { return reinterpret_cast<const mh_llm_strat_map_geom *>(mh::state::live().base[mh::state::RID_GENERAL])->width_mask; }
    uint32_t                   mask_y() { return reinterpret_cast<const mh_llm_strat_map_geom *>(mh::state::live().base[mh::state::RID_GENERAL])->height_mask; }
    const void                *game_clock() { return reinterpret_cast<const void *>(ADDR_CLOCK); }
    mh_llm_strat_crew_soldier *soldier(uint32_t player, uint32_t si) {
        if (si >= mh::caps::g(mh::caps::SOLDIERS).limit) return nullptr;
        return mh::caps::rec<mh_llm_strat_crew_soldier>(mh::caps::SOLDIERS, player, si);
    }
    uint16_t cur_index() { return *reinterpret_cast<const uint16_t *>(ADDR_CUR_INDEX); }

    void    fow_remove_sight(uint32_t player, int32_t x, int32_t y, uint8_t sight) { mh::call::llm_strat_fow_remove_sight(player, x, y, sight); }
    void    fow_update(uint32_t player, uint32_t x, uint32_t y, uint8_t sight) { mh::call::map_fow_UpdateFoWPlus(player, x, y, sight); }
    void    soldiers_set_heading(uint16_t player, int32_t idx, uint8_t h) { mh::call::llm_strat_unit_soldiers_set_heading(player, idx, h); }
    int32_t make_single_step(uint32_t player, int32_t idx) { return mh::call::llm_strat_path_make_single_step(player, idx); }
    void    free_slot(uint16_t player, int32_t idx) { mh::call::llm_strat_path_free_slot(player, idx); }
    void    set_order_param(int32_t player, int32_t idx, int16_t param) { mh::call::llm_unit_set_order_param(player, idx, param); }
    void    set_state_of(int32_t player, int32_t idx, int16_t state) { mh::call::llm_strat_unit_set_state_of(player, idx, state); }
    void    notify_status(uint32_t player, int32_t idx, uint32_t code) { mh::call::llm_strat_unit_notify_status(player, idx, code); }

    // One log line per event (the header caps each kind at 200 per run): the evidence that a key fired, and where.
    void note(const char *what, uint32_t player, int32_t ai, int32_t bi, uint32_t sx, uint32_t sy, uint32_t dx, uint32_t dy, int n) {
        char b[200];
        wsprintfA(b, "; M1N %s p%u unit %d at %u,%u / unit %d at %u,%u (%d)\n", what, player, (int)ai, sx, sy, (int)bi, dx, dy, n);
        seam_log(b);
    }
    void hit_nd() { mv_hit(MVH_ND); } // the walker's meeting decision runs (a key of this site is on)
    void hit_eq() { mv_hit(MVH_EQ); } // mods:M1BE move_enter_queue: an entering unit waited behind an entering friend
    void hit_mk() {}                  // the marshal stubs count in asm (mk_stub / mk2_stub)
    void hit_mk2() {}
};

} // namespace

// ---- the decision --------------------------------------------------------------------------------------------------------------
extern "C" {
int                             g_nd_nudge   = 0;             // [net] move_nudge_idle in force
int                             g_nd_claim   = 0;             // [net] move_claim_leader in force
int                             g_nd_swap    = 0;             // [net] move_swap_headon in force
int                             g_qf_req     = 0;             // [net] move_queue_flank_guard in force (mp:U75; the stub tests it, the nudge decisions pass it on)
uintptr_t                       g_qf_ret     = 0x004d7f42;    // qf_stub: the first instruction after the displaced prologue
int                             g_eq_on      = 0;             // [net] move_enter_queue in force (mods:M1BE; also read by move_proto_blocked.cpp)
uintptr_t                       g_nd_orig_fn = FN_STEP_CHECK; // the stub's tail-jump target
uintptr_t                       g_mk_ret     = ADDR_MK_RET;   // mk_stub: continue the retail clock-bump branch
uintptr_t                       g_mk_skip    = ADDR_MK_SKIP;  // mk_stub: handled, next member
uintptr_t                       g_mk2_ret    = ADDR_MK2_RET;  // mk2_stub: continue the retry branch
int                             g_rt_on      = 0;             // [net] move_route_through_idle in force
int                             g_rtm_on     = 0;             // [net] move_route_through_moving in force (mods:M1N 3e-6)
int                             g_rtm_cost   = 0;             // [net] move_route_through_moving_cost: the cost level (sim/move/route_through.h rt_move_cost_bits)
int                             g_rtr_on     = 0;             // [net] move_route_through_replanning in force (mods:M1DT fix 2)
uintptr_t                       g_rt_orig_fn = FN_PATHFIND;   // rt_stub: the wave planner it wraps
int                             g_fc_on      = 0;             // [net] move_formation_click in force (mods:M1G 4b): 0 off, 1..2 = old keep rule with N - 1 rings of slack, 3 = the formation moves as one
int                             g_fc_handled = 0;             // the formation planned the commit that just ran (read once by the 2a stub, move_proto_arrival.cpp)
int                             g_sh_on      = 0;             // [net] move_stream_heat in force (mods:M1WL v2)
int                             g_sh_group   = 0;             // the commit that is planning: its move_group_id and is_plain_move_flag (set by rt_stub off commit's frame, read by rt_unmask; 2a's re-run reuses them)
uint32_t                        g_sh_plain   = 0;
mh::sim::move::sh_params        g_sh_p;    // mods:M1WL v3: the tuning keys in force (cost / window / min mates / scale), set by mh_set_move_stream_heat_params
mh::sim::move::sh_retrace_stats g_sh_rt;   // mods:M1WL v4: what the re-trace did (display + the log line; not sim state)
mh::sim::move::sh_margin_stats  g_sh_mg;   // mods:M1WL v5: what the margin rule did (display + the log line; not sim state)
mh::heatui::snapshot            g_sh_snap; // mods:M1WL v3: the last priced plan, for the ImGui read-outs and the "last plan" heat map (display only, not sim state)
}
int mv_fc_run(int32_t group_id, uint32_t mode, uint32_t target, const void *undo); // move_proto_arrival.cpp: the formation over live memory

// returns -1 = run the retail function; else its return value (0 = "wait for the occupant")
// The rule is mh::sim::mvc::nd_decide (sim/move/nudge.h): the same code the libmh twin runs.
static int __cdecl nd_decide(uint32_t sx, uint32_t sy, uint32_t dx, uint32_t dy) {
    if (!(g_nd_nudge | g_nd_claim | g_nd_swap | g_eq_on)) return -1;
    splice_world w;
    return mh::sim::mvc::nd_decide(w, mh::sim::mvc::nudge_keys{g_nd_nudge != 0, g_nd_claim != 0, g_nd_swap != 0, g_eq_on != 0, g_qf_req != 0}, sx, sy, dx, dy);
}

// clang-format off
__declspec(naked) static void nd_stub() {
    __asm {
        pushad
        push  ecx                               // destination y
        push  ebx                               // destination x
        push  edx                               // source y
        push  eax                               // source x
        call  nd_decide
        add   esp, 16
        cmp   eax, -1
        je    nd_retail
        mov   dword ptr [esp + 28], eax         // the result rides out in the saved EAX slot
        popad
        ret
    nd_retail:
        popad
        jmp   dword ptr [g_nd_orig_fn]          // the retail function sees the registers it was called with
    }
}
// clang-format on

// ---- the group-marshal no-path site (field bug rc10, 2026-10-16) ---------------------------------------------------------------
// A unit whose route the wave planner cut short at a friend's tile ends its trip in state 0x0a with NO path (slot 0xff): retail adds
// 0.05 s to its clock and tries again, forever (until its order times out). The walker's blocked-step splice above never runs, because
// the unit is not walking. mk_decide() looks at the tile the unit wants next (one step toward its goal, wrapped) and, for a friendly
// ground unit standing on it:
//   * IDLE friend, a free neighbour other than straight ahead  -> the same nudge as the walker case (state 0x36); the member retries.
//   * IDLE friend, no sidestep (a one-tile corridor)           -> swap: the friend takes the member's tile and the member the friend's.
//   * friend that is itself marshal-stuck and wants OUR tile, or walking with its next step onto our tile (head-on)  -> swap.
// The swap is the walker's own exchange (do_exchange): each unit gets a one-leg path made by the game's own path_make_single_step
// (its goal set to the neighbour tile for that call), both are handed to the walker (0xf for the ticking unit, 0xc = enter_class_move
// for the others, exactly as the retail success path does), then the tiles are traded. When the leg ends a unit that is not at its
// goal goes back to marshal (walker path-end rule) and replans from its new tile; an idle friend has goal := the tile it was moved
// to and order := STOP_TO_DEFAULT, so it ends idle there.
// returns 1 = handled (skip the retail clock bump), 0 = run retail
// The rule is mh::sim::mvc::mk_decide (sim/move/nudge.h): the same code the libmh twin runs.
static int __cdecl mk_decide(uint32_t player, uint32_t idx) {
    if (!(g_nd_nudge | g_nd_swap)) return 0;
    splice_world w;
    return mh::sim::mvc::mk_decide(w, mh::sim::mvc::nudge_keys{g_nd_nudge != 0, g_nd_claim != 0, g_nd_swap != 0, false, g_qf_req != 0}, player, idx) ? 1 : 0;
}

// clang-format off
__declspec(naked) static void mk_stub() {
    __asm {
        pushad
        cmp   g_nd_nudge, 0                     // mods:M1H hit counter: the decision runs only with nudge or swap on
        jne   mk_cnt
        cmp   g_nd_swap, 0
        je    mk_nocnt
    mk_cnt:
        inc   dword ptr [g_mv_hit + 4 * MVH_MK]
    mk_nocnt:
        push  dword ptr [ebp - 0x48]            // the member's unit index (the marshal's loop local)
        movzx eax, word ptr ds:[0x00e58144]     // _G_LLM_STRAT_CUR_PLAYER
        push  eax
        call  mk_decide
        add   esp, 8
        test  eax, eax
        jnz   mk_done
        popad
        movzx eax, word ptr ds:[0x00e58144]     // the displaced instruction
        jmp   dword ptr [g_mk_ret]
    mk_done:
        popad
        jmp   dword ptr [g_mk_skip]
    }
}

__declspec(naked) static void mk2_stub() {
    __asm {
        pushad
        cmp   g_nd_nudge, 0
        jne   mk2_cnt
        cmp   g_nd_swap, 0
        je    mk2_nocnt
    mk2_cnt:
        inc   dword ptr [g_mv_hit + 4 * MVH_MK2]
    mk2_nocnt:
        push  dword ptr [ebp - 0x48]
        movzx eax, word ptr ds:[0x00e58144]
        push  eax
        call  mk_decide
        add   esp, 8
        test  eax, eax
        jnz   mk2_done
        popad
        movzx edx, word ptr ds:[0x00e58144]     // the displaced instruction
        jmp   dword ptr [g_mk2_ret]
    mk2_done:
        popad
        jmp   dword ptr [g_mk_skip]
    }
}
// clang-format on


// ---- 3e-5 THE ROUTE PLANNER: idle friends are not walls (field fix, user playtest rc11, 2026-10-17) -------------------------------
// The wave planner (llm_strat_group_move_order_pathfind: region flood, route trace, "nearest reachable tile" fallback, per-member
// walks) reads the tile `passable` byte as its only obstacle test, and a unit standing on a tile holds that byte at 0 (the tile's own
// value lives in the unit's origin_tile_was_passable). So an idle friend in a one-tile corridor is a WALL to the planner: the goal is
// shortened to the nearest tile on this side of it (a dead-end pocket) and the order is given up after ~6 s, although the walker
// (3e-1 nudge) and the marshal site (3e-4) would happily have made room.
// The wrapper below makes the planner see those tiles as open for the duration of ONE call: rt_unmask() writes the tile's own value
// (origin_tile_was_passable) into passable[] for every IDLE same-player ground unit of the order's owner, rt_restore() puts the 0 back
// right after. One stretch of code, so every reader of passable[] inside the planner (flood eligibility, trace_route's flank test,
// find_nearest_valid_tile, walk_to_valid_tile, the per-member corner-cut walks) agrees. Kept as walls: the goal tile itself (the
// goal logic is unchanged: a goal held by an idle friend is moved as today), walking / sleeping-in-order units, enemies, allies,
// buildings and every unit of another player. Wave members are not touched (register_member already restored their tiles).
// DETERMINISM: integer, units visited in index order, a fixed-size undo list, no clock, no rand; the same call on both peers.
// The rule itself (which tiles are opened, the cost penalty, the byte-exact restore) is sim/move/route_through.h, shared with the libmh
// twin (sim_group_move_order_commit.cpp); this is its live-memory World.
namespace {

mh::sim::move::rt_undo g_rt_undo;

struct rt_live_world {
    int32_t unit_count() { return (int32_t)mh::caps::g(mh::caps::UNITS).limit; }
    bool    unit(uint32_t player, int32_t i, mh::sim::move::rt_unit *out) {
        mh_map_object_unit *u = unit_rec(player, i);
        if (!u) return false;
        const mh_cfg_final_struct_Unit *c = cfg_of(*u);
        if (!c) return false;
        out->state                    = u->state;
        out->order_queued             = u->order_queued;
        out->move_microstep           = u->move_microstep;
        out->origin_tile_was_passable = u->origin_tile_was_passable;
        out->cfg_ground_idle          = c->move_op_code == 0x0f && c->default_op_code == ST_IDLE;
        out->cfg_ground_move          = c->move_op_code == 0x0f;
        out->alive                    = unit_alive(*u);
        out->x                        = u->x;
        out->y                        = u->y;
        return true;
    }
    uint16_t  tile_building(uint32_t flat) { return tile_of(flat >> 8, flat & 0xffu)->building; }
    uint8_t   tile_class_owner(uint32_t flat) { return tile_of(flat >> 8, flat & 0xffu)->class_owner; }
    uint8_t  &passable(uint32_t flat) { return *pass_of(flat >> 8, flat & 0xffu); }
    uint32_t &cell_flags(uint32_t flat) {
        return *reinterpret_cast<uint32_t *>(reinterpret_cast<uint8_t *>(mh::state::live().base[mh::state::RID_MAP_REGION_GRID]) + flat * 8u + 4u);
    }
};

// mods:M1WL v2 move_stream_heat: stream_heat.h's World over live memory.
constexpr uintptr_t ADDR_GRP_COUNT         = 0x0051de38; // _G_LLM_STRAT_GROUP_MEMBER_COUNT
constexpr uintptr_t ADDR_ROUTE_STEP_DELTAS = 0x00708af4; // _G_LLM_MAP_REGION_ROUTE_STEP_DELTAS byte[32]: {dy, dx, -, -} per route_search direction (mods:M1WL v4)
constexpr uintptr_t ADDR_ROUTE_DIR_CODES   = 0x0051de88; // _G_LLM_MAP_REGION_ROUTE_DIR_CODES int[8]: 1, 4, 7 .. 22
constexpr uintptr_t ADDR_GRP_MEMBERS       = 0x006804ac; // _G_LLM_STRAT_GROUP_MEMBERS {uint unit_handle; byte cur_col; byte cur_row}[256]

mh::sim::move::sh_state g_sh_state;
bool                    g_sh_logged = false;

struct sh_live_world {
    int32_t unit_count() { return (int32_t)mh::caps::g(mh::caps::UNITS).limit; }
    bool    unit(uint32_t player, int32_t i, mh::sim::move::sh_unit *out) {
        mh_map_object_unit *u = unit_rec(player, i);
        if (!u) return false;
        const uint8_t slot = mh::caps::path::unit_slot_u8(reinterpret_cast<const uint8_t *>(u));
        out->order         = u->order;
        out->state         = u->state;
        out->group         = u->move_group_id;
        out->alive         = unit_alive(*u);
        out->x             = u->x;
        out->y             = u->y;
        out->path_slot     = slot < mh::caps::path::slot_limit() ? slot : 0xff;
        out->path_cursor   = u->path_cursor;
        return true;
    }
    void wp_get(uint32_t player, uint32_t slot, uint32_t entry, uint8_t *heading, uint8_t *run) {
        const uint8_t *wp = mh::caps::path::waypoint(player, slot, entry);
        *heading          = wp[0];
        *run              = wp[1];
    }
    void dir_delta(int h, int *dx, int *dy) {
        const int8_t *t = reinterpret_cast<const int8_t *>(ADDR_DIR_DELTA);
        *dx             = t[h * 2];
        *dy             = t[h * 2 + 1];
    }
    // mods:M1WL v4 (sh_retrace): the route_search tables (0x004236c2), read live: byte[32] {dy, dx, -, -} per direction and int[8] dir codes.
    void route_step(int i, int *dx, int *dy, int *code) {
        const int8_t *t = reinterpret_cast<const int8_t *>(ADDR_ROUTE_STEP_DELTAS);
        *dy             = t[i * 4];
        *dx             = t[i * 4 + 1];
        *code           = reinterpret_cast<const int32_t *>(ADDR_ROUTE_DIR_CODES)[i];
    }
    uint32_t cell_cost(uint32_t low) { return mv_flood_cost(low); }                                  // what the flood adds for a cost byte (route_cost table when that key is on)
    uint32_t flood_wrap_mask() { return *reinterpret_cast<const volatile uint32_t *>(0x00708b1cu); } // mods:M1WL v5 (sh_margin): the flood's wrap mask, as net_lockstep.cpp fe_run reads it
    void     wp_put(uint32_t player, uint32_t slot, uint32_t entry, uint8_t heading, uint8_t run) {
        uint8_t *wp = mh::caps::path::waypoint(player, slot, entry);
        wp[0]       = heading;
        wp[1]       = run;
    }
    uint32_t &cell_flags(uint32_t flat) {
        return *reinterpret_cast<uint32_t *>(reinterpret_cast<uint8_t *>(mh::state::live().base[mh::state::RID_MAP_REGION_GRID]) + flat * 8u + 4u);
    }
    int32_t  member_count() { return *reinterpret_cast<const int32_t *>(ADDR_GRP_COUNT); }
    uint32_t member_handle(int32_t m) { return *reinterpret_cast<const uint32_t *>(ADDR_GRP_MEMBERS + static_cast<uint32_t>(m) * 6u); }
};

} // namespace

static void __cdecl rt_unmask() {
    g_rt_undo.n          = 0;
    const bool idle_on   = g_rt_on && g_nd_nudge; // the nudge / trade is what resolves the idle meeting: no key, no change
    const bool moving_on = g_rtm_on != 0;
    const bool replan_on = g_rtr_on != 0;
    const bool heat_on   = g_sh_on != 0;
    if (!idle_on && !moving_on && !replan_on && !heat_on) return;
    if (idle_on) mv_hit(MVH_RT);
    if (moving_on) mv_hit(MVH_RTM);
    if (replan_on) mv_hit(MVH_RTR);
    const auto    &live   = mh::state::live();
    const uint32_t player = *reinterpret_cast<const uint32_t *>(live.base[mh::state::RID_STRAT_GROUP_ORDER_OWNER]);
    const int32_t  goal_x = *reinterpret_cast<const int32_t *>(live.base[mh::state::RID_STRAT_GROUP_ORDER_GOAL_X]);
    const int32_t  goal_y = *reinterpret_cast<const int32_t *>(live.base[mh::state::RID_STRAT_GROUP_ORDER_GOAL_Y]);
    const auto    *geom   = reinterpret_cast<const mh_llm_strat_map_geom *>(live.base[mh::state::RID_GENERAL]);
    if (player >= 8 || !geom) return;
    rt_live_world w;
    const int     opened    = mh::sim::move::rt_unmask(w, g_rt_undo, idle_on ? g_rt_on : 0, true, player, goal_x, goal_y, geom->width_mask, geom->height_mask);
    const int     opened_mv = moving_on ? mh::sim::move::rt_unmask_moving(w, g_rt_undo, g_rtm_cost, player, goal_x, goal_y, geom->width_mask, geom->height_mask) : 0;
    const int     opened_rp = replan_on ? mh::sim::move::rt_unmask_replanning(w, g_rt_undo, g_rtm_cost, player, goal_x, goal_y, geom->width_mask, geom->height_mask) : 0;
    if (heat_on) { // mods:M1WL v2: after the unmasks (closed first in rt_restore, LIFO)
        sh_live_world hw;
        if (mh::sim::move::sh_open(hw, g_sh_state, player, g_sh_group, g_sh_plain, geom->width_mask, geom->height_mask, g_sh_p) > 0) {
            mv_hit(MVH_SH);
            { // mods:M1WL v3: the snapshot the ImGui section reads (a copy of what this plan priced; the rule itself never reads it back)
                mh::heatui::snapshot &sn = g_sh_snap;
                sn.step                  = mh::desync::replay_rec::current_step();
                sn.player                = player;
                sn.unit                  = hw.member_handle(0);
                sn.mates                 = g_sh_state.mates;
                sn.tiles                 = g_sh_state.tiles;
                sn.cost                  = g_sh_p.cost;
                sn.scale                 = g_sh_p.scale;
                sn.n                     = g_sh_state.n < mh::heatui::SNAP_TILES ? g_sh_state.n : mh::heatui::SNAP_TILES;
                int priced               = 0;
                for (int k = 0; k < sn.n; ++k) {
                    sn.flat[k] = g_sh_state.flat[k];
                    sn.cnt[k]  = g_sh_state.cnt[k];
                    if (g_sh_state.cnt[k]) ++priced;
                }
                sn.priced = priced;
                ++sn.seq;
            }
            char b[128];
            if (!g_sh_logged) {
                g_sh_logged = true;
                wsprintfA(b, "; move prototype [net] move_stream_heat=%d active\n", g_sh_on);
                seam_log(b);
            }
            static int n;
            if (n++ < 60) {
                wsprintfA(b, "; M1WL heat p%u u%u mates=%d tiles=%d\n", player, hw.member_handle(0), g_sh_state.mates, g_sh_state.tiles);
                seam_log(b);
            }
        }
    }
    if (opened_rp) {
        static int n;
        if (n++ < 200) {
            char b[160];
            wsprintfA(b, "; M1DT route-through p%u replanning friends opened=%d cost=%d goal=%d,%d (%d)\n", player, opened_rp, g_rtm_cost,
                      (int)(static_cast<uint32_t>(goal_x) & geom->width_mask), (int)(static_cast<uint32_t>(goal_y) & geom->height_mask), n);
            seam_log(b);
        }
    }
    if (opened) {
        static int n;
        if (n++ < 200) {
            char b[160];
            wsprintfA(b, "; M1N route-through p%u idle friends opened=%d goal=%d,%d (%d)\n", player, opened, (int)(static_cast<uint32_t>(goal_x) & geom->width_mask),
                      (int)(static_cast<uint32_t>(goal_y) & geom->height_mask), n);
            seam_log(b);
        }
    }
    if (opened_mv) {
        static int n;
        if (n++ < 200) {
            char b[160];
            wsprintfA(b, "; M1N route-through p%u walking friends opened=%d cost=%d goal=%d,%d (%d)\n", player, opened_mv, g_rtm_cost,
                      (int)(static_cast<uint32_t>(goal_x) & geom->width_mask), (int)(static_cast<uint32_t>(goal_y) & geom->height_mask), n);
            seam_log(b);
        }
    }
}

static void __cdecl rt_restore() {
    sh_live_world hw;
    if (g_sh_state.n > 0 && g_sh_p.retrace) { // mods:M1WL v4: the heat fired and the planner has run: re-trace the route charging the tile stepped onto, while the heat is still priced
        const auto    &live   = mh::state::live();
        const uint32_t player = *reinterpret_cast<const uint32_t *>(live.base[mh::state::RID_STRAT_GROUP_ORDER_OWNER]);
        const auto    *geom   = reinterpret_cast<const mh_llm_strat_map_geom *>(live.base[mh::state::RID_GENERAL]);
        if (player < 8 && geom) {
            mh::sim::move::sh_retrace_info inf;
            const int                      r = mh::sim::move::sh_retrace(hw, g_sh_state, player, geom->width_mask, geom->height_mask, g_sh_p, &inf);
            mh::sim::move::sh_retrace_count(g_sh_rt, r);
            static int n;
            if (r != mh::sim::move::SHR_NONE && n++ < 4000) {
                char b[192];
                wsprintfA(b, "; M1WL retrace p%u u%u result=%d turn=%d len=%d>%d heat=%d>%d runs=%d>%d turns=%d>%d\n", player, hw.member_handle(0), r, g_sh_p.turn, inf.len_before,
                          inf.len_after, inf.heat_before, inf.heat_after, inf.runs_before, inf.runs_after, inf.turns_before, inf.turns_after);
                seam_log(b);
            }
        }
    }
    if (g_sh_state.n > 0 && g_sh_p.margin > 0) { // mods:M1WL v5: the margin rule, still inside the heat (it re-floods the unheated field and puts every cell word back)
        const auto    &live   = mh::state::live();
        const uint32_t player = *reinterpret_cast<const uint32_t *>(live.base[mh::state::RID_STRAT_GROUP_ORDER_OWNER]);
        const auto    *geom   = reinterpret_cast<const mh_llm_strat_map_geom *>(live.base[mh::state::RID_GENERAL]);
        if (player < 8 && geom) {
            mh::sim::move::sh_margin_info inf;
            const int                     r = mh::sim::move::sh_margin(hw, g_sh_state, player, geom->width_mask, geom->height_mask, g_sh_p, &inf);
            mh::sim::move::sh_margin_count(g_sh_mg, r);
            static int n;
            if (r != mh::sim::move::SHM_NONE && n++ < 4000) {
                char b[160];
                wsprintfA(b, "; M1WL margin p%u u%u result=%d margin=%d len=%d plain=%d\n", player, hw.member_handle(0), r, g_sh_p.margin, inf.len_heated, inf.len_plain);
                seam_log(b);
            }
        }
    }
    mh::sim::move::sh_close(hw, g_sh_state); // the heat was priced last: closed first
    rt_live_world w;
    mh::sim::move::rt_restore(w, g_rt_undo);
}

// mods:M1DT fix A': the SAME bracket around 2a's single re-run of the planner (move_proto_arrival.cpp, rerun_pathfind). With every
// route-through key off rt_unmask() leaves the undo list empty and rt_restore() has nothing to put back.
void mv_rt_open() { rt_unmask(); }
void mv_rt_close() { rt_restore(); }

// mods:M1G 4b: the formation from the click, inside the open bracket right after the retail plan. commit's frame is the stub caller's
// (EBP unchanged): [ebp+8] = move_group_id, [ebp+0xc] = is_plain_move_flag, [ebp+0x10] = target_class, as mv_arr_commit_stub reads them.
static void __cdecl fc_after_pathfind(int32_t group_id, uint32_t mode, uint32_t target) {
    mv_hit(MVH_FC);
    g_fc_handled = mv_fc_run(group_id, mode, target, &g_rt_undo);
}

// clang-format off
__declspec(naked) static void rt_stub() {
    __asm {
        pushad
        mov   eax, dword ptr [ebp + 0x08]       // mods:M1WL v2: commit's move_group_id and is_plain_move_flag, for the stream heat
        mov   g_sh_group, eax
        mov   eax, dword ptr [ebp + 0x0c]
        mov   g_sh_plain, eax
        call  rt_unmask
        popad
        call  dword ptr [g_rt_orig_fn]          // EAX = mode, EDX = target mask, exactly as the commit loaded them
        cmp   g_fc_on, 0
        je    rt_nofc
        pushad                                  // mods:M1G 4b: the formation, in the bracket, after the retail plan (phase 1)
        push  dword ptr [ebp + 0x10]            // commit's target_class
        push  dword ptr [ebp + 0x0c]            // commit's is_plain_move_flag
        push  dword ptr [ebp + 0x08]            // commit's move_group_id
        call  fc_after_pathfind
        add   esp, 12
        popad
    rt_nofc:
        pushad
        call  rt_restore
        popad
        ret
    }
}
// clang-format on

// ---- mp:U75: [net] move_queue_flank_guard -- the unit-queue shuffle search --------------------------------------------------------
// llm_strat_unit_queue_advance_search (0x004d7f3a, __watcall EAX = player, EDX = unit index, returns the chain length in EAX; ~0x26c bytes,
// one callee, llm_strat_order_issue_0xf_adjacent_by_offset) is the "make room" shuffle a storage exit runs when a friendly idle unit stands on
// the exit tile. It is a DFS through the crowd that takes the first neighbour tile (8 directions, diagonals included) that is passable and
// unoccupied and then orders every unit of the chain one step. It tests only the destination tile, so a diagonal step between two wall tiles
// cuts a cliff corner (the route planner forbids it) and can seal a unit in a one-tile notch. Report 01M5WMNKXS2Q0PN3AJWWP5F48Q.
//
// THE SPLICE. The function's first 8 bytes (PUSH EBP / MOV EBP,ESP / PUSH 0x1c) become JMP qf_stub + 3 NOPs. Key off: the stub runs the three
// displaced instructions and continues at 0x004d7f42, so the retail body runs byte for byte (nothing about the original's writes changes).
// Key on: the stub calls qf_search, a transcription of the libmh twin (sim_unit_queue_advance.cpp) with the flank rule and the depth cap; it
// makes the SAME writes to the hashed _G_LLM_UNITQ_* globals, in the same order, so a libmh peer and a plain peer hash the same.
//
// RULE (sim/move/queue_flank.h). A diagonal direction is taken, in both neighbour loops, only if at least one orthogonal flank tile is OPEN
// (passable != 0 or holds a unit). If nothing legal exists the search returns 0 and the exiting unit waits. The descent is capped at the typed
// extent of _G_LLM_UNITQ_FRONTIER (128; the real extent is unproven) and _G_LLM_UNITQ_CLOSED (1024).
constexpr uintptr_t ADDR_QF_SITE = 0x004d7f3a;
const uint8_t       QF_EXPECT[8] = {0x55, 0x89, 0xE5, 0x68, 0x1C, 0x00, 0x00, 0x00};

struct qf_node {
    uint32_t tile; // x << 8 | y
    uint16_t dir;
    uint16_t unit;
};

static int __cdecl qf_search(uint32_t player, uint32_t unit_index) {
    const auto        &live     = mh::state::live();
    volatile uint32_t *cur_tile = reinterpret_cast<volatile uint32_t *>(live.base[mh::state::RID_UNITQ_CUR_TILE]);
    volatile int32_t  *fcount   = reinterpret_cast<volatile int32_t *>(live.base[mh::state::RID_UNITQ_FRONTIER_COUNT]);
    volatile int32_t  *ccount   = reinterpret_cast<volatile int32_t *>(live.base[mh::state::RID_UNITQ_CLOSED_COUNT]);
    volatile int32_t  *iter     = reinterpret_cast<volatile int32_t *>(live.base[mh::state::RID_UNITQ_ITER]);
    qf_node           *frontier = reinterpret_cast<qf_node *>(live.base[mh::state::RID_UNITQ_FRONTIER]);
    qf_node           *closed   = reinterpret_cast<qf_node *>(live.base[mh::state::RID_UNITQ_CLOSED]);
    const int32_t     *ndx      = reinterpret_cast<const int32_t *>(live.base[mh::state::RID_UNITQ_NEIGHBOR_DX]);
    const int32_t     *ndy      = reinterpret_cast<const int32_t *>(live.base[mh::state::RID_UNITQ_NEIGHBOR_DY]);
    const uint32_t     wm       = *reinterpret_cast<const uint8_t *>(live.base[mh::state::RID_WIDTH_M]);
    const uint32_t     hm       = *reinterpret_cast<const uint8_t *>(live.base[mh::state::RID_HEIGHT_M]);
    const bool         guard    = true; // the stub only calls with the key on

    *cur_tile         = 0;
    *fcount           = 0;
    *ccount           = 0;
    uint32_t cur_unit = unit_index;

    for (;;) {
        const int32_t stage_idx = *fcount;
        if (stage_idx < 0 || stage_idx >= mh::sim::move::QF_FRONTIER_NODES) break; // cannot happen (the descent is capped); never write past the array
        const mh_map_object_unit *su = unit_rec(player, static_cast<int32_t>(cur_unit));
        if (!su) break; // an unreadable unit: end the search with what is committed
        const uint8_t sx = su->x, sy = su->y;
        frontier[stage_idx].tile = (static_cast<uint32_t>(sx) << 8) | sy;
        frontier[stage_idx].unit = static_cast<uint16_t>(cur_unit);

        // the flank rule for direction d from (sx, sy)
        auto dir_ok = [&](int32_t d) {
            const int dx = static_cast<int8_t>(ndx[d]), dy = static_cast<int8_t>(ndy[d]);
            if (dx == 0 || dy == 0) return true;
            const uint32_t fx = (sx + static_cast<uint32_t>(dx)) & wm, fy = (sy + static_cast<uint32_t>(dy)) & hm;
            const bool     ox = mh::sim::move::qf_tile_open(*pass_of(fx, sy), tile_of(fx, sy)->class_owner);
            const bool     oy = mh::sim::move::qf_tile_open(*pass_of(sx, fy), tile_of(sx, fy)->class_owner);
            const bool     ok = mh::sim::move::qf_step_ok(guard, dx, dy, ox, oy);
            if (!ok) mv_hit(MVH_QF);
            return ok;
        };

        bool found_free = false;
        for (int32_t d = 0; d < 8; ++d) {
            const uint32_t nx = (sx + static_cast<uint32_t>(static_cast<int8_t>(ndx[d]))) & wm & 0xffu;
            const uint32_t ny = (sy + static_cast<uint32_t>(static_cast<int8_t>(ndy[d]))) & hm & 0xffu;
            *cur_tile         = (nx << 8) | ny;
            if (!dir_ok(d)) continue;
            const uint8_t p = *pass_of(nx, ny);
            if (p != 0 && p != 5 && tile_of(nx, ny)->class_owner == 0) {
                frontier[stage_idx].dir = static_cast<uint16_t>(d);
                *fcount                 = stage_idx + 1;
                found_free              = true;
                break;
            }
        }
        if (found_free) break;

        bool descended = false;
        for (int32_t d = 0; d < 8; ++d) {
            const uint32_t nx = (sx + static_cast<uint32_t>(static_cast<int8_t>(ndx[d]))) & wm & 0xffu;
            const uint32_t ny = (sy + static_cast<uint32_t>(static_cast<int8_t>(ndy[d]))) & hm & 0xffu;
            const uint32_t t  = (nx << 8) | ny;
            *cur_tile         = t;
            if (static_cast<uint32_t>(tile_of(nx, ny)->class_owner) != (player | 0x80u)) continue;
            if (!dir_ok(d)) continue;
            bool seen = false;
            *iter     = 0;
            while (*iter < *fcount) {
                if (frontier[*iter].tile == t) {
                    seen = true;
                    break;
                }
                *iter = *iter + 1;
            }
            if (!seen) {
                *iter = 0;
                while (*iter < *ccount) {
                    if (closed[*iter].tile == t) {
                        seen = true;
                        break;
                    }
                    *iter = *iter + 1;
                }
            }
            if (seen) continue;
            if (!mh::sim::move::qf_may_descend(guard, stage_idx, *ccount)) {
                mv_hit(MVH_QF);
                continue;
            }
            frontier[stage_idx].dir = static_cast<uint16_t>(d);
            *fcount                 = stage_idx + 1;
            cur_unit                = tile_of(nx, ny)->building;
            descended               = true;
            break;
        }
        if (descended) continue;

        if (*fcount == 0) break;
        // dead end: the staged node goes to CLOSED, pop, resume from the parent
        const int32_t closed_idx = *ccount;
        if (closed_idx < 0 || closed_idx >= mh::sim::move::QF_CLOSED_NODES) {
            *fcount = 0; // cannot happen (qf_may_descend keeps live + closed nodes within the array); never write past it
            break;
        }
        closed[closed_idx] = frontier[stage_idx];
        const int32_t nf   = stage_idx - 1;
        *fcount            = nf;
        cur_unit           = frontier[nf].unit;
        *ccount            = closed_idx + 1;
    }

    const int32_t n = *fcount;
    *iter           = 0;
    while (*iter < n) {
        const qf_node &f = frontier[*iter];
        mh::call::llm_strat_order_issue_0xf_adjacent_by_offset(player, f.unit, ndx[f.dir & 7], ndy[f.dir & 7]);
        *iter = *iter + 1;
    }
    return n;
}

// clang-format off
__declspec(naked) static void qf_stub() {
    __asm {
        cmp   g_qf_req, 0
        je    qf_retail
        push  ecx                               // the original saves ECX (watcall callee-saved); the C++ body may clobber it
        push  edx                               // unit index
        push  eax                               // player
        call  qf_search
        add   esp, 8
        pop   ecx
        ret
    qf_retail:
        push  ebp                               // the three displaced instructions
        mov   ebp, esp
        push  0x1c
        jmp   dword ptr [g_qf_ret]
    }
}
// clang-format on

static void install_queue_flank() {
    if (mh::hook::promoted_owner_of(ADDR_QF_SITE)) {
        g_qf_state = -2;
    } else {
        uint8_t       repl[8] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90};
        const int32_t rel     = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&qf_stub) - (ADDR_QF_SITE + 5));
        memcpy(repl + 1, &rel, sizeof(rel));
        g_qf_state = mh::hook::patch_bytes_guarded(ADDR_QF_SITE, QF_EXPECT, repl, 8) ? 1 : -1;
    }
    char b[200];
    wsprintfA(b, "; move prototype splice (mp:U75 queue-advance flank guard): %s\n", nd_state_name(g_qf_state));
    seam_log(b);
    mh::seams::mv::note_site(mh::seams::mv::SITE_QF, g_qf_state);
}

void install_move_proto_nudge() {
    if (mh::hook::promoted_owner_of(ADDR_ND_CALL)) {
        g_nd_state = -2;
    } else {
        uint8_t       repl[5];
        const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&nd_stub) - (ADDR_ND_CALL + 5));
        repl[0]           = 0xE8;
        memcpy(repl + 1, &rel, sizeof(rel));
        g_nd_state = mh::hook::patch_bytes_guarded(ADDR_ND_CALL, ND_EXPECT, repl, 5) ? 1 : -1;
    }
    char b[200];
    wsprintfA(b, "; move prototype splice (mods:M1N nudge / claim / swap): %s\n", nd_state_name(g_nd_state));
    seam_log(b);

    if (mh::hook::promoted_owner_of(ADDR_MK_SITE)) {
        g_mk_state = -2;
    } else {
        uint8_t       repl[7] = {0xE9, 0, 0, 0, 0, 0x90, 0x90};
        const int32_t rel     = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&mk_stub) - (ADDR_MK_SITE + 5));
        memcpy(repl + 1, &rel, sizeof(rel));
        g_mk_state = mh::hook::patch_bytes_guarded(ADDR_MK_SITE, MK_EXPECT, repl, 7) ? 1 : -1;
    }
    if (mh::hook::promoted_owner_of(ADDR_MK2_SITE)) {
        g_mk2_state = -2;
    } else {
        uint8_t       repl[7] = {0xE9, 0, 0, 0, 0, 0x90, 0x90};
        const int32_t rel     = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&mk2_stub) - (ADDR_MK2_SITE + 5));
        memcpy(repl + 1, &rel, sizeof(rel));
        g_mk2_state = mh::hook::patch_bytes_guarded(ADDR_MK2_SITE, MK2_EXPECT, repl, 7) ? 1 : -1;
    }
    wsprintfA(b, "; move prototype splice (mods:M1N marshal no-path nudge / swap): no-path branch %s, exhausted-path branch %s\n", nd_state_name(g_mk_state), nd_state_name(g_mk2_state));
    seam_log(b);

    if (mh::hook::promoted_owner_of(ADDR_RT_CALL)) {
        g_rt_state = -2;
    } else {
        uint8_t       repl[5];
        const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&rt_stub) - (ADDR_RT_CALL + 5));
        repl[0]           = 0xE8;
        memcpy(repl + 1, &rel, sizeof(rel));
        g_rt_state = mh::hook::patch_bytes_guarded(ADDR_RT_CALL, RT_EXPECT, repl, 5) ? 1 : -1;
    }
    wsprintfA(b, "; move prototype splice (mods:M1N route planner, idle friends are not walls): %s\n", nd_state_name(g_rt_state));
    seam_log(b);
    install_queue_flank(); // mp:U75
    mh::seams::mv::note_site(mh::seams::mv::SITE_ND, g_nd_state);
    mh::seams::mv::note_site(mh::seams::mv::SITE_MK, g_mk_state);
    mh::seams::mv::note_site(mh::seams::mv::SITE_MK2, g_mk2_state);
    mh::seams::mv::note_site(mh::seams::mv::SITE_RT, g_rt_state);
    mh::seams::mv::note_site(mh::seams::mv::SITE_RTM, g_rt_state);
    mh::seams::mv::note_site(mh::seams::mv::SITE_RTR, g_rt_state);
    mh::seams::mv::note_site(mh::seams::mv::SITE_FC, g_rt_state);
    mh::seams::mv::note_site(mh::seams::mv::SITE_SH, g_rt_state);
}

// The setters set the stubs' gates and tell the carrier report (move_carrier.cpp) the requested value; the report -- late, once the
// unit-state table is filled -- is the one line per key ("retail splice" / "libmh twin" / "NOT ACTIVE: ...").
int mh_set_move_nudge_idle(int on) {
    on         = on != 0;
    g_nd_nudge = 0;
    mh::seams::mv::request(mh::seams::mv::KEY_NUDGE_IDLE, on, 0);
    if (!on || g_nd_state != 1) return 0;
    g_nd_nudge = 1;
    return 1;
}

// mp:U75: the shuffle search's flank guard + depth cap (the splice at 0x004d7f3a) and the same flank rule in the idle nudge's tile pick.
// 1 = in force on at least one retail site (the libmh twin carries the rest; the carrier line says which).
int mh_set_move_queue_flank_guard(int on) {
    on       = on != 0;
    g_qf_req = 0;
    mh::seams::mv::request(mh::seams::mv::KEY_QUEUE_FLANK, on, 0);
    if (!on) return 0;
    if (g_qf_state != 1 && g_nd_state != 1) return 0;
    g_qf_req = 1;
    return 1;
}

int mh_set_move_claim_leader(int on) {
    on         = on != 0;
    g_nd_claim = 0;
    mh::seams::mv::request(mh::seams::mv::KEY_CLAIM_LEADER, on, 0);
    if (!on || g_nd_state != 1) return 0;
    g_nd_claim = 1;
    return 1;
}

int mh_set_move_swap_headon(int on) {
    on        = on != 0;
    g_nd_swap = 0;
    mh::seams::mv::request(mh::seams::mv::KEY_SWAP_HEADON, on, 0);
    if (!on || g_nd_state != 1) return 0;
    g_nd_swap = 1;
    return 1;
}

int mv_bk_state(); // move_proto_blocked.cpp: the state of the blocked-unit splice (1 carried)

// mods:M1BE: needs BOTH walker sites (the nudge CALL for the wait, the blocked block for the exemption), which sit in one function.
void mh_set_move_enter_queue(int on) {
    on      = on != 0;
    g_eq_on = 0;
    mh::seams::mv::request(mh::seams::mv::KEY_ENTER_QUEUE, on, 0);
    if (!on || g_nd_state != 1 || mv_bk_state() != 1) return;
    g_eq_on = 1;
}

int mh_set_move_route_through_idle(int on) {
    g_rt_on = 0;
    mh::seams::mv::request(mh::seams::mv::KEY_ROUTE_THROUGH_IDLE, on, 0);
    if (on == 0 || g_rt_state != 1) return 0;
    g_rt_on = on == 2 ? 2 : 1; // 2 = measurement: open the tiles at the plain tile cost (no penalty)
    return 1;
}

// mods:M1N 3e-6: a WALKING friend's tile is open ground to the planner too (sim/move/route_through.h rt_unmask_moving), priced by `cost`.
// Same splice as move_route_through_idle; needs no other key.
int mh_set_move_route_through_moving(int on) {
    g_rtm_on = 0;
    mh::seams::mv::request(mh::seams::mv::KEY_ROUTE_THROUGH_MOVING, on, 0);
    if (on == 0 || g_rt_state != 1) return 0;
    g_rtm_on = 1;
    return 1;
}

void mh_set_move_route_through_moving_cost(int level) {
    g_rtm_cost = level < 0 ? 0 : (level > 4 ? 4 : level);
    mh::seams::mv::request(mh::seams::mv::KEY_ROUTE_THROUGH_MOVING_COST, level, 0);
}

// mods:M1DT fix 2: an own unit in state 0x0a (re-planning) is open ground to the planner too (sim/move/route_through.h
// rt_unmask_replanning), priced by the moving cost level. Same splice as move_route_through_idle; needs no other key.
int mh_set_move_route_through_replanning(int on) {
    g_rtr_on = 0;
    mh::seams::mv::request(mh::seams::mv::KEY_ROUTE_THROUGH_REPLANNING, on, 0);
    if (on == 0 || g_rt_state != 1) return 0;
    g_rtr_on = 1;
    return 1;
}

// mods:M1WL v2: a lone unit of a stream plans round the next 48 path tiles of its groupmates (sim/move/stream_heat.h). Same splice as the
// route-through keys (the bracket at 0x0041c9ce); needs no other key. 0 off, 1 on.
int mh_set_move_stream_heat(int on) {
    g_sh_on     = 0;
    g_sh_logged = false; // the "active" line is written by the first plan that fires (the setter runs before the match log opens)
    mh::seams::mv::request(mh::seams::mv::KEY_STREAM_HEAT, on, 0);
    if (on == 0 || g_rt_state != 1) return 0;
    g_sh_on = 1;
    return 1;
}

// mods:M1WL v3: the stream heat's tuning keys (raw numbers; sh_params_clamped is the one clamp, shared with the libmh twin). Takes effect from the next plan.
void mh_set_move_stream_heat_params(int cost, int window, int min_mates, int scale, int retrace, int turn, int scale_cap, int margin) {
    g_sh_p = mh::sim::move::sh_params_clamped(cost, window, min_mates, scale, retrace, turn, scale_cap, margin);
}

// mods:M1G 4b: 0 off (retail), 1..2 = old keep rule (N - 1 rings of slack), 3 = the formation moves as one (sim/move/formation.h). Same splice as move_route_through_idle.
int mh_set_move_formation_click(int n) {
    g_fc_on      = 0;
    g_fc_handled = 0;
    n            = n < 0 ? 0 : (n > 3 ? 3 : n);
    mh::seams::mv::request(mh::seams::mv::KEY_FORMATION_CLICK, n, 0);
    if (n == 0 || g_rt_state != 1) return 0;
    g_fc_on = n;
    return 1;
}

// ---- mods:M1DT: world access for the walker block site / the marshal check (move_proto_blocked.cpp) ------------------------------
// Who stands on the walker's destination tile: an own unit's index, and whether it is a walking ground mover (state 0x0f, alive).
void mv_blocker_info(uint32_t player, uint32_t x, uint32_t y, mh::sim::mvc::blocker_info *out) {
    *out                       = mh::sim::mvc::blocker_info{};
    mh_map_tile_object_data *t = tile_of(x, y);
    if (t->building == 0 || t->class_owner != static_cast<uint8_t>(player | 0x80u)) return;
    mh_map_object_unit *b = unit_rec(player, t->building);
    if (!b) return;
    const mh_cfg_final_struct_Unit *c = cfg_of(*b);
    out->idx                          = t->building;
    out->walking_friend               = b->state == ST_WALK && c && c->move_op_code == 0x0f && unit_alive(*b);
}

// The marshal good-path check. Returns the unit record (non-zero) when the route the planner just attached must be discarded: the path
// slot is freed here (the caller advances the clock and skips the member), else 0.
uintptr_t mv_cap_refuse(uint32_t player, uint32_t idx, int cap_pct) {
    mh_map_object_unit *u = unit_rec(player, static_cast<int32_t>(idx));
    if (!u) return 0;
    const auto *geom = reinterpret_cast<const mh_llm_strat_map_geom *>(mh::state::live().base[mh::state::RID_GENERAL]);
    if (!geom) return 0;
    mv_hit(MVH_CAPOK);
    const bool refuse = mh::sim::mvc::rc_should_refuse(*u, cap_pct, geom->width_mask, geom->height_mask, [player](uint32_t slot, int entry) -> const mh::game::mh_llm_strat_path_waypoint & {
        return reinterpret_cast<const mh::game::mh_llm_strat_path_waypoint *>(path_of(player, slot))[entry];
    });
    if (!refuse) return 0;
    mh::call::llm_strat_path_free_slot(static_cast<uint16_t>(player), static_cast<int32_t>(idx));
    static int n;
    if (n++ < 200) {
        char b[160];
        wsprintfA(b, "; M1DT replan refused p%u unit %u at %u,%u goal %u,%u (refusal %d) (%d)\n", player, idx, (unsigned)u->x, (unsigned)u->y, (unsigned)u->goal_x,
                  (unsigned)u->goal_y, (int)u->_pad_0xca[0] - 1, n);
        seam_log(b);
    }
    return reinterpret_cast<uintptr_t>(u);
}

// ---- mods:M1WL v3: the reads behind the ImGui "Stream heat" section (seams/heat_ui.h). Display only: no write to game memory, no RNG. ----------
namespace mh::heatui {

const snapshot &last_plan() { return g_sh_snap; }
uint32_t        hit_counter() { return g_mv_hit[MVH_SH]; }

void get_live(live_values *out) {
    out->on        = g_sh_on;
    out->cost      = g_sh_p.cost;
    out->window    = g_sh_p.window;
    out->min_mates = g_sh_p.min_mates;
    out->scale     = g_sh_p.scale;
    out->retrace   = g_sh_p.retrace;
    out->turn      = g_sh_p.turn;
    out->scale_cap = g_sh_p.scale_cap;
    out->margin    = g_sh_p.margin;
}

void retrace_counts(uint32_t *plans, uint32_t *changed, uint32_t *refused) {
    *plans   = g_sh_rt.plans;
    *changed = g_sh_rt.changed;
    *refused = g_sh_rt.refused;
}

void margin_counts(uint32_t *checks, uint32_t *kept) {
    *checks = g_sh_mg.checks;
    *kept   = g_sh_mg.rewrote;
}

int traffic_sample(uint8_t *counts) {
    memset(counts, 0, 65536);
    const auto &g     = mh::caps::g(mh::caps::UNITS);
    int         total = 0;
    for (uint32_t p = 0; p < g.players && p < 8; ++p) {
        for (uint32_t i = 1; i < g.limit; ++i) {
            const mh_map_object_unit *u = unit_rec(p, static_cast<int32_t>(i));
            if (!u || !unit_alive(*u)) continue;
            const bool stored = u->state >= 0x20 && u->state <= 0x2b && u->state != 0x23; // docked in a building: no tile of its own
            if (stored) continue;
            uint8_t &c = counts[(static_cast<uint32_t>(u->x) << 8) | u->y];
            if (c < 255) ++c;
            ++total;
        }
    }
    return total;
}

int unit_route(uint32_t player, uint32_t unit, uint32_t *flat, int max) {
    const mh_map_object_unit *u    = unit_rec(player, static_cast<int32_t>(unit));
    const auto               *geom = reinterpret_cast<const mh_llm_strat_map_geom *>(mh::state::live().base[mh::state::RID_GENERAL]);
    if (!u || !geom || !unit_alive(*u)) return 0;
    const uint8_t slot = mh::caps::path::unit_slot_u8(reinterpret_cast<const uint8_t *>(u));
    if (slot >= mh::caps::path::slot_limit()) return 0;
    int      e = u->path_cursor, n = 0;
    uint32_t x = u->x, y = u->y;
    if (e < 0 || e >= mh::sim::move::SH_PATH_ENTRIES) return 0;
    const uint8_t *wp = mh::caps::path::waypoint(player, slot, static_cast<uint32_t>(e));
    if (wp[1] == 0) ++e; // a spent leg, skipped once like the walker does
    for (; e < mh::sim::move::SH_PATH_ENTRIES && n < max; ++e) {
        wp          = mh::caps::path::waypoint(player, slot, static_cast<uint32_t>(e));
        const int h = wp[0], r = wp[1];
        if (h == 0) break;
        int dx = 0, dy = 0;
        if (h <= 24) {
            const int8_t *t = reinterpret_cast<const int8_t *>(ADDR_DIR_DELTA);
            dx              = t[h * 2];
            dy              = t[h * 2 + 1];
        }
        for (int k = 0; k < r && n < max; ++k) {
            x         = (x + static_cast<uint32_t>(dx)) & geom->width_mask;
            y         = (y + static_cast<uint32_t>(dy)) & geom->height_mask;
            flat[n++] = ((x & 0xffu) << 8) | (y & 0xffu);
        }
    }
    return n;
}

} // namespace mh::heatui
