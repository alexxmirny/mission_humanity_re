//
// sim_storage_purge_dead_docked_selftest.cpp -- offline `simtest` oracle for
//   llm_strat_storage_purge_dead_docked @0x0049b8f5  (sim/sim_storage_purge_dead_docked.h/.cpp)
//
// WHY AN OFFLINE ORACLE. This function is translated + reimpl-verify clean but was left `reviewed`
// (no evidence tier) -- it had never been given execution evidence. It makes exactly one outward
// call (llm_strat_storage_remove_docked_unit, indirected through storage_purge_dead_docked_calls),
// so its own read/branch logic is fully exercisable against a recording stub without a live game
// process. This file is that stub + one test per distinct branch, promoting the function from
// "reviewed" to "verified".
//
// EVERY EXPECTED VALUE IS DERIVED FROM sim_storage_purge_dead_docked.h's OWN BANNER (address-cited
// against the raw disassembly), not from the .cpp under test. Re-cited inline per case.
//
// A NOTE ON THE ONE THING THIS FILE DOES NOT EXERCISE: the header's "THE HOME_STORAGE_SLOT WRITE'S
// WIDTH" section documents that the write stores only `static_cast<uint8_t>(storage_sub_id)`, i.e.
// a byte-truncating store. Genuinely proving truncation (as opposed to merely proving the write
// happens with the right value) needs a `storage_sub_id` whose low byte differs from its full
// 32-bit value, i.e. `storage_sub_id > 255`. That is NOT safe to construct here: `storage_sub_id`
// is not just the value written into home_storage_slot -- the same parameter is also used, live,
// as the direct index into `unit_storage[player][storage_sub_id]` (see the .cpp's
// `storage_of(v, p, storage_sub_id)`), and the fixture's `storage` vector is sized at the REAL
// extent `MAX_PLAYERS(8) * STORAGE_PER_PLAYER(25)` = 200 entries with no slack (unlike e.g.
// `tile_objects`/`passable`, which are padded to their full domain specifically so an out-of-range
// index lands in fixture memory instead of overrunning it). Any `storage_sub_id` big enough to
// prove truncation (>255) indexes far past those 200 entries -- a real heap-buffer-overflow, not a
// faithful "large drift" case. test_repair_fires_on_drift_with_byte_truncation below therefore uses
// the largest SAFE in-range value (STORAGE_PER_PLAYER-1 == 24) to prove the write applies the
// correct `static_cast<uint8_t>(...)` value end-to-end; it cannot additionally distinguish that
// from a hypothetical non-truncating write, since 24 fits a byte either way. Flagged as an open
// uncertainty rather than silently skipped.
//
#include <array>

#include "sim/sim_storage_purge_dead_docked.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// Seed a docked-unit list entry directly in the fixture's real-extent storage record -- same
// pattern as sim_bldg_scrap_stored_units_selftest.cpp's storage seeding helper.
unit_storage &storage_rec(sim_fixture &f, uint32_t player, int32_t slot) {
    return f.storage[(size_t)player * (size_t)STORAGE_PER_PLAYER + (size_t)slot];
}

// ---- recorder --------------------------------------------------------------------------------
// The one outward call this function makes: llm_strat_storage_remove_docked_unit via
// storage_purge_dead_docked_calls::storage_remove_docked_unit(uint16_t player, int32_t unit_index,
// int32_t storage_slot). A captureless lambda converts to the stub's raw function-pointer member,
// same shape as sim_prod_completion_selftest.cpp's recorders. Per the task brief, this stub only
// RECORDS the call -- it deliberately does not simulate the callee's documented list-compacting
// side effect (header banner: "roster_write_via" / the mid-walk-mutation note); that is the
// callee's own closure, not this function's own read/branch logic under test here.
struct spd_recorder {
    std::vector<std::array<int32_t, 3>> remove_calls; // (player, unit_index, storage_slot)
    void                                reset() { *this = spd_recorder{}; }
};
spd_recorder g_spd;

const storage_purge_dead_docked_calls &rec_spd_calls() {
    static const storage_purge_dead_docked_calls c = {
        [](uint16_t player, int32_t unit_index, int32_t storage_slot) {
            g_spd.remove_calls.push_back({(int32_t)player, unit_index, storage_slot});
        },
    };
    return c;
}

// ==== llm_strat_storage_purge_dead_docked ==========================================================
// From the header banner: for i in [0, docked_count) (BOTH re-read from the fixture every visit,
// 0x0049b919-0x0049b959), (1) repair home_storage_slot if it drifted from storage_sub_id
// (0x0049b972-0x0049b991, byte-truncating write), (2) remove the unit if energy<=0.0
// (0x0049b9a7-0x0049b9be), both tested independently every visited iteration (not else-if), and the
// index increment (0x0049b939-0x0049b93f) is unconditional regardless of whether remove fired.

void test_empty_docked_list_zero_iterations() {
    sim_fixture f;
    g_spd.reset();
    constexpr int32_t PLAYER                            = 5;
    constexpr int32_t STORAGE_SUB_ID                    = 11;
    storage_rec(f, PLAYER, STORAGE_SUB_ID).docked_count = 0; // 0x0049b919-0x0049b929: bound check fails immediately

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::storage_purge_dead_docked(v, own, rec_spd_calls(), PLAYER, STORAGE_SUB_ID);

    ck_eq((uint32_t)g_spd.remove_calls.size(), 0u,
          "empty docked list: docked_count==0 -> zero loop visits -> zero remove calls");
}

void test_home_storage_slot_already_matching_no_repair_write() {
    sim_fixture f;
    g_spd.reset();
    constexpr int32_t PLAYER         = 2;
    constexpr int32_t STORAGE_SUB_ID = 19;
    constexpr int32_t UNIT_INDEX     = 44;
    constexpr double  ENERGY         = 17.5; // > 0.0, isolates this case from the remove branch

    unit_storage &s     = storage_rec(f, PLAYER, STORAGE_SUB_ID);
    s.docked_count      = 1;
    s.docked_units[0]   = UNIT_INDEX;
    unit &u             = f.u(PLAYER, UNIT_INDEX);
    u.home_storage_slot = (uint8_t)STORAGE_SUB_ID; // already matches -> 0x0049b979 compare is EQUAL
    u.energy            = ENERGY;

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::storage_purge_dead_docked(v, own, rec_spd_calls(), PLAYER, STORAGE_SUB_ID);

    ck_eq((uint32_t)u.home_storage_slot, (uint32_t)STORAGE_SUB_ID,
          "home_storage_slot already == storage_sub_id: field stays exactly as seeded (repair branch "
          "does not fire, 0x0049b97c JZ-over-the-write)");
    ck_eq((uint32_t)g_spd.remove_calls.size(), 0u,
          "home_storage_slot already matching, energy>0: no remove call either");
}

void test_repair_fires_on_drift_with_byte_truncation() {
    sim_fixture f;
    g_spd.reset();
    constexpr int32_t PLAYER         = 6;
    constexpr int32_t STORAGE_SUB_ID = 24; // largest SAFE in-range value -- see file banner's open
                                           // uncertainty note on why >255 cannot be used here
    constexpr int32_t UNIT_INDEX = 53;
    constexpr double  ENERGY     = 8.25; // > 0.0, isolates this case from the remove branch

    unit_storage &s     = storage_rec(f, PLAYER, STORAGE_SUB_ID);
    s.docked_count      = 1;
    s.docked_units[0]   = UNIT_INDEX;
    unit &u             = f.u(PLAYER, UNIT_INDEX);
    u.home_storage_slot = 250; // != STORAGE_SUB_ID(24) -> 0x0049b979 compare is NOT-EQUAL, drift
    u.energy            = ENERGY;

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::storage_purge_dead_docked(v, own, rec_spd_calls(), PLAYER, STORAGE_SUB_ID);

    ck_eq((uint32_t)u.home_storage_slot, (uint32_t)(uint8_t)STORAGE_SUB_ID,
          "drifted home_storage_slot: repaired to static_cast<uint8_t>(storage_sub_id), 0x0049b991");
    ck_eq((uint32_t)g_spd.remove_calls.size(), 0u,
          "drift repair fires, energy>0: no remove call (independent branch, not else-if -- this half "
          "proven false)");
}

void test_energy_at_or_below_zero_fires_remove_with_right_args() {
    sim_fixture f;
    g_spd.reset();
    constexpr int32_t PLAYER         = 3;
    constexpr int32_t STORAGE_SUB_ID = 6;
    constexpr int32_t UNIT_INDEX     = 71;
    constexpr double  ENERGY         = 0.0; // boundary: energy<=0.0, not merely <0.0 (0x0049b9a7 FCOMP)

    unit_storage &s     = storage_rec(f, PLAYER, STORAGE_SUB_ID);
    s.docked_count      = 1;
    s.docked_units[0]   = UNIT_INDEX;
    unit &u             = f.u(PLAYER, UNIT_INDEX);
    u.home_storage_slot = (uint8_t)STORAGE_SUB_ID; // already matches -> isolates this case from repair
    u.energy            = ENERGY;

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::storage_purge_dead_docked(v, own, rec_spd_calls(), PLAYER, STORAGE_SUB_ID);

    ck_eq((uint32_t)g_spd.remove_calls.size(), 1u,
          "energy==0.0 (<=0.0 boundary): exactly one remove call, 0x0049b9be");
    ck(g_spd.remove_calls.size() == 1 && g_spd.remove_calls[0][0] == PLAYER &&
           g_spd.remove_calls[0][1] == UNIT_INDEX && g_spd.remove_calls[0][2] == STORAGE_SUB_ID,
       "remove call args are (player narrowed to uint16, unit_index, storage_sub_id) exactly, per "
       "0x0049b9ba's MOVZX word / the callee's committed prototype");
}

void test_energy_positive_no_remove() {
    sim_fixture f;
    g_spd.reset();
    constexpr int32_t PLAYER         = 1;
    constexpr int32_t STORAGE_SUB_ID = 14;
    constexpr int32_t UNIT_INDEX     = 88;
    constexpr double  ENERGY         = 33.75; // > 0.0

    unit_storage &s     = storage_rec(f, PLAYER, STORAGE_SUB_ID);
    s.docked_count      = 1;
    s.docked_units[0]   = UNIT_INDEX;
    unit &u             = f.u(PLAYER, UNIT_INDEX);
    u.home_storage_slot = (uint8_t)STORAGE_SUB_ID; // isolate from the repair branch
    u.energy            = ENERGY;

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::storage_purge_dead_docked(v, own, rec_spd_calls(), PLAYER, STORAGE_SUB_ID);

    ck_eq((uint32_t)g_spd.remove_calls.size(), 0u,
          "energy>0.0: 0x0049b9a7 FCOMP/JC does not take the remove branch");
}

void test_repair_and_remove_both_fire_independently_same_iteration() {
    sim_fixture f;
    g_spd.reset();
    constexpr int32_t PLAYER         = 7;
    constexpr int32_t STORAGE_SUB_ID = 23;
    constexpr int32_t UNIT_INDEX     = 61;
    constexpr double  ENERGY         = -4.5; // < 0.0, well past the <=0.0 boundary

    unit_storage &s     = storage_rec(f, PLAYER, STORAGE_SUB_ID);
    s.docked_count      = 1;
    s.docked_units[0]   = UNIT_INDEX;
    unit &u             = f.u(PLAYER, UNIT_INDEX);
    u.home_storage_slot = 200; // drifted -> repair fires too
    u.energy            = ENERGY;

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::storage_purge_dead_docked(v, own, rec_spd_calls(), PLAYER, STORAGE_SUB_ID);

    ck_eq((uint32_t)u.home_storage_slot, (uint32_t)(uint8_t)STORAGE_SUB_ID,
          "both-fire iteration: repair branch DID fire (home_storage_slot repaired)");
    ck_eq((uint32_t)g_spd.remove_calls.size(), 1u,
          "both-fire iteration: remove branch ALSO fired in the SAME visit -- proves the two "
          "conditions are independent, sequential checks (0x0049b97c/0x0049b991-0x0049b997), not "
          "else-if");
}

// The unconditional per-visit index increment (0x0049b939-0x0049b93f). The stub records calls but,
// per the task brief, deliberately does NOT simulate the real callee's list-compacting side effect,
// so docked_count/docked_units[] here stay exactly as seeded across the whole walk -- which is
// exactly what lets this test prove the ++i itself is unconditional: units AFTER an earlier
// "removed" slot must still be visited (and repaired) exactly once each.
void test_multi_unit_every_slot_visited_regardless_of_earlier_removes() {
    sim_fixture f;
    g_spd.reset();
    constexpr int32_t PLAYER         = 4;
    constexpr int32_t STORAGE_SUB_ID = 17;
    constexpr int32_t UNIT_A         = 8;  // removed (energy<=0), already matching -> repair does NOT fire
    constexpr int32_t UNIT_B         = 15; // kept (energy>0), drifted -> repair fires
    constexpr int32_t UNIT_C         = 23; // removed (energy==0.0 boundary), already matching
    constexpr int32_t UNIT_D         = 31; // kept (energy>0), drifted -> repair fires

    unit_storage &s   = storage_rec(f, PLAYER, STORAGE_SUB_ID);
    s.docked_count    = 4;
    s.docked_units[0] = UNIT_A;
    s.docked_units[1] = UNIT_B;
    s.docked_units[2] = UNIT_C;
    s.docked_units[3] = UNIT_D;

    unit &ua             = f.u(PLAYER, UNIT_A);
    ua.home_storage_slot = (uint8_t)STORAGE_SUB_ID;
    ua.energy            = -1.0;

    unit &ub             = f.u(PLAYER, UNIT_B);
    ub.home_storage_slot = 200;
    ub.energy            = 9.0;

    unit &uc             = f.u(PLAYER, UNIT_C);
    uc.home_storage_slot = (uint8_t)STORAGE_SUB_ID;
    uc.energy            = 0.0;

    unit &ud             = f.u(PLAYER, UNIT_D);
    ud.home_storage_slot = 250;
    ud.energy            = 3.0;

    sim_view  v   = f.view();
    sim_store own = f.store();
    detail::storage_purge_dead_docked(v, own, rec_spd_calls(), PLAYER, STORAGE_SUB_ID);

    ck_eq((uint32_t)g_spd.remove_calls.size(), 2u,
          "multi-unit: exactly 2 of 4 visited slots pass the energy<=0 gate (A and C)");
    ck(g_spd.remove_calls.size() == 2 && g_spd.remove_calls[0][1] == UNIT_A &&
           g_spd.remove_calls[1][1] == UNIT_C,
       "multi-unit: remove calls fired for UNIT_A then UNIT_C, in visit order i=0 then i=2");
    ck_eq((uint32_t)ub.home_storage_slot, (uint32_t)(uint8_t)STORAGE_SUB_ID,
          "multi-unit: UNIT_B (i=1, right after a removed slot) was still visited and repaired -- "
          "the ++i did not skip it");
    ck_eq((uint32_t)ud.home_storage_slot, (uint32_t)(uint8_t)STORAGE_SUB_ID,
          "multi-unit: UNIT_D (i=3, the last slot, right after another removed slot) was still "
          "visited and repaired -- the unconditional increment ran for every seeded slot regardless "
          "of whether remove fired earlier in the walk");
}

} // namespace

void run_storage_purge_dead_docked_tests() {
    test_empty_docked_list_zero_iterations();
    test_home_storage_slot_already_matching_no_repair_write();
    test_repair_fires_on_drift_with_byte_truncation();
    test_energy_at_or_below_zero_fires_remove_with_right_args();
    test_energy_positive_no_remove();
    test_repair_and_remove_both_fire_independently_same_iteration();
    test_multi_unit_every_slot_visited_regardless_of_earlier_removes();
}

} // namespace mh::sim::test
