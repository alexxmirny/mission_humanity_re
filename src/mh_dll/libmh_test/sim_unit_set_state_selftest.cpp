//
// sim_unit_set_state_selftest.cpp -- `simtest` cases for llm_strat_unit_set_state
// (sim/sim_unit_set_state.h/.cpp), SIM1A.
//
// Trivial function -- a bare `own.cur_unit().state = new_state` -- so two cases suffice: write a
// value and read it back through own.cur_unit(), and confirm a second write overwrites the first
// (rules out a stuck/latched write).
//
#include "sim/sim_unit_set_state.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

void test_unit_set_state_writes_cur_unit_state() {
    sim_fixture f;
    sim_store   own = f.store();

    own.cur_unit().state = 0; // distinct starting value, not the value under test
    detail::unit_set_state(own, 0x1234);

    ck_eq(own.cur_unit().state, 0x1234u, "set_state: cur_unit().state reads back the written value");
}

void test_unit_set_state_second_write_overwrites_first() {
    sim_fixture f;
    sim_store   own = f.store();

    detail::unit_set_state(own, 7);
    ck_eq(own.cur_unit().state, 7u, "set_state: first write lands");

    detail::unit_set_state(own, 0xffff); // full 16-bit width, not just a small value
    ck_eq(own.cur_unit().state, 0xffffu, "set_state: second write replaces the first, no latching");
}

} // namespace

void run_unit_set_state_tests() {
    test_unit_set_state_writes_cur_unit_state();
    test_unit_set_state_second_write_overwrites_first();
}

} // namespace mh::sim::test
