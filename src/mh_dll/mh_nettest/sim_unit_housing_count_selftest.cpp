//
// sim_unit_housing_count_selftest.cpp -- `simtest` cases for llm_strat_unit_housing_count_add and
// llm_strat_unit_housing_count_remove (sim/sim_unit_housing_count.h/.cpp), SIM1A fourth/fifth
// slices. Neither function had a direct test before this file (sim_unit_recruit_selftest.cpp only
// mocks _add as a CALLEE of unit_recruit, never exercises the real body) -- so both get full
// five-way-ladder coverage here, not just the newly-added _remove.
//
#include "sim/sim_unit_housing_count.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- llm_strat_unit_housing_count_add @0x0049779b --------------------------------------------

void test_housing_count_add_ladder() {
    sim_fixture    f;
    const sim_view v   = f.view();
    sim_store      own = f.store();

    const int32_t player = 4;

    // type == UNIT_TYPE_UNDEFINED (0) -> nothing touched.
    f.cfg_units[10].type          = UNIT_TYPE_UNDEFINED;
    f.cfg_units[10].soldier_count = 99; // must be ignored
    own.unit_housing_at(player)   = housing_stats{};
    detail::unit_housing_count_add(v, own, player, 10);
    ck(own.unit_housing_at(player).used_vehicles == 0 && own.unit_housing_at(player).used_soldiers == 0 &&
           own.unit_housing_at(player).used_planes == 0 && own.unit_housing_at(player).used_helis == 0,
       "housing_count_add: type==UNDEFINED touches nothing");

    // 0 < type < UNIT_TYPE_A_WALKER (0xb) -> used_soldiers += soldier_count.
    f.cfg_units[11].type          = 5;
    f.cfg_units[11].soldier_count = 17;
    own.unit_housing_at(player)   = housing_stats{};
    detail::unit_housing_count_add(v, own, player, 11);
    ck_eq((uint32_t)own.unit_housing_at(player).used_soldiers, 17u,
          "housing_count_add: 0<type<A_WALKER -> used_soldiers += soldier_count");
    ck_eq((uint32_t)own.unit_housing_at(player).used_vehicles, 0u,
          "housing_count_add: the soldier bucket does not also touch used_vehicles");

    // UNIT_TYPE_A_WALKER <= type < UNIT_TYPE_A_HELI (0xb..0xe) -> used_vehicles += 1.
    f.cfg_units[12].type        = UNIT_TYPE_A_WALKER; // exactly the lower bound, inclusive
    own.unit_housing_at(player) = housing_stats{};
    detail::unit_housing_count_add(v, own, player, 12);
    ck_eq((uint32_t)own.unit_housing_at(player).used_vehicles, 1u,
          "housing_count_add: type==A_WALKER (lower bound, inclusive) -> used_vehicles += 1");

    // UNIT_TYPE_A_HELI <= type < UNIT_TYPE_A_PLANE (0xf..0x10) -> used_helis += 1.
    f.cfg_units[13].type        = UNIT_TYPE_A_HELI;
    own.unit_housing_at(player) = housing_stats{};
    detail::unit_housing_count_add(v, own, player, 13);
    ck_eq((uint32_t)own.unit_housing_at(player).used_helis, 1u,
          "housing_count_add: type==A_HELI (lower bound) -> used_helis += 1");

    // UNIT_TYPE_A_PLANE <= type < UNIT_TYPE_A_HELI_MOTHER (0x11..0x12) -> used_planes += 1.
    f.cfg_units[14].type        = UNIT_TYPE_A_PLANE;
    own.unit_housing_at(player) = housing_stats{};
    detail::unit_housing_count_add(v, own, player, 14);
    ck_eq((uint32_t)own.unit_housing_at(player).used_planes, 1u,
          "housing_count_add: type==A_PLANE (lower bound) -> used_planes += 1");

    // type >= UNIT_TYPE_A_HELI_MOTHER (0x13+) -> nothing, including at the vestigial 0x18 compare.
    f.cfg_units[15].type        = 0x18; // the vestigial dead-compare immediate itself
    own.unit_housing_at(player) = housing_stats{};
    detail::unit_housing_count_add(v, own, player, 15);
    ck(own.unit_housing_at(player).used_vehicles == 0 && own.unit_housing_at(player).used_soldiers == 0 &&
           own.unit_housing_at(player).used_planes == 0 && own.unit_housing_at(player).used_helis == 0,
       "housing_count_add: type==0x18 (the vestigial dead-compare value) still hits the >=A_HELI_MOTHER "
       "no-op arm");
}

void test_housing_count_add_indexes_by_player_not_proto() {
    sim_fixture    f;
    const sim_view v   = f.view();
    sim_store      own = f.store();

    f.cfg_units[20].type = UNIT_TYPE_A_WALKER; // -> used_vehicles += 1
    detail::unit_housing_count_add(v, own, /*player=*/2, 20);

    ck_eq((uint32_t)own.unit_housing_at(2).used_vehicles, 1u,
          "housing_count_add: EAX indexes _G_LLM_STRAT_UNIT_HOUSING_STATS by PLAYER (stride 0x40)");
    ck_eq((uint32_t)own.unit_housing_at(3).used_vehicles, 0u,
          "housing_count_add: a different player's housing row is untouched");
}

// ---- llm_strat_unit_housing_count_remove @0x00497842 -- exact mirror, decrements -------------

void test_housing_count_remove_ladder() {
    sim_fixture    f;
    const sim_view v   = f.view();
    sim_store      own = f.store();

    const int32_t player = 6;

    // type == UNIT_TYPE_UNDEFINED (0) -> nothing touched.
    f.cfg_units[30].type          = UNIT_TYPE_UNDEFINED;
    f.cfg_units[30].soldier_count = 99; // must be ignored
    own.unit_housing_at(player)   = housing_stats{4, 4, 4, 4};
    detail::unit_housing_count_remove(v, own, player, 30);
    ck(own.unit_housing_at(player).used_vehicles == 4 && own.unit_housing_at(player).used_soldiers == 4 &&
           own.unit_housing_at(player).used_planes == 4 && own.unit_housing_at(player).used_helis == 4,
       "housing_count_remove: type==UNDEFINED touches nothing");

    // 0 < type < UNIT_TYPE_A_WALKER -> used_soldiers -= soldier_count.
    f.cfg_units[31].type          = 3;
    f.cfg_units[31].soldier_count = 9;
    own.unit_housing_at(player)   = housing_stats{0, 20, 0, 0};
    detail::unit_housing_count_remove(v, own, player, 31);
    ck_eq((uint32_t)own.unit_housing_at(player).used_soldiers, 11u,
          "housing_count_remove: 0<type<A_WALKER -> used_soldiers -= soldier_count");

    // UNIT_TYPE_A_WALKER <= type < UNIT_TYPE_A_HELI -> used_vehicles -= 1.
    f.cfg_units[32].type        = 0xd; // mid-range, not a boundary value
    own.unit_housing_at(player) = housing_stats{5, 0, 0, 0};
    detail::unit_housing_count_remove(v, own, player, 32);
    ck_eq((uint32_t)own.unit_housing_at(player).used_vehicles, 4u,
          "housing_count_remove: A_WALKER<=type<A_HELI -> used_vehicles -= 1");

    // UNIT_TYPE_A_HELI <= type < UNIT_TYPE_A_PLANE -> used_helis -= 1.
    f.cfg_units[33].type        = 0x10; // upper end of the heli bucket, still < A_PLANE
    own.unit_housing_at(player) = housing_stats{0, 0, 0, 5};
    detail::unit_housing_count_remove(v, own, player, 33);
    ck_eq((uint32_t)own.unit_housing_at(player).used_helis, 4u,
          "housing_count_remove: A_HELI<=type<A_PLANE -> used_helis -= 1");

    // UNIT_TYPE_A_PLANE <= type < UNIT_TYPE_A_HELI_MOTHER -> used_planes -= 1.
    f.cfg_units[34].type        = 0x12; // upper end of the plane bucket, still < A_HELI_MOTHER
    own.unit_housing_at(player) = housing_stats{0, 0, 5, 0};
    detail::unit_housing_count_remove(v, own, player, 34);
    ck_eq((uint32_t)own.unit_housing_at(player).used_planes, 4u,
          "housing_count_remove: A_PLANE<=type<A_HELI_MOTHER -> used_planes -= 1");

    // type >= UNIT_TYPE_A_HELI_MOTHER -> nothing, including at the vestigial 0x18 compare.
    f.cfg_units[35].type        = UNIT_TYPE_A_HELI_MOTHER; // exactly the lower bound of "nothing"
    own.unit_housing_at(player) = housing_stats{7, 7, 7, 7};
    detail::unit_housing_count_remove(v, own, player, 35);
    ck(own.unit_housing_at(player).used_vehicles == 7 && own.unit_housing_at(player).used_soldiers == 7 &&
           own.unit_housing_at(player).used_planes == 7 && own.unit_housing_at(player).used_helis == 7,
       "housing_count_remove: type==A_HELI_MOTHER (lower bound, inclusive of the no-op arm) touches "
       "nothing");
}

void test_housing_count_remove_indexes_by_player_not_proto() {
    sim_fixture    f;
    const sim_view v   = f.view();
    sim_store      own = f.store();

    f.cfg_units[40].type   = UNIT_TYPE_A_HELI; // -> used_helis -= 1
    own.unit_housing_at(2) = housing_stats{0, 0, 0, 3};
    own.unit_housing_at(3) = housing_stats{0, 0, 0, 3};
    detail::unit_housing_count_remove(v, own, /*player=*/2, 40);

    ck_eq((uint32_t)own.unit_housing_at(2).used_helis, 2u,
          "housing_count_remove: EAX indexes _G_LLM_STRAT_UNIT_HOUSING_STATS by PLAYER, not proto_id");
    ck_eq((uint32_t)own.unit_housing_at(3).used_helis, 3u,
          "housing_count_remove: a different player's housing row is untouched");
}

} // namespace

void run_unit_housing_count_tests() {
    test_housing_count_add_ladder();
    test_housing_count_add_indexes_by_player_not_proto();
    test_housing_count_remove_ladder();
    test_housing_count_remove_indexes_by_player_not_proto();
}

} // namespace mh::sim::test
