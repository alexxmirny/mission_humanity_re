//
// sim_storage_get_approach_tile_selftest.cpp -- offline `simtest` oracle for
// llm_strat_storage_get_approach_tile @0x0048b37c (sim/sim_storage_get_approach_tile.h/.cpp,
// RI-SIM / SIM1-G3).
//
// WHY THIS ORACLE EXISTS DESPITE 400+ ARMED-SHADOW CALLS: the function returns void and delivers
// its whole result through two OUT params (*out_fine_x/*out_fine_y) with no other observable
// effect, so gen_dll_shadow.py flags the site VACUOUS -- zero compared regions. 400+ clean calls
// over a 15000-step all-AI soak compared NOTHING. This file is the only real evidence for this
// function; ONLY possible posture, same as sim_bldg_get_coords_selftest.cpp's
// llm_strat_bldg_get_coords.
//
// EXPECTED BEHAVIOUR, from the DISASSEMBLY
// (tmp/decomp_sim/llm_strat_storage_get_approach_tile_0048b37c.asm), never the .c draft beside it:
//
//   resolved_slot = (storage_idx != 0) ? storage_idx                                (0x0048b39d-0x0048b3a9)
//                                       : units[player][unit_index].home_storage_slot (0x0048b3ab-0x0048b3c8)
//   unit_proto_id = units[player][unit_index].unit_proto_id                          (0x0048b3cb-0x0048b3e1)
//   if (Unit[unit_proto_id].move_op_code == 0x11) {                                  (0x0048b3ee/0x0048b3f5)
//       b_index          = unit_storage[player][resolved_slot].b_index               (0x0048b405-0x0048b40e)
//       building_type_id = buildings[player][b_index].building_id                    (0x0048b418-0x0048b42b)
//       facing           = cfg_buildings[building_type_id].door_approach_route[0]
//                                        (byte @0xd9f4a0-relative: 0x0048b49e X block / 0x0048b527 Y block)
//       diff = fine_to_tile(Unit[unit_proto_id].elevation) - fine_to_tile(Unit[unit_proto_id].elevation_2)
//                        (X block re-derives it 0x0048b458-0x0048b492; Y block re-derives it AGAIN,
//                         independently, 0x0048b4e1-0x0048b51b -- two separate reads, no shared local)
//       *out_fine_x = (exit_tile_x - diff * dir_step_offsets[facing].dx) & map_width_mask  (0x0048b4c2-0x0048b4d5)
//       *out_fine_y = (exit_tile_y - diff * dir_step_offsets[facing].dy) & map_height_mask (0x0048b54b-0x0048b55e)
//   } else {
//       *out_fine_x = unit_storage[player][resolved_slot].exit_tile_x   -- RAW, no wrap mask (0x0048b575-0x0048b57e)
//       *out_fine_y = unit_storage[player][resolved_slot].exit_tile_y   -- RAW, no wrap mask (0x0048b593-0x0048b59c)
//   }
//
// Cross-checked against .cpp: matches the disassembly exactly in every branch, every offset
// (b_index/building_id/door_approach_route/exit_tile_x/exit_tile_y/elevation/elevation_2), every
// stride (STORAGE_PER_PLAYER=25 * 0xf4 == 0x17d4; BUILDINGS_PER_PLAYER=100 * 0x111 == 0x6aa4;
// UNITS_PER_PLAYER=100 * 0xe9 == 0x5b04, all confirmed against tools' own constants), the dx/dy
// table stride (facing*8, dx at +0/dy at +4, matching mh_llm_vec2i), and the mask pair
// (map_width_mask/map_height_mask == v.geom->width_mask/height_mask). NO DIVERGENCE FOUND between
// the .cpp and the .asm.
//
// One header-banner claim is now STALE, not wrong: the .h's "DECLARED NEED" section proposes
// splitting a `facing` field out of cfg_final_struct_Building's pad block at +0x820. That need is
// already resolved in addr/mh_structs.gen.h -- `door_approach_route` (uint8_t[14] @ +0x820) is a
// real named field today (added SIM1-G3, 2026-08-21), and the .cpp already reads
// `door_approach_route[0]` directly. Nothing for this oracle to pin there; noted for the conductor.
//
#include "sim/sim_storage_get_approach_tile.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

void check_approach(sim_fixture &fx, uint16_t player, uint16_t unit_index, uint32_t storage_idx,
                    uint32_t want_x, uint32_t want_y, const char *what_x, const char *what_y) {
    uint32_t out_x = 0x0badf00d, out_y = 0x0badf00d;
    detail::storage_get_approach_tile(fx.view(), player, unit_index, &out_x, &out_y, storage_idx);
    ck_eq(out_x, want_x, what_x);
    ck_eq(out_y, want_y, what_y);
}

} // namespace

void run_storage_get_approach_tile_tests() {
    sim_fixture fx;

    // ---- T1: non-heli (else) branch, storage_idx EXPLICIT/nonzero -- resolved_slot must take the
    // storage_idx arm of the ternary, NOT home_storage_slot (seeded to a DIFFERENT slot carrying
    // decoy exit-tile values, so taking the wrong arm fails loudly instead of accidentally passing).
    // Also pins "NO wrap mask applied" on this branch: the seeded exit tiles exceed BOTH width_mask
    // (0xff) and height_mask (0x3f) from sim_fixture::reset()'s defaults, so a wrongly-masked result
    // would come back clipped (44/36) instead of the raw values asserted here. -------------------
    {
        fx.reset();
        constexpr uint16_t PLAYER = 2, UNIT_INDEX = 7;
        constexpr uint32_t STORAGE_IDX = 5, DECOY_HOME_SLOT = 9;

        fx.u(PLAYER, UNIT_INDEX).unit_proto_id     = 30;
        fx.u(PLAYER, UNIT_INDEX).home_storage_slot = static_cast<uint8_t>(DECOY_HOME_SLOT);
        fx.cfg_units[30].move_op_code              = 0x05; // NOT 0x11 -> else branch, 0x0048b3f5

        fx.storage[PLAYER * STORAGE_PER_PLAYER + STORAGE_IDX].exit_tile_x = 300; // > width_mask 0xff
        fx.storage[PLAYER * STORAGE_PER_PLAYER + STORAGE_IDX].exit_tile_y = 100; // > height_mask 0x3f
        // decoy at home_storage_slot -- must NOT be read since storage_idx (5) != 0
        fx.storage[PLAYER * STORAGE_PER_PLAYER + DECOY_HOME_SLOT].exit_tile_x = 999;
        fx.storage[PLAYER * STORAGE_PER_PLAYER + DECOY_HOME_SLOT].exit_tile_y = 888;

        check_approach(
            fx, PLAYER, UNIT_INDEX, STORAGE_IDX, 300u, 100u,
            "T1: resolved_slot = storage_idx (5), NOT home_storage_slot (9) -- out_fine_x = exit_tile_x "
            "of slot 5 (300), not the slot-9 decoy (999), 0x0048b39d-0x0048b3a9",
            "T1: else-branch out_fine_y = exit_tile_y RAW, no mask (100 exceeds height_mask 0x3f but "
            "passes through unmasked, not clipped to 36), 0x0048b593-0x0048b59c");
    }

    // ---- T2: non-heli (else) branch, storage_idx == 0 -- resolved_slot must FALL BACK to
    // home_storage_slot. A decoy sits at the LITERAL slot 0 (the param's own value), catching a bug
    // that used storage_idx directly as the slot index instead of taking the ternary's fallback arm.
    // ------------------------------------------------------------------------------------------
    {
        fx.reset();
        constexpr uint16_t PLAYER = 2, UNIT_INDEX = 7;
        constexpr uint32_t HOME_SLOT = 12;

        fx.u(PLAYER, UNIT_INDEX).unit_proto_id     = 31;
        fx.u(PLAYER, UNIT_INDEX).home_storage_slot = static_cast<uint8_t>(HOME_SLOT);
        fx.cfg_units[31].move_op_code              = 0x07; // NOT 0x11 -> else branch, 0x0048b3f5

        fx.storage[PLAYER * STORAGE_PER_PLAYER + HOME_SLOT].exit_tile_x = 333;
        fx.storage[PLAYER * STORAGE_PER_PLAYER + HOME_SLOT].exit_tile_y = 70; // > height_mask 0x3f too
        // decoy at literal slot 0 -- must NOT be read when the fallback arm is taken correctly
        fx.storage[PLAYER * STORAGE_PER_PLAYER + 0].exit_tile_x = 1;
        fx.storage[PLAYER * STORAGE_PER_PLAYER + 0].exit_tile_y = 2;

        check_approach(fx, PLAYER, UNIT_INDEX, /*storage_idx=*/0, 333u, 70u,
                       "T2: storage_idx==0 -> resolved_slot falls back to home_storage_slot (12), "
                       "not the literal-0 decoy (1), 0x0048b3a1-0x0048b3c8",
                       "T2: else-branch out_fine_y = home slot's exit_tile_y RAW, no mask (70 > "
                       "height_mask 0x3f, not clipped to 6), 0x0048b593-0x0048b59c");
    }

    // ---- T3: heli branch (move_op_code==0x11), POSITIVE elevation diff. Building is reached through
    // TWO levels of indirection (storage.b_index -> buildings[player][b_index].building_id ->
    // cfg_buildings[building_type_id]) with resolved_slot(6)/b_index(8)/building_type_id(45) all
    // DISTINCT so a one-level shortcut (e.g. treating b_index as building_type_id) fails. off.dx
    // (3) != off.dy (-5) so an axis swap fails. X's term drives the pre-mask value NEGATIVE (wraps
    // below 0 through width_mask); Y's drives it ABOVE height_mask (wraps down through it) -- the
    // two mask-bite directions, one per axis. ---------------------------------------------------
    {
        fx.reset();
        constexpr uint16_t PLAYER = 4, UNIT_INDEX = 11;
        constexpr uint32_t STORAGE_IDX = 6, B_INDEX = 8, BUILDING_TYPE_ID = 45, FACING = 12,
                           UNIT_PROTO_ID = 20;

        fx.u(PLAYER, UNIT_INDEX).unit_proto_id   = static_cast<uint16_t>(UNIT_PROTO_ID);
        fx.cfg_units[UNIT_PROTO_ID].move_op_code = 0x11; // heli mover class -> if-branch, 0x0048b3ee
        fx.cfg_units[UNIT_PROTO_ID].elevation    = 320;  // fine_to_tile(320) = 10
        fx.cfg_units[UNIT_PROTO_ID].elevation_2  = 64;   // fine_to_tile(64)  = 2; diff = 10-2 = 8

        fx.storage[PLAYER * STORAGE_PER_PLAYER + STORAGE_IDX].b_index     = static_cast<int32_t>(B_INDEX);
        fx.storage[PLAYER * STORAGE_PER_PLAYER + STORAGE_IDX].exit_tile_x = 10;
        fx.storage[PLAYER * STORAGE_PER_PLAYER + STORAGE_IDX].exit_tile_y = 30;
        fx.b(PLAYER, static_cast<int32_t>(B_INDEX)).building_id           = static_cast<uint16_t>(BUILDING_TYPE_ID);
        fx.cfg_buildings[BUILDING_TYPE_ID].door_approach_route[0]         = static_cast<uint8_t>(FACING);
        fx.dir_step_offsets[FACING].dx                                    = 3;
        fx.dir_step_offsets[FACING].dy                                    = -5;

        // out_fine_x = (10 - 8*3)  & 0xff = (10-24)  & 0xff = (uint32_t)-14 & 0xff = 0xf2 = 242
        // out_fine_y = (30 - 8*-5) & 0x3f = (30+40)  & 0x3f = 70 & 0x3f             = 6
        check_approach(
            fx, PLAYER, UNIT_INDEX, STORAGE_IDX, 242u, 6u,
            "T3: out_fine_x = (exit_x 10 - diff 8 * dx 3) & width_mask 0xff, wraps below 0 -> 242 "
            "(indirection: slot 6 -> b_index 8 -> building_type_id 45 -> facing 12 -> dx 3), 0x0048b4c2-0x0048b4d5",
            "T3: out_fine_y = (exit_y 30 - diff 8 * dy -5) & height_mask 0x3f, wraps above 0x3f -> 6 "
            "(SAME facing 12 -> dy -5, not off.dx), 0x0048b54b-0x0048b55e");
    }

    // ---- T4: heli branch, NEGATIVE elevation diff (pins the subtraction's direction from the OTHER
    // side vs T3), a DIFFERENT resolved_slot/b_index/building_type_id/facing/off pair so this case
    // cannot pass by reusing T3's numbers, and the OPPOSITE mask-bite direction on each axis from T3
    // (X now wraps ABOVE, Y now wraps BELOW) -- both directions pinned on both axes across T3+T4. --
    {
        fx.reset();
        constexpr uint16_t PLAYER = 4, UNIT_INDEX = 11;
        constexpr uint32_t STORAGE_IDX = 14, B_INDEX = 9, BUILDING_TYPE_ID = 50, FACING = 17,
                           UNIT_PROTO_ID = 21;

        fx.u(PLAYER, UNIT_INDEX).unit_proto_id   = static_cast<uint16_t>(UNIT_PROTO_ID);
        fx.cfg_units[UNIT_PROTO_ID].move_op_code = 0x11;
        fx.cfg_units[UNIT_PROTO_ID].elevation    = 64;  // fine_to_tile(64)  = 2
        fx.cfg_units[UNIT_PROTO_ID].elevation_2  = 320; // fine_to_tile(320) = 10; diff = 2-10 = -8

        fx.storage[PLAYER * STORAGE_PER_PLAYER + STORAGE_IDX].b_index     = static_cast<int32_t>(B_INDEX);
        fx.storage[PLAYER * STORAGE_PER_PLAYER + STORAGE_IDX].exit_tile_x = 220;
        fx.storage[PLAYER * STORAGE_PER_PLAYER + STORAGE_IDX].exit_tile_y = 10;
        fx.b(PLAYER, static_cast<int32_t>(B_INDEX)).building_id           = static_cast<uint16_t>(BUILDING_TYPE_ID);
        fx.cfg_buildings[BUILDING_TYPE_ID].door_approach_route[0]         = static_cast<uint8_t>(FACING);
        fx.dir_step_offsets[FACING].dx                                    = 5;
        fx.dir_step_offsets[FACING].dy                                    = -3;

        // out_fine_x = (220 - (-8)*5)  & 0xff = (220+40) & 0xff = 260 & 0xff = 4
        // out_fine_y = (10  - (-8)*-3) & 0x3f = (10-24)  & 0x3f = (uint32_t)-14 & 0x3f = 0x32 = 50
        check_approach(
            fx, PLAYER, UNIT_INDEX, STORAGE_IDX, 4u, 50u,
            "T4: out_fine_x = (exit_x 220 - diff -8 * dx 5) & width_mask 0xff, wraps above 0xff -> 4 "
            "(indirection: slot 14 -> b_index 9 -> building_type_id 50 -> facing 17 -> dx 5), 0x0048b4c2-0x0048b4d5",
            "T4: out_fine_y = (exit_y 10 - diff -8 * dy -3) & height_mask 0x3f, wraps below 0 -> 50 "
            "(SAME facing 17 -> dy -3, not off.dx; diff's sign pinned from the elevation<elevation_2 side), 0x0048b54b-0x0048b55e");
    }
}

} // namespace mh::sim::test
