//
// sim_unit_ctrlgroup_member_selftest.cpp -- `simtest` cases for llm_strat_unit_ctrlgroup_add_member
// and llm_strat_unit_ctrlgroup_remove_member (sim/sim_unit_ctrlgroup_member.h/.cpp), SIM1A fifth
// slice. Own recording calls struct (2 members), same shape as sim_unit_ctrl_group_selftest.cpp's
// log_t -- one static instance, reconfigured per case.
//
#include "sim/sim_unit_ctrlgroup_member.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

struct log_t {
    int      select_calls        = 0;
    uint32_t last_select_side    = 0;
    uint16_t last_select_unit_id = 0;
    uint32_t last_select_group   = 0;

    int      flash_calls        = 0;
    uint16_t last_flash_side    = 0;
    uint16_t last_flash_unit_id = 0;
    uint32_t last_flash_group   = 0;

    void reset() { *this = log_t{}; }
};
log_t g_log;

const unit_ctrlgroup_member_calls &recording_calls() {
    static const unit_ctrlgroup_member_calls c = {
        [](uint32_t side, uint16_t unit_id, int32_t group_index) -> void {
            ++g_log.select_calls;
            g_log.last_select_side    = side;
            g_log.last_select_unit_id = unit_id;
            g_log.last_select_group   = group_index;
        },
        [](uint16_t side, uint16_t unit_id, int32_t group_index) -> void {
            ++g_log.flash_calls;
            g_log.last_flash_side    = side;
            g_log.last_flash_unit_id = unit_id;
            g_log.last_flash_group   = group_index;
        },
    };
    return c;
}

// ---- llm_strat_unit_ctrlgroup_add_member @0x00449401 --------------------------------------------

void test_add_member_real_group_appends_and_stamps_ctrl_group_id() {
    sim_fixture    f;
    const sim_view v   = f.view();
    sim_store      own = f.store();
    f.player_side      = 3;

    ctrl_group &g = own.ctrl_group_at(5);
    g.count       = 2;
    g.unit_ids[0] = 100;
    g.unit_ids[1] = 200;

    g_log.reset();
    detail::unit_ctrlgroup_add_member(v, own, recording_calls(), /*unit_id=*/55, &g.count, /*group_idx=*/5);

    ck_eq(g.unit_ids[2], 55u, "add_member: appended at unit_ids[*count_ptr] (the pre-increment index)");
    ck_eq((uint32_t)g.count, 3u, "add_member: *count_ptr incremented by exactly one");
    ck_eq(own.unit_at(3, 55).ctrl_group_id, 5u,
          "add_member: group_idx!=0 -> units[PlayerSide][unit_id].ctrl_group_id stamped to group_idx");
    ck(g_log.select_calls == 1 && g_log.last_select_side == 3u && g_log.last_select_unit_id == 55u &&
           g_log.last_select_group == 5u,
       "add_member: order_ctrlgrp_select_member(side, unit_id, group_idx) fires unconditionally");
}

void test_add_member_group_zero_appends_but_does_not_stamp_ctrl_group_id() {
    sim_fixture    f;
    const sim_view v   = f.view();
    sim_store      own = f.store();
    f.player_side      = 4;

    ctrl_group &g = own.ctrl_group_at(0);
    g.count       = 0;

    unit &u         = own.unit_at(4, 77);
    u.ctrl_group_id = 111; // sentinel: must survive UNTOUCHED

    g_log.reset();
    detail::unit_ctrlgroup_add_member(v, own, recording_calls(), /*unit_id=*/77, &g.count, /*group_idx=*/0);

    ck_eq(g.unit_ids[0], 77u,
          "add_member: the append itself is UNCONDITIONAL -- group_idx==0 still appends to unit_ids[]");
    ck_eq((uint32_t)g.count, 1u, "add_member: *count_ptr still increments even for group_idx==0");
    ck_eq(u.ctrl_group_id, 111u,
          "add_member: group_idx==0 -> the ctrl_group_id STAMP is skipped, sentinel survives");
    ck(g_log.select_calls == 1 && g_log.last_select_group == 0u,
       "add_member: the notify call still fires even for group_idx==0 (unconditional tail call)");
}

// ---- llm_strat_unit_ctrlgroup_remove_member @0x0044947e ------------------------------------------

void test_remove_member_real_group_shift_compacts_and_clears_ctrl_group_id() {
    sim_fixture    f;
    const sim_view v   = f.view();
    sim_store      own = f.store();
    f.player_side      = 2;

    ctrl_group &g = own.ctrl_group_at(6);
    g.count       = 4;
    g.unit_ids[0] = 10;
    g.unit_ids[1] = 20;
    g.unit_ids[2] = 30;
    g.unit_ids[3] = 40;

    unit &u         = own.unit_at(2, 20);
    u.ctrl_group_id = 6;

    g_log.reset();
    detail::unit_ctrlgroup_remove_member(v, own, recording_calls(), /*unit_idx=*/20, &g.count,
                                         /*group_idx=*/6);

    ck_eq((uint32_t)g.count, 3u, "remove_member: a found match decrements *count_ptr by one");
    ck_eq(g.unit_ids[0], 10u, "remove_member: entries before the match are untouched");
    ck_eq(g.unit_ids[1], 30u, "remove_member: shift-compact -- unit_ids[1] takes what was unit_ids[2]");
    ck_eq(g.unit_ids[2], 40u, "remove_member: shift-compact -- unit_ids[2] takes what was unit_ids[3]");
    ck_eq(u.ctrl_group_id, 0u, "remove_member: group_idx!=0 -> ctrl_group_id cleared to 0 on a match");
    ck(g_log.flash_calls == 1 && g_log.last_flash_side == 2u && g_log.last_flash_unit_id == 20u &&
           g_log.last_flash_group == 6u,
       "remove_member: order_ctrlgrp_flash_member(side, unit_idx, group_idx) fires");
}

void test_remove_member_group_zero_shift_compacts_but_does_not_clear_ctrl_group_id() {
    sim_fixture    f;
    const sim_view v   = f.view();
    sim_store      own = f.store();
    f.player_side      = 6; // valid player index: MAX_PLAYERS is 8, so 0..7

    ctrl_group &g = own.ctrl_group_at(0);
    g.count       = 2;
    g.unit_ids[0] = 5;
    g.unit_ids[1] = 6;

    unit &u         = own.unit_at(6, 5);
    u.ctrl_group_id = 222; // sentinel: must survive UNTOUCHED, group_idx==0

    g_log.reset();
    detail::unit_ctrlgroup_remove_member(v, own, recording_calls(), /*unit_idx=*/5, &g.count,
                                         /*group_idx=*/0);

    ck_eq((uint32_t)g.count, 1u, "remove_member: the shift-compact itself is unconditional on group_idx");
    ck_eq(g.unit_ids[0], 6u, "remove_member: group_idx==0 still shift-compacts the array normally");
    ck_eq(u.ctrl_group_id, 222u,
          "remove_member: group_idx==0 -> the ctrl_group_id CLEAR is skipped, sentinel survives");
}

// ---- FINDING (not a bug -- verified against the raw asm/translation, preserved faithfully): the
// group_idx!=0 ctrl_group_id-clear and the flash-member notify call both run UNCONDITIONALLY after
// the scan loop -- NOT gated on whether the loop actually found and removed a match. So removing a
// unit_idx that was never a member of the group still clears units[side][unit_idx].ctrl_group_id
// (if group_idx!=0) and still fires the notify call, even though the array itself is untouched. ---
void test_remove_member_absent_unit_id_is_array_noop_but_still_clears_and_notifies() {
    sim_fixture    f;
    const sim_view v   = f.view();
    sim_store      own = f.store();
    f.player_side      = 1;

    ctrl_group &g = own.ctrl_group_at(7);
    g.count       = 3;
    g.unit_ids[0] = 1;
    g.unit_ids[1] = 2;
    g.unit_ids[2] = 3;

    unit &u = own.unit_at(1, 77); // never a member of group 7 (UNITS_PER_PLAYER is 100, so
                                  // this must stay < 100 -- unlike unit_ids[], which holds
                                  // arbitrary roster-index VALUES up to 200, this indexes the
                                  // roster array itself)
    u.ctrl_group_id = 55;         // sentinel

    g_log.reset();
    detail::unit_ctrlgroup_remove_member(v, own, recording_calls(), /*unit_idx=*/77, &g.count,
                                         /*group_idx=*/7);

    ck_eq((uint32_t)g.count, 3u, "remove_member: absent unit_idx -> *count_ptr unchanged (no match found)");
    ck(g.unit_ids[0] == 1 && g.unit_ids[1] == 2 && g.unit_ids[2] == 3,
       "remove_member: absent unit_idx -> unit_ids[] contents unchanged");
    ck_eq(u.ctrl_group_id, 0u,
          "remove_member: FINDING -- the ctrl_group_id clear is UNCONDITIONAL on group_idx!=0, not on "
          "having found a match; it still fires for an absent unit_idx (faithful to the original)");
    ck(g_log.flash_calls == 1,
       "remove_member: the notify call is likewise unconditional -- fires even with no match");
}

} // namespace

void run_unit_ctrlgroup_member_tests() {
    test_add_member_real_group_appends_and_stamps_ctrl_group_id();
    test_add_member_group_zero_appends_but_does_not_stamp_ctrl_group_id();
    test_remove_member_real_group_shift_compacts_and_clears_ctrl_group_id();
    test_remove_member_group_zero_shift_compacts_but_does_not_clear_ctrl_group_id();
    test_remove_member_absent_unit_id_is_array_noop_but_still_clears_and_notifies();
}

} // namespace mh::sim::test
