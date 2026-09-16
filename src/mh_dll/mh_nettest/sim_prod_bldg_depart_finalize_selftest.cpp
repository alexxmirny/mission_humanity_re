#include "sim/sim_prod_bldg_depart_finalize.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// Distinct, non-default, non-symmetric across every axis (player/building_index/shuttle_slot/
// dest_planet/building_id) so a translation that swapped two of them would disagree with the fixture.
constexpr uint16_t PLAYER         = 5;
constexpr int32_t  BUILDING_INDEX = 37;
constexpr int32_t  SHUTTLE_SLOT   = 6; // 0..9 (PROD_SHUTTLE_SLOTS_PER_PLAYER)
constexpr int32_t  DEST_PLANET    = 21;
constexpr int32_t  SRC_PLANET     = 3;  // == G_PLANET_INDEX for these tests
constexpr uint16_t BUILDING_ID    = 41; // the cfg::final::data::Building[] index the building row holds

prod_shuttle_slot &slot_of(sim_fixture &f, uint32_t player, int32_t slot) {
    return f.prod_shuttle_slots[player * PROD_SHUTTLE_SLOTS_PER_PLAYER + (uint32_t)slot];
}

// ---- the recorder ----------------------------------------------------------------------------------
// One file-scope recorder feeds the single `_calls` stub table, same shape as sim_prod_completion_
// selftest.cpp's g_pc/g_da/g_sa. Field order mirrors prod_bldg_depart_finalize_calls exactly.
struct unbind_call {
    int32_t player, planet_slot;
};

struct pf_recorder {
    // planet_distance(src_x, src_y, dst_x, dst_y)
    int32_t  planet_distance_n = 0;
    uint32_t pd_src_x = 0, pd_src_y = 0, pd_dst_x = 0, pd_dst_y = 0;
    double   planet_distance_ret = 0.0;

    // planet_distance_factor(src_planet, dest_planet)
    int32_t df_src_planet = 0, df_dest_planet = 0;
    int32_t distance_factor_n   = 0;
    double  distance_factor_ret = 0.0;

    // shuttle_fuel_check(player, building_index, dest_planet)
    int32_t  fuel_check_n        = 0;
    uint16_t fuel_player         = 0;
    int32_t  fuel_building_index = 0;
    int32_t  fuel_dest_planet    = 0;
    int32_t  fuel_check_ret      = 0; // 0 = fuel OK

    // unit_add_docked(equivalent, player, sub_id)
    int32_t  add_docked_n          = 0;
    uint32_t add_docked_equivalent = 0;
    uint16_t add_docked_player     = 0;
    uint32_t add_docked_sub_id     = 0;
    int32_t  add_docked_ret        = 1; // nonzero = success

    // storage_launch_parked_to_orbit(player, building_index)
    int32_t  launch_n              = 0;
    uint16_t launch_player         = 0;
    int32_t  launch_building_index = 0;
    int32_t  launch_ret            = 0; // new_unit_slot; 0 = no unit launched

    // prod_unbind_planet(player, planet_slot) -- both call sites append here.
    std::vector<unbind_call> unbind_calls;

    void reset() { *this = pf_recorder{}; }
};
pf_recorder g_pf;

const prod_bldg_depart_finalize_calls &rec_pf_calls() {
    static const prod_bldg_depart_finalize_calls c = {
        [](int32_t src_x, int32_t src_y, int32_t dst_x, int32_t dst_y) -> double {
            g_pf.planet_distance_n++;
            g_pf.pd_src_x = src_x;
            g_pf.pd_src_y = src_y;
            g_pf.pd_dst_x = dst_x;
            g_pf.pd_dst_y = dst_y;
            return g_pf.planet_distance_ret;
        },
        [](int32_t src_planet, int32_t dest_planet) -> double {
            g_pf.distance_factor_n++;
            g_pf.df_src_planet  = src_planet;
            g_pf.df_dest_planet = dest_planet;
            return g_pf.distance_factor_ret;
        },
        [](uint16_t player, int32_t building_index, int32_t dest_planet) -> int32_t {
            g_pf.fuel_check_n++;
            g_pf.fuel_player         = player;
            g_pf.fuel_building_index = building_index;
            g_pf.fuel_dest_planet    = dest_planet;
            return g_pf.fuel_check_ret;
        },
        [](uint32_t unit_proto_id, uint16_t player, uint32_t probe_slot) -> int32_t {
            g_pf.add_docked_n++;
            g_pf.add_docked_equivalent = unit_proto_id;
            g_pf.add_docked_player     = player;
            g_pf.add_docked_sub_id     = probe_slot;
            return g_pf.add_docked_ret;
        },
        [](uint16_t player, int32_t building_index) -> int32_t {
            g_pf.launch_n++;
            g_pf.launch_player         = player;
            g_pf.launch_building_index = building_index;
            return g_pf.launch_ret;
        },
        [](int32_t player, int32_t planet_slot) { g_pf.unbind_calls.push_back({player, planet_slot}); },
    };
    return c;
}

// ==== THE HEAD: single building read, the distance call, the always-written slot fields ============
// 0x0048ec67-0x0048ed5e: shuttle_slot/building_id from ONE building read; distance = planet_distance(
// src coords, dst coords); slot.dest_planet/.travel_duration/.travel_duration_copy ALWAYS written,
// before the fuel gate.

void test_head_distance_call_uses_planet_index_and_dest_planet_coords() {
    sim_fixture f;
    g_pf.reset();
    building &bldg                                  = f.b(PLAYER, BUILDING_INDEX);
    bldg.shuttle_slot                               = (uint8_t)SHUTTLE_SLOT;
    bldg.building_id                                = BUILDING_ID;
    f.planet_index                                  = SRC_PLANET;
    f.cfg_planets[(size_t)SRC_PLANET].coordinate_x  = 1001;
    f.cfg_planets[(size_t)SRC_PLANET].coordinate_y  = 2002;
    f.cfg_planets[(size_t)DEST_PLANET].coordinate_x = 3003;
    f.cfg_planets[(size_t)DEST_PLANET].coordinate_y = 4004;
    g_pf.planet_distance_ret                        = 50.0;
    g_pf.fuel_check_ret                             = 1; // fail -- keeps this case isolated to the head/distance call

    sim_view      v   = f.view();
    sim_store     own = f.store();
    const int32_t r =
        detail::prod_bldg_depart_finalize(v, own, rec_pf_calls(), PLAYER, BUILDING_INDEX, DEST_PLANET);

    ck_eq((uint32_t)g_pf.planet_distance_n, 1u, "pf: planet_distance called exactly once");
    ck_eq(g_pf.pd_src_x, 1001u, "pf: planet_distance src_x = cfg_planets[G_PLANET_INDEX].coordinate_x");
    ck_eq(g_pf.pd_src_y, 2002u, "pf: planet_distance src_y = cfg_planets[G_PLANET_INDEX].coordinate_y");
    ck_eq(g_pf.pd_dst_x, 3003u, "pf: planet_distance dst_x = cfg_planets[dest_planet].coordinate_x");
    ck_eq(g_pf.pd_dst_y, 4004u, "pf: planet_distance dst_y = cfg_planets[dest_planet].coordinate_y");
    ck(r == -1, "pf: fuel check failing -> return -1");
}

void test_travel_duration_formula_is_distance_factor_times_velocity_times_distance() {
    sim_fixture f;
    g_pf.reset();
    building &bldg                        = f.b(PLAYER, BUILDING_INDEX);
    bldg.shuttle_slot                     = (uint8_t)SHUTTLE_SLOT;
    bldg.building_id                      = BUILDING_ID;
    f.cfg_buildings[BUILDING_ID].velocity = 3.5;
    f.cfg_buildings[BUILDING_ID].type     = 0x55; // unrecognized -> no-op dispatch, keep focus on the math
    f.planet_index                        = SRC_PLANET;
    g_pf.planet_distance_ret              = 12.0;
    g_pf.distance_factor_ret              = 0.25;
    g_pf.fuel_check_ret                   = 0; // pass, so the tail also runs cleanly

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::prod_bldg_depart_finalize(v, own, rec_pf_calls(), PLAYER, BUILDING_INDEX, DEST_PLANET);

    // vel_x_dist = velocity * distance = 3.5 * 12.0 = 42.0 (computed FIRST, per the asm's FMUL order);
    // travel_duration = distance_factor * vel_x_dist = 0.25 * 42.0 = 10.5 -- NOT
    // (distance_factor*velocity)*distance.
    ck_eq((uint32_t)g_pf.distance_factor_n, 1u, "pf: planet_distance_factor called exactly once");
    ck_eq((uint32_t)g_pf.df_src_planet, (uint32_t)SRC_PLANET, "pf: distance_factor src = G_PLANET_INDEX");
    ck_eq((uint32_t)g_pf.df_dest_planet, (uint32_t)DEST_PLANET, "pf: distance_factor dest = dest_planet arg");
    ck_eq_d(slot_of(f, PLAYER, SHUTTLE_SLOT).travel_duration, 10.5,
            "pf: travel_duration = distance_factor * (velocity * distance) = 0.25*(3.5*12.0) = 10.5, "
            "grouped exactly as the FMUL order in the asm");
}

void test_travel_duration_copy_reloads_from_travel_duration() {
    sim_fixture f;
    g_pf.reset();
    building &bldg                        = f.b(PLAYER, BUILDING_INDEX);
    bldg.shuttle_slot                     = (uint8_t)SHUTTLE_SLOT;
    bldg.building_id                      = BUILDING_ID;
    f.cfg_buildings[BUILDING_ID].velocity = 2.0;
    f.cfg_buildings[BUILDING_ID].type     = 0x55;
    g_pf.planet_distance_ret              = 9.0;
    g_pf.distance_factor_ret              = 1.5;
    g_pf.fuel_check_ret                   = 0;

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::prod_bldg_depart_finalize(v, own, rec_pf_calls(), PLAYER, BUILDING_INDEX, DEST_PLANET);

    prod_shuttle_slot &s = slot_of(f, PLAYER, SHUTTLE_SLOT);
    ck_eq_d(s.travel_duration, 27.0, "pf: travel_duration = 1.5 * (2.0 * 9.0) = 27.0");
    ck_eq_d(s.travel_duration_copy, s.travel_duration,
            "pf: travel_duration_copy is a genuine reload from the just-written travel_duration field "
            "(FLD then FSTP), not an independent recompute -- must equal it exactly");
}

void test_slot_fields_written_even_when_fuel_check_fails() {
    sim_fixture f;
    g_pf.reset();
    building &bldg                        = f.b(PLAYER, BUILDING_INDEX);
    bldg.shuttle_slot                     = (uint8_t)SHUTTLE_SLOT;
    bldg.building_id                      = BUILDING_ID;
    bldg.state                            = 0x11; // sentinel -- must stay untouched, dispatch never runs
    f.cfg_buildings[BUILDING_ID].velocity = 4.0;
    g_pf.planet_distance_ret              = 5.0;
    g_pf.distance_factor_ret              = 2.0;
    g_pf.fuel_check_ret                   = 1; // FAILS

    sim_view      v   = f.view();
    sim_store     own = f.store();
    const int32_t r =
        detail::prod_bldg_depart_finalize(v, own, rec_pf_calls(), PLAYER, BUILDING_INDEX, DEST_PLANET);

    ck(r == -1, "pf: fuel check fails -> return -1");
    prod_shuttle_slot &s = slot_of(f, PLAYER, SHUTTLE_SLOT);
    ck_eq((uint32_t)(uint16_t)s.dest_planet, (uint32_t)(uint16_t)DEST_PLANET,
          "pf: dest_planet stays written on a failed departure -- the ETA/dest commit is NOT "
          "transactional with the fuel gate");
    ck_eq_d(s.travel_duration, 40.0, "pf: travel_duration (2.0*(4.0*5.0)=40.0) stays written on fuel failure");
    ck_eq_d(s.travel_duration_copy, 40.0, "pf: travel_duration_copy stays written on fuel failure");
    ck_eq((uint32_t)f.b(PLAYER, BUILDING_INDEX).state, 0x11u,
          "pf: fuel failure bails BEFORE the type dispatch -- building.state untouched");
    ck_eq((uint32_t)g_pf.add_docked_n, 0u, "pf: fuel failure -> dispatch never runs -> no add_docked call");
    ck_eq((uint32_t)g_pf.unbind_calls.size(), 0u, "pf: fuel failure -> no unbind calls either");
}

void test_slot_fields_persist_through_a_normal_pass_and_dispatch() {
    sim_fixture f;
    g_pf.reset();
    building &bldg                        = f.b(PLAYER, BUILDING_INDEX);
    bldg.shuttle_slot                     = (uint8_t)SHUTTLE_SLOT;
    bldg.building_id                      = BUILDING_ID;
    f.cfg_buildings[BUILDING_ID].velocity = 6.0;
    f.cfg_buildings[BUILDING_ID].type     = 0x77; // unrecognized -> no-op dispatch tail
    g_pf.planet_distance_ret              = 4.0;
    g_pf.distance_factor_ret              = 0.5;
    g_pf.fuel_check_ret                   = 0; // PASSES this time

    sim_view      v   = f.view();
    sim_store     own = f.store();
    const int32_t r =
        detail::prod_bldg_depart_finalize(v, own, rec_pf_calls(), PLAYER, BUILDING_INDEX, DEST_PLANET);

    ck(r == 1, "pf: fuel passes, unrecognized type -> no-op tail returns 1");
    prod_shuttle_slot &s = slot_of(f, PLAYER, SHUTTLE_SLOT);
    ck_eq((uint32_t)(uint16_t)s.dest_planet, (uint32_t)(uint16_t)DEST_PLANET,
          "pf: dest_planet from the head survives a full pass+no-op-dispatch run");
    ck_eq_d(s.travel_duration, 12.0, "pf: travel_duration (0.5*(6.0*4.0)=12.0) survives too");
    ck_eq_d(s.travel_duration_copy, 12.0, "pf: travel_duration_copy survives too");
}

// ==== THE DEAD BRANCH (0x0048ed64-0x0048ed99) ========================================================
// The inner guard (`local_40 = 0; if (local_40 > 0)`) is a Watcom fake-branch idiom that ALWAYS skips
// -- permanently dead code. The outer guard (G_PLANET_INDEX > 4) IS reachable; this proves that
// reaching it changes nothing about travel_duration.

void test_dead_branch_planet_index_over_4_does_not_multiply_duration() {
    sim_fixture f;
    g_pf.reset();
    building &bldg                        = f.b(PLAYER, BUILDING_INDEX);
    bldg.shuttle_slot                     = (uint8_t)SHUTTLE_SLOT;
    bldg.building_id                      = BUILDING_ID;
    f.cfg_buildings[BUILDING_ID].velocity = 1.0;
    f.cfg_buildings[BUILDING_ID].type     = 0x55;
    f.planet_index                        = 9; // > 4 -- the OUTER guard is real and reachable
    g_pf.planet_distance_ret              = 3.0;
    g_pf.distance_factor_ret              = 2.0;
    g_pf.fuel_check_ret                   = 0;

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::prod_bldg_depart_finalize(v, own, rec_pf_calls(), PLAYER, BUILDING_INDEX, DEST_PLANET);

    ck_eq_d(slot_of(f, PLAYER, SHUTTLE_SLOT).travel_duration, 6.0,
            "pf: planet_index>4 enters the outer dead-branch guard, but the inner guard is permanently "
            "false (Watcom fake-branch idiom) -- travel_duration stays the plain formula result "
            "(2.0*(1.0*3.0)=6.0), never multiplied by shuttle_duration_mult_dead_branch");
}

// ==== THE FUEL RE-CHECK (0x0048ed9f-0x0048ede3) ======================================================

void test_fuel_check_called_with_player_building_index_dest_planet() {
    sim_fixture f;
    g_pf.reset();
    building &bldg                        = f.b(PLAYER, BUILDING_INDEX);
    bldg.shuttle_slot                     = (uint8_t)SHUTTLE_SLOT;
    bldg.building_id                      = BUILDING_ID;
    f.cfg_buildings[BUILDING_ID].velocity = 1.0;
    g_pf.fuel_check_ret                   = 1; // fail, don't care about dispatch here

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::prod_bldg_depart_finalize(v, own, rec_pf_calls(), PLAYER, BUILDING_INDEX, DEST_PLANET);

    ck_eq((uint32_t)g_pf.fuel_check_n, 1u, "pf: shuttle_fuel_check called exactly once");
    ck_eq((uint32_t)g_pf.fuel_player, (uint32_t)PLAYER, "pf: fuel check player arg");
    ck_eq((uint32_t)g_pf.fuel_building_index, (uint32_t)BUILDING_INDEX, "pf: fuel check building_index arg");
    ck_eq((uint32_t)g_pf.fuel_dest_planet, (uint32_t)DEST_PLANET, "pf: fuel check dest_planet arg");
}

// ==== THE TYPE DISPATCH (0x0048edec-0x0048efba) ======================================================
// {A_PORT(0xc), H_PORT(0x20)} -> dock/launch (LAB_0048ee57); {A_MOTHER(0x6), A_SHUTTLE(0xd),
// H_MOTHER(0x1a), H_SHUTTLE(0x21)} -> "departed" (LAB_0048ee36); everything else -> silent no-op.

void test_dispatch_port_add_docked_call_args() {
    sim_fixture f;
    g_pf.reset();
    building &bldg                          = f.b(PLAYER, BUILDING_INDEX);
    bldg.shuttle_slot                       = (uint8_t)SHUTTLE_SLOT;
    bldg.building_id                        = BUILDING_ID;
    bldg.sub_id                             = 23;
    f.cfg_buildings[BUILDING_ID].type       = BUILDING_TYPE_H_PORT;
    f.cfg_buildings[BUILDING_ID].equivalent = 91;
    g_pf.fuel_check_ret                     = 0;
    g_pf.add_docked_ret                     = 1; // succeed, but launch also fails so the test stays cheap
    g_pf.launch_ret                         = 0;

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::prod_bldg_depart_finalize(v, own, rec_pf_calls(), PLAYER, BUILDING_INDEX, DEST_PLANET);

    ck_eq((uint32_t)g_pf.add_docked_n, 1u, "pf: unit_add_docked called exactly once");
    ck_eq((uint32_t)g_pf.add_docked_equivalent, 91u,
          "pf: unit_add_docked's 1st arg = cfg_buildings[building_id].equivalent");
    ck_eq((uint32_t)g_pf.add_docked_player, (uint32_t)PLAYER, "pf: unit_add_docked's 2nd arg = player");
    ck_eq((uint32_t)g_pf.add_docked_sub_id, 23u,
          "pf: unit_add_docked's 3rd arg = building.sub_id (a fresh read, per the header's third "
          "independent building_id/sub_id read note)");
}

void test_dispatch_aport_add_docked_fails_unbinds_two_arg_undoes_header_row() {
    sim_fixture f;
    g_pf.reset();
    building &bldg                    = f.b(PLAYER, BUILDING_INDEX);
    bldg.shuttle_slot                 = (uint8_t)SHUTTLE_SLOT;
    bldg.building_id                  = BUILDING_ID;
    f.cfg_buildings[BUILDING_ID].type = BUILDING_TYPE_A_PORT;
    f.planet_index                    = SRC_PLANET; // distinct from PLAYER/BUILDING_INDEX/SHUTTLE_SLOT -- catches a
                                                    // swapped-argument regression at the unbind call site
    g_pf.fuel_check_ret = 0;
    g_pf.add_docked_ret = 0; // FAILS

    unit &header_row = f.u(PLAYER, 0);
    header_row.order = 200; // non-default, non-zero baseline so INC-then-DEC is actually observable

    sim_view      v   = f.view();
    sim_store     own = f.store();
    const int32_t r =
        detail::prod_bldg_depart_finalize(v, own, rec_pf_calls(), PLAYER, BUILDING_INDEX, DEST_PLANET);

    ck(r == -1, "pf: add_docked==0 -> return -1 (this IS treated as an error)");
    // THE PHANTOM-ARGUMENT HAZARD, call site 1 (0x0048eebf, per the header banner): the raw opcodes
    // load only EAX=player/EDX=G_PLANET_INDEX before this CALL, so llm_strat_prod_unbind_planet is a
    // 2-arg call here -- no shuttle_slot/building_index leaks in as a phantom 3rd arg.
    ck(g_pf.unbind_calls.size() == 1 && g_pf.unbind_calls[0].player == (int32_t)PLAYER &&
           g_pf.unbind_calls[0].planet_slot == SRC_PLANET,
       "pf: prod_unbind_planet(player, G_PLANET_INDEX) -- exactly these 2 args, add_docked-failure site");
    ck_eq((uint32_t)header_row.order, 200u,
          "pf: the header-row queue-counter INC (units[player][0].order += "
          "UNIT_STATE_STOP_TO_DEFAULT) is UNDONE (DEC) on the add_docked failure path");
    ck_eq((uint32_t)g_pf.launch_n, 0u,
          "pf: add_docked failure -> storage_launch_parked_to_orbit never called");
}

void test_dispatch_hport_launch_fails_unbinds_two_arg_header_row_not_undone_returns_one() {
    sim_fixture f;
    g_pf.reset();
    building &bldg                    = f.b(PLAYER, BUILDING_INDEX);
    bldg.shuttle_slot                 = (uint8_t)SHUTTLE_SLOT;
    bldg.building_id                  = BUILDING_ID;
    f.cfg_buildings[BUILDING_ID].type = BUILDING_TYPE_H_PORT;
    f.planet_index                    = SRC_PLANET;
    g_pf.fuel_check_ret               = 0;
    g_pf.add_docked_ret               = 1; // succeeds
    g_pf.launch_ret                   = 0; // FAILS (new_unit_slot == 0)

    unit &header_row = f.u(PLAYER, 0);
    header_row.order = 300; // distinct baseline from the add_docked-failure test above

    sim_view      v   = f.view();
    sim_store     own = f.store();
    const int32_t r =
        detail::prod_bldg_depart_finalize(v, own, rec_pf_calls(), PLAYER, BUILDING_INDEX, DEST_PLANET);

    ck(r == 1, "pf: launch failure is NOT an error (unlike add_docked failure) -- returns 1");
    // THE PHANTOM-ARGUMENT HAZARD, call site 2 (0x0048efb5, per the header banner -- and per this
    // file's own CORRECTION to the task brief: this is the site whose Ghidra .c draft rendering
    // actually carries the phantom 3rd arg, not site 1 above). Both sites are 2-arg off the raw opcodes.
    ck(g_pf.unbind_calls.size() == 1 && g_pf.unbind_calls[0].player == (int32_t)PLAYER &&
           g_pf.unbind_calls[0].planet_slot == SRC_PLANET,
       "pf: prod_unbind_planet(player, G_PLANET_INDEX) -- exactly these 2 args, launch-failure site");
    ck_eq((uint32_t)header_row.order, 301u,
          "pf: the header-row INC is NOT undone here -- only the add_docked-failure arm undoes it");
    ck_eq((uint32_t)f.b(PLAYER, BUILDING_INDEX).shuttle_slot, (uint32_t)SHUTTLE_SLOT,
          "pf: launch failure -> building.shuttle_slot NOT cleared (clearing only happens on launch "
          "success)");
}

void test_dispatch_aport_launch_succeeds_stamps_new_unit_and_slot_fields_returns_one() {
    sim_fixture f;
    g_pf.reset();
    constexpr int32_t  NEW_UNIT_SLOT  = 55;
    constexpr uint16_t NEW_PROTO_ID   = 77;
    building          &bldg           = f.b(PLAYER, BUILDING_INDEX);
    bldg.shuttle_slot                 = (uint8_t)SHUTTLE_SLOT;
    bldg.building_id                  = BUILDING_ID;
    f.cfg_buildings[BUILDING_ID].type = BUILDING_TYPE_A_PORT;
    g_pf.fuel_check_ret               = 0;
    g_pf.add_docked_ret               = 1;
    g_pf.launch_ret                   = NEW_UNIT_SLOT;
    // The stub doesn't populate the launched unit record the way the real llm_strat_storage_launch_
    // parked_to_orbit would -- seed unit_proto_id ourselves, same posture as sim_prod_completion_
    // selftest.cpp's own create-style stubs.
    f.u(PLAYER, NEW_UNIT_SLOT).unit_proto_id = NEW_PROTO_ID;

    unit &header_row = f.u(PLAYER, 0);
    header_row.order = 50;

    sim_view      v   = f.view();
    sim_store     own = f.store();
    const int32_t r =
        detail::prod_bldg_depart_finalize(v, own, rec_pf_calls(), PLAYER, BUILDING_INDEX, DEST_PLANET);

    ck(r == 1, "pf: launch succeeds -> return 1");
    ck_eq((uint32_t)header_row.order, 51u, "pf: header-row INC stays (not undone on the success path)");
    ck_eq((uint32_t)f.u(PLAYER, NEW_UNIT_SLOT).shuttle_slot, (uint32_t)SHUTTLE_SLOT,
          "pf: the newly-launched unit's shuttle_slot = the building's own shuttle_slot");
    ck_eq((uint32_t)f.b(PLAYER, BUILDING_INDEX).shuttle_slot, 0u,
          "pf: building.shuttle_slot cleared to 0 on launch success");
    prod_shuttle_slot &s = slot_of(f, PLAYER, SHUTTLE_SLOT);
    ck_eq((uint32_t)s.type_ref_id, (uint32_t)NEW_PROTO_ID,
          "pf: slot.type_ref_id mirrors the new unit's unit_proto_id");
    ck_eq((uint32_t)s.src_building_type, (uint32_t)BUILDING_TYPE_A_PORT,
          "pf: slot.src_building_type = a FRESH final building_id re-read's cfg type");
    ck_eq((uint32_t)g_pf.unbind_calls.size(), 0u, "pf: launch success -> no unbind call at all");
}

void test_dispatch_mother_a_mother_sets_deploy_start_returns_one_no_port_calls() {
    sim_fixture f;
    g_pf.reset();
    building &bldg                    = f.b(PLAYER, BUILDING_INDEX);
    bldg.shuttle_slot                 = (uint8_t)SHUTTLE_SLOT;
    bldg.building_id                  = BUILDING_ID;
    bldg.state                        = 0x05; // sentinel, must be overwritten
    f.cfg_buildings[BUILDING_ID].type = BUILDING_TYPE_A_MOTHER;
    g_pf.fuel_check_ret               = 0;

    sim_view      v   = f.view();
    sim_store     own = f.store();
    const int32_t r =
        detail::prod_bldg_depart_finalize(v, own, rec_pf_calls(), PLAYER, BUILDING_INDEX, DEST_PLANET);

    ck(r == 1, "pf(A_MOTHER): returns 1");
    ck_eq((uint32_t)f.b(PLAYER, BUILDING_INDEX).state, (uint32_t)BLDG_STATE_DEPLOY_START,
          "pf(A_MOTHER): building.state = DEPLOY_START(0x7d)");
    ck_eq((uint32_t)g_pf.add_docked_n, 0u, "pf(A_MOTHER): the PORT dock/launch arm never runs");
    ck_eq((uint32_t)g_pf.launch_n, 0u, "pf(A_MOTHER): storage_launch_parked_to_orbit never called");
    ck_eq((uint32_t)g_pf.unbind_calls.size(), 0u, "pf(A_MOTHER): no unbind call either");
}

void test_dispatch_mother_a_shuttle_sets_deploy_start_returns_one() {
    sim_fixture f;
    g_pf.reset();
    building &bldg                    = f.b(PLAYER, BUILDING_INDEX);
    bldg.shuttle_slot                 = (uint8_t)SHUTTLE_SLOT;
    bldg.building_id                  = BUILDING_ID;
    bldg.state                        = 0x06;
    f.cfg_buildings[BUILDING_ID].type = BUILDING_TYPE_A_SHUTTLE;
    g_pf.fuel_check_ret               = 0;

    sim_view      v   = f.view();
    sim_store     own = f.store();
    const int32_t r =
        detail::prod_bldg_depart_finalize(v, own, rec_pf_calls(), PLAYER, BUILDING_INDEX, DEST_PLANET);

    ck(r == 1, "pf(A_SHUTTLE): returns 1");
    ck_eq((uint32_t)f.b(PLAYER, BUILDING_INDEX).state, (uint32_t)BLDG_STATE_DEPLOY_START,
          "pf(A_SHUTTLE): building.state = DEPLOY_START(0x7d)");
}

void test_dispatch_mother_h_mother_sets_deploy_start_returns_one() {
    sim_fixture f;
    g_pf.reset();
    building &bldg                    = f.b(PLAYER, BUILDING_INDEX);
    bldg.shuttle_slot                 = (uint8_t)SHUTTLE_SLOT;
    bldg.building_id                  = BUILDING_ID;
    bldg.state                        = 0x07;
    f.cfg_buildings[BUILDING_ID].type = BUILDING_TYPE_H_MOTHER;
    g_pf.fuel_check_ret               = 0;

    sim_view      v   = f.view();
    sim_store     own = f.store();
    const int32_t r =
        detail::prod_bldg_depart_finalize(v, own, rec_pf_calls(), PLAYER, BUILDING_INDEX, DEST_PLANET);

    ck(r == 1, "pf(H_MOTHER): returns 1");
    ck_eq((uint32_t)f.b(PLAYER, BUILDING_INDEX).state, (uint32_t)BLDG_STATE_DEPLOY_START,
          "pf(H_MOTHER): building.state = DEPLOY_START(0x7d)");
}

void test_dispatch_mother_h_shuttle_sets_deploy_start_returns_one() {
    sim_fixture f;
    g_pf.reset();
    building &bldg                    = f.b(PLAYER, BUILDING_INDEX);
    bldg.shuttle_slot                 = (uint8_t)SHUTTLE_SLOT;
    bldg.building_id                  = BUILDING_ID;
    bldg.state                        = 0x08;
    f.cfg_buildings[BUILDING_ID].type = BUILDING_TYPE_H_SHUTTLE;
    g_pf.fuel_check_ret               = 0;

    sim_view      v   = f.view();
    sim_store     own = f.store();
    const int32_t r =
        detail::prod_bldg_depart_finalize(v, own, rec_pf_calls(), PLAYER, BUILDING_INDEX, DEST_PLANET);

    ck(r == 1, "pf(H_SHUTTLE): returns 1");
    ck_eq((uint32_t)f.b(PLAYER, BUILDING_INDEX).state, (uint32_t)BLDG_STATE_DEPLOY_START,
          "pf(H_SHUTTLE): building.state = DEPLOY_START(0x7d)");
}

void test_dispatch_unrecognized_type_is_noop_returns_one_no_calls() {
    sim_fixture f;
    g_pf.reset();
    building &bldg                    = f.b(PLAYER, BUILDING_INDEX);
    bldg.shuttle_slot                 = (uint8_t)SHUTTLE_SLOT;
    bldg.building_id                  = BUILDING_ID;
    bldg.state                        = 0x42; // sentinel, must stay untouched
    f.cfg_buildings[BUILDING_ID].type = 0x99; // not in either outcome set
    g_pf.fuel_check_ret               = 0;

    sim_view      v   = f.view();
    sim_store     own = f.store();
    const int32_t r =
        detail::prod_bldg_depart_finalize(v, own, rec_pf_calls(), PLAYER, BUILDING_INDEX, DEST_PLANET);

    ck(r == 1, "pf(unrecognized type): silent no-op still returns 1");
    ck_eq((uint32_t)f.b(PLAYER, BUILDING_INDEX).state, 0x42u,
          "pf(unrecognized type): building.state untouched");
    ck_eq((uint32_t)f.b(PLAYER, BUILDING_INDEX).shuttle_slot, (uint32_t)SHUTTLE_SLOT,
          "pf(unrecognized type): building.shuttle_slot untouched");
    ck_eq((uint32_t)g_pf.add_docked_n, 0u, "pf(unrecognized type): no add_docked call");
    ck_eq((uint32_t)g_pf.launch_n, 0u, "pf(unrecognized type): no launch call");
    ck_eq((uint32_t)g_pf.unbind_calls.size(), 0u, "pf(unrecognized type): no unbind call");
}

} // namespace

void run_prod_bldg_depart_finalize_tests() {
    test_head_distance_call_uses_planet_index_and_dest_planet_coords();
    test_travel_duration_formula_is_distance_factor_times_velocity_times_distance();
    test_travel_duration_copy_reloads_from_travel_duration();
    test_slot_fields_written_even_when_fuel_check_fails();
    test_slot_fields_persist_through_a_normal_pass_and_dispatch();
    test_dead_branch_planet_index_over_4_does_not_multiply_duration();
    test_fuel_check_called_with_player_building_index_dest_planet();

    test_dispatch_port_add_docked_call_args();
    test_dispatch_aport_add_docked_fails_unbinds_two_arg_undoes_header_row();
    test_dispatch_hport_launch_fails_unbinds_two_arg_header_row_not_undone_returns_one();
    test_dispatch_aport_launch_succeeds_stamps_new_unit_and_slot_fields_returns_one();

    test_dispatch_mother_a_mother_sets_deploy_start_returns_one_no_port_calls();
    test_dispatch_mother_a_shuttle_sets_deploy_start_returns_one();
    test_dispatch_mother_h_mother_sets_deploy_start_returns_one();
    test_dispatch_mother_h_shuttle_sets_deploy_start_returns_one();

    test_dispatch_unrecognized_type_is_noop_returns_one_no_calls();
}

} // namespace mh::sim::test
