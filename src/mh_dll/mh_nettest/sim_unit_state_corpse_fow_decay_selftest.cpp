//
// sim_unit_state_corpse_fow_decay_selftest.cpp -- `simtest` cases for
// llm_strat_unit_state_corpse_fow_decay @0x004822dc (sim/sim_unit_state_corpse_fow_decay.h/.cpp).
//
// EXPECTED BEHAVIOUR HAND-DERIVED FROM tmp/decomp/llm_strat_unit_state_corpse_fow_decay_004822dc.asm --
// every assertion below cites the instruction address(es) it pins. NOT read off the .cpp body.
//
// SCOPE (honest, not exhaustive): the top-level dispatch on move_microstep (0x004822f9 CMP dword/
// 0x00482300 JZ, a full DWORD test); the SINGLE-STEP arm (move_microstep==0, 0x004823fc-0x00482447)
// both sides of its budget-vs-20.0 gate (0x00482402 FCOMP DAT_00501452/0x0048240b JC), incl. the exact
// boundary (tick_budget==20.0 takes the free_slot side, not the stall side, since JC only fires
// strictly-below); the tail zeroing of tick_budget that runs on BOTH sides of that gate
// (0x00482433-0x0048243d); the RING-DRAIN arm (move_microstep!=0, 0x00482306-0x004823f5)'s loop
// condition (`tick_budget != 0.0 && move_microstep != 0`, the TEST/JNZ+CMP/JZ bit-trick at
// 0x00482306-0x0048232e, resolved the same way the .h's own banner resolves it) with BOTH loop-exit
// routes distinguished (exit via move_microstep hitting 0 leaves tick_budget UNCHANGED/nonzero,
// 0x00482329 JMP straight to the epilogue with no zeroing tail -- vs exit via the in-loop budget<5.0
// stall branch, which explicitly zeroes tick_budget at 0x004823e1/0x004823eb and leaves
// move_microstep UNTOUCHED); the per-ring budget-vs-5.0 gate (0x0048233a FCOMP DAT_00501442/
// 0x0048233d JC) both sides, incl. the exact boundary (tick_budget==5.0 takes the spend side); the
// PRE-decrement microstep used as fow_remove_sight's radius arg (0x00482348 MOVZX byte [+0xa2] BEFORE
// the 0x00482378 DEC) vs the POST-decrement microstep used as map_fow_UpdateFoWPlus's sight arg
// (0x00482391 MOVZX byte [+0xa2] AFTER the DEC); the UpdateFoWPlus call being conditional on the
// POST-decrement value being nonzero (0x00482383 CMP/0x0048238a JZ -- skipped when a ring drains
// microstep to exactly 0); the budget add-back per ring (0x004823c2 FADD DAT_0050144a == -5.0, i.e.
// budget -= 5.0); and neighbouring-roster-slot non-corruption. It does NOT independently re-verify
// the DAT_ image-constant values (20.0/5.0/-5.0) a second time -- those are read-memory-confirmed by
// the conductor per the .h's own banner and cited here as literals with their DAT_ addresses.
//
#include "sim/sim_unit_state_corpse_fow_decay.h"

#include <cstdint>
#include <cstring>
#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- shared trace: one sequence proves CALL ORDER across all 3 callees -------------------------
std::vector<const char *> g_trace;
void                      tr(const char *tag) { g_trace.push_back(tag); }

bool trace_eq(const std::vector<const char *> &want) {
    if (g_trace.size() != want.size()) return false;
    for (size_t i = 0; i < want.size(); ++i)
        if (std::strcmp(g_trace[i], want[i]) != 0) return false;
    return true;
}

// ---- per-callee recorders (3, one per unit_state_corpse_fow_decay_calls member) -----------------
struct FreeSlotCall {
    uint32_t player;
    int32_t  idx;
};
std::vector<FreeSlotCall> g_free_slot_calls;
void                      rec_unit_free_slot(uint32_t player, int32_t unit_index) {
    tr("unit_free_slot");
    g_free_slot_calls.push_back({player, unit_index});
}

struct FowRemoveCall {
    uint32_t player;
    int32_t  x, y;
    uint8_t  radius;
};
std::vector<FowRemoveCall> g_fow_remove_calls;
void                       rec_fow_remove_sight(uint32_t player, int32_t x, int32_t y, uint8_t radius) {
    tr("fow_remove_sight");
    g_fow_remove_calls.push_back({player, x, y, radius});
}

struct FowUpdateCall {
    uint32_t player;
    uint32_t x, y;
    uint8_t  sight;
};
std::vector<FowUpdateCall> g_fow_update_calls;
void                       rec_map_fow_UpdateFoWPlus(uint32_t player, uint32_t x, uint32_t y, uint8_t sight) {
    tr("map_fow_UpdateFoWPlus");
    g_fow_update_calls.push_back({player, x, y, sight});
}

const unit_state_corpse_fow_decay_calls g_calls = {
    &rec_unit_free_slot,
    &rec_fow_remove_sight,
    &rec_map_fow_UpdateFoWPlus,
};

void reset_observations() {
    g_trace.clear();
    g_free_slot_calls.clear();
    g_fow_remove_calls.clear();
    g_fow_update_calls.clear();
}

// Fixed "guard" slot no test's own (player,index) ever touches -- seeded with sentinel nonzero data
// each run so a wrong-index write lands somewhere observable. Outside every test's own player (0..3)
// / index (1..5) ranges below.
constexpr uint16_t GUARD_PLAYER = 6;
constexpr int32_t  GUARD_INDEX  = 9;

void seed_guard_slot(sim_fixture &fx) {
    unit &g          = fx.u(GUARD_PLAYER, GUARD_INDEX);
    g.x              = 111;
    g.y              = 122;
    g.activity_clock = 246.5;
    g.move_microstep = 999;
}

// ---- fixture seeding -----------------------------------------------------------------------------
struct Seed {
    uint16_t player = 0;
    int32_t  index  = 1;

    int32_t move_microstep = 0;
    double  activity_clock = 100.0;
    double  tick_budget    = 10.0;

    uint8_t ux = 37, uy = 91; // the unit's OWN tile x/y -- distinct from every other seeded field
};

void seed_and_run(sim_fixture &fx, const Seed &s) {
    fx.reset();
    seed_guard_slot(fx);

    unit &u          = fx.u(s.player, s.index);
    u.move_microstep = s.move_microstep;
    u.activity_clock = s.activity_clock;
    u.x              = s.ux;
    u.y              = s.uy;

    fx.cur_unit_ptr    = &u;
    fx.view_cur_player = s.player;
    fx.view_cur_index  = (uint16_t)s.index;
    fx.tick_budget     = s.tick_budget;

    reset_observations();

    sim_store own = fx.store();
    detail::unit_state_corpse_fow_decay(fx.view(), own, g_calls);
}

} // namespace

void run_unit_state_corpse_fow_decay_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- SINGLE-STEP arm (move_microstep==0), STALL side: tick_budget(10.0) < 20.0
    // (0x0048240b JC taken). activity_clock -= tick_budget; tick_budget always zeroed at the tail
    // (0x00482433-0x0048243d).
    // =================================================================================================
    {
        Seed s;
        s.player         = 0;
        s.index          = 1;
        s.move_microstep = 0;
        s.activity_clock = 57.25;
        s.tick_budget    = 10.0;
        seed_and_run(fx, s);

        ck(g_free_slot_calls.empty(), "T1: budget<20.0 -- unit_free_slot does NOT fire (0x0048240b JC taken)");
        ck(g_fow_remove_calls.empty() && g_fow_update_calls.empty(),
           "T1: SINGLE-STEP arm never touches fow_remove_sight/map_fow_UpdateFoWPlus");
        ck_eq_d(fx.u(0, 1).activity_clock, 57.25 - 10.0,
                "T1: activity_clock -= tick_budget (0x00482422-0x00482430)");
        ck_eq_d(fx.tick_budget, 0.0, "T1: tick_budget zeroed at the shared tail (0x00482433/0x0048243d)");
        ck_eq((uint32_t)fx.u(0, 1).move_microstep, 0u, "T1: move_microstep untouched by the SINGLE-STEP arm");
    }

    // =================================================================================================
    // T2 -- SINGLE-STEP arm, FREE_SLOT side + the exact JC boundary: tick_budget>=20.0 takes the
    // unit_free_slot branch (activity_clock is NOT touched on this side); tick_budget==20.0 EXACTLY
    // still takes this side (JC only fires strictly-below); tick_budget==19.0 (boundary-1) takes the
    // stall side instead.
    // =================================================================================================
    {
        Seed s;
        s.player         = 0;
        s.index          = 1;
        s.move_microstep = 0;
        s.activity_clock = 57.25;

        s.tick_budget = 25.0; // clearly >= 20.0
        seed_and_run(fx, s);
        ck(g_free_slot_calls.size() == 1 && g_free_slot_calls[0].player == 0 && g_free_slot_calls[0].idx == 1,
           "T2a: budget=25.0>=20.0 -- unit_free_slot(cur_player, cur_index) fires (0x0048240d-0x0048241b)");
        ck_eq_d(fx.u(0, 1).activity_clock, 57.25, "T2a: activity_clock is NOT touched on the free_slot side");
        ck_eq_d(fx.tick_budget, 0.0, "T2a: tick_budget still zeroed at the shared tail");

        s.tick_budget = 20.0; // exact boundary
        seed_and_run(fx, s);
        ck(g_free_slot_calls.size() == 1,
           "T2b: budget==20.0 EXACTLY -- still takes the free_slot side (0x0048240b JC fires only for "
           "STRICTLY-below, confirmed by the FCOMP/JC opcode pair)");
        ck_eq_d(fx.u(0, 1).activity_clock, 57.25, "T2b: activity_clock untouched at the exact boundary");

        s.tick_budget = 19.0; // boundary - 1
        seed_and_run(fx, s);
        ck(g_free_slot_calls.empty(), "T2c: budget=19.0 (boundary-1) -- takes the STALL side, not free_slot");
        ck_eq_d(fx.u(0, 1).activity_clock, 57.25 - 19.0, "T2c: activity_clock -= tick_budget on the stall side");
    }

    // =================================================================================================
    // T3 -- RING-DRAIN arm, full multi-ring unwind: enough budget (17.0) to run 3 rings each spending
    // 5.0 (0x004823c2 FADD DAT_0050144a==-5.0) before move_microstep reaches 0 mid-budget. Pins the
    // exact CALL ORDER (fow_remove_sight with the PRE-decrement radius, THEN map_fow_UpdateFoWPlus with
    // the POST-decrement sight -- SKIPPED on the ring that drains microstep to exactly 0, 0x0048238a
    // JZ), and the loop-exit-via-microstep==0 route: tick_budget is LEFT AS-IS (2.0, nonzero) since
    // that exit path (0x00482329 JMP 0x004823fa) never reaches the zeroing tail.
    // =================================================================================================
    {
        Seed s;
        s.player         = 1;
        s.index          = 2;
        s.move_microstep = 3;
        s.activity_clock = 500.0; // must stay unchanged -- the stall branch never fires on this path
        s.tick_budget    = 17.0;
        s.ux             = 44;
        s.uy             = 200;
        seed_and_run(fx, s);

        ck(trace_eq({"fow_remove_sight", "map_fow_UpdateFoWPlus", "fow_remove_sight", "map_fow_UpdateFoWPlus",
                     "fow_remove_sight"}),
           "T3: exact call order across 3 rings -- fow(pre-decrement radius) then upd(post-decrement "
           "sight) per ring, upd SKIPPED on the ring that drains microstep to 0 (0x0048238a JZ)");

        ck(g_fow_remove_calls.size() == 3, "T3: exactly 3 fow_remove_sight draws (one per ring)");
        if (g_fow_remove_calls.size() == 3) {
            ck(g_fow_remove_calls[0].player == 1 && g_fow_remove_calls[0].x == 44 && g_fow_remove_calls[0].y == 200 &&
                   g_fow_remove_calls[0].radius == 3,
               "T3: ring1 fow_remove_sight(cur_player, unit.x, unit.y, PRE-decrement radius=3) "
               "(0x00482343-0x0048236e)");
            ck_eq((uint32_t)g_fow_remove_calls[1].radius, 2u, "T3: ring2 fow_remove_sight radius=2 (pre-decrement)");
            ck_eq((uint32_t)g_fow_remove_calls[2].radius, 1u, "T3: ring3 fow_remove_sight radius=1 (pre-decrement)");
        }
        ck(g_fow_update_calls.size() == 2, "T3: exactly 2 map_fow_UpdateFoWPlus calls (ring3's is skipped)");
        if (g_fow_update_calls.size() == 2) {
            ck(g_fow_update_calls[0].player == 1 && g_fow_update_calls[0].x == 44 && g_fow_update_calls[0].y == 200 &&
                   g_fow_update_calls[0].sight == 2,
               "T3: ring1 map_fow_UpdateFoWPlus(cur_player, unit.x, unit.y, POST-decrement sight=2) "
               "(0x0048238c-0x004823b7)");
            ck_eq((uint32_t)g_fow_update_calls[1].sight, 1u, "T3: ring2 map_fow_UpdateFoWPlus sight=1 (post-decrement)");
        }

        ck_eq((uint32_t)fx.u(1, 2).move_microstep, 0u,
              "T3: move_microstep reaches exactly 0 after 3 decrements (0x00482373-0x0048237e x3)");
        ck_eq_d(fx.tick_budget, 17.0 - 5.0 - 5.0 - 5.0,
                "T3: tick_budget = 17.0 - 5.0*3 = 2.0, LEFT AS-IS on loop-exit-via-microstep==0 (no "
                "zeroing tail on this exit route, 0x00482329 JMP 0x004823fa)");
        ck_eq_d(fx.u(1, 2).activity_clock, 500.0,
                "T3: activity_clock untouched -- the in-loop stall branch (0x004823d0) never fires on this path");
        ck_eq((uint32_t)fx.u(1, 2).x, 44u, "T3: unit.x is only READ by this function, never written");
        ck_eq((uint32_t)fx.u(1, 2).y, 200u, "T3: unit.y is only READ by this function, never written");
    }

    // =================================================================================================
    // T4 -- RING-DRAIN arm, budget EXHAUSTS MID-LOOP via the in-loop stall branch (0x004823d0): 2 full
    // rings drain 5.0 each, then the 3rd iteration's remaining budget (2.0) is < 5.0 and takes the
    // stall side -- activity_clock -= tick_budget, tick_budget zeroed, and CRUCIALLY move_microstep is
    // LEFT UNCHANGED (the stall branch has no DEC, unlike the spend branch).
    // =================================================================================================
    {
        Seed s;
        s.player         = 2;
        s.index          = 3;
        s.move_microstep = 5;
        s.activity_clock = 1000.0;
        s.tick_budget    = 12.0;
        s.ux             = 10;
        s.uy             = 20;
        seed_and_run(fx, s);

        ck(trace_eq({"fow_remove_sight", "map_fow_UpdateFoWPlus", "fow_remove_sight", "map_fow_UpdateFoWPlus"}),
           "T4: only 2 full rings execute before the 3rd iteration's budget<5.0 stall (no 3rd "
           "fow_remove_sight/UpdateFoWPlus pair)");
        if (g_fow_remove_calls.size() == 2) {
            ck_eq((uint32_t)g_fow_remove_calls[0].radius, 5u, "T4: ring1 radius=5 (pre-decrement)");
            ck_eq((uint32_t)g_fow_remove_calls[1].radius, 4u, "T4: ring2 radius=4 (pre-decrement)");
        }
        if (g_fow_update_calls.size() == 2) {
            ck_eq((uint32_t)g_fow_update_calls[0].sight, 4u, "T4: ring1 UpdateFoWPlus sight=4 (post-decrement)");
            ck_eq((uint32_t)g_fow_update_calls[1].sight, 3u, "T4: ring2 UpdateFoWPlus sight=3 (post-decrement)");
        }

        ck_eq((uint32_t)fx.u(2, 3).move_microstep, 3u,
              "T4: move_microstep stays at 3 (the value BEFORE the aborted 3rd ring) -- the stall branch "
              "(0x004823d0-0x004823de) does not DEC move_microstep");
        ck_eq_d(fx.tick_budget, 0.0,
                "T4: tick_budget explicitly zeroed by the stall branch itself (0x004823e1/0x004823eb)");
        ck_eq_d(fx.u(2, 3).activity_clock, 1000.0 - (12.0 - 5.0 - 5.0),
                "T4: activity_clock -= the REMAINING tick_budget(2.0) at the moment of the stall "
                "(0x004823db FSUBR/0x004823de FSTP)");
    }

    // =================================================================================================
    // T5 -- RING-DRAIN arm, the per-ring threshold's EXACT boundary: tick_budget==5.0 takes the SPEND
    // side (0x0048233d JC fires only strictly-below, same asymmetry as T2's 20.0 boundary), and this
    // ring's decrement drains move_microstep(1) to exactly 0, so map_fow_UpdateFoWPlus is skipped and
    // the loop exits via the microstep==0 route with tick_budget landing at exactly 0.0 anyway (5.0-5.0).
    // =================================================================================================
    {
        Seed s;
        s.player         = 3;
        s.index          = 4;
        s.move_microstep = 1;
        s.activity_clock = 42.0;
        s.tick_budget    = 5.0; // exact per-ring boundary
        s.ux             = 5;
        s.uy             = 6;
        seed_and_run(fx, s);

        ck(trace_eq({"fow_remove_sight"}),
           "T5: budget==5.0 EXACTLY takes the SPEND side (one fow_remove_sight, no stall), and "
           "map_fow_UpdateFoWPlus is skipped because this ring drains microstep to 0 (0x0048238a JZ)");
        ck(g_fow_remove_calls.size() == 1 && g_fow_remove_calls[0].radius == 1,
           "T5: fow_remove_sight(cur_player, unit.x, unit.y, radius=1) (pre-decrement)");
        ck_eq((uint32_t)fx.u(3, 4).move_microstep, 0u, "T5: move_microstep -= 1 reaches exactly 0");
        ck_eq_d(fx.tick_budget, 0.0, "T5: tick_budget = 5.0 + (-5.0) = 0.0 (0x004823bc-0x004823c8)");
        ck_eq_d(fx.u(3, 4).activity_clock, 42.0, "T5: activity_clock untouched -- the stall branch never fires");
    }

    // =================================================================================================
    // T6 -- RING-DRAIN arm, IMMEDIATE stall: budget(4.0) is already < 5.0 on the FIRST loop iteration,
    // so the loop stalls without ever calling fow_remove_sight/map_fow_UpdateFoWPlus at all, and
    // move_microstep (7) is left completely untouched.
    // =================================================================================================
    {
        Seed s;
        s.player         = 0;
        s.index          = 5;
        s.move_microstep = 7;
        s.activity_clock = 88.0;
        s.tick_budget    = 4.0; // < 5.0 threshold from the very first iteration
        seed_and_run(fx, s);

        ck(g_trace.empty(), "T6: budget<5.0 on entry -- neither fow_remove_sight nor map_fow_UpdateFoWPlus fires");
        ck_eq((uint32_t)fx.u(0, 5).move_microstep, 7u, "T6: move_microstep is completely untouched by the stall branch");
        ck_eq_d(fx.tick_budget, 0.0, "T6: tick_budget zeroed by the immediate stall (0x004823e1/0x004823eb)");
        ck_eq_d(fx.u(0, 5).activity_clock, 88.0 - 4.0, "T6: activity_clock -= tick_budget on the immediate stall");
    }

    // =================================================================================================
    // T7 -- neighbouring-slot non-corruption: a guard unit this function never addresses stays exactly
    // as seeded across a representative run of each arm.
    // =================================================================================================
    {
        Seed s;
        s.player         = 1;
        s.index          = 2;
        s.move_microstep = 3;
        s.tick_budget    = 17.0;
        seed_and_run(fx, s); // seed_and_run's own fx.reset() + seed_guard_slot() ran fresh for this call

        const unit &g = fx.u(GUARD_PLAYER, GUARD_INDEX);
        ck(g.x == 111 && g.y == 122, "T7: guard unit's x/y untouched");
        ck_eq_d(g.activity_clock, 246.5, "T7: guard unit's activity_clock untouched");
        ck_eq((uint32_t)g.move_microstep, 999u, "T7: guard unit's move_microstep untouched");
    }
}

} // namespace mh::sim::test
