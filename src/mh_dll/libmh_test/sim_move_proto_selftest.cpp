//
// sim_move_proto_selftest.cpp -- `simtest` cases for the ROUTE PLANNER half of the movement prototype keys (mods:M1H lane B).
//
// Each key is one retail byte splice in mh.dll AND a twin in a libmh body; both call the same header-only rule in
// sim/move/*.h. This suite pins (a) the pure rules over fakes (key off = retail, key on = the decision, plus the negative case)
// and (b) the libmh twin bodies through the real fixture (flood, pathfind centroid, detour). The commit-twin cases
// (route_through_idle, distinct_goals) are T10/T11 in sim_group_move_order_commit_selftest.cpp, the slot_dist fold is case (e)
// of sim_path_slot_dist_selftest.cpp.
//
//   T1  route_cost_calc    defaults == the raw byte for all 256 low bytes x wave sizes 0..12; table values exact; wave_full
//                          scaling is integer ring*min(n,N)/N; unknown bits stay raw; clamps.
//   T2  corridor_widen     mode 0 nothing; mode 1 marks only neighbours of marks 1/2, a mark-3 region never expands, list order
//                          irrelevant; mode 2 marks all; mode 3 widens nothing; eligible-cell count.
//   T3  wrap / guards      centroid `d - w` vs `0x80 - d`; slot fold of d < -half; detour skip only for slot 0xff; flood full
//                          at 0xff8 pending (ring-wrapped cursors).
//   T4  arrival            the distinct-goals decision over a fake world: free-tile predicate (passable / fog / reachability /
//                          taken), ring order and ties, the single-wave re-goal + the planner re-run, the multi-wave passes.
//   T4b formation          mods:M1G 4b over a fake world: gates, eligibility, ring order, keep + slack, the Hungarian assignment (optimal vs brute
//                          force, deterministic ties), the n = 1 keep-goal case, the per-member plans and what is put back.
//   T4c move-as-one        mods:M1G 4b v3 (key 3): the helpers, the spec model's assignments on 12 scenarios (F14-F16, S2/S3/S5/S7-S9, rc18, rc17 O1/O7), the
//                          queued groupmates (planned with the wave, stamped, kept by wave 2), the seam wrap, n = 1, the old rule untouched.
//   T5  route_through      only idle same-player ground units off the goal are opened; the restore is byte-exact; RT_MAX cap.
//   T5b route_through_moving  only WALKING (state 0x0f) friends, settled (0) or sliding (5); cost levels; exact restore; never touches idle units.
//   T6  flood twin         sim_map_region_routing.cpp region_flood_reachable: key off = retail field; route_cost_mode on with the
//                          retail table = the same field, with a custom table = the exact table field; wave_full scaling.
//   T7  flood queue guard  the guard only changes a flood whose front laps the ring.
//   T8  pathfind centroid  sim_group_move_order_pathfind.cpp: `d - w` vs `0x80 - d` on both axes.
//   T9  detour guard       sim_unit_path_detour.cpp: occupant slot 0xff is not recursed into with the key on.
//
#include "sim/move/arrival.h"
#include "sim/move/corridor.h"
#include "sim/move/formation.h"
#include "sim/move/guards.h"
#include "sim/move/planner_hits.h"
#include "sim/move/route_cost.h"
#include "sim/move/route_through.h"
#include "sim/move/wrap.h"
#include "sim/sim_group_move_order_pathfind.h"
#include "sim/sim_map_region_routing.h"
#include "sim/sim_move_fixes.h"
#include "sim/sim_unit_path_detour.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

namespace mv = mh::sim::move;

// A key switchboard value that is put back when the case ends, so one case cannot leak a key into the next suite.
struct fixes_scope {
    move_fixes saved;
    fixes_scope() : saved(move_fix()) {}
    ~fixes_scope() { set_move_fixes(saved); }
};

// ============================================================================================================================
// T1 -- route_cost_calc
// ============================================================================================================================
void t1_route_cost() {
    const mv::route_cost_params retail; // 1 / 4 / 8 / 16, wave_full 0
    bool                        all_same = true;
    for (uint32_t low = 0; low < 256; ++low)
        for (int32_t n = 0; n <= 12; ++n)
            if (mv::route_cost_calc(retail, low, n) != low) all_same = false;
    ck(all_same, "T1: the retail table (1/4/8/16, wave_full 0) returns the raw byte for all 256 low bytes x wave sizes 0..12");

    mv::route_cost_params p;
    p.base   = 2;
    p.ring4  = 1;
    p.ring8  = 3;
    p.ring16 = 5;
    ck_eq(mv::route_cost_calc(p, 0, 1), 0u, "T1: low 0 stays 0");
    ck_eq(mv::route_cost_calc(p, 1, 1), 2u, "T1: bit0 -> route_cost_base");
    ck_eq(mv::route_cost_calc(p, 5, 1), 2u + 1u, "T1: bit0 + ring4 -> base + ring4");
    ck_eq(mv::route_cost_calc(p, 9, 1), 2u + 3u, "T1: bit0 + ring8");
    ck_eq(mv::route_cost_calc(p, 17, 1), 2u + 5u, "T1: bit0 + ring16");
    ck_eq(mv::route_cost_calc(p, 29, 1), 2u + 1u + 3u + 5u, "T1: a cell touching an obstacle (29) pays base + all three rings");
    ck_eq(mv::route_cost_calc(p, 4, 1), 1u, "T1: ring bits without bit0 still pay their ring (no base)");
    ck_eq(mv::route_cost_calc(p, 0x20 | 1, 1), 0x20u + 2u, "T1: bits other than passable / the rings stay raw (0x20 + base)");
    ck_eq(mv::route_cost_calc(p, 0x80 | 29, 1), 0x80u + 2u + 9u, "T1: bit 7 stays raw");

    p.wave_full = 4; // the ring part scales with min(n, 4) / 4
    ck_eq(mv::route_cost_calc(p, 29, 4), 2u + 9u, "T1: wave of N pays the full ring");
    ck_eq(mv::route_cost_calc(p, 29, 9), 2u + 9u, "T1: a wave above N is clamped to N");
    ck_eq(mv::route_cost_calc(p, 29, 2), 2u + 9u * 2u / 4u, "T1: wave of 2 of 4 pays ring*2/4 (integer division)");
    ck_eq(mv::route_cost_calc(p, 29, 1), 2u + 9u / 4u, "T1: a lone unit pays ring*1/4");
    ck_eq(mv::route_cost_calc(p, 29, 0), 2u + 9u / 4u, "T1: wave size 0 counts as 1");
    ck_eq(mv::route_cost_calc(p, 29, -3), 2u + 9u / 4u, "T1: a negative wave size counts as 1");
    ck_eq(mv::route_cost_calc(p, 1, 1), 2u, "T1: wave_full never scales the base");

    const mv::route_cost_params c = mv::route_cost_params_clamped(0, -5, 500, 101, 99);
    ck(c.base == 1 && c.ring4 == 0 && c.ring8 == 100 && c.ring16 == 100 && c.wave_full == 64, "T1: the setters' clamps (base 1..100, rings 0..100, wave_full 0..64)");
    const mv::route_cost_params d = mv::route_cost_params_clamped(1, 4, 8, 16, 0);
    ck(d.base == 1 && d.ring4 == 4 && d.ring8 == 8 && d.ring16 == 16 && d.wave_full == 0, "T1: in-range values pass through the clamp unchanged");
}

// ============================================================================================================================
// T2 -- corridor_widen over an array fake
// ============================================================================================================================
struct fake_region {
    int32_t  mark     = 0;
    uint32_t cells    = 0;
    int      nb[6]    = {-1, -1, -1, -1, -1, -1};
    uint32_t nb_count = 0;
    int      next_idx = -1;
};
struct fake_regions {
    using handle = fake_region *;
    std::vector<fake_region> r;
    int                      head_idx = 0;
    handle                   head() { return r.empty() ? nullptr : &r[(size_t)head_idx]; }
    handle                   next(handle h) { return h->next_idx < 0 ? nullptr : &r[(size_t)h->next_idx]; }
    int32_t                 &mark(handle h) { return h->mark; }
    uint32_t                 cells(handle h) { return h->cells; }
    uint32_t                 neighbor_count(handle h) { return h->nb_count; }
    handle                   neighbor(handle h, uint32_t i) { return h->nb[i] < 0 ? nullptr : &r[(size_t)h->nb[i]]; }
};

// A chain 0-1-2-3-4-5 (each region neighbours the next/previous), list order given by `order`; marks: r1 = 2 (member region),
// r2 = 1 (route). cells = 10 * (index + 1).
fake_regions make_chain(const int *order) {
    fake_regions f;
    f.r.resize(6);
    for (int i = 0; i < 6; ++i) {
        f.r[(size_t)i].cells = 10u * (uint32_t)(i + 1);
        uint32_t n           = 0;
        if (i > 0) f.r[(size_t)i].nb[n++] = i - 1;
        if (i < 5) f.r[(size_t)i].nb[n++] = i + 1;
        f.r[(size_t)i].nb_count = n;
    }
    for (int k = 0; k < 5; ++k) f.r[(size_t)order[k]].next_idx = order[k + 1];
    f.r[(size_t)order[5]].next_idx = -1;
    f.head_idx                     = order[0];
    f.r[1].mark                    = 2;
    f.r[2].mark                    = 1;
    return f;
}

void t2_corridor() {
    const int fwd[6] = {0, 1, 2, 3, 4, 5}, rev[6] = {5, 4, 3, 2, 1, 0}, mix[6] = {3, 0, 5, 2, 4, 1};
    for (const int *order : {fwd, rev, mix}) {
        fake_regions              f0 = make_chain(order);
        const mv::corridor_result r0 = mv::corridor_widen(0, f0);
        ck(r0.regions == 6 && r0.before == 2 && r0.after == 2, "T2: mode 0 counts regions, widens nothing");
        ck_eq(r0.cells, 20u + 30u, "T2: mode 0 eligible cells = the cells of the two marked regions");

        fake_regions              f1 = make_chain(order);
        const mv::corridor_result r1 = mv::corridor_widen(1, f1);
        // r1 (mark 2) neighbours r0, r2; r2 (mark 1) neighbours r1, r3: r0 and r3 become 3; r4 (two away) stays 0.
        ck(f1.r[0].mark == 3 && f1.r[3].mark == 3, "T2: mode 1 marks the regions adjacent to marks 1/2 with 3");
        ck(f1.r[1].mark == 2 && f1.r[2].mark == 1, "T2: mode 1 leaves the original marks alone");
        ck(f1.r[4].mark == 0 && f1.r[5].mark == 0, "T2: mode 1 does not expand a 3 (list order cannot matter: two hops stay 0)");
        ck(r1.before == 2 && r1.after == 4, "T2: mode 1 before/after counts");
        ck_eq(r1.cells, 10u + 20u + 30u + 40u, "T2: mode 1 eligible cells = regions 0..3");

        fake_regions              f2   = make_chain(order);
        const mv::corridor_result r2   = mv::corridor_widen(2, f2);
        bool                      all3 = true;
        for (int i = 0; i < 6; ++i)
            if (f2.r[(size_t)i].mark == 0) all3 = false;
        ck(all3 && f2.r[1].mark == 2 && f2.r[2].mark == 1, "T2: mode 2 marks every region (originals untouched)");
        ck(r2.after == 6, "T2: mode 2 after count");
        ck_eq(r2.cells, 10u + 20u + 30u + 40u + 50u + 60u, "T2: mode 2 eligible cells = everything");

        fake_regions              f3 = make_chain(order);
        const mv::corridor_result r3 = mv::corridor_widen(3, f3);
        ck(r3.after == 2 && f3.r[0].mark == 0 && f3.r[3].mark == 0, "T2: mode 3 (measure only) widens nothing");
    }
    fake_regions              empty;
    const mv::corridor_result re = mv::corridor_widen(1, empty);
    ck(re.regions == 0 && re.before == 0 && re.after == 0 && re.cells == 0, "T2: an empty region list is a no-op");
    ck(mv::corridor_clamp_mode(-1) == 0 && mv::corridor_clamp_mode(2) == 2 && mv::corridor_clamp_mode(9) == 3, "T2: the mode clamp 0..3");
}

// ============================================================================================================================
// T3 -- wrap and guard rules
// ============================================================================================================================
void t3_wrap_guards() {
    ck_eq((uint32_t)mv::centroid_wrap_delta(200, 256, false), (uint32_t)(0x80 - 200), "T3: key off = retail `0x80 - d`");
    ck_eq((uint32_t)mv::centroid_wrap_delta(200, 256, true), (uint32_t)(200 - 256), "T3: key on = `d - width` (-56, not the mirror -72)");
    ck_eq((uint32_t)mv::centroid_wrap_delta(180, 200, true), (uint32_t)-20, "T3: key on on a 200-wide map");
    ck_eq((uint32_t)mv::centroid_wrap_delta(180, 200, false), (uint32_t)-52, "T3: key off on a 200-wide map is the retail mirror");
    // the two rules agree only where d == size/2 + 0x40 ... never mind: they must differ for a seam-straddling delta
    ck(mv::centroid_wrap_delta(130, 256, true) != mv::centroid_wrap_delta(130, 256, false), "T3: the fix changes a seam-straddling delta");

    bool fired = false;
    ck_eq((uint32_t)mv::slot_wrap_delta(150, 100, 256, false, &fired), (uint32_t)-106, "T3: slot d > half folds by -size (key off)");
    ck(!fired, "T3: ... and is not the fix's extra fold");
    ck_eq((uint32_t)mv::slot_wrap_delta(150, 100, 256, true, &fired), (uint32_t)-106, "T3: slot d > half folds by -size (key on, same)");
    ck(!fired, "T3: ... still not the extra fold");
    ck_eq((uint32_t)mv::slot_wrap_delta(-150, 100, 256, false, &fired), (uint32_t)-150, "T3: slot d < -half is left alone with the key off (retail)");
    ck(!fired, "T3: ... no fold fired");
    ck_eq((uint32_t)mv::slot_wrap_delta(-150, 100, 256, true, &fired), 106u, "T3: slot d < -half folds by +size with the key on");
    ck(fired, "T3: ... the extra fold fired");
    fired = false;
    ck_eq((uint32_t)mv::slot_wrap_delta(-100, 100, 256, true, &fired), (uint32_t)-100, "T3: d == -half is not folded (strictly below)");
    ck(!fired, "T3: ... nothing fired");
    ck_eq((uint32_t)mv::slot_wrap_delta(100, 100, 256, true, &fired), 100u, "T3: d == half is not folded either");

    ck(!mv::detour_skip_occupant(false, 0xff), "T3: detour guard off never skips");
    ck(mv::detour_skip_occupant(true, 0xff), "T3: detour guard on skips an occupant without a path slot");
    ck(!mv::detour_skip_occupant(true, 0), "T3: ... but not slot 0");
    ck(!mv::detour_skip_occupant(true, 99), "T3: ... nor the last real slot (99)");
    ck(!mv::detour_skip_occupant(true, 0xfe), "T3: ... nor 0xfe");

    ck(!mv::flood_queue_full(false, 0, 0xff8), "T3: flood guard off never stops");
    ck(!mv::flood_queue_full(true, 0, 0xff7), "T3: 0xff7 pending: keep going");
    ck(mv::flood_queue_full(true, 0, 0xff8), "T3: 0xff8 pending: stop");
    ck(mv::flood_queue_full(true, 0x10, 0x10 + 0xff9), "T3: 0xff9 pending, offset cursors: stop");
    ck(mv::flood_queue_full(true, 0xff0, 0xfe8), "T3: write behind read: (write-read)&0xfff = 0xff8 wraps and stops");
    ck(!mv::flood_queue_full(true, 0xff0, 0x20), "T3: write behind read, 0x30 pending: keep going");
}

// ============================================================================================================================
// T4 -- arrival_after_pathfind over a fake world
// ============================================================================================================================
struct fake_arrival {
    uint32_t              mask  = 0xff;
    uint32_t              own_  = 3;
    int32_t               n     = 1;
    int32_t               limit = 100;
    int32_t               gx = 30, gy = 30, ax = 30, ay = 30, cx = 10, cy = 10;
    uint32_t              handle[100] = {7};
    uint8_t               col[100] = {10}, row[100] = {10};
    uint8_t               tile[200] = {};
    std::vector<uint8_t>  pass      = std::vector<uint8_t>(65536, 1);
    std::vector<uint8_t>  fogv      = std::vector<uint8_t>(65536, 0xff);
    std::vector<uint32_t> reg       = std::vector<uint32_t>(65536, 0);
    std::vector<uint32_t> field     = std::vector<uint32_t>(65536, 2); // reachable
    mv::arr_unit          units[100];
    int                   reruns     = 0;
    uint32_t              rerun_mode = 0, rerun_target = 0;
    int                   logs_single = 0, logs_wave = 0;
    int                   last_kept = -1, last_moved = -1, last_failed = -1;

    fake_arrival() {
        std::memset(units, 0, sizeof(units));
        for (auto &u : units) u.order = 0x0f;
        field[(size_t)((30u << 8) | 30u)] = 1; // the flood was seeded at the goal: the field is fresh
    }
    uint32_t path_mask() { return mask; }
    uint32_t owner() { return own_; }
    int32_t  member_count() { return n; }
    int32_t  unit_limit() { return limit; }
    uint32_t goal_x() { return (uint32_t)gx; }
    uint32_t goal_y() { return (uint32_t)gy; }
    void     set_goal(int32_t x, int32_t y) {
        gx = x;
        gy = y;
    }
    uint32_t anchor_x() { return (uint32_t)ax; }
    uint32_t anchor_y() { return (uint32_t)ay; }
    uint32_t centroid_x() { return (uint32_t)cx; }
    uint32_t centroid_y() { return (uint32_t)cy; }
    uint32_t member_handle(int32_t m) { return handle[m]; }
    uint8_t  member_col(int32_t m) { return col[m]; }
    uint8_t  member_row(int32_t m) { return row[m]; }
    uint8_t  tile_byte(int32_t m, int k) { return tile[m * 2 + k]; }
    void     set_tile_byte(int32_t m, int k, uint8_t b) { tile[m * 2 + k] = b; }
    uint8_t  passable(uint32_t t) { return pass[t]; }
    uint8_t  fog(uint32_t t) { return fogv[t]; }
    uint32_t region_of(uint32_t t) { return reg[t]; }
    uint32_t field_of(uint32_t t) { return field[t]; }
    bool     unit(uint32_t, uint32_t i, mv::arr_unit *out) {
        *out = units[i];
        return true;
    }
    void set_unit_goal(uint32_t, uint32_t i, uint8_t x, uint8_t y) {
        units[i].goal_x = x;
        units[i].goal_y = y;
    }
    void rerun_pathfind(uint32_t mode, uint32_t target) {
        ++reruns;
        rerun_mode   = mode;
        rerun_target = target;
    }
    void log_single(uint32_t, uint32_t, uint32_t, uint32_t, bool, uint32_t, uint32_t) { ++logs_single; }
    void log_wave(uint32_t, int32_t, uint32_t, uint32_t, bool, int kept, int moved, int failed) {
        ++logs_wave;
        last_kept   = kept;
        last_moved  = moved;
        last_failed = failed;
    }
    static uint32_t T(uint32_t x, uint32_t y) { return (x << 8) | y; }
};

void t4_arrival() {
    // gates
    {
        fake_arrival w;
        w.pass[fake_arrival::T(30, 30)] = 0;
        mv::arrival_after_pathfind(w, 0, 1, 1);
        ck(w.reruns == 0 && w.gx == 30 && w.gy == 30, "T4: move_group_id 0 (a lone right-click without a group) is untouched");
        mv::arrival_after_pathfind(w, 5, 0, 1);
        ck(w.reruns == 0 && w.gx == 30 && w.gy == 30, "T4: an attack order (mode 0) is untouched");
        w.n = 0;
        mv::arrival_after_pathfind(w, 5, 1, 1);
        ck(w.reruns == 0, "T4: an empty wave is untouched");
        w.n = 257;
        mv::arrival_after_pathfind(w, 5, 1, 1);
        ck(w.reruns == 0, "T4: an oversize wave is untouched");
    }
    // single wave, the route's goal is free: retail
    {
        fake_arrival w;
        mv::arrival_after_pathfind(w, 5, 1, 1);
        ck(w.reruns == 0 && w.gx == 30 && w.gy == 30 && w.logs_single == 0, "T4: single wave, free goal: retail (no re-run, no log)");
    }
    // single wave, goal blocked: nearest free tile = scan order (dx then dy ascending) among d2 == 1 -> (29,30)
    {
        fake_arrival w;
        w.pass[fake_arrival::T(30, 30)] = 0;
        mv::arrival_after_pathfind(w, 5, 1, 2);
        ck(w.gx == 29 && w.gy == 30, "T4: single wave, blocked goal -> the nearest free tile, ties by scan order (29,30)");
        ck(w.reruns == 1 && w.rerun_mode == 1 && w.rerun_target == 2, "T4: the planner is re-run once with the same mode / target");
        ck(w.units[7].goal_x == 29 && w.units[7].goal_y == 30, "T4: the unit's own goal is the new tile");
        ck(w.logs_single == 1, "T4: one log line");
    }
    // fog: the undiscovered candidate is skipped
    {
        fake_arrival w;
        w.pass[fake_arrival::T(30, 30)] = 0;
        w.fogv[fake_arrival::T(29, 30)] = 0; // not discovered by anyone
        mv::arrival_after_pathfind(w, 5, 1, 1);
        ck(w.gx == 30 && w.gy == 29, "T4: an undiscovered tile is not free (next best (30,29))");
        fake_arrival w2;
        w2.pass[fake_arrival::T(30, 30)] = 0;
        w2.fogv[fake_arrival::T(29, 30)] = (uint8_t)(0xff & ~(1u << 3));
        mv::arrival_after_pathfind(w2, 5, 1, 1);
        ck(w2.gx == 30 && w2.gy == 29, "T4: discovery is per owner bit");
    }
    // reachability, fresh field: 0 and 0xffffff are not reachable
    {
        fake_arrival w;
        w.pass[fake_arrival::T(30, 30)]  = 0;
        w.field[fake_arrival::T(29, 30)] = 0;
        w.field[fake_arrival::T(30, 29)] = 0xffffffu;
        mv::arrival_after_pathfind(w, 5, 1, 1);
        ck(w.gx == 30 && w.gy == 31, "T4: field 0 / 0xffffff tiles are unreachable (next best (30,31))");
    }
    // reachability, stale field: same nav region as the goal
    {
        fake_arrival w;
        w.field[fake_arrival::T(30, 30)] = 9; // the flood was NOT seeded at the goal
        w.pass[fake_arrival::T(30, 30)]  = 0;
        std::fill(w.reg.begin(), w.reg.end(), 4u);
        w.reg[fake_arrival::T(29, 30)] = 5; // another region
        mv::arrival_after_pathfind(w, 5, 1, 1);
        ck(w.gx == 30 && w.gy == 29, "T4: stale field: a tile in another nav region is not free");
        fake_arrival w2;
        w2.field[fake_arrival::T(30, 30)] = 9;
        w2.pass[fake_arrival::T(30, 30)]  = 0; // region 0 everywhere: no region -> nothing is free
        mv::arrival_after_pathfind(w2, 5, 1, 1);
        ck(w2.gx == 30 && w2.gy == 30 && w2.reruns == 0 && w2.logs_single == 1, "T4: stale field and no region: no free tile, kept (logged, no re-run)");
    }
    // taken by another unit of the group
    {
        fake_arrival w;
        w.pass[fake_arrival::T(30, 30)] = 0;
        w.units[20].group               = 5;
        w.units[20].state               = 0x0f;
        w.units[20].goal_x              = 29;
        w.units[20].goal_y              = 30;
        mv::arrival_after_pathfind(w, 5, 1, 1);
        ck(w.gx == 30 && w.gy == 29, "T4: a tile another unit of the group is walking to is taken");
        fake_arrival w2;
        w2.pass[fake_arrival::T(30, 30)] = 0;
        w2.units[20].group               = 5;
        w2.units[20].state               = 0x0a;
        w2.units[20].goal_x              = 29;
        w2.units[20].goal_y              = 30; // marshalling to a tile != click
        mv::arrival_after_pathfind(w2, 5, 1, 1);
        ck(w2.gx == 30 && w2.gy == 29, "T4: a marshalling unit with a goal other than the click holds it");
        fake_arrival w3;
        w3.pass[fake_arrival::T(30, 30)] = 0;
        w3.units[20].group               = 5;
        w3.units[20].state               = 0x0a;
        w3.units[20].goal_x              = 29;
        w3.units[20].goal_y              = 30;
        w3.ax                            = 29;
        w3.ay                            = 30; // ... but its goal IS the click: it has not been given one yet
        mv::arrival_after_pathfind(w3, 5, 1, 1);
        ck(w3.gx == 29 && w3.gy == 30, "T4: a marshalling unit whose goal is still the click does not hold it");
        fake_arrival w4;
        w4.pass[fake_arrival::T(30, 30)] = 0;
        w4.units[20].group               = 6;
        w4.units[20].state               = 0x0f;
        w4.units[20].goal_x              = 29;
        w4.units[20].goal_y              = 30; // another group
        mv::arrival_after_pathfind(w4, 5, 1, 1);
        ck(w4.gx == 29 && w4.gy == 30, "T4: a unit of ANOTHER move group does not hold the tile");
    }
    // single wave, the member is not on a plain move: untouched
    {
        fake_arrival w;
        w.pass[fake_arrival::T(30, 30)] = 0;
        w.units[7].order                = 0x05;
        mv::arrival_after_pathfind(w, 5, 1, 1);
        ck(w.reruns == 0 && w.gx == 30, "T4: a member on a non-move order is untouched");
        w.units[7].order = 0x01; // STOP_TO_DEFAULT queued: a REPLAN of a move
        mv::arrival_after_pathfind(w, 5, 1, 1);
        ck(w.reruns == 1 && w.gx == 29, "T4: ... order 0x01 (the replan of a move) is a plain move");
    }
    // no free tile within the radius: kept, no re-run
    {
        fake_arrival w;
        std::fill(w.pass.begin(), w.pass.end(), 0);
        mv::arrival_after_pathfind(w, 5, 1, 1);
        ck(w.reruns == 0 && w.gx == 30 && w.gy == 30 && w.logs_single == 1, "T4: nothing free within 8 tiles: the goal is kept");
    }
    // multi-member wave
    {
        fake_arrival w;
        w.n                 = 3;
        const uint32_t h[3] = {7, 8, 9};
        const uint8_t  c[3] = {9, 10, 11};
        for (int i = 0; i < 3; ++i) {
            w.handle[i] = h[i];
            w.col[i]    = c[i];
            w.row[i]    = 10;
        }
        w.gx = w.ax = 50;
        w.gy = w.ay                      = 50;
        w.field[fake_arrival::T(30, 30)] = 2;
        w.field[fake_arrival::T(50, 50)] = 1;
        for (int i = 0; i < 3; ++i) {
            w.tile[i * 2]     = 47;
            w.tile[i * 2 + 1] = 47;
        } // the retail tiles
        w.pass[fake_arrival::T(50, 50)] = 0; // member 1's formation tile is blocked
        mv::arrival_after_pathfind(w, 5, 1, 1);
        ck(w.tile[0] == 49 && w.tile[1] == 50, "T4: wave pass 1: member 0 keeps its free formation tile (49,50)");
        ck(w.tile[4] == 51 && w.tile[5] == 50, "T4: wave pass 1: member 2 keeps its free formation tile (51,50)");
        ck(w.tile[2] == 50 && w.tile[3] == 49, "T4: wave pass 2: member 1 takes the nearest free tile (50,49) (the others' tiles are taken)");
        ck(w.reruns == 0, "T4: a multi-member wave never re-runs the planner");
        ck(w.logs_wave == 1 && w.last_moved == 3 && w.last_kept == 0 && w.last_failed == 0, "T4: wave counts: 3 moved (their retail tiles were (47,47)), 0 failed");
    }
    // multi-member wave: a member not on a plain move keeps its retail tile and that tile is taken
    {
        fake_arrival w;
        w.n         = 2;
        w.handle[0] = 7;
        w.handle[1] = 8;
        w.col[0]    = 10;
        w.col[1]    = 9;
        w.row[0] = w.row[1] = 10; // member 1's formation tile is (50 - (10 - 9), 50) = (49,50) ...
        w.gx = w.ax = 50;
        w.gy = w.ay                      = 50;
        w.field[fake_arrival::T(50, 50)] = 1;
        w.tile[0]                        = 49;
        w.tile[1]                        = 50; // ... which is member 0's retail tile (member 0 is on a non-move order)
        w.tile[2]                        = 47;
        w.tile[3]                        = 47;
        w.units[7].order                 = 0x05;
        mv::arrival_after_pathfind(w, 5, 1, 1);
        ck(w.tile[0] == 49 && w.tile[1] == 50, "T4: a member on a non-move order keeps its retail tile");
        ck(w.tile[2] == 48 && w.tile[3] == 50, "T4: ... and that tile is taken: the other member gets the nearest free tile (48,50)");
    }
}

// ============================================================================================================================
// T5 -- rt_unmask / rt_restore over a fake world
// ============================================================================================================================
struct fake_rt {
    int32_t                  cap = 12;
    std::vector<mv::rt_unit> u   = std::vector<mv::rt_unit>(2048);
    std::vector<uint16_t>    bld = std::vector<uint16_t>(65536, 0);
    std::vector<uint8_t>     cls = std::vector<uint8_t>(65536, 0);
    std::vector<uint8_t>     pas = std::vector<uint8_t>(65536, 1);
    std::vector<uint32_t>    fl  = std::vector<uint32_t>(65536, 0);
    int32_t                  unit_count() { return cap; }
    bool                     unit(uint32_t player, int32_t i, mv::rt_unit *out) {
        if (player != 2) return false;
        *out = u[(size_t)i];
        return true;
    }
    uint16_t  tile_building(uint32_t f) { return bld[f]; }
    uint8_t   tile_class_owner(uint32_t f) { return cls[f]; }
    uint8_t  &passable(uint32_t f) { return pas[f]; }
    uint32_t &cell_flags(uint32_t f) { return fl[f]; }
    // an eligible idle friend i at tile (10 + i, 20)
    void make_idle(int i) {
        mv::rt_unit &r             = u[(size_t)i];
        r.state                    = mv::RT_ST_IDLE;
        r.order_queued             = 0;
        r.move_microstep           = mv::RT_MS_SETTLED;
        r.origin_tile_was_passable = 1;
        r.cfg_ground_idle          = true;
        r.alive                    = true;
        r.x                        = (uint8_t)(10 + (i & 0x7f));
        r.y                        = (uint8_t)(20 + (i >> 7));
        const uint32_t f           = ((uint32_t)r.x << 8) | r.y;
        bld[f]                     = (uint16_t)i;
        cls[f]                     = (uint8_t)(2 | 0x80);
        pas[f]                     = 0; // a unit stands here
        fl[f]                      = 0xabcd0001u;
    }
};


// ============================================================================================================================
// T4b -- formation_after_pathfind over a fake world (mods:M1G 4b)
// ============================================================================================================================
struct fake_formation : fake_arrival {
    std::vector<uint8_t>  truep           = std::vector<uint8_t>(65536, 1);
    std::vector<int32_t>  occ             = std::vector<int32_t>(65536, 0);
    uint32_t              first_run[100]  = {};
    bool                  terminated[100] = {};
    int                   replans         = 0;
    std::vector<uint32_t> planned_handle; // the member each single-member plan was run for
    std::vector<uint32_t> planned_goal;
    int                   adjusts = 0, assigns = 0, fc_logs = 0;
    int                   last_kept = -1, last_filled = -1, last_retail = -1, last_rk = -1, last_replans = -1, last_reused = -1;
    uint32_t              goal_override_x = 0xffffffffu; // run_pathfind rewrites the goal (an unreachable goal moved by the planner)

    // members 0..n-1 are units 7, 8, 9, ... standing at the given tiles; each has path slot m (non-empty path)
    void members(std::initializer_list<std::pair<int, int>> at) {
        int m = 0;
        n     = (int32_t)at.size();
        for (auto &p : at) {
            handle[m]              = 7 + m;
            col[m]                 = (uint8_t)p.first;
            row[m]                 = (uint8_t)p.second;
            tile[m * 2 + 0]        = (uint8_t)p.first; // the retail plan: every member's tile is where it stands + a rigid shift of 0
            tile[m * 2 + 1]        = (uint8_t)p.second;
            units[7 + m].order     = 0x0f;
            units[7 + m].x         = (uint8_t)p.first;
            units[7 + m].y         = (uint8_t)p.second;
            units[7 + m].group     = 5;
            units[7 + m].path_slot = (uint8_t)(10 + m);
            first_run[10 + m]      = 3;
            ++m;
        }
    }
    bool    true_passable(uint32_t t) { return truep[t] != 0; }
    int32_t occupant(uint32_t, uint32_t t) { return occ[t]; }
    void    set_member_count(int32_t v) { n = v; }
    void    set_member(int32_t m, uint32_t h, uint8_t c, uint8_t r) {
        handle[m] = h;
        col[m]    = c;
        row[m]    = r;
    }
    void set_centroid(uint32_t x, uint32_t y) {
        cx = (int32_t)x;
        cy = (int32_t)y;
    }
    void run_pathfind(uint32_t, uint32_t) {
        ++replans;
        planned_handle.push_back(handle[0]);
        planned_goal.push_back(((uint32_t)gx << 8) | (uint32_t)gy);
        if (goal_override_x != 0xffffffffu) gx = (int32_t)goal_override_x;
    }
    uint32_t path_first_run(uint32_t, uint32_t slot) { return first_run[slot]; }
    void     path_terminate(uint32_t, uint32_t slot) {
        terminated[slot] = true;
        first_run[slot]  = 0;
    }
    void log_fc(uint32_t, int, int kept, int filled, int retail, int rk, int replans_, int reused) {
        ++fc_logs;
        last_kept    = kept;
        last_filled  = filled;
        last_retail  = retail;
        last_rk      = rk;
        last_replans = replans_;
        last_reused  = reused;
    }
    void     log_adjust(uint32_t, uint32_t, uint32_t, uint32_t) { ++adjusts; }
    void     log_assign(uint32_t, uint32_t, uint32_t, uint32_t, int) { ++assigns; }
    uint32_t tile_of_member(int m) { return ((uint32_t)tile[m * 2] << 8) | tile[m * 2 + 1]; }
    // the other units of the group that "hold" a goal (arrival.h's taken rule)
    void hold_goal(int unit_idx, int x, int y, uint16_t state = 0x0f) {
        units[unit_idx].order  = 0x0f;
        units[unit_idx].state  = state;
        units[unit_idx].group  = 5;
        units[unit_idx].goal_x = (uint8_t)x;
        units[unit_idx].goal_y = (uint8_t)y;
    }
};

void t4b_formation() {
    using fw = fake_formation;
    // ---- gates --------------------------------------------------------------------------------------------------------------
    {
        fw w;
        w.members({{30, 30}});
        ck(!mv::formation_after_pathfind(w, 0, 1, 1, 1), "T4b: move_group_id 0 (a lone Ctrl-click) is not handled");
        ck(!mv::formation_after_pathfind(w, 5, 0, 1, 1), "T4b: an attack order (mode 0) is not handled");
        w.units[7].order = 0x1a;
        ck(!mv::formation_after_pathfind(w, 5, 1, 1, 1), "T4b: a member on a non-move order (attack) -> the whole commit is left to 2a");
        w.units[7].order = 0x33;
        ck(!mv::formation_after_pathfind(w, 5, 1, 1, 1), "T4b: ... and a squad merge (0x33)");
        w.units[7].order = 0x01;
        ck(mv::formation_after_pathfind(w, 5, 1, 1, 1), "T4b: order 0x01 (STOP_TO_DEFAULT queued: every replan) is a plain move");
        w.n = 0;
        ck(!mv::formation_after_pathfind(w, 5, 1, 1, 1), "T4b: an empty wave is not handled");
        w.n = 101;
        ck(!mv::formation_after_pathfind(w, 5, 1, 1, 1), "T4b: more than 100 members is not handled");
        fw w2;
        w2.members({{30, 30}});
        std::fill(w2.truep.begin(), w2.truep.end(), (uint8_t)0);
        ck(!mv::formation_after_pathfind(w2, 5, 1, 1, 1), "T4b: no eligible tile anywhere -> not handled (2a runs)");
    }
    // ---- n == 1 ---------------------------------------------------------------------------------------------------------------
    {
        fw w; // the member's own goal is the click and it is eligible: used as is, the retail route (phase 1) is reused
        w.members({{10, 10}});
        w.units[7].goal_x = 30;
        w.units[7].goal_y = 30;
        w.tile[0]         = 30; // phase 1's tile for the member
        w.tile[1]         = 30;
        ck(mv::formation_after_pathfind(w, 5, 1, 1, 1), "T4b n=1: handled");
        ck(w.replans == 0 && w.last_reused == 1, "T4b n=1: the member's goal is eligible and the retail route leads there -> no extra plan");
        ck(w.tile_of_member(0) == fw::T(30, 30) && w.units[7].goal_x == 30 && w.units[7].goal_y == 30, "T4b n=1: tile and unit goal = the click");
        ck(!w.terminated[10], "T4b n=1: the path is kept");
    }
    {
        fw w; // its goal is held by another unit of the group: the first eligible tile in ring order around the click
        w.members({{10, 10}});
        w.units[7].goal_x = 30;
        w.units[7].goal_y = 30;
        w.tile[0]         = 30;
        w.tile[1]         = 30;
        w.hold_goal(20, 30, 30);
        ck(mv::formation_after_pathfind(w, 5, 1, 1, 1), "T4b n=1 taken: handled");
        ck(w.replans == 1 && w.planned_goal[0] == fw::T(29, 30), "T4b n=1 taken: ring 1 order is (-1,0) first -> (29,30), planned alone");
        ck(w.gx == 30 && w.gy == 30 && w.n == 1 && w.handle[0] == 7 && w.col[0] == 10, "T4b n=1 taken: the wave scratch (goal, count, member 0) is put back");
        ck(w.tile_of_member(0) == fw::T(29, 30) && w.units[7].goal_x == 29, "T4b n=1 taken: tile and unit goal are the new tile");
        ck(w.cx == 10 && w.cy == 10, "T4b n=1 taken: the centroid is put back");
    }
    {
        fw w; // ring order: (d2, dx, dy) ascending, signed
        w.members({{10, 10}});
        w.units[7].goal_x = 30;
        w.units[7].goal_y = 30;
        w.hold_goal(20, 30, 30);
        w.hold_goal(21, 29, 30);
        w.hold_goal(22, 30, 29);
        w.hold_goal(23, 30, 31);
        ck(mv::formation_after_pathfind(w, 5, 1, 1, 1) && w.planned_goal.size() == 1 && w.planned_goal[0] == fw::T(31, 30),
           "T4b n=1 ring order: with (29,30), (30,29), (30,31) taken the next of ring 1 is (31,30)");
        fw w2;
        w2.members({{10, 10}});
        w2.units[7].goal_x = 30;
        w2.units[7].goal_y = 30;
        w2.hold_goal(20, 30, 30);
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy)
                if (dx || dy) w2.hold_goal(30 + (dx + 1) * 3 + (dy + 1), 30 + dx, 30 + dy);
        ck(mv::formation_after_pathfind(w2, 5, 1, 1, 1) && w2.planned_goal.size() == 1 && w2.planned_goal[0] == fw::T(28, 30),
           "T4b n=1 ring order: with ring 1 taken the first of ring 2 is (-2,0) -> (28,30)");
    }
    {
        fw w; // the other units' goals: only states 0x0c / 0x0f, or 0x0a with a goal other than the click
        w.members({{10, 10}});
        w.units[7].goal_x = 30;
        w.units[7].goal_y = 30;
        w.tile[0]         = 30;
        w.tile[1]         = 30;
        w.hold_goal(20, 30, 30, 0x0a); // 0x0a with goal == click = a later wave of this order: does not hold the goal
        ck(mv::formation_after_pathfind(w, 5, 1, 1, 1) && w.replans == 0, "T4b taken rule: a later wave of the same order (0x0a, goal = click) does not hold the click");
        fw w2;
        w2.members({{10, 10}});
        w2.units[7].goal_x = 30;
        w2.units[7].goal_y = 30;
        w2.tile[0]         = 30;
        w2.tile[1]         = 30;
        w2.hold_goal(20, 30, 30, 0x0c);
        ck(mv::formation_after_pathfind(w2, 5, 1, 1, 1) && w2.replans == 1, "T4b taken rule: an earlier wave in hop state 0x0c does");
    }
    // ---- eligibility ----------------------------------------------------------------------------------------------------------
    {
        fw w;
        w.members({{10, 10}});
        w.units[7].goal_x      = 30;
        w.units[7].goal_y      = 30;
        w.occ[fw::T(30, 30)]   = 99; // a unit that is not in the wave stands on the click
        w.truep[fw::T(29, 30)] = 0;  // a tile that was impassable BEFORE the route-through bracket opened it
        w.fogv[fw::T(30, 29)]  = 0;  // not discovered
        w.field[fw::T(30, 31)] = 0;  // not reachable
        ck(mv::formation_after_pathfind(w, 5, 1, 1, 1) && w.planned_goal[0] == fw::T(31, 30),
           "T4b eligibility: a non-member occupant, a tile closed before the bracket, fog and an unreachable tile are all skipped");
        fw w2;
        w2.members({{30, 30}, {31, 30}});
        w2.occ[fw::T(30, 30)] = 7; // a member of this wave standing on its own tile is fine
        ck(mv::formation_after_pathfind(w2, 5, 1, 1, 1) && w2.last_kept == 2, "T4b eligibility: a member's own tile is eligible");
    }
    // ---- n > 1: keep, fill, slack ---------------------------------------------------------------------------------------------
    {
        fw w; // a blob already on the click's target set stays: nothing is planned, every path is made empty
        w.members({{30, 30}, {31, 30}, {30, 31}});
        w.hold_goal(20, 30, 30); // (a member's goal is not looked at; a non-member's is)
        w.units[20].group = 4;   // ... of another group: does not hold anything
        w.gx = w.ax = 30;
        w.gy = w.ay = 30;
        ck(mv::formation_after_pathfind(w, 5, 1, 1, 1), "T4b blob: handled");
        ck(w.replans == 0 && w.last_kept == 3 && w.last_filled == 0, "T4b blob: all three kept, nothing planned");
        ck(w.terminated[10] && w.terminated[11] && w.terminated[12], "T4b blob: every kept member's path is made empty");
        ck(w.tile_of_member(0) == fw::T(30, 30) && w.tile_of_member(1) == fw::T(31, 30) && w.tile_of_member(2) == fw::T(30, 31),
           "T4b blob: the tiles are where they stand");
        ck(w.units[8].goal_x == 31 && w.units[9].goal_y == 31, "T4b blob: ... and so are the unit goals (the marshal's on-goal stop)");
        ck(w.last_rk == 1, "T4b blob: rk = 1 (1 + 4 eligible tiles reach 3)");
    }
    {
        fw w; // slack: a member at ring 2 when rk = 1 keeps its tile with slack 1, not with slack 0
        w.members({{30, 30}, {32, 30}});
        ck(mv::formation_after_pathfind(w, 5, 1, 1, 1) && w.last_kept == 2 && w.last_rk == 1, "T4b slack 1: the member at ring 2 (rk 1) keeps its tile");
        fw w0;
        w0.members({{30, 30}, {32, 30}});
        ck(mv::formation_after_pathfind(w0, 5, 1, 1, 0) && w0.last_kept == 1 && w0.last_filled == 1, "T4b slack 0: ... and does not: it is re-assigned");
        ck(w0.tile_of_member(1) == fw::T(29, 30), "T4b slack 0: to the first free tile in ring order (29,30)");
        ck(w0.replans == 1 && w0.planned_handle[0] == 8, "T4b slack 0: planned alone, member 1 (unit 8)");
        ck(w0.n == 2 && w0.handle[0] == 7 && w0.col[0] == 30, "T4b slack 0: count and member 0 are put back");
    }
    {
        fw w; // a far member fills the nearest hole; the two inside stay
        w.members({{30, 30}, {31, 30}, {50, 50}});
        ck(mv::formation_after_pathfind(w, 5, 1, 1, 1) && w.last_kept == 2 && w.last_filled == 1, "T4b fill: two stay, one is filled");
        ck(w.tile_of_member(2) == fw::T(29, 30), "T4b fill: first unclaimed eligible tile in ring order (29,30)");
        ck(w.replans == 1 && w.planned_handle[0] == 9 && w.planned_goal[0] == fw::T(29, 30), "T4b fill: exactly one single-member plan, for unit 9");
        ck(w.units[9].goal_x == 29 && w.units[9].goal_y == 30, "T4b fill: ... and its unit goal");
    }
    {
        fw w; // the phase-1 route already leads to the assigned tile: reused, no plan
        w.members({{50, 50}});
        w.n               = 1;
        w.units[7].goal_x = 30;
        w.units[7].goal_y = 30;
        w.tile[0]         = 30;
        w.tile[1]         = 30;
        ck(mv::formation_after_pathfind(w, 5, 1, 1, 1) && w.replans == 0 && w.last_reused == 1, "T4b reuse: a non-empty phase-1 route to the tile is kept");
        fw w2;
        w2.members({{50, 50}});
        w2.units[7].goal_x = 30;
        w2.units[7].goal_y = 30;
        w2.tile[0]         = 30;
        w2.tile[1]         = 30;
        w2.first_run[10]   = 0; // phase 1 left an empty path: planned again
        ck(mv::formation_after_pathfind(w2, 5, 1, 1, 1) && w2.replans == 1, "T4b reuse: ... an EMPTY one is planned again");
    }
    {
        fw w; // the planner moves an unreachable goal: the tile written back is where the plan really goes
        w.members({{50, 50}});
        w.units[7].goal_x = 30;
        w.units[7].goal_y = 30;
        w.goal_override_x = 33;
        ck(mv::formation_after_pathfind(w, 5, 1, 1, 1), "T4b adjust: handled");
        ck(w.adjusts == 1 && w.tile_of_member(0) == fw::T(33, 30), "T4b adjust: MEMBER_TILE is the goal after the plan (33,30), and the move is logged");
        ck(w.gx == 30, "T4b adjust: ... while the wave goal is put back");
    }
    {
        fw w; // more members than tiles: the surplus keeps its retail tile and path
        w.members({{30, 30}, {50, 50}, {60, 60}});
        for (int x = 0; x < 256; ++x) // only the click itself is eligible
            for (int y = 0; y < 256; ++y)
                if (!(x == 30 && y == 30)) w.truep[fw::T((uint32_t)x, (uint32_t)y)] = 0;
        w.tile[2] = 55;
        w.tile[3] = 55; // member 1's retail tile
        w.tile[4] = 66;
        w.tile[5] = 66;
        ck(mv::formation_after_pathfind(w, 5, 1, 1, 1), "T4b shortage: handled");
        ck(w.last_kept == 1 && w.last_retail == 2, "T4b shortage: one tile, three members: one keeps it, two keep their retail tile");
        ck(w.tile_of_member(1) == fw::T(55, 55) && w.tile_of_member(2) == fw::T(66, 66) && w.replans == 0, "T4b shortage: the retail tiles and paths stand");
    }
    // ---- the Hungarian step ---------------------------------------------------------------------------------------------------
    {
        // optimal against brute force for N = 1..7 on a deterministic pseudo-random matrix
        uint32_t seed = 12345;
        auto     rnd  = [&]() {
            seed = seed * 1664525u + 1013904223u;
            return (int32_t)((seed >> 16) % 50);
        };
        for (int N = 1; N <= 7; ++N) {
            for (int rep = 0; rep < 6; ++rep) {
                int32_t cost[49];
                for (int i = 0; i < N * N; ++i) cost[i] = rnd();
                int assign[8];
                mv::formation_detail::hungarian(N, cost, assign);
                int64_t got     = 0;
                bool    used[8] = {};
                bool    perm    = true;
                for (int i = 0; i < N; ++i) {
                    got += cost[i * N + assign[i]];
                    if (assign[i] < 0 || assign[i] >= N || used[assign[i]]) perm = false;
                    else used[assign[i]] = true;
                }
                int idx[8];
                for (int i = 0; i < N; ++i) idx[i] = i;
                int64_t best = 1 << 30;
                do {
                    int64_t s = 0;
                    for (int i = 0; i < N; ++i) s += cost[i * N + idx[i]];
                    best = std::min(best, s);
                } while (std::next_permutation(idx, idx + N));
                ck(perm && got == best, "T4b hungarian: a permutation of minimum total cost (vs brute force)");
            }
        }
        // ties: all-equal costs resolve by (row order, column order), and twice the same
        int32_t eq[16];
        for (int i = 0; i < 16; ++i) eq[i] = 7;
        int a1[4], a2[4];
        mv::formation_detail::hungarian(4, eq, a1);
        mv::formation_detail::hungarian(4, eq, a2);
        ck(std::memcmp(a1, a2, sizeof(a1)) == 0 && a1[0] == 0 && a1[1] == 1 && a1[2] == 2 && a1[3] == 3, "T4b hungarian: equal costs -> row i gets column i (the prototype's tie-break), deterministic");
        // the prototype's tie: two members, two tiles, symmetric
        int32_t sym[4] = {1, 1, 1, 1};
        int     s1[2];
        mv::formation_detail::hungarian(2, sym, s1);
        ck(s1[0] == 0 && s1[1] == 1, "T4b hungarian: a symmetric 2x2 -> the identity");
    }
    // ---- ring order -----------------------------------------------------------------------------------------------------------
    {
        mv::formation_detail::ring_entry r[8 * 3 + 1];
        const int                        n1 = mv::formation_detail::ring_order(1, r);
        ck(n1 == 8 && r[0].dx == -1 && r[0].dy == 0 && r[1].dx == 0 && r[1].dy == -1 && r[2].dx == 0 && r[2].dy == 1 && r[3].dx == 1 && r[3].dy == 0, "T4b ring 1: orthogonal first, by (dx, dy)");
        ck(r[4].dx == -1 && r[4].dy == -1 && r[7].dx == 1 && r[7].dy == 1, "T4b ring 1: then the diagonals, (-1,-1) first, (1,1) last");
        const int n2 = mv::formation_detail::ring_order(2, r);
        ck(n2 == 16 && r[0].dx == -2 && r[0].dy == 0 && r[3].dx == 2 && r[3].dy == 0 && r[15].dx == 2 && r[15].dy == 2, "T4b ring 2: 16 tiles, d2 4 first ((-2,0) .. (2,0)), (2,2) last");
        const int n0 = mv::formation_detail::ring_order(0, r);
        ck(n0 == 1 && r[0].dx == 0 && r[0].dy == 0, "T4b ring 0: the centre");
    }
}

// ============================================================================================================================
// T4c -- the move-as-one rule (move_formation_click = 3, mods:M1G 4b v3) over the same fake world
//   The expected tiles come from an independent Python re-expression of tmp/rc18dig/spec.md section 2 (not from this code): median centre,
//   core, moment vs 1.5 x ideal, truncated shift, fixed core + Hungarian rest (SHAPE) or a square block gathered on the click (GATHER).
// ============================================================================================================================
using fc_pts = std::vector<std::pair<int, int>>;
struct fc3_row {
    const char         *name;
    fc_pts              start, others, walls;
    std::pair<int, int> click;
    fc_pts              want;
};
const fc3_row FC3_ROWS[] = {
    // rc18 (50,32): shape d=(0, 2) core=16
    {"rc18", {{49, 29}, {48, 30}, {51, 30}, {50, 29}, {51, 29}, {49, 30}, {50, 31}, {48, 28}, {48, 31}, {49, 28}, {51, 31}, {48, 29}, {49, 31}, {50, 28}, {51, 28}, {50, 30}}, {{49, 20}, {49, 21}, {50, 22}}, {{45, 25}, {45, 26}, {45, 27}, {45, 28}, {45, 31}, {45, 32}, {46, 25}, {46, 26}, {46, 27}, {46, 28}, {46, 32}, {47, 25}, {47, 27}, {49, 25}, {49, 32}, {50, 33}, {50, 35}, {52, 31}, {53, 30}, {53, 31}, {53, 32}, {53, 33}, {53, 34}, {53, 35}, {54, 27}, {54, 30}, {54, 31}}, {50, 32}, {{49, 31}, {48, 32}, {51, 32}, {50, 31}, {51, 31}, {52, 32}, {50, 34}, {48, 30}, {48, 33}, {49, 30}, {51, 33}, {48, 31}, {49, 33}, {50, 30}, {51, 30}, {50, 32}}},
    // rc17 O1 (50, 31): shape d=(1, 1) core=11
    {"o1", {{44, 29}, {48, 30}, {49, 29}, {47, 28}, {48, 28}, {47, 29}, {49, 30}, {42, 28}, {41, 29}, {43, 29}, {50, 30}, {41, 28}, {49, 31}, {46, 29}, {48, 27}, {48, 29}}, {}, {{38, 27}, {38, 28}, {38, 29}, {38, 30}, {39, 29}, {39, 30}, {40, 24}, {40, 25}, {40, 30}, {40, 31}, {40, 32}, {41, 24}, {41, 25}, {41, 26}, {41, 27}, {41, 31}, {41, 32}, {42, 26}, {42, 27}, {42, 31}, {42, 32}, {43, 27}, {43, 28}, {43, 31}, {43, 32}, {44, 27}, {44, 28}, {44, 31}, {44, 32}, {45, 25}, {45, 26}, {45, 27}, {45, 28}, {45, 31}, {45, 32}, {46, 25}, {46, 26}, {46, 27}, {46, 28}, {46, 32}, {47, 24}, {47, 25}, {47, 27}, {48, 24}, {49, 25}, {49, 32}, {50, 33}, {52, 31}, {53, 30}, {53, 31}, {53, 32}, {53, 33}, {53, 34}}, {50, 31}, {{51, 30}, {49, 31}, {50, 30}, {48, 29}, {49, 29}, {48, 30}, {50, 31}, {50, 29}, {48, 32}, {51, 32}, {51, 31}, {48, 31}, {50, 32}, {47, 30}, {49, 28}, {49, 30}}},
    // rc17 O7 (44, 30): shape d=(-4, 0) core=15
    {"o7", {{53, 17}, {48, 30}, {51, 28}, {49, 29}, {51, 29}, {48, 28}, {49, 28}, {50, 30}, {47, 30}, {48, 31}, {48, 32}, {47, 31}, {51, 30}, {48, 29}, {47, 29}, {50, 31}}, {}, {{41, 24}, {41, 25}, {41, 26}, {41, 27}, {41, 31}, {41, 32}, {42, 20}, {42, 26}, {42, 27}, {42, 31}, {42, 32}, {43, 27}, {43, 28}, {43, 31}, {43, 32}, {44, 21}, {44, 22}, {44, 27}, {44, 28}, {44, 31}, {44, 32}, {45, 17}, {45, 18}, {45, 21}, {45, 22}, {45, 23}, {45, 25}, {45, 26}, {45, 27}, {45, 28}, {45, 31}, {45, 32}, {46, 17}, {46, 18}, {46, 20}, {46, 21}, {46, 22}, {46, 23}, {46, 25}, {46, 26}, {46, 27}, {46, 28}, {46, 32}, {47, 16}, {47, 17}, {47, 18}, {47, 20}, {47, 21}, {47, 22}, {47, 23}, {47, 24}, {47, 25}, {47, 27}, {48, 20}, {48, 21}, {48, 22}, {48, 23}, {48, 24}, {49, 14}, {49, 15}, {49, 19}, {49, 25}, {49, 32}, {50, 14}, {50, 15}, {50, 18}, {50, 19}, {50, 20}, {50, 33}, {50, 35}, {51, 18}, {51, 19}, {51, 20}, {52, 18}, {52, 19}, {52, 20}, {52, 31}, {53, 18}, {53, 19}, {53, 20}, {53, 21}, {53, 30}, {53, 31}, {53, 32}, {53, 33}, {53, 34}, {53, 35}, {54, 18}, {54, 19}, {54, 20}, {54, 24}, {54, 27}, {54, 30}, {54, 31}, {55, 26}, {55, 27}, {55, 28}, {55, 29}, {55, 30}, {56, 14}, {56, 25}, {56, 26}, {56, 27}}, {44, 30}, {{46, 29}, {44, 30}, {47, 28}, {45, 29}, {47, 29}, {42, 28}, {42, 29}, {46, 30}, {43, 30}, {42, 30}, {45, 30}, {41, 30}, {47, 30}, {44, 29}, {43, 29}, {46, 31}}},
    // F14 in place (22,22): shape d=(0, 0) core=16
    {"f14", {{20, 20}, {21, 20}, {22, 20}, {23, 20}, {20, 21}, {21, 21}, {22, 21}, {23, 21}, {20, 22}, {21, 22}, {22, 22}, {23, 22}, {20, 23}, {21, 23}, {22, 23}, {23, 23}}, {}, {}, {22, 22}, {{20, 20}, {21, 20}, {22, 20}, {23, 20}, {20, 21}, {21, 21}, {22, 21}, {23, 21}, {20, 22}, {21, 22}, {22, 22}, {23, 22}, {20, 23}, {21, 23}, {22, 23}, {23, 23}}},
    // F15 far (32,22): shape d=(10, 0) core=16
    {"f15", {{20, 20}, {21, 20}, {22, 20}, {23, 20}, {20, 21}, {21, 21}, {22, 21}, {23, 21}, {20, 22}, {21, 22}, {22, 22}, {23, 22}, {20, 23}, {21, 23}, {22, 23}, {23, 23}}, {}, {}, {32, 22}, {{30, 20}, {31, 20}, {32, 20}, {33, 20}, {30, 21}, {31, 21}, {32, 21}, {33, 21}, {30, 22}, {31, 22}, {32, 22}, {33, 22}, {30, 23}, {31, 23}, {32, 23}, {33, 23}}},
    // F16 wall (28,22): shape d=(6, 0) core=16. 0.2.1 hotfix: the two members the shifted block would put on the wall column (29) used to be refilled to (30,21) / (30,22) -- the
    // far side of the 10-tall wall, 1 ring away but ~11 steps by path; the path-distance ranking takes (25,22) and (26,24) on the click's side instead (members 3 and 7).
    {"f16", {{20, 20}, {21, 20}, {22, 20}, {23, 20}, {20, 21}, {21, 21}, {22, 21}, {23, 21}, {20, 22}, {21, 22}, {22, 22}, {23, 22}, {20, 23}, {21, 23}, {22, 23}, {23, 23}}, {}, {{29, 17}, {29, 18}, {29, 19}, {29, 20}, {29, 21}, {29, 22}, {29, 23}, {29, 24}, {29, 25}, {29, 26}}, {28, 22}, {{26, 20}, {27, 20}, {28, 20}, {25, 22}, {26, 21}, {27, 21}, {28, 21}, {26, 24}, {26, 22}, {27, 22}, {28, 22}, {27, 24}, {26, 23}, {27, 23}, {28, 23}, {28, 24}}},
    // S2 own edge (23,21): shape d=(1, 0) core=16
    {"s2", {{20, 20}, {21, 20}, {22, 20}, {23, 20}, {20, 21}, {21, 21}, {22, 21}, {23, 21}, {20, 22}, {21, 22}, {22, 22}, {23, 22}, {20, 23}, {21, 23}, {22, 23}, {23, 23}}, {}, {}, {23, 21}, {{21, 20}, {22, 20}, {23, 20}, {24, 20}, {21, 21}, {22, 21}, {23, 21}, {24, 21}, {21, 22}, {22, 22}, {23, 22}, {24, 22}, {21, 23}, {22, 23}, {23, 23}, {24, 23}}},
    // S3 1 below (22,24): shape d=(0, 2) core=16
    {"s3", {{20, 20}, {21, 20}, {22, 20}, {23, 20}, {20, 21}, {21, 21}, {22, 21}, {23, 21}, {20, 22}, {21, 22}, {22, 22}, {23, 22}, {20, 23}, {21, 23}, {22, 23}, {23, 23}}, {}, {}, {22, 24}, {{20, 22}, {21, 22}, {22, 22}, {23, 22}, {20, 23}, {21, 23}, {22, 23}, {23, 23}, {20, 24}, {21, 24}, {22, 24}, {23, 24}, {20, 25}, {21, 25}, {22, 25}, {23, 25}}},
    // S5 spread (22,22): gather w=4 h=4
    {"s5", {{21, 18}, {22, 26}, {16, 17}, {24, 17}, {21, 25}, {16, 24}, {19, 16}, {17, 22}, {22, 17}, {19, 17}, {24, 22}, {16, 25}, {17, 19}, {26, 26}, {25, 16}, {25, 25}}, {}, {}, {22, 22}, {{22, 21}, {22, 23}, {20, 20}, {23, 21}, {21, 23}, {20, 22}, {21, 20}, {21, 22}, {22, 20}, {21, 21}, {22, 22}, {20, 23}, {20, 21}, {23, 23}, {23, 20}, {23, 22}}},
    // S7 loose (22,22): gather w=4 h=4
    {"s7", {{18, 18}, {20, 18}, {22, 18}, {24, 18}, {18, 20}, {20, 20}, {22, 20}, {24, 20}, {18, 22}, {20, 22}, {22, 22}, {24, 22}, {18, 24}, {20, 24}, {22, 24}, {24, 24}}, {}, {}, {22, 22}, {{20, 20}, {21, 20}, {22, 20}, {23, 20}, {20, 21}, {21, 21}, {22, 21}, {23, 21}, {20, 22}, {21, 22}, {22, 22}, {23, 22}, {20, 23}, {21, 23}, {22, 23}, {23, 23}}},
    // S8 line far (34,22): gather w=4 h=4
    {"s8", {{16, 22}, {17, 22}, {18, 22}, {19, 22}, {20, 22}, {21, 22}, {22, 22}, {23, 22}, {16, 23}, {17, 23}, {18, 23}, {19, 23}, {20, 23}, {21, 23}, {22, 23}, {23, 23}}, {}, {}, {34, 22}, {{32, 20}, {32, 21}, {33, 21}, {33, 20}, {34, 20}, {34, 21}, {35, 21}, {35, 20}, {32, 23}, {32, 22}, {33, 23}, {33, 22}, {34, 23}, {34, 22}, {35, 23}, {35, 22}}},
    // S9 line own end (16,22): gather w=4 h=4
    {"s9", {{16, 22}, {17, 22}, {18, 22}, {19, 22}, {20, 22}, {21, 22}, {22, 22}, {23, 22}, {16, 23}, {17, 23}, {18, 23}, {19, 23}, {20, 23}, {21, 23}, {22, 23}, {23, 23}}, {}, {}, {16, 22}, {{14, 20}, {14, 21}, {15, 20}, {15, 21}, {16, 21}, {16, 20}, {17, 20}, {17, 21}, {14, 23}, {14, 22}, {15, 22}, {15, 23}, {16, 22}, {16, 23}, {17, 22}, {17, 23}}},
};
constexpr int FC3_RULE = mv::FC_RULE_MOVE_AS_ONE;

// units 7+m stand at `start`, the unit marks (occupant) as the live world has them; `others` = foreign units, `walls` = impassable
void fc3_setup(fake_formation &w, const fc3_row &r) {
    const uint32_t N = (uint32_t)r.start.size();
    w.n              = (int32_t)N;
    for (uint32_t m = 0; m < N; ++m) {
        w.handle[m]                                                                     = 7 + m;
        w.col[m]                                                                        = (uint8_t)r.start[m].first;
        w.row[m]                                                                        = (uint8_t)r.start[m].second;
        w.tile[m * 2 + 0]                                                               = (uint8_t)r.start[m].first;
        w.tile[m * 2 + 1]                                                               = (uint8_t)r.start[m].second;
        w.units[7 + m].order                                                            = 0x0f;
        w.units[7 + m].state                                                            = 0x0a;
        w.units[7 + m].x                                                                = (uint8_t)r.start[m].first;
        w.units[7 + m].y                                                                = (uint8_t)r.start[m].second;
        w.units[7 + m].group                                                            = 5;
        w.units[7 + m].goal_x                                                           = (uint8_t)r.click.first;
        w.units[7 + m].goal_y                                                           = (uint8_t)r.click.second;
        w.units[7 + m].path_slot                                                        = (uint8_t)(10 + m);
        w.first_run[10 + m]                                                             = 3;
        w.occ[fake_arrival::T((uint32_t)r.start[m].first, (uint32_t)r.start[m].second)] = (int32_t)(7 + m);
    }
    for (size_t i = 0; i < r.others.size(); ++i)
        w.occ[fake_arrival::T((uint32_t)r.others[i].first, (uint32_t)r.others[i].second)] = (int32_t)(60 + i);
    for (const auto &p : r.walls) w.truep[fake_arrival::T((uint32_t)p.first, (uint32_t)p.second)] = 0;
    w.gx = w.ax = r.click.first;
    w.gy = w.ay                                                                 = r.click.second;
    w.field[fake_arrival::T((uint32_t)r.click.first, (uint32_t)r.click.second)] = 1; // the flood was seeded at the click: the field is fresh
}

void t4c_move_as_one() {
    using fw = fake_formation;
    // ---- the pure helpers -----------------------------------------------------------------------------------------------------
    {
        using namespace mv::formation_detail;
        ck(trunc_div(7, 2) == 3 && trunc_div(-7, 2) == -3 && trunc_div(-1, 16) == 0 && trunc_div(0, 5) == 0 && trunc_div(56, 16) == 3, "T4c trunc_div: toward zero");
        ck(wrap_delta(1, 62, 63) == 3 && wrap_delta(62, 1, 63) == -3 && wrap_delta(5, 5, 63) == 0 && wrap_delta(32, 0, 63) == -32 && wrap_delta(31, 0, 63) == 31, "T4c wrap_delta: the shortest way round a 64 torus");
        ck(ideal_radius(1) == 0 && ideal_radius(2) == 1 && ideal_radius(9) == 1 && ideal_radius(10) == 2 && ideal_radius(16) == 2 && ideal_radius(25) == 2 && ideal_radius(26) == 3, "T4c ideal_radius: smallest r with (2r+1)^2 >= n");
        ck(ideal_moment(1) == 0 && ideal_moment(2) == 2 && ideal_moment(3) == 12 && ideal_moment(4) == 44 && ideal_moment(9) == 972 && ideal_moment(16) == 10544,
           "T4c ideal_moment: the ring-order blob's moment (0, 2, 12, 44, 972, 10544 for 1, 2, 3, 4, 9, 16)");
    }
    // ---- the scenarios ---------------------------------------------------------------------------------------------------------
    for (const auto &r : FC3_ROWS) {
        fw w;
        fc3_setup(w, r);
        const bool handled = mv::formation_after_pathfind(w, 5, 1, 1, FC3_RULE);
        bool       same    = handled;
        for (size_t m = 0; same && m < r.want.size(); ++m)
            if (w.tile_of_member((int)m) != fw::T((uint32_t)r.want[m].first, (uint32_t)r.want[m].second)) {
                same = false;
                std::printf("T4c %s: member %zu (%d,%d) got (%u,%u), want (%d,%d)\n", r.name, m, r.start[m].first, r.start[m].second, w.tile[m * 2], w.tile[m * 2 + 1], r.want[m].first, r.want[m].second);
            }
        if (!same) {
            std::printf("T4c %s: got", r.name);
            for (size_t m = 0; m < r.start.size(); ++m) std::printf(" {%u, %u},", w.tile[m * 2], w.tile[m * 2 + 1]);
            std::printf("\n");
        }
        char msg[96];
        std::snprintf(msg, sizeof(msg), "T4c %s: every member's tile == the spec model's assignment", r.name);
        ck(same, msg);
        bool goals_ok = handled;
        for (size_t m = 0; goals_ok && m < r.want.size(); ++m)
            if (w.units[7 + m].goal_x != r.want[m].first || w.units[7 + m].goal_y != r.want[m].second) goals_ok = false;
        std::snprintf(msg, sizeof(msg), "T4c %s: ... and so is every unit's goal", r.name);
        ck(goals_ok, msg);
    }
    { // the three headline cases, by name
        fw w;
        fc3_setup(w, FC3_ROWS[3]); // f14
        ck(mv::formation_after_pathfind(w, 5, 1, 1, FC3_RULE) && w.replans == 0 && w.last_kept == 16 && w.last_filled == 0, "T4c F14 in place: all 16 kept, nothing planned, nobody moves (the deadband)");
        fw w2;
        fc3_setup(w2, FC3_ROWS[4]); // f15
        bool rigid = mv::formation_after_pathfind(w2, 5, 1, 1, FC3_RULE);
        for (int m = 0; m < 16; ++m)
            if (w2.tile_of_member(m) != fw::T((uint32_t)FC3_ROWS[4].start[m].first + 10, (uint32_t)FC3_ROWS[4].start[m].second)) rigid = false;
        ck(rigid, "T4c F15 far: every unit moves exactly (10,0)");
    }
    // ---- queued groupmates: planned with the wave, stamped, kept by the later wave -------------------------------------------------
    {
        const fc3_row *s3 = nullptr;
        for (const auto &x : FC3_ROWS)
            if (std::strcmp(x.name, "s3") == 0) s3 = &x;
        ck(s3 != nullptr, "T4c: the S3 row exists");
        if (s3) {
            // wave 1 = the first 10 units; units 6..15 (handles 7+10..) wait in state 0x0a with the goal = the click
            fw w;
            fc3_setup(w, *s3);
            w.n = 10; // the wave is the first ten; the other six are queued (fc3_setup left them in 0x0a, goal = click, group 5)
            ck(mv::formation_after_pathfind(w, 5, 1, 1, FC3_RULE), "T4c queued: wave 1 handled");
            bool wave_ok = true, stamp_ok = true;
            for (int m = 0; m < 10; ++m)
                if (w.tile_of_member(m) != fw::T((uint32_t)s3->want[m].first, (uint32_t)s3->want[m].second)) wave_ok = false;
            for (int m = 10; m < 16; ++m)
                if (w.units[7 + m].goal_x != s3->want[m].first || w.units[7 + m].goal_y != s3->want[m].second) stamp_ok = false;
            ck(wave_ok, "T4c queued: the wave's tiles == the plan over the whole order (as if all 16 were one wave)");
            ck(stamp_ok, "T4c queued: each queued unit's goal is STAMPED with its target");
            // wave 2: the six queued units, goals != click now. Wave-1 members stand in hop state 0x0c with their targets as goals.
            fw w2;
            fc3_setup(w2, *s3);
            for (int m = 0; m < 10; ++m) {
                w2.units[7 + m].state  = 0x0c;
                w2.units[7 + m].goal_x = (uint8_t)s3->want[m].first;
                w2.units[7 + m].goal_y = (uint8_t)s3->want[m].second;
            }
            for (int m = 10; m < 16; ++m) {
                w2.units[7 + m].goal_x = (uint8_t)s3->want[m].first;
                w2.units[7 + m].goal_y = (uint8_t)s3->want[m].second;
            }
            for (int k = 0; k < 6; ++k) { // the wave scratch holds units 10..15 only
                w2.handle[k]       = 7 + 10 + k;
                w2.col[k]          = (uint8_t)s3->start[10 + k].first;
                w2.row[k]          = (uint8_t)s3->start[10 + k].second;
                w2.tile[k * 2]     = w2.col[k];
                w2.tile[k * 2 + 1] = w2.row[k];
            }
            w2.n = 6;
            ck(mv::formation_after_pathfind(w2, 5, 1, 1, FC3_RULE), "T4c queued: wave 2 handled");
            bool keep_ok = true;
            for (int k = 0; k < 6; ++k)
                if (w2.tile_of_member(k) != fw::T((uint32_t)s3->want[10 + k].first, (uint32_t)s3->want[10 + k].second)) keep_ok = false;
            ck(keep_ok, "T4c queued: wave 2 keeps the tile wave 1 stamped for every member (no re-centring per wave)");
        }
    }
    // ---- a group across the map seam: positions are unwrapped around the first member -------------------------------------------------
    {
        fw w;
        w.mask = 63;
        fc3_row r;
        r.name = "seam";
        for (int j = 0; j < 4; ++j)
            for (int i = 0; i < 4; ++i) r.start.push_back({(62 + i) & 63, 20 + j});
        r.click = {3, 22};
        fc3_setup(w, r);
        ck(mv::formation_after_pathfind(w, 5, 1, 1, FC3_RULE), "T4c seam: handled");
        bool shift = true;
        for (int m = 0; m < 16; ++m)
            if (w.tile_of_member(m) != fw::T((uint32_t)((r.start[m].first + 3) & 63), (uint32_t)r.start[m].second)) shift = false;
        ck(shift, "T4c seam: a 4x4 block across x = 63/0 moves rigidly by (3,0) (the centroid fix's wrap rule)");
    }
    // ---- n == 1 and the lone-straggler rule -------------------------------------------------------------------------------------------
    {
        fw w;
        w.members({{10, 10}});
        w.units[7].goal_x = 30;
        w.units[7].goal_y = 30;
        w.tile[0]         = 30;
        w.tile[1]         = 30;
        ck(mv::formation_after_pathfind(w, 5, 1, 1, FC3_RULE) && w.replans == 0 && w.tile_of_member(0) == fw::T(30, 30), "T4c n=1: the member's goal is the click: unchanged");
        fw w2; // planned by an earlier wave: its own goal (stamped) is kept when eligible, even if it is not near the click
        w2.members({{10, 10}});
        w2.units[7].goal_x = 33;
        w2.units[7].goal_y = 31;
        ck(mv::formation_after_pathfind(w2, 5, 1, 1, FC3_RULE) && w2.planned_goal.size() == 1 && w2.planned_goal[0] == fw::T(33, 31), "T4c n=1: a stamped goal != click is kept and planned alone");
        fw w3; // ... and replaced by the first ring tile when it is no longer eligible
        w3.members({{10, 10}});
        w3.units[7].goal_x      = 33;
        w3.units[7].goal_y      = 31;
        w3.truep[fw::T(33, 31)] = 0;
        ck(mv::formation_after_pathfind(w3, 5, 1, 1, FC3_RULE) && w3.planned_goal.size() == 1 && w3.planned_goal[0] == fw::T(30, 30), "T4c n=1: a stamped goal that went ineligible -> the first ring tile");
    }
    // ---- the old rule is untouched by the new code (key 1, 2) ------------------------------------------------------------------------
    {
        const fc3_row *s3 = nullptr;
        for (const auto &x : FC3_ROWS)
            if (std::strcmp(x.name, "s3") == 0) s3 = &x;
        if (s3) {
            fw w;
            fc3_setup(w, *s3);
            ck(mv::formation_after_pathfind(w, 5, 1, 1, 1), "T4c old rule: handled");
            ck(w.last_kept > 0 && w.last_kept < 16, "T4c old rule (key 2) on S3 keeps the near units (the crossing the new rule removes)");
            int moved = 0;
            for (int m = 0; m < 16; ++m)
                if (w.tile_of_member(m) != fw::T((uint32_t)s3->start[m].first, (uint32_t)s3->start[m].second)) ++moved;
            ck(moved == 4, "T4c old rule (key 2) on S3: 4 units move, as the spec's table says (4/17)");
        }
    }
}

void t5_route_through() {
    mv::rt_undo undo;
    {
        fake_rt w;
        for (int i = 1; i < 5; ++i) w.make_idle(i);
        ck(mv::rt_unmask(w, undo, 0, true, 2, 0, 0, 0xff, 0xff) == 0 && w.pas[((10u + 1) << 8) | 20u] == 0, "T5: mode 0 opens nothing");
        ck(mv::rt_unmask(w, undo, 1, false, 2, 0, 0, 0xff, 0xff) == 0 && w.pas[((10u + 1) << 8) | 20u] == 0, "T5: nudge key off: nothing opened");
        ck(mv::rt_unmask(w, undo, 1, true, 8, 0, 0, 0xff, 0xff) == 0, "T5: a player >= 8 opens nothing");
    }
    {
        fake_rt w;
        for (int i = 1; i < 6; ++i) w.make_idle(i);
        const auto pas0 = w.pas;
        const auto fl0  = w.fl;
        const int  n    = mv::rt_unmask(w, undo, 1, true, 2, 200, 200, 0xff, 0xff);
        ck(n == 5 && undo.n == 5, "T5: five idle friends opened");
        bool open = true, pen = true;
        for (int i = 1; i < 6; ++i) {
            const uint32_t f = ((uint32_t)(10 + i) << 8) | 20u;
            if (w.pas[f] != 1) open = false;
            if ((w.fl[f] & 0x1du) != 0x1du || (w.fl[f] & 0xffffff00u) != 0xabcd0000u) pen = false;
        }
        ck(open, "T5: opened tiles hold the unit's own passable value (origin_tile_was_passable)");
        ck(pen, "T5: mode 1 ORs the penalty bits 0x1d into the cached cost byte, upper bits untouched");
        mv::rt_restore(w, undo);
        ck(w.pas == pas0 && w.fl == fl0 && undo.n == 0, "T5: rt_restore is byte-exact (passable plane and cost words equal before / after)");
    }
    {
        fake_rt w;
        w.make_idle(1);
        mv::rt_unmask(w, undo, 2, true, 2, 200, 200, 0xff, 0xff);
        ck(w.pas[((11u) << 8) | 20u] == 1 && w.fl[((11u) << 8) | 20u] == 0xabcd0001u, "T5: mode 2 opens the tile at the plain cost (no penalty)");
        mv::rt_restore(w, undo);
        ck(w.pas[((11u) << 8) | 20u] == 0 && w.fl[((11u) << 8) | 20u] == 0xabcd0001u, "T5: ... and restores");
    }
    // each disqualifier
    struct dq {
        const char *name;
        void (*edit)(fake_rt &, int);
    };
    static const dq dqs[] = {
        {"state != idle", [](fake_rt &w, int i) { w.u[(size_t)i].state = 0x0f; }},
        {"order queued", [](fake_rt &w, int i) { w.u[(size_t)i].order_queued = 1; }},
        {"still sliding (microstep < 0x1f)", [](fake_rt &w, int i) { w.u[(size_t)i].move_microstep = 0x1e; }},
        {"origin tile impassable", [](fake_rt &w, int i) { w.u[(size_t)i].origin_tile_was_passable = 0; }},
        {"not a ground/idle class", [](fake_rt &w, int i) { w.u[(size_t)i].cfg_ground_idle = false; }},
        {"dead", [](fake_rt &w, int i) { w.u[(size_t)i].alive = false; }},
        {"wrong tile building index", [](fake_rt &w, int i) { w.bld[((uint32_t)w.u[(size_t)i].x << 8) | w.u[(size_t)i].y] = 77; }},
        {"another player's unit on the tile", [](fake_rt &w, int i) { w.cls[((uint32_t)w.u[(size_t)i].x << 8) | w.u[(size_t)i].y] = 0x81; }},
        {"an unit-less class (building)", [](fake_rt &w, int i) { w.cls[((uint32_t)w.u[(size_t)i].x << 8) | w.u[(size_t)i].y] = 0x42; }},
        {"passable already nonzero", [](fake_rt &w, int i) { w.pas[((uint32_t)w.u[(size_t)i].x << 8) | w.u[(size_t)i].y] = 1; }},
    };
    for (const dq &d : dqs) {
        fake_rt w;
        w.make_idle(3);
        d.edit(w, 3);
        const int n = mv::rt_unmask(w, undo, 1, true, 2, 200, 200, 0xff, 0xff);
        char      what[96];
        snprintf(what, sizeof what, "T5: a friend that is %s stays a wall", d.name);
        ck(n == 0, what);
    }
    {
        fake_rt w;
        w.make_idle(3);
        ck(mv::rt_unmask(w, undo, 1, true, 2, 13, 20, 0xff, 0xff) == 0, "T5: the goal tile itself stays a wall");
        mv::rt_restore(w, undo);
        ck(mv::rt_unmask(w, undo, 1, true, 2, 13 + 256, 20, 0xff, 0xff) == 0, "T5: ... the goal is wrapped through the mask first");
        mv::rt_restore(w, undo);
        w.make_idle(0); // unit index 0 is never visited (the loop starts at 1)
        w.u[0].x                  = 100;
        w.u[0].y                  = 100;
        w.bld[(100u << 8) | 100u] = 0;
        w.cls[(100u << 8) | 100u] = 0x82;
        w.pas[(100u << 8) | 100u] = 0;
        ck(mv::rt_unmask(w, undo, 1, true, 2, 200, 200, 0xff, 0xff) == 1, "T5: unit index 0 is skipped, index 3 opened");
        mv::rt_restore(w, undo);
    }
    {
        fake_rt w;
        w.cap = 1500;
        for (int i = 1; i < 1500; ++i) w.make_idle(i);
        const int n = mv::rt_unmask(w, undo, 1, true, 2, 255, 255, 0xff, 0xff);
        ck(n == mv::RT_MAX && undo.n == mv::RT_MAX, "T5: the undo list caps the opened tiles at RT_MAX");
        mv::rt_restore(w, undo);
        int left = 0;
        for (int i = 1; i < 1500; ++i)
            if (w.pas[((uint32_t)w.u[(size_t)i].x << 8) | w.u[(size_t)i].y] != 0) ++left;
        ck(left == 0, "T5: ... and every one of them is restored");
    }
}

// T5b -- rt_unmask_moving (move_route_through_moving): walking friends opened and priced, everything else untouched
static void make_walker(fake_rt &w, int i, uint8_t pass_now) {
    w.make_idle(i);
    mv::rt_unit &r    = w.u[(size_t)i];
    r.state           = mv::RT_ST_WALK;
    r.cfg_ground_move = true;
    r.move_microstep  = pass_now == 5 ? 7 : mv::RT_MS_SETTLED;
    const uint32_t f  = ((uint32_t)r.x << 8) | r.y;
    w.pas[f]          = pass_now;
}

void t5b_route_through_moving() {
    mv::rt_undo undo;
    {
        fake_rt w;
        make_walker(w, 1, 0); // settled between steps
        make_walker(w, 2, 5); // sliding into its tile
        const auto pas0 = w.pas;
        const auto fl0  = w.fl;
        ck(mv::rt_unmask_moving(w, undo, -1, 2, 200, 200, 0xff, 0xff) == 0 && w.pas == pas0, "T5b: level -1 (key off) opens nothing");
        ck(mv::rt_unmask_moving(w, undo, 3, 8, 200, 200, 0xff, 0xff) == 0, "T5b: a player >= 8 opens nothing");
        ck(mv::rt_unmask(w, undo, 0, true, 2, 200, 200, 0xff, 0xff) == 0 && undo.n == 0, "T5b: the idle pass with its key off opens nothing and resets the list");
        const int n = mv::rt_unmask_moving(w, undo, 3, 2, 200, 200, 0xff, 0xff);
        ck(n == 2 && undo.n == 2, "T5b: a settled and a sliding walker are opened");
        const uint32_t f1 = (11u << 8) | 20u, f2 = (12u << 8) | 20u;
        ck(w.pas[f1] == 1 && w.pas[f2] == 1, "T5b: both hold the unit's own passable value");
        ck((w.fl[f1] & 0xffu) == 0x1du && (w.fl[f2] & 0xffu) == 0x1du && (w.fl[f1] & 0xffffff00u) == 0xabcd0000u,
           "T5b: level 3 = the idle key's ring bits 0x1d, upper bits untouched");
        mv::rt_restore(w, undo);
        ck(w.pas == pas0 && w.fl == fl0 && w.pas[f2] == 5, "T5b: restore is byte-exact (the sliding tile goes back to 5, not 0)");
    }
    {
        static const uint32_t want[5] = {0x01u, 0x05u, 0x0du, 0x1du, 0x1fu};
        bool                  ok      = true;
        for (int l = 0; l < 5; ++l) {
            fake_rt w;
            make_walker(w, 1, 0);
            mv::rt_unmask_moving(w, undo, l, 2, 200, 200, 0xff, 0xff);
            if ((w.fl[(11u << 8) | 20u] & 0xffu) != (want[l] | 1u)) ok = false;
            mv::rt_restore(w, undo);
        }
        ck(ok, "T5b: cost levels 0..4 OR in 0x01 / 0x05 / 0x0d / 0x1d / 0x1f");
        ck(mv::rt_move_cost_bits(-3) == 0x01u && mv::rt_move_cost_bits(99) == 0x1fu, "T5b: levels clamp");
    }
    {
        fake_rt w; // idle units are not touched by the moving pass; walkers not by the idle pass
        w.make_idle(1);
        make_walker(w, 2, 0);
        ck(mv::rt_unmask_moving(w, undo, 3, 2, 200, 200, 0xff, 0xff) == 1 && w.pas[(11u << 8) | 20u] == 0 && w.pas[(12u << 8) | 20u] == 1,
           "T5b: the moving pass opens the walker only, an idle friend stays a wall");
        mv::rt_restore(w, undo);
        ck(mv::rt_unmask(w, undo, 1, true, 2, 200, 200, 0xff, 0xff) == 1 && w.pas[(11u << 8) | 20u] == 1 && w.pas[(12u << 8) | 20u] == 0,
           "T5b: the idle pass opens the idle friend only, a walker stays a wall");
        mv::rt_restore(w, undo);
        ck(mv::rt_unmask(w, undo, 1, true, 2, 200, 200, 0xff, 0xff) == 1 && mv::rt_unmask_moving(w, undo, 3, 2, 200, 200, 0xff, 0xff) == 1 && undo.n == 2,
           "T5b: the passes share one undo list (append)");
        mv::rt_restore(w, undo);
        ck(w.pas[(11u << 8) | 20u] == 0 && w.pas[(12u << 8) | 20u] == 0 && w.fl[(11u << 8) | 20u] == 0xabcd0001u, "T5b: ... and both restore");
    }
    struct dq {
        const char *name;
        void (*edit)(fake_rt &, int);
    };
    static const dq dqs[] = {
        {"not walking (idle state)", [](fake_rt &w, int i) { w.u[(size_t)i].state = mv::RT_ST_IDLE; }},
        {"in another state (marshal 0x0a)", [](fake_rt &w, int i) { w.u[(size_t)i].state = 0x0a; }},
        {"origin tile impassable", [](fake_rt &w, int i) { w.u[(size_t)i].origin_tile_was_passable = 0; }},
        {"not a ground mover", [](fake_rt &w, int i) { w.u[(size_t)i].cfg_ground_move = false; }},
        {"dead", [](fake_rt &w, int i) { w.u[(size_t)i].alive = false; }},
        {"wrong tile building index", [](fake_rt &w, int i) { w.bld[((uint32_t)w.u[(size_t)i].x << 8) | w.u[(size_t)i].y] = 77; }},
        {"an ally's / enemy's unit on the tile", [](fake_rt &w, int i) { w.cls[((uint32_t)w.u[(size_t)i].x << 8) | w.u[(size_t)i].y] = 0x81; }},
        {"a building on the tile", [](fake_rt &w, int i) { w.cls[((uint32_t)w.u[(size_t)i].x << 8) | w.u[(size_t)i].y] = 0x42; }},
        {"a member of the order's own group (lifted: passable == origin)", [](fake_rt &w, int i) { w.pas[((uint32_t)w.u[(size_t)i].x << 8) | w.u[(size_t)i].y] = 1; }},
        {"on a tile with another passable value", [](fake_rt &w, int i) { w.pas[((uint32_t)w.u[(size_t)i].x << 8) | w.u[(size_t)i].y] = 9; }},
    };
    for (const dq &d : dqs) {
        fake_rt w;
        make_walker(w, 3, 0);
        d.edit(w, 3);
        const int n = mv::rt_unmask_moving(w, undo, 3, 2, 200, 200, 0xff, 0xff);
        char      what[128];
        snprintf(what, sizeof what, "T5b: a friend that is %s stays a wall", d.name);
        ck(n == 0, what);
    }
    {
        fake_rt w;
        make_walker(w, 3, 0);
        ck(mv::rt_unmask_moving(w, undo, 3, 2, 13, 20, 0xff, 0xff) == 0, "T5b: the goal tile itself stays a wall");
        ck(mv::rt_unmask_moving(w, undo, 3, 2, 13 + 256, 20, 0xff, 0xff) == 0, "T5b: ... the goal is wrapped through the mask first");
        ck(mv::rt_unmask_moving(w, undo, 3, 3, 200, 200, 0xff, 0xff) == 0, "T5b: another player's units are not visited (the order owner's roster only)");
    }
}

// T5c -- rt_unmask_replanning (mods:M1DT fix 2, move_route_through_replanning): own units in state 0x0a are opened and priced
void t5c_route_through_replanning() {
    mv::rt_undo undo;
    auto        make_marshal = [](fake_rt &w, int i) {
        make_walker(w, i, 0);
        w.u[(size_t)i].state = mv::RT_ST_MARSHAL;
    };
    {
        fake_rt w;
        make_marshal(w, 1);
        const auto pas0 = w.pas;
        const auto fl0  = w.fl;
        ck(mv::rt_unmask_replanning(w, undo, -1, 2, 200, 200, 0xff, 0xff) == 0 && w.pas == pas0, "T5c: level -1 (key off) opens nothing");
        ck(mv::rt_unmask_replanning(w, undo, 0, 8, 200, 200, 0xff, 0xff) == 0, "T5c: a player >= 8 opens nothing");
        const int      n  = mv::rt_unmask_replanning(w, undo, 0, 2, 200, 200, 0xff, 0xff);
        const uint32_t f1 = (11u << 8) | 20u;
        ck(n == 1 && w.pas[f1] == 1 && (w.fl[f1] & 0xffu) == 0x01u, "T5c: a re-planning (0x0a) friend is opened at the plain tile cost (level 0)");
        mv::rt_restore(w, undo);
        ck(w.pas == pas0 && w.fl == fl0, "T5c: restore is byte-exact");
    }
    {
        fake_rt w; // walkers and idle friends are not touched by the replanning pass; marshal units not by the moving pass
        w.make_idle(1);
        make_walker(w, 2, 0);
        make_marshal(w, 3);
        ck(mv::rt_unmask_replanning(w, undo, 0, 2, 200, 200, 0xff, 0xff) == 1 && w.pas[(13u << 8) | 20u] == 1 && w.pas[(11u << 8) | 20u] == 0 &&
               w.pas[(12u << 8) | 20u] == 0,
           "T5c: only the 0x0a unit is opened");
        mv::rt_restore(w, undo);
        ck(mv::rt_unmask_moving(w, undo, 0, 2, 200, 200, 0xff, 0xff) == 1 && w.pas[(13u << 8) | 20u] == 0, "T5c: the moving pass leaves the 0x0a unit a wall");
        mv::rt_restore(w, undo);
        ck(mv::rt_unmask_moving(w, undo, 0, 2, 200, 200, 0xff, 0xff) == 1 && mv::rt_unmask_replanning(w, undo, 0, 2, 200, 200, 0xff, 0xff) == 1 && undo.n == 2,
           "T5c: the passes share one undo list (append)");
        mv::rt_restore(w, undo);
        ck(w.pas[(12u << 8) | 20u] == 0 && w.pas[(13u << 8) | 20u] == 0, "T5c: ... and both restore");
    }
    struct dq {
        const char *name;
        void (*edit)(fake_rt &, int);
    };
    static const dq dqs[] = {
        {"origin tile impassable", [](fake_rt &w, int i) { w.u[(size_t)i].origin_tile_was_passable = 0; }},
        {"not a ground mover", [](fake_rt &w, int i) { w.u[(size_t)i].cfg_ground_move = false; }},
        {"dead", [](fake_rt &w, int i) { w.u[(size_t)i].alive = false; }},
        {"an ally's / enemy's unit on the tile", [](fake_rt &w, int i) { w.cls[((uint32_t)w.u[(size_t)i].x << 8) | w.u[(size_t)i].y] = 0x81; }},
        {"a building on the tile", [](fake_rt &w, int i) { w.cls[((uint32_t)w.u[(size_t)i].x << 8) | w.u[(size_t)i].y] = 0x42; }},
        {"a member of the order's own group (lifted: passable == origin)", [](fake_rt &w, int i) { w.pas[((uint32_t)w.u[(size_t)i].x << 8) | w.u[(size_t)i].y] = 1; }},
    };
    for (const dq &d : dqs) {
        fake_rt w;
        make_marshal(w, 3);
        d.edit(w, 3);
        char what[128];
        snprintf(what, sizeof what, "T5c: a re-planning friend that is %s stays a wall", d.name);
        ck(mv::rt_unmask_replanning(w, undo, 0, 2, 200, 200, 0xff, 0xff) == 0, what);
    }
    {
        fake_rt w;
        make_marshal(w, 3);
        ck(mv::rt_unmask_replanning(w, undo, 0, 2, 13, 20, 0xff, 0xff) == 0, "T5c: the goal tile itself stays a wall");
        ck(mv::rt_unmask_replanning(w, undo, 0, 3, 200, 200, 0xff, 0xff) == 0, "T5c: another player's units are not visited");
    }
}

// ============================================================================================================================
// T6 / T7 -- the flood twin
// ============================================================================================================================
struct flood_world {
    sim_fixture    fx;
    llm_map_region region{};
    flood_world(int dim) {
        fx.reset();
        fx.map_width              = dim;
        fx.map_height             = dim;
        fx.region_coord_wrap_mask = (dim == 256) ? 0xffffu : (uint32_t)(((dim - 1) << 8) | (dim - 1));
        std::fill(fx.passable.begin(), fx.passable.end(), (uint8_t)0);
        region.route_mark   = 1;
        fx.region_list_head = &region;
    }
    void cell(int x, int y, uint8_t low) {
        fx.passable[(size_t)((x << 8) | y)] = 1;
        llm_map_region_cell &c              = fx.region_grid[(size_t)(x * MAP_GRID_DIM + y)];
        c.region                            = &region;
        c.terrain_flags                     = low;
    }
    uint32_t field(int x, int y) { return fx.region_grid[(size_t)(x * MAP_GRID_DIM + y)].terrain_flags; }
    int32_t  run(int32_t query, int32_t start) {
        sim_store own = fx.store();
        sim_view  v   = fx.view();
        return detail::region_flood_reachable(v, own, query, start);
    }
};

// A corridor x=0, y=0..4 with the low bytes 1 / 29 / 1 / 29 / 1.
void corridor_cells(flood_world &w) {
    const uint8_t low[5] = {1, 29, 1, 29, 1};
    for (int y = 0; y < 5; ++y) w.cell(0, y, low[y]);
}

void t6_flood_twin() {
    fixes_scope scope;
    // key off: the retail field
    {
        flood_world w(16);
        corridor_cells(w);
        const int32_t r = w.run(4, 0);
        ck_eq(r, 1u, "T6: the flood reaches the end of the corridor");
        ck_eq(w.field(0, 0), 0x100u | 1u, "T6: retail field: the seed");
        ck_eq(w.field(0, 1), 0x200u | 29u, "T6: retail field: cell 1 = 0x100 + (1<<8)");
        ck_eq(w.field(0, 2), 0x1f00u | 1u, "T6: retail field: cell 2 = 0x200 + (29<<8)");
        ck_eq(w.field(0, 3), 0x2000u | 29u, "T6: retail field: cell 3");
        ck_eq(w.field(0, 4), 0x3d00u | 1u, "T6: retail field: cell 4");
    }
    // route_cost_mode on with the retail table = the same field
    {
        move_fixes mf;
        mf.route_cost_mode = 1;
        set_move_fixes(mf);
        flood_world w(16);
        corridor_cells(w);
        w.run(4, 0);
        ck(w.field(0, 1) == (0x200u | 29u) && w.field(0, 2) == (0x1f00u | 1u) && w.field(0, 3) == (0x2000u | 29u) && w.field(0, 4) == (0x3d00u | 1u),
           "T6: route_cost_mode on, retail table (1/4/8/16): the very same field");
    }
    // a custom table: base 2, rings 1 / 3 / 5 -> low 1 costs 2, low 29 costs 11
    {
        move_fixes mf;
        mf.route_cost_mode   = 1;
        mf.route_cost_base   = 2;
        mf.route_cost_ring4  = 1;
        mf.route_cost_ring8  = 3;
        mf.route_cost_ring16 = 5;
        set_move_fixes(mf);
        flood_world w(16);
        corridor_cells(w);
        w.run(4, 0);
        ck_eq(w.field(0, 1), (0x100u + (2u << 8)) | 29u, "T6: custom table: cell 1 = 0x100 + (2<<8) (low 1 costs base 2)");
        ck_eq(w.field(0, 2), (0x300u + (11u << 8)) | 1u, "T6: custom table: cell 2 = + (11<<8) (low 29 costs 2+1+3+5)");
        ck_eq(w.field(0, 3), (0xe00u + (2u << 8)) | 29u, "T6: custom table: cell 3");
        ck_eq(w.field(0, 4), (0x1000u + (11u << 8)) | 1u, "T6: custom table: cell 4");
        ck(mv::planner_hit_count(mv::PH_FLOOD_C) > 0, "T6: the twin counted route_cost hits");
    }
    // wave_full: a lone unit pays ring*1/4 of the ring part: 29 -> 2 + 9/4 = 4
    {
        move_fixes mf;
        mf.route_cost_mode     = 1;
        mf.route_cost_base     = 2;
        mf.route_cost_ring4    = 1;
        mf.route_cost_ring8    = 3;
        mf.route_cost_ring16   = 5;
        mf.route_gap_wave_full = 4;
        set_move_fixes(mf);
        flood_world w(16);
        corridor_cells(w);
        w.fx.group_member_count = 1;
        w.run(4, 0);
        ck_eq(w.field(0, 2), (0x300u + (4u << 8)) | 1u, "T6: wave_full 4, a wave of 1: low 29 costs 2 + 9/4 = 4");
        flood_world w4(16);
        corridor_cells(w4);
        w4.fx.group_member_count = 4;
        w4.run(4, 0);
        ck_eq(w4.field(0, 2), (0x300u + (11u << 8)) | 1u, "T6: ... a wave of 4 pays the full ring (11)");
    }
    // out-of-range knob values are clamped like the setters do
    {
        move_fixes mf;
        mf.route_cost_mode   = 3;
        mf.route_cost_base   = 0;
        mf.route_cost_ring4  = 4;
        mf.route_cost_ring8  = 8;
        mf.route_cost_ring16 = 16;
        set_move_fixes(mf);
        flood_world w(16);
        corridor_cells(w);
        w.run(4, 0);
        ck_eq(w.field(0, 1), (0x100u + (1u << 8)) | 29u, "T6: route_cost_base 0 is clamped to 1 (any nonzero mode = the table)");
    }
}

void t7_flood_guard() {
    fixes_scope scope;
    // A front that laps the 4096-slot ring. Random per-cell costs make the FIFO label-correcting flood re-queue cells many times;
    // with the guard off the retail loop runs to the end (or overwrites live entries), with the guard on it stops at 0xff8 pending.
    uint32_t             seed = 12345;
    auto                 rnd  = [&seed]() { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
    std::vector<uint8_t> low(65536);
    for (auto &b : low) b = (uint8_t)(1 + (rnd() % 60));
    auto build = [&](flood_world &w) {
        for (int x = 0; x < 256; ++x)
            for (int y = 0; y < 256; ++y) w.cell(x, y, low[(size_t)((x << 8) | y)]);
    };
    uint32_t off_sig = 0, on_sig = 0;
    {
        set_move_fixes(move_fixes{});
        flood_world w(256);
        build(w);
        w.run(255 * 256 + 255, 128 * 256 + 128);
        for (size_t i = 0; i < w.fx.region_grid.size(); ++i) off_sig = off_sig * 31u + (w.fx.region_grid[i].terrain_flags >> 8);
    }
    mv::planner_hits_reset();
    {
        move_fixes mf;
        mf.flood_queue_guard = true;
        set_move_fixes(mf);
        flood_world w(256);
        build(w);
        w.run(255 * 256 + 255, 128 * 256 + 128);
        for (size_t i = 0; i < w.fx.region_grid.size(); ++i) on_sig = on_sig * 31u + (w.fx.region_grid[i].terrain_flags >> 8);
    }
    ck(mv::planner_hit_count(mv::PH_FLOOD_Q) > 0, "T7: the guard twin counted its checks");
    // The guard changes the result exactly when the front laps the ring. Report which case this fixture is in, and pin it.
    if (trace_on()) printf("  T7: guard off sig=%08x on sig=%08x\n", off_sig, on_sig);
    ck(off_sig != on_sig, "T7: a front that laps the 4096-slot ring is cut by the guard (the field differs from the lapping retail loop)");
}

// ============================================================================================================================
// T8 -- the pathfind centroid
// ============================================================================================================================
int32_t pf_find_closer(uint8_t, uint8_t, uint32_t *, uint32_t *, int32_t, int32_t) { return 1; } // "failed": the planner bails out
void    pf_free_slot(uint16_t, int32_t) {}
int32_t pf_alloc_slot(int32_t, int32_t) { return 0; }
int32_t pf_walk_valid(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t *, uint32_t *) { return 1; }
int32_t pf_tile_dist(int32_t, int32_t, int32_t, int32_t) { return 0; }
int32_t pf_build_steps(int32_t, int32_t, int32_t, uint32_t, int32_t) { return 0; }
void    pf_plan_formation(uint8_t) {}
void    pf_hook(int32_t, int32_t) {}
int32_t pf_find_route(uint32_t, uint32_t) { return 0; }
void    pf_mark() {}
int32_t pf_flood(int32_t, int32_t) { return 1; }
int32_t pf_route_search(uint16_t, int16_t, uint8_t *) { return 0; }
void    pf_guard() {}
int32_t pf_nearest(uint8_t *, uint8_t *) { return 0; }
int32_t pf_plan_route(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t *, uint32_t *) { return 1; }
int32_t pf_trace(uint8_t, uint8_t, uint8_t, uint8_t, void *) { return 0; }
void    pf_step_record(int32_t, uint32_t, int32_t, int32_t) {}
int32_t pf_queue_count(int32_t, int32_t) { return 0; }

const group_move_order_pathfind_calls g_pf_calls = {
    &pf_find_closer,
    &pf_free_slot,
    &pf_alloc_slot,
    &pf_walk_valid,
    &pf_tile_dist,
    &pf_build_steps,
    &pf_plan_formation,
    &pf_hook,
    &pf_find_route,
    &pf_mark,
    &pf_flood,
    &pf_route_search,
    &pf_guard,
    &pf_nearest,
    &pf_plan_route,
    &pf_trace,
    &pf_step_record,
    &pf_queue_count,
};

// Two members on a 200 x 120 map, member 0 at (c0, r0), member 1 at (c1, r1); the goal tile is impassable so the planner leaves after
// step 2 (find_closer_visible_tile "fails"), having written the group centre. Returns {centroid_x, centroid_y}.
void run_centroid(int c0, int r0, int c1, int r1, int32_t *cx, int32_t *cy) {
    sim_fixture fx;
    fx.reset();
    fx.map_width  = 200;
    fx.map_height = 120;
    std::fill(fx.passable.begin(), fx.passable.end(), (uint8_t)1);
    fx.path_wrap_mask                     = 255;
    fx.group_member_count                 = 2;
    fx.group_order_owner                  = 1;
    fx.group_members[0].cur_col           = (uint8_t)c0;
    fx.group_members[0].cur_row           = (uint8_t)r0;
    fx.group_members[0].unit_handle       = 4;
    fx.group_members[1].cur_col           = (uint8_t)c1;
    fx.group_members[1].cur_row           = (uint8_t)r1;
    fx.group_members[1].unit_handle       = 5;
    fx.group_order_goal_x                 = 77;
    fx.group_order_goal_y                 = 66;
    fx.passable[(size_t)((77 << 8) | 66)] = 0; // goal impassable -> step 2 bails out
    sim_store own                         = fx.store();
    sim_view  v                           = fx.view();
    detail::group_move_order_pathfind(v, own, g_pf_calls, 1, 1);
    *cx = fx.group_centroid_x;
    *cy = fx.group_centroid_y;
}

void t8_pathfind_centroid() {
    fixes_scope scope;
    int32_t     cx = -1, cy = -1;
    // x: member 0 at col 10, member 1 at col 190: delta 180 > w/2 = 100 (the seam-straddling case)
    set_move_fixes(move_fixes{});
    mv::planner_hits_reset();
    run_centroid(10, 5, 190, 5, &cx, &cy);
    ck_eq((uint32_t)cx, 240u, "T8: key off: retail `0x80 - d` = -52 -> centroid col (-26 + 10) & 255 = 240 (the mirror, off-centre)");
    ck_eq(mv::planner_hit_count(mv::PH_CENT_X), 0u, "T8: key off counts no hit");
    move_fixes mf;
    mf.centroid_wrap_fix = true;
    set_move_fixes(mf);
    run_centroid(10, 5, 190, 5, &cx, &cy);
    ck_eq((uint32_t)cx, 0u, "T8: key on: `d - width` = -20 -> centroid col (-10 + 10) & 255 = 0 (the true middle across the seam)");
    ck_eq(mv::planner_hit_count(mv::PH_CENT_X), 1u, "T8: key on counts the x hit");
    // y: rows 5 and 115 on a 120-high map: delta 110 > 60
    set_move_fixes(move_fixes{});
    run_centroid(10, 5, 10, 115, &cx, &cy);
    ck_eq((uint32_t)cy, 14u, "T8: key off, y axis: retail -> row (18/2 + 5) = 14");
    set_move_fixes(mf);
    run_centroid(10, 5, 10, 115, &cx, &cy);
    ck_eq((uint32_t)cy, 0u, "T8: key on, y axis: row (-10/2 + 5) = 0");
    ck_eq(mv::planner_hit_count(mv::PH_CENT_Y), 1u, "T8: key on counts the y hit");
    // a delta inside the half extent is untouched either way
    run_centroid(10, 5, 50, 5, &cx, &cy);
    ck_eq((uint32_t)cx, 30u, "T8: key on, a delta within w/2: the plain mean (30)");
    set_move_fixes(move_fixes{});
    run_centroid(10, 5, 50, 5, &cx, &cy);
    ck_eq((uint32_t)cx, 30u, "T8: ... the same with the key off");
}

// ============================================================================================================================
// T9 -- the detour occupant guard
// ============================================================================================================================
int     g_splice_calls = 0;
int32_t mock_splice(int32_t, int32_t, int32_t slot, int32_t) {
    ++g_splice_calls;
    (void)slot;
    return 0;
}
const unit_path_detour_calls g_detour_calls = {&mock_splice};

// Unit 5 (player 0) has a path at slot 0 but every candidate tile around it is a wall, so the search reaches the recursion into the
// occupant `alt` (unit 9). The occupant has a free candidate (its own tile, heading deltas 0): the recursion would splice and return 0.
// Returns the detour result; g_splice_calls says whether the recursion ran.
int32_t run_detour(uint8_t occupant_slot) {
    sim_fixture fx;
    fx.reset();
    fx.map_width = 256;
    std::fill(fx.passable.begin(), fx.passable.end(), (uint8_t)0);
    for (auto &t : fx.tile_objects) t.building = 0;
    unit &u5                                                                 = fx.units[(size_t)(0 * UNITS_PER_PLAYER + 5)];
    u5.x                                                                     = 10;
    u5.y                                                                     = 10;
    u5.path_slot_id                                                          = 0;
    u5.path_cursor                                                           = 0;
    fx.path_buffers[0].heading                                               = 4;
    unit &u9                                                                 = fx.units[(size_t)(0 * UNITS_PER_PLAYER + 9)];
    u9.x                                                                     = 40;
    u9.y                                                                     = 40;
    u9.path_slot_id                                                          = occupant_slot;
    u9.path_cursor                                                           = 0;
    fx.path_buffers[(size_t)occupant_slot * PATH_WAYPOINTS_PER_SLOT].heading = 4;
    fx.passable[(size_t)((40 << 8) | 40)]                                    = 1; // the occupant's own tile is the one free candidate (heading deltas are 0)
    g_splice_calls                                                           = 0;
    sim_store own                                                            = fx.store();
    sim_view  v                                                              = fx.view();
    return detail::unit_path_detour(v, own, g_detour_calls, 0, 5, 9);
}

void t9_detour_guard() {
    fixes_scope scope;
    set_move_fixes(move_fixes{});
    mv::planner_hits_reset();
    ck_eq((uint32_t)run_detour(3), 0u, "T9: key off, occupant with a real slot: the recursion runs and finds a detour (0)");
    ck_eq((uint32_t)g_splice_calls, 1u, "T9: ... one splice");
    ck_eq((uint32_t)run_detour(0xff), 0u, "T9: key off, occupant slot 0xff: the retail recursion reads past the slots and still runs (0)");
    ck_eq((uint32_t)g_splice_calls, 1u, "T9: ... it spliced");
    move_fixes mf;
    mf.detour_slot_guard = true;
    set_move_fixes(mf);
    ck_eq((uint32_t)run_detour(3), 0u, "T9: key on, occupant with a real slot: the recursion still runs (0)");
    ck_eq((uint32_t)g_splice_calls, 1u, "T9: ... one splice");
    ck_eq((uint32_t)run_detour(0xff), 1u, "T9: key on, occupant slot 0xff: no recursion, 'no detour' (1)");
    ck_eq((uint32_t)g_splice_calls, 0u, "T9: ... nothing spliced");
    ck_eq(mv::planner_hit_count(mv::PH_DETOUR), 2u, "T9: the twin counted its two guarded decisions");
}

} // namespace

// ============================================================================================================================
// T4d -- key 3 ranks tiles by PATH distance from the centre (0.2.1 hotfix, rc21 "formation in tight places leads to detours")
//   The geometric ring put a tile one step away across a thin wall ahead of tiles on the click's own side. The fake world's flood reaches both sides of a wall
//   (as the live one does), so only the path ranking keeps the targets on the click's side.
// ============================================================================================================================
void t4d_path_ranking() {
    using fw = fake_formation;
    // ---- a thin vertical wall between a compact group and a click that is NOT next to the group's side ---------------------------------
    {
        fc3_row r;
        r.name = "thin wall";
        for (int j = 0; j < 4; ++j)
            for (int i = 0; i < 4; ++i) r.start.push_back({20 + i, 20 + j});
        for (int y = 14; y <= 30; ++y) r.walls.push_back({29, y}); // one tile thick, 17 tall: the far side is ~9 steps away round the end
        r.click = {28, 22};                                        // one tile west of the wall
        fw w;
        fc3_setup(w, r);
        const bool         handled = mv::formation_after_pathfind(w, 5, 1, 1, FC3_RULE);
        int                far = 0, onwall = 0;
        std::set<uint32_t> seen;
        bool               dup = false;
        for (int m = 0; m < 16; ++m) {
            const uint32_t t = w.tile_of_member(m);
            if ((t >> 8) > 29) ++far;
            if ((t >> 8) == 29) ++onwall;
            if (!seen.insert(t).second) dup = true;
        }
        ck(handled && w.last_kept >= 0, "T4d thin wall: handled");
        ck(far == 0, "T4d thin wall: no member's target is on the far side of the wall");
        ck(onwall == 0 && !dup, "T4d thin wall: nobody on the wall, no shared tile");
        ck(w.last_rk >= 2 && w.last_rk <= 3, "T4d thin wall: rk is a path-distance band (2..3 for 16 tiles on the click's side)");
    }
    // ---- a DIAGONAL wall: the flank rule keeps the BFS from slipping between two touching wall tiles -------------------------------------
    {
        fc3_row r;
        r.name = "diagonal wall";
        for (int j = 0; j < 4; ++j)
            for (int i = 0; i < 4; ++i) r.start.push_back({34 + i, 14 + j}); // on the click's side: y < x - 10
        for (int x = 20; x <= 45; ++x) r.walls.push_back({x, x - 10});
        r.click = {30, 19}; // the shifted 4x4 block (29..32, 17..20) holds the wall tiles (29,19) and (30,20) and the across tile (29,20), one diagonal step from the click
        fw w;
        fc3_setup(w, r);
        ck(mv::formation_after_pathfind(w, 5, 1, 1, FC3_RULE), "T4d diagonal wall: handled");
        int far = 0;
        for (int m = 0; m < 16; ++m) {
            const int x = (int)(w.tile_of_member(m) >> 8), y = (int)(w.tile_of_member(m) & 0xff);
            if (y >= x - 10) ++far; // on or across the wall line
        }
        ck(far == 0, "T4d diagonal wall: every target is on the click's side of the diagonal (no slip between two touching wall tiles)");
    }
    // ---- GATHER into a pocket: walls on three sides, open to the south; free ground (behind the west wall) is geometrically nearer than the pocket's far end ------
    {
        fc3_row r;
        r.name                     = "pocket";
        static const int SC[16][2] = {{5, 2}, {6, 10}, {0, 1}, {8, 1}, {5, 9}, {0, 8}, {3, 0}, {1, 6}, {6, 1}, {3, 1}, {8, 6}, {0, 9}, {1, 3}, {10, 10}, {9, 0}, {9, 9}};
        for (int k = 0; k < 16; ++k) r.start.push_back({44 + SC[k][0], 58 + SC[k][1]}); // a spread group in the open hall south of the pocket
        for (int y = 48; y <= 54; ++y) r.walls.push_back({56, y});                      // west wall of the pocket
        for (int x = 56; x <= 60; ++x) r.walls.push_back({x, 48});                      // north wall
        for (int y = 48; y <= 60; ++y) r.walls.push_back({60, y});                      // east wall
        r.click = {58, 50};
        fw w;
        fc3_setup(w, r);
        ck(mv::formation_after_pathfind(w, 5, 1, 1, FC3_RULE), "T4d pocket: handled");
        int                out = 0;
        std::set<uint32_t> seen;
        bool               dup = false;
        for (int m = 0; m < 16; ++m) {
            const uint32_t t = w.tile_of_member(m);
            const int      x = (int)(t >> 8), y = (int)(t & 0xff);
            if (x < 57 || x > 59 || y < 49 || y > 54) ++out; // inside the pocket (3 wide, 6 deep = 18 tiles)
            if (!seen.insert(t).second) dup = true;
        }
        ck(out == 0 && !dup, "T4d pocket: GATHER fills the pocket (16 of its 18 tiles), nobody is sent behind a wall");
    }
    // ---- open ground is untouched: D == the ring, so the order is the geometric one (T4c's rows pin the full assignments) -------------------
    {
        fc3_row r;
        r.name = "open";
        for (int j = 0; j < 4; ++j)
            for (int i = 0; i < 4; ++i) r.start.push_back({20 + i, 20 + j});
        r.click = {22, 22};
        fw w;
        fc3_setup(w, r);
        ck(mv::formation_after_pathfind(w, 5, 1, 1, FC3_RULE) && w.last_rk == 2 && w.last_kept == 16, "T4d open ground: rk is the ring (2) and the group stays");
    }
}

void run_move_proto_planner_tests() {
    t1_route_cost();
    t2_corridor();
    t3_wrap_guards();
    t4_arrival();
    t4b_formation();
    t4c_move_as_one();
    t4d_path_ranking();
    t5_route_through();
    t5b_route_through_moving();
    t5c_route_through_replanning();
    t6_flood_twin();
    t7_flood_guard();
    t8_pathfind_centroid();
    t9_detour_guard();
}

} // namespace mh::sim::test
