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
// If libmh promotes move_walker the site is DISPLACED and the keys log "NOT ACTIVE" (no libmh twin in this phase).
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
#include "hook/patch.h"    // patch_bytes_guarded
#include "hook/promoted.h" // promoted_owner_of
#include "state/roster_caps.h"
#include "seams/net_internal.h"

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

const char *nd_state_name(int s) {
    return s == 1 ? "carried" : (s == -1 ? "the splice MISMATCHED" : (s == -2 ? "DISPLACED (libmh owns move_walker; no twin)" : "not installed"));
}

// ---- state access (live bases: the hosted exe or the standalone host's own allocation) ---------------------------------------
mh_map_object_unit *unit_rec(uint32_t player, int32_t idx) {
    const auto caps = mh::state::live_roster_caps();
    if (player >= 8 || idx < 0 || idx >= caps.units) return nullptr;
    return reinterpret_cast<mh_map_object_unit *>(mh::state::live().base[mh::state::RID_UNITS]) + player * caps.units + idx;
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
// One slot's path entries: {heading, run_length} pairs, 300 per slot, 100 slots per player.
uint8_t *path_of(uint32_t player, uint32_t slot) {
    return reinterpret_cast<uint8_t *>(mh::state::live().base[mh::state::RID_STRAT_PATH_BUFFERS]) + (player * 30000u + slot * 300u) * 2u;
}

void heading_delta(int h, int *dx, int *dy) {
    if (h < 1 || h > 24) {
        *dx = *dy = 0;
        return;
    }
    const int8_t *t = reinterpret_cast<const int8_t *>(ADDR_DIR_DELTA);
    *dx             = t[h * 2];
    *dy             = t[h * 2 + 1];
}
int heading_wrap(int h) { return ((h - 1) % 24 + 24) % 24 + 1; }
int turn_notches(int a, int b) {
    int d = a - b;
    if (d < 0) d = -d;
    d %= 24;
    return d > 12 ? 24 - d : d;
}

// A unit's NEXT step as the walker will take it: the leg at its cursor, after the walker's own "skip a spent leg" rule. Returns the
// heading (0 = the path ends / no path) and the cursor the leg sits at.
int next_leg(const mh_map_object_unit &u, uint32_t player, int *cursor_out) {
    if (u.path_slot_id >= 100) return 0;
    int cur = u.path_cursor;
    if (cur < 0 || cur >= PATH_ENTRIES - 1) return 0;
    const uint8_t *e = path_of(player, u.path_slot_id);
    if (e[cur * 2 + 1] == 0) ++cur; // spent leg
    if (cur >= PATH_ENTRIES) return 0;
    *cursor_out = cur;
    return e[cur * 2];
}

uint64_t raw_bits(const double &d) {
    uint64_t v;
    memcpy(&v, &d, sizeof(v));
    return v;
}

// ---- 3e-1: where does the idle blocker go --------------------------------------------------------------------------------
bool pick_nudge_tile(const mh_map_object_unit &b, int leg, uint32_t sx, uint32_t sy, uint32_t mask_x, uint32_t mask_y, uint32_t *ox, uint32_t *oy,
                     bool *straight = nullptr) {
    static const int ks[7] = {2, -2, 1, -1, 3, -3, 0}; // perpendicular, then the diagonals, then (last resort) straight ahead
    int              best  = 1000;
    if (straight) *straight = false;
    for (int i = 0; i < 7; ++i) {
        const int h = heading_wrap(leg + 3 * ks[i]);
        int       dx, dy;
        heading_delta(h, &dx, &dy);
        if (!dx && !dy) continue;
        const uint32_t nx = (b.x + static_cast<uint32_t>(dx)) & mask_x, ny = (b.y + static_cast<uint32_t>(dy)) & mask_y;
        if (nx == sx && ny == sy) continue;
        if (*pass_of(nx, ny) == 0 || tile_of(nx, ny)->building != 0) continue; // step_adjacent's own test: passable and unoccupied
        const int score = turn_notches(b.facing_target, h) + (ks[i] == 0 ? 30 : 0);
        if (score < best) {
            best = score;
            *ox  = nx;
            *oy  = ny;
            if (straight) *straight = ks[i] == 0;
        }
    }
    return best < 1000;
}

// ---- 3e-2 / 3e-3: the exchange -------------------------------------------------------------------------------------------------
void soldiers_clear_wander(uint32_t player, int32_t unit_index, mh_map_object_unit &u, const mh_cfg_final_struct_Unit &c, bool always_heading) {
    if (c.soldier_count <= 0) return;
    const auto     caps = mh::state::live_roster_caps();
    const uint32_t si   = static_cast<uint32_t>(u.unit_above[0] | (u.unit_above[1] << 8));
    auto          *sold = reinterpret_cast<mh_llm_strat_crew_soldier *>(mh::state::live().base[mh::state::RID_STRAT_SOLDIERS]);
    if (!sold || si >= static_cast<uint32_t>(caps.soldiers)) {
        if (always_heading) mh::call::llm_strat_unit_soldiers_set_heading(static_cast<uint16_t>(player), unit_index, u.facing_target);
        return;
    }
    mh_llm_strat_crew_soldier &s = sold[player * caps.soldiers + si];
    if (s.idle_wander_flag != 0 || always_heading) {
        mh::call::llm_strat_unit_soldiers_set_heading(static_cast<uint16_t>(player), unit_index, u.facing_target);
        s.idle_wander_flag = 0;
    }
}

// A (acting, at S) and B (at T) trade tiles. claim = false: SWAP, both just take their next step. claim = true: B (the turning
// leader) is moved onto S and gets one extra leg in front of its path (heading hA, run 1) so its path stays valid.
void do_exchange(mh_map_object_unit &A, int32_t ai, mh_map_object_unit &B, int32_t bi, uint32_t player, bool claim, int hA, int cursA, int cursB) {
    const uint32_t sx = A.x, sy = A.y, tx = B.x, ty = B.y;
    const auto    *ca = cfg_of(A);
    const auto    *cb = cfg_of(B);

    // tiles: each unit's new tile is claimed at once, each old tile is (re)claimed by the other one in the same tick
    mh_map_tile_object_data *tS = tile_of(sx, sy), *tT = tile_of(tx, ty);
    tS->building     = static_cast<uint16_t>(bi);
    tS->class_owner  = static_cast<uint8_t>(player | 0x80u);
    tT->building     = static_cast<uint16_t>(ai);
    tT->class_owner  = static_cast<uint8_t>(player | 0x80u);
    *pass_of(sx, sy) = 5;
    *pass_of(tx, ty) = 5;
    const uint8_t oa = A.origin_tile_was_passable, ob = B.origin_tile_was_passable;
    A.origin_tile_was_passable = ob; // the underlying value of T is what B stood on
    B.origin_tile_was_passable = oa;

    // paths: A takes its step (the leg shrinks by one, like a normal step)
    uint8_t *pa       = path_of(player, A.path_slot_id);
    pa[cursA * 2 + 1] = static_cast<uint8_t>(pa[cursA * 2 + 1] - 1);
    if (!claim) {
        uint8_t *pb       = path_of(player, B.path_slot_id);
        pb[cursB * 2 + 1] = static_cast<uint8_t>(pb[cursB * 2 + 1] - 1);
        B.path_cursor     = cursB;
    } else {
        // room for one more entry: shift cursor..end (end sentinel included) up by one, write the return leg at the cursor
        // (at cursB, the leg B would step next, so a spent leg behind the cursor is still skipped by the walker's own rule)
        uint8_t *pb = path_of(player, B.path_slot_id);
        int      e  = cursB;
        while (e < PATH_ENTRIES - 1 && pb[e * 2] != 0) ++e; // e = index of the end sentinel (caller checked the room)
        for (int i = e; i >= cursB; --i) {
            pb[(i + 1) * 2]     = pb[i * 2];
            pb[(i + 1) * 2 + 1] = pb[i * 2 + 1];
        }
        pb[cursB * 2]     = static_cast<uint8_t>(hA);
        pb[cursB * 2 + 1] = 1;
        const uint8_t rev = static_cast<uint8_t>(heading_wrap(hA + 12)); // the direction B slides: from T to S
        B.facing_target   = rev;
        B.facing_current  = rev;
    }

    A.x                        = static_cast<uint8_t>(tx);
    A.y                        = static_cast<uint8_t>(ty);
    B.x                        = static_cast<uint8_t>(sx);
    B.y                        = static_cast<uint8_t>(sy);
    A.move_microstep           = 0;
    B.move_microstep           = 0;
    A.path_blocked_retry_count = 0;
    B.path_blocked_retry_count = 0;

    // fog of war: each unit's sight leaves its old tile and arrives on its new one (same player: the counts net out except where
    // the two sight radii differ)
    mh::call::llm_strat_fow_remove_sight(player, static_cast<int32_t>(sx), static_cast<int32_t>(sy), ca->sight);
    mh::call::llm_strat_fow_remove_sight(player, static_cast<int32_t>(tx), static_cast<int32_t>(ty), cb->sight);
    mh::call::map_fow_UpdateFoWPlus(player, tx, ty, ca->sight);
    mh::call::map_fow_UpdateFoWPlus(player, sx, sy, cb->sight);

    soldiers_clear_wander(player, ai, A, *ca, false);
    soldiers_clear_wander(player, bi, B, *cb, claim);
}

} // namespace

// One log line per event (first 200 of each kind per run): the evidence that a key fired, and where.
static void nd_note(const char *what, uint32_t player, int32_t ai, int32_t bi, uint32_t sx, uint32_t sy, uint32_t dx, uint32_t dy) {
    static int n[3];
    const int  k = what[0] == 'n' ? 0 : (what[0] == 's' ? 1 : 2);
    if (n[k]++ >= 200) return;
    char b[200];
    wsprintfA(b, "; M1N %s p%u unit %d at %u,%u / unit %d at %u,%u (%d)\n", what, player, (int)ai, sx, sy, (int)bi, dx, dy, n[k]);
    seam_log(b);
}

namespace {
// a walking unit A trades tiles with the IDLE friend B standing on the tile A wants (defined with the marshal helpers below)
bool nd_trade(mh_map_object_unit &A, int32_t ai, mh_map_object_unit &B, int32_t bi, uint32_t player, int hA, int cursA);
} // namespace

// ---- the decision --------------------------------------------------------------------------------------------------------------
extern "C" {
int       g_nd_nudge   = 0;             // [net] move_nudge_idle in force
int       g_nd_claim   = 0;             // [net] move_claim_leader in force
int       g_nd_swap    = 0;             // [net] move_swap_headon in force
uintptr_t g_nd_orig_fn = FN_STEP_CHECK; // the stub's tail-jump target
uintptr_t g_mk_ret     = ADDR_MK_RET;   // mk_stub: continue the retail clock-bump branch
uintptr_t g_mk_skip    = ADDR_MK_SKIP;  // mk_stub: handled, next member
uintptr_t g_mk2_ret    = ADDR_MK2_RET;  // mk2_stub: continue the retry branch
int       g_rt_on      = 0;             // [net] move_route_through_idle in force
uintptr_t g_rt_orig_fn = FN_PATHFIND;   // rt_stub: the wave planner it wraps
}

// returns -1 = run the retail function; else its return value (0 = wait for the occupant)
static int __cdecl nd_decide(uint32_t sx, uint32_t sy, uint32_t dx, uint32_t dy) {
    if (!(g_nd_nudge | g_nd_claim | g_nd_swap)) return -1;
    const mh_map_tile_object_data *tS = tile_of(sx, sy), *tT = tile_of(dx, dy);
    if ((tS->class_owner & 0x80u) == 0 || (tT->class_owner & 0x80u) == 0) return -1; // a building / terrain blocks: retail
    const uint32_t ap = tS->class_owner & 0xfu, bp = tT->class_owner & 0xfu;
    if (ap != bp) return -1; // enemy / ally: retail
    const int32_t ai = tS->building, bi = tT->building;
    if (ai == bi) return -1;
    mh_map_object_unit *A = unit_rec(ap, ai), *B = unit_rec(bp, bi);
    if (!A || !B || A->x != sx || A->y != sy || B->x != dx || B->y != dy) return -1;
    const mh_cfg_final_struct_Unit *ca = cfg_of(*A), *cb = cfg_of(*B);
    if (!ca || !cb || ca->move_op_code != 0x0f || cb->move_op_code != 0x0f) return -1;  // ground movers only
    if ((raw_bits(B->energy) >> 63) != 0 || (raw_bits(B->energy) << 1) == 0) return -1; // dead or dying: retail
    if (A->state != ST_WALK || A->path_slot_id >= 100) return -1;

    int       cursA = 0;
    const int hA    = next_leg(*A, ap, &cursA);
    if (hA == 0 || A->facing_target != hA) return -1;
    int adx, ady;
    heading_delta(hA, &adx, &ady);
    const uint32_t mask_x = reinterpret_cast<const mh_llm_strat_map_geom *>(mh::state::live().base[mh::state::RID_GENERAL])->width_mask;
    const uint32_t mask_y = reinterpret_cast<const mh_llm_strat_map_geom *>(mh::state::live().base[mh::state::RID_GENERAL])->height_mask;
    if (((sx + static_cast<uint32_t>(adx)) & mask_x) != dx || ((sy + static_cast<uint32_t>(ady)) & mask_y) != dy) return -1; // not the walker's own step

    // ---- 3e-1: an idle friend in the way -------------------------------------------------------------------------------
    if (g_nd_nudge) {
        if (B->state == ST_STEP_ADJACENT) return 0; // the nudge has been asked for, the unit has not started it yet
        if (B->state == ST_IDLE && cb->default_op_code == ST_IDLE && B->order_queued == 0 && B->move_microstep >= MS_SETTLED) {
            uint32_t nx, ny;
            bool     straight = false;
            if (pick_nudge_tile(*B, hA, sx, sy, mask_x, mask_y, &nx, &ny, &straight)) {
                // only straight ahead is free (a one-tile corridor): pushing the friend ahead of us tile after tile is worse than trading
                // tiles with it, as the marshal site does (3e-4); the push stays as the fallback when the trade cannot be set up
                if (straight && A->move_microstep >= MS_SETTLED && A->order_queued == 0 && nd_trade(*A, ai, *B, bi, ap, hA, cursA)) {
                    nd_note("swap-trade", ap, ai, bi, sx, sy, dx, dy);
                    return 0;
                }
                B->goal_x = static_cast<uint8_t>(nx);
                B->goal_y = static_cast<uint8_t>(ny);
                B->state  = ST_STEP_ADJACENT;
                nd_note("nudge", ap, ai, bi, sx, sy, nx, ny);
                B->order = ST_IDLE;
                // a sleeping unit (clock ahead of the game clock) must not sit out its sleep before it makes room
                if (raw_bits(B->activity_clock) > *reinterpret_cast<const uint64_t *>(ADDR_CLOCK)) memcpy(&B->activity_clock, reinterpret_cast<const void *>(ADDR_CLOCK), 8);
                return 0;
            }
        }
    }

    // ---- 3e-3 / 3e-2: a walking friend ---------------------------------------------------------------------------------
    if ((g_nd_swap | g_nd_claim) && B->state == ST_WALK && B->order_queued == 0 && A->order_queued == 0 &&
        A->move_microstep >= MS_SETTLED && B->path_slot_id < 100) {
        int       cursB = 0;
        const int hB    = next_leg(*B, bp, &cursB);
        if (hB != 0) {
            int bdx, bdy;
            heading_delta(hB, &bdx, &bdy);
            const bool head_on = ((dx + static_cast<uint32_t>(bdx)) & mask_x) == sx && ((dy + static_cast<uint32_t>(bdy)) & mask_y) == sy;
            if (head_on) {
                if (!g_nd_swap) return -1;
                // still sliding into its tile or still turning toward us: wait for it (its own blocked step would do the swap too)
                if (B->move_microstep < MS_SETTLED || B->facing_target != hB) return 0;
                nd_note("swap", ap, ai, bi, sx, sy, dx, dy);
                do_exchange(*A, ai, *B, bi, ap, false, hA, cursA, cursB);
                return 0;
            }
            if (g_nd_claim && B->move_microstep >= MS_SETTLED && B->facing_target != hB && B->path_blocked_retry_count == 0) {
                const uint8_t *ea = path_of(ap, A->path_slot_id);
                if (ea[cursA * 2 + 1] >= 2) { // F keeps going straight after this step
                    const uint8_t *eb = path_of(bp, B->path_slot_id);
                    int            e  = cursB;
                    while (e < PATH_ENTRIES - 1 && eb[e * 2] != 0) ++e;
                    if (e < PATH_ENTRIES - 2) { // room for one more entry behind the end sentinel
                        nd_note("claim", ap, ai, bi, sx, sy, dx, dy);
                        do_exchange(*A, ai, *B, bi, ap, true, hA, cursA, cursB);
                        return 0;
                    }
                }
            }
        }
    }
    return -1;
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
namespace {

int wrap_delta(uint32_t from, uint32_t to, uint32_t mask) {
    int d = static_cast<int>((to - from) & mask);
    if (d > static_cast<int>((mask + 1) / 2)) d -= static_cast<int>(mask + 1);
    return d;
}
int sgn(int v) { return v > 0 ? 1 : (v < 0 ? -1 : 0); }

// the principal heading (1,4,..,22) whose step delta is exactly (dx,dy); 0 if none
int principal_heading(int dx, int dy) {
    for (int h = 1; h <= 22; h += 3) {
        int ex, ey;
        heading_delta(h, &ex, &ey);
        if (ex == dx && ey == dy) return h;
    }
    return 0;
}

bool unit_alive(const mh_map_object_unit &u) { return (raw_bits(u.energy) >> 63) == 0 && (raw_bits(u.energy) << 1) != 0; }

// give u a one-leg path onto tile (tx,ty) (adjacent) through the game's own path_make_single_step, whose goal is the target tile for
// the call; the unit's own goal is put back when keep_goal.
bool one_leg(uint32_t player, int32_t idx, mh_map_object_unit &u, uint32_t tx, uint32_t ty, int expect, bool keep_goal) {
    const uint8_t gx = u.goal_x, gy = u.goal_y;
    u.goal_x         = static_cast<uint8_t>(tx);
    u.goal_y         = static_cast<uint8_t>(ty);
    const int32_t ok = mh::call::llm_strat_path_make_single_step(player, idx);
    if (keep_goal) {
        u.goal_x = gx;
        u.goal_y = gy;
    }
    if (!ok) return false;
    if (u.path_slot_id >= 100 || path_of(player, u.path_slot_id)[0] != static_cast<uint8_t>(expect)) {
        mh::call::llm_strat_path_free_slot(static_cast<uint16_t>(player), idx);
        return false;
    }
    return true;
}

// hand a unit that now holds a one-leg path to the walker the way the retail success path of the marshal does
void to_walker(uint32_t player, int32_t idx, mh_map_object_unit &u, const mh_cfg_final_struct_Unit &c) {
    if (u.order == static_cast<uint16_t>(c.move_op_code)) mh::call::llm_unit_set_order_param(static_cast<int32_t>(player), idx, 1);
    const bool cur = idx == static_cast<int32_t>(*reinterpret_cast<const uint16_t *>(ADDR_CUR_INDEX));
    mh::call::llm_strat_unit_set_state_of(static_cast<int32_t>(player), idx, static_cast<int16_t>(cur ? c.move_op_code : ST_HOP));
    u.path_cursor              = 0;
    u.path_blocked_retry_count = 0;
    mh::call::llm_strat_unit_notify_status(player, idx, 100);
}

void face(mh_map_object_unit &u, const mh_cfg_final_struct_Unit &c, uint32_t player, int32_t idx, int h) {
    u.facing_target  = static_cast<uint8_t>(h);
    u.facing_current = static_cast<uint8_t>(h);
    if (c.soldier_count > 0) mh::call::llm_strat_unit_soldiers_set_heading(static_cast<uint16_t>(player), idx, static_cast<uint8_t>(h));
}

// The walker's version of the corridor trade: A (walking, settled, its next leg heading hA at cursor cursA) steps onto B's tile and the idle
// B onto A's. B gets a one-leg path from the game's own path_make_single_step (its goal = A's tile for that call), is turned toward it and
// handed to the walker (state 0xc: it is not the ticking unit); do_exchange claims both tiles crosswise and A's leg shrinks like a normal step.
// B's goal stays the tile it slides to and its order STOP_TO_DEFAULT, so it ends idle there. False = nothing was changed.
bool nd_trade(mh_map_object_unit &A, int32_t ai, mh_map_object_unit &B, int32_t bi, uint32_t player, int hA, int cursA) {
    const mh_cfg_final_struct_Unit *cb = cfg_of(B);
    if (!cb) return false;
    const uint32_t sx = A.x, sy = A.y;
    const int      hB = heading_wrap(hA + 12);
    if (!one_leg(player, bi, B, sx, sy, hB, false)) return false;
    face(B, *cb, player, bi, hB);
    do_exchange(A, ai, B, bi, player, false, hA, cursA, 0);
    to_walker(player, bi, B, *cb);
    B.order = ST_IDLE;
    return true;
}

} // namespace

// returns 1 = handled (skip the retail clock bump), 0 = run retail
static int __cdecl mk_decide(uint32_t player, uint32_t idx) {
    if (!(g_nd_nudge | g_nd_swap)) return 0;
    mh_map_object_unit *A = unit_rec(player, static_cast<int32_t>(idx));
    if (!A) return 0;
    const mh_cfg_final_struct_Unit *ca = cfg_of(*A);
    if (!ca || ca->move_op_code != 0x0f || (A->order != 0x0f && A->order != ST_IDLE)) return 0; // a plain move (order 0x0f, or 1 once the planner has replanned it)
    if (A->state != ST_MARSHAL || A->path_slot_id != 0xff || A->order_queued != 0 || A->move_microstep < MS_SETTLED) return 0;
    const auto    *geom   = reinterpret_cast<const mh_llm_strat_map_geom *>(mh::state::live().base[mh::state::RID_GENERAL]);
    const uint32_t mask_x = geom->width_mask, mask_y = geom->height_mask;
    const int      ddx = wrap_delta(A->x, A->goal_x, mask_x), ddy = wrap_delta(A->y, A->goal_y, mask_y);
    if (!ddx && !ddy) return 0;
    const int h0 = principal_heading(sgn(ddx), sgn(ddy));
    if (!h0) return 0;
    const int32_t ai = static_cast<int32_t>(idx);

    // the tile the unit wants next, then (only when that one is a wall) its two flanks
    const int hs[3] = {h0, heading_wrap(h0 - 3), heading_wrap(h0 + 3)};
    for (int k = 0; k < 3; ++k) {
        int dx, dy;
        heading_delta(hs[k], &dx, &dy);
        const uint32_t                 tx = (A->x + static_cast<uint32_t>(dx)) & mask_x, ty = (A->y + static_cast<uint32_t>(dy)) & mask_y;
        const mh_map_tile_object_data *tT  = tile_of(tx, ty);
        const bool                     occ = (tT->class_owner & 0x80u) != 0 && tT->building != 0;
        if (!occ) {
            if (*pass_of(tx, ty) == 0) continue; // a wall: look at the flank
            return 0;                            // a free tile: the planner's business
        }
        if ((tT->class_owner & 0xfu) != player) return 0;
        const int32_t bi = tT->building;
        if (bi == ai) return 0;
        mh_map_object_unit *B = unit_rec(player, bi);
        if (!B || B->x != tx || B->y != ty) return 0;
        const mh_cfg_final_struct_Unit *cb = cfg_of(*B);
        if (!cb || cb->move_op_code != 0x0f || !unit_alive(*B) || B->move_microstep < MS_SETTLED) return 0;
        const uint32_t sx = A->x, sy = A->y;
        const int      hA = hs[k];
        const int      hB = heading_wrap(hA + 12);

        // ---- an idle friend ----
        if (B->state == ST_STEP_ADJACENT) return 0; // already nudged, not started: keep waiting
        if (g_nd_nudge && B->state == ST_IDLE && cb->default_op_code == ST_IDLE && B->order_queued == 0) {
            uint32_t nx, ny;
            bool     straight = false;
            if (pick_nudge_tile(*B, hA, sx, sy, mask_x, mask_y, &nx, &ny, &straight) && !straight) {
                B->goal_x = static_cast<uint8_t>(nx);
                B->goal_y = static_cast<uint8_t>(ny);
                B->state  = ST_STEP_ADJACENT;
                nd_note("nudge-marshal", player, ai, bi, sx, sy, nx, ny);
                B->order = ST_IDLE;
                if (raw_bits(B->activity_clock) > *reinterpret_cast<const uint64_t *>(ADDR_CLOCK)) memcpy(&B->activity_clock, reinterpret_cast<const void *>(ADDR_CLOCK), 8);
                return 0; // retry soon (retail clock bump)
            }
            // no sidestep: trade tiles
            if (!one_leg(player, ai, *A, tx, ty, hA, true)) return 0;
            const uint8_t bgx = B->goal_x, bgy = B->goal_y;
            if (!one_leg(player, bi, *B, sx, sy, hB, false)) {
                mh::call::llm_strat_path_free_slot(static_cast<uint16_t>(player), ai);
                B->goal_x = bgx;
                B->goal_y = bgy;
                return 0;
            }
            nd_note("swap-marshal", player, ai, bi, sx, sy, tx, ty);
            face(*A, *ca, player, ai, hA);
            face(*B, *cb, player, bi, hB);
            do_exchange(*A, ai, *B, bi, player, false, hA, 0, 0);
            to_walker(player, ai, *A, *ca);
            to_walker(player, bi, *B, *cb);
            B->order = ST_IDLE; // an idle friend stays "default" on arrival (goal = the tile it is sliding to)
            return 1;
        }

        // ---- a friend that wants our tile (head-on) ----
        if (!g_nd_swap || B->order_queued != 0) return 0;
        int  cursB   = 0;
        bool b_stuck = false;
        if (B->state == ST_MARSHAL && B->path_slot_id == 0xff && (B->order == 0x0f || B->order == ST_IDLE)) {
            if (B->move_group_id == A->move_group_id && B->goal_x == A->goal_x && B->goal_y == A->goal_y) return 0; // one group: the planner's
            const int bdx = wrap_delta(B->x, B->goal_x, mask_x), bdy = wrap_delta(B->y, B->goal_y, mask_y);
            if (principal_heading(sgn(bdx), sgn(bdy)) != hB) return 0; // not head-on
            if (!one_leg(player, ai, *A, tx, ty, hA, true)) return 0;
            if (!one_leg(player, bi, *B, sx, sy, hB, true)) {
                mh::call::llm_strat_path_free_slot(static_cast<uint16_t>(player), ai);
                return 0;
            }
            b_stuck = true;
        } else if (B->state == ST_WALK && B->path_slot_id < 100) {
            const int hb = next_leg(*B, player, &cursB);
            if (hb != hB || B->facing_target != hB) return 0;
            if (!one_leg(player, ai, *A, tx, ty, hA, true)) return 0;
        } else {
            return 0;
        }
        nd_note("swap-marshal", player, ai, bi, sx, sy, tx, ty);
        face(*A, *ca, player, ai, hA);
        if (b_stuck) face(*B, *cb, player, bi, hB);
        do_exchange(*A, ai, *B, bi, player, false, hA, 0, cursB);
        to_walker(player, ai, *A, *ca);
        if (b_stuck) to_walker(player, bi, *B, *cb);
        return 1;
    }
    return 0;
}

// clang-format off
__declspec(naked) static void mk_stub() {
    __asm {
        pushad
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
namespace {

constexpr int      RT_MAX = 1024;
uint32_t           g_rt_undo[RT_MAX]; // flat (x << 8 | y) indices whose passable byte was lifted
uint8_t            g_rt_low[RT_MAX];  // the cell's cached cost byte (terrain_flags low byte) before the penalty
int                g_rt_n          = 0;
constexpr uint32_t RT_PENALTY_BITS = 0x1d; // passable + the three proximity rings: costs like a tile pressed against a cliff

uint32_t *rt_cell_flags(uint32_t flat) {
    return reinterpret_cast<uint32_t *>(reinterpret_cast<uint8_t *>(mh::state::live().base[mh::state::RID_MAP_REGION_GRID]) + flat * 8u + 4u);
}

} // namespace

static void __cdecl rt_unmask() {
    g_rt_n = 0;
    if (!g_rt_on || !g_nd_nudge) return; // the nudge / trade is what resolves the meeting: no key, no change
    const auto    &live   = mh::state::live();
    const uint32_t player = *reinterpret_cast<const uint32_t *>(live.base[mh::state::RID_STRAT_GROUP_ORDER_OWNER]);
    const int32_t  goal_x = *reinterpret_cast<const int32_t *>(live.base[mh::state::RID_STRAT_GROUP_ORDER_GOAL_X]);
    const int32_t  goal_y = *reinterpret_cast<const int32_t *>(live.base[mh::state::RID_STRAT_GROUP_ORDER_GOAL_Y]);
    const auto     caps   = mh::state::live_roster_caps();
    const auto    *geom   = reinterpret_cast<const mh_llm_strat_map_geom *>(live.base[mh::state::RID_GENERAL]);
    if (player >= 8 || !geom) return;
    const uint32_t gx = static_cast<uint32_t>(goal_x) & geom->width_mask, gy = static_cast<uint32_t>(goal_y) & geom->height_mask;
    for (int32_t i = 1; i < caps.units && g_rt_n < RT_MAX; ++i) {
        mh_map_object_unit *u = unit_rec(player, i);
        if (!u || u->state != ST_IDLE || u->order_queued != 0 || u->move_microstep < MS_SETTLED || u->origin_tile_was_passable == 0) continue;
        const mh_cfg_final_struct_Unit *c = cfg_of(*u);
        if (!c || c->move_op_code != 0x0f || c->default_op_code != ST_IDLE || !unit_alive(*u)) continue;
        if (u->x == gx && u->y == gy) continue;
        const mh_map_tile_object_data *t = tile_of(u->x, u->y);
        if (t->building != i || t->class_owner != static_cast<uint8_t>(player | 0x80u)) continue;
        uint8_t *p = pass_of(u->x, u->y);
        if (*p != 0) continue;
        *p                  = u->origin_tile_was_passable;
        const uint32_t flat = (static_cast<uint32_t>(u->x) << 8) | u->y;
        g_rt_low[g_rt_n]    = static_cast<uint8_t>(*rt_cell_flags(flat));
        if (g_rt_on == 1) *rt_cell_flags(flat) |= RT_PENALTY_BITS;
        g_rt_undo[g_rt_n++] = flat;
    }
    if (g_rt_n) {
        static int n;
        if (n++ < 200) {
            char b[160];
            wsprintfA(b, "; M1N route-through p%u idle friends opened=%d goal=%d,%d (%d)\n", player, g_rt_n, (int)gx, (int)gy, n);
            seam_log(b);
        }
    }
}

static void __cdecl rt_restore() {
    for (int k = 0; k < g_rt_n; ++k) {
        *(reinterpret_cast<uint8_t *>(mh::state::live().base[mh::state::RID_PASSABLE]) + g_rt_undo[k]) = 0;
        uint32_t *f                                                                                    = rt_cell_flags(g_rt_undo[k]);
        *f                                                                                             = (*f & 0xffffff00u) | g_rt_low[k];
    }
    g_rt_n = 0;
}

// clang-format off
__declspec(naked) static void rt_stub() {
    __asm {
        pushad
        call  rt_unmask
        popad
        call  dword ptr [g_rt_orig_fn]          // EAX = mode, EDX = target mask, exactly as the commit loaded them
        pushad
        call  rt_restore
        popad
        ret
    }
}
// clang-format on

namespace {

bool nd_refuse(const char *key, int v) {
    if (v == 0 || g_nd_state == 1) return false;
    char b[260];
    wsprintfA(b, "; move prototype [net] %s=%d NOT ACTIVE (mods:M1N): %s\n", key, v, nd_state_name(g_nd_state));
    seam_log(b);
    return true;
}

} // namespace

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
}

int mh_set_move_nudge_idle(int on) {
    on         = on != 0;
    g_nd_nudge = 0;
    if (!on || nd_refuse("move_nudge_idle", on)) return 0;
    g_nd_nudge = 1;
    seam_log("; move prototype [net] move_nudge_idle=1 active (mods:M1N): a unit whose next tile holds an idle friendly ground unit asks it to step to a free "
             "neighbour (state 0x36) and waits\n");
    return 1;
}

int mh_set_move_claim_leader(int on) {
    on         = on != 0;
    g_nd_claim = 0;
    if (!on || nd_refuse("move_claim_leader", on)) return 0;
    g_nd_claim = 1;
    seam_log("; move prototype [net] move_claim_leader=1 active (mods:M1N): a follower takes the tile of a friendly leader that is turning in place; the leader "
             "steps back onto the follower's old tile and follows\n");
    return 1;
}

int mh_set_move_swap_headon(int on) {
    on        = on != 0;
    g_nd_swap = 0;
    if (!on || nd_refuse("move_swap_headon", on)) return 0;
    g_nd_swap = 1;
    seam_log("; move prototype [net] move_swap_headon=1 active (mods:M1N): two friendly units meeting head-on both take their next step, through each other's "
             "tile (no sidestep, no replan)\n");
    return 1;
}

int mh_set_move_route_through_idle(int on) {
    g_rt_on = 0;
    if (on == 0) return 0;
    if (g_rt_state != 1) {
        char b[260];
        wsprintfA(b, "; move prototype [net] move_route_through_idle=1 NOT ACTIVE (mods:M1N): %s\n", nd_state_name(g_rt_state));
        seam_log(b);
        return 0;
    }
    g_rt_on = on == 2 ? 2 : 1; // 2 = measurement: open the tiles at the plain tile cost (no penalty)
    seam_log("; move prototype [net] move_route_through_idle=1 active (mods:M1N): the route planner treats a tile held by an idle friendly ground unit as open "
             "(needs move_nudge_idle=1: the unit that arrives there asks the friend to make room); the goal tile itself stays as it is\n");
    return 1;
}
