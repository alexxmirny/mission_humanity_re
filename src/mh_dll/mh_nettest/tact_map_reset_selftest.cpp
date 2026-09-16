//
// tact_map_reset_selftest.cpp -- offline oracle for llm_tact_map_reset (TACT1A batch A,
// 2026-08-26). See tact/tact_map_reset.h for the derivation.
//
// WHY OFFLINE, NOT RIG: this function's own tail call (llm_tact_mission_load) reaches the SAME 5
// TACT-CUT2-ungated `effectful` shared callees mission_load's own manifest entry does -- arming it
// under shadow would double-fire real file I/O, PRNG draws and palette conversions. The one outward
// call is mocked via the header's `map_reset_calls` struct, so this oracle never executes the real
// loader.
//
#include "tact/tact_map_reset.h"
#include "tact_test_support.h"

namespace mh::tact::test {

namespace {

struct call_log {
    int   mission_load_calls = 0;
    char *mission_load_arg   = nullptr;
};

call_log &log() {
    static call_log l;
    return l;
}

void reset_log() { log() = call_log{}; }

void mock_mission_load(char *mission_name) {
    ++log().mission_load_calls;
    log().mission_load_arg = mission_name;
}

map_reset_calls mock_calls() { return {mock_mission_load}; }

} // namespace

void run_map_reset_tests() {
    // T1: path slot 0 reserved (=1), slots 1..99 freed (=0) -- and NO OTHER owner's slots touched.
    {
        tact_fixture fx;
        // Poison every path-slot-flags cell to a sentinel distinct from both 0 and 1, across ALL 8
        // owners, so an untouched cell is distinguishable from one the function legitimately wrote.
        for (auto &b : fx.path_slot_flags) b = 0x7f;
        tact_store      own             = fx.store();
        map_reset_calls c               = mock_calls();
        char            mission_name[8] = "MIS1";
        detail::map_reset(own, c, mission_name);
        ck_eq((uint32_t)own.planes().path_slot_flag_at(0, 0), 1u,
              "T1: owner-0 slot 0 reserved, 0x0042e739");
        for (int32_t j = 1; j < mh::state::PATH_SLOTS_PER_OWNER; ++j) {
            if (own.planes().path_slot_flag_at(0, j) != 0) {
                ck(false, "T1: owner-0 slot freed");
                break;
            }
        }
        ck_eq((uint32_t)own.planes().path_slot_flag_at(1, 0), 0x7fu,
              "T1: owner-1's slots are NOT touched -- pins the owner index at 0, not a full sweep");
    }

    // T2: the tile grid's top-left 128x128 sub-block is reset -- flags={2,0x40}, building=0,
    // unit={0,0}, class_owner=0, visibility=0 -- and the grid OUTSIDE that sub-block is untouched.
    {
        tact_fixture fx;
        for (auto &t : fx.tile_objects) {
            t.flags[0]    = 0x11;
            t.flags[1]    = 0x22;
            t.building    = 0x3333;
            t.unit[0]     = 0x44;
            t.unit[1]     = 0x55;
            t.class_owner = 0x66;
            t.visibility  = 0x77;
        }
        tact_store      own             = fx.store();
        map_reset_calls c               = mock_calls();
        char            mission_name[8] = "MIS1";
        detail::map_reset(own, c, mission_name);
        auto &t00 = own.planes().tile_object_at(0, 0);
        ck_eq((uint32_t)t00.flags[0], 2u, "T2: tile_object_at(0,0).flags[0] = 2, 0x0042e7af");
        ck_eq((uint32_t)t00.flags[1], 0x40u, "T2: tile_object_at(0,0).flags[1] = 0x40, 0x0042e7af");
        ck_eq((uint32_t)t00.building, 0u, "T2: tile_object_at(0,0).building = 0, 0x0042e7db");
        ck_eq((uint32_t)t00.unit[0], 0u, "T2: tile_object_at(0,0).unit[0] = 0, 0x0042e7f2");
        ck_eq((uint32_t)t00.unit[1], 0u, "T2: tile_object_at(0,0).unit[1] = 0, 0x0042e807");
        ck_eq((uint32_t)t00.class_owner, 0u, "T2: tile_object_at(0,0).class_owner = 0, 0x0042e81c");
        ck_eq((uint32_t)t00.visibility, 0u, "T2: tile_object_at(0,0).visibility = 0, 0x0042e807/0x0042e81c pair");
        auto &t127 = own.planes().tile_object_at(127, 127);
        ck_eq((uint32_t)t127.flags[0], 2u, "T2: tile_object_at(127,127) -- the sub-block's far corner is reset too");
        auto &t128 = own.planes().tile_object_at(128, 0);
        ck_eq((uint32_t)t128.flags[0], 0x11u,
              "T2: tile_object_at(128,0) -- ONE PAST the 128-wide sub-block, must be UNTOUCHED");
        auto &t0_128 = own.planes().tile_object_at(0, 128);
        ck_eq((uint32_t)t0_128.flags[0], 0x11u,
              "T2: tile_object_at(0,128) -- ONE PAST the 128-tall sub-block, must be UNTOUCHED");
    }

    // T3: passable is set to PASSABLE_DEFAULT (2) over the same sub-block, and untouched outside it.
    {
        tact_fixture fx;
        for (auto &p : fx.passable) p = 0x99;
        tact_store      own             = fx.store();
        map_reset_calls c               = mock_calls();
        char            mission_name[8] = "MIS1";
        detail::map_reset(own, c, mission_name);
        ck_eq((uint32_t)own.planes().passable_at(0, 0), (uint32_t)mh::state::PASSABLE_DEFAULT,
              "T3: passable_at(0,0) = PASSABLE_DEFAULT, 0x0042e82c");
        ck_eq((uint32_t)own.planes().passable_at(127, 127), (uint32_t)mh::state::PASSABLE_DEFAULT,
              "T3: passable_at(127,127) -- the sub-block's far corner");
        ck_eq((uint32_t)own.planes().passable_at(128, 0), 0x99u,
              "T3: passable_at(128,0) -- ONE PAST the sub-block, must be UNTOUCHED");
    }

    // T4: the tile-height-sprite cache (8 uint16 slots per cell) is zeroed over the same sub-block,
    // at its real stride (col*1024 + row*8 + k), and untouched outside it.
    {
        tact_fixture fx;
        for (auto &h : fx.tile_height_sprites) h = 0xbeef;
        tact_store      own             = fx.store();
        map_reset_calls c               = mock_calls();
        char            mission_name[8] = "MIS1";
        detail::map_reset(own, c, mission_name);
        const uint16_t *base = fx.tile_height_sprites.data();
        ck_eq((uint32_t)base[0 * 1024 + 0 * 8 + 0], 0u, "T4: height_sprites[col=0,row=0,k=0] zeroed");
        ck_eq((uint32_t)base[0 * 1024 + 0 * 8 + 7], 0u, "T4: height_sprites[col=0,row=0,k=7] zeroed -- all 8 slots");
        ck_eq((uint32_t)base[63 * 1024 + 91 * 8 + 3], 0u,
              "T4: height_sprites[col=63,row=91,k=3] zeroed -- pins the col*1024+row*8+k stride");
        // No "one past the sub-block" check here, unlike T2/T3: the fixture's tile_height_sprites
        // buffer is sized to EXACTLY the 128x128x8 sub-block the loop covers (a real-extent buffer
        // one column wider does not exist offline), so col==128 would read past this vector's own
        // allocation rather than proving anything about the write bound.
    }

    // T5: the mission loader is called exactly once, with the ORIGINAL pointer.
    {
        tact_fixture fx;
        reset_log();
        tact_store      own             = fx.store();
        map_reset_calls c               = mock_calls();
        char            mission_name[8] = "POZ1L";
        detail::map_reset(own, c, mission_name);
        ck_eq((uint32_t)log().mission_load_calls, 1u, "T5: mission_load called exactly once, 0x0042e87a");
        ck((log().mission_load_arg == mission_name), "T5: mission_load receives the SAME pointer, not a copy");
    }
}

} // namespace mh::tact::test
