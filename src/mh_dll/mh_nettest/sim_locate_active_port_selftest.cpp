//
// sim_locate_active_port_selftest.cpp -- `simtest` offline oracle for llm_strat_locate_active_port
// (sim/sim_locate_active_port.h/.cpp, RI-SIM / SIM1F).
//
// A bounded roster scan (buildings[player][1..99]) for an operational A_PORT/H_PORT (energy>0,
// cfg type gate, built_flags==3, online_state!=0), then ONE outward call
// (llm_strat_tile_neighbor_reverse_dir) to compute the found port's approach-tile screen position.
// No sim_store write anywhere -- the outward call writes only through the caller-supplied
// out_col/out_row scratch, so it is stubbed with a RECORDING mock (records the call args, and hands
// back caller-chosen out_col/out_row values so the test can also confirm they propagate through).
//
#include "sim/sim_locate_active_port.h"

#include <limits>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

struct TNRDCall {
    int32_t tile_x, tile_y, dir_index;
};
std::vector<TNRDCall> g_tnrd_calls;
int32_t               g_stub_out_col = 0, g_stub_out_row = 0;

void stub_tnrd(int32_t tile_x, int32_t tile_y, int32_t dir_index, uint32_t *out_x, uint32_t *out_y) {
    g_tnrd_calls.push_back({tile_x, tile_y, dir_index});
    *out_x = (uint32_t)g_stub_out_col;
    *out_y = (uint32_t)g_stub_out_row;
}

const locate_active_port_calls test_calls_v = {stub_tnrd};

constexpr uint32_t PLAYER = 2; // arbitrary, non-zero, within MAX_PLAYERS(8)

} // namespace

void run_locate_active_port_tests() {
    sim_fixture fx;
    int32_t     out_col = 0, out_row = 0;
    uint32_t    out_port_slot = 0, r = 0;

    // ---- C1: empty roster (slot0 sentinel/count == 0) -> immediate 0, out params untouched, no
    // outward call.
    fx.reset();
    fx.b((int32_t)PLAYER, 0).index = 0;
    out_col                        = 0x1111;
    out_row                        = 0x2222;
    out_port_slot                  = 0x3333;
    g_tnrd_calls.clear();
    r = detail::locate_active_port(fx.view(), test_calls_v, PLAYER, &out_col, &out_row,
                                   &out_port_slot);
    ck_eq(r, 0u, "locate_active_port C1: empty roster -> 0");
    ck_eq((uint32_t)out_col, 0x1111u, "locate_active_port C1: out_col untouched");
    ck_eq((uint32_t)out_row, 0x2222u, "locate_active_port C1: out_row untouched");
    ck_eq(out_port_slot, 0x3333u, "locate_active_port C1: out_port_slot untouched");
    ck(g_tnrd_calls.empty(), "locate_active_port C1: no outward call");

    // ---- C2: non-empty roster but no match anywhere -- exhausts the slot>99 bound -> 0.
    fx.reset();
    fx.b((int32_t)PLAYER, 0).index = 1; // nonzero -> loop proceeds; slots 1..99 default energy=0.0 (dead)
    g_tnrd_calls.clear();
    r = detail::locate_active_port(fx.view(), test_calls_v, PLAYER, &out_col, &out_row,
                                   &out_port_slot);
    ck_eq(r, 0u, "locate_active_port C2: no match anywhere -> 0 (exhausts the slot>99 bound)");
    ck(g_tnrd_calls.empty(), "locate_active_port C2: no outward call");

    // ---- C3: energy==0.0 EXACTLY is DEAD (the gate is `!(energy<=0.0)`) -- skipped even though
    // every other field matches.
    fx.reset();
    fx.b((int32_t)PLAYER, 0).index = 1;
    {
        building &cand           = fx.b((int32_t)PLAYER, 5);
        cand.energy              = 0.0; // boundary: <=0.0 -> dead
        cand.building_id         = 7;
        fx.cfg_buildings[7].type = BUILDING_TYPE_A_PORT;
        cand.built_flags         = 3;
        cand.online_state        = 4;
    }
    r = detail::locate_active_port(fx.view(), test_calls_v, PLAYER, &out_col, &out_row,
                                   &out_port_slot);
    ck_eq(r, 0u, "locate_active_port C3: energy==0.0 boundary is DEAD (skip)");

    // ---- C4: ONE port, full FOUND path -- register mapping + owner-vs-port facing redirection.
    // The port itself is at buildings[PLAYER][5]; its storage slot's owner (b_index) is a DIFFERENT
    // building (index 9) whose facing (17) must be used, NOT the port's own facing (99).
    fx.reset();
    fx.b((int32_t)PLAYER, 0).index = 1;
    {
        building &port                             = fx.b((int32_t)PLAYER, 5);
        port.energy                                = 50.0;
        port.building_id                           = 7;
        fx.cfg_buildings[7].type                   = BUILDING_TYPE_A_PORT;
        fx.cfg_buildings[7].door_approach_route[0] = 99; // the PORT's own facing -- must NOT be used (see below)
        port.built_flags                           = 3;
        port.online_state                          = 4;
        port.sub_id                                = 3; // -> out_port_slot

        unit_storage &st = fx.storage[(size_t)(PLAYER * STORAGE_PER_PLAYER + 3)];
        st.b_index       = 9; // owner building index, DIFFERENT from the port's own slot (5)
        st.exit_tile_x   = 123;
        st.exit_tile_y   = 456;

        building &owner                             = fx.b((int32_t)PLAYER, 9);
        owner.building_id                           = 42;
        fx.cfg_buildings[42].door_approach_route[0] = 17; // the OWNER's facing -- THIS is what must be used
    }
    g_tnrd_calls.clear();
    g_stub_out_col = 777;
    g_stub_out_row = 888;
    out_col        = 0;
    out_row        = 0;
    out_port_slot  = 0;
    r              = detail::locate_active_port(fx.view(), test_calls_v, PLAYER, &out_col, &out_row,
                                                &out_port_slot);
    ck_eq(r, 1u, "locate_active_port C4: A_PORT match -> returns 1");
    ck_eq(out_port_slot, 3u, "locate_active_port C4: out_port_slot = matched building's sub_id (3)");
    ck(g_tnrd_calls.size() == 1,
       "locate_active_port C4: tile_neighbor_reverse_dir called exactly once");
    ck_eq((uint32_t)g_tnrd_calls[0].tile_x, 123u,
          "locate_active_port C4: callee tile_x = storage.exit_tile_x");
    ck_eq((uint32_t)g_tnrd_calls[0].tile_y, 456u,
          "locate_active_port C4: callee tile_y = storage.exit_tile_y");
    ck_eq((uint32_t)g_tnrd_calls[0].dir_index, 17u,
          "locate_active_port C4: callee dir_index = the OWNER building's facing (17), not the port's own (99)");
    ck_eq((uint32_t)out_col, 777u, "locate_active_port C4: out_col receives the callee's write (777)");
    ck_eq((uint32_t)out_row, 888u, "locate_active_port C4: out_row receives the callee's write (888)");

    // ---- C5: first-match ordering -- two valid candidates, the LOWER slot wins.
    fx.reset();
    fx.b((int32_t)PLAYER, 0).index = 1;
    {
        building &first                            = fx.b((int32_t)PLAYER, 2);
        first.energy                               = 10.0;
        first.building_id                          = 1;
        fx.cfg_buildings[1].type                   = BUILDING_TYPE_A_PORT;
        fx.cfg_buildings[1].door_approach_route[0] = 5;
        first.built_flags                          = 3;
        first.online_state                         = 1;
        first.sub_id                               = 0;
        unit_storage &st0                          = fx.storage[(size_t)(PLAYER * STORAGE_PER_PLAYER + 0)];
        st0.b_index                                = 2;
        st0.exit_tile_x                            = 11;
        st0.exit_tile_y                            = 22;

        // a SECOND valid port at a later slot -- must be ignored since slot 2 wins first.
        building &second         = fx.b((int32_t)PLAYER, 6);
        second.energy            = 10.0;
        second.building_id       = 2;
        fx.cfg_buildings[2].type = BUILDING_TYPE_H_PORT;
        second.built_flags       = 3;
        second.online_state      = 1;
        second.sub_id            = 1;
    }
    g_tnrd_calls.clear();
    r = detail::locate_active_port(fx.view(), test_calls_v, PLAYER, &out_col, &out_row,
                                   &out_port_slot);
    ck_eq(r, 1u, "locate_active_port C5: two candidates -> still returns 1");
    ck_eq(out_port_slot, 0u,
          "locate_active_port C5: FIRST match wins (slot 2's sub_id=0), not slot 6's (sub_id=1)");
    ck_eq((uint32_t)g_tnrd_calls[0].tile_x, 11u,
          "locate_active_port C5: exit tile taken from the FIRST match's storage slot");

    // ---- C6: type gate failure -- neither A_PORT nor H_PORT -> skip, even with everything else right.
    fx.reset();
    fx.b((int32_t)PLAYER, 0).index = 1;
    {
        building &wrong          = fx.b((int32_t)PLAYER, 5);
        wrong.energy             = 10.0;
        wrong.building_id        = 1;
        fx.cfg_buildings[1].type = 0x01; // not A_PORT(0x0c)/H_PORT(0x20)
        wrong.built_flags        = 3;
        wrong.online_state       = 1;
    }
    r = detail::locate_active_port(fx.view(), test_calls_v, PLAYER, &out_col, &out_row,
                                   &out_port_slot);
    ck_eq(r, 0u, "locate_active_port C6: wrong building type -> no match");

    // ---- C7: H_PORT also matches (the type gate's second arm).
    fx.reset();
    fx.b((int32_t)PLAYER, 0).index = 1;
    {
        building &hport                                               = fx.b((int32_t)PLAYER, 5);
        hport.energy                                                  = 10.0;
        hport.building_id                                             = 1;
        fx.cfg_buildings[1].type                                      = BUILDING_TYPE_H_PORT;
        hport.built_flags                                             = 3;
        hport.online_state                                            = 1;
        hport.sub_id                                                  = 0;
        fx.storage[(size_t)(PLAYER * STORAGE_PER_PLAYER + 0)].b_index = 5;
    }
    r = detail::locate_active_port(fx.view(), test_calls_v, PLAYER, &out_col, &out_row,
                                   &out_port_slot);
    ck_eq(r, 1u, "locate_active_port C7: H_PORT (0x20) also matches the type gate");

    // ---- C8: built_flags gate is EXACT ==3, not a bitmask test -- extra bits (7) must NOT match.
    fx.reset();
    fx.b((int32_t)PLAYER, 0).index = 1;
    {
        building &bf             = fx.b((int32_t)PLAYER, 5);
        bf.energy                = 10.0;
        bf.building_id           = 1;
        fx.cfg_buildings[1].type = BUILDING_TYPE_A_PORT;
        bf.built_flags           = 7; // (7 & 3) == 3, but the gate is `== 3` exactly
        bf.online_state          = 1;
    }
    r = detail::locate_active_port(fx.view(), test_calls_v, PLAYER, &out_col, &out_row,
                                   &out_port_slot);
    ck_eq(r, 0u,
          "locate_active_port C8: built_flags==7 (extra bits) does NOT match; gate is exact ==3");

    // ---- C9: online_state==0 -> skip.
    fx.reset();
    fx.b((int32_t)PLAYER, 0).index = 1;
    {
        building &os             = fx.b((int32_t)PLAYER, 5);
        os.energy                = 10.0;
        os.building_id           = 1;
        fx.cfg_buildings[1].type = BUILDING_TYPE_A_PORT;
        os.built_flags           = 3;
        os.online_state          = 0;
    }
    r = detail::locate_active_port(fx.view(), test_calls_v, PLAYER, &out_col, &out_row,
                                   &out_port_slot);
    ck_eq(r, 0u, "locate_active_port C9: online_state==0 -> no match");

    // ---- C10: NaN energy is NOT <=0.0 -- falls through to the alive body, matching
    // `!(energy<=0.0)`, NOT the naive `0.0<energy` reading (per the header's FPU-condition-code
    // correction).
    fx.reset();
    fx.b((int32_t)PLAYER, 0).index = 1;
    {
        building &nanb                                                = fx.b((int32_t)PLAYER, 5);
        nanb.energy                                                   = std::numeric_limits<double>::quiet_NaN();
        nanb.building_id                                              = 1;
        fx.cfg_buildings[1].type                                      = BUILDING_TYPE_A_PORT;
        nanb.built_flags                                              = 3;
        nanb.online_state                                             = 1;
        nanb.sub_id                                                   = 0;
        fx.storage[(size_t)(PLAYER * STORAGE_PER_PLAYER + 0)].b_index = 5;
    }
    r = detail::locate_active_port(fx.view(), test_calls_v, PLAYER, &out_col, &out_row,
                                   &out_port_slot);
    ck_eq(r, 1u,
          "locate_active_port C10: NaN energy is NOT <=0.0 -> treated as alive (!(energy<=0.0))");

    // ---- C11: `player`'s high bits (above bit 15) are masked off (`player & 0xffff`).
    fx.reset();
    fx.b((int32_t)PLAYER, 0).index = 1;
    {
        building &tp                                                  = fx.b((int32_t)PLAYER, 5);
        tp.energy                                                     = 10.0;
        tp.building_id                                                = 1;
        fx.cfg_buildings[1].type                                      = BUILDING_TYPE_A_PORT;
        tp.built_flags                                                = 3;
        tp.online_state                                               = 1;
        tp.sub_id                                                     = 0;
        fx.storage[(size_t)(PLAYER * STORAGE_PER_PLAYER + 0)].b_index = 5;
    }
    {
        const uint32_t hi_player = 0x10000u | PLAYER; // high bits set, low 16 bits == PLAYER
        r                        = detail::locate_active_port(fx.view(), test_calls_v, hi_player, &out_col, &out_row,
                                                              &out_port_slot);
    }
    ck_eq(r, 1u, "locate_active_port C11: high bits of `player` above bit15 are masked off");

    // ---- MUTATION NOTES ------------------------------------------------------------------------
    // 1. Reading building[player][0].index as the roster count without the uint16 truncation would
    //    still pass most cases here (values are small), but the C11 player-masking check specifically
    //    catches a dropped `& 0xffffu` on `player` (an unmasked hi_player indexes far outside the
    //    fixture's buildings vector -> a crash/ASan report, itself a clear red).
    // 2. Using `energy > 0.0` (or `0.0 < energy`) instead of `!(energy <= 0.0)` fails C10 (NaN
    //    energy would be excluded instead of matched).
    // 3. Using `(built_flags & 3) == 3` instead of `built_flags == 3` fails C8 (7 would match).
    // 4. Using the PORT's own facing instead of the storage slot's OWNER building's facing fails
    //    C4's dir_index check (99 instead of 17).
}

} // namespace mh::sim::test
