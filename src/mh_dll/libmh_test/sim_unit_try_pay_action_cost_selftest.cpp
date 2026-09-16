//
// sim_unit_try_pay_action_cost_selftest.cpp -- offline oracle for llm_strat_unit_try_pay_action_cost
// @0x00493254 (RI-SIM, SIM1-G5). See src/mh_dll/libmh/sim/sim_unit_try_pay_action_cost.h for
// the full derivation. Written because the rig's all-AI soak logged 0 calls for this site (a scenario
// gap, not a translation problem) -- offline is the only evidence can produce for it.
//
#include "sim/sim_unit_try_pay_action_cost.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {
using namespace mh::sim;

struct SpendCall {
    int32_t player, res_id, amount;
};
std::vector<SpendCall> g_spend_calls;
void                   rec_spend(int32_t player, int32_t res_id, int32_t amount) {
    g_spend_calls.push_back({player, res_id, amount});
}

const unit_try_pay_action_cost_calls &rec_tpac_calls() {
    static const unit_try_pay_action_cost_calls c = {&rec_spend};
    return c;
}

void reset_tpac_observations() { g_spend_calls.clear(); }

constexpr uint16_t TPAC_PROTO = 4;

// Seeds cfg_units[TPAC_PROTO].resource_2[7] as a terminator-ended cost list and zeroes the target
// player's resource row. Callers then poke individual .id/.val/holdings as each case needs.
void seed_tpac(sim_fixture &f, uint32_t player) {
    for (int i = 0; i < CFG_RESOURCE_SLOTS; ++i) {
        f.cfg_units[TPAC_PROTO].resource_2[i].id  = 0; // terminator by default
        f.cfg_units[TPAC_PROTO].resource_2[i].val = 0;
    }
    for (int i = 0; i < PLAYER_RESOURCE_SLOTS; ++i) f.player_resources[player * PLAYER_RESOURCE_SLOTS + i] = 0;
    f.u(player, 0).unit_proto_id = TPAC_PROTO;
}

void run_tpac_tests() {
    sim_fixture        fx;
    constexpr uint32_t PLAYER = 2;

    // ---- A. fully affordable, single entry: spends it, returns 0 (0x00493374-0x0049337e) -----------
    {
        seed_tpac(fx, PLAYER);
        fx.cfg_units[TPAC_PROTO].resource_2[0]                  = {5, 10}; // id=5, cost=10
        fx.player_resources[PLAYER * PLAYER_RESOURCE_SLOTS + 5] = 20;      // have plenty
        reset_tpac_observations();
        int32_t rc = detail::unit_try_pay_action_cost(fx.view(), rec_tpac_calls(), PLAYER, 0);
        ck_eq(rc, 0, "A (0x00493374): fully affordable -> returns 0");
        ck_eq((uint32_t)g_spend_calls.size(), 1u, "A: exactly one spend call");
        ck(g_spend_calls[0].player == (int32_t)PLAYER && g_spend_calls[0].res_id == 5 &&
               g_spend_calls[0].amount == 10,
           "A (0x00493367): game_SpendResource(player, id=5, val=10)");
    }

    // ---- B. fully affordable, MULTIPLE entries: spends every one, in order --------------------------
    {
        seed_tpac(fx, PLAYER);
        fx.cfg_units[TPAC_PROTO].resource_2[0]                  = {1, 3};
        fx.cfg_units[TPAC_PROTO].resource_2[1]                  = {2, 4};
        fx.cfg_units[TPAC_PROTO].resource_2[2]                  = {3, 5};
        fx.player_resources[PLAYER * PLAYER_RESOURCE_SLOTS + 1] = 100;
        fx.player_resources[PLAYER * PLAYER_RESOURCE_SLOTS + 2] = 100;
        fx.player_resources[PLAYER * PLAYER_RESOURCE_SLOTS + 3] = 100;
        reset_tpac_observations();
        int32_t rc = detail::unit_try_pay_action_cost(fx.view(), rec_tpac_calls(), PLAYER, 0);
        ck_eq(rc, 0, "B: fully affordable, 3 entries -> returns 0");
        ck_eq((uint32_t)g_spend_calls.size(), 3u, "B (0x00493325 loop): all 3 entries spent");
        ck(g_spend_calls[0].res_id == 1 && g_spend_calls[1].res_id == 2 && g_spend_calls[2].res_id == 3,
           "B: spent in cost-list order 1,2,3");
    }

    // ---- C. one insufficient entry: returns id+0x89, NO spend calls at all (0x004932fd) -------------
    {
        seed_tpac(fx, PLAYER);
        fx.cfg_units[TPAC_PROTO].resource_2[0]                  = {7, 50};
        fx.player_resources[PLAYER * PLAYER_RESOURCE_SLOTS + 7] = 10; // short
        reset_tpac_observations();
        int32_t rc = detail::unit_try_pay_action_cost(fx.view(), rec_tpac_calls(), PLAYER, 0);
        ck_eq(rc, 7 + 0x89, "C (0x00493300-0x00493305): single shortfall -> id(7)+0x89");
        ck_eq((uint32_t)g_spend_calls.size(), 0u, "C: pass 2 never runs, no spend calls");
    }

    // ---- D. TWO insufficient entries: the SECOND collapses the code to the flat 0x89 sentinel -------
    // (0x004932f4-0x004932fb: error_code != 0 -> 0x89, not a second id+0x89).
    {
        seed_tpac(fx, PLAYER);
        fx.cfg_units[TPAC_PROTO].resource_2[0]                  = {3, 50}; // short #1
        fx.cfg_units[TPAC_PROTO].resource_2[1]                  = {6, 50}; // short #2
        fx.player_resources[PLAYER * PLAYER_RESOURCE_SLOTS + 3] = 1;
        fx.player_resources[PLAYER * PLAYER_RESOURCE_SLOTS + 6] = 1;
        reset_tpac_observations();
        int32_t rc = detail::unit_try_pay_action_cost(fx.view(), rec_tpac_calls(), PLAYER, 0);
        ck_eq(rc, 0x89, "D: second shortfall collapses to the FLAT sentinel 0x89, not id(6)+0x89");
        ck_eq((uint32_t)g_spend_calls.size(), 0u, "D: no spend calls");
    }

    // ---- E. sufficiency is inclusive: available == cost is affordable (0x004932ec: JGE) -------------
    {
        seed_tpac(fx, PLAYER);
        fx.cfg_units[TPAC_PROTO].resource_2[0]                  = {4, 25};
        fx.player_resources[PLAYER * PLAYER_RESOURCE_SLOTS + 4] = 25; // exactly equal
        reset_tpac_observations();
        int32_t rc = detail::unit_try_pay_action_cost(fx.view(), rec_tpac_calls(), PLAYER, 0);
        ck_eq(rc, 0, "E (0x004932ec JGE): available==cost is affordable, inclusive boundary");
        ck_eq((uint32_t)g_spend_calls.size(), 1u, "E: the equal-cost entry IS spent");
    }

    // ---- F. terminator stops the walk BEFORE the cap of 7: entries after id==0 are never read -------
    {
        seed_tpac(fx, PLAYER);
        fx.cfg_units[TPAC_PROTO].resource_2[0]                  = {1, 5};
        fx.cfg_units[TPAC_PROTO].resource_2[1]                  = {0, 0};   // terminator
        fx.cfg_units[TPAC_PROTO].resource_2[2]                  = {2, 999}; // would be unaffordable if ever read
        fx.player_resources[PLAYER * PLAYER_RESOURCE_SLOTS + 1] = 5;
        fx.player_resources[PLAYER * PLAYER_RESOURCE_SLOTS + 2] = 0; // deliberately can't afford slot 2
        reset_tpac_observations();
        int32_t rc = detail::unit_try_pay_action_cost(fx.view(), rec_tpac_calls(), PLAYER, 0);
        ck_eq(rc, 0, "F (0x0049329c terminator check): stops at id==0, slot 2 never examined");
        ck_eq((uint32_t)g_spend_calls.size(), 1u, "F: only the pre-terminator entry spent");
    }

    // ---- G. the 8th slot (index 7) is never reached: the loop bound is CFG_RESOURCE_SLOTS==7 ---------
    // (0x004932ba/0x00493343: CMP j,0x7 / JL) -- a non-zero id at slot 7 would be unaffordable if read.
    {
        seed_tpac(fx, PLAYER);
        for (int i = 0; i < 7; ++i) fx.cfg_units[TPAC_PROTO].resource_2[i] = {(uint32_t)(i + 1), 1};
        for (int i = 1; i <= 7; ++i) fx.player_resources[PLAYER * PLAYER_RESOURCE_SLOTS + i] = 1;
        reset_tpac_observations();
        int32_t rc = detail::unit_try_pay_action_cost(fx.view(), rec_tpac_calls(), PLAYER, 0);
        ck_eq(rc, 0, "G: all 7 slots (no terminator) fully affordable -> returns 0");
        ck_eq((uint32_t)g_spend_calls.size(), 7u, "G (0x004932ba/0x00493343 CMP j,7): exactly 7 spends, bound respected");
    }

    // ---- H. PRESERVE-BUG: player is truncated to its low 16 bits at every use (0x00493271/0x004932d1/
    // 0x00493363, all MOVZX word) -- a caller-side player with garbage in bits 16-31 behaves identically
    // to the same value masked to 16 bits. -------------------------------------------------------------
    {
        seed_tpac(fx, PLAYER);
        fx.cfg_units[TPAC_PROTO].resource_2[0]                  = {9, 7};
        fx.player_resources[PLAYER * PLAYER_RESOURCE_SLOTS + 9] = 10;
        reset_tpac_observations();
        uint32_t garbage_player = 0x00010000u | PLAYER; // upper 16 bits set, low 16 = PLAYER
        int32_t  rc             = detail::unit_try_pay_action_cost(fx.view(), rec_tpac_calls(), garbage_player, 0);
        ck_eq(rc, 0, "H (0x00493271/0x004932d1/0x00493363 MOVZX word): upper-16-bit garbage truncated, "
                     "same result as player=2 alone");
        ck_eq((uint32_t)g_spend_calls.size(), 1u, "H: one spend call, at the TRUNCATED player");
        ck_eq(g_spend_calls[0].player, (int32_t)PLAYER,
              "H: game_SpendResource sees the truncated player (2), not the raw 0x10002");
    }
}

} // namespace

void run_unit_try_pay_action_cost_tests() { run_tpac_tests(); }

} // namespace mh::sim::test
