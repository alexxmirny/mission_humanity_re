//
// sim_unit_ctrl_group_selftest.cpp -- `simtest` cases for llm_strat_ctrl_group_contains_unit and
// llm_strat_unit_ctrl_group_assign (sim/sim_unit_ctrl_group.{h,cpp}), SIM1A.
//
// Own recording calls struct (2 members: ctrlgroup_add_member / ctrlgroup_remove_member), same shape
// as sim_unit_passive_engage_selftest.cpp's log_t -- one static instance, reconfigured per case.
//
#include "sim/sim_unit_ctrl_group.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

struct log_t {
    int     add_calls          = 0;
    int32_t last_add_unit_id   = -1;
    int32_t last_add_group_id  = -1;
    void   *last_add_count_ptr = nullptr;

    int      remove_calls          = 0;
    uint32_t last_remove_unit_id   = 0;
    int32_t  last_remove_group_id  = -1;
    void    *last_remove_count_ptr = nullptr;

    void reset() { *this = log_t{}; }
};
log_t g_log;

const unit_ctrl_group_calls &recording_calls() {
    static const unit_ctrl_group_calls c = {
        [](int32_t unit_id, int32_t *count_ptr, int32_t group_id) -> void {
            ++g_log.add_calls;
            g_log.last_add_unit_id   = unit_id;
            g_log.last_add_group_id  = group_id;
            g_log.last_add_count_ptr = count_ptr;
        },
        [](uint32_t unit_id, int32_t *count_ptr, int32_t group_id) -> void {
            ++g_log.remove_calls;
            g_log.last_remove_unit_id   = unit_id;
            g_log.last_remove_group_id  = group_id;
            g_log.last_remove_count_ptr = count_ptr;
        },
    };
    return c;
}

// ---- llm_strat_ctrl_group_contains_unit @0x00445f27 -----------------------------------------------
void test_ctrl_group_contains_unit() {
    sim_fixture    f;
    const sim_view v = f.view();

    f.ctrl_groups[2].unit_ids[0] = 10;
    f.ctrl_groups[2].unit_ids[1] = 20;
    f.ctrl_groups[2].unit_ids[2] = 30;
    f.ctrl_groups[2].unit_ids[3] = 40; // deliberately one slot PAST count=3 below

    ck(detail::ctrl_group_contains_unit(v, 20, /*count=*/3, /*group_index=*/2) == 1,
       "contains_unit: unit_id present within [0,count) -> 1");
    ck(detail::ctrl_group_contains_unit(v, 30, 3, 2) == 1,
       "contains_unit: unit_id at the LAST scanned index (count-1) is still found -> 1");
    ck(detail::ctrl_group_contains_unit(v, 40, 3, 2) == 0,
       "contains_unit: unit_id at index==count (one PAST the exclusive bound) is never scanned -> 0");
    ck(detail::ctrl_group_contains_unit(v, 99, 3, 2) == 0,
       "contains_unit: unit_id absent from the scanned range -> 0");
    ck(detail::ctrl_group_contains_unit(v, 10, 0, 2) == 0,
       "contains_unit: count==0 -> empty scan, 0 regardless of contents");

    // A different group_index at a different slot is scanned independently -- no cross-group leakage.
    f.ctrl_groups[5].unit_ids[0] = 10;
    ck(detail::ctrl_group_contains_unit(v, 10, 3, 2) == 1, "contains_unit: group 2 still finds its own member 10");
    ck(detail::ctrl_group_contains_unit(v, 10, 1, 5) == 1,
       "contains_unit: group 5's own member 10 found independently, via group_index (the 3rd param)");
}

// ---- llm_strat_unit_ctrl_group_assign @0x0044aa18 --------------------------------------------------
void test_unit_ctrl_group_assign_already_in_different_group() {
    sim_fixture f;
    sim_store   own = f.store();
    f.player_side   = 3; // PlayerSide -- the AMBIENT global consulted here, not a function parameter

    unit &u         = f.u(3, 12);
    u.ctrl_group_id = 7; // already a member of group 7

    g_log.reset();
    detail::unit_ctrl_group_assign(f.view(), own, recording_calls(), /*unit_id=*/12, /*new_group_id=*/9);

    ck(g_log.remove_calls == 1 && g_log.last_remove_unit_id == 12u && g_log.last_remove_group_id == 7,
       "assign: unit already in a DIFFERENT group (7) -> remove_member(unit_id, &group[7].count, 7) fires");
    ck(g_log.last_remove_count_ptr == &own.ctrl_group_at(7).count,
       "assign: remove_member's count_ptr addresses group 7's OWN .count field");
    ck(g_log.add_calls == 1 && g_log.last_add_unit_id == 12 && g_log.last_add_group_id == 9,
       "assign: add_member(unit_id, &group[9].count, 9) always fires too, unconditionally");
    ck(g_log.last_add_count_ptr == &own.ctrl_group_at(9).count,
       "assign: add_member's count_ptr addresses the NEW group's .count field");
}

void test_unit_ctrl_group_assign_no_prior_group() {
    sim_fixture f;
    sim_store   own = f.store();
    f.player_side   = 4;

    unit &u         = f.u(4, 20);
    u.ctrl_group_id = 0; // not currently in any group

    g_log.reset();
    detail::unit_ctrl_group_assign(f.view(), own, recording_calls(), /*unit_id=*/20, /*new_group_id=*/2);

    ck(g_log.remove_calls == 0,
       "assign: ctrl_group_id == 0 -> the whole remove-step block is skipped, no remove_member call");
    ck(g_log.add_calls == 1 && g_log.last_add_unit_id == 20 && g_log.last_add_group_id == 2,
       "assign: add_member still fires unconditionally when there was no prior group");
}

void test_unit_ctrl_group_assign_reassign_to_same_group_still_runs_both_steps() {
    // The original does not compare the unit's CURRENT group against new_group_id, only against 0 --
    // so assigning a unit already sitting in group 5 back into group 5 still runs BOTH the remove and
    // the add step (absorbed as a no-op-ish swap-removal + re-add by the ORIGINAL callees), not a
    // short-circuited no-op. Do not "optimize" this away.
    sim_fixture f;
    sim_store   own = f.store();
    f.player_side   = 1;

    unit &u         = f.u(1, 33);
    u.ctrl_group_id = 5;

    g_log.reset();
    detail::unit_ctrl_group_assign(f.view(), own, recording_calls(), /*unit_id=*/33, /*new_group_id=*/5);

    ck(g_log.remove_calls == 1 && g_log.last_remove_group_id == 5,
       "assign: reassigning a unit into the group it is ALREADY in still runs the remove step");
    ck(g_log.add_calls == 1 && g_log.last_add_group_id == 5,
       "assign: ...and the add step too -- both address group 5's own .count field, no short-circuit");
}

void test_unit_ctrl_group_assign_uses_ambient_player_side_not_a_parameter() {
    // Two different PlayerSide values must reach two different roster slots even with the SAME
    // unit_id -- proves the group being consulted is units[PlayerSide][unit_id], read from the
    // ambient global, not from any parameter of this function (there is no player parameter).
    sim_fixture f;
    sim_store   own = f.store();

    f.u(2, 15).ctrl_group_id = 6;
    f.u(6, 15).ctrl_group_id = 0; // same unit_id (15), different player, different starting state

    f.player_side = 2;
    g_log.reset();
    detail::unit_ctrl_group_assign(f.view(), own, recording_calls(), /*unit_id=*/15, /*new_group_id=*/8);
    ck(g_log.remove_calls == 1 && g_log.last_remove_group_id == 6,
       "assign: PlayerSide==2 -> reads units[2][15].ctrl_group_id (6), triggers the remove step");

    f.player_side = 6;
    g_log.reset();
    detail::unit_ctrl_group_assign(f.view(), own, recording_calls(), /*unit_id=*/15, /*new_group_id=*/8);
    ck(g_log.remove_calls == 0,
       "assign: same unit_id, PlayerSide==6 instead -> reads units[6][15].ctrl_group_id (0), no remove");
}

} // namespace

void run_unit_ctrl_group_tests() {
    test_ctrl_group_contains_unit();
    test_unit_ctrl_group_assign_already_in_different_group();
    test_unit_ctrl_group_assign_no_prior_group();
    test_unit_ctrl_group_assign_reassign_to_same_group_still_runs_both_steps();
    test_unit_ctrl_group_assign_uses_ambient_player_side_not_a_parameter();
}

} // namespace mh::sim::test
