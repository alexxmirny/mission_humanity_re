//
// sim_bldg_finish_order_selftest.cpp -- `simtest` cases for llm_bldg_finish_current_order
// (sim/sim_bldg_finish_order.{h,cpp}).
//
// Four independent state-gated branches (PROD_WORKING/RESEARCHING/UPGRADING/CHARGE_STEP) plus an
// unconditional idle-state fallback -- each branch gets its own case so a translation that folded
// two branches together, or dropped the always-run tail, is caught by name.
//
#include "sim/sim_bldg_finish_order.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

constexpr uint32_t PLAYER  = 2;
constexpr uint32_t BLDG_IX = 5;

// llm_strat_bldg_state members this test drives -- same values sim_bldg_finish_order.cpp declares
// (its own anonymous namespace, not shared across TUs).
constexpr uint16_t ST_CHARGE_STEP  = 0x6a;
constexpr uint16_t ST_PROD_WORKING = 0x6d;
constexpr uint16_t ST_UPGRADING    = 0x82;
constexpr uint16_t ST_RESEARCHING  = 0x89;
constexpr uint16_t ST_OTHER        = 0x77; // matches none of the four gates

struct log_t {
    int      apply_production_calls           = 0;
    uint32_t last_apply_production_player     = 0;
    int32_t  last_apply_production_unit_proto = 0;

    int      notify_lifecycle_calls = 0;
    uint16_t last_notify_unit_type  = 0;
    uint32_t last_notify_param4     = 0;

    int      apply_project_calls      = 0;
    uint32_t last_apply_project_index = 0;

    int     grant_type_calls              = 0;
    int32_t last_grant_type_building_type = 0;

    int     uses_workers_calls  = 0;
    int32_t uses_workers_return = 1;

    int      unassign_calls      = 0;
    uint32_t last_unassign_count = 0;
    int32_t  unassign_return     = 0;

    int clear_staffed_calls = 0;

    int notify_ui_calls = 0;

    void reset() { *this = log_t{}; }
};
log_t g_log;

const bldg_finish_order_calls &recording_calls() {
    static const bldg_finish_order_calls c = {
        [](uint32_t player, int32_t unit_proto_id) {
            ++g_log.apply_production_calls;
            g_log.last_apply_production_player     = player;
            g_log.last_apply_production_unit_proto = unit_proto_id;
        },
        [](uint16_t, uint16_t unit_type, uint32_t, uint32_t param_4) {
            ++g_log.notify_lifecycle_calls;
            g_log.last_notify_unit_type = unit_type;
            g_log.last_notify_param4    = param_4;
        },
        [](uint32_t, uint32_t project_index) {
            ++g_log.apply_project_calls;
            g_log.last_apply_project_index = project_index;
        },
        [](uint32_t, int32_t building_type_idx) {
            ++g_log.grant_type_calls;
            g_log.last_grant_type_building_type = building_type_idx;
        },
        [](uint32_t, int32_t) -> int32_t {
            ++g_log.uses_workers_calls;
            return g_log.uses_workers_return;
        },
        [](uint16_t, uint32_t, uint32_t count) -> int32_t {
            ++g_log.unassign_calls;
            g_log.last_unassign_count = count;
            return g_log.unassign_return;
        },
        [](uint16_t, uint32_t) { ++g_log.clear_staffed_calls; },
        [](uint16_t, uint32_t) { ++g_log.notify_ui_calls; },
    };
    return c;
}

// PROD_WORKING: finish the in-flight unit, credit BOTH the running total (slot 0) and the per-type
// slot -- two writes to two different indices of the SAME array, which a translation could collapse
// into one.
void test_prod_working() {
    sim_fixture f;
    sim_store   own                                                     = f.store();
    f.b(PLAYER, BLDG_IX).state                                          = ST_PROD_WORKING;
    f.b(PLAYER, BLDG_IX).sub_id                                         = 3;
    f.productions[PLAYER * PRODUCTIONS_PER_PLAYER + 3].active_unit_type = 6;
    f.productions[PLAYER * PRODUCTIONS_PER_PLAYER + 3].queued_count[0]  = 10;
    f.productions[PLAYER * PRODUCTIONS_PER_PLAYER + 3].queued_count[6]  = 4;
    g_log.reset();

    detail::bldg_finish_current_order(f.view(), own, recording_calls(), PLAYER, BLDG_IX);

    ck(g_log.apply_production_calls == 1 && g_log.last_apply_production_unit_proto == 6,
       "prod_working: llm_unit_apply_production_completion(player, active_unit_type)");
    ck(g_log.notify_lifecycle_calls == 1 && g_log.last_notify_unit_type == 6 &&
           g_log.last_notify_param4 == 2,
       "prod_working: ai_notify_unit_lifecycle(player, active_unit_type, 0, 2)");
    ck(f.productions[PLAYER * PRODUCTIONS_PER_PLAYER + 3].queued_count[0] == 11,
       "prod_working: queued_count[0] (the running total) incremented");
    ck(f.productions[PLAYER * PRODUCTIONS_PER_PLAYER + 3].queued_count[6] == 5,
       "prod_working: queued_count[active_unit_type] (its OWN slot) also incremented");
    // Not this branch's business:
    ck(g_log.apply_project_calls == 0 && g_log.grant_type_calls == 0 && g_log.uses_workers_calls == 0,
       "prod_working: does not touch the research/upgrade/re-staff branches");
}

// RESEARCHING: apply the finished project's resource grant. Nothing else.
void test_researching() {
    sim_fixture f;
    sim_store   own                                        = f.store();
    f.b(PLAYER, BLDG_IX).state                             = ST_RESEARCHING;
    f.b(PLAYER, BLDG_IX).sub_id                            = 4;
    f.labs[PLAYER * LABS_PER_PLAYER + 4].active_project_id = 17;
    g_log.reset();

    detail::bldg_finish_current_order(f.view(), own, recording_calls(), PLAYER, BLDG_IX);

    ck(g_log.apply_project_calls == 1 && g_log.last_apply_project_index == 17,
       "researching: cfg_apply_project_resources(player, active_project_id)");
    ck(g_log.apply_production_calls == 0 && g_log.grant_type_calls == 0,
       "researching: does not touch the production/upgrade branches");
}

// UPGRADING: grant the new type's resources, THEN re-staff. Two workers sub-cases (uses_workers ==
// 0 drops everyone; != 0 unassigns only the surplus over the (possibly smaller, post-upgrade) cfg
// builder_count) so both arms of the shared re-staffing helper are exercised from this call site.
void test_upgrading() {
    {
        sim_fixture f;
        sim_store   own                      = f.store();
        f.b(PLAYER, BLDG_IX).state           = ST_UPGRADING;
        f.b(PLAYER, BLDG_IX).building_id     = 9;
        f.b(PLAYER, BLDG_IX).current_workers = 6;
        f.cfg_buildings[9].upgrade_index     = 21;
        g_log.reset();
        g_log.uses_workers_return = 0; // doesn't use workers -> drop them all

        detail::bldg_finish_current_order(f.view(), own, recording_calls(), PLAYER, BLDG_IX);

        ck(g_log.grant_type_calls == 1 && g_log.last_grant_type_building_type == 21,
           "upgrading: bldg_grant_type_resources(player, cfg.upgrade_index) BEFORE re-staffing");
        ck(g_log.unassign_calls == 1 && g_log.last_unassign_count == 6,
           "upgrading, uses_workers==0: unassigns ALL current_workers");
        ck(g_log.clear_staffed_calls == 0,
           "upgrading, uses_workers==0 branch: NEVER calls clear_staffed_flag (faithful to the "
           "original -- that call only exists on the uses_workers!=0 side)");
    }
    {
        sim_fixture f;
        sim_store   own                      = f.store();
        f.b(PLAYER, BLDG_IX).state           = ST_UPGRADING;
        f.b(PLAYER, BLDG_IX).building_id     = 9;
        f.b(PLAYER, BLDG_IX).current_workers = 6;
        // worker_count, NOT builder_count -- a real bug reimpl-verify caught 2026-08-10 (confirmed
        // against the disassembly's offset math), this fixture is what pins the fix: builder_count
        // is left at its zero default, so a translation that reads the wrong field would compare
        // against 0 and unassign all 6, not the true surplus of 2.
        f.cfg_buildings[9].worker_count  = 4;   // surplus of 2
        f.cfg_buildings[9].builder_count = 999; // decoy -- must NOT be read by this path
        f.cfg_buildings[9].upgrade_index = 21;
        g_log.reset();
        g_log.uses_workers_return = 1;
        g_log.unassign_return     = 0; // building's current_workers "becomes" 0 for the test

        detail::bldg_finish_current_order(f.view(), own, recording_calls(), PLAYER, BLDG_IX);

        ck(g_log.unassign_calls == 1 && g_log.last_unassign_count == 2,
           "upgrading, uses_workers!=0: unassigns only the SURPLUS over cfg worker_count (6-4=2), "
           "NOT builder_count (which is a decoy 999 here and would surplus to 0 if misread)");
    }
}

// CHARGE_STEP: re-staff only -- no resource grant. Same helper as UPGRADING; this pins that no
// grant_type_resources call leaks in on this path.
void test_charge_step() {
    sim_fixture f;
    sim_store   own                      = f.store();
    f.b(PLAYER, BLDG_IX).state           = ST_CHARGE_STEP;
    f.b(PLAYER, BLDG_IX).building_id     = 9;
    f.b(PLAYER, BLDG_IX).current_workers = 3;
    g_log.reset();
    g_log.uses_workers_return = 0;

    detail::bldg_finish_current_order(f.view(), own, recording_calls(), PLAYER, BLDG_IX);

    ck(g_log.grant_type_calls == 0, "charge_step: NO resource grant -- that's UPGRADING-only");
    ck(g_log.unassign_calls == 1 && g_log.last_unassign_count == 3,
       "charge_step: re-staffs exactly like UPGRADING's uses_workers==0 arm");
}

// ALWAYS, regardless of which (if any) of the four gates matched: fall back to the config-driven
// idle state and notify the UI. A state matching NONE of the four gates still gets this tail.
void test_always_idle_fallback_and_notify() {
    sim_fixture f;
    sim_store   own                             = f.store();
    f.b(PLAYER, BLDG_IX).state                  = ST_OTHER;
    f.b(PLAYER, BLDG_IX).building_id            = 12;
    f.cfg_buildings[12].state_transition_ids[1] = 0x64;
    g_log.reset();

    detail::bldg_finish_current_order(f.view(), own, recording_calls(), PLAYER, BLDG_IX);

    ck(g_log.apply_production_calls == 0 && g_log.apply_project_calls == 0 &&
           g_log.grant_type_calls == 0 && g_log.uses_workers_calls == 0,
       "no-match state: none of the four gated branches ran");
    ck(f.b(PLAYER, BLDG_IX).state == 0x64,
       "no-match state: state STILL falls back to cfg_buildings[building_id].state_transition_ids[1]");
    ck(g_log.notify_ui_calls == 1, "no-match state: bldg_notify_ui STILL called -- this tail is unconditional");

    // And on a MATCHED state (PROD_WORKING), the same tail runs too, overwriting whatever the branch
    // itself left in `state` -- the fallback is unconditional, not an "else".
    f.b(PLAYER, BLDG_IX).state  = ST_PROD_WORKING;
    f.b(PLAYER, BLDG_IX).sub_id = 0;
    g_log.reset();
    detail::bldg_finish_current_order(f.view(), own, recording_calls(), PLAYER, BLDG_IX);
    ck(f.b(PLAYER, BLDG_IX).state == 0x64,
       "matched state (PROD_WORKING): the idle-state fallback STILL runs afterward and wins");
    ck(g_log.notify_ui_calls == 1, "matched state: bldg_notify_ui still called exactly once");
}

} // namespace

void run_bldg_finish_order_tests() {
    test_prod_working();
    test_researching();
    test_upgrading();
    test_charge_step();
    test_always_idle_fallback_and_notify();
}

} // namespace mh::sim::test
