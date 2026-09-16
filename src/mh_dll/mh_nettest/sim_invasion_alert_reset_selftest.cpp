#include "sim/resid/sim_invasion_alert_reset.h"

#include <cstdio>
#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

std::vector<int32_t> g_clear_calls;

// Set by the test case to &own.advisor_next_time() before the call so this mock can PROVE
// ordering, not just occurrence: it stomps the same storage with a sentinel on every clear call,
// so the post-call value reading back 0.0 (not the sentinel) is only possible if the
// advisor_next_time store @0x0049b480/0x0049b48a executed strictly AFTER the 32nd clear call --
// i.e. all 32 clears precede the store. nullptr means "not probing" (left null between cases).
double *g_order_probe_ptr = nullptr;

// Distinct from 0.0 and from the seed the test case writes to advisor_next_time below, so a
// misordered store (or a store that silently never fires) reads back as neither the seed nor 0.0.
constexpr double kOrderSentinel = -13.25;

void rec_invasion_alert_clear(int32_t planet) {
    g_clear_calls.push_back(planet);
    if (g_order_probe_ptr) *g_order_probe_ptr = kOrderSentinel;
}

const invasion_alert_reset_calls g_calls = {
    &rec_invasion_alert_clear,
};

void reset_recorders() {
    g_clear_calls.clear();
    g_order_probe_ptr = nullptr;
}

} // namespace

void run_invasion_alert_reset_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- exhaustive: all 32 invasion_alert_clear(i) calls, in ascending order, both loop ends named
    // individually; then advisor_next_time zeroed, proved non-vacuous by seeding it to a distinctive
    // non-zero double first, and proved ORDERED (32 clears before the store, not just "both happened")
    // by having the clear mock stomp the same storage with a sentinel on every call.
    // =================================================================================================
    {
        fx.reset();
        reset_recorders();

        // fx.reset() already leaves advisor_next_time at 0.0, so seeding it here is what makes the
        // post-call zero-check real rather than a vacuous pass against the fixture's own default.
        // Exactly representable in double -- no rounding surprises in the comparison.
        fx.advisor_next_time = 987654.5;

        sim_store own = fx.store();
        // store() passes &advisor_next_time straight through (sim_test_support.h), so this aliases
        // the same double the seed above just wrote and own.advisor_next_time() reads.
        g_order_probe_ptr = &own.advisor_next_time();

        detail::invasion_alert_reset_all(own, g_calls);

        // ---- clear-call count: exactly 32, not "at least 32" ----------------------------------------
        ck_eq((uint32_t)g_clear_calls.size(), 32u,
              "T1: invasion_alert_clear called exactly 32 times -- loop bound `CMP dword ptr "
              "[EBP-0x18],0x20 / JL LAB_0049b476` @0x0049b466");

        // ---- ascending order, element by element -----------------------------------------------------
        for (int32_t i = 0; i < 32 && (size_t)i < g_clear_calls.size(); ++i) {
            char msg[176];
            std::snprintf(msg, sizeof(msg),
                          "T1: call #%d passed planet index %d in ascending order -- loop counter "
                          "i@[EBP-0x18] loaded just before CALL @0x0049b479, header @0x0049b466",
                          i, i);
            ck_eq((uint32_t)g_clear_calls[(size_t)i], (uint32_t)i, msg);
        }

        // ---- first/last named individually, per the mutation-testing brief ---------------------------
        ck(!g_clear_calls.empty() && g_clear_calls.front() == 0,
           "T1: FIRST clear call is planet 0 -- i initialised `MOV dword ptr [EBP-0x18],0x0` "
           "@0x0049b45f");
        ck(!g_clear_calls.empty() && g_clear_calls.back() == 31,
           "T1: LAST clear call is planet 31 -- loop bound `CMP dword ptr [EBP-0x18],0x20 / JL` "
           "@0x0049b466 (i==31 passes the test, i==32 fails it and exits)");

        // ---- advisor_next_time: zeroed, AFTER all 32 clears -------------------------------------------
        // Real because (a) it was seeded non-zero above rather than left at the fixture's default
        // 0.0, and (b) the clear mock stomps the identical storage with kOrderSentinel on every one
        // of the 32 calls -- so reading back exactly 0.0 here (not kOrderSentinel, not the seed)
        // proves the store @0x0049b480/0x0049b48a ran strictly AFTER, and overwrote, the last
        // clear's sentinel write: the 32 clears all precede the store, not merely both occur.
        ck_eq_d(own.advisor_next_time(), 0.0,
                "T1: advisor_next_time == 0.0 after the call, overwriting the clear mock's "
                "sentinel written on every one of the 32 calls -- double-store `MOV dword ptr "
                "[0xb64ba8],0x0` @0x0049b480 / `MOV dword ptr [0xb64bac],0x0` @0x0049b48a, proving "
                "the store runs strictly after all 32 clears");

        g_order_probe_ptr = nullptr;
    }
}

} // namespace mh::sim::test
