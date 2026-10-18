//
// seams/move_proto_arrival.cpp -- mods:M1X lane A, the ARRIVAL movement prototypes (movement plan 2a + 2b).
//
//   [net] move_distinct_goals (2a)  every member of a group move gets its OWN free goal tile.
//   [net] move_near_enough    (2b)  a unit whose replan fails next to a goal a friend is parked on stops there at once
//                                   (OpenRA's nearEnough) instead of retrying 4 x 2 s and then giving up.
//
// Both are ALWAYS-installed call-splices into retail code (the M1P pattern in net_lockstep.cpp), each runtime-gated by a
// global its setter flips; with both keys at 0 the stubs run exactly the instructions they replaced. Host-set ([net],
// F_HOSTSYNC): the sim changes, so every peer must run the same values. Neither owner is promoted by libmh at the time of
// writing; if one is, the site is DISPLACED and the key logs NOT ACTIVE.
//
// THE RETAIL PIPELINE (tmp/m1/pipeline.md sections 2 and 4, libmh sim_group_move_order_{commit,pathfind}.cpp and
// sim_unit_state_group_marshal.cpp): llm_strat_unit_state_group_marshal 0x00482451 splits a click's units into waves and
// calls llm_strat_group_move_order_commit 0x0041c86c per wave, which calls llm_strat_group_move_order_pathfind
// 0x0041ca37. For a multi-member wave each member's tile is `goal - (centroid - member)` (the selection's shape moved
// rigidly), its path walks the route at that offset and, when the walk gets stuck, the tile becomes "wherever it got to".
// commit copies those tiles into the marshal scratch and the marshal makes them the members' goals (only when
// move_group_id != 0 and the wave has > 1 member). A single-member wave keeps the CLICK as its goal, so the waves of one
// of a spread selection all aim at the same tile (replay order 16: u2 waits 604 steps in state 0xa and gives up).
//
// 2a -- SITE 0x0041c9d3 in commit, right after its CALL to pathfind: `CMP dword [0x0051de38],1` (7 bytes) becomes
// CALL mv_arr_commit_stub + 2 NOP; the stub runs arrival_after_pathfind() and then the CMP itself (the JLE after it needs
// its flags). Only for a plain move (commit's is_plain_move_flag != 0) with move_group_id != 0 (the normal right-click);
// only members on a plain ground move: order 0x0f (fresh) or 0x01 (STOP_TO_DEFAULT queued -- the marshal good path
// sets it, so every REPLAN of a move runs with order 1; seen in the replay MV lines).
//   * A tile T is FREE when: passable[T] != 0 (no terrain/building/settled unit), discovered by the owner (no fog clamp),
//     REACHABLE (the route flood pathfind just ran from the goal reached T: terrain_flags>>8 not 0 and not 0xffffff;
//     if that field is not fresh -- the goal cell is not the flood seed, value 1 -- T must be in the goal's nav region),
//     and NOT TAKEN: not assigned to an earlier member of this wave, and not the goal of another unit of the same player
//     and move_group_id that already has one (state 0xc/0xf, or 0xa with a goal other than the click).
//   * Multi-member wave: pass 1 keeps every member's formation tile F = goal - (centroid - member) that is free (in
//     member order); pass 2 gives each remaining member the nearest free tile around F (radius 8), else around the
//     goal, else keeps the retail tile. The chosen tile is written to _G_LLM_STRAT_GROUP_MEMBER_TILE, which commit copies
//     and the marshal makes the goal. The member's path is the retail walk (it ends at F, or short of it); a member that
//     ends off its tile replans from there as a wave of one.
//   * Single-member wave: if the route's goal G is not free, the nearest free tile T around G becomes the group goal,
//     pathfind is run AGAIN for T (it frees and re-fills the path slot), and the unit's own goal is set to T.
//   Nearest = smallest dx*dx+dy*dy, scanned ring by ring (Chebyshev), ties by scan order (dx then dy ascending).
//
// 2b -- SITE 0x00482b5b in the marshal's "path exhausted or first step blocked" branch: `INC byte [EAX+0xdd8cf3] ;
// CMP byte [EAX+0xdd8cf3],4` (13 bytes; the unit's path_blocked_retry_count) becomes CALL mv_arr_retry_stub + 8 NOP.
// The stub does the INC, asks arrival_retry_check(), then does the CMP (the JNC after it needs its flags). When the
// check says stop, the counter is set to 4, so the retail give-up branch runs at once: STOP_TO_DEFAULT, notify 0x66,
// passable = 0 under the unit -- exactly what retail does after its 4th retry, without the 4 x 2 s of waiting. Only
// for order 0x0f/0x01. Stop when:
//   * the unit stands ON its goal (an empty route to its own tile; retail retries 4 x 2 s and then stops anyway) --
//     with either key on; or
//   * move_near_enough = R > 0, the goal tile holds a settled unit of the same player that is not walking or
//     regrouping (state not 0xa/0xc/0xf), and the unit is within R + isqrt(N) tiles of the goal (Chebyshev, torus),
//     N = the units of this player alive with the same move_group_id (1 for id 0).
//
// DETERMINISM: integer arithmetic only, units in table order, fixed scan order, no clock, no rand. The only writes are
// _G_LLM_STRAT_GROUP_MEMBER_TILE / GROUP_ORDER_GOAL (the group-move scratch globals, rewritten by every commit), the
// unit's goal_x/goal_y and path_blocked_retry_count (unit record: saved and hashed), and what the second pathfind run
// writes (the same set the first one writes). No new state survives the call. The one energy read compares a double
// with 0.0 as raw bits (no FPU).
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <cstring>

#include "seams/net_internal.h" // seam_log + the setter declarations
#include "hook/patch.h"         // patch_bytes_guarded
#include "hook/promoted.h"      // promoted_owner_of
#include "sim/sim_merge_goal.h" // mods:M1X move_merge_goal_fix: the shared goal search (header-only)

#pragma comment(lib, "user32.lib") // wsprintfA

int g_arr_distinct_rt = 0; // move_distinct_goals in force (0/1)
int g_arr_near_r      = 0; // move_near_enough R in force (0 = off)
int g_arr_merge_rt    = 0; // move_merge_goal_fix in force (0/1)

namespace {

// ---- EN /eng/mh.exe addresses (cross-checked against the EN disassembly and mh_addrs.gen.h) ------------------------
constexpr uintptr_t A_UNITS        = 0x00dd8c48; // map::g::units[8][100], 0xe9 bytes
constexpr uint32_t  UNIT_SIZE      = 0xe9;
constexpr uint32_t  PLAYER_STRIDE  = 0x5b04;
constexpr uintptr_t A_PASSABLE     = 0x00b64bb0; // byte[(x<<8)|y]
constexpr uintptr_t A_FOG_DISC     = 0x00814fa0; // fog_of_war.discovered[(x<<8)|y], bit = player
constexpr uintptr_t A_REGION_GRID  = 0x00688ac4; // {llm_map_region *region; uint32 terrain_flags}[(x<<8)|y]
constexpr uintptr_t A_TILE_OBJECTS = 0x00d1ec80; // 8 B per tile: +2 building/unit index (u16), +6 class_owner
constexpr uintptr_t A_GRP_COUNT    = 0x0051de38; // _G_LLM_STRAT_GROUP_MEMBER_COUNT
constexpr uintptr_t A_GRP_CENT_X   = 0x0051de30; // _G_LLM_STRAT_GROUP_CENTROID_X / _Y (+4)
constexpr uintptr_t A_GRP_MEMBERS  = 0x006804ac; // {uint unit_handle; byte cur_col; byte cur_row}[256]
constexpr uintptr_t A_GRP_TILE     = 0x006802ac; // _G_LLM_STRAT_GROUP_MEMBER_TILE byte[256][2]
constexpr uintptr_t A_WRAP_MASK    = 0x00680aac; // _G_LLM_STRAT_PATH_WRAP_MASK (map width - 1)
constexpr uintptr_t A_GRP_OWNER    = 0x00680ab0; // _G_LLM_STRAT_GROUP_ORDER_OWNER
constexpr uintptr_t A_GOAL_Y       = 0x00680ab4; // _G_LLM_STRAT_GROUP_ORDER_GOAL_Y
constexpr uintptr_t A_GOAL_X       = 0x00680ab8; // _G_LLM_STRAT_GROUP_ORDER_GOAL_X
constexpr uintptr_t A_ANCHOR_Y     = 0x00680abc; // _G_LLM_STRAT_GROUP_ANCHOR_Y (= the click, set by commit)
constexpr uintptr_t A_ANCHOR_X     = 0x00680ac0; // _G_LLM_STRAT_GROUP_ANCHOR_X
constexpr uintptr_t A_CUR_PLAYER   = 0x00e58144; // _G_LLM_STRAT_CUR_PLAYER (u16)
constexpr uintptr_t A_MAP_W        = 0x00825084; // general.width
constexpr uintptr_t A_MAP_H        = 0x00825064; // general.height
constexpr uintptr_t A_PATHFIND     = 0x0041ca37; // llm_strat_group_move_order_pathfind (__watcall EAX=mode, EDX=target)
static_assert(A_PATHFIND == 0x0041ca37u, "call_pathfind's inline asm spells this address literally");

// unit record fields
constexpr uint32_t U_ORDER = 0x04, U_STATE = 0x06, U_ENERGY = 0x18, U_X = 0x84, U_Y = 0x85, U_GOAL_X = 0x86, U_GOAL_Y = 0x87,
                   U_RETRY = 0xab, U_GROUP = 0xc6;
constexpr uint16_t ORDER_MOVE = 0x0f, ORDER_STOP_TO_DEFAULT = 0x01, ST_MARSHAL = 0x0a, ST_MOVE_HOP = 0x0c, ST_MOVE = 0x0f;
constexpr int      SEARCH_R  = 8;   // ring-search radius around a formation tile / the goal
constexpr int      MAX_TAKEN = 128; // other units' goals (at most 100 per player)

template <class T>
T rd(uintptr_t a) { return *reinterpret_cast<const volatile T *>(a); }
template <class T>
void wr(uintptr_t a, T v) { *reinterpret_cast<volatile T *>(a) = v; }
bool is_plain_move(uint16_t order) { return order == ORDER_MOVE || order == ORDER_STOP_TO_DEFAULT; }

uintptr_t unit_at(uint32_t p, uint32_t i) { return A_UNITS + p * PLAYER_STRIDE + i * UNIT_SIZE; }
uint32_t  tidx(uint32_t x, uint32_t y) { return ((x & 0xffu) << 8) | (y & 0xffu); }
uint32_t  region_of(uint32_t t) { return rd<uint32_t>(A_REGION_GRID + t * 8); }
uint32_t  field_of(uint32_t t) { return rd<uint32_t>(A_REGION_GRID + t * 8 + 4) >> 8; }

// One commit's view: what makes a tile acceptable as a member's goal.
struct pick_ctx {
    uint32_t owner_bit;
    bool     field_fresh; // the region flood was last seeded at the goal
    uint32_t goal_region; // fallback reachability: same nav region as the goal
    uint32_t taken[MAX_TAKEN + 256];
    int      n_taken;
    uint32_t mask; // path wrap mask (map width - 1)
};

bool is_taken(const pick_ctx &c, uint32_t t) {
    for (int k = 0; k < c.n_taken; ++k)
        if (c.taken[k] == t) return true;
    return false;
}

bool tile_free(const pick_ctx &c, uint32_t t) {
    if (rd<uint8_t>(A_PASSABLE + t) == 0) return false;
    if ((rd<uint8_t>(A_FOG_DISC + t) & c.owner_bit) == 0) return false;
    if (c.field_fresh) {
        const uint32_t f = field_of(t);
        if (f == 0 || f == 0xffffffu) return false;
    } else {
        const uint32_t r = region_of(t);
        if (r == 0 || r != c.goal_region) return false;
    }
    return !is_taken(c, t);
}

// Nearest free tile around (cx,cy) within SEARCH_R (not the centre itself): ring by ring, smallest dx*dx+dy*dy, ties by
// scan order. Returns 0xffffffff when none.
uint32_t search_near(const pick_ctx &c, uint32_t cx, uint32_t cy) {
    uint32_t best = 0xffffffffu;
    int      bd2  = 0x7fffffff;
    for (int r = 1; r <= SEARCH_R; ++r) {
        for (int dx = -r; dx <= r; ++dx) {
            for (int dy = -r; dy <= r; ++dy) {
                if (dx != -r && dx != r && dy != -r && dy != r) continue; // ring r only
                const int d2 = dx * dx + dy * dy;
                if (d2 >= bd2) continue;
                const uint32_t t = tidx((cx + (uint32_t)dx) & c.mask, (cy + (uint32_t)dy) & c.mask);
                if (!tile_free(c, t)) continue;
                best = t;
                bd2  = d2;
            }
        }
        if (best != 0xffffffffu && bd2 <= (r + 1) * (r + 1)) break; // no tile further out can be nearer
    }
    return best;
}

void call_pathfind(uint32_t mode, uint32_t target) {
    __asm {
        pushad
        mov   eax, mode
        mov   edx, target
        mov   ecx, 0x0041ca37 // A_PATHFIND (inline asm cannot take a constexpr as an immediate)
        call  ecx
        popad
    }
}

// Runs at commit 0x0041c9d3, after pathfind returned. Arguments are commit's own stack parameters.
void __cdecl arrival_after_pathfind(int32_t group_id, uint32_t mode, uint32_t target) {
    if (!g_arr_distinct_rt || group_id == 0 || mode == 0) return;
    const int32_t n = rd<int32_t>(A_GRP_COUNT);
    if (n < 1 || n > 256) return;
    const uint32_t owner = rd<uint32_t>(A_GRP_OWNER) & 7u;

    pick_ctx c; // this call's scratch, on the stack: nothing survives the call
    c.mask            = rd<uint32_t>(A_WRAP_MASK) & 0xffu;
    c.owner_bit       = 1u << owner;
    const uint32_t gx = rd<uint32_t>(A_GOAL_X) & c.mask, gy = rd<uint32_t>(A_GOAL_Y) & c.mask;
    const uint32_t g     = tidx(gx, gy);
    c.field_fresh        = field_of(g) == 1u;
    c.goal_region        = region_of(g);
    const uint32_t click = tidx(rd<uint32_t>(A_ANCHOR_X) & c.mask, rd<uint32_t>(A_ANCHOR_Y) & c.mask);

    // Goals other units of this group already hold (not this wave's members).
    c.n_taken = 0;
    for (uint32_t i = 0; i < 100; ++i) {
        bool member = false;
        for (int32_t m = 0; m < n; ++m)
            if (rd<uint32_t>(A_GRP_MEMBERS + (uint32_t)m * 6) == i) {
                member = true;
                break;
            }
        if (member) continue;
        const uintptr_t u = unit_at(owner, i);
        if (rd<int32_t>(u + U_GROUP) != group_id) continue;
        const uint16_t st   = rd<uint16_t>(u + U_STATE);
        const uint32_t goal = tidx(rd<uint8_t>(u + U_GOAL_X), rd<uint8_t>(u + U_GOAL_Y));
        if (!(st == ST_MOVE || st == ST_MOVE_HOP || (st == ST_MARSHAL && goal != click))) continue;
        if (c.n_taken < MAX_TAKEN) c.taken[c.n_taken++] = goal;
    }

    int kept = 0, moved = 0, failed = 0;
    if (n == 1) {
        const uint32_t  ui = rd<uint32_t>(A_GRP_MEMBERS);
        const uintptr_t u  = unit_at(owner, ui % 100u);
        if (ui >= 100 || !is_plain_move(rd<uint16_t>(u + U_ORDER))) return;
        if (tile_free(c, g)) return; // the route's goal is free: retail
        const uint32_t t = search_near(c, gx, gy);
        if (t == 0xffffffffu) {
            failed = 1;
        } else {
            wr<int32_t>(A_GOAL_X, (int32_t)(t >> 8));
            wr<int32_t>(A_GOAL_Y, (int32_t)(t & 0xffu));
            call_pathfind(mode, target); // re-plan the wave of one to T (frees + refills its path slot)
            wr<uint8_t>(u + U_GOAL_X, (uint8_t)(t >> 8));
            wr<uint8_t>(u + U_GOAL_Y, (uint8_t)(t & 0xffu));
            moved = 1;
        }
        char b[200];
        wsprintfA(b, "; arrival (mods:M1X) p%u u%u single: goal (%u,%u) taken/blocked -> %s (%u,%u)\n", owner, ui, gx, gy,
                  moved ? "moved to" : "no free tile, kept", moved ? (t >> 8) : gx, moved ? (t & 0xffu) : gy);
        seam_log(b);
        return;
    }

    // Multi-member wave.
    const uint32_t cx = rd<uint32_t>(A_GRP_CENT_X) & 0xffu, cy = rd<uint32_t>(A_GRP_CENT_X + 4) & 0xffu;
    uint32_t       form[256];
    uint8_t        state[256]; // 0 = not ours (keep retail tile), 1 = pending, 2 = assigned
    for (int32_t m = 0; m < n; ++m) {
        const uintptr_t mem = A_GRP_MEMBERS + (uint32_t)m * 6;
        const uint32_t  ui  = rd<uint32_t>(mem);
        const uint8_t   dc  = (uint8_t)((uint8_t)cx - rd<uint8_t>(mem + 4));
        const uint8_t   dr  = (uint8_t)((uint8_t)cy - rd<uint8_t>(mem + 5));
        form[m]             = tidx((uint8_t)((uint8_t)gx - dc) & c.mask, (uint8_t)((uint8_t)gy - dr) & c.mask);
        state[m]            = (ui < 100 && is_plain_move(rd<uint16_t>(unit_at(owner, ui) + U_ORDER))) ? 1 : 0;
        if (state[m] == 0) c.taken[c.n_taken++] = tidx(rd<uint8_t>(A_GRP_TILE + m * 2), rd<uint8_t>(A_GRP_TILE + m * 2 + 1));
    }
    for (int32_t m = 0; m < n; ++m) { // pass 1: formation tiles that are free
        if (state[m] != 1 || !tile_free(c, form[m])) continue;
        state[m]             = 2;
        c.taken[c.n_taken++] = form[m];
        const uint32_t old   = tidx(rd<uint8_t>(A_GRP_TILE + m * 2), rd<uint8_t>(A_GRP_TILE + m * 2 + 1));
        if (old != form[m]) ++moved;
        else ++kept;
        wr<uint8_t>(A_GRP_TILE + m * 2, (uint8_t)(form[m] >> 8));
        wr<uint8_t>(A_GRP_TILE + m * 2 + 1, (uint8_t)(form[m] & 0xffu));
    }
    for (int32_t m = 0; m < n; ++m) { // pass 2: the rest take the nearest free tile
        if (state[m] != 1) continue;
        uint32_t t = search_near(c, form[m] >> 8, form[m] & 0xffu);
        if (t == 0xffffffffu) t = tile_free(c, g) ? g : search_near(c, gx, gy);
        const uint32_t old = tidx(rd<uint8_t>(A_GRP_TILE + m * 2), rd<uint8_t>(A_GRP_TILE + m * 2 + 1));
        if (t == 0xffffffffu) {
            ++failed;
            t = old; // retail tile
        } else {
            ++moved;
            wr<uint8_t>(A_GRP_TILE + m * 2, (uint8_t)(t >> 8));
            wr<uint8_t>(A_GRP_TILE + m * 2 + 1, (uint8_t)(t & 0xffu));
        }
        c.taken[c.n_taken++] = t;
        state[m]             = 2;
    }
    char b[200];
    wsprintfA(b, "; arrival (mods:M1X) p%u wave n=%d goal (%u,%u) field %s: kept %d, moved %d, no free tile %d\n", owner, n, gx, gy,
              c.field_fresh ? "fresh" : "stale", kept, moved, failed);
    seam_log(b);
}

uint32_t isqrt(uint32_t v) {
    uint32_t r = 0;
    while ((r + 1) * (r + 1) <= v) ++r;
    return r;
}

int wrap_dist(int a, int b, int size) {
    int d = a - b;
    if (d < 0) d = -d;
    if (size > 0 && d > size / 2) d = size - d;
    return d;
}

// Runs at marshal 0x00482b5b after the retry INC. `unit_off` = player*0x5b04 + index*0xe9 (the marshal's EAX).
void __cdecl arrival_retry_check(uint32_t unit_off) {
    const uint32_t  p = rd<uint16_t>(A_CUR_PLAYER) & 7u;
    const uintptr_t u = A_UNITS + unit_off;
    if (!is_plain_move(rd<uint16_t>(u + U_ORDER))) return;
    const uint32_t idx = (unit_off - p * PLAYER_STRIDE) / UNIT_SIZE;
    const int      x = rd<uint8_t>(u + U_X), y = rd<uint8_t>(u + U_Y), gx = rd<uint8_t>(u + U_GOAL_X), gy = rd<uint8_t>(u + U_GOAL_Y);
    const char    *why = nullptr;
    if (x == gx && y == gy) {
        why = "on its goal";
    } else if (g_arr_near_r > 0) {
        const uint32_t g = tidx((uint32_t)gx, (uint32_t)gy);
        if (rd<uint8_t>(A_PASSABLE + g) != 0) return;
        if (rd<uint8_t>(A_TILE_OBJECTS + g * 8 + 6) != (uint8_t)(0x80u | p)) return;
        const uint32_t occ = rd<uint16_t>(A_TILE_OBJECTS + g * 8 + 2);
        if (occ >= 100 || occ == idx) return;
        const uint16_t ost = rd<uint16_t>(unit_at(p, occ) + U_STATE);
        if (ost == ST_MARSHAL || ost == ST_MOVE_HOP || ost == ST_MOVE) return;
        const int32_t gid = rd<int32_t>(u + U_GROUP);
        uint32_t      n   = 1;
        if (gid != 0) {
            n = 0;
            for (uint32_t i = 0; i < 100; ++i) {
                const uintptr_t w = unit_at(p, i);
                const uint64_t  e = rd<uint64_t>(w + U_ENERGY); // energy > 0.0, read as bits (no x87/SSE use)
                if (rd<int32_t>(w + U_GROUP) == gid && (e >> 63) == 0 && (e & 0x7fffffffffffffffull) != 0) ++n;
            }
            if (n < 1) n = 1;
        }
        const int dx = wrap_dist(x, gx, rd<int32_t>(A_MAP_W)), dy = wrap_dist(y, gy, rd<int32_t>(A_MAP_H));
        const int d = dx > dy ? dx : dy;
        if (d > g_arr_near_r + (int)isqrt(n)) return;
        why = "near enough to a goal a friend holds";
    } else {
        return;
    }
    wr<uint8_t>(u + U_RETRY, 4); // the retail give-up branch runs now
    char b[200];
    wsprintfA(b, "; arrival (mods:M1X) p%u u%u stops at (%d,%d), goal (%d,%d): %s\n", p, idx, x, y, gx, gy, why);
    seam_log(b);
}

// move_merge_goal_fix (mods:M1X, sim/sim_merge_goal.h): runs in place of the CALL llm_strat_pathfind_find_closer_visible_tile
// at 0x0041cc69 (group_move_order_pathfind step 2: the goal is impassable or undiscovered). Same arguments as the callee:
// goal col/row, the two out pointers (the ORDER_GOAL globals), the group centre col/row. Returns 1 = handled (the outs hold the
// new goal; the stub returns 0 = found to the caller), 0 = run the retail pick.
uint32_t g_mg_lines = 0;
int __cdecl merge_goal_pick(uint32_t gx, uint32_t gy, uint32_t *ox, uint32_t *oy, uint32_t cx, uint32_t cy) {
    const uint32_t owner = rd<uint32_t>(A_GRP_OWNER) & 7u;
    const int32_t  n     = rd<int32_t>(A_GRP_COUNT);
    if (n < 1 || n > 256) return 0;
    const uint32_t ui = rd<uint32_t>(A_GRP_MEMBERS);
    if (ui >= 100 || rd<uint16_t>(unit_at(owner, ui) + U_ORDER) != mh::sim::MERGE_GOAL_ORDER) return 0;
    const uint32_t w = rd<uint32_t>(A_MAP_W), h = rd<uint32_t>(A_MAP_H), bit = 1u << owner;
    uint32_t       tx = 0, ty = 0;
    const bool     ok = mh::sim::merge_goal_search(
        cx, cy, gx, gy, w, h, [](uint32_t t) { return rd<uint8_t>(A_PASSABLE + t) != 0; },
        [bit](uint32_t t) { return (rd<uint8_t>(A_FOG_DISC + t) & bit) != 0; }, &tx, &ty);
    if (g_mg_lines < 200) {
        ++g_mg_lines;
        char b[200];
        wsprintfA(b, "; merge goal (mods:M1X) p%u u%u from (%u,%u) target (%u,%u) -> %s (%u,%u)\n", owner, ui, cx & 0xffu, cy & 0xffu, gx & 0xffu,
                  gy & 0xffu, ok ? "nearest reachable" : "none found, retail pick", tx, ty);
        seam_log(b);
    }
    if (!ok) return 0;
    *ox = tx;
    *oy = ty;
    return 1;
}

uintptr_t g_fn_closer_visible = 0x0041fff5; // llm_strat_pathfind_find_closer_visible_tile (read by the stub)
int       g_mg_handled        = 0;          // merge_goal_pick's verdict, carried across the stub's POPAD (sim thread only)

// clang-format off
__declspec(naked) void mv_arr_commit_stub() {
    __asm {
        cmp   g_arr_distinct_rt, 0
        je    ac_done
        pushad
        push  dword ptr [ebp + 0x10]             // commit's target_class
        push  dword ptr [ebp + 0x0c]             // commit's is_plain_move_flag
        push  dword ptr [ebp + 0x08]             // commit's move_group_id
        call  arrival_after_pathfind
        add   esp, 12
        popad
    ac_done:
        cmp   dword ptr ds:[0x0051de38], 1       // the instruction this stub replaced (its flags feed the JLE)
        ret
    }
}

// Entry as the callee: EAX/EDX goal col/row, EBX/ECX the out pointers, [ESP+4]/[ESP+8] centre col/row; callee pops 8.
__declspec(naked) void mv_merge_goal_stub() {
    __asm {
        cmp   g_arr_merge_rt, 0
        je    mg_retail
        pushad
        push  dword ptr [esp + 40]               // centre row  (32 pushad + ret + col)
        push  dword ptr [esp + 40]               // centre col  (shifted by the push above)
        push  ecx
        push  ebx
        push  edx
        push  eax
        call  merge_goal_pick
        add   esp, 24
        mov   g_mg_handled, eax                  // the registers (EAX = goal col) must reach the retail callee intact
        popad
        cmp   g_mg_handled, 0
        je    mg_retail
        xor   eax, eax                           // 0 = found, as the callee returns
        ret   8
    mg_retail:
        jmp   dword ptr [g_fn_closer_visible]
    }
}

__declspec(naked) void mv_arr_retry_stub() {
    __asm {
        inc   byte ptr [eax + 0xdd8cf3]          // path_blocked_retry_count++ (replaced)
        cmp   g_arr_distinct_rt, 0
        jne   ar_check
        cmp   g_arr_near_r, 0
        je    ar_done
    ar_check:
        pushad
        push  eax
        call  arrival_retry_check
        add   esp, 4
        popad
    ar_done:
        cmp   byte ptr [eax + 0xdd8cf3], 4       // replaced; its flags feed the JNC
        ret
    }
}
// clang-format on

constexpr uintptr_t ADDR_ARR_COMMIT = 0x0041c9d3, ADDR_ARR_RETRY = 0x00482b5b;
constexpr uintptr_t ADDR_MV_MERGE_GOAL      = 0x0041cc69;                                                                     // CALL llm_strat_pathfind_find_closer_visible_tile
const uint8_t       MV_MERGE_GOAL_EXPECT[5] = {0xE8, 0x87, 0x33, 0x00, 0x00};                                                 // E8 rel32 -> 0x0041fff5
const uint8_t       ARR_COMMIT_EXPECT[7]    = {0x83, 0x3D, 0x38, 0xDE, 0x51, 0x00, 0x01};                                     // CMP dword [0x0051de38],1
const uint8_t       ARR_RETRY_EXPECT[13]    = {0xFE, 0x80, 0xF3, 0x8C, 0xDD, 0x00, 0x80, 0xB8, 0xF3, 0x8C, 0xDD, 0x00, 0x04}; // INC/CMP retry

int g_arr_state_commit = 0, g_arr_state_retry = 0, g_arr_state_merge = 0; // 1 carried, -1 MISMATCHED, -2 DISPLACED, 0 not installed

const char *arr_state_name(int s) {
    return s == 1 ? "carried" : (s == -1 ? "a splice MISMATCHED" : (s == -2 ? "DISPLACED (libmh owns the function; no twin)" : "not installed"));
}

int arr_patch(uintptr_t addr, const uint8_t *expect, int len, void (*stub)()) {
    if (mh::hook::promoted_owner_of(addr)) return -2;
    uint8_t       repl[16];
    const int32_t rel = (int32_t)((uintptr_t)stub - (addr + 5));
    repl[0]           = 0xE8;
    memcpy(repl + 1, &rel, sizeof(rel));
    for (int i = 5; i < len; ++i) repl[i] = 0x90;
    bool ok = false;
    // one literal expression per site: lint_dll_patches keys the manifest on (file, expr)
    switch (addr) {
        case ADDR_ARR_COMMIT: ok = mh::hook::patch_bytes_guarded(ADDR_ARR_COMMIT, expect, repl, len); break;
        case ADDR_ARR_RETRY: ok = mh::hook::patch_bytes_guarded(ADDR_ARR_RETRY, expect, repl, len); break;
        case ADDR_MV_MERGE_GOAL: ok = mh::hook::patch_bytes_guarded(ADDR_MV_MERGE_GOAL, expect, repl, len); break;
    }
    return ok ? 1 : -1;
}

void arr_log_key(const char *key, int v, int state, const char *what) {
    char b[360];
    if (state == 1) wsprintfA(b, "; move prototype [net] %s=%d active (mods:M1X): %s\n", key, v, what);
    else wsprintfA(b, "; move prototype [net] %s=%d NOT ACTIVE (mods:M1X): %s\n", key, v, arr_state_name(state));
    seam_log(b);
}

} // namespace

// Called once from install_move_proto (net_lockstep.cpp).
void install_move_proto_arrival() {
    g_arr_state_commit = arr_patch(ADDR_ARR_COMMIT, ARR_COMMIT_EXPECT, 7, mv_arr_commit_stub);
    g_arr_state_retry  = arr_patch(ADDR_ARR_RETRY, ARR_RETRY_EXPECT, 13, mv_arr_retry_stub);
    g_arr_state_merge  = arr_patch(ADDR_MV_MERGE_GOAL, MV_MERGE_GOAL_EXPECT, 5, mv_merge_goal_stub);
    char b[320];
    wsprintfA(b, "; move prototype splices (mods:M1X arrival): commit %s, marshal retry %s, merge goal %s\n", arr_state_name(g_arr_state_commit),
              arr_state_name(g_arr_state_retry), arr_state_name(g_arr_state_merge));
    seam_log(b);
}

// move_distinct_goals: 0 retail, 1 on. Needs BOTH sites (the retry site carries its "on its goal" stop).
int mh_set_move_distinct_goals(int on) {
    on                = on != 0;
    const int st      = (g_arr_state_commit == 1 && g_arr_state_retry == 1) ? 1 : (g_arr_state_commit != 1 ? g_arr_state_commit : g_arr_state_retry);
    g_arr_distinct_rt = on && st == 1;
    if (on) arr_log_key("move_distinct_goals", 1, st, "every group member gets its own free goal tile (formation tile kept when free)");
    return g_arr_distinct_rt == on;
}

// move_near_enough: R tiles (0 = retail), clamped 0..10.
int mh_set_move_near_enough(int r) {
    r            = r < 0 ? 0 : (r > 10 ? 10 : r);
    g_arr_near_r = (r > 0 && g_arr_state_retry == 1) ? r : 0;
    if (r > 0) arr_log_key("move_near_enough", r, g_arr_state_retry, "a failed replan within R + sqrt(group) of a goal a parked friend holds stops there");
    return (g_arr_near_r > 0) == (r > 0);
}

// move_merge_goal_fix: 0 retail, 1 on. DISPLACED (libmh promotes group_move_order_pathfind) = its twin carries the key.
int mh_set_move_merge_goal_fix(int on) {
    on             = on != 0;
    g_arr_merge_rt = on && g_arr_state_merge == 1;
    if (on) {
        if (g_arr_state_merge == -2)
            seam_log("; move prototype [net] move_merge_goal_fix=1 carried by libmh (group_move_order_pathfind promoted)\n");
        else
            arr_log_key("move_merge_goal_fix", 1, g_arr_state_merge,
                        "a squad merge walks to the nearest reachable tile next to the target soldier (retail: the first closer tile of a spiral)");
    }
    return (g_arr_merge_rt == on) || g_arr_state_merge == -2;
}
