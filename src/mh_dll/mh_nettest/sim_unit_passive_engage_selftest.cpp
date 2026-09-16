//
// sim_unit_passive_engage_selftest.cpp -- `simtest` cases for llm_strat_unit_passive_engage_tick
// (sim/sim_unit_passive_engage.{h,cpp}), SIM1A.
//
// Own recording calls struct (9 members) -- one instance, reconfigured per case via g_log, same
// shape as sim_debug_roll_random_selftest.cpp's log_t but wider (this closure has 9 callees against
// that one's 2).
//
#include "sim/sim_unit_passive_engage.h"

#include <limits>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

struct log_t {
    int     order_pending_calls  = 0;
    int32_t order_pending_return = 0;

    int     idle_or_patrolling_calls  = 0;
    int32_t idle_or_patrolling_return = 0;

    // scan_targets_for_engage fires from up to 3 sites in one call (step 1/6, the tail) -- FIRST-call
    // args matter for telling which site ran first, so both first and last are tracked rather than
    // just "last" (which the tail's own call would otherwise silently overwrite).
    int     scan_targets_for_engage_calls = 0;
    int32_t first_scan_targets_arg        = -1;
    int32_t last_scan_targets_arg         = -1;
    // When set, the scan "finds something": writes a nonzero engage-candidate scratch count through
    // this raw pointer, simulating what the REAL callee would do as a side effect. A plain function
    // pointer (the calls struct's shape, matching the live binding) cannot capture `own`, so the test
    // wires this up by address instead -- see the per-case setup.
    int32_t *scan_targets_writes_scratch       = nullptr;
    int32_t  scan_targets_writes_scratch_value = 0;

    int     target_ref_is_alive_calls  = 0;
    int32_t target_ref_is_alive_return = 1;

    int     engage_select_and_commit_calls  = 0;
    int32_t engage_select_and_commit_return = 0;

    int scan_target_list_for_engage_candidates_calls = 0;

    int      unit_scan_engage_candidates_in_range_calls = 0;
    uint32_t first_range_mask                           = 0;
    uint32_t last_range_mask                            = 0;

    int engage_filter_and_commit_target_calls = 0;

    int unit_issue_default_order_calls = 0;

    void reset() { *this = log_t{}; }
};
log_t g_log;

const unit_passive_engage_calls &recording_calls() {
    static const unit_passive_engage_calls c = {
        [](uint32_t, uint32_t) -> uint32_t {
            ++g_log.order_pending_calls;
            return (uint32_t)g_log.order_pending_return;
        },
        [](int32_t, int32_t) -> int32_t {
            ++g_log.idle_or_patrolling_calls;
            return g_log.idle_or_patrolling_return;
        },
        [](int32_t, int32_t target_unit_id) -> int32_t {
            if (g_log.scan_targets_for_engage_calls == 0) g_log.first_scan_targets_arg = target_unit_id;
            ++g_log.scan_targets_for_engage_calls;
            g_log.last_scan_targets_arg = target_unit_id;
            if (g_log.scan_targets_writes_scratch)
                *g_log.scan_targets_writes_scratch = g_log.scan_targets_writes_scratch_value;
            return 0;
        },
        [](uint32_t, int32_t) -> int32_t {
            ++g_log.target_ref_is_alive_calls;
            return g_log.target_ref_is_alive_return;
        },
        [](int32_t, int32_t, int32_t) -> int32_t {
            ++g_log.engage_select_and_commit_calls;
            return g_log.engage_select_and_commit_return;
        },
        [](int32_t, uint16_t) -> int32_t {
            ++g_log.scan_target_list_for_engage_candidates_calls;
            return 0;
        },
        [](int32_t, int32_t, uint32_t mask) -> int32_t {
            if (g_log.unit_scan_engage_candidates_in_range_calls == 0) g_log.first_range_mask = mask;
            ++g_log.unit_scan_engage_candidates_in_range_calls;
            g_log.last_range_mask = mask;
            return 0;
        },
        [](uint32_t, uint32_t) -> int32_t {
            ++g_log.engage_filter_and_commit_target_calls;
            return 0;
        },
        [](uint32_t, int32_t) -> void { ++g_log.unit_issue_default_order_calls; },
    };
    return c;
}

// Seeds a fixture with exactly ONE occupied roster slot (unit_id 1) for `player`, and returns it by
// reference. `remaining` (units[player][0].unit_above, little-endian) is set to 1 -- the count-driven
// walk idiom sim_unit_predicates_selftest.cpp's sibling file does not need but this one does, since
// the whole body under test lives inside the roster loop.
unit &seed_one_unit(sim_fixture &f, int32_t player) {
    f.u(player, 0).unit_above[0] = 1;
    f.u(player, 0).unit_above[1] = 0;
    unit &u                      = f.u(player, 1);
    u.unit_proto_id              = 7; // any nonzero proto id -- "occupied"
    u.energy                     = 10.0;
    u.target2_ref                = 0; // tail runs by default unless a case overrides it
    return u;
}

// ---- ENTRY: the diagonal self-relation write, independent of the roster walk (remaining=0) --------
void test_entry_diagonal_write() {
    sim_fixture f;
    sim_store   own         = f.store();
    f.u(3, 0).unit_above[0] = 0;
    f.u(3, 0).unit_above[1] = 0; // remaining=0 -- the loop body never runs

    f.foreign_bldg_change_flag = 0;
    g_log.reset();
    detail::unit_passive_engage_tick(f.view(), own, recording_calls(), 3);
    ck(f.players[3].ai_player_relation[3] == 1,
       "entry: FOREIGN_BLDG_CHANGE_FLAG == 0 -> ai_player_relation[player][player] = 1");

    f.foreign_bldg_change_flag = 1;
    g_log.reset();
    detail::unit_passive_engage_tick(f.view(), own, recording_calls(), 3);
    ck(f.players[3].ai_player_relation[3] == -1,
       "entry: FOREIGN_BLDG_CHANGE_FLAG != 0 -> ai_player_relation[player][player] = -1");

    ck(g_log.order_pending_calls == 0 && g_log.scan_targets_for_engage_calls == 0,
       "entry: no callee fires when the roster walk sees zero occupied slots");
}

// ---- EXIT: ai_target_list_count is written on every call, regardless of what the walk did --------
void test_exit_writes_target_list_count() {
    sim_fixture f;
    sim_store   own                   = f.store();
    f.players[5].ai_target_list_count = 999;
    f.u(5, 0).unit_above[0]           = 0;
    f.u(5, 0).unit_above[1]           = 0;

    detail::unit_passive_engage_tick(f.view(), own, recording_calls(), 5);
    ck(f.players[5].ai_target_list_count == 0,
       "exit: player_data[player].ai_target_list_count = 0 on the ONLY exit path");
}

// ---- energy <= 0.0 (ordered) skips the unit entirely; NaN energy does NOT (x87 unordered) ---------
void test_energy_gate() {
    sim_fixture f;
    sim_store   own = f.store();

    unit &u  = seed_one_unit(f, 1);
    u.energy = 0.0;
    g_log.reset();
    detail::unit_passive_engage_tick(f.view(), own, recording_calls(), 1);
    ck(g_log.order_pending_calls == 0,
       "energy gate: energy == 0.0 (ordered <=) skips the unit -- no callee fires");

    u.energy = -5.0;
    g_log.reset();
    detail::unit_passive_engage_tick(f.view(), own, recording_calls(), 1);
    ck(g_log.order_pending_calls == 0, "energy gate: negative energy skips the unit");

    // THE FIX THIS CASE GUARDS (reimpl-verify finding, 2026-08-10): a NaN energy must NOT be
    // treated as <= 0 -- the original's x87 JNC does not take the skip branch on an unordered
    // compare, so a NaN-energy unit is processed exactly like a positive-energy one.
    u.energy = std::numeric_limits<double>::quiet_NaN();
    g_log.reset();
    detail::unit_passive_engage_tick(f.view(), own, recording_calls(), 1);
    ck(g_log.order_pending_calls == 1,
       "energy gate: NaN energy does NOT skip the unit (x87 unordered != ordered <=0, matches JNC)");

    u.energy = 10.0;
    g_log.reset();
    detail::unit_passive_engage_tick(f.view(), own, recording_calls(), 1);
    ck(g_log.order_pending_calls == 1, "energy gate: positive energy processes the unit normally");
}

// ---- not order-pending, NOT idle/patrolling: no-op, falls straight through to the shared tail -----
void test_not_idle_falls_to_tail_only() {
    sim_fixture f;
    sim_store   own = f.store();
    unit       &u   = seed_one_unit(f, 2);

    g_log.reset();
    g_log.order_pending_return      = 0;
    g_log.idle_or_patrolling_return = 0; // not idle/patrolling -> no-op branch
    detail::unit_passive_engage_tick(f.view(), own, recording_calls(), 2);

    ck(g_log.engage_select_and_commit_calls == 0,
       "not-idle: the graduated target-seek cascade never runs");
    // The tail (target2_ref == 0): scan_targets_for_engage, scan_target_list_for_engage_candidates,
    // unit_scan_engage_candidates_in_range(0xa0), engage_filter_and_commit_target -- one call each.
    ck(g_log.scan_targets_for_engage_calls == 1 && g_log.last_scan_targets_arg == 1,
       "not-idle: tail's scan_targets_for_engage(player, unit_id) still runs, arg is unit_id (1)");
    ck(g_log.scan_target_list_for_engage_candidates_calls == 1,
       "not-idle: tail's AI-group scan still runs");
    ck(g_log.unit_scan_engage_candidates_in_range_calls == 1 && g_log.last_range_mask == 0xa0u,
       "not-idle: tail's range scan uses mask 0xa0 (NOT the graduated cascade's 0xe0)");
    ck(g_log.engage_filter_and_commit_target_calls == 1, "not-idle: tail commits via filter_and_commit_target");
    (void)u;
}

// ---- idle, no existing passive-engage target: full graduated cascade, all steps fail --------------
void test_idle_full_cascade_all_fail() {
    sim_fixture f;
    sim_store   own               = f.store();
    unit       &u                 = seed_one_unit(f, 2);
    u.passive_engage_target_index = 0; // skip step (1/6)

    g_log.reset();
    g_log.order_pending_return            = 0;
    g_log.idle_or_patrolling_return       = 1;
    g_log.engage_select_and_commit_return = 0; // every commit attempt fails

    detail::unit_passive_engage_tick(f.view(), own, recording_calls(), 2);

    ck(g_log.target_ref_is_alive_calls == 0,
       "idle/no-target: passive_engage_target_index == 0 skips the re-validate step entirely");
    // Steps (2/6),(3/6),(4/6) each try engage_select_and_commit once -> 3 calls; the tail's
    // engage_filter_and_commit_target is a DIFFERENT callee and does not add to this count.
    ck(g_log.engage_select_and_commit_calls == 3,
       "idle/no-target: all three graduated cascade steps attempt a commit (2/6, 3/6, 4/6)");
    // Falls through into the tail afterward (target2_ref == 0 by seed_one_unit's default), which
    // makes its OWN unit_scan_engage_candidates_in_range(0xa0) call -- so 2 total, first one 0xe0.
    ck(g_log.unit_scan_engage_candidates_in_range_calls == 2 && g_log.first_range_mask == 0xe0u &&
           g_log.last_range_mask == 0xa0u,
       "idle/no-target: the cascade's last-resort range scan uses mask 0xe0, THEN the tail's own "
       "range scan (mask 0xa0) runs on fallthrough");
    ck(g_log.engage_filter_and_commit_target_calls == 1,
       "idle/no-target: falls through into the shared tail after the cascade");
}

// ---- idle, existing passive-engage target commits immediately: goto tail, cascade never runs ------
void test_idle_existing_target_commits_immediately() {
    sim_fixture f;
    sim_store   own               = f.store();
    unit       &u                 = seed_one_unit(f, 2);
    u.passive_engage_target_index = 42;
    u.passive_engage_target_ref   = 0x13;

    g_log.reset();
    g_log.order_pending_return            = 0;
    g_log.idle_or_patrolling_return       = 1;
    g_log.target_ref_is_alive_return      = 1; // target still alive -- index is NOT cleared
    g_log.engage_select_and_commit_return = 1; // commits on the FIRST attempt

    detail::unit_passive_engage_tick(f.view(), own, recording_calls(), 2);

    // Step (1/6) scans the EXISTING target index (42) first; the tail (reached via goto) makes its
    // OWN scan_targets_for_engage(player, unit_id) call afterward -- 2 total, first one is 42.
    ck(g_log.scan_targets_for_engage_calls == 2 && g_log.first_scan_targets_arg == 42 &&
           g_log.last_scan_targets_arg == 1,
       "idle/existing-target: step (1/6) scans around the EXISTING target index (42) first, not "
       "unit_id -- the tail's own scan (unit_id=1) follows via the goto");
    ck(g_log.engage_select_and_commit_calls == 1,
       "idle/existing-target: committing on the first attempt skips steps 2-4 entirely (goto tail)");
    ck(own.unit_at(2, 1).passive_engage_target_index == 42,
       "idle/existing-target: target still alive -> passive_engage_target_index is NOT cleared");
    ck(g_log.engage_filter_and_commit_target_calls == 1,
       "idle/existing-target: the goto still lands in the shared tail (target2_ref == 0)");
    (void)u;
}

// ---- idle, existing target no longer alive: index is cleared, cascade continues from step (2/6) ---
void test_idle_existing_target_dead_clears_and_continues() {
    sim_fixture f;
    sim_store   own               = f.store();
    unit       &u                 = seed_one_unit(f, 2);
    u.passive_engage_target_index = 42;

    g_log.reset();
    g_log.order_pending_return            = 0;
    g_log.idle_or_patrolling_return       = 1;
    g_log.target_ref_is_alive_return      = 0; // dead -- clear the index
    g_log.engage_select_and_commit_return = 0; // and step (1/6)'s own commit attempt also fails

    detail::unit_passive_engage_tick(f.view(), own, recording_calls(), 2);

    ck(own.unit_at(2, 1).passive_engage_target_index == 0,
       "idle/existing-target: target no longer alive -> passive_engage_target_index cleared to 0");
    // Step (1/6) still attempted a commit (1 call) before falling into (2/6),(3/6),(4/6) (3 more).
    ck(g_log.engage_select_and_commit_calls == 4,
       "idle/existing-target: a failed re-validate falls through into the full 3-step cascade too");
    (void)u;
}

// ---- order-pending, engaged + building target: re-scan and conditionally re-issue -----------------
void test_order_pending_engaged_building_reissue() {
    sim_fixture f;
    sim_store   own    = f.store();
    unit       &u      = seed_one_unit(f, 4);
    u.engagement_flags = 0x1;
    u.target_ref       = 0x40; // building bit set

    g_log.reset();
    g_log.order_pending_return = 1; // order-pending -> the OTHER branch
    // The plain-function-pointer calls struct can't capture `own`, so simulate "the scan found
    // something" by writing through a raw pointer into the SAME scratch-count storage `own` reads,
    // taken by address before the call -- see log_t's comment.
    g_log.scan_targets_writes_scratch       = &own.engage_candidate_scratch_count();
    g_log.scan_targets_writes_scratch_value = 3; // nonzero -- the reissue check reads this
    detail::unit_passive_engage_tick(f.view(), own, recording_calls(), 4);

    ck(g_log.idle_or_patrolling_calls == 0,
       "order-pending: the idle/patrolling branch is never consulted");
    // The order-pending branch's own re-scan runs first; target2_ref == 0 (seed_one_unit's default)
    // also runs the shared tail afterward, which makes its OWN scan_targets_for_engage call -- 2 total.
    ck(g_log.scan_targets_for_engage_calls == 2,
       "order-pending+engaged+building: re-scans once for the reissue check, once more in the tail");
    ck(g_log.unit_issue_default_order_calls == 1,
       "order-pending+engaged+building: nonzero scratch count after the scan -> re-issues the default order");
    (void)u;
}

void test_order_pending_no_reissue_when_scratch_stays_empty() {
    sim_fixture f;
    sim_store   own    = f.store();
    unit       &u      = seed_one_unit(f, 4);
    u.engagement_flags = 0x1;
    u.target_ref       = 0x40;

    g_log.reset();
    g_log.order_pending_return = 1;
    // scan_targets_writes_scratch left null -- the scan "finds nothing", scratch count stays 0.
    detail::unit_passive_engage_tick(f.view(), own, recording_calls(), 4);

    ck(g_log.unit_issue_default_order_calls == 0,
       "order-pending+engaged+building: scratch count stays 0 after the scan -> no re-issue");
    (void)u;
}

void test_order_pending_not_engaged_skips_inner_check() {
    sim_fixture f;
    sim_store   own    = f.store();
    unit       &u      = seed_one_unit(f, 4);
    u.engagement_flags = 0; // bit 0x1 clear -- the inner check's first condition already fails
    u.target_ref       = 0x40;

    g_log.reset();
    g_log.order_pending_return = 1;
    detail::unit_passive_engage_tick(f.view(), own, recording_calls(), 4);

    ck(g_log.scan_targets_for_engage_calls == 1,
       "order-pending+not-engaged: skips the inner re-scan/re-issue check, but STILL reaches the "
       "shared tail (which has its own scan_targets_for_engage call)");
    ck(g_log.unit_issue_default_order_calls == 0,
       "order-pending+not-engaged: never re-issues (the inner check's first condition already failed)");
    (void)u;
}

// ---- the shared tail is gated on target2_ref, independent of which branch reached it ---------------
void test_tail_gated_by_target2_ref() {
    sim_fixture f;
    sim_store   own               = f.store();
    unit       &u                 = seed_one_unit(f, 2);
    u.target2_ref                 = 0x99; // nonzero -- the tail body must NOT run
    u.passive_engage_target_index = 0;

    g_log.reset();
    g_log.order_pending_return      = 0;
    g_log.idle_or_patrolling_return = 0; // no-op branch -> falls straight to the (gated) tail
    detail::unit_passive_engage_tick(f.view(), own, recording_calls(), 2);

    ck(g_log.scan_targets_for_engage_calls == 0 && g_log.engage_filter_and_commit_target_calls == 0,
       "tail gate: target2_ref != 0 suppresses the ENTIRE tail body, even when every earlier branch "
       "was a pure fallthrough");
    (void)u;
}

} // namespace

void run_unit_passive_engage_tests() {
    test_entry_diagonal_write();
    test_exit_writes_target_list_count();
    test_energy_gate();
    test_not_idle_falls_to_tail_only();
    test_idle_full_cascade_all_fail();
    test_idle_existing_target_commits_immediately();
    test_idle_existing_target_dead_clears_and_continues();
    test_order_pending_engaged_building_reissue();
    test_order_pending_no_reissue_when_scratch_stays_empty();
    test_order_pending_not_engaged_skips_inner_check();
    test_tail_gated_by_target2_ref();
}

} // namespace mh::sim::test
