//
// sim_move_proto_selftest_walker.cpp -- mods:M1H lane C: `simtest` cases for the walker / marshal movement-prototype twins
// (sim/move/{blocked,nudge,near_enough,group_speed}.h and their call sites in sim_unit_state_move_walker.cpp /
// sim_unit_state_group_marshal.cpp). The retail splices (mh/seams/move_proto_*.cpp) run the SAME header code, so what is proven here
// over array fakes is what the splice computes over live memory; the call sites are proven through the real walker body below and
// through the marshal cases in sim_unit_state_group_marshal_selftest.cpp.
//
//   A  blocked_decide / make_blocked_params     (3b wait limit, 3c keep sidestep, 3d cooldown)       KEY OFF = retail, ON = the decision
//   B  near_enough_check                          (2b + the on-goal stop)
//   C  group_speed: the IEEE divide on awkward ratios vs an SSE2 divsd reference, the tag, the walk keep test, the wave rule
//   D  nd_decide / mk_decide over a fake world    (3e-1 nudge, 3e-2 claim, 3e-3 swap, 3e-4 marshal sites) + the negative cases
//   E  the real walker body with the keys on      (wiring: blocked limit + cooldown, swap exchange, tagged scale kept)
//
#include "sim/sim_unit_state_move_walker.h"

#include <emmintrin.h>
#include <float.h>

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <utility>
#include <vector>

#include "sim/move/blocked.h"
#include "sim/move/group_speed.h"
#include "sim/move/near_enough.h"
#include "sim/move/nudge.h"
#include "sim/move/respond.h"
#include "sim/move/world_c.h"
#include "sim/sim_move_fixes.h"
#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using mh::game::mh_cfg_final_struct_Unit;
using mh::game::mh_llm_strat_order;
using mh::game::mh_llm_strat_path_waypoint;
using mh::game::mh_map_object_unit;
using mh::game::mh_map_tile_object_data;

// The game's _G_LLM_MAP_DIR_STEP_DELTAS, read from /eng/mh.exe 0x0051de40 (28 x {dx, dy}, int8): the eight principal headings only.
const uint8_t k_dir_table[56] = {0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x01, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00,
                                 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x01, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,
                                 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

// restores the key switchboard on scope exit
struct fixes_guard {
    mh::sim::move_fixes saved = mh::sim::move_fix();
    ~fixes_guard() { mh::sim::set_move_fixes(saved); }
};

// ---- a fake world for the shared headers (one player, 16 units, a 256x256 plane) -------------------------------------------
struct fake_world {
    static constexpr int                    NU = 16, NS = 16;
    std::vector<mh_map_object_unit>         units{NU};
    mh_cfg_final_struct_Unit                cfg_one{};
    std::vector<mh_map_tile_object_data>    tiles{256 * 256};
    std::vector<uint8_t>                    passable = std::vector<uint8_t>(256 * 256);
    std::vector<mh_llm_strat_path_waypoint> paths{NS * 300};
    std::vector<const char *>               log;
    std::vector<const char *>               notes;
    uint16_t                                cur       = 1;
    double                                  clock_val = 1000.0;
    int                                     hits_nd = 0, hits_mk = 0, hits_eq = 0, hits_dh = 0;
    int                                     fow_rm = 0, fow_up = 0;

    fake_world() {
        std::memset(units.data(), 0, units.size() * sizeof(mh_map_object_unit));
        std::memset(tiles.data(), 0, tiles.size() * sizeof(mh_map_tile_object_data));
        std::memset(paths.data(), 0, paths.size() * sizeof(mh_llm_strat_path_waypoint));
        std::memset(&cfg_one, 0, sizeof(cfg_one));
        std::memset(passable.data(), 1, passable.size());
        cfg_one.move_op_code    = 0x0f;
        cfg_one.default_op_code = 1;
        cfg_one.soldier_count   = 0;
        cfg_one.sight           = 3;
        for (auto &u : units) u.path_slot_id = 0xff;
    }
    static size_t ti(uint32_t x, uint32_t y) { return ((x & 0xff) << 8) | (y & 0xff); }

    // a unit standing on a tile (player 0): the tile belongs to it, passable is 0 under it
    mh_map_object_unit &put(int idx, uint32_t x, uint32_t y, uint16_t state) {
        auto &u                     = units[idx];
        u.x                         = static_cast<uint8_t>(x);
        u.y                         = static_cast<uint8_t>(y);
        u.state                     = state;
        u.order                     = state == 0x0f ? 0x0f : 1;
        u.energy                    = 10.0;
        u.move_microstep            = 0x1f;
        u.origin_tile_was_passable  = 1;
        u.goal_x                    = static_cast<uint8_t>(x);
        u.goal_y                    = static_cast<uint8_t>(y);
        tiles[ti(x, y)].building    = static_cast<uint16_t>(idx);
        tiles[ti(x, y)].class_owner = 0x80;
        passable[ti(x, y)]          = 0;
        return u;
    }
    void set_path(int slot, std::initializer_list<std::pair<int, int>> legs) {
        int i = 0;
        for (auto &l : legs) {
            paths[slot * 300 + i].heading    = static_cast<uint8_t>(l.first);
            paths[slot * 300 + i].run_length = static_cast<uint8_t>(l.second);
            ++i;
        }
        paths[slot * 300 + i].heading    = 0;
        paths[slot * 300 + i].run_length = 0;
    }
    void give_path(int idx, int slot, std::initializer_list<std::pair<int, int>> legs) {
        set_path(slot, legs);
        units[idx].path_slot_id = static_cast<uint8_t>(slot);
        units[idx].path_cursor  = 0;
    }

    // ---- the world interface ----
    mh_map_object_unit             *unit(uint32_t player, int32_t idx) { return (player == 0 && idx >= 0 && idx < NU) ? &units[idx] : nullptr; }
    const mh_cfg_final_struct_Unit *cfg(const mh_map_object_unit &) { return &cfg_one; }
    mh_map_tile_object_data        &tile(uint32_t x, uint32_t y) { return tiles[ti(x, y)]; }
    uint8_t                        &pass(uint32_t x, uint32_t y) { return passable[ti(x, y)]; }
    mh_llm_strat_path_waypoint     *path(uint32_t, uint32_t slot) { return &paths[slot * 300]; }
    void                            dir_delta(int h, int *dx, int *dy) {
        *dx = static_cast<int8_t>(k_dir_table[h * 2]);
        *dy = static_cast<int8_t>(k_dir_table[h * 2 + 1]);
    }
    uint32_t                             mask_x() { return 0xff; }
    uint32_t                             mask_y() { return 0xff; }
    const void                          *game_clock() { return &clock_val; }
    mh::game::mh_llm_strat_crew_soldier *soldier(uint32_t, uint32_t) { return nullptr; }
    uint16_t                             cur_index() { return cur; }
    int32_t                              map_w() { return 256; }
    int32_t                              map_h() { return 256; }

    void    fow_remove_sight(uint32_t, int32_t, int32_t, uint8_t) { ++fow_rm; }
    void    fow_update(uint32_t, uint32_t, uint32_t, uint8_t) { ++fow_up; }
    void    soldiers_set_heading(uint16_t, int32_t, uint8_t) {}
    int32_t make_single_step(uint32_t, int32_t idx) {
        auto     &u  = units[idx];
        const int dx = static_cast<int>(u.goal_x) - u.x, dy = static_cast<int>(u.goal_y) - u.y;
        if (dx < -1 || dx > 1 || dy < -1 || dy > 1 || (!dx && !dy)) return 0;
        int h = 0;
        for (int k = 1; k <= 22; k += 3)
            if (static_cast<int8_t>(k_dir_table[k * 2]) == dx && static_cast<int8_t>(k_dir_table[k * 2 + 1]) == dy) h = k;
        const int slot = 8 + idx % 8; // a free slot of its own
        set_path(slot, {{h, 1}});
        u.path_slot_id = static_cast<uint8_t>(slot);
        u.path_cursor  = 0;
        log.push_back("make_single_step");
        return 1;
    }
    void free_slot(uint16_t, int32_t idx) { units[idx].path_slot_id = 0xff; }
    void set_order_param(int32_t, int32_t, int16_t) { log.push_back("set_order_param"); }
    void set_state_of(int32_t, int32_t idx, int16_t st) {
        units[idx].state = static_cast<uint16_t>(st);
        log.push_back("set_state_of");
    }
    void    notify_status(uint32_t, int32_t, uint32_t) { log.push_back("notify_status"); }
    void    note(const char *what, uint32_t, int32_t, int32_t, uint32_t, uint32_t, uint32_t, uint32_t, int) { notes.push_back(what); }
    void    hit_nd() { ++hits_nd; }
    void    hit_eq() { ++hits_eq; }
    void    hit_dh() { ++hits_dh; }
    int32_t unit_limit() { return NU; }
    void    hit_mk() { ++hits_mk; }
    void    hit_mk2() {}
    bool    noted(const char *what) const {
        for (auto *n : notes)
            if (std::strcmp(n, what) == 0) return true;
        return false;
    }
};

// =====================================================================================================
// A -- the blocked-unit protocol
// =====================================================================================================
void test_blocked() {
    using namespace mh::sim::mvc;
    // params: the clamps and the retail defaults
    ck(!make_blocked_params(0, false, 0).active(), "A1: all three keys at their retail values -> inactive (the retail block runs)");
    ck_eq(static_cast<uint32_t>(make_blocked_params(1500, false, 0).wait_limit), 30u, "A2: 1500 ms -> limit 30 (ms / 50)");
    ck_eq(static_cast<uint32_t>(make_blocked_params(1, false, 0).wait_limit), 1u, "A3: 1 ms -> limit clamped up to 1");
    ck_eq(static_cast<uint32_t>(make_blocked_params(60000, false, 0).wait_limit), 201u, "A4: 60000 ms -> clamped to 10050 ms -> limit 201");
    ck_eq(static_cast<uint32_t>(make_blocked_params(-5, false, 0).wait_limit), 201u, "A5: negative -> retail");
    ck(make_blocked_params(0, false, 500).cd_on && make_blocked_params(0, false, 500).cd_s == 0.5, "A6: cooldown 500 ms -> 0.5 s");
    ck(make_blocked_params(0, false, 99999).cd_s == 10.0, "A7: cooldown clamped to 10000 ms");
    ck(!make_blocked_params(0, false, -3).cd_on, "A8: negative cooldown -> off");

    auto never = [](uint32_t, int) -> mh_llm_strat_path_waypoint & {
        static mh_llm_strat_path_waypoint w;
        return w;
    };
    // the wait budget (no detour): limit 2 -> wait, wait, replan; retail limit 201 -> keeps waiting
    {
        mh_map_object_unit u{};
        const auto         p = make_blocked_params(100, false, 0);
        ck(blocked_decide(u, false, p, never) == blocked_next::wait && u.path_blocked_retry_count == 1, "A9: retry 0 < 2 -> wait, retry 1");
        ck(blocked_decide(u, false, p, never) == blocked_next::wait && u.path_blocked_retry_count == 2, "A10: retry 1 < 2 -> wait, retry 2");
        ck(blocked_decide(u, false, p, never) == blocked_next::replan && u.path_blocked_retry_count == 2, "A11: retry 2 >= 2 -> replan (the caller zeroes it)");
        mh_map_object_unit r{};
        r.path_blocked_retry_count = 200;
        const auto retail          = make_blocked_params(0, true, 0); // keep on, wait at retail 201
        ck(blocked_decide(r, false, retail, never) == blocked_next::wait && r.path_blocked_retry_count == 201, "A12: retail limit: retry 200 -> wait, 201");
        ck(blocked_decide(r, false, retail, never) == blocked_next::replan, "A13: retry 201 -> replan (old <= 0xc8 fails)");
    }
    // a detour without the keep key replans at once
    {
        mh_map_object_unit u{};
        ck(blocked_decide(u, true, make_blocked_params(100, false, 0), never) == blocked_next::replan, "A14: detour requested, keep off -> replan");
    }
    // keep sidestep
    {
        std::vector<mh_llm_strat_path_waypoint> pb(100 * 300);
        auto                                    wp    = [&](uint32_t slot, int e) -> mh_llm_strat_path_waypoint                                       &{ return pb[slot * 300 + e]; };
        const auto                              keep  = make_blocked_params(0, true, 0);
        auto                                    fresh = [&](uint8_t leg, uint8_t h) {
            std::memset(pb.data(), 0xAA, pb.size() * sizeof(mh_llm_strat_path_waypoint));
            mh_map_object_unit u{};
            u.order          = 0x0f;
            u.path_slot_id   = 3;
            u.path_cursor    = 4;
            u.facing_target  = leg;
            wp(3, 4).heading = h;
            return u;
        };
        auto u = fresh(1, 7); // cardinal leg, a sidestep (7) was spliced in
        ck(blocked_decide(u, true, keep, wp) == blocked_next::done, "A15: kept sidestep -> done");
        ck(wp(3, 5).heading == 0 && wp(3, 5).run_length == 0 && wp(3, 6).heading == 0xAA, "A16: cardinal: the path ends after the probe step only (entry cur+1)");
        ck_eq(u.path_blocked_retry_count, 0x40u, "A17: a keep adds 0x40 to the retry byte");
        u = fresh(4, 7); // diagonal leg
        ck(blocked_decide(u, true, keep, wp) == blocked_next::done && wp(3, 6).heading == 0 && wp(3, 5).heading == 0xAA, "A18: diagonal: the path ends after both steps (entry cur+2)");
        u                          = fresh(1, 7);
        u.path_blocked_retry_count = 0xc0;
        ck(blocked_decide(u, true, keep, wp) == blocked_next::replan, "A19: retry >= 0xc0: the keep is refused");
        u                          = fresh(1, 7);
        u.path_blocked_retry_count = 0x80;
        ck(blocked_decide(u, true, keep, wp) == blocked_next::done && u.path_blocked_retry_count == 0xc0, "A20: 0x80 + 0x40 = 0xc0, the next keep is refused");
        for (uint16_t o : {0x1a, 0x1b, 0x1c, 0x10, 0x33}) {
            u       = fresh(1, 7);
            u.order = o;
            ck(blocked_decide(u, true, keep, wp) == blocked_next::replan, "A21: attack / patrol / merge orders keep the retail replan");
        }
        u              = fresh(1, 7);
        u.path_slot_id = 100;
        ck(blocked_decide(u, true, keep, wp) == blocked_next::replan, "A22: slot >= 100 -> replan");
        u = fresh(1, 0);
        ck(blocked_decide(u, true, keep, wp) == blocked_next::replan, "A23: heading 0 at the cursor (nothing spliced) -> replan");
        u = fresh(1, 1);
        ck(blocked_decide(u, true, keep, wp) == blocked_next::replan, "A24: heading == facing (the occupant got the sidestep) -> replan");
        u             = fresh(1, 7);
        u.path_cursor = 298;
        ck(blocked_decide(u, true, keep, wp) == blocked_next::replan, "A25: cursor + 2 >= 300 -> replan");
    }
}

// =====================================================================================================
// A3 -- mods:M1BE: move_enter_queue (blocked.h exemption + nd_decide wait) and storage_door_hold (dh_entering_near), sim/move/enter_queue.h
// =====================================================================================================
void test_enter_queue() {
    using namespace mh::sim::mvc;
    auto never = [](uint32_t, int) -> mh_llm_strat_path_waypoint & {
        static mh_llm_strat_path_waypoint w;
        return w;
    };
    // blocked.h: ORDER 0x24 is exempt from the wait limit, the kept sidestep and the cooldown; every other order is not
    {
        const auto         p = make_blocked_params(100, true, 500, false, 0, true); // limit 2, keep, cooldown, enter_queue ON
        mh_map_object_unit e{}, o{};
        e.order = 0x24;
        o.order = 1;
        ck(blocked_enter_exempt(p, e) && !blocked_enter_exempt(p, o), "G1: ORDER 0x24 is exempt, another order is not");
        ck(!blocked_enter_exempt(make_blocked_params(100, true, 500, false, 0, false), e), "G2: key off: nobody is exempt");
        const bool w1 = blocked_decide(e, false, p, never) == blocked_next::wait;
        const bool w2 = blocked_decide(e, false, p, never) == blocked_next::wait;
        const bool w3 = blocked_decide(e, false, p, never) == blocked_next::wait;
        ck(w1 && w2 && w3 && e.path_blocked_retry_count == 3, "G3: an entering unit keeps waiting past the 2-retry budget (the retail limit is 201)");
        const bool o1 = blocked_decide(o, false, p, never) == blocked_next::wait;
        const bool o2 = blocked_decide(o, false, p, never) == blocked_next::wait;
        const bool o3 = blocked_decide(o, false, p, never) == blocked_next::replan;
        ck(o1 && o2 && o3, "G4: another unit replans after the 2-retry budget");
        mh_map_object_unit d{};
        d.order = 0x24;
        ck(blocked_decide(d, true, p, never) == blocked_next::replan, "G5: a detour request on an entering unit is the retail replan (no kept sidestep)");
        ck(p.active() == make_blocked_params(100, true, 500, false, 0, false).active(), "G6: enter_queue alone does not change active()");
        ck(!make_blocked_params(0, false, 0, false, 0, true).active(), "G7: enter_queue with every other key at retail: the retail block runs");
    }
    // nd_decide: wait behind an entering friend of the same storage; anything else falls through
    {
        fake_world w;
        auto      &a = w.put(1, 10, 10, 0x0f);
        auto      &b = w.put(2, 11, 10, 0x25);
        a.order = b.order   = 0x24;
        a.home_storage_slot = b.home_storage_slot = 3;
        const nudge_keys on{false, false, false, true}, off{};
        ck_eq(static_cast<uint32_t>(nd_decide(w, on, 10, 10, 11, 10)), 0u, "G8: both ORDER 0x24, same storage: wait (0)");
        ck_eq(static_cast<uint32_t>(w.hits_eq), 1u, "G8b: the hit is counted");
        ck(nd_decide(w, off, 10, 10, 11, 10) == -1, "G9: key off: the retail check runs");
        b.home_storage_slot = 4;
        ck(nd_decide(w, on, 10, 10, 11, 10) == -1, "G10: another storage: the retail check runs");
        b.home_storage_slot = 3;
        b.order             = 1;
        ck(nd_decide(w, on, 10, 10, 11, 10) == -1, "G11: the blocker is not entering: the retail check runs");
        b.order = 0x24;
        a.order = 1;
        ck(nd_decide(w, on, 10, 10, 11, 10) == -1, "G12: the walker is not entering: the retail check runs");
        a.order  = 0x24;
        b.energy = 0.0;
        ck(nd_decide(w, on, 10, 10, 11, 10) == -1, "G13: a dead blocker: the retail check runs");
        b.energy                   = 10.0;
        w.tile(11, 10).class_owner = 0x81; // another player's unit
        ck(nd_decide(w, on, 10, 10, 11, 10) == -1, "G14: another player's unit: the retail check runs");
    }
    // dh_entering_near: an entering unit of this storage, on the map, within 2 tiles of the door tile
    {
        fake_world w;
        auto      &a        = w.put(1, 20, 20, 0x0f);
        a.order             = 0x24;
        a.home_storage_slot = 5;
        ck(dh_entering_near(w, 0, 5, 22, 22), "G15: Chebyshev 2 from the door: hold");
        ck(!dh_entering_near(w, 0, 5, 23, 20), "G16: 3 tiles away: no hold");
        ck(!dh_entering_near(w, 0, 6, 22, 22), "G17: another storage: no hold");
        a.order = 0x29;
        ck(!dh_entering_near(w, 0, 5, 22, 22), "G18: another order: no hold");
        a.order                 = 0x24;
        w.tile(20, 20).building = 9; // not on the map any more (docked / walking in)
        ck(!dh_entering_near(w, 0, 5, 22, 22), "G19: a unit that is off the tile map does not hold the door");
        w.tile(20, 20).building = 1;
        a.energy                = 0.0;
        ck(!dh_entering_near(w, 0, 5, 22, 22), "G20: a dead unit does not hold the door");
        a.energy = 10.0;
        ck(!dh_entering_near(w, 0, 5, 1, 21), "G21: the door 19 columns away: no hold");
        auto &c             = w.put(2, 255, 20, 0x0f); // wraps: x 255 is 1 tile from x 0
        c.order             = 0x24;
        c.home_storage_slot = 5;
        ck(dh_entering_near(w, 0, 5, 1, 20), "G22: the torus wrap: x 255 is 2 tiles from x 1");
    }
}

// =====================================================================================================
// A2 -- mods:M1DT fix 4: move_block_reset_moving (4a) + the replan tag and the marshal's detour cap (4b), sim/move/replan_cap.h
// =====================================================================================================
void test_replan_cap() {
    using namespace mh::sim::mvc;
    ck(!make_blocked_params(0, false, 0, false, 0).active(), "M1DT-1: reset off, cap 0 -> inactive (the retail block runs)");
    ck(make_blocked_params(0, false, 0, true, 0).active() && make_blocked_params(0, false, 0, false, 150).active(), "M1DT-2: either key makes the decision path active");
    ck_eq(static_cast<uint32_t>(make_blocked_params(0, false, 0, false, 5000).detour_cap), 1000u, "M1DT-3: the cap clamps to 1000 %");
    ck_eq(static_cast<uint32_t>(make_blocked_params(0, false, 0, false, -4).detour_cap), 0u, "M1DT-4: a negative cap is off");

    auto never = [](uint32_t, int) -> mh_llm_strat_path_waypoint & {
        static mh_llm_strat_path_waypoint w;
        return w;
    };
    // (a) the wait budget restarts when the blocker CHANGES to a walking friend
    {
        mh_map_object_unit u{};
        const auto         p = make_blocked_params(100, false, 0, true, 0); // limit 2
        const blocker_info b5{5, true}, b6{6, true}, wall{9, false};
        ck(blocked_decide(u, false, p, never, b5) == blocked_next::wait && u.path_blocked_retry_count == 1 && rc_last_blocker(u) == 5, "M1DT-5: first block by unit 5: wait, remembered");
        ck(blocked_decide(u, false, p, never, b5) == blocked_next::wait && u.path_blocked_retry_count == 2, "M1DT-6: same blocker: the count runs on");
        ck(blocked_decide(u, false, p, never, b5) == blocked_next::replan, "M1DT-7: ... and the budget is used up (replan)");
        ck(blocked_decide(u, false, p, never, b6) == blocked_next::wait && u.path_blocked_retry_count == 1 && rc_last_blocker(u) == 6,
           "M1DT-8: the blocker changed to another walking friend: the count restarts (wait, retry 1)");
        mh_map_object_unit w{};
        w.path_blocked_retry_count = 2;
        ck(blocked_decide(w, false, p, never, wall) == blocked_next::replan && w.path_blocked_retry_count == 2, "M1DT-9: a changed blocker that is not a walking friend never restarts the count");
        mh_map_object_unit n{};
        n.path_blocked_retry_count = 2;
        ck(blocked_decide(n, false, p, never, blocker_info{}) == blocked_next::replan, "M1DT-10: no unit on the tile (a wall): retail count, replan");
        mh_map_object_unit off{};
        off.path_blocked_retry_count = 2;
        ck(blocked_decide(off, false, make_blocked_params(100, false, 0, false, 0), never, b6) == blocked_next::replan && rc_last_blocker(off) == 0,
           "M1DT-11: key off: the blocker is ignored and nothing is remembered");
    }
    // (b) the replan is tagged; the marshal check
    {
        std::vector<mh_llm_strat_path_waypoint> pb(100 * 300);
        auto                                    wp    = [&](uint32_t slot, int e) -> mh_llm_strat_path_waypoint                                       &{ return pb[slot * 300 + e]; };
        auto                                    route = [&](uint32_t slot, int steps) { // one run of `steps` steps in slot
            std::memset(pb.data(), 0, pb.size() * sizeof(mh_llm_strat_path_waypoint));
            wp(slot, 0).heading    = 1;
            wp(slot, 0).run_length = static_cast<uint8_t>(steps);
        };
        const auto         p = make_blocked_params(100, false, 0, false, 150);
        mh_map_object_unit u{};
        u.order                    = 0x0f;
        u.goal_x                   = 40;
        u.goal_y                   = 30;
        u.x                        = 30;
        u.y                        = 30;
        u.path_slot_id             = 3;
        u.path_blocked_retry_count = 2;
        ck(blocked_decide(u, false, p, never) == blocked_next::replan && rc_tag_valid(u) && u._pad_0xca[0] == 1, "M1DT-12: a block-wait replan is tagged (cap on)");
        mh_map_object_unit a       = u;
        a.order                    = 0x1a; // attack: the path end settles instead of replanning
        a._pad_0xca[0]             = 0;
        a.path_blocked_retry_count = 2;
        ck(blocked_decide(a, false, p, never) == blocked_next::replan && !rc_tag_valid(a), "M1DT-13: attack / patrol / merge orders are never tagged");
        mh_map_object_unit c       = u;
        c._pad_0xca[0]             = 0;
        c.path_blocked_retry_count = 2;
        ck(blocked_decide(c, false, make_blocked_params(100, false, 0, false, 0), never) == blocked_next::replan && c._pad_0xca[0] == 0, "M1DT-14: cap 0: no tag");

        // direct distance 10; cap 150 % -> budget 15 + 2 slack = 17 steps
        route(3, 17);
        mh_map_object_unit t = u;
        ck(!rc_should_refuse(t, 150, 0xff, 0xff, wp) && t._pad_0xca[0] == 0, "M1DT-15: a route of 17 steps (<= 1.5 x 10 + 2) is taken and the tag cleared");
        route(3, 18);
        t = u;
        ck(rc_should_refuse(t, 150, 0xff, 0xff, wp) && t._pad_0xca[0] == 2, "M1DT-16: 18 steps is refused, the tag counts one refusal");
        ck(rc_should_refuse(t, 150, 0xff, 0xff, wp) && t._pad_0xca[0] == 3 && rc_should_refuse(t, 150, 0xff, 0xff, wp) && t._pad_0xca[0] == 4, "M1DT-17: refused 3 times in all");
        ck(!rc_should_refuse(t, 150, 0xff, 0xff, wp) && t._pad_0xca[0] == 0, "M1DT-18: the 4th time the long route is taken (bounded), tag cleared");
        t = u;
        ck(!rc_should_refuse(t, 0, 0xff, 0xff, wp) && t._pad_0xca[0] == 1, "M1DT-19: cap 0 never refuses");
        t        = u;
        t.goal_x = 41;
        ck(!rc_should_refuse(t, 150, 0xff, 0xff, wp), "M1DT-20: a different goal (a fresh order) invalidates the tag");
        t       = u;
        t.order = 0x0e;
        ck(!rc_should_refuse(t, 150, 0xff, 0xff, wp), "M1DT-21: a different order invalidates the tag");
        t              = u;
        t._pad_0xca[0] = 0;
        ck(!rc_should_refuse(t, 150, 0xff, 0xff, wp), "M1DT-22: an untagged (fresh) route is never refused, however long");
        // wrap: x 250 -> 4 is 10 tiles through the edge, not 246
        t              = u;
        t.x            = 250;
        t.goal_x       = 4;
        t.goal_y       = 30;
        t._pad_0xca[1] = 4;
        route(3, 17);
        ck(!rc_should_refuse(t, 150, 0xff, 0xff, wp), "M1DT-23: the straight distance wraps through the map edge");
        route(3, 18);
        t._pad_0xca[0] = 1;
        ck(rc_should_refuse(t, 150, 0xff, 0xff, wp), "M1DT-24: ... and the refusal measures against it");
        t              = u;
        t.path_slot_id = 255;
        ck(!rc_should_refuse(t, 150, 0xff, 0xff, wp), "M1DT-25: no path slot: nothing to judge");
    }
}

// =====================================================================================================
// B -- near enough
// =====================================================================================================
void test_near_enough() {
    using namespace mh::sim::mvc;
    fake_world w;
    // on its goal: stops with either key; a non-move order never
    auto &a = w.put(1, 5, 5, 0x0a);
    a.order = 0x0f;
    ck(near_enough_check(w, 0, 1, 0) == ne_stop::on_goal, "B1: standing on its goal -> stop (no near_enough needed)");
    a.order = 0x1a;
    ck(near_enough_check(w, 0, 1, 5) == ne_stop::no, "B2: an attack order never stops here");
    a.order = 1;
    ck(near_enough_check(w, 0, 1, 0) == ne_stop::on_goal, "B3: order 1 (the replan form of a move) counts as a plain move");
    // near a goal a friend holds
    a.goal_x                                  = 7;
    a.goal_y                                  = 5;
    auto &f                                   = w.put(2, 7, 5, 1);
    f.energy                                  = 10.0;
    w.tiles[fake_world::ti(7, 5)].class_owner = 0x80; // player 0
    ck(near_enough_check(w, 0, 1, 0) == ne_stop::no, "B4: R = 0 and not on the goal -> no");
    ck(near_enough_check(w, 0, 1, 1) == ne_stop::near_goal, "B5: distance 2 <= R(1) + isqrt(1) -> stop");
    a.goal_x                                  = 9;
    w.tiles[fake_world::ti(7, 5)].building    = 0;
    w.tiles[fake_world::ti(9, 5)].building    = 2;
    w.tiles[fake_world::ti(9, 5)].class_owner = 0x80;
    w.passable[fake_world::ti(9, 5)]          = 0;
    f.x                                       = 9;
    ck(near_enough_check(w, 0, 1, 1) == ne_stop::no, "B6: distance 4 > 1 + 1 -> no");
    ck(near_enough_check(w, 0, 1, 3) == ne_stop::near_goal, "B7: distance 4 <= 3 + 1 -> stop");
    f.state = 0x0f;
    ck(near_enough_check(w, 0, 1, 3) == ne_stop::no, "B8: the occupant is walking -> no");
    f.state = 0x0c;
    ck(near_enough_check(w, 0, 1, 3) == ne_stop::no, "B9: the occupant is regrouping (0xc) -> no");
    f.state = 0x0a;
    ck(near_enough_check(w, 0, 1, 3) == ne_stop::no, "B10: the occupant is in the marshal (0xa) -> no");
    f.state                                   = 1;
    w.tiles[fake_world::ti(9, 5)].class_owner = 0x81;
    ck(near_enough_check(w, 0, 1, 3) == ne_stop::no, "B11: the tile belongs to another player -> no");
    w.tiles[fake_world::ti(9, 5)].class_owner = 0x80;
    w.passable[fake_world::ti(9, 5)]          = 1;
    ck(near_enough_check(w, 0, 1, 3) == ne_stop::no, "B12: the goal tile is passable (not held) -> no");
    w.passable[fake_world::ti(9, 5)] = 0;
    // the group size widens the radius: 9 live units of the group -> isqrt = 3
    a.move_group_id = 77;
    for (int i = 3; i <= 10; ++i) {
        auto &g         = w.units[i];
        g.move_group_id = 77;
        g.energy        = 1.0;
    }
    a.energy = 1.0;
    ck(near_enough_check(w, 0, 1, 1) == ne_stop::near_goal, "B13: 9 live members -> isqrt 3: distance 4 <= 1 + 3 -> stop");
    w.units[3].energy = 0.0;
    w.units[4].energy = 0.0;
    ck(near_enough_check(w, 0, 1, 1) == ne_stop::no, "B14: dead members do not count: 7 live -> isqrt 2: 4 > 3 -> no");
}

// =====================================================================================================
// C -- group speed
// =====================================================================================================
struct speed_fake {
    std::vector<mh_map_object_unit>       us{16};
    std::vector<mh_cfg_final_struct_Unit> cs{8};
    std::vector<int32_t>                  scratch;
    std::vector<mh_llm_strat_order>       q;
    int                                   logs = 0;
    speed_fake() {
        std::memset(us.data(), 0, us.size() * sizeof(mh_map_object_unit));
        std::memset(cs.data(), 0, cs.size() * sizeof(mh_cfg_final_struct_Unit));
        for (auto &u : us) {
            u.energy                = 5.0;
            u.move_step_speed_scale = 1.0;
        }
    }
    int32_t                         units() { return 16; }
    mh_map_object_unit             &unit(int32_t i) { return us[i]; }
    const mh_cfg_final_struct_Unit &cfg(uint32_t p) { return cs[p]; }
    int32_t                         scratch_unit(int32_t w) { return scratch[w]; }
    int32_t                         queue_count() { return static_cast<int32_t>(q.size()); }
    const mh_llm_strat_order       &queue(int32_t k) { return q[k]; }
    void                            log_wave(int32_t, int32_t, double, double) { ++logs; }
};

double sse2_div(double a, double b) { return _mm_cvtsd_f64(_mm_div_sd(_mm_set_sd(a), _mm_set_sd(b))); }

void test_group_speed() {
    using namespace mh::sim::mvc;
    // the divide: PC=53 is the guarantee both DLLs rely on (libmh installs it; mh.dll's CRT runs there)
    unsigned cw = 0, saved_cw = 0;
    _controlfp_s(&saved_cw, 0, 0);
    _controlfp_s(&cw, _PC_53, _MCW_PC); // what libmh installs (mh::fp::install_pc53); the precondition for x87 FDIV == SSE2 divsd

    // awkward ratios: repeating binary fractions, near-ties, subnormal-adjacent magnitudes of a speed, and an LCG sweep
    const double awk[][2] = {{0.1, 0.3}, {1.0, 3.0}, {2.0, 3.0}, {0.7, 0.9}, {1.0 / 3.0, 0.1}, {0.29999999999999999, 0.1}, {5.0, 7.0}, {1e-3, 3e-3}, {123456.789, 0.001}, {0.5, 0.49999999999999994}, {1.0, 0.9999999999999999}, {4.0, 2.0}, {3.0, 2.0}, {2.9999999999999996, 1.0000000000000002}, {17.0, 13.0}, {0.0625, 0.1}};
    int          bad      = 0;
    for (const auto &p : awk) {
        const double q = gs_ratio(p[0], p[1]);
        const double r = gs_tagged(sse2_div(p[0], p[1]));
        if (gs_bits(q) != gs_bits(r)) ++bad;
    }
    uint64_t s = 0x9E3779B97F4A7C15ull;
    for (int i = 0; i < 20000; ++i) {
        s              = s * 6364136223846793005ull + 1442695040888963407ull;
        const double a = 0.25 + static_cast<double>((s >> 11) & 0xFFFFF) / 1048576.0 * 63.0;
        s              = s * 6364136223846793005ull + 1442695040888963407ull;
        const double b = 0.25 + static_cast<double>((s >> 11) & 0xFFFFF) / 1048576.0 * 63.0;
        if (gs_bits(gs_ratio(a, b)) != gs_bits(gs_tagged(sse2_div(a, b)))) ++bad;
    }
    ck_eq(static_cast<uint32_t>(bad), 0u, "C1: gs_ratio == tagged(SSE2 divsd) bit for bit on 16 awkward ratios + 20000 sweep pairs");
    ck(gs_is_tagged(gs_ratio(3.0, 2.0)) && !gs_is_tagged(1.0) && !gs_is_tagged(0.0), "C2: the tag: set on a ratio, absent on 1.0 and 0.0");
    ck(gs_ratio(3.0, 2.0) > 1.4999999 && gs_ratio(3.0, 2.0) < 1.5000001, "C3: tagging moves the value by less than 1e-15 relative");

    // the walk keep test == the splice's integer test on the double's two dwords
    auto splice_keep = [](double d) {
        uint32_t lo, hi;
        std::memcpy(&lo, &d, 4);
        std::memcpy(&hi, reinterpret_cast<const char *>(&d) + 4, 4);
        return (lo & 0xF) == 0xB && static_cast<int32_t>(hi) >= 0x3FF00000;
    };
    int keep_bad = 0;
    for (double v : {1.0, 1.5, gs_tagged(1.5), gs_tagged(1.0), gs_tagged(0.9), gs_tagged(-2.0), gs_tagged(2.5), gs_tagged(1e300), 0.0, -0.0})
        if (gs_keep_scale(v) != splice_keep(v)) ++keep_bad;
    ck_eq(static_cast<uint32_t>(keep_bad), 0u, "C4: gs_keep_scale == the splice's dword test (tagged and high dword >= 0x3FF00000 as a signed int)");
    ck(gs_keep_scale(gs_tagged(1.0)) && !gs_keep_scale(gs_tagged(0.9)) && !gs_keep_scale(1.5), "C5: tagged >= 1.0 kept; tagged < 1.0 and untagged are not");

    // the wave rule
    {
        speed_fake f;
        f.cs[0].step_speed[0] = 2.0;
        f.cs[1].step_speed[0] = 4.0;
        f.us[1].unit_proto_id = 0;
        f.us[2].unit_proto_id = 1;
        f.scratch             = {1, 2};
        const auto r          = group_speed_apply(f, 0, 2, 1, 0);
        ck(r.mixed && r.slowest == 4.0 && r.fastest == 2.0 && r.ground == 2, "C6: mixed wave: slowest 4 fastest 2");
        ck(f.us[1].move_step_speed_scale == gs_ratio(4.0, 2.0) && gs_is_tagged(f.us[1].move_step_speed_scale), "C7: the faster member gets tagged slowest/own");
        ck(f.us[2].move_step_speed_scale == 1.0, "C8: the slowest member is untouched");
        ck_eq(static_cast<uint32_t>(f.logs), 1u, "C9: one wave log line");
        // the next order: a same-speed wave clears the tag; an untagged scale > 1 stays
        f.us[2].unit_proto_id         = 0;
        f.us[2].move_step_speed_scale = 1.25; // retail value (above 1 via the unit-0 tile): untagged, never written
        const auto r2                 = group_speed_apply(f, 0, 2, 1, 0);
        ck(!r2.mixed && f.us[1].move_step_speed_scale == 1.0, "C10: a same-speed wave resets a tagged scale to 1.0");
        ck(f.us[2].move_step_speed_scale == 1.25, "C11: an untagged scale above 1.0 is never written");
        // an attack wave (is_move 0) is never mixed and clears the tag
        f.us[2].unit_proto_id         = 1;
        f.us[1].move_step_speed_scale = gs_tagged(2.0);
        const auto r3                 = group_speed_apply(f, 0, 2, 0, 0);
        ck(!r3.mixed && f.us[1].move_step_speed_scale == 1.0, "C12: is_move == 0 -> not mixed, tag cleared");
    }
    // the order-wide slowest: a unit of the same move_group_id outside the wave, and a queued record
    {
        speed_fake f;
        f.cs[0].step_speed[0] = 2.0;
        f.cs[1].step_speed[0] = 3.0;
        f.cs[2].step_speed[0] = 8.0;
        f.cs[3].step_speed[0] = 16.0;
        f.us[1].unit_proto_id = 0;
        f.us[2].unit_proto_id = 1;
        f.us[5].unit_proto_id = 2;
        f.us[5].move_group_id = 9;
        f.us[6].unit_proto_id = 3;
        f.us[6].move_group_id = 0; // not yet marshalled: only its queue record names the order
        f.scratch             = {1, 2};
        auto r                = group_speed_apply(f, 0, 2, 1, 9);
        ck(r.slowest == 8.0, "C13: a live ground unit of the same move_group_id outside the wave counts (slowest 8)");
        mh_llm_strat_order rec{};
        rec.owner_and_kind = 0x80;
        rec.unit_index     = 6;
        rec.args[12]       = 9;
        f.q.push_back(rec);
        r = group_speed_apply(f, 0, 2, 1, 9);
        ck(r.slowest == 16.0, "C14: a queued record of the order counts (slowest 16)");
        f.us[6].energy = 0.0;
        r              = group_speed_apply(f, 0, 2, 1, 9);
        ck(r.slowest == 8.0, "C15: a dead unit's queued record does not");
        f.cs[2].type          = 0xf; // a flier is skipped everywhere
        f.us[2].unit_proto_id = 1;
        r                     = group_speed_apply(f, 0, 2, 1, 0);
        ck(r.slowest == 3.0 && r.ground == 2, "C16: wave only (mgid 0): slowest 3");
    }
    _controlfp_s(&cw, saved_cw & _MCW_PC, _MCW_PC);
}

// =====================================================================================================
// D -- nudge / claim / swap, walker and marshal sites
// =====================================================================================================
void test_nudge() {
    using namespace mh::sim::mvc;
    const nudge_keys nudge{true, false, false}, swap{false, false, true}, claim{false, true, false}, none{};
    // A (idx 1) walks south (heading 1) from (5,5) into (5,6)
    auto setup = [](fake_world &w, uint16_t b_state) {
        auto &a = w.put(1, 5, 5, 0x0f);
        w.give_path(1, 1, {{1, 3}});
        a.facing_target = 1;
        auto &b         = w.put(2, 5, 6, b_state);
        b.facing_target = 7;
        return &b;
    };
    {
        fake_world w;
        setup(w, 1);
        ck_eq(static_cast<uint32_t>(nd_decide(w, none, 5, 5, 5, 6) + 1), 0u, "D1: every key off -> -1 (retail), nothing counted");
        ck_eq(static_cast<uint32_t>(w.hits_nd), 0u, "D1b: no hit counted with the keys off");
    }
    { // 3e-1: an idle friend with a free perpendicular tile is nudged
        fake_world w;
        auto      *b = setup(w, 1);
        ck_eq(static_cast<uint32_t>(nd_decide(w, nudge, 5, 5, 5, 6)), 0u, "D2: nudge -> 0 (wait for the occupant)");
        ck(b->state == 0x36 && b->goal_x == 4 && b->goal_y == 6 && b->order == 1, "D3: the blocker is sent to (4,6) through state 0x36, order 1");
        ck(w.noted("nudge") && w.hits_nd == 1, "D4: the nudge is noted and counted");
        ck(w.units[1].x == 5 && w.units[1].y == 5, "D5: the walker did not move");
    }
    { // a sleeping blocker is woken: clock pulled back to the game clock
        fake_world w;
        auto      *b      = setup(w, 1);
        b->activity_clock = 5000.0;
        nd_decide(w, nudge, 5, 5, 5, 6);
        ck(b->activity_clock == 1000.0, "D6: a clock ahead of the game clock is pulled back");
        fake_world w2;
        auto      *b2      = setup(w2, 1);
        b2->activity_clock = 500.0;
        nd_decide(w2, nudge, 5, 5, 5, 6);
        ck(b2->activity_clock == 500.0, "D7: a clock behind the game clock is left alone");
    }
    { // already asked: wait without a second nudge
        fake_world w;
        auto      *b = setup(w, 0x36);
        b->goal_x    = 3;
        ck_eq(static_cast<uint32_t>(nd_decide(w, nudge, 5, 5, 5, 6)), 0u, "D8: blocker in state 0x36 -> wait");
        ck(b->goal_x == 3, "D8b: its goal is not rewritten");
    }
    { // 1-tile corridor: only straight ahead is free -> TRADE
        fake_world w;
        auto      *b = setup(w, 1);
        for (uint32_t x = 4; x <= 6; ++x)
            for (uint32_t y = 5; y <= 7; ++y)
                if (!(x == 5 && y == 7) && !(x == 5 && (y == 5 || y == 6))) w.passable[fake_world::ti(x, y)] = 0;
        const uint8_t oa = 3, ob = 4;
        w.units[1].origin_tile_was_passable = oa;
        b->origin_tile_was_passable         = ob;
        ck_eq(static_cast<uint32_t>(nd_decide(w, nudge, 5, 5, 5, 6)), 0u, "D9: corridor -> trade, returns 0");
        ck(w.units[1].x == 5 && w.units[1].y == 6 && b->x == 5 && b->y == 5, "D10: positions traded");
        ck(w.tiles[fake_world::ti(5, 6)].building == 1 && w.tiles[fake_world::ti(5, 5)].building == 2, "D11: tile_objects traded");
        ck(w.passable[fake_world::ti(5, 6)] == 5 && w.passable[fake_world::ti(5, 5)] == 5, "D12: both tiles held by passable 5 (a unit moving in)");
        ck(w.units[1].origin_tile_was_passable == ob && b->origin_tile_was_passable == oa, "D13: origin_tile_was_passable handed over crosswise");
        ck(w.paths[1 * 300].run_length == 2, "D14: A's current leg shrank by one like a normal step");
        ck(b->state == 0x0c && b->order == 1 && b->path_cursor == 0 && b->facing_target == 13, "D15: B is handed to the walker (0xc), facing the tile it slides to");
        ck(w.units[1].move_microstep == 0 && b->move_microstep == 0 && w.fow_rm == 2 && w.fow_up == 2, "D16: microsteps reset, fog moved for both");
        ck(w.noted("swap-trade"), "D17: noted as swap-trade");
        // the same corridor with A not settled: the push stays the fallback
        fake_world w2;
        auto      *b2 = setup(w2, 1);
        for (uint32_t x = 4; x <= 6; ++x)
            for (uint32_t y = 5; y <= 7; ++y)
                if (!(x == 5 && y == 7) && !(x == 5 && (y == 5 || y == 6))) w2.passable[fake_world::ti(x, y)] = 0;
        w2.units[1].move_microstep = 0x10;
        nd_decide(w2, nudge, 5, 5, 5, 6);
        ck(b2->state == 0x36 && b2->goal_x == 5 && b2->goal_y == 7, "D18: A mid-step -> no trade, the idle friend is pushed straight ahead");
    }
    { // 3e-3: head-on swap of two walking friends
        fake_world w;
        setup(w, 0x0f);
        w.give_path(2, 2, {{13, 3}});
        w.units[2].facing_target = 13;
        ck_eq(static_cast<uint32_t>(nd_decide(w, swap, 5, 5, 5, 6)), 0u, "D19: head-on -> swap, 0");
        ck(w.units[1].x == 5 && w.units[1].y == 6 && w.units[2].x == 5 && w.units[2].y == 5, "D20: positions swapped");
        ck(w.paths[1 * 300].run_length == 2 && w.paths[2 * 300].run_length == 2, "D21: both current legs shrank by one");
        ck(w.noted("swap"), "D22: noted as swap");
        fake_world w2;
        setup(w2, 0x0f);
        w2.give_path(2, 2, {{13, 3}});
        w2.units[2].facing_target = 10; // still turning toward us
        ck_eq(static_cast<uint32_t>(nd_decide(w2, swap, 5, 5, 5, 6)), 0u, "D23: the other unit still turns -> wait (0)");
        ck(w2.units[1].x == 5 && w2.units[1].y == 5, "D24: nothing moved");
        fake_world w3;
        setup(w3, 0x0f);
        w3.give_path(2, 2, {{13, 3}});
        w3.units[2].facing_target = 13;
        ck_eq(static_cast<uint32_t>(nd_decide(w3, nudge, 5, 5, 5, 6) + 1), 0u, "D25: nudge alone does not swap walking friends -> retail");
    }
    { // 3e-2: claim a turning leader
        fake_world w;
        setup(w, 0x0f);
        w.give_path(2, 2, {{7, 2}});
        w.units[2].facing_target = 4; // must turn to 7
        ck_eq(static_cast<uint32_t>(nd_decide(w, claim, 5, 5, 5, 6)), 0u, "D26: claim -> 0");
        const auto *pb = &w.paths[2 * 300];
        ck(pb[0].heading == 1 && pb[0].run_length == 1 && pb[1].heading == 7 && pb[1].run_length == 2 && pb[2].heading == 0,
           "D27: the leader's path gets one leg in front (back onto the follower's old tile), the rest shifted");
        ck(w.units[2].x == 5 && w.units[2].y == 5 && w.units[2].facing_target == 13 && w.units[2].facing_current == 13, "D28: the leader slides back, facing it");
        ck(w.noted("claim"), "D29: noted as claim");
        fake_world w2; // the follower's leg has only one step left: no claim
        setup(w2, 0x0f);
        w2.give_path(1, 1, {{1, 1}});
        w2.give_path(2, 2, {{7, 2}});
        w2.units[2].facing_target = 4;
        ck_eq(static_cast<uint32_t>(nd_decide(w2, claim, 5, 5, 5, 6) + 1), 0u, "D30: the follower's leg has < 2 steps -> retail");
    }
    { // negatives
        fake_world w;
        auto      *b                              = setup(w, 1);
        w.tiles[fake_world::ti(5, 6)].class_owner = 0x81; // another player
        ck_eq(static_cast<uint32_t>(nd_decide(w, nudge, 5, 5, 5, 6) + 1), 0u, "D31: another player's unit -> retail");
        w.tiles[fake_world::ti(5, 6)].class_owner = 0x80;
        b->energy                                 = 0.0;
        ck_eq(static_cast<uint32_t>(nd_decide(w, nudge, 5, 5, 5, 6) + 1), 0u, "D32: a dying blocker -> retail");
        b->energy                = 10.0;
        w.units[1].facing_target = 4; // A is not facing its step
        ck_eq(static_cast<uint32_t>(nd_decide(w, nudge, 5, 5, 5, 6) + 1), 0u, "D33: A still turning -> retail");
        w.units[1].facing_target                  = 1;
        w.tiles[fake_world::ti(5, 6)].class_owner = 0; // a building / terrain tile
        ck_eq(static_cast<uint32_t>(nd_decide(w, nudge, 5, 5, 5, 6) + 1), 0u, "D34: not a unit tile -> retail");
    }
    // ---- the marshal sites ----
    auto marshal = [](fake_world &w) {
        auto &a         = w.put(1, 5, 5, 0x0a);
        a.order         = 0x0f;
        a.goal_x        = 5;
        a.goal_y        = 9;
        a.path_slot_id  = 0xff;
        a.facing_target = 1;
        auto &b         = w.put(2, 5, 6, 1);
        b.facing_target = 7;
        return &b;
    };
    {
        fake_world w;
        auto      *b = marshal(w);
        ck(!mk_decide(w, none, 0, 1), "D35: marshal site, keys off -> retail");
        ck(!mk_decide(w, nudge, 0, 1), "D36: an idle friend with a free side: nudged, the member retries (false)");
        ck(b->state == 0x36 && b->goal_x == 4 && b->goal_y == 6, "D37: the friend is sent to (4,6)");
        ck(w.noted("nudge-marshal"), "D38: noted as nudge-marshal");
    }
    {
        fake_world w;
        auto      *b = marshal(w);
        for (uint32_t x = 4; x <= 6; ++x)
            for (uint32_t y = 5; y <= 7; ++y)
                if (!(x == 5 && y == 7) && !(x == 5 && (y == 5 || y == 6))) w.passable[fake_world::ti(x, y)] = 0;
        ck(mk_decide(w, nudge, 0, 1), "D39: a corridor: the member and the idle friend trade tiles (true = skip the retail bump)");
        ck(w.units[1].x == 5 && w.units[1].y == 6 && b->x == 5 && b->y == 5, "D40: positions traded");
        ck(w.units[1].state == 0x0f && b->state == 0x0c && b->order == 1, "D41: the ticking member walks (0xf), the friend is handed over (0xc), order 1");
        ck(w.units[1].goal_y == 9 && w.noted("swap-marshal"), "D42: the member keeps its goal; noted as swap-marshal");
        // not stuck (has a path) -> retail
        fake_world w2;
        marshal(w2);
        w2.units[1].path_slot_id = 4;
        ck(!mk_decide(w2, nudge, 0, 1), "D43: a member with a path is not the marshal site's business");
    }
    { // head-on with a walking friend (swap only)
        fake_world w;
        auto      *b = marshal(w);
        b->state     = 0x0f;
        w.give_path(2, 2, {{13, 3}});
        b->facing_target = 13;
        ck(!mk_decide(w, nudge, 0, 1), "D44: nudge alone does not swap with a walking friend");
        ck(mk_decide(w, swap, 0, 1), "D45: swap: the walking head-on friend trades tiles");
        ck(w.units[1].x == 5 && w.units[1].y == 6 && b->x == 5 && b->y == 5 && w.paths[2 * 300].run_length == 2, "D46: traded, the friend's leg shrank");
    }
}

// =====================================================================================================
// E -- the real walker body with the keys on
// =====================================================================================================
int      g_walker_set_state = 0, g_walker_set_state_arg = 0, g_walker_notify = 0, g_walker_fow_rm = 0, g_walker_fow_up = 0, g_walker_check = 0;
double   w_dir_step_factor(int32_t) { return 1.0; }
int32_t  w_target_class(uint32_t, int32_t) { return 0; }
uint32_t w_in_range(int32_t, int32_t, int32_t, int32_t, int32_t) { return 0; }
int32_t  w_goal_in_range(int32_t, int32_t, int32_t, int32_t, int32_t) { return 0; }
void     w_fire() {}
void     w_set_state_order(uint16_t, uint16_t) {}
void     w_set_state(uint16_t s) {
    ++g_walker_set_state;
    g_walker_set_state_arg = s;
}
void w_notify(uint32_t, int32_t, uint32_t) { ++g_walker_notify; }
void w_release(uint32_t, int32_t, uint32_t) {}
void w_move_auto(uint16_t, int32_t, uint32_t, uint32_t) {}
void w_get_coords(uint16_t, int32_t, int32_t *, int32_t *) {}
void w_bldg_coords(uint16_t, int32_t, int32_t *, int32_t *) {}
void w_footprint(uint32_t, uint32_t, int32_t, int32_t, uint32_t *, uint32_t *) {}
void w_facing24(uint32_t h, int32_t *dx, int32_t *dy) {
    *dx = static_cast<int8_t>(k_dir_table[h * 2]);
    *dy = static_cast<int8_t>(k_dir_table[h * 2 + 1]);
}
int32_t w_check(uint32_t, uint32_t, int32_t, int32_t) {
    ++g_walker_check;
    return 0;
}
void    w_fow_rm(uint32_t, int32_t, int32_t, uint8_t) { ++g_walker_fow_rm; }
void    w_fow_up(uint32_t, uint32_t, uint32_t, uint8_t) { ++g_walker_fow_up; }
void    w_soldier(uint16_t, int32_t, uint8_t) {}
int32_t w_walk_allowed(uint32_t, int32_t) { return 0; }
int32_t w_single(uint32_t, int32_t) { return 0; }
void    w_free(uint16_t, int32_t) {}
void    w_order_param(int32_t, int32_t, int16_t) {}
void    w_state_of(int32_t, int32_t, int16_t) {}

const unit_state_move_walker_calls g_wcalls = {
    &w_dir_step_factor,
    &w_target_class,
    &w_in_range,
    &w_goal_in_range,
    &w_fire,
    &w_set_state_order,
    &w_set_state,
    &w_notify,
    &w_release,
    &w_move_auto,
    &w_get_coords,
    &w_bldg_coords,
    &w_footprint,
    &w_facing24,
    &w_check,
    &w_fow_rm,
    &w_fow_up,
    &w_soldier,
    &w_walk_allowed,
    &w_single,
    &w_free,
    &w_order_param,
    &w_state_of,
};

void wreset(sim_fixture &fx) {
    fx.reset();
    std::memcpy(fx.map_dir_step_deltas.data(), k_dir_table, sizeof(k_dir_table));
    g_walker_set_state = g_walker_set_state_arg = g_walker_notify = g_walker_fow_rm = g_walker_fow_up = g_walker_check = 0;
}

// a settled walker at (x, y) whose next step (heading 1, south) is (x, y+1)
unit &place_walker(sim_fixture &fx, uint32_t idx, uint32_t x, uint32_t y, int slot) {
    unit &u                                                                             = fx.u(0, idx);
    u.unit_proto_id                                                                     = 5;
    u.state                                                                             = 0x0f;
    u.order                                                                             = 0x0f;
    u.x                                                                                 = static_cast<uint8_t>(x);
    u.y                                                                                 = static_cast<uint8_t>(y);
    u.move_microstep                                                                    = 0x1f;
    u.facing_target                                                                     = 1;
    u.facing_current                                                                    = 1;
    u.move_step_speed_scale                                                             = 1.0;
    u.energy                                                                            = 10.0;
    u.path_slot_id                                                                      = static_cast<uint8_t>(slot);
    u.path_cursor                                                                       = 0;
    u.origin_tile_was_passable                                                          = 1;
    fx.path_buffers[0 * PATH_WAYPOINTS_PER_PLAYER + slot * PATH_WAYPOINTS_PER_SLOT + 0] = {1, 5};
    fx.path_buffers[0 * PATH_WAYPOINTS_PER_PLAYER + slot * PATH_WAYPOINTS_PER_SLOT + 1] = {0, 0};
    fx.t(x, y).building                                                                 = static_cast<uint16_t>(idx);
    fx.t(x, y).class_owner                                                              = 0x80;
    fx.passable[(x << 8) | y]                                                           = 0;
    return u;
}

void walk(sim_fixture &fx, uint32_t idx) {
    unit &u            = fx.u(0, idx);
    fx.cur_unit_ptr    = &u;
    fx.view_cur_player = 0;
    fx.view_cur_index  = static_cast<uint16_t>(idx);
    fx.tick_budget     = 100.0;
    sim_store own      = fx.store();
    detail::unit_state_move_walker(fx.view(), own, g_wcalls);
}

void test_walker_body(sim_fixture &fx) {
    fixes_guard guard;
    // E1/E2: the wait budget. Blocked by a parked unit of ANOTHER player (retail: detour request; here step_check says 0 = wait).
    auto blocked_setup = [&]() {
        wreset(fx);
        unit &u                       = place_walker(fx, 1, 10, 10, 0);
        fx.passable[(10u << 8) | 11u] = 0; // the next tile is held
        u.activity_clock              = 100.0;
        return &u;
    };
    {
        mh::sim::set_move_fixes({});
        unit *u = blocked_setup();
        for (int i = 0; i < 3; ++i) walk(fx, 1);
        ck_eq(u->path_blocked_retry_count, 3u, "E1: keys off: three blocked ticks -> retry 3, no replan (retail wait budget 201)");
        ck(g_walker_set_state == 0 && g_walker_check == 3, "E1b: no state change, the retail step_check ran every tick");
        double want_clock = 100.0;
        for (int i = 0; i < 3; ++i) want_clock += 0.05; // the same three additions, in order
        ck_eq_d(u->activity_clock, want_clock, "E1c: +0.05 s per wait");
    }
    {
        mh::sim::move_fixes mf;
        mf.block_wait_ms = 100; // limit 2
        mh::sim::set_move_fixes(mf);
        unit *u = blocked_setup();
        walk(fx, 1);
        walk(fx, 1);
        ck(g_walker_set_state == 0 && u->path_blocked_retry_count == 2, "E2: block_wait_ms=100: two waits");
        walk(fx, 1);
        ck(g_walker_set_state == 1 && g_walker_set_state_arg == 0x0a && g_walker_notify == 1 && u->path_blocked_retry_count == 0,
           "E3: the third blocked tick replans: state 0xa, notify, retry 0");
        ck_eq_d(fx.tick_budget, 0.0, "E4: the tick budget is zeroed");
    }
    {
        mh::sim::move_fixes mf;
        mf.block_wait_ms      = 50; // limit 1
        mf.repath_cooldown_ms = 500;
        mh::sim::set_move_fixes(mf);
        unit *u = blocked_setup();
        walk(fx, 1);
        const double before = u->activity_clock;
        walk(fx, 1);
        ck_eq_d(u->activity_clock, before + 0.5, "E5: the replan advances the activity clock by the cooldown (and not by the wait bump)");
    }
    // E6: the swap exchange through the walker body
    {
        wreset(fx);
        mh::sim::move_fixes mf;
        mf.swap_headon = true;
        mh::sim::set_move_fixes(mf);
        fx.cfg_units[5].move_op_code                                                     = 0x0f;
        fx.cfg_units[5].default_op_code                                                  = 1;
        fx.cfg_units[5].sight                                                            = 2;
        unit &a                                                                          = place_walker(fx, 1, 10, 10, 0);
        unit &b                                                                          = place_walker(fx, 2, 10, 11, 1);
        b.facing_target                                                                  = 13;
        fx.path_buffers[0 * PATH_WAYPOINTS_PER_PLAYER + 1 * PATH_WAYPOINTS_PER_SLOT + 0] = {13, 5};
        a.activity_clock                                                                 = 1.0;
        walk(fx, 1);
        ck(a.x == 10 && a.y == 11 && b.x == 10 && b.y == 10, "E6: head-on walkers traded tiles through the real walker body");
        ck(fx.t(10, 11).building == 1 && fx.t(10, 10).building == 2 && fx.passable[(10u << 8) | 11u] == 5 && fx.passable[(10u << 8) | 10u] == 5,
           "E7: tile_objects traded, passable 5 on both");
        ck(g_walker_check == 0 && g_walker_fow_rm == 2 && g_walker_fow_up == 2, "E8: the retail step_check did not run; fog moved for both units");
        ck_eq_d(fx.tick_budget, 0.0, "E9: the tick budget is zeroed");
        ck(fx.path_buffers[0 * PATH_WAYPOINTS_PER_PLAYER + 0].run_length == 4 && fx.path_buffers[PATH_WAYPOINTS_PER_SLOT].run_length == 4,
           "E10: both legs shrank by one");
    }
    // E11/E12: the tagged scale survives a step with the key on, and is averaged with it off
    for (int on = 0; on < 2; ++on) {
        wreset(fx);
        mh::sim::move_fixes mf;
        mf.group_speed = on != 0;
        mh::sim::set_move_fixes(mf);
        unit &a                       = place_walker(fx, 1, 10, 10, 0);
        a.move_step_speed_scale       = mvc::gs_tagged(1.5);
        const double tagged_val       = a.move_step_speed_scale;
        fx.passable[(10u << 8) | 11u] = 1; // free
        walk(fx, 1);
        if (on) ck(fx.u(0, 1).move_step_speed_scale == tagged_val, "E11: group_speed on + tagged scale: kept exactly, no averaging");
        else ck(fx.u(0, 1).move_step_speed_scale == (1.0 + tagged_val) * 0.5, "E12: group_speed off: the retail averaging runs");
        ck(fx.u(0, 1).y == 11, "E13: the step was taken");
    }
    // E14..: mods:M1G 4c [net] move_turn_rate -- the turn cost (turn_speed 2.0 * scale 1.0 = 2.0 of tick budget per notch) is scaled by 2^-k
    for (int k = 0; k <= 2; ++k) {
        wreset(fx);
        mh::sim::move_fixes mf;
        mf.turn_rate = k;
        mh::sim::set_move_fixes(mf);
        fx.cfg_units[5].turn_speed[0]                                                    = 2.0;
        unit &a                                                                          = place_walker(fx, 1, 10, 10, 0);
        fx.path_buffers[0 * PATH_WAYPOINTS_PER_PLAYER + 0 * PATH_WAYPOINTS_PER_SLOT + 0] = {4, 5}; // next leg: heading 4, unit faces 1 -> turn
        a.facing_target = a.facing_current = 1;
        const uint32_t hits0               = mvc::twin_hit_counts()[mvc::TS_TURN];
        walk(fx, 1);
        const double want = 100.0 - 2.0 * (k == 0 ? 1.0 : (k == 1 ? 0.5 : 0.25));
        ck_eq_d(fx.tick_budget, want, k == 0 ? "E14: turn_rate 0: a notch costs the full 2.0 (retail)" : (k == 1 ? "E15: turn_rate 1: a notch costs 1.0 (2x faster)" : "E16: turn_rate 2: a notch costs 0.5 (4x faster)"));
        ck(a.facing_target == 2, "E17: one heading notch per turn tick, whatever the rate");
        ck_eq(mvc::twin_hit_counts()[mvc::TS_TURN] - hits0, k == 0 ? 0u : 1u, "E18: the twin hit counter ticks only with the key on");
    }
}

} // namespace

void run_move_proto_resp_tests(); // mods:M1G 4c: sim_move_proto_selftest_resp.cpp

void run_move_proto_walker_tests() {
    test_blocked();
    test_replan_cap();
    test_enter_queue();
    test_near_enough();
    test_group_speed();
    test_nudge();
    static sim_fixture fx;
    test_walker_body(fx);
    run_move_proto_resp_tests(); // mods:M1G 4c: move_reverse_now rule + move_turn_rate scale
}

} // namespace mh::sim::test
