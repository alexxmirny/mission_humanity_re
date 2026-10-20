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

#include "seams/net_internal.h"     // seam_log + the setter declarations
#include "seams/move_carrier.h"     // mods:M1H: site states, requested values, stub hit counters
#include "hook/patch.h"             // patch_bytes_guarded
#include "hook/promoted.h"          // promoted_owner_of
#include "sim/sim_merge_goal.h"     // mods:M1X move_merge_goal_fix: the shared goal search (header-only)
#include "sim/move/arrival.h"       // mods:M1H move_distinct_goals: the shared decision (header-only)
#include "sim/move/near_enough.h"   // mods:M1H move_near_enough (2b) + the on-goal stop: the shared decision (header-only)
#include "sim/move/route_through.h" // mods:M1G 4b: the undo list the formation reads (true_passable)
#include "sim/move/formation.h"     // mods:M1G 4b move_formation_click: the shared decision (header-only)

#pragma comment(lib, "user32.lib") // wsprintfA

int            g_arr_distinct_rt = 0; // move_distinct_goals in force (0/1)
int            g_arr_near_r      = 0; // move_near_enough R in force (0 = off)
int            g_arr_merge_rt    = 0; // move_merge_goal_fix in force (0/1)
extern "C" int g_fc_on;               // move_formation_click in force (0 off, 1..3; defined with the 0x0041c9ce stub, move_proto_nudge.cpp)
extern "C" int g_fc_handled;          // the formation handled this commit: the 2a stub below skips arrival_after_pathfind once (set by mv_fc_run's caller)

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
constexpr uint32_t U_ORDER = 0x04, U_STATE = 0x06, U_X = 0x84, U_Y = 0x85, U_GOAL_X = 0x86, U_GOAL_Y = 0x87,
                   U_PATH_SLOT = 0xaa, U_RETRY = 0xab, U_GROUP = 0xc6;
constexpr uintptr_t A_PATHBUF = 0x00aee8c0; // _G_LLM_STRAT_PATH_BUFFERS: player * 0xea60 + slot * 0x258 + entry * 2, {byte heading; byte run_length}

template <class T>
T rd(uintptr_t a) { return *reinterpret_cast<const volatile T *>(a); }
template <class T>
void wr(uintptr_t a, T v) { *reinterpret_cast<volatile T *>(a) = v; }

uintptr_t unit_at(uint32_t p, uint32_t i) { return A_UNITS + p * PLAYER_STRIDE + i * UNIT_SIZE; }
uint32_t  tidx(uint32_t x, uint32_t y) { return ((x & 0xffu) << 8) | (y & 0xffu); }
uint32_t  region_of(uint32_t t) { return rd<uint32_t>(A_REGION_GRID + t * 8); }
uint32_t  field_of(uint32_t t) { return rd<uint32_t>(A_REGION_GRID + t * 8 + 4) >> 8; }

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

// The decision is sim/move/arrival.h (shared with the libmh twin, sim_group_move_order_commit.cpp); this is its live-memory World.
struct arrival_live_world {
    uint32_t path_mask() { return rd<uint32_t>(A_WRAP_MASK); }
    uint32_t owner() { return rd<uint32_t>(A_GRP_OWNER); }
    int32_t  member_count() { return rd<int32_t>(A_GRP_COUNT); }
    int32_t  unit_limit() { return 100; } // the splice's unit_at() strides are the stock 100-slot rows
    uint32_t goal_x() { return rd<uint32_t>(A_GOAL_X); }
    uint32_t goal_y() { return rd<uint32_t>(A_GOAL_Y); }
    void     set_goal(int32_t x, int32_t y) {
        wr<int32_t>(A_GOAL_X, x);
        wr<int32_t>(A_GOAL_Y, y);
    }
    uint32_t anchor_x() { return rd<uint32_t>(A_ANCHOR_X); }
    uint32_t anchor_y() { return rd<uint32_t>(A_ANCHOR_Y); }
    uint32_t centroid_x() { return rd<uint32_t>(A_GRP_CENT_X); }
    uint32_t centroid_y() { return rd<uint32_t>(A_GRP_CENT_X + 4); }
    uint32_t member_handle(int32_t m) { return rd<uint32_t>(A_GRP_MEMBERS + static_cast<uint32_t>(m) * 6); }
    uint8_t  member_col(int32_t m) { return rd<uint8_t>(A_GRP_MEMBERS + static_cast<uint32_t>(m) * 6 + 4); }
    uint8_t  member_row(int32_t m) { return rd<uint8_t>(A_GRP_MEMBERS + static_cast<uint32_t>(m) * 6 + 5); }
    uint8_t  tile_byte(int32_t m, int k) { return rd<uint8_t>(A_GRP_TILE + static_cast<uint32_t>(m) * 2 + static_cast<uint32_t>(k)); }
    void     set_tile_byte(int32_t m, int k, uint8_t b) { wr<uint8_t>(A_GRP_TILE + static_cast<uint32_t>(m) * 2 + static_cast<uint32_t>(k), b); }
    uint8_t  passable(uint32_t t) { return rd<uint8_t>(A_PASSABLE + t); }
    uint8_t  fog(uint32_t t) { return rd<uint8_t>(A_FOG_DISC + t); }
    uint32_t region_of(uint32_t t) { return ::region_of(t); }
    uint32_t field_of(uint32_t t) { return ::field_of(t); }
    bool     unit(uint32_t owner_, uint32_t i, mh::sim::move::arr_unit *out) {
        const uintptr_t u = unit_at(owner_, i);
        out->order        = rd<uint16_t>(u + U_ORDER);
        out->state        = rd<uint16_t>(u + U_STATE);
        out->goal_x       = rd<uint8_t>(u + U_GOAL_X);
        out->goal_y       = rd<uint8_t>(u + U_GOAL_Y);
        out->group        = rd<int32_t>(u + U_GROUP);
        out->x            = rd<uint8_t>(u + U_X);
        out->y            = rd<uint8_t>(u + U_Y);
        out->path_slot    = rd<uint8_t>(u + U_PATH_SLOT);
        return true;
    }
    void set_unit_goal(uint32_t owner_, uint32_t i, uint8_t x, uint8_t y) {
        const uintptr_t u = unit_at(owner_, i);
        wr<uint8_t>(u + U_GOAL_X, x);
        wr<uint8_t>(u + U_GOAL_Y, y);
    }
    // mods:M1DT fix A' (rc17 dig, cause A): the re-run is bracketed like the first run, so settled walkers / 0x0a units / idle friends the
    // route-through keys open for the first run are open for this one too (they were walls, and the re-plan took the long way round).
    // mv_rt_open / mv_rt_close are no-ops with every route-through key off.
    void rerun_pathfind(uint32_t mode, uint32_t target) {
        mv_rt_open();
        call_pathfind(mode, target); // 0x0041ca37 directly; the bracket is the one around it
        mv_rt_close();
    }
    void log_single(uint32_t owner_, uint32_t ui, uint32_t gx, uint32_t gy, bool moved, uint32_t tx, uint32_t ty) {
        char b[200];
        wsprintfA(b, "; arrival (mods:M1X) p%u u%u single: goal (%u,%u) taken/blocked -> %s (%u,%u)\n", owner_, ui, gx, gy,
                  moved ? "moved to" : "no free tile, kept", tx, ty);
        seam_log(b);
    }
    void log_wave(uint32_t owner_, int32_t n, uint32_t gx, uint32_t gy, bool fresh, int kept, int moved, int failed) {
        char b[200];
        wsprintfA(b, "; arrival (mods:M1X) p%u wave n=%d goal (%u,%u) field %s: kept %d, moved %d, no free tile %d\n", owner_, n, gx, gy,
                  fresh ? "fresh" : "stale", kept, moved, failed);
        seam_log(b);
    }
};

// mods:M1G 4b move_formation_click: arrival.h's World plus what sim/move/formation.h reads (live memory, like its parent).
struct formation_live_world : arrival_live_world {
    const mh::sim::move::rt_undo *undo = nullptr; // the route-through bracket's undo list: the passable bytes as they were BEFORE it opened tiles
    bool                          true_passable(uint32_t t) {
        for (int k = 0; k < undo->n; ++k)
            if (undo->flat[k] == t) return undo->pass[k] != 0;
        return passable(t) != 0;
    }
    // 0 = nobody, u >= 1 = a unit of `owner_` stands there, -1 = a unit mark of anyone else
    int32_t occupant(uint32_t owner_, uint32_t t) {
        const auto &o = *reinterpret_cast<const mh::game::mh_map_tile_object_data *>(A_TILE_OBJECTS + t * 8u);
        if ((o.class_owner & 0x80u) == 0 || o.building == 0) return 0;
        return (o.class_owner & 0xfu) == owner_ ? static_cast<int32_t>(o.building) : -1;
    }
    bool unit(uint32_t owner_, uint32_t i, mh::sim::move::arr_unit *out) { return arrival_live_world::unit(owner_, i, out); }
    void set_member_count(int32_t n) { wr<int32_t>(A_GRP_COUNT, n); }
    void set_member(int32_t m, uint32_t handle, uint8_t col, uint8_t row) {
        const uintptr_t e = A_GRP_MEMBERS + static_cast<uint32_t>(m) * 6;
        wr<uint32_t>(e, handle);
        wr<uint8_t>(e + 4, col);
        wr<uint8_t>(e + 5, row);
    }
    void set_centroid(uint32_t x, uint32_t y) {
        wr<uint32_t>(A_GRP_CENT_X, x);
        wr<uint32_t>(A_GRP_CENT_X + 4, y);
    }
    void     run_pathfind(uint32_t mode, uint32_t target) { call_pathfind(mode, target); } // the bracket is already open
    uint32_t path_first_run(uint32_t owner_, uint32_t slot) { return rd<uint8_t>(A_PATHBUF + owner_ * 0xea60u + slot * 0x258u + 1u); }
    void     path_terminate(uint32_t owner_, uint32_t slot) { wr<uint16_t>(A_PATHBUF + owner_ * 0xea60u + slot * 0x258u, 0); }
    void     log_assign(uint32_t owner_, uint32_t ui, uint32_t from, uint32_t to, int how) {
        static int lines;
        if (lines++ >= 400) return;
        char b[160];
        wsprintfA(b, "; M1G FC assign p%u u%u (%u,%u) -> (%u,%u) %s\n", owner_, ui, from >> 8, from & 0xffu, to >> 8, to & 0xffu, how == 1 ? "kept" : (how == 2 ? "moved" : "retail"));
        seam_log(b);
    }
    void log_adjust(uint32_t owner_, uint32_t ui, uint32_t wanted, uint32_t got) {
        static int lines;
        if (lines++ >= 200) return;
        char b[160];
        wsprintfA(b, "; M1G FC adjust p%u u%u wanted (%u,%u) got (%u,%u)\n", owner_, ui, wanted >> 8, wanted & 0xffu, got >> 8, got & 0xffu);
        seam_log(b);
    }
    void log_fc(uint32_t owner_, int n, int kept, int filled, int retail, int rk, int replans, int reused) {
        static int lines;
        if (lines++ >= 200) return;
        char b[200];
        wsprintfA(b, "; M1G FC p%u n=%d kept=%d filled=%d retail=%d rk=%d replans=%d reused=%d\n", owner_, n, kept, filled, retail, rk, replans, reused);
        seam_log(b);
    }
};

// Runs at commit 0x0041c9d3, after pathfind returned. Arguments are commit's own stack parameters.
void __cdecl arrival_after_pathfind(int32_t group_id, uint32_t mode, uint32_t target) {
    if (!g_arr_distinct_rt) return;
    arrival_live_world w;
    mh::sim::move::arrival_after_pathfind(w, group_id, mode, target);
}

// The decision is sim/move/near_enough.h (shared with the libmh twin, sim_unit_state_group_marshal.cpp); this is its live-memory World.
// Raw stock 100-slot rows (unit_at / A_TILE_OBJECTS), exactly what the splice always read: the retail marshal's EAX is a stock offset.
struct ne_live_world {
    const mh::game::mh_map_object_unit *unit(uint32_t p, int32_t i) {
        if (p >= 8 || i < 0 || i >= mh::sim::mvc::NE_ROSTER) return nullptr;
        return reinterpret_cast<const mh::game::mh_map_object_unit *>(unit_at(p, static_cast<uint32_t>(i)));
    }
    uint8_t                                  pass(uint32_t x, uint32_t y) { return rd<uint8_t>(A_PASSABLE + tidx(x, y)); }
    const mh::game::mh_map_tile_object_data &tile(uint32_t x, uint32_t y) { return *reinterpret_cast<const mh::game::mh_map_tile_object_data *>(A_TILE_OBJECTS + tidx(x, y) * 8u); }
    int32_t                                  map_w() { return rd<int32_t>(A_MAP_W); }
    int32_t                                  map_h() { return rd<int32_t>(A_MAP_H); }
};

// Runs at marshal 0x00482b5b after the retry INC. `unit_off` = player*0x5b04 + index*0xe9 (the marshal's EAX).
void __cdecl arrival_retry_check(uint32_t unit_off) {
    const uint32_t  p   = rd<uint16_t>(A_CUR_PLAYER) & 7u;
    const uintptr_t u   = A_UNITS + unit_off;
    const uint32_t  idx = (unit_off - p * PLAYER_STRIDE) / UNIT_SIZE;
    ne_live_world   w;
    const auto      st = mh::sim::mvc::near_enough_check(w, p, static_cast<int32_t>(idx), g_arr_near_r);
    if (st == mh::sim::mvc::ne_stop::no) return;
    const char *why = st == mh::sim::mvc::ne_stop::on_goal ? "on its goal" : "near enough to a goal a friend holds";
    const int   x = rd<uint8_t>(u + U_X), y = rd<uint8_t>(u + U_Y), gx = rd<uint8_t>(u + U_GOAL_X), gy = rd<uint8_t>(u + U_GOAL_Y);
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
        cmp   g_fc_handled, 0                    // mods:M1G 4b: the formation planned this commit inside the bracket -- 2a is skipped, once
        je    ac_nofc
        mov   g_fc_handled, 0
        jmp   ac_done
    ac_nofc:
        cmp   g_arr_distinct_rt, 0
        je    ac_done
        inc   dword ptr [g_mv_hit + 4 * MVH_ARR_COMMIT]
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
        inc   dword ptr [g_mv_hit + 4 * MVH_MERGE_GOAL]
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
        cmp   g_fc_on, 0                         // mods:M1G 4b: kept members have an empty path and must stop here, not retry
        jne   ar_check
        cmp   g_arr_near_r, 0
        je    ar_done
    ar_check:
        inc   dword ptr [g_mv_hit + 4 * MVH_ARR_RETRY]
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

} // namespace

// mods:M1G 4b. Runs at commit 0x0041c9ce, inside the route-through bracket, right after the retail plan (move_proto_nudge.cpp's rt_stub).
// `undo` is the bracket's list (a mh::sim::move::rt_undo). Returns 1 = handled (the 2a stub skips this commit).
int mv_fc_run(int32_t group_id, uint32_t mode, uint32_t target, const void *undo) {
    formation_live_world w;
    w.undo = static_cast<const mh::sim::move::rt_undo *>(undo);
    LARGE_INTEGER t0, t1, fq; // the per-commit cost of the formation (the planner runs inside it included); logging only, never read back by the sim
    QueryPerformanceCounter(&t0);
    const int handled = mh::sim::move::formation_after_pathfind(w, group_id, mode, target, g_fc_on - 1) ? 1 : 0;
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&fq);
    static uint32_t cnt, tot, mx;
    const uint32_t  us = static_cast<uint32_t>((t1.QuadPart - t0.QuadPart) * 1000000 / (fq.QuadPart ? fq.QuadPart : 1));
    if (!handled) return handled; // the commits it declined (a lone Ctrl-click, an attack) cost nothing worth reporting
    ++cnt;
    tot += us;
    if (us > mx) mx = us;
    if (cnt <= 8 || cnt % 16 == 0) {
        char b[160];
        wsprintfA(b, "; M1G FC cost commits=%u us_total=%u us_last=%u us_max=%u\n", cnt, tot, us, mx);
        seam_log(b);
    }
    return handled;
}

// Called once from install_move_proto (net_lockstep.cpp).
void install_move_proto_arrival() {
    g_arr_state_commit = arr_patch(ADDR_ARR_COMMIT, ARR_COMMIT_EXPECT, 7, mv_arr_commit_stub);
    g_arr_state_retry  = arr_patch(ADDR_ARR_RETRY, ARR_RETRY_EXPECT, 13, mv_arr_retry_stub);
    g_arr_state_merge  = arr_patch(ADDR_MV_MERGE_GOAL, MV_MERGE_GOAL_EXPECT, 5, mv_merge_goal_stub);
    mh::seams::mv::note_site(mh::seams::mv::SITE_ARR_COMMIT, g_arr_state_commit);
    mh::seams::mv::note_site(mh::seams::mv::SITE_ARR_RETRY, g_arr_state_retry);
    mh::seams::mv::note_site(mh::seams::mv::SITE_MERGE_GOAL, g_arr_state_merge);
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
    mh::seams::mv::request(mh::seams::mv::KEY_DISTINCT_GOALS, on, 0); // the honest line comes from the carrier report (move_carrier.cpp)
    return g_arr_distinct_rt == on;
}

// move_near_enough: R tiles (0 = retail), clamped 0..10.
int mh_set_move_near_enough(int r) {
    r            = r < 0 ? 0 : (r > 10 ? 10 : r);
    g_arr_near_r = (r > 0 && g_arr_state_retry == 1) ? r : 0;
    mh::seams::mv::request(mh::seams::mv::KEY_NEAR_ENOUGH, r, 0);
    return (g_arr_near_r > 0) == (r > 0);
}

// move_merge_goal_fix: 0 retail, 1 on. DISPLACED (libmh promotes group_move_order_pathfind) = its twin carries the key.
int mh_set_move_merge_goal_fix(int on) {
    on             = on != 0;
    g_arr_merge_rt = on && g_arr_state_merge == 1;
    mh::seams::mv::request(mh::seams::mv::KEY_MERGE_GOAL, on, 0);
    return (g_arr_merge_rt == on) || g_arr_state_merge == -2;
}
