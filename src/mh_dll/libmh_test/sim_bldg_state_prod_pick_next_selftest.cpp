//
// sim_bldg_state_prod_pick_next_selftest.cpp -- `simtest` cases for llm_strat_bldg_state_prod_pick_next
// (sim/sim_bldg_state_prod.h/.cpp @0x00473a20), SIM1-G4 second-slice building_tick machinery -- the
// PROD_PICK_NEXT handler that scans a production building's queue for the next unit type to start.
//
// THIS FUNCTION IS DO-NOT-ARM (its write closure reaches game_SetEvent's ~780-function UI/gfx/snd/menu
// teardown over-approximation via a sibling in the same dispatch table), so this file is the function's
// ONLY evidence -- there is no rig run backing it up. See the header derivation in
// sim/sim_bldg_state_prod.h for the full narrative; this file pins it branch-by-branch against the raw
// disassembly (tmp/decomp_sim/llm_strat_bldg_state_prod_pick_next_00473a20.asm), NOT the Ghidra .c draft.
//
// SCOPE: only llm_strat_bldg_state_prod_pick_next. The other three siblings in sim_bldg_state_prod.h/.cpp
// (prod_working, prod_blocked_notify, prod_retry_wait) are out of scope for this file.
//
// EXPECTED BEHAVIOUR (see the header's derivation for the full prose; addresses cited per-check below):
//   Unconditionally (0x00473a3f-0x00473a67): state=IDLE_ACTIVATE(1), online_state=2, cycle_progress=0.0.
//   slot = (productions[player][sub_id].active_unit_type % 99) + 1 (0x00473a6d-0x00473a9d), computed ONCE
//   before the loop from the CACHED player/sub_id (see the CUR_PLAYER CACHING finding below).
//   Loop, up to 100 attempts (0x00473aa4/0x00473aa8/0x00473aaa): for each `slot`, skip unless
//   queued_count[slot]!=0 AND cfg_buildings[bid].unit_quant[slot]!=0.0 (the raw-bit-test note: this is
//   `!= 0.0`, not an FCOMP -- see sim_bldg_state_prod.h's BIT-TEST section). If a candidate:
//     call prod_try_start_unit(player, slot) (0x00473b3b).
//       == 0 (SUCCESS, 0x00473bd9-0x00473c97): decrement queued_count[slot] and queued_count[0], set
//       active_unit_type=slot, state=PROD_WORKING(0x6d), online_state=2, cycle_progress=0.0, call
//       ai_notify_unit_lifecycle(player, slot, 0, 0), RETURN -- the search stops here.
//       != 0 (BLOCKED, 0x00473b4d-0x00473bd4): state=PROD_BLOCKED_NOTIFY(0x6e); bump fail_count; merge
//       the reason into online_state (fail_count==1 OR online_state==reason -> online_state=reason
//       (0x00473b5f/0x00473b6a->LAB_00473bbd); else if BOTH online_state and reason are in [0x89,0x8d]
//       (inclusive both ends, 0x00473b71-0x00473ba3) -> online_state=0x89 (UNIFY); else -> online_state=0
//       (0x00473bb0)). If reason==6 (STOP_REASON, 0x00473bca), RETURN; else continue.
//   slot advances by the SAME (slot % 99) + 1 recurrence at the BOTTOM of every iteration regardless of
//   whether the candidate test ran (0x00473ab5-0x00473ac6), so with 100 attempts over a 99-slot cycle the
//   very first slot is revisited on the 100th (last) attempt.
//   If the loop exhausts without a success or a reason==6 exit, the function falls off the end with no
//   further state/online_state/cycle_progress change beyond whatever the last processed candidate wrote
//   (or the unconditional top-of-function values, if no candidate was ever found at all).
//
// ---- FINDING: llm_strat_bldg_state_prod_pick_next.cpp CACHES `player` (and `sub_id`) as VALUES once at
// function entry (`const uint16_t player = *v.cur_player;`, `const uint8_t sub_id = b.sub_id;`), while
// the disassembly RE-READS _G_LLM_STRAT_CUR_PLAYER (0x00e58144) from memory at EVERY one of its several
// use sites (0x00473a7a, 0x00473add, 0x00473b34, 0x00473beb, 0x00473c1b, 0x00473c49, 0x00473c8b) and
// re-reads CUR_BUILDING->sub_id (0x00e162dc + 0xc6) similarly at 0x00473a6d/0x00473ad0/0x00473bde/
// 0x00473c0e/0x00473c35. This is DIFFERENT from the (harmless) pattern of caching the CUR_BUILDING
// *pointer* into a `building &b` reference (dereferencing `b.field` always re-reads live, matching the
// asm's re-read-through-pointer) -- caching `player`/`sub_id` as plain VALUES breaks live pass-through:
// if `prod_try_start_unit` (an ORIGINAL, uninspected function reachable via the shadow arm) were ever to
// mutate _G_LLM_STRAT_CUR_PLAYER or the building's sub_id as a side effect mid-search, the real game would
// pick that mutation up on its very next field access while this translation would keep using the value
// captured at entry. C-PLAYER-CACHE below PINS the .cpp's actual (cached) behaviour and demonstrates the
// gap is real and observable, not hypothetical -- flagged per the translator brief's "the assembly wins,
// report don't fix" rule rather than silently changed. Whether it can ever matter in practice (i.e.
// whether prod_try_start_unit really does write CUR_PLAYER) is for the conductor to assess; the SAME
// caching pattern applies identically to `sub_id`, not separately pinned here to avoid a near-duplicate
// case (structurally identical single-dereference-at-entry caching, same risk class).
//
#include "sim/sim_bldg_state_prod.h"

#include <cstdint>
#include <cstring>
#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- shared trace: one sequence proves CALL ORDER / CALL PRESENCE across both callees -------------
std::vector<const char *> g_trace;
void                      tr(const char *tag) { g_trace.push_back(tag); }

bool trace_eq(const std::vector<const char *> &want) {
    if (g_trace.size() != want.size()) return false;
    for (size_t i = 0; i < want.size(); ++i)
        if (std::strcmp(g_trace[i], want[i]) != 0) return false;
    return true;
}

// ---- prod_try_start_unit: a PROGRAMMABLE sequence of return values, one per call (0-based), with an
// optional one-shot mutation of *cur_player fired from inside a specific call -- this is what makes
// C-PLAYER-CACHE possible: the mock reaches into the fixture's ambient CUR_PLAYER slot exactly like a
// real (if hypothetical) side-effecting callee would.
struct TryStartCall {
    uint32_t player;
    int32_t  unit_type;
};
std::vector<TryStartCall> g_try_start_calls;
std::vector<int32_t>      g_try_start_results;            // programmed per-call-index results
int32_t                   g_try_start_default     = -999; // used once g_try_start_results is exhausted
uint16_t                 *g_mutate_cur_player_ptr = nullptr;
uint16_t                  g_mutate_cur_player_new = 0;
int32_t                   g_mutate_on_call_index  = -1; // 0-based; -1 = never mutate

int32_t rec_prod_try_start_unit(uint32_t player, int32_t unit_type) {
    tr("prod_try_start_unit");
    g_try_start_calls.push_back({player, unit_type});
    const int32_t idx = static_cast<int32_t>(g_try_start_calls.size()) - 1;
    if (g_mutate_cur_player_ptr != nullptr && idx == g_mutate_on_call_index) {
        *g_mutate_cur_player_ptr = g_mutate_cur_player_new;
    }
    return (idx < static_cast<int32_t>(g_try_start_results.size())) ? g_try_start_results[(size_t)idx]
                                                                    : g_try_start_default;
}

struct NotifyLifecycleCall {
    uint16_t player;
    uint16_t unit_type;
    uint32_t unit_id;
    uint32_t param_4;
};
std::vector<NotifyLifecycleCall> g_notify_lifecycle_calls;
void                             rec_ai_notify_unit_lifecycle(uint16_t player, uint16_t unit_type, uint32_t unit_id, uint32_t param_4) {
    tr("ai_notify_unit_lifecycle");
    g_notify_lifecycle_calls.push_back({player, unit_type, unit_id, param_4});
}

const bldg_state_prod_pick_next_calls g_calls = {
    &rec_prod_try_start_unit,
    &rec_ai_notify_unit_lifecycle,
};

void reset_observations() {
    g_trace.clear();
    g_try_start_calls.clear();
    g_try_start_results.clear();
    g_try_start_default     = -999;
    g_mutate_cur_player_ptr = nullptr;
    g_mutate_cur_player_new = 0;
    g_mutate_on_call_index  = -1;
    g_notify_lifecycle_calls.clear();
}

// ---- fixture seeding -----------------------------------------------------------------------------
struct Seed {
    uint16_t player           = 0;
    int32_t  index            = 1;
    uint16_t cfg_row          = 7; // building_id -> cfg_buildings index (Building[bid].unit_quant)
    uint8_t  sub_id           = 3; // -> productions row = player*PRODUCTIONS_PER_PLAYER + sub_id
    uint8_t  active_unit_type = 0; // drives the initial slot = (active_unit_type % 99) + 1

    // Distinct, non-{1,2,0.0} sentinels so a case that finds NO candidate at all (and therefore never
    // re-enters any branch) can still prove the unconditional top-of-function writes fired.
    uint16_t state_in          = 77;
    int16_t  online_state_in   = 200;
    double   cycle_progress_in = 42.5;
};

production &prow(sim_fixture &fx, uint16_t player, uint8_t sub_id) {
    return fx.productions[(size_t)player * (size_t)PRODUCTIONS_PER_PLAYER + sub_id];
}

// Sets up slot `slot` of the seed's own (player, sub_id) row as a candidate: queued_count[slot] and
// cfg_buildings[cfg_row].unit_quant[slot] both nonzero.
void set_candidate(sim_fixture &fx, const Seed &s, int32_t slot, int32_t queued_count_val, double unit_quant_val) {
    prow(fx, s.player, s.sub_id).queued_count[slot] = queued_count_val;
    fx.cfg_buildings[s.cfg_row].unit_quant[slot]    = unit_quant_val;
}

double bits_to_double(uint64_t bits) {
    double d;
    std::memcpy(&d, &bits, sizeof(d));
    return d;
}

void seed(sim_fixture &fx, const Seed &s) {
    fx.reset();

    building &b      = fx.b(s.player, s.index);
    b.building_id    = s.cfg_row;
    b.sub_id         = s.sub_id;
    b.state          = s.state_in;
    b.online_state   = s.online_state_in;
    b.cycle_progress = s.cycle_progress_in;

    fx.cur_building_ptr = &b;
    fx.view_cur_player  = s.player;
    fx.view_cur_index   = (uint16_t)s.index;

    prow(fx, s.player, s.sub_id).active_unit_type = s.active_unit_type;

    reset_observations();
}

void run(sim_fixture &fx) {
    sim_store own = fx.store();
    detail::bldg_state_prod_pick_next(fx.view(), own, g_calls);
}

} // namespace

void run_bldg_state_prod_pick_next_tests() {
    sim_fixture fx;

    // =================================================================================================
    // C1 -- NO CANDIDATE ANYWHERE: every slot has queued_count==0 (fixture default after reset()), so
    // the search exhausts all 100 attempts without ever calling prod_try_start_unit. Pins the THREE
    // UNCONDITIONAL top-of-function writes as genuinely unconditional -- they must survive even when no
    // branch inside the loop body ever executes.
    // =================================================================================================
    {
        Seed s;
        s.active_unit_type = 0; // slot starts at 1; irrelevant here since nothing is a candidate
        seed(fx, s);
        run(fx);

        ck(trace_eq({}), "C1: no candidate anywhere -- neither callee fires (0x00473aa4..0x00473c9e, "
                         "100 attempts, every queued_count[slot]==0 skip at 0x00473afc)");
        ck_eq((uint32_t)fx.b(s.player, s.index).state, 1u,
              "C1: state = IDLE_ACTIVATE(1) unconditionally (0x00473a44), survives even with 0 loop hits");
        ck_eq((uint32_t)(uint16_t)fx.b(s.player, s.index).online_state, 2u,
              "C1: online_state = 2 unconditionally (0x00473a4f), survives even with 0 loop hits");
        ck_eq_d(fx.b(s.player, s.index).cycle_progress, 0.0,
                "C1: cycle_progress = 0.0 unconditionally (0x00473a5a/0x00473a61), survives 0 loop hits");
    }

    // =================================================================================================
    // C2/C3/C4/C5 -- INITIAL SLOT FORMULA: slot = (active_unit_type % 99) + 1, exercised at four
    // boundary values (0, 98, 99-wraps-to-1, 255) by making ONLY that computed slot a candidate that
    // succeeds immediately (so the call's `unit_type` argument names exactly which slot the formula
    // picked, with no loop-advance noise).
    // =================================================================================================
    {
        Seed s;
        s.active_unit_type = 0; // (0 % 99) + 1 = 1
        seed(fx, s);
        set_candidate(fx, s, 1, /*queued_count*/ 1, /*unit_quant*/ 1.0);
        g_try_start_results = {0};
        run(fx);
        ck(g_try_start_calls.size() == 1 && g_try_start_calls[0].unit_type == 1,
           "C2: active_unit_type=0 -> initial slot = (0 % 99) + 1 = 1 (0x00473a90-0x00473a9d)");
    }
    {
        Seed s;
        s.active_unit_type = 98; // (98 % 99) + 1 = 99 -- the last slot before the cycle wraps
        seed(fx, s);
        set_candidate(fx, s, 99, 1, 1.0);
        g_try_start_results = {0};
        run(fx);
        ck(g_try_start_calls.size() == 1 && g_try_start_calls[0].unit_type == 99,
           "C3: active_unit_type=98 -> initial slot = (98 % 99) + 1 = 99 (upper boundary before wrap)");
    }
    {
        Seed s;
        s.active_unit_type = 99; // (99 % 99) + 1 = 0 + 1 = 1 -- wraps back to 1 at the modulus itself
        seed(fx, s);
        set_candidate(fx, s, 1, 1, 1.0);
        g_try_start_results = {0};
        run(fx);
        ck(g_try_start_calls.size() == 1 && g_try_start_calls[0].unit_type == 1,
           "C4: active_unit_type=99 -> initial slot = (99 % 99) + 1 = 1, the modulus wraps exactly AT "
           "99 (contrast C3's 98 -> 99)");
    }
    {
        Seed s;
        s.active_unit_type = 255; // 0xff: (255 % 99) + 1 = 57 + 1 = 58
        seed(fx, s);
        set_candidate(fx, s, 58, 1, 1.0);
        g_try_start_results = {0};
        run(fx);
        ck(g_try_start_calls.size() == 1 && g_try_start_calls[0].unit_type == 58,
           "C5: active_unit_type=255 (0xff, the uint8_t max) -> initial slot = (255 % 99) + 1 = 58");
    }

    // =================================================================================================
    // C6 -- SLOT ADVANCE USES THE SAME RECURRENCE AT THE BOTTOM OF THE LOOP, mid-search: starting slot
    // 99 (blocked, non-stop reason) advances to (99 % 99) + 1 = 1, where the second candidate succeeds.
    // Pins the bottom-of-loop advance (0x00473ab5-0x00473ac6) as the SAME formula as the initial one, at
    // the same wrap point C4 pins for the initial computation.
    // =================================================================================================
    {
        Seed s;
        s.active_unit_type = 98; // initial slot = 99
        seed(fx, s);
        set_candidate(fx, s, 99, 1, 1.0); // blocked, continues
        set_candidate(fx, s, 1, 1, 1.0);  // then succeeds
        g_try_start_results = {9, 0};
        run(fx);
        ck(g_try_start_calls.size() == 2 && g_try_start_calls[0].unit_type == 99 &&
               g_try_start_calls[1].unit_type == 1,
           "C6: slot sequence 99 -> 1 -- the bottom-of-loop advance (slot % 99) + 1 wraps 99->1 exactly "
           "like the initial-slot formula (0x00473ab5-0x00473ac6)");
        ck_eq((uint32_t)prow(fx, s.player, s.sub_id).active_unit_type, 1u,
              "C6: the second (wrapped) slot is the one that actually succeeds");
    }

    // =================================================================================================
    // C7/C8 -- THE BIT-TEST (see sim_bldg_state_prod.h's BIT-TEST note): `unit_quant[slot] != 0.0` is a
    // raw masked-dword test, not an FCOMP. C7 pins the SIGN-BIT-MASKED side (-0.0 reads as zero, same as
    // +0.0 -- skip); C8 pins the NaN side (a NaN bit pattern reads as nonzero, matching `!= 0.0`'s own
    // truth table for NaN -- NOT skipped). Both use a single would-be candidate slot so the whole
    // 100-attempt search either fires 0 or 1 calls, unambiguously.
    // =================================================================================================
    {
        Seed s;
        s.active_unit_type = 0; // slot 1
        seed(fx, s);
        prow(fx, s.player, s.sub_id).queued_count[1] = 1;    // queued_count side: nonzero
        fx.cfg_buildings[s.cfg_row].unit_quant[1]    = -0.0; // unit_quant side: NEGATIVE zero
        run(fx);
        ck(trace_eq({}),
           "C7: unit_quant[slot] == -0.0 reads as ZERO under the sign-bit-masked bit test -- candidate "
           "SKIPPED, no call at all across all 100 attempts (0x00473b16-0x00473b29)");
    }
    {
        Seed s;
        s.active_unit_type = 0; // slot 1
        seed(fx, s);
        prow(fx, s.player, s.sub_id).queued_count[1] = 1; // queued_count side: nonzero
        fx.cfg_buildings[s.cfg_row].unit_quant[1] =
            bits_to_double(0x7FF8000000000000ULL); // a quiet NaN bit pattern
        g_try_start_results = {0};
        run(fx);
        ck(g_try_start_calls.size() == 1 && g_try_start_calls[0].unit_type == 1,
           "C8: unit_quant[slot] == a NaN bit pattern reads as NONZERO under the masked bit test (matches "
           "`!= 0.0`'s own truth table for NaN, per the header's provable-equivalence claim) -- candidate "
           "NOT skipped, the call fires (0x00473b16-0x00473b29 falls through to 0x00473b30)");
    }

    // =================================================================================================
    // C9 -- queued_count[slot]==0 skips the slot REGARDLESS of unit_quant, pinning the `&&` short-circuit
    // order documented in the header (queued_count gates first).
    // =================================================================================================
    {
        Seed s;
        s.active_unit_type = 0; // slot 1
        seed(fx, s);
        prow(fx, s.player, s.sub_id).queued_count[1] = 0;   // queued_count side: ZERO
        fx.cfg_buildings[s.cfg_row].unit_quant[1]    = 5.0; // unit_quant side: clearly nonzero
        run(fx);
        ck(trace_eq({}),
           "C9: queued_count[slot]==0 skips the slot even though unit_quant[slot]!=0.0 -- queued_count "
           "gates first (0x00473af5/0x00473afc JZ to skip, before the unit_quant test ever runs)");
    }

    // =================================================================================================
    // C10 -- FULL SUCCESS PATH: decrements BOTH queued_count[slot] and queued_count[0] (the row's own
    // "total demand" slot), sets active_unit_type=slot, state=PROD_WORKING(0x6d), online_state=2 (even
    // though seeded to a different sentinel), cycle_progress=0.0 (seeded nonzero), calls
    // ai_notify_unit_lifecycle(player, slot, 0, 0) exactly once, and the search STOPS -- a second,
    // equally-valid candidate slot is left completely untouched.
    // =================================================================================================
    {
        Seed s;
        s.active_unit_type = 0; // slot 1
        seed(fx, s);
        set_candidate(fx, s, 1, /*queued_count*/ 3, /*unit_quant*/ 2.5);
        set_candidate(fx, s, 2, /*queued_count*/ 7, /*unit_quant*/ 9.9); // a second candidate, never reached
        prow(fx, s.player, s.sub_id).queued_count[0] = 5;                // the row's own "total demand" slot
        g_try_start_results                          = {0};
        run(fx);

        ck(trace_eq({"prod_try_start_unit", "ai_notify_unit_lifecycle"}),
           "C10: exact call order on success -- prod_try_start_unit then ai_notify_unit_lifecycle, "
           "nothing else (0x00473b3b -> 0x00473c92), and the search STOPS (only one prod_try_start_unit "
           "call even though slot 2 is also a valid candidate)");
        ck(g_try_start_calls.size() == 1 && g_try_start_calls[0].player == s.player &&
               g_try_start_calls[0].unit_type == 1,
           "C10: prod_try_start_unit(player, slot=1) (0x00473b3b)");
        ck_eq((uint32_t)prow(fx, s.player, s.sub_id).queued_count[1], 2u,
              "C10: queued_count[slot] decremented 3 -> 2 (0x00473c03 DEC dword[...+0xdd264d])");
        ck_eq((uint32_t)prow(fx, s.player, s.sub_id).queued_count[0], 4u,
              "C10: queued_count[0] (the row's total-demand slot) ALSO decremented 5 -> 4 (0x00473c2a, a "
              "SEPARATE DEC from the slot's own counter)");
        ck_eq((uint32_t)prow(fx, s.player, s.sub_id).active_unit_type, 1u,
              "C10: active_unit_type = slot = 1 (0x00473c54)");
        ck_eq((uint32_t)fx.b(s.player, s.index).state, 0x6du,
              "C10: state = PROD_WORKING(0x6d) (0x00473c5f)");
        ck_eq((uint32_t)(uint16_t)fx.b(s.player, s.index).online_state, 2u,
              "C10: online_state = 2 on success, overriding the seeded sentinel 200 (0x00473c6a)");
        ck_eq_d(fx.b(s.player, s.index).cycle_progress, 0.0,
                "C10: cycle_progress = 0.0 on success, overriding the seeded sentinel 42.5 "
                "(0x00473c75/0x00473c7c)");
        ck(g_notify_lifecycle_calls.size() == 1 && g_notify_lifecycle_calls[0].player == s.player &&
               g_notify_lifecycle_calls[0].unit_type == 1 && g_notify_lifecycle_calls[0].unit_id == 0 &&
               g_notify_lifecycle_calls[0].param_4 == 0,
           "C10: ai_notify_unit_lifecycle(player, (uint16)slot=1, 0, 0) (0x00473c83-0x00473c92)");
        ck_eq((uint32_t)prow(fx, s.player, s.sub_id).queued_count[2], 7u,
              "C10: slot 2's queued_count UNCHANGED -- the search stopped at slot 1's success, slot 2 "
              "was never reached");
    }

    // =================================================================================================
    // C12..C18 -- THE MERGE LOGIC (see sim_bldg_state_prod.h's MERGE LOGIC note). All of these use a
    // SINGLE candidate at slot 1: because 100 attempts cycle over a 99-slot period, the ONE candidate
    // slot is visited exactly TWICE (attempt #0 and attempt #99, the wraparound revisit C6 already
    // pins), which is exactly the two blocked attempts each of these cases needs -- no second distinct
    // slot required, and no risk of a stray third call polluting the online_state result.
    // =================================================================================================

    // C12 -- fail_count==1 ALWAYS records the reason, regardless of whatever online_state held before
    // (seeded to 200, matching neither the merge band nor the reason below).
    {
        Seed s;
        s.active_unit_type = 0; // slot 1, visited at attempt #0 and #99
        seed(fx, s);
        set_candidate(fx, s, 1, 1, 1.0);
        g_try_start_default = 40; // both visits return the SAME reason 40 (see C13 for the OTHER branch)
        run(fx);
        ck(g_try_start_calls.size() == 2, "C12: the sole candidate is visited twice (attempt #0 and the "
                                          "wraparound revisit at attempt #99, per the 100-over-99 cycle)");
        ck_eq((uint32_t)fx.b(s.player, s.index).state, 0x6eu,
              "C12: state = PROD_BLOCKED_NOTIFY(0x6e) on a blocked result (0x00473b4d)");
        ck_eq((uint32_t)(uint16_t)fx.b(s.player, s.index).online_state, 40u,
              "C12: fail_count==1 -> online_state = reason(40) UNCONDITIONALLY, overriding the seeded "
              "sentinel 200 that matches neither the reason nor the merge band (0x00473b5f -> "
              "LAB_00473bbd/0x00473bc6)");
    }

    // C13 -- fail_count>1, SAME reason as the current online_state -> online_state stays that reason
    // (the second clause of the `||`), NOT reset to 0 by the band/else logic.
    {
        Seed s;
        s.active_unit_type = 0;
        seed(fx, s);
        set_candidate(fx, s, 1, 1, 1.0);
        g_try_start_default = 12; // out-of-band reason, repeated on both visits
        run(fx);
        ck_eq((uint32_t)(uint16_t)fx.b(s.player, s.index).online_state, 12u,
              "C13: fail_count==2, online_state(12 from attempt #0) == reason(12) -> online_state stays "
              "12 via the `online_state_u == result` clause (0x00473b66-0x00473b6d JNZ falls through to "
              "LAB_00473b6f), NOT the else-branch's 0");
    }

    // C14 -- fail_count>1, DIFFERENT reason, BOTH in the merge band [0x89,0x8d] -> unify to 0x89.
    {
        Seed s;
        s.active_unit_type = 0;
        seed(fx, s);
        set_candidate(fx, s, 1, 1, 1.0);
        g_try_start_results = {0x8b, 0x8d}; // both in-band, DIFFERENT from each other
        run(fx);
        ck_eq((uint32_t)(uint16_t)fx.b(s.player, s.index).online_state, 0x89u,
              "C14: online_state(0x8b) != reason(0x8d), but BOTH are in [0x89,0x8d] -> unify to the "
              "LITERAL constant 0x89 (0x00473b71-0x00473ba8), not min/max/either operand");
    }

    // C15 -- the merge band's boundary is INCLUSIVE at BOTH ends simultaneously: online_state exactly
    // 0x89 (the low bound) and reason exactly 0x8d (the high bound) still unify.
    {
        Seed s;
        s.active_unit_type = 0;
        seed(fx, s);
        set_candidate(fx, s, 1, 1, 1.0);
        g_try_start_results = {0x89, 0x8d}; // exact lo and exact hi
        run(fx);
        ck_eq((uint32_t)(uint16_t)fx.b(s.player, s.index).online_state, 0x89u,
              "C15: online_state==0x89 (exact LO bound) and reason==0x8d (exact HI bound) both count as "
              "'in band' -- unify fires at the closed interval's own edges (0x00473b7c JC / 0x00473b89 "
              "JBE / 0x00473b94 JGE / 0x00473b9f JLE all inclusive)");
    }

    // C16 -- one below the band's LOW edge (0x88) on the online_state side excludes the merge, even
    // though the reason IS in-band -- falls to the else-branch's 0, not the unify.
    {
        Seed s;
        s.active_unit_type = 0;
        seed(fx, s);
        set_candidate(fx, s, 1, 1, 1.0);
        g_try_start_results = {0x88, 0x8a}; // online_state ends up 0x88 (just BELOW the band) then 0x8a (in-band)
        run(fx);
        ck_eq((uint32_t)(uint16_t)fx.b(s.player, s.index).online_state, 0u,
              "C16: online_state==0x88 is one LESS than the band's LO(0x89) -- 0x88 < 0x89 fails the "
              "band test (0x00473b7c JC taken) even though reason==0x8a IS in-band -- falls to the "
              "else-branch's 0, NOT unify (0x00473bb0/0x00473bb5)");
    }

    // C17 -- one above the band's HIGH edge (0x8e) on the online_state side, same exclusion, other end.
    {
        Seed s;
        s.active_unit_type = 0;
        seed(fx, s);
        set_candidate(fx, s, 1, 1, 1.0);
        g_try_start_results = {0x8e, 0x8c}; // online_state ends up 0x8e (just ABOVE the band) then 0x8c (in-band)
        run(fx);
        ck_eq((uint32_t)(uint16_t)fx.b(s.player, s.index).online_state, 0u,
              "C17: online_state==0x8e is one MORE than the band's HI(0x8d) -- 0x8e > 0x8d fails the "
              "band test (0x00473b89 JBE not taken) even though reason==0x8c IS in-band -- falls to the "
              "else-branch's 0, NOT unify");
    }

    // C18 -- different reasons, NEITHER in the merge band at all -> plain 0, the ordinary "no dominant
    // reason" case (not a boundary probe, just the baseline the boundary cases above are contrasted
    // against).
    {
        Seed s;
        s.active_unit_type = 0;
        seed(fx, s);
        set_candidate(fx, s, 1, 1, 1.0);
        g_try_start_results = {5, 9};
        run(fx);
        ck_eq((uint32_t)(uint16_t)fx.b(s.player, s.index).online_state, 0u,
              "C18: online_state(5) != reason(9), neither in [0x89,0x8d] -> online_state = 0 "
              "(0x00473bb0/0x00473bb5)");
    }

    // =================================================================================================
    // C19/C20/C21 -- THE STOP_REASON SENTINEL (6), pinned from both sides. C19: reason==6 halts the
    // search immediately, even with a further candidate slot that would otherwise succeed. C20/C21:
    // reasons adjacent to 6 (5 and 7) do NOT halt -- the search continues to revisit the sole candidate
    // slot at the wraparound (attempt #99), exactly like C12-C18's two-call shape.
    // =================================================================================================
    {
        Seed s;
        s.active_unit_type = 0; // slot 1
        seed(fx, s);
        set_candidate(fx, s, 1, 1, 1.0);
        set_candidate(fx, s, 2, 1, 1.0); // would succeed if ever reached -- it must not be
        g_try_start_results = {6};
        g_try_start_default = 0; // if wrongly reached, slot 2 would silently "succeed"
        run(fx);
        ck(g_try_start_calls.size() == 1 && g_try_start_calls[0].unit_type == 1,
           "C19: reason==STOP_REASON(6) halts the search IMMEDIATELY (0x00473bca CMP/JZ 0x00473c9e) -- "
           "slot 2 (a further valid candidate) is never reached");
        ck_eq((uint32_t)prow(fx, s.player, s.sub_id).queued_count[2], 1u,
              "C19: slot 2's queued_count UNCHANGED -- confirms it was truly never processed, not just "
              "that its call happened to look like a no-op");
        ck(g_notify_lifecycle_calls.empty(),
           "C19: ai_notify_unit_lifecycle never fires -- the search stopped on a BLOCKED result, not a "
           "success");
    }
    {
        Seed s;
        s.active_unit_type = 0;
        seed(fx, s);
        set_candidate(fx, s, 1, 1, 1.0);
        g_try_start_default = 5; // one LESS than STOP_REASON -- must NOT stop
        run(fx);
        ck(g_try_start_calls.size() == 2,
           "C20: reason==5 (STOP_REASON-1) does NOT halt the search -- the sole candidate is revisited "
           "at the wraparound (attempt #99), same 2-call shape as C12-C18");
    }
    {
        Seed s;
        s.active_unit_type = 0;
        seed(fx, s);
        set_candidate(fx, s, 1, 1, 1.0);
        g_try_start_default = 7; // one MORE than STOP_REASON -- must NOT stop
        run(fx);
        ck(g_try_start_calls.size() == 2,
           "C21: reason==7 (STOP_REASON+1) does NOT halt the search -- same 2-call wraparound shape");
    }

    // =================================================================================================
    // C22 -- EXHAUSTION AT EXACTLY 100 ATTEMPTS: every slot 1..99 is a valid-but-always-blocked
    // candidate (constant reason 9, never 0, never 6), so the search runs the full bound and pins BOTH
    // the modulus-99 cycle AND the max-attempts-100 bound together: the recorded slot sequence is
    // 1,2,...,99,1 (100 entries -- the 100th attempt revisits slot 1, the SAME wraparound C6/C12-C21
    // exercise with a single candidate, now shown across the WHOLE cycle).
    // =================================================================================================
    {
        Seed s;
        s.active_unit_type = 0; // initial slot = 1
        seed(fx, s);
        for (int32_t slot = 1; slot <= 99; ++slot) set_candidate(fx, s, slot, 1, 1.0);
        g_try_start_default = 9; // constant reason every single attempt

        run(fx);

        ck_eq((uint32_t)g_try_start_calls.size(), 100u,
              "C22: exactly 100 calls -- the loop bound (0x00473aa4 CMP iter,0x64 / 0x00473aa8 JC) is "
              "100 attempts, not 99 (one per slot) and not unbounded");
        std::vector<int32_t> want_slots;
        {
            int32_t slot = 1;
            for (int32_t i = 0; i < 100; ++i) {
                want_slots.push_back(slot);
                slot = (slot % 99) + 1; // the documented bottom-of-loop recurrence, restated independently
            }
        }
        bool seq_ok = true;
        for (size_t i = 0; i < 100 && seq_ok; ++i)
            if (g_try_start_calls[i].unit_type != want_slots[i]) seq_ok = false;
        ck(seq_ok, "C22: recorded slot sequence is 1,2,...,99,1 -- the 100th attempt revisits slot 1 "
                   "because a 100-attempt search over a 99-slot modulus cycle wraps exactly once "
                   "(0x00473ab5-0x00473ac6 recurrence, period 99)");
        ck_eq((uint32_t)fx.b(s.player, s.index).state, 0x6eu,
              "C22: final state = PROD_BLOCKED_NOTIFY(0x6e) -- the last (100th) attempt was also blocked");
        ck_eq((uint32_t)(uint16_t)fx.b(s.player, s.index).online_state, 9u,
              "C22: final online_state = 9 -- every attempt after the first repeats the SAME reason, so "
              "the `online_state_u == result` same-reason branch holds for all 99 subsequent attempts");
        ck_eq_d(fx.b(s.player, s.index).cycle_progress, 0.0,
                "C22: cycle_progress stays 0.0 through the whole exhaustion -- only the top-of-function "
                "unconditional write and the (never-taken) success path touch it");
        ck(g_notify_lifecycle_calls.empty(), "C22: ai_notify_unit_lifecycle never fires -- no attempt "
                                             "ever returned 0 (success)");
    }

    // =================================================================================================
    // C23 -- CUR-PLAYER RE-READ (fixed 2026-08-22, see sim_bldg_state_prod.cpp's comment on the loop):
    // *v.cur_player and b.sub_id are read FRESH at the top of every loop iteration (0x00473a7a /
    // 0x00473add / 0x00473b34 / ...), never cached across iterations -- an oracle-fan-out run against
    // the FIRST version of this translation (which cached `player` once at entry) caught it landing a
    // success write on the wrong player's row after a mid-loop CUR_PLAYER mutation; the .cpp was
    // corrected to re-read per-iteration and this case now pins THAT (corrected) behaviour. The mock
    // mutates the fixture's CUR_PLAYER from 0 to 5 while handling the FIRST (blocked) call; the SECOND
    // call, and the eventual success write, must land on PLAYER 5's row -- the CURRENT value at the
    // top of that iteration -- not player 0's.
    // =================================================================================================
    {
        Seed s;
        s.player           = 0;
        s.active_unit_type = 0; // slot starts at 1
        seed(fx, s);
        set_candidate(fx, s, 1, 1, 1.0);                 // player 0's row: candidate at slot 1 (blocked, continues)
        fx.cfg_buildings[s.cfg_row].unit_quant[2] = 1.0; // slot 2 is a valid type (cfg is player-independent)
        prow(fx, 5, s.sub_id).queued_count[2]     = 1;   // but the CANDIDATE itself is on PLAYER 5's row
        // player 0's row has NO candidate at slot 2 (left zero by reset()) -- if the code still used the
        // stale cached player=0 for the second iteration, slot 2's check would read player 0's (zeroed)
        // queued_count[2] and skip, never reaching success.
        g_try_start_results     = {9, 0}; // call #0: blocked (continues); call #1: success
        g_mutate_cur_player_ptr = &fx.view_cur_player;
        g_mutate_cur_player_new = 5;
        g_mutate_on_call_index  = 0; // mutate CUR_PLAYER while handling the FIRST call
        run(fx);

        ck_eq((uint32_t)fx.view_cur_player, 5u,
              "C23: CUR_PLAYER really was mutated to 5 by the mock -- the re-read below is observable, "
              "not a no-op");
        ck(g_try_start_calls.size() == 2 && g_try_start_calls[0].player == 0 && g_try_start_calls[1].player == 5,
           "C23 CUR-PLAYER RE-READ: call #0 carries player=0 (the value at loop-top before the mutation), "
           "call #1 carries player=5 (re-read fresh at 0x00473b34's loop-top on the SECOND iteration, not "
           "the value cached at entry)");
        ck_eq((uint32_t)prow(fx, 5, s.sub_id).active_unit_type, 2u,
              "C23: the success write landed on PLAYER 5's production row (active_unit_type=slot=2), "
              "using the freshly re-read player -- NOT player 0's row");
        ck_eq((uint32_t)prow(fx, 0, s.sub_id).active_unit_type, s.active_unit_type,
              "C23: player 0's production row's active_unit_type is UNTOUCHED by the success write "
              "(still its seeded value) -- proves the write targeted player 5, not player 0");
        ck(g_notify_lifecycle_calls.size() == 1 && g_notify_lifecycle_calls[0].player == 5,
           "C23: ai_notify_unit_lifecycle also carries player=5 (freshly re-read), not the stale 0");
    }
}

} // namespace mh::sim::test
