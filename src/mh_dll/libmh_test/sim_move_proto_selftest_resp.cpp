//
// sim_move_proto_selftest_resp.cpp -- mods:M1G step 4c: `simtest` cases for the movement RESPONSIVENESS rules (sim/move/respond.h):
// [net] move_reverse_now and [net] move_turn_rate. The retail splices (mh/seams/move_proto_respond.cpp) run the SAME header code over
// live memory; the libmh call sites are proven through the real dispatcher / walker bodies in sim_dispatch_selftest.cpp and
// sim_move_proto_selftest_walker.cpp.
//
//   A  reverse_decide: fires only on a STRICTLY nearer origin tile; refuses an occupied / claimed / impassable origin, a moving unit with
//      index 0, every non-walker / settled / non-ground / non-move / boarding / goal-unset case; the torus wrap
//   B  reverse_apply: ms -> 30 - ms, facing flipped, tile / passable / origin / fog writes mirrored, the sprite position continuous for
//      all eight headings x ms 0..14, and it cannot fire twice
//   C  turn_scale: exactly 2^-k (x * scale == ldexp(x, -k) bit for bit, incl. subnormals), k = 0 the identity, the clamp
//
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "sim/move/respond.h"
#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using mh::game::mh_cfg_final_struct_Unit;
using mh::game::mh_llm_strat_crew_soldier;
using mh::game::mh_map_object_unit;
using mh::game::mh_map_tile_object_data;

// The eight principal headings of _G_LLM_MAP_DIR_STEP_DELTAS (1 S, 4 SW, 7 W, 10 NW, 13 N, 16 NE, 19 E, 22 SE).
struct hd {
    int h, dx, dy;
};
const hd k_heads[8] = {{1, 0, 1}, {4, -1, 1}, {7, -1, 0}, {10, -1, -1}, {13, 0, -1}, {16, 1, -1}, {19, 1, 0}, {22, 1, 1}};

struct rworld {
    std::vector<mh_map_object_unit>      units = std::vector<mh_map_object_unit>(8);
    mh_cfg_final_struct_Unit             cfg_one{};
    std::vector<mh_map_tile_object_data> tiles    = std::vector<mh_map_tile_object_data>(256 * 256);
    std::vector<uint8_t>                 passable = std::vector<uint8_t>(256 * 256, 1);
    mh_llm_strat_crew_soldier            sold{};
    uint32_t                             mx = 0xff, my = 0xff;
    int                                  fow_rm = 0, fow_up = 0, set_heading = 0;
    uint32_t                             last_rm_x = 0, last_rm_y = 0, last_up_x = 0, last_up_y = 0;

    rworld() {
        std::memset(units.data(), 0, units.size() * sizeof(mh_map_object_unit));
        std::memset(tiles.data(), 0, tiles.size() * sizeof(mh_map_tile_object_data));
        std::memset(&cfg_one, 0, sizeof(cfg_one));
        cfg_one.move_op_code = 0x0f;
        cfg_one.move_op_arg  = 0x0a;
        cfg_one.type         = 5;
        cfg_one.sight        = 4;
    }
    static size_t ti(uint32_t x, uint32_t y) { return ((x & 0xff) << 8) | (y & 0xff); }

    // unit idx sliding into N = (x, y) along heading h, ms microsteps in; origin value `origin`
    mh_map_object_unit &slide(int idx, uint32_t x, uint32_t y, int h, int ms, uint8_t origin = 1) {
        auto &u                     = units[idx];
        u.x                         = static_cast<uint8_t>(x);
        u.y                         = static_cast<uint8_t>(y);
        u.state                     = 0x0f;
        u.order                     = 0x0f;
        u.facing_target             = static_cast<uint8_t>(h);
        u.facing_current            = static_cast<uint8_t>(h);
        u.move_microstep            = static_cast<decltype(u.move_microstep)>(ms);
        u.origin_tile_was_passable  = origin;
        u.energy                    = 10.0;
        tiles[ti(x, y)].building    = static_cast<uint16_t>(idx);
        tiles[ti(x, y)].class_owner = 0x80;
        passable[ti(x, y)]          = 5;
        return u;
    }

    // ---- the world interface (sim/move/respond.h) ----
    mh_map_object_unit             *unit(uint32_t player, int32_t idx) { return (player == 0 && idx >= 0 && idx < 8) ? &units[idx] : nullptr; }
    const mh_cfg_final_struct_Unit *cfg(const mh_map_object_unit &) { return &cfg_one; }
    mh_map_tile_object_data        &tile(uint32_t x, uint32_t y) { return tiles[ti(x, y)]; }
    uint8_t                        &pass(uint32_t x, uint32_t y) { return passable[ti(x, y)]; }
    void                            dir_delta(int h, int *dx, int *dy) {
        *dx = *dy = 0;
        for (auto &e : k_heads)
            if (e.h == h) {
                *dx = e.dx;
                *dy = e.dy;
            }
    }
    uint32_t                   mask_x() { return mx; }
    uint32_t                   mask_y() { return my; }
    mh_llm_strat_crew_soldier *soldier(uint32_t, uint32_t) { return cfg_one.soldier_count > 0 ? &sold : nullptr; }
    void                       fow_remove_sight(uint32_t, int32_t x, int32_t y, uint8_t) {
        ++fow_rm;
        last_rm_x = static_cast<uint32_t>(x);
        last_rm_y = static_cast<uint32_t>(y);
    }
    void fow_update(uint32_t, uint32_t x, uint32_t y, uint8_t) {
        ++fow_up;
        last_up_x = x;
        last_up_y = y;
    }
    void soldiers_set_heading(uint16_t, int32_t, uint8_t) { ++set_heading; }
    int  notes = 0;
    void note_reverse(uint32_t, int32_t, uint32_t, uint32_t, uint32_t, uint32_t, int) { ++notes; }
};

// ---- A: the decision ------------------------------------------------------------------------------------------------------------
void test_decide() {
    using namespace mh::sim::mvc;
    // unit 1 sliding EAST (h19) into N = (20, 10), came from O = (19, 10)
    auto fresh = []() {
        rworld w;
        w.slide(1, 20, 10, 19, 8);
        w.passable[rworld::ti(19, 10)] = 1; // the walker restored O when it left it
        return w;
    };
    {
        rworld w = fresh();
        ck(reverse_decide(w, 0, 1, 10, 10, 0x0f, false), "A1: goal behind (west of O): reverse");
    }
    {
        rworld w = fresh();
        ck(!reverse_decide(w, 0, 1, 40, 10, 0x0f, false), "A2: goal ahead (east): no reverse");
    }
    {
        // An exact tie needs a diagonal slide (an axis slide has no integer tie): SE (h22) into N = (21, 11), came from O = (20, 10).
        rworld w;
        w.slide(1, 21, 11, 22, 8);
        w.passable[rworld::ti(20, 10)] = 1;
        ck(!reverse_decide(w, 0, 1, 25, 6, 0x0f, false), "A3: exactly equal distance (41 vs 41): retail, no reverse");
        ck(!reverse_decide(w, 0, 1, 26, 6, 0x0f, false), "A3b: (26,6): O 52, N 50 -> nearer to N: no reverse");
        ck(reverse_decide(w, 0, 1, 24, 6, 0x0f, false), "A3c: (24,6): O 32, N 34 -> nearer to O: reverse");
        ck(reverse_decide(w, 0, 1, 16, 14, 0x0f, false), "A3d: (16,14): O 32, N 34 -> reverse");
    }
    // refusals of the origin tile
    {
        rworld w                             = fresh();
        w.tiles[rworld::ti(19, 10)].building = 5; // a unit / building record
        ck(!reverse_decide(w, 0, 1, 10, 10, 0x0f, false), "A4: O holds a record: refused");
    }
    {
        rworld w                                = fresh();
        w.tiles[rworld::ti(19, 10)].class_owner = 0x80; // building index 0 but owned: a moving unit with index 0
        ck(!reverse_decide(w, 0, 1, 10, 10, 0x0f, false), "A5: O claimed by a moving unit with index 0: refused");
    }
    {
        rworld w                       = fresh();
        w.passable[rworld::ti(19, 10)] = 5;
        ck(!reverse_decide(w, 0, 1, 10, 10, 0x0f, false), "A6: O marked 'a unit is moving in' (5): refused");
        w.passable[rworld::ti(19, 10)] = 0;
        ck(!reverse_decide(w, 0, 1, 10, 10, 0x0f, false), "A7: O impassable (0): refused");
    }
    // state refusals
    {
        rworld w         = fresh();
        w.units[1].state = 0x01;
        ck(!reverse_decide(w, 0, 1, 10, 10, 0x0f, false), "A8: not in the walker state: no");
    }
    {
        rworld w                  = fresh();
        w.units[1].move_microstep = 31;
        ck(!reverse_decide(w, 0, 1, 10, 10, 0x0f, false), "A9: settled (ms 31): no");
    }
    {
        rworld w       = fresh();
        w.cfg_one.type = 0x11;
        ck(!reverse_decide(w, 0, 1, 10, 10, 0x0f, false), "A10: non-ground class: no");
    }
    {
        rworld w = fresh();
        ck(!reverse_decide(w, 0, 1, 10, 10, 0x1a, false), "A11: not a move record (order code 0x1a): no");
        ck(reverse_decide(w, 0, 1, 10, 10, 0x0a, false), "A11b: a group move (move_op_arg 0x0a) is a move record");
        ck(!reverse_decide(w, 0, 1, 10, 10, 0x0f, true), "A12: boarding: no");
        ck(!reverse_decide(w, 0, 1, -1, 10, 0x0f, false) && !reverse_decide(w, 0, 1, 10, -1, 0x0f, false), "A13: goal unset (-1): no");
        ck(!reverse_decide(w, 0, 9, 10, 10, 0x0f, false), "A14: unit index out of range: no");
    }
    {
        rworld w                             = fresh();
        w.tiles[rworld::ti(20, 10)].building = 2; // N is not this unit's
        ck(!reverse_decide(w, 0, 1, 10, 10, 0x0f, false), "A15: the unit does not own the tile it is 'sliding into': no");
    }
    // torus wrap: masks 63 (a 64-wide map), unit sliding east into x = 0 from x = 63
    for (uint32_t mask : {0x3fu, 0x7fu, 0xffu}) {
        rworld w;
        w.mx = w.my = mask;
        w.slide(1, 0, 5, 19, 3);
        w.passable[rworld::ti(mask, 5)] = 1;
        ck(reverse_decide(w, 0, 1, mask - 3, 5, 0x0f, false), "A16: wrapped map: goal 3 tiles before the seam is nearer to O (x = mask)");
        ck(!reverse_decide(w, 0, 1, 4, 5, 0x0f, false), "A17: wrapped map: goal 4 tiles after the seam is nearer to N");
    }
    // A18: the half-slide guard (REVERSE_MS_LIMIT = 15): ms 14 fires, ms 15 and later refuse, and ms 0 fires
    for (int ms : {0, 14, 15, 16, 24, 30}) {
        rworld w                  = fresh();
        w.units[1].move_microstep = static_cast<decltype(w.units[1].move_microstep)>(ms);
        const bool want           = ms < 15;
        ck(reverse_decide(w, 0, 1, 10, 10, 0x0f, false) == want, want ? "A18: ms < 15 (less than halfway): reverse" : "A18: ms >= 15 (halfway or past): no reverse");
        ck(reverse_try(w, 0, 1, 10, 10, 0x0f, false) == want, "A18b: reverse_try agrees with reverse_decide");
    }
}

// ---- B: the apply ---------------------------------------------------------------------------------------------------------------
void test_apply() {
    using namespace mh::sim::mvc;
    {
        rworld w;
        auto  &u                       = w.slide(1, 20, 10, 19, 8, 7); // origin_tile_was_passable = 7
        w.passable[rworld::ti(19, 10)] = 3;                            // O's own value
        ck(reverse_try(w, 0, 1, 10, 10, 0x0f, false), "B1: reverses");
        ck(u.x == 19 && u.y == 10, "B2: x,y = O");
        ck(u.facing_target == 7 && u.facing_current == 7, "B3: facing flipped 180 degrees (19 -> 7)");
        ck_eq(static_cast<uint32_t>(u.move_microstep), 22u, "B4: ms 8 -> 30 - 8 = 22");
        ck(w.tiles[rworld::ti(20, 10)].building == 0 && w.tiles[rworld::ti(20, 10)].class_owner == 0, "B5: N's tile record cleared");
        ck_eq(static_cast<uint32_t>(w.passable[rworld::ti(20, 10)]), 7u, "B6: passable[N] restored from origin_tile_was_passable");
        ck(w.tiles[rworld::ti(19, 10)].building == 1 && w.tiles[rworld::ti(19, 10)].class_owner == 0x80, "B7: O's tile record = this unit, owner|0x80");
        ck_eq(static_cast<uint32_t>(w.passable[rworld::ti(19, 10)]), 5u, "B8: passable[O] = 5 (a unit is moving in)");
        ck_eq(static_cast<uint32_t>(u.origin_tile_was_passable), 3u, "B9: origin_tile_was_passable = the old passable[O]");
        ck(w.fow_rm == 1 && w.last_rm_x == 20 && w.fow_up == 1 && w.last_up_x == 19, "B10: fog: sight removed at N, added at O");
        // cannot fire twice: the goal is now strictly nearer the unit's tile
        ck_eq(static_cast<uint32_t>(w.notes), 1u, "B10b: one evidence note per reverse");
        ck(!reverse_try(w, 0, 1, 10, 10, 0x0f, false), "B11: a second pass does not fire (strict test)");
        ck(u.x == 19 && w.fow_rm == 1, "B12: and changes nothing");
    }
    // soldiers: the heading follows the flip
    {
        rworld w;
        w.cfg_one.soldier_count = 2;
        w.sold.idle_wander_flag = 1;
        w.slide(1, 20, 10, 19, 8);
        w.passable[rworld::ti(19, 10)] = 1;
        ck(reverse_try(w, 0, 1, 10, 10, 0x0f, false) && w.set_heading == 1 && w.sold.idle_wander_flag == 0, "B13: soldiers get the new heading and the wander flag clears");
    }
    // the sprite position is continuous across the flip for all eight headings x ms 0..14 (the half-slide guard refuses 15..30, A18):
    //   screen = tile * 32 + 16 - delta(facing_target) * (31 - ms)       (x,y is already the destination tile N)
    int bad = 0;
    for (const hd &e : k_heads) {
        for (int ms = 0; ms < REVERSE_MS_LIMIT; ++ms) {
            rworld w;
            auto  &u                                       = w.slide(1, 100, 100, e.h, ms);
            w.passable[rworld::ti(100 - e.dx, 100 - e.dy)] = 1;
            const int bx = 100 * 32 + 16 - e.dx * (31 - ms), by = 100 * 32 + 16 - e.dy * (31 - ms);
            if (!reverse_try(w, 0, 1, 100 - 40 * e.dx, 100 - 40 * e.dy, 0x0f, false)) {
                ++bad;
                continue;
            }
            int ndx = 0, ndy = 0;
            w.dir_delta(u.facing_target, &ndx, &ndy);
            const int ax = u.x * 32 + 16 - ndx * (31 - u.move_microstep), ay = u.y * 32 + 16 - ndy * (31 - u.move_microstep);
            if (ax != bx || ay != by) ++bad;
            if (u.x != 100 - e.dx || u.y != 100 - e.dy) ++bad;
        }
    }
    ck_eq(static_cast<uint32_t>(bad), 0u, "B14: all 8 headings x ms 0..14: reversed, on O, and the render position is identical before and after");
}

// ---- C: the turn scale ----------------------------------------------------------------------------------------------------------
void test_turn_scale() {
    using namespace mh::sim::mvc;
    ck(turn_scale(0) == 1.0 && turn_scale(1) == 0.5 && turn_scale(2) == 0.25, "C1: 2^-k for k = 0, 1, 2");
    ck(turn_scale(-3) == 1.0 && turn_scale(9) == 0.25, "C2: clamped to 0..2");
    ck_eq(static_cast<uint32_t>(turn_rate_clamp(7)), 2u, "C3: turn_rate_clamp");
    const double xs[] = {1.0, 0.1, 3.14159265358979, 1e-300, 4.9e-324 /* smallest subnormal */, 2.2250738585072014e-308, 123456.789, 1e10 * 0.3};
    int          bad  = 0;
    for (double x : xs)
        for (int k = 0; k <= 2; ++k) {
            const double a = x * turn_scale(k), b = std::ldexp(x, -k);
            if (std::memcmp(&a, &b, sizeof(a)) != 0) {
                // a subnormal result is rounded once either way; only a bit mismatch counts
                ++bad;
            }
        }
    ck_eq(static_cast<uint32_t>(bad), 0u, "C4: x * turn_scale(k) == ldexp(x, -k), bit for bit, incl. subnormals");
    // k = 0 is the identity on every bit pattern (incl. -0.0)
    const double z = -0.0;
    const double r = z * turn_scale(0);
    ck(std::memcmp(&z, &r, sizeof(z)) == 0, "C5: k = 0 is the identity (-0.0 stays -0.0)");
}

} // namespace

void run_move_proto_resp_tests() {
    test_decide();
    test_apply();
    test_turn_scale();
}

} // namespace mh::sim::test
