//
// sim_bldg_network_critical_selftest.cpp -- `simtest` offline oracle for
// llm_strat_bldg_is_network_critical @0x00497405 (sim/resid/sim_bldg_network_critical.h/.cpp,
// RI-SIM / sim_resid batch E).
//
// NO SHADOW SITE (sim_resid rule 1) -- this offline oracle is the only verification. Expected
// behaviour hand-derived from the DISASSEMBLY
// (tmp/decomp_sim_resid/llm_strat_bldg_is_network_critical_00497405.asm):
//
//   0x0049741c-d428  recompute #1, BEFORE anything is read.
//   0x0049742d-d444  the candidate's own built_flags & 0x1 gate -- 0 if it is not connected, and
//                    the two later recomputes must NOT happen on that arm.
//   0x00497452-d4cf  BEFORE scan: remaining = trunc(buildings[player][0].energy) -- the SENTINEL
//                    slot's energy is the live-building HEADCOUNT, not a building's HP. Walk slots
//                    1..99 while remaining != 0; a slot with energy > 0 decrements `remaining`
//                    UNCONDITIONALLY and, only if it is also connected, increments the count.
//   0x004974df-d50b  snapshot the candidate's energy (raw 8 bytes), zero it.
//   0x00497515-d528  sentinel.energy += BLDG_COUNT_OFFLINE_DECREMENT -- the SENTINEL, not the
//                    candidate. Then recompute #2.
//   0x00497539-d5b6  AFTER scan: the SAME table, same shape, re-read after the recompute.
//   0x004975b6-d5f3  restore the candidate; sentinel.energy += 1.0 (NOT -DECREMENT -- the
//                    PRESERVE-BUG asymmetry); recompute #3.
//   0x004975f8-d614  return (after + 1 < before) ? 1 : 0.
//
// The recompute is mocked, so what this oracle CANNOT prove is the real flood-fill's behaviour. What
// it does prove is the protocol around it: how many times it is called and with what, what is read
// before and after each call, and that the candidate's and the sentinel's state are left as they were
// found. The connectivity DELTA is injected by the mock, which is what makes the cut-point verdict
// testable at all offline.
//
#include "sim/resid/sim_bldg_network_critical.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- the recompute mock -------------------------------------------------------------------------
//
// It stands in for the real flood-fill, and it is what injects the connectivity change the function
// is trying to measure: on call N it applies the Nth entry of `g_flags_per_call` to
// buildings[player][1..].built_flags, so a case can say "after the candidate is taken offline, these
// buildings lose their connection".
sim_fixture                      *g_fx              = nullptr;
int32_t                           g_player_seen[8]  = {0};
int32_t                           g_recompute_calls = 0;
std::vector<std::vector<uint8_t>> g_flags_per_call;
// The sentinel headcount as it was at the top of each recompute call -- the only way to observe the
// ORDER of the bookkeeping writes relative to the recomputes.
std::vector<double> g_sentinel_at_call;

void stub_power_network_recompute(uint16_t player) {
    if (g_recompute_calls < 8) g_player_seen[g_recompute_calls] = (int32_t)player;
    g_sentinel_at_call.push_back(g_fx->buildings[(size_t)(player * BUILDINGS_PER_PLAYER)].energy);
    if ((size_t)g_recompute_calls < g_flags_per_call.size()) {
        const std::vector<uint8_t> &flags = g_flags_per_call[(size_t)g_recompute_calls];
        for (size_t i = 0; i < flags.size(); ++i) {
            g_fx->buildings[(size_t)(player * BUILDINGS_PER_PLAYER) + 1 + i].built_flags = flags[i];
        }
    }
    ++g_recompute_calls;
}

const bldg_network_critical_calls g_calls = {
    &stub_power_network_recompute,
};

void reset_calls(sim_fixture &fx) {
    g_fx              = &fx;
    g_recompute_calls = 0;
    for (int32_t i = 0; i < 8; ++i) g_player_seen[i] = -1;
    g_flags_per_call.clear();
    g_sentinel_at_call.clear();
}

int32_t run(sim_fixture &fx, int32_t player, int32_t b_index) {
    sim_store own = fx.store();
    return detail::bldg_is_network_critical(fx.view(), own, g_calls, player, b_index);
}

building &slot(sim_fixture &fx, int32_t player, int32_t i) {
    return fx.buildings[(size_t)(player * BUILDINGS_PER_PLAYER + i)];
}

// Seed `n` live buildings at slots 1..n, all connected, and set the sentinel headcount to `n`.
void seed_live(sim_fixture &fx, int32_t player, int32_t n) {
    slot(fx, player, 0).energy = (double)n;
    for (int32_t i = 1; i <= n; ++i) {
        slot(fx, player, i).energy      = 100.0;
        slot(fx, player, i).built_flags = 0x1;
    }
}

} // namespace

void run_bldg_is_network_critical_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- THE CANDIDATE IS NOT CONNECTED (built_flags bit 0 clear): return 0 after exactly ONE
    // recompute, with nothing written. Bit 0x2 is set on the candidate as a decoy, so a translation
    // testing "built_flags != 0" instead of "& 0x1" takes the wrong branch and fails here.
    // =================================================================================================
    {
        fx.reset();
        reset_calls(fx);
        seed_live(fx, /*player=*/2, /*n=*/5);
        slot(fx, 2, 3).built_flags   = 0x2; // NOT connected, but non-zero
        const double sentinel_before = slot(fx, 2, 0).energy;
        const double cand_before     = slot(fx, 2, 3).energy;

        const int32_t ret = run(fx, /*player=*/2, /*b_index=*/3);

        ck_eq((uint32_t)ret, 0u, "T1: a disconnected candidate is trivially not critical, 0x0049742d-d444");
        ck_eq((uint32_t)g_recompute_calls, 1u,
              "T1: exactly ONE recompute -- the gate is AFTER the first and returns before the others");
        ck_eq((uint32_t)g_player_seen[0], 2u, "T1: recompute is called with the player parameter, 0x0049741c");
        ck_eq_d(slot(fx, 2, 0).energy, sentinel_before, "T1: the sentinel headcount is untouched");
        ck_eq_d(slot(fx, 2, 3).energy, cand_before, "T1: the candidate's energy is untouched");
    }

    // =================================================================================================
    // T2 -- NOT CRITICAL: taking the candidate offline loses only the candidate itself. The mock drops
    // the candidate's own connected bit on recompute #2 and nothing else, so before=5, after=4, and
    // 4 + 1 < 5 is FALSE.
    //
    // Also the full protocol pin: THREE recomputes, all for this player; the candidate is restored;
    // the sentinel is left at start + DECREMENT + 1.0; and the sentinel value seen at the top of each
    // recompute proves the bookkeeping write happened BEFORE recompute #2 and the restore BEFORE #3.
    // =================================================================================================
    {
        fx.reset();
        reset_calls(fx);
        seed_live(fx, /*player=*/1, /*n=*/5);
        const double cand_before     = 100.0;
        const double sentinel_before = 5.0;
        // recompute #1: everything stays connected. #2: the candidate (slot 3) loses its bit. #3: back.
        g_flags_per_call.push_back({0x1, 0x1, 0x1, 0x1, 0x1});
        g_flags_per_call.push_back({0x1, 0x1, 0x0, 0x1, 0x1});
        g_flags_per_call.push_back({0x1, 0x1, 0x1, 0x1, 0x1});

        const int32_t ret = run(fx, /*player=*/1, /*b_index=*/3);

        ck_eq((uint32_t)ret, 0u,
              "T2: losing only the candidate is NOT critical -- (4 + 1 < 5) is false, 0x004975f8-d614");
        ck_eq((uint32_t)g_recompute_calls, 3u, "T2: exactly THREE recomputes on the full path");
        ck_eq((uint32_t)g_player_seen[1], 1u, "T2: recompute #2 also takes the player parameter");
        ck_eq((uint32_t)g_player_seen[2], 1u, "T2: recompute #3 too");
        ck_eq_d(slot(fx, 1, 3).energy, cand_before, "T2: the candidate's energy is restored, 0x004975b6");
        ck_eq_d(slot(fx, 1, 0).energy, sentinel_before + (-1.0) + 1.0,
                "T2: the sentinel ends at start + DECREMENT + 1.0 -- the PRESERVE-BUG round trip");
        // Ordering, observed through the sentinel value the mock read at each call.
        ck(g_sentinel_at_call.size() == 3, "T2: three sentinel observations");
        if (g_sentinel_at_call.size() == 3) {
            ck_eq_d(g_sentinel_at_call[0], 5.0, "T2: recompute #1 runs BEFORE any bookkeeping");
            ck_eq_d(g_sentinel_at_call[1], 5.0 + (-1.0),
                    "T2: the DECREMENT is applied BEFORE recompute #2, 0x00497515-d528");
            ck_eq_d(g_sentinel_at_call[2], 5.0 + (-1.0) + 1.0,
                    "T2: the +1.0 restore is applied BEFORE recompute #3, 0x004975df-d5e7");
        }
    }

    // =================================================================================================
    // T3 -- CRITICAL: the candidate is a cut point. Taking slot 3 offline also disconnects slots 4
    // and 5, so before=5, after=2, and 2 + 1 < 5 holds.
    // =================================================================================================
    {
        fx.reset();
        reset_calls(fx);
        seed_live(fx, /*player=*/1, /*n=*/5);
        g_flags_per_call.push_back({0x1, 0x1, 0x1, 0x1, 0x1});
        g_flags_per_call.push_back({0x1, 0x1, 0x0, 0x0, 0x0}); // the candidate + two behind it
        g_flags_per_call.push_back({0x1, 0x1, 0x1, 0x1, 0x1});

        const int32_t ret = run(fx, /*player=*/1, /*b_index=*/3);

        ck_eq((uint32_t)ret, 1u, "T3: losing two more than the candidate IS critical -- 2 + 1 < 5");
    }

    // =================================================================================================
    // T4 -- THE VERDICT BOUNDARY, which is what a `<=` / `<` mutation moves. Exactly ONE building
    // beyond the candidate is lost: before=5, after=3, and 3 + 1 < 5 holds (critical). One fewer loss
    // (after=4) is T2's not-critical case. These two cases sit either side of the same comparison.
    // =================================================================================================
    {
        fx.reset();
        reset_calls(fx);
        seed_live(fx, /*player=*/1, /*n=*/5);
        g_flags_per_call.push_back({0x1, 0x1, 0x1, 0x1, 0x1});
        g_flags_per_call.push_back({0x1, 0x1, 0x0, 0x0, 0x1}); // candidate + exactly one other
        g_flags_per_call.push_back({0x1, 0x1, 0x1, 0x1, 0x1});

        const int32_t ret = run(fx, /*player=*/1, /*b_index=*/3);

        ck_eq((uint32_t)ret, 1u,
              "T4: exactly one COLLATERAL loss is already critical -- 3 + 1 < 5, the `<` boundary");
    }

    // =================================================================================================
    // T5 -- `remaining` IS THE SCAN BUDGET, and it comes from the SENTINEL, truncated. Ten buildings
    // are live and connected but the sentinel headcount says 3.5, so the scan must stop after
    // THREE energy-positive slots -- trunc(3.5) = 3, not 4 and not 10.
    //
    // The mock leaves flags alone in both scans, so before == after and the verdict is 0; what the
    // case pins is that the scan is BUDGETED. A translation that walked all 100 slots would count 10
    // in both scans and also return 0 -- so the budget is measured directly instead, by making slots
    // 4..10 the ONLY ones that lose their bit in the second scan: they are past the budget, so the
    // AFTER count cannot see the loss and the verdict stays 0. An unbudgeted scan sees 3 vs 10 and
    // returns 1.
    // =================================================================================================
    {
        fx.reset();
        reset_calls(fx);
        seed_live(fx, /*player=*/1, /*n=*/10);
        slot(fx, 1, 0).energy = 3.5;
        std::vector<uint8_t> all_on(10, 0x1);
        std::vector<uint8_t> tail_off = {0x1, 0x1, 0x1, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0};
        g_flags_per_call.push_back(all_on);
        g_flags_per_call.push_back(tail_off);
        g_flags_per_call.push_back(all_on);

        const int32_t ret = run(fx, /*player=*/1, /*b_index=*/1);

        ck_eq((uint32_t)ret, 0u,
              "T5: the scan budget is trunc(sentinel.energy) = 3, so losses past slot 3 are invisible");
    }

    // =================================================================================================
    // T6 -- THE BUDGET IS SPENT ON ENERGY-POSITIVE SLOTS, CONNECTED OR NOT (0x004974a8-d4ab: the
    // decrement precedes the built_flags test). Slots 1 and 2 are live but DISCONNECTED, slot 3 is
    // live and connected. With a budget of 2, the scan spends it on slots 1 and 2 and never reaches
    // slot 3 -- so before == 0. A translation that only decremented for CONNECTED slots would still
    // have budget left, reach slot 3 and count 1.
    //
    // With before == 0, the verdict is (after + 1 < 0), which cannot hold: return 0.
    // =================================================================================================
    {
        fx.reset();
        reset_calls(fx);
        seed_live(fx, /*player=*/1, /*n=*/3);
        slot(fx, 1, 0).energy      = 2.0;
        slot(fx, 1, 1).built_flags = 0x0;
        slot(fx, 1, 2).built_flags = 0x0;
        slot(fx, 1, 3).built_flags = 0x1;
        // The candidate must be connected to get past the gate; use slot 3 and leave the mock inert.
        g_flags_per_call.push_back({0x0, 0x0, 0x1});
        g_flags_per_call.push_back({0x0, 0x0, 0x1});
        g_flags_per_call.push_back({0x0, 0x0, 0x1});

        const int32_t ret = run(fx, /*player=*/1, /*b_index=*/3);

        ck_eq((uint32_t)ret, 0u,
              "T6: budget is spent on every energy-positive slot, connected or not, 0x004974a8-d4ab");
    }

    // =================================================================================================
    // T7 -- A ZERO-ENERGY SLOT IS SKIPPED ENTIRELY (neither budget nor count). Slot 1 is a dead
    // building (energy 0) that IS still flagged connected; the budget is 2 and slots 2 and 3 are
    // live. If the zero-energy slot consumed budget or counted, the BEFORE count would be 1, not 2.
    //
    // THIS CASE COVERS ZERO ONLY -- see T7b for the negative side. It used to seed a negative-energy
    // slot 4 and claim it as coverage of `!(energy <= 0.0)` vs `energy != 0.0`; the budget is 2, so
    // the scan stopped at slot 3 and slot 4 was never visited. The mutation campaign caught the
    // over-claim: `if (b.energy != 0.0)` survived this file with 0 failures (2026-09-01).
    // =================================================================================================
    {
        fx.reset();
        reset_calls(fx);
        seed_live(fx, /*player=*/1, /*n=*/5);
        slot(fx, 1, 0).energy = 2.0;
        slot(fx, 1, 1).energy = 0.0; // dead, still flagged connected
        std::vector<uint8_t> all_on(5, 0x1);
        std::vector<uint8_t> after = {0x1, 0x0, 0x0, 0x1, 0x1}; // slots 2 and 3 lose their bit
        g_flags_per_call.push_back(all_on);
        g_flags_per_call.push_back(after);
        g_flags_per_call.push_back(all_on);

        const int32_t ret = run(fx, /*player=*/1, /*b_index=*/2);

        ck_eq((uint32_t)ret, 1u,
              "T7: a zero-energy slot is skipped without spending budget -- before=2, after=0");
    }

    // =================================================================================================
    // T7b -- A NEGATIVE ENERGY IS ALSO SKIPPED: the gate is `!(energy <= 0.0)`, NOT `energy != 0.0`.
    //
    // Separating the two counts is what makes this measurable. The mutant adds the negative slot to
    // BOTH scans, and `after + 1 < before` cancels an equal shift -- so a case where the slot is
    // merely present cannot see it. Here its CONNECTED bit differs between the two recomputes, so the
    // mutant's extra count lands in the BEFORE scan only and the verdict flips:
    //
    //   budget 10 (generous -- every slot is visited, unlike T7)
    //   slot 1: energy -50   flags 0x1 in the BEFORE recompute, 0x0 in the AFTER one
    //   slot 2: energy 100   flags 0x1 throughout -- the candidate
    //   slot 3: energy 100   flags 0x1 throughout
    //
    //   correct  before = 2 (slots 2,3)     after = 1 (slot 3; the candidate is zeroed)  -> 2 < 2 false -> 0
    //   `!= 0.0` before = 3 (slot 1 counts) after = 1 (slot 1's bit is clear by then)    -> 2 < 3 true  -> 1
    // =================================================================================================
    {
        fx.reset();
        reset_calls(fx);
        seed_live(fx, /*player=*/1, /*n=*/3);
        slot(fx, 1, 0).energy = 10.0;  // a budget large enough to reach every slot
        slot(fx, 1, 1).energy = -50.0; // NEGATIVE, and connected in the first scan
        g_flags_per_call.push_back({0x1, 0x1, 0x1});
        g_flags_per_call.push_back({0x0, 0x1, 0x1}); // slot 1 loses its bit before the AFTER scan
        g_flags_per_call.push_back({0x1, 0x1, 0x1});

        const int32_t ret = run(fx, /*player=*/1, /*b_index=*/2);

        ck_eq((uint32_t)ret, 0u,
              "T7b: a NEGATIVE-energy slot is not a live building -- the gate is `!(energy <= 0.0)`, "
              "not `energy != 0.0`, 0x00497475-d4cf");
    }

    // =================================================================================================
    // T8 -- THE PLAYER ROW. A second player's roster is seeded as a full decoy at the same slot
    // indices with the OPPOSITE connectivity outcome, so indexing the wrong row flips the verdict.
    // The decoy row must also come back untouched.
    // =================================================================================================
    {
        fx.reset();
        reset_calls(fx);
        seed_live(fx, /*player=*/4, /*n=*/5);
        seed_live(fx, /*player=*/5, /*n=*/5);
        const double decoy_sentinel = slot(fx, 5, 0).energy;
        const double decoy_cand     = slot(fx, 5, 3).energy;
        g_flags_per_call.push_back({0x1, 0x1, 0x1, 0x1, 0x1});
        g_flags_per_call.push_back({0x1, 0x1, 0x0, 0x0, 0x0});
        g_flags_per_call.push_back({0x1, 0x1, 0x1, 0x1, 0x1});

        const int32_t ret = run(fx, /*player=*/4, /*b_index=*/3);

        ck_eq((uint32_t)ret, 1u, "T8: player 4's row is the one measured");
        ck_eq_d(slot(fx, 5, 0).energy, decoy_sentinel, "T8: player 5's sentinel untouched");
        ck_eq_d(slot(fx, 5, 3).energy, decoy_cand, "T8: player 5's candidate untouched");
        ck_eq((uint32_t)slot(fx, 5, 3).built_flags, 1u, "T8: player 5's flags untouched");
    }

    // =================================================================================================
    // T9 -- THE CANDIDATE'S ENERGY IS ZEROED FOR THE SECOND SCAN AND RESTORED AFTERWARDS. The mock
    // observes the candidate's energy at each recompute, which is the only place the intermediate
    // state is visible from outside. A non-integral energy is used so a restore that re-derived the
    // value (rather than replaying the raw 8-byte snapshot) would land somewhere else.
    // =================================================================================================
    {
        fx.reset();
        reset_calls(fx);
        seed_live(fx, /*player=*/1, /*n=*/3);
        const double odd_energy = 12345.6789;
        slot(fx, 1, 2).energy   = odd_energy;

        // A second, dedicated probe: record the candidate's energy at each recompute.
        static std::vector<double> cand_at_call;
        cand_at_call.clear();
        struct Probe {
            static void recompute(uint16_t player) {
                cand_at_call.push_back(g_fx->buildings[(size_t)(player * BUILDINGS_PER_PLAYER + 2)].energy);
                stub_power_network_recompute(player);
            }
        };
        const bldg_network_critical_calls probe_calls = {&Probe::recompute};

        sim_store     own = fx.store();
        const int32_t ret = detail::bldg_is_network_critical(fx.view(), own, probe_calls, 1, 2);
        (void)ret;

        ck(cand_at_call.size() == 3, "T9: three recomputes observed the candidate");
        if (cand_at_call.size() == 3) {
            ck_eq_d(cand_at_call[0], odd_energy, "T9: recompute #1 sees the candidate intact");
            ck_eq_d(cand_at_call[1], 0.0, "T9: recompute #2 sees it ZEROED, 0x00497501-d50b");
            ck_eq_d(cand_at_call[2], odd_energy,
                    "T9: recompute #3 sees the raw 8-byte snapshot restored, 0x004975b6-d5d0");
        }
        ck_eq_d(slot(fx, 1, 2).energy, odd_energy, "T9: and it is left restored on return");
    }
}

} // namespace mh::sim::test
