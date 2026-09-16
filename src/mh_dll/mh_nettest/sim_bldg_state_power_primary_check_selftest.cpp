//
// sim_bldg_state_power_primary_check_selftest.cpp -- `simtest` cases for
// llm_strat_bldg_state_power_primary_check (sim/sim_bldg_state_power.h/.cpp @0x00474707), SIM1-G4
// building_tick machinery -- "is this building the roster's tracked primary mother/power-plant
// building for its planet".
//
// SCOPE, hand-derived from the DISASSEMBLY
// (tmp/decomp_sim/llm_strat_bldg_state_power_primary_check_00474707.asm), NOT the sibling .c draft:
//
//   Gate (0x0047471f-0x0047475d): `if ((uint32_t)cur_index ==
//   _G_LLM_STRAT_PLAYERS[cur_player].primary_mother_bldg[G_PLANET_INDEX]) { cur_building->state =
//   0x8b /* POWER_GENERATE */; } else { cur_building->state = 0x8c /* IDLE_NOOP_8C */; }` --
//   written through the cur_building POINTER (state@0xd, uint16_t). No cfg read anywhere in this
//   function -- it is exactly the one compare (NOT a call, unlike hangar_recharge_check's gate),
//   the branch, and the tail.
//   Tail, UNCONDITIONAL on BOTH arms (0x0047475d-0x00474770): `llm_strat_bldg_notify_ui(cur_player,
//   cur_index)` -- ONE call only, unlike hangar_recharge_check's sibling shape which also calls
//   llm_strat_refresh_building.
//
// PIN LIST (each ck message below names the mechanism + its address):
//   - cur_index == primary_mother_bldg[planet] -> state = POWER_GENERATE (0x8b)          (C1)
//   - cur_index != primary_mother_bldg[planet] -> state = IDLE_NOOP_8C (0x8c)             (C2)
//   - off-by-one on BOTH sides of the primary index (primary-1 and primary+1) still take the
//     NOT-EQUAL arm (0x8c) -- rules off a translation that used <= or >= instead of ==       (C3a, C3b)
//   - the tail's notify_ui call fires exactly once on EACH arm, with (cur_player, cur_index) in
//     that order, not swapped                                                          (C1,C2,C4,C5)
//   - player/planet_index are DISTINCT, non-symmetric, and one case swaps their relative magnitude
//     (planet < player) to catch a translation that read profiles[planet].primary_mother_bldg[player]
//     (indices swapped) instead of profiles[player].primary_mother_bldg[planet]              (C4)
//   - no second callee anywhere in this closure -- unlike hangar_recharge_check there is no
//     llm_strat_refresh_building call to check for; g_notify_calls.size()==1 on every case is
//     itself the "only one call" pin
//
// NOT PINNED, and why: hangar_recharge_check's own C5 mutates cur_player/cur_index INSIDE its gate
// CALL's recorder to prove the tail calls re-read the ambient globals fresh rather than reusing a
// value cached before the call. This function's gate is a plain compare (0x0047471f-0x0047473d), not
// a call -- there is no callee between the compare and the tail's own reads of cur_player/cur_index
// (0x0047475d/0x00474764) into which a mutation could be hooked. The .cpp under test
// (sim_bldg_state_power.cpp) reads `*v.cur_player`/`*v.cur_index` ONCE into locals at the top of the
// function and reuses them at the notify_ui call site, where the .asm issues fresh MOVZX reads at
// 0x0047475d/0x00474764 instead of carrying a register across. The two are NOT distinguishable by any
// case this closure can construct (nothing else in the closure can write those globals mid-call), so
// this is recorded here as an observed .cpp/.asm shape difference, not a bug and not a testable pin.
//
#include "sim/sim_bldg_state_power.h"

#include <cstdint>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- per-callee recorder (1, the only member of bldg_state_power_primary_check_calls) -----------
struct NotifyUiCall {
    uint16_t player;
    uint32_t index;
};
std::vector<NotifyUiCall> g_notify_ui_calls;
void                      rec_bldg_notify_ui(uint16_t player, uint32_t index) {
    g_notify_ui_calls.push_back({player, index});
}

const bldg_state_power_primary_check_calls g_calls = {
    &rec_bldg_notify_ui,
};

void reset_observations() { g_notify_ui_calls.clear(); }

// ---- fixture seeding -------------------------------------------------------------------------------
struct Seed {
    // Distinct, non-symmetric player/planet/cur_index so a swapped-index or swapped-arg translation
    // is caught by value, not just by accident of two fields being equal.
    uint16_t player    = 3;
    int32_t  planet    = 20;
    int32_t  cur_index = 44; // also the roster building slot -- must stay < BUILDINGS_PER_PLAYER (100)

    int32_t primary_mother_bldg_seed = 44; // case overrides to walk the gate

    // A sentinel PRE-EXISTING state value, distinct from BOTH branch outcomes (0x8b/0x8c) and from
    // 0, so a case that forgets to check the gate at all (leaving state untouched) does not
    // accidentally read as passing.
    uint16_t state_in = 0x55;
};

void seed_and_run(sim_fixture &fx, const Seed &s) {
    fx.reset();

    building &b = fx.b(s.player, s.cur_index);
    b.state     = s.state_in;

    fx.cur_building_ptr = &b;
    fx.view_cur_player  = s.player;
    fx.view_cur_index   = (uint16_t)s.cur_index;
    fx.planet_index     = s.planet;

    fx.profiles[s.player].primary_mother_bldg[s.planet] = s.primary_mother_bldg_seed;

    reset_observations();

    sim_store own = fx.store();
    detail::bldg_state_power_primary_check(fx.view(), own, g_calls);
}

} // namespace

void run_bldg_state_power_primary_check_tests() {
    sim_fixture fx;

    // =================================================================================================
    // C1 -- cur_index == primary_mother_bldg[planet] (0x0047473d CMP / 0x00474743 JNZ NOT taken):
    // state -> POWER_GENERATE (0x8b, `MOV word ptr [EAX+0xd],0x8b` @0x0047474a). This building IS the
    // roster's tracked primary power plant for its planet.
    // =================================================================================================
    {
        Seed s;
        s.player                   = 3;
        s.planet                   = 20;
        s.cur_index                = 44;
        s.primary_mother_bldg_seed = 44; // == cur_index
        seed_and_run(fx, s);

        ck_eq((uint32_t)fx.b(s.player, s.cur_index).state, 0x8bu,
              "C1: cur_index==primary_mother_bldg[planet] -> state=POWER_GENERATE (0x8b), "
              "CMP @0x0047473d / JNZ @0x00474743 NOT taken -> `MOV word ptr [EAX+0xd],0x8b` "
              "@0x0047474a");

        ck(g_notify_ui_calls.size() == 1 && g_notify_ui_calls[0].player == s.player &&
               g_notify_ui_calls[0].index == (uint32_t)s.cur_index,
           "C1: llm_strat_bldg_notify_ui(cur_player, cur_index) fires exactly once with (3, 44) on "
           "the POWER_GENERATE arm (0x0047475d-0x0047476b)");
    }

    // =================================================================================================
    // C2 -- cur_index != primary_mother_bldg[planet] (JNZ @0x00474743 TAKEN -> LAB_00474752): state ->
    // IDLE_NOOP_8C (0x8c, `MOV word ptr [EAX+0xd],0x8c` @0x00474757). Same unconditional tail, proven
    // fired on THIS arm too (the inverse half of the "not gated" pin -- there is no gate to gate the
    // tail behind here, but the case still proves the tail is truly unconditional on the compare
    // outcome).
    // =================================================================================================
    {
        Seed s;
        s.player                   = 3;
        s.planet                   = 20;
        s.cur_index                = 44;
        s.primary_mother_bldg_seed = 77; // != cur_index, distinct from both 0 and cur_index
        seed_and_run(fx, s);

        ck_eq((uint32_t)fx.b(s.player, s.cur_index).state, 0x8cu,
              "C2: cur_index!=primary_mother_bldg[planet] -> state=IDLE_NOOP_8C (0x8c), JNZ "
              "@0x00474743 TAKEN -> LAB_00474752 `MOV word ptr [EAX+0xd],0x8c` @0x00474757");

        ck(g_notify_ui_calls.size() == 1 && g_notify_ui_calls[0].player == s.player &&
               g_notify_ui_calls[0].index == (uint32_t)s.cur_index,
           "C2: llm_strat_bldg_notify_ui(cur_player, cur_index) fires exactly once with (3, 44) on "
           "the IDLE_NOOP_8C arm too (0x0047475d-0x0047476b) -- unconditional on the compare result");
    }

    // =================================================================================================
    // C3a/C3b -- OFF-BY-ONE pin: primary_mother_bldg[planet]==5 with cur_index one BELOW (4) and one
    // ABOVE (6) the primary slot must BOTH take the NOT-EQUAL arm (0x8c). Rules off a translation that
    // wrote `<=`/`>=`/`<`/`>` instead of the original's exact `==` (the CMP/JNZ pair at
    // 0x0047473d-0x00474743 branches on EXACT equality only).
    // =================================================================================================
    {
        Seed s;
        s.player                   = 3;
        s.planet                   = 20;
        s.primary_mother_bldg_seed = 5;
        s.cur_index                = 4; // primary - 1
        seed_and_run(fx, s);
        ck_eq((uint32_t)fx.b(s.player, s.cur_index).state, 0x8cu,
              "C3a: cur_index=primary-1 (4 vs primary=5) -> STILL state=IDLE_NOOP_8C (0x8c) -- "
              "CMP @0x0047473d is exact equality, not a range test");
    }
    {
        Seed s;
        s.player                   = 3;
        s.planet                   = 20;
        s.primary_mother_bldg_seed = 5;
        s.cur_index                = 6; // primary + 1
        seed_and_run(fx, s);
        ck_eq((uint32_t)fx.b(s.player, s.cur_index).state, 0x8cu,
              "C3b: cur_index=primary+1 (6 vs primary=5) -> STILL state=IDLE_NOOP_8C (0x8c) -- "
              "same exact-equality pin as C3a from the other side");
    }

    // =================================================================================================
    // C4 -- INDEX-SWAP pin: planet DELIBERATELY smaller than player (planet=2 < player=6, the opposite
    // magnitude relationship from C1-C3's player=3/planet=20) so a translation that read
    // `profiles[planet].primary_mother_bldg[player]` (the two indices swapped) disagrees with the
    // fixture instead of accidentally agreeing. EQUAL arm exercised here; C5 below exercises the
    // NOT-EQUAL arm with its own distinct magnitudes.
    // =================================================================================================
    {
        Seed s;
        s.player                   = 6;
        s.planet                   = 2;
        s.cur_index                = 10;
        s.primary_mother_bldg_seed = 10; // == cur_index
        seed_and_run(fx, s);

        ck_eq((uint32_t)fx.b(s.player, s.cur_index).state, 0x8bu,
              "C4: profiles[player=6].primary_mother_bldg[planet=2]==cur_index=10 -> state=0x8b -- "
              "planet < player here, catching a player/planet index swap");

        ck(g_notify_ui_calls.size() == 1 && g_notify_ui_calls[0].player == s.player &&
               g_notify_ui_calls[0].index == (uint32_t)s.cur_index,
           "C4: notify_ui uses the SAME (cur_player=6, cur_index=10) pair, not a swapped one "
           "(0x0047475d-0x0047476b)");
    }

    // =================================================================================================
    // C5 -- second index-swap + argument-order pin on the NOT-EQUAL arm, with yet another distinct
    // player/planet/cur_index combination (player=1, planet=7, cur_index=5, primary=99) so no two
    // cases in this file share a seed by accident.
    // =================================================================================================
    {
        Seed s;
        s.player                   = 1;
        s.planet                   = 7;
        s.cur_index                = 5;
        s.primary_mother_bldg_seed = 99; // != cur_index, and != planet/player too
        seed_and_run(fx, s);

        ck_eq((uint32_t)fx.b(s.player, s.cur_index).state, 0x8cu,
              "C5: profiles[player=1].primary_mother_bldg[planet=7]=99 != cur_index=5 -> state=0x8c");

        ck(g_notify_ui_calls.size() == 1 && g_notify_ui_calls[0].player == s.player &&
               g_notify_ui_calls[0].index == (uint32_t)s.cur_index,
           "C5: notify_ui fires once with (cur_player=1, cur_index=5), not swapped and not the "
           "unrelated planet/primary values (0x0047475d-0x0047476b)");
    }
}

} // namespace mh::sim::test
