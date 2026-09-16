//
// sim_unit_path_free_slot_selftest.cpp -- `simtest` cases for llm_strat_path_free_slot
// (sim/sim_unit_path_free_slot.h/.cpp), SIM1A.
//
// Single guarded release: if unit.path_slot_id != 0xff, clear the flags-table entry, release the
// slot id to 0xff, and bump the player's free-slot count. Two cases -- the guard taken and the
// guard skipped -- cover the whole function.
//
#include "sim/sim_unit_path_free_slot.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

void test_path_free_slot_releases_a_valid_slot() {
    sim_fixture    f;
    const sim_view v   = f.view();
    sim_store      own = f.store();

    const uint16_t player     = 3;
    const int32_t  unit_index = 7;
    const uint8_t  slot_id    = 42; // distinct, non-edge value

    own.unit_at(player, unit_index).path_slot_id = slot_id;
    own.path_slot_flag_at(player, slot_id)       = 1; // pre-set, must be cleared
    own.path_free_slot_count_at(player)          = 5; // distinct starting count

    detail::path_free_slot(v, own, player, unit_index);

    ck_eq(own.unit_at(player, unit_index).path_slot_id, 0xffu,
          "path_free_slot: a unit WITH a valid slot_id has it released to 0xff");
    ck_eq(own.path_slot_flag_at(player, slot_id), 0u,
          "path_free_slot: the released slot's flags-table entry is cleared");
    ck_eq((uint32_t)own.path_free_slot_count_at(player), 6u,
          "path_free_slot: the player's free-slot count is incremented by exactly one");
}

void test_path_free_slot_noop_when_already_free() {
    sim_fixture    f;
    const sim_view v   = f.view();
    sim_store      own = f.store();

    const uint16_t player     = 5;
    const int32_t  unit_index = 11;

    own.unit_at(player, unit_index).path_slot_id = 0xff; // already released
    own.path_free_slot_count_at(player)          = 9;    // must NOT change
    own.path_slot_flag_at(player, 0)             = 1;    // must NOT change either

    detail::path_free_slot(v, own, player, unit_index);

    ck_eq(own.unit_at(player, unit_index).path_slot_id, 0xffu,
          "path_free_slot: a unit already at slot_id==0xff stays 0xff (no-op)");
    ck_eq((uint32_t)own.path_free_slot_count_at(player), 9u,
          "path_free_slot: no-op case must NOT increment the free-slot count");
    ck_eq(own.path_slot_flag_at(player, 0), 1u,
          "path_free_slot: no-op case must NOT touch the flags table at all");
}

} // namespace

void run_unit_path_free_slot_tests() {
    test_path_free_slot_releases_a_valid_slot();
    test_path_free_slot_noop_when_already_free();
}

} // namespace mh::sim::test
