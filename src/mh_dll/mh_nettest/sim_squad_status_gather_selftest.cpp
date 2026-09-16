//
// sim_squad_status_gather_selftest.cpp -- `simtest` offline oracle for
// llm_strat_bldg_gather_nearby_squad_status @0x0044d468 (949 B)
// (sim/resid/sim_squad_status_gather.h/.cpp, RI-SIM / sim_resid batch E).
//
// NO SHADOW SITE (sim_resid rule 1) -- this offline oracle is the only verification. The sibling in
// the same TU, llm_strat_try_enter_tactical_mission, already has its own oracle
// (sim_try_enter_tactical_mission_selftest.cpp) and drives THIS function for real to reach its gate;
// what that file does NOT cover is this 949-byte body itself, which is what this file is for.
//
// Expected behaviour hand-derived from the DISASSEMBLY
// (tmp/decomp_sim_resid/llm_strat_bldg_gather_nearby_squad_status_0044d468.asm):
//
//   0x0044d49c-d4ec  zero all 64 SQUAD_STATUS slots, four addressed stores each.
//   0x0044d4ee-d53c  x1 = width_mask  & (cfg_buildings[building_id].height / 2 + building.x)
//   0x0044d53f-d58d  y1 = height_mask & (cfg_buildings[building_id].width  / 2 + building.y)
//                    -- the height<->X / width<->Y pairing is SWAPPED relative to
//                    sim_bldg_footprint_random_offset.cpp's convention; transcribed as read, and
//                    pinned by T2 below so the question stays measurable rather than remembered.
//   0x0044d59e-d5af  scan ctrl_groups[0] -- LITERALLY group 0, never scan_player-indexed -- while
//                    i < count AND squad_status_count < 64.
//   0x0044d5ea-d5f1  reject cfg_units[proto].soldier_count <= 0.
//   0x0044d627-d62f  reject tile_dist_wrapped(x1,y1,unit.x,unit.y) >= 15.
//   0x0044d65c-d665  reject the WHOLE unit if count + soldier_count > 64 (all-or-nothing, and the
//                    scan CONTINUES to the next member rather than stopping).
//   0x0044d688-d6bf  energy_pct = max(trunc(unit.energy * UNIT_SCALE / cfg_units[proto].energy), 1).
//   0x0044d6c6-d76a  append soldier_count IDENTICAL slots, one per soldier carried.
//   0x0044d774-d7c8  the building's own pct, with the BLDG scale and cfg_buildings[..].energy.
//   0x0044d7c8-d80f  the five SQUAD_BB_* scalars then SQUAD_STATUS_COUNT; return the count.
//
#include <algorithm> // std::fill over the per-case distance table

#include "sim/resid/sim_squad_status_gather.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- mocks --------------------------------------------------------------------------------------

struct DistCall {
    int32_t x1, y1, x2, y2;
};
std::vector<DistCall> g_dist_calls;
// Distance answers are keyed on the UNIT's tile so a case can place some members inside the radius
// and some outside without depending on a real wrapped-distance implementation.
std::vector<int32_t> g_dist_by_unit_x = std::vector<int32_t>(256, 0);
int32_t              stub_tile_dist_wrapped(int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    g_dist_calls.push_back({x1, y1, x2, y2});
    return g_dist_by_unit_x[(size_t)(x2 & 0xff)];
}

int32_t  g_save_calls = 0;
uint32_t stub_save_planet_to_disk(uint32_t, uint32_t) {
    ++g_save_calls;
    return 1;
}
int32_t g_mission_calls = 0;
void    stub_mission_start() { ++g_mission_calls; }

const squad_status_gather_calls g_calls = {
    &stub_tile_dist_wrapped,
    &stub_save_planet_to_disk,
    &stub_mission_start,
};

void reset_calls() {
    g_dist_calls.clear();
    std::fill(g_dist_by_unit_x.begin(), g_dist_by_unit_x.end(), 0);
    g_save_calls    = 0;
    g_mission_calls = 0;
}

int32_t run(sim_fixture &fx, int32_t scan_player, int32_t bldg_owner, int32_t bldg_idx) {
    sim_store own = fx.store();
    return detail::bldg_gather_nearby_squad_status(fx.view(), own, g_calls, scan_player, bldg_owner, bldg_idx);
}

// Seed one soldier-carrying unit type.
void seed_proto(sim_fixture &fx, uint16_t proto, int32_t soldier_count, double max_energy,
                uint32_t soldier_type) {
    fx.cfg_units[proto].soldier_count = soldier_count;
    fx.cfg_units[proto].energy        = max_energy;
    fx.cfg_units[proto].soldier_type  = soldier_type;
}

// Put unit `slot` of `player` on the map with a proto and an energy, and register it as member
// `member_idx` of control group 0.
void seed_member(sim_fixture &fx, int32_t player, int32_t slot, int32_t member_idx, uint16_t proto,
                 double energy, uint8_t x, uint8_t y) {
    unit &u                                = fx.units[(size_t)(player * UNITS_PER_PLAYER + slot)];
    u.unit_proto_id                        = proto;
    u.energy                               = energy;
    u.x                                    = x;
    u.y                                    = y;
    fx.ctrl_groups[0].unit_ids[member_idx] = (uint16_t)slot;
    if (member_idx + 1 > fx.ctrl_groups[0].count) fx.ctrl_groups[0].count = member_idx + 1;
}

void seed_target_building(sim_fixture &fx, int32_t owner, int32_t idx, uint16_t building_id, uint8_t bx,
                          uint8_t by, uint8_t cfg_height, uint8_t cfg_width, double energy,
                          double cfg_max_energy) {
    building &b                          = fx.buildings[(size_t)(owner * BUILDINGS_PER_PLAYER + idx)];
    b.building_id                        = building_id;
    b.x                                  = bx;
    b.y                                  = by;
    b.energy                             = energy;
    fx.cfg_buildings[building_id].height = cfg_height;
    fx.cfg_buildings[building_id].width  = cfg_width;
    fx.cfg_buildings[building_id].energy = cfg_max_energy;
}

} // namespace

void run_bldg_gather_nearby_squad_status_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- EMPTY SCAN: control group 0 has no members. Every slot must be zeroed, all five SQUAD_BB_*
    // scalars and the count written anyway (the tail at 0x0044d774-d80f runs unconditionally), and the
    // return is 0. Pre-dirtied slots prove the zero-fill loop actually runs.
    // =================================================================================================
    {
        fx.reset();
        reset_calls();
        for (int32_t i = 0; i < SQUAD_STATUS_CAPACITY; ++i) {
            fx.squad_status[(size_t)i].unit_proto_id   = 0x1111;
            fx.squad_status[(size_t)i].energy_pct      = 0x2222;
            fx.squad_status[(size_t)i].unit_slot_index = 0x3333;
            fx.squad_status[(size_t)i].is_commando     = 0x4444;
        }
        fx.geom.width_mask  = 0xff;
        fx.geom.height_mask = 0xff;
        // building.energy 50 of a 200 max, BLDG scale 200.0 -> trunc(50*200/200) = 50.
        seed_target_building(fx, /*owner=*/3, /*idx=*/7, /*building_id=*/9, /*bx=*/10, /*by=*/20,
                             /*cfg_height=*/4, /*cfg_width=*/6, /*energy=*/50.0, /*cfg_max_energy=*/200.0);

        const int32_t ret = run(fx, /*scan_player=*/2, /*bldg_owner=*/3, /*bldg_idx=*/7);

        ck_eq((uint32_t)ret, 0u, "T1: empty group -> count 0");
        bool all_zero = true;
        for (int32_t i = 0; i < SQUAD_STATUS_CAPACITY; ++i) {
            const squad_status_slot &s = fx.squad_status[(size_t)i];
            if (s.unit_proto_id || s.energy_pct || s.unit_slot_index || s.is_commando) all_zero = false;
        }
        ck(all_zero, "T1: all 64 slots zeroed, all four fields, 0x0044d49c-d4ec");
        ck(g_dist_calls.empty(), "T1: tile_dist_wrapped never called with no members");
        ck_eq((uint32_t)fx.squad_bb_target_energy_pct, 50u,
              "T1: SQUAD_BB_TARGET_ENERGY_PCT = trunc(50 * 200.0 / 200.0), the BLDG scale, 0x0044d774-d7c8");
        ck_eq((uint32_t)fx.squad_bb_target_building_id, 9u, "T1: SQUAD_BB_TARGET_BUILDING_ID, 0x0044d7e7");
        ck_eq((uint32_t)fx.squad_bb_target_building_idx, 7u, "T1: SQUAD_BB_TARGET_BUILDING_IDX, 0x0044d7ef");
        ck_eq((uint32_t)fx.squad_bb_scan_player, 2u, "T1: SQUAD_BB_SCAN_PLAYER, 0x0044d7f7");
        ck_eq((uint32_t)fx.squad_bb_target_owner, 3u, "T1: SQUAD_BB_TARGET_OWNER, 0x0044d7ff");
        ck_eq((uint32_t)fx.squad_status_count, 0u, "T1: SQUAD_STATUS_COUNT, 0x0044d807");
    }

    // =================================================================================================
    // T2 -- THE SCAN CENTRE, and the height/width <-> X/Y pairing that the translator flagged.
    //   x1 = width_mask  & (cfg.height / 2 + building.x)
    //   y1 = height_mask & (cfg.width  / 2 + building.y)
    // cfg.height and cfg.width are DIFFERENT (8 vs 20) and the two masks are DIFFERENT (0xff vs 0x3f),
    // so all four of {height<->x, width<->y} and {width_mask<->x, height_mask<->y} are pinned: a
    // swap of either pair changes at least one coordinate.
    //   x1 = 0xff & (8/2  + 100) = 104
    //   y1 = 0x3f & (20/2 + 200) = 210 & 0x3f = 18
    // The centre is observed through the arguments the distance mock records.
    // =================================================================================================
    {
        fx.reset();
        reset_calls();
        fx.geom.width_mask  = 0xff;
        fx.geom.height_mask = 0x3f;
        seed_target_building(fx, /*owner=*/1, /*idx=*/2, /*building_id=*/5, /*bx=*/100, /*by=*/200,
                             /*cfg_height=*/8, /*cfg_width=*/20, /*energy=*/10.0, /*cfg_max_energy=*/100.0);
        seed_proto(fx, /*proto=*/4, /*soldier_count=*/1, /*max_energy=*/100.0, /*soldier_type=*/0);
        seed_member(fx, /*player=*/2, /*slot=*/11, /*member_idx=*/0, /*proto=*/4, /*energy=*/50.0,
                    /*x=*/70, /*y=*/71);
        g_dist_by_unit_x[70] = 99; // out of radius -- this case only measures the CENTRE arguments

        run(fx, /*scan_player=*/2, /*bldg_owner=*/1, /*bldg_idx=*/2);

        ck(g_dist_calls.size() == 1, "T2: tile_dist_wrapped called once per soldier-carrying member, 0x0044d627");
        if (g_dist_calls.size() == 1) {
            ck_eq((uint32_t)g_dist_calls[0].x1, 104u,
                  "T2: x1 = width_mask & (cfg.HEIGHT/2 + building.x) = 0xff & (4+100), 0x0044d4ee-d53c");
            ck_eq((uint32_t)g_dist_calls[0].y1, 18u,
                  "T2: y1 = height_mask & (cfg.WIDTH/2 + building.y) = 0x3f & (10+200), 0x0044d53f-d58d");
            ck_eq((uint32_t)g_dist_calls[0].x2, 70u, "T2: arg 3 is the member unit's x");
            ck_eq((uint32_t)g_dist_calls[0].y2, 71u, "T2: arg 4 is the member unit's y");
        }
    }

    // =================================================================================================
    // T3 -- THE THREE REJECTIONS, each with a neighbouring ACCEPT so the gate is shown to be a gate
    // rather than a blanket refusal:
    //   member 0: soldier_count == 0   -> rejected before the distance call (no call recorded for it)
    //   member 1: soldier_count == -1  -> same (the test is <= 0, not == 0)
    //   member 2: distance == 15       -> rejected, the boundary a `<=` mutation flips
    //   member 3: distance == 14       -> ACCEPTED, the other side of that same boundary
    // =================================================================================================
    {
        fx.reset();
        reset_calls();
        fx.geom.width_mask  = 0xff;
        fx.geom.height_mask = 0xff;
        seed_target_building(fx, 0, 0, /*building_id=*/1, 0, 0, 0, 0, /*energy=*/1.0, /*cfg_max_energy=*/1.0);
        seed_proto(fx, /*proto=*/1, /*soldier_count=*/0, 100.0, 0);
        seed_proto(fx, /*proto=*/2, /*soldier_count=*/-1, 100.0, 0);
        seed_proto(fx, /*proto=*/3, /*soldier_count=*/1, 100.0, 0);
        seed_member(fx, 0, /*slot=*/10, /*member_idx=*/0, /*proto=*/1, 100.0, /*x=*/50, 0);
        seed_member(fx, 0, /*slot=*/11, /*member_idx=*/1, /*proto=*/2, 100.0, /*x=*/51, 0);
        seed_member(fx, 0, /*slot=*/12, /*member_idx=*/2, /*proto=*/3, 100.0, /*x=*/52, 0);
        seed_member(fx, 0, /*slot=*/13, /*member_idx=*/3, /*proto=*/3, 100.0, /*x=*/53, 0);
        g_dist_by_unit_x[52] = SQUAD_SCAN_RADIUS_TILES;     // 15 -- rejected
        g_dist_by_unit_x[53] = SQUAD_SCAN_RADIUS_TILES - 1; // 14 -- accepted

        const int32_t ret = run(fx, /*scan_player=*/0, /*bldg_owner=*/0, /*bldg_idx=*/0);

        ck_eq((uint32_t)ret, 1u, "T3: exactly one member survives both gates");
        ck(g_dist_calls.size() == 2,
           "T3: the two soldier_count<=0 members never reach the distance call, 0x0044d5ea-d5f1");
        ck_eq((uint32_t)fx.squad_status[0].unit_slot_index, 13u,
              "T3: the accepted member is the dist==14 one -- the radius test is `>= 15`, 0x0044d62f");
        ck_eq((uint32_t)fx.squad_status[1].unit_slot_index, 0u, "T3: no second slot filled");
    }

    // =================================================================================================
    // T4 -- ONE SLOT PER SOLDIER CARRIED, all four fields identical across the run, plus is_commando.
    // Two members: proto 5 carries 3 soldiers and is a COMMANDO type (soldier_type == 2); proto 6
    // carries 2 and is not. Slot order and per-slot content are asserted individually.
    //
    // energy_pct for member A: trunc(60 * 100.0 / 80.0) = trunc(75.0) = 75  (UNIT scale is 100.0)
    // energy_pct for member B: trunc(10 * 100.0 / 400.0) = trunc(2.5) = 2
    // =================================================================================================
    {
        fx.reset();
        reset_calls();
        fx.geom.width_mask  = 0xff;
        fx.geom.height_mask = 0xff;
        seed_target_building(fx, 0, 0, 1, 0, 0, 0, 0, 1.0, 1.0);
        seed_proto(fx, /*proto=*/5, /*soldier_count=*/3, /*max_energy=*/80.0,
                   /*soldier_type=*/SOLDIER_TYPE_COMMANDO);
        seed_proto(fx, /*proto=*/6, /*soldier_count=*/2, /*max_energy=*/400.0, /*soldier_type=*/1);
        seed_member(fx, 0, /*slot=*/21, /*member_idx=*/0, /*proto=*/5, /*energy=*/60.0, /*x=*/40, 0);
        seed_member(fx, 0, /*slot=*/22, /*member_idx=*/1, /*proto=*/6, /*energy=*/10.0, /*x=*/41, 0);

        const int32_t ret = run(fx, 0, 0, 0);

        ck_eq((uint32_t)ret, 5u, "T4: 3 + 2 soldiers -> 5 slots, 0x0044d6c6-d76a");
        ck_eq((uint32_t)fx.squad_status_count, 5u, "T4: SQUAD_STATUS_COUNT matches the return");
        for (int32_t i = 0; i < 3; ++i) {
            ck_eq((uint32_t)fx.squad_status[(size_t)i].unit_proto_id, 5u, "T4: member A slots carry proto 5");
            ck_eq((uint32_t)fx.squad_status[(size_t)i].unit_slot_index, 21u,
                  "T4: member A slots carry the unit's ROSTER slot, not the group member index");
            ck_eq((uint32_t)fx.squad_status[(size_t)i].energy_pct, 75u,
                  "T4: member A energy_pct = trunc(60 * 100.0 / 80.0), the UNIT scale, 0x0044d688-d6bf");
            ck_eq((uint32_t)fx.squad_status[(size_t)i].is_commando, 1u,
                  "T4: soldier_type == 2 -> is_commando 1, 0x0044d739-d75a");
        }
        for (int32_t i = 3; i < 5; ++i) {
            ck_eq((uint32_t)fx.squad_status[(size_t)i].unit_proto_id, 6u, "T4: member B slots carry proto 6");
            ck_eq((uint32_t)fx.squad_status[(size_t)i].unit_slot_index, 22u, "T4: member B slot index");
            ck_eq((uint32_t)fx.squad_status[(size_t)i].energy_pct, 2u,
                  "T4: member B energy_pct = trunc(10 * 100.0 / 400.0) = 2 -- truncated, not rounded");
            ck_eq((uint32_t)fx.squad_status[(size_t)i].is_commando, 0u, "T4: soldier_type != 2 -> is_commando 0");
        }
        ck_eq((uint32_t)fx.squad_status[5].unit_proto_id, 0u, "T4: slot 5 untouched -- exactly 5 were filled");
    }

    // =================================================================================================
    // T5 -- the energy_pct FLOOR AT 1 (0x0044d6b2-d6be / 0x0044d7bb-d7c7), on both the unit and the
    // building path. A unit at 0.4% and a building at 0.0 both report 1, not 0 -- and a NEGATIVE
    // energy does too, which is what separates the floor from a max(...,0) or an abs().
    // =================================================================================================
    {
        fx.reset();
        reset_calls();
        fx.geom.width_mask  = 0xff;
        fx.geom.height_mask = 0xff;
        // building energy 0 -> trunc(0 * 200 / 100) = 0 -> floored to 1
        seed_target_building(fx, 0, 0, 1, 0, 0, 0, 0, /*energy=*/0.0, /*cfg_max_energy=*/100.0);
        seed_proto(fx, /*proto=*/7, /*soldier_count=*/1, /*max_energy=*/1000.0, /*soldier_type=*/0);
        seed_proto(fx, /*proto=*/8, /*soldier_count=*/1, /*max_energy=*/1000.0, /*soldier_type=*/0);
        seed_member(fx, 0, /*slot=*/31, /*member_idx=*/0, /*proto=*/7, /*energy=*/4.0, /*x=*/60, 0);
        seed_member(fx, 0, /*slot=*/32, /*member_idx=*/1, /*proto=*/8, /*energy=*/-500.0, /*x=*/61, 0);

        run(fx, 0, 0, 0);

        ck_eq((uint32_t)fx.squad_status[0].energy_pct, 1u,
              "T5: trunc(4 * 100.0 / 1000.0) = 0 -> floored to 1, 0x0044d6b2-d6be");
        ck_eq((uint32_t)fx.squad_status[1].energy_pct, 1u,
              "T5: a NEGATIVE energy also floors to 1 -- the test is `pct <= 0`, not `pct == 0`");
        ck_eq((uint32_t)fx.squad_bb_target_energy_pct, 1u,
              "T5: the BUILDING pct floors the same way, 0x0044d7bb-d7c7");
    }

    // =================================================================================================
    // T6 -- THE CAPACITY GUARD IS ALL-OR-NOTHING AND DOES NOT STOP THE SCAN (0x0044d65c-d665).
    // Member 0 carries 60 soldiers (fits: 0 + 60 <= 64). Member 1 carries 10 (60 + 10 = 70 > 64 --
    // the WHOLE unit is skipped, no partial fill of the 4 remaining slots). Member 2 carries 4
    // (60 + 4 = 64, exactly at the limit -- ACCEPTED, the boundary a `>=` mutation flips), and its
    // acceptance is also what proves the scan continued past the rejected member rather than breaking.
    // =================================================================================================
    {
        fx.reset();
        reset_calls();
        fx.geom.width_mask  = 0xff;
        fx.geom.height_mask = 0xff;
        seed_target_building(fx, 0, 0, 1, 0, 0, 0, 0, 1.0, 1.0);
        seed_proto(fx, /*proto=*/9, /*soldier_count=*/60, 100.0, 0);
        seed_proto(fx, /*proto=*/10, /*soldier_count=*/10, 100.0, 0);
        seed_proto(fx, /*proto=*/11, /*soldier_count=*/4, 100.0, 0);
        seed_member(fx, 0, /*slot=*/41, /*member_idx=*/0, /*proto=*/9, 100.0, /*x=*/80, 0);
        seed_member(fx, 0, /*slot=*/42, /*member_idx=*/1, /*proto=*/10, 100.0, /*x=*/81, 0);
        seed_member(fx, 0, /*slot=*/43, /*member_idx=*/2, /*proto=*/11, 100.0, /*x=*/82, 0);

        const int32_t ret = run(fx, 0, 0, 0);

        ck_eq((uint32_t)ret, 64u, "T6: 60 + (skipped 10) + 4 = 64 -- the guard is `count + n > 64`, 0x0044d665");
        for (int32_t i = 0; i < 60; ++i) {
            ck(fx.squad_status[(size_t)i].unit_slot_index == 41, "T6: slots 0..59 belong to the 60-soldier member");
        }
        for (int32_t i = 60; i < 64; ++i) {
            ck(fx.squad_status[(size_t)i].unit_slot_index == 43,
               "T6: slots 60..63 belong to the LAST member -- the oversized one was skipped whole, "
               "and the scan did not stop at it");
        }
        ck_eq((uint32_t)fx.squad_status_count, 64u, "T6: SQUAD_STATUS_COUNT reaches the capacity exactly");
    }

    // =================================================================================================
    // T7 -- CONTROL GROUP 0 IS LITERAL (0x0044d5a1), and the units come from SCAN_PLAYER's roster.
    // Group 3 is seeded with a decoy member that would be accepted if the group index followed
    // scan_player; player 5's roster slot is left empty while player 2's carries the real unit, so
    // indexing the wrong player's roster yields proto 0 (soldier_count 0) and a count of 0.
    // =================================================================================================
    {
        fx.reset();
        reset_calls();
        fx.geom.width_mask  = 0xff;
        fx.geom.height_mask = 0xff;
        seed_target_building(fx, 0, 0, 1, 0, 0, 0, 0, 1.0, 1.0);
        seed_proto(fx, /*proto=*/12, /*soldier_count=*/2, 100.0, 0);
        // The real member: group 0, scan_player 3's roster.
        seed_member(fx, /*player=*/3, /*slot=*/50, /*member_idx=*/0, /*proto=*/12, 100.0, /*x=*/90, 0);
        // Decoy in group 3 -- must never be scanned.
        fx.ctrl_groups[3].count       = 1;
        fx.ctrl_groups[3].unit_ids[0] = 51;
        unit &decoy                   = fx.units[(size_t)(3 * UNITS_PER_PLAYER + 51)];
        decoy.unit_proto_id           = 12;
        decoy.energy                  = 100.0;
        decoy.x                       = 91;

        const int32_t ret = run(fx, /*scan_player=*/3, /*bldg_owner=*/0, /*bldg_idx=*/0);

        ck_eq((uint32_t)ret, 2u, "T7: only control group 0 is scanned -- 2 soldiers from its one member");
        ck_eq((uint32_t)fx.squad_status[0].unit_slot_index, 50u, "T7: the member came from group 0");
        ck_eq((uint32_t)fx.squad_status[1].unit_slot_index, 50u, "T7: ... both its slots");
        ck_eq((uint32_t)fx.squad_status[2].unit_proto_id, 0u, "T7: group 3's decoy contributed nothing");
    }

    // =================================================================================================
    // T8 -- the ROSTER a member index resolves against is scan_player's, and the TARGET BUILDING is
    // bldg_owner's. Both are non-zero and DIFFERENT, and a decoy unit sits at the same roster slot in
    // the other player's rows, so swapping the two parameters changes the result.
    // =================================================================================================
    {
        fx.reset();
        reset_calls();
        fx.geom.width_mask  = 0xff;
        fx.geom.height_mask = 0xff;
        // The real target: owner 4, idx 6, building_id 3, energy 30 of 300 -> trunc(30*200/300) = 20.
        seed_target_building(fx, /*owner=*/4, /*idx=*/6, /*building_id=*/3, /*bx=*/0, /*by=*/0,
                             /*cfg_height=*/0, /*cfg_width=*/0, /*energy=*/30.0, /*cfg_max_energy=*/300.0);
        // Decoy building at the swapped (owner,idx): a different id and a different energy.
        seed_target_building(fx, /*owner=*/6, /*idx=*/4, /*building_id=*/8, /*bx=*/0, /*by=*/0,
                             /*cfg_height=*/0, /*cfg_width=*/0, /*energy=*/300.0, /*cfg_max_energy=*/300.0);
        seed_proto(fx, /*proto=*/13, /*soldier_count=*/1, /*max_energy=*/100.0, /*soldier_type=*/0);
        seed_member(fx, /*player=*/2, /*slot=*/60, /*member_idx=*/0, /*proto=*/13, /*energy=*/25.0, /*x=*/95, 0);
        // Decoy unit at player 4's slot 60 -- what a scan_player/bldg_owner swap would read.
        unit &decoy         = fx.units[(size_t)(4 * UNITS_PER_PLAYER + 60)];
        decoy.unit_proto_id = 13;
        decoy.energy        = 100.0;
        decoy.x             = 95;

        const int32_t ret = run(fx, /*scan_player=*/2, /*bldg_owner=*/4, /*bldg_idx=*/6);

        ck_eq((uint32_t)ret, 1u, "T8: one soldier gathered");
        ck_eq((uint32_t)fx.squad_status[0].energy_pct, 25u,
              "T8: energy_pct = trunc(25 * 100.0 / 100.0) -- SCAN_PLAYER 2's unit, not bldg_owner 4's decoy");
        ck_eq((uint32_t)fx.squad_bb_target_energy_pct, 20u,
              "T8: the target pct = trunc(30 * 200.0 / 300.0) -- buildings[owner=4][idx=6], not the swap");
        ck_eq((uint32_t)fx.squad_bb_target_building_id, 3u, "T8: SQUAD_BB_TARGET_BUILDING_ID is the real target's");
    }
}

} // namespace mh::sim::test
