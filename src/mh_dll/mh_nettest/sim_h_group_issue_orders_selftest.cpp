//
// sim_h_group_issue_orders_selftest.cpp -- `simtest` OFFLINE ORACLE for
//   llm_strat_group_issue_attack_order        @0x00444894 (tmp/decomp_sim/llm_strat_group_issue_
//     attack_order_00444894.asm)
//   llm_strat_group_issue_enter_building_order @0x004451d2 (tmp/decomp_sim/llm_strat_group_issue_
//     enter_building_order_004451d2.asm)
// (sim/hostreach/sim_h_group_issue_orders.h/.cpp, SIM1-H).
//
// WHY OFFLINE: both are driven by PLAYER INPUT through llm_strat_input_update -- neither is reached
// by the batch's rig fixtures (an all-AI soak + a save-load, no human selection order), so the
// whole-closure A/B cannot see either body. This file is their only evidence, and every case below is
// written to be MUTATION-PROOF: each assertion pins a specific instruction address so a single-branch
// or single-constant mutation in the .cpp fails a specific check here.
//
// EXPECTED BEHAVIOUR, re-derived from the DISASSEMBLY (not the .c drafts -- see the header banner for
// why): a THREE-WAY selector chain (0x20 / 0x40 / 0x80, anything else a no-op), a hardcoded
// _G_LLM_STRAT_CTRL_GROUPS[0] iteration in both functions, a cargo-heli (A_HELI_CARGO=0x17 /
// H_HELI_CARGO=0x18) skip in both, a player==PlayerSide && unit_id==param_1 self-skip in the 0x20 and
// 0x80 arms only (NOT 0x40), a select_weapon()==0x64 skip in all three attack_order arms, and a
// trailing race_alert_sound_emit()/group_order_ack_voice() call gated on "did the loop issue at least
// one order" (a single flag threaded across the whole loop, not a per-iteration local).
//
// THE MODIFIER-KEY ASYMMETRY (the selector==0x80 arm's sharpest fact, and the one the batch brief got
// WRONG before the translator corrected it): RSHIFT (0x004449cb) and LSHIFT (0x004449d4) test ONLY bit
// 0 of their byte; LCTRL (0x004449dd/0x004449e6) and LALT (0x00444a2e/0x00444a37) test bits 0 AND 1.
// So a key byte of 2 (bit 1 only -- what the real WndProc tap ORs in for an EXTENDED scancode) reads as
// HELD for LCTRL/LALT but NOT HELD for LSHIFT/RSHIFT. Several cases below seed exactly 2, never only 0
// or 1, to make that asymmetry observable.
//
#include "sim/hostreach/sim_h_group_issue_orders.h"

#include <map>
#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- fixture constants. DISTINCT, non-symmetric, per sim_test_support.h's own rule. -----------------
constexpr uint16_t PLAYER_SIDE    = 5;  // fixture's PlayerSide
constexpr uint32_t TARGET_PLAYER  = 2;  // the `player` order-owner argument -- DISTINCT from PLAYER_SIDE
constexpr uint32_t ATK_PARAM1     = 42; // attack_order's `param_1` (target unit/building id) argument
constexpr uint8_t  WEAPON_DEFAULT = 7;  // select_weapon's default mocked return -- distinct from the
                                        // literal weapon constant (4) the 0x80 arm's non-attack_unit
                                        // outcomes pass, so a case can tell "select_weapon's result" from
                                        // "the hardcoded 4" apart by the number alone.
constexpr uint32_t CONST_WEAPON   = 4;  // the literal PUSH 0x4 at 0x00444a40/0x00444a63
constexpr uint32_t NON_CARGO_TYPE = 1;  // any cfg Unit.type that is not 0x17/0x18
constexpr uint32_t EB_PARAM1      = 11; // enter_building_order's param_1
constexpr uint32_t EB_PARAM2      = 22; // enter_building_order's param_2
constexpr uint32_t EB_PARAM3      = 33; // enter_building_order's param_3

// ---- select_weapon: records every call; return keyed PER UNIT so one case can drive several units
// down different arms (0x64-skip vs. a real weapon) in a single loop. -----------------------------
struct select_weapon_call {
    uint16_t player;
    int32_t  unit_index;
    uint32_t target_mask;
};
std::vector<select_weapon_call> g_select_weapon_calls;
std::map<int32_t, uint8_t>      g_weapon_return_by_unit;
uint8_t                         rec_select_weapon(uint16_t player, int32_t unit_index, uint32_t target_mask) {
    g_select_weapon_calls.push_back({player, unit_index, target_mask});
    auto it = g_weapon_return_by_unit.find(unit_index);
    return it != g_weapon_return_by_unit.end() ? it->second : WEAPON_DEFAULT;
}

// ---- the four order-issue calls. Same (player, unit_idx, target_side, target_idx, weapon_idx) shape
// for all four -- one recorder struct, four independent call logs so a case can tell them apart. -----
struct order_call {
    uint32_t player;
    int32_t  unit_idx;
    uint32_t target_side;
    int32_t  target_idx;
    uint32_t weapon_idx;
};
std::vector<order_call> g_attack_target_calls;
std::vector<order_call> g_attack_target_alt_calls;
std::vector<order_call> g_attack_unit_calls;
std::vector<order_call> g_attack_building_reposition_calls;

void rec_order_attack_target(uint32_t player, int32_t unit_idx, uint32_t target_side,
                             int32_t target_unit_idx, uint32_t weapon_idx) {
    g_attack_target_calls.push_back({player, unit_idx, target_side, target_unit_idx, weapon_idx});
}
void rec_order_attack_target_alt(uint32_t player, int32_t unit_idx, uint32_t target_side,
                                 int32_t target_unit_idx, uint32_t weapon_idx) {
    g_attack_target_alt_calls.push_back({player, unit_idx, target_side, target_unit_idx, weapon_idx});
}
void rec_order_attack_unit(uint32_t player, int32_t unit_idx, uint32_t target_side,
                           int32_t target_unit_idx, uint32_t weapon_idx) {
    g_attack_unit_calls.push_back({player, unit_idx, target_side, target_unit_idx, weapon_idx});
}
void rec_order_attack_building_reposition(uint32_t player, int32_t unit_idx, uint32_t target_side,
                                          int32_t target_bldg_idx, uint32_t weapon_idx) {
    g_attack_building_reposition_calls.push_back(
        {player, unit_idx, target_side, target_bldg_idx, weapon_idx});
}

// ---- enter_building_order's single order call. ------------------------------------------------------
struct exit_storage_call {
    uint32_t player;
    uint32_t unit_idx;
    int32_t  a2;
    uint32_t param_4;
    uint32_t param_5;
};
std::vector<exit_storage_call> g_exit_storage_calls;
void                           rec_order_exit_storage(uint32_t player, uint32_t unit_idx, int32_t a2, uint32_t param_4,
                                                      uint32_t param_5) {
    g_exit_storage_calls.push_back({player, unit_idx, a2, param_4, param_5});
}

// ---- llm_unit_state_is_boarding: records every call's `state` argument; return keyed by that SAME
// state value so a case can give each group member a distinct boarding outcome by giving it a
// distinct `state`. Defaults to 0 (not boarding) for any state not explicitly configured. -------------
std::vector<int32_t>       g_is_boarding_calls;
std::map<int32_t, int32_t> g_is_boarding_result_by_state;
int32_t                    rec_unit_state_is_boarding(int32_t state) {
    g_is_boarding_calls.push_back(state);
    auto it = g_is_boarding_result_by_state.find(state);
    return it != g_is_boarding_result_by_state.end() ? it->second : 0;
}

int32_t g_race_alert_calls = 0;
void    rec_race_alert_sound_emit() { ++g_race_alert_calls; }
int32_t g_ack_voice_calls = 0;
void    rec_group_order_ack_voice() { ++g_ack_voice_calls; }

const group_issue_orders_calls g_calls = {
    &rec_select_weapon,
    &rec_order_attack_target,
    &rec_order_attack_target_alt,
    &rec_order_attack_unit,
    &rec_order_attack_building_reposition,
    &rec_order_exit_storage,
    &rec_unit_state_is_boarding,
    &rec_race_alert_sound_emit,
    &rec_group_order_ack_voice,
};

void reset_recorders() {
    g_select_weapon_calls.clear();
    g_weapon_return_by_unit.clear();
    g_attack_target_calls.clear();
    g_attack_target_alt_calls.clear();
    g_attack_unit_calls.clear();
    g_attack_building_reposition_calls.clear();
    g_exit_storage_calls.clear();
    g_is_boarding_calls.clear();
    g_is_boarding_result_by_state.clear();
    g_race_alert_calls = 0;
    g_ack_voice_calls  = 0;
}

// _G_LLM_STRAT_CTRL_GROUPS[0] -- the ONLY group index either function reads (hardcoded, see the header
// banner). Every test that is not the dedicated group-index case seeds group 0 only.
void seed_group0(sim_fixture &fx, const std::vector<uint16_t> &ids) {
    fx.ctrl_groups[0].count = static_cast<int32_t>(ids.size());
    for (size_t i = 0; i < ids.size(); ++i) fx.ctrl_groups[0].unit_ids[i] = ids[i];
}
// Seeds group 1 -- used ONLY by the group-index tests, where its contents must be CONTRADICTORY (a
// would-issue unit) so that a translation wrongly reading group 1 instead of group 0 fails loudly.
void seed_group1(sim_fixture &fx, const std::vector<uint16_t> &ids) {
    fx.ctrl_groups[1].count = static_cast<int32_t>(ids.size());
    for (size_t i = 0; i < ids.size(); ++i) fx.ctrl_groups[1].unit_ids[i] = ids[i];
}

unit &make_unit(sim_fixture &fx, int32_t roster_index, uint16_t proto_id, uint32_t unit_type,
                uint16_t state = 0) {
    unit &u                     = fx.u(PLAYER_SIDE, roster_index);
    u.unit_proto_id             = proto_id;
    u.state                     = state;
    fx.cfg_units[proto_id].type = unit_type;
    return u;
}

void setup_common(sim_fixture &fx) {
    fx.reset();
    reset_recorders();
    fx.player_side = static_cast<int16_t>(PLAYER_SIDE);
}

} // namespace

void run_h_group_issue_orders_tests() {
    sim_fixture fx;

    // =====================================================================================================
    // ---- llm_strat_group_issue_attack_order @0x00444894 --------------------------------------------------
    // =====================================================================================================

    // =================================================================================================
    // T1/T2 -- THE THREE-WAY SELECTOR CHAIN: a value in none of {0x20,0x40,0x80} issues nothing and
    // does not call the race-alert emitter, for both a near-miss (0x21) and a far-out-of-range value
    // (0x100).
    // =================================================================================================
    {
        setup_common(fx);
        make_unit(fx, 10, /*proto*/ 1, NON_CARGO_TYPE);
        seed_group0(fx, {10});

        detail::group_issue_attack_order(fx.view(), g_calls, ATK_PARAM1, TARGET_PLAYER,
                                         /*selector*/ 0x21);

        ck_eq((uint32_t)g_select_weapon_calls.size(), 0u,
              "T1: selector=0x21 (neither <0x40+==0x20 nor ==0x40 nor ==0x80) -- select_weapon never "
              "called, 0x004448ea");
        ck_eq((uint32_t)g_race_alert_calls, 0u,
              "T1: selector=0x21 -- race_alert_sound_emit NOT called (any_issued stays false), "
              "0x00444c89/0x00444c8d");
    }
    {
        setup_common(fx);
        make_unit(fx, 10, /*proto*/ 1, NON_CARGO_TYPE);
        seed_group0(fx, {10});

        detail::group_issue_attack_order(fx.view(), g_calls, ATK_PARAM1, TARGET_PLAYER,
                                         /*selector*/ 0x100);

        ck_eq((uint32_t)g_select_weapon_calls.size(), 0u,
              "T2: selector=0x100 (fails <0x40, fails ==0x40, fails ==0x80) -- select_weapon never "
              "called, 0x004448da");
        ck_eq((uint32_t)g_race_alert_calls, 0u, "T2: selector=0x100 -- no race_alert_sound_emit call");
    }

    // =================================================================================================
    // T3 -- selector==0x20 baseline: mode=2 passed to select_weapon (0x00444b24-0x00444b3c), its
    // result forwarded (not a constant) to order_attack_target, race_alert_sound_emit fires.
    // =================================================================================================
    {
        setup_common(fx);
        make_unit(fx, 10, /*proto*/ 1, NON_CARGO_TYPE);
        seed_group0(fx, {10});

        detail::group_issue_attack_order(fx.view(), g_calls, ATK_PARAM1, TARGET_PLAYER,
                                         /*selector*/ 0x20);

        ck_eq((uint32_t)g_select_weapon_calls.size(), 1u, "T3: select_weapon called once, 0x00444b3c");
        ck_eq(g_select_weapon_calls[0].target_mask, 2u,
              "T3: selector==0x20 passes mode=2 to select_weapon, 0x00444b24");
        ck_eq((uint32_t)g_attack_target_calls.size(), 1u,
              "T3: order_attack_target called once, 0x00444b8c");
        ck(g_attack_target_calls[0].player == PLAYER_SIDE && g_attack_target_calls[0].unit_idx == 10 &&
               g_attack_target_calls[0].target_side == TARGET_PLAYER &&
               g_attack_target_calls[0].target_idx == (int32_t)ATK_PARAM1 &&
               g_attack_target_calls[0].weapon_idx == WEAPON_DEFAULT,
           "T3: order_attack_target(PlayerSide, unit_id, player, param_1, select_weapon-result) -- "
           "weapon is the FORWARDED result, not a constant");
        ck_eq((uint32_t)g_race_alert_calls, 1u,
              "T3: race_alert_sound_emit fires after an issued order, 0x00444c8f");
    }

    // =================================================================================================
    // T4 -- THE SELF-SKIP (selector==0x20 arm): player==PlayerSide AND unit_id==param_1 skips; either
    // half alone does not. Three subcases pin both halves of the conjunction independently.
    // =================================================================================================
    {
        // T4a: both match -- skipped (sole unit, so also no race_alert).
        setup_common(fx);
        make_unit(fx, (int32_t)ATK_PARAM1, /*proto*/ 1, NON_CARGO_TYPE);
        seed_group0(fx, {(uint16_t)ATK_PARAM1});

        detail::group_issue_attack_order(fx.view(), g_calls, ATK_PARAM1, /*player*/ PLAYER_SIDE,
                                         /*selector*/ 0x20);

        ck_eq((uint32_t)g_attack_target_calls.size(), 0u,
              "T4a: player==PlayerSide && unit_id==param_1 -- self-skip, 0x00444b4d-0x00444b68");
        ck_eq((uint32_t)g_race_alert_calls, 0u, "T4a: self-skip is the sole unit -- no race_alert");
    }
    {
        // T4b: same player, DIFFERENT id -- NOT skipped.
        setup_common(fx);
        make_unit(fx, 99, /*proto*/ 1, NON_CARGO_TYPE);
        seed_group0(fx, {99});

        detail::group_issue_attack_order(fx.view(), g_calls, ATK_PARAM1, /*player*/ PLAYER_SIDE,
                                         /*selector*/ 0x20);

        ck_eq((uint32_t)g_attack_target_calls.size(), 1u,
              "T4b: player==PlayerSide but unit_id!=param_1 -- NOT skipped, 0x00444b59/0x00444b65");
    }
    {
        // T4c: different player, SAME id -- NOT skipped.
        setup_common(fx);
        make_unit(fx, (int32_t)ATK_PARAM1, /*proto*/ 1, NON_CARGO_TYPE);
        seed_group0(fx, {(uint16_t)ATK_PARAM1});

        detail::group_issue_attack_order(fx.view(), g_calls, ATK_PARAM1, /*player*/ TARGET_PLAYER,
                                         /*selector*/ 0x20);

        ck_eq((uint32_t)g_attack_target_calls.size(), 1u,
              "T4c: unit_id==param_1 but player!=PlayerSide -- NOT skipped, 0x00444b4d/0x00444b57");
    }

    // =================================================================================================
    // T5 -- THE 0x64 SKIP: select_weapon returning 0x64 skips the unit entirely; as the sole unit, no
    // race_alert emit either.
    // =================================================================================================
    {
        setup_common(fx);
        make_unit(fx, 10, /*proto*/ 1, NON_CARGO_TYPE);
        seed_group0(fx, {10});
        g_weapon_return_by_unit[10] = 0x64;

        detail::group_issue_attack_order(fx.view(), g_calls, ATK_PARAM1, TARGET_PLAYER,
                                         /*selector*/ 0x20);

        ck_eq((uint32_t)g_attack_target_calls.size(), 0u,
              "T5: select_weapon==0x64 -- unit skipped, no order, 0x00444b47/0x00444b4b");
        ck_eq((uint32_t)g_race_alert_calls, 0u, "T5: 0x64-skip is the sole unit -- no race_alert");
    }

    // =================================================================================================
    // T6 -- THE CARGO-HELI FILTER (selector==0x20 arm): BOTH A_HELI_CARGO(0x17) and H_HELI_CARGO(0x18)
    // are skipped BEFORE select_weapon is ever called for them; a non-cargo unit in the same group
    // still issues.
    // =================================================================================================
    {
        setup_common(fx);
        make_unit(fx, 1, /*proto*/ 1, UNIT_TYPE_A_HELI_CARGO);
        make_unit(fx, 2, /*proto*/ 2, UNIT_TYPE_H_HELI_CARGO);
        make_unit(fx, 3, /*proto*/ 3, NON_CARGO_TYPE);
        seed_group0(fx, {1, 2, 3});

        detail::group_issue_attack_order(fx.view(), g_calls, ATK_PARAM1, TARGET_PLAYER,
                                         /*selector*/ 0x20);

        ck_eq((uint32_t)g_select_weapon_calls.size(), 1u,
              "T6: select_weapon called ONLY for the non-cargo unit, 0x00444ae2/0x00444b19 (cargo "
              "filter precedes the call)");
        ck_eq((uint32_t)g_select_weapon_calls[0].unit_index, 3u,
              "T6: the one select_weapon call is for unit 3, not the two cargo-heli units");
        ck_eq((uint32_t)g_attack_target_calls.size(), 1u, "T6: only the non-cargo unit issues");
        ck_eq((uint32_t)g_attack_target_calls[0].unit_idx, 3u, "T6: the issued order targets unit 3");
    }

    // =================================================================================================
    // T7 -- selector==0x40 arm: mode=1 to select_weapon, the cargo filter and 0x64-skip apply here too,
    // and -- the arm's own distinguishing fact -- there is NO self-exclude check: a unit with
    // unit_id==param_1 && player==PlayerSide still issues (unlike the 0x20/0x80 arms' T4a).
    // =================================================================================================
    {
        setup_common(fx);
        make_unit(fx, 1, /*proto*/ 1, UNIT_TYPE_A_HELI_CARGO);           // cargo -- skip pre-select_weapon
        make_unit(fx, 2, /*proto*/ 2, NON_CARGO_TYPE);                   // 0x64 -- select_weapon called, skip
        make_unit(fx, (int32_t)ATK_PARAM1, /*proto*/ 3, NON_CARGO_TYPE); // id==param_1, issues anyway
        seed_group0(fx, {1, 2, (uint16_t)ATK_PARAM1});
        g_weapon_return_by_unit[2] = 0x64;

        detail::group_issue_attack_order(fx.view(), g_calls, ATK_PARAM1, /*player*/ PLAYER_SIDE,
                                         /*selector*/ 0x40);

        ck_eq((uint32_t)g_select_weapon_calls.size(), 2u,
              "T7: select_weapon called for unit 2 and the param_1-matching unit, NOT the cargo unit");
        ck(g_select_weapon_calls[0].target_mask == 1u && g_select_weapon_calls[1].target_mask == 1u,
           "T7: selector==0x40 passes mode=1 to select_weapon, 0x00444c31");
        ck_eq((uint32_t)g_attack_building_reposition_calls.size(), 1u,
              "T7: order_attack_building_reposition called once, 0x00444c78 -- id==param_1 && "
              "player==PlayerSide did NOT skip (no self-exclude compare exists in this arm)");
        ck(g_attack_building_reposition_calls[0].unit_idx == (int32_t)ATK_PARAM1 &&
               g_attack_building_reposition_calls[0].weapon_idx == WEAPON_DEFAULT,
           "T7: the issuing unit is the param_1-matching one, weapon is select_weapon's forwarded "
           "result");
        ck_eq((uint32_t)g_race_alert_calls, 1u, "T7: race_alert_sound_emit fires");
    }

    // =================================================================================================
    // T8 -- THE TRAILING EMIT, "only the LAST unit issues": a 3-unit loop where units 1 and 2 are
    // skipped (0x64-skip, then self-skip) and only unit 3 issues -- any_issued still ends up true, so
    // race_alert_sound_emit fires once. Proves any_issued is threaded across the WHOLE loop, not read
    // from the last iteration's local state.
    // =================================================================================================
    {
        setup_common(fx);
        make_unit(fx, 1, /*proto*/ 1, NON_CARGO_TYPE);                   // 0x64-skip
        make_unit(fx, (int32_t)ATK_PARAM1, /*proto*/ 2, NON_CARGO_TYPE); // self-skip
        make_unit(fx, 3, /*proto*/ 3, NON_CARGO_TYPE);                   // issues
        seed_group0(fx, {1, (uint16_t)ATK_PARAM1, 3});
        g_weapon_return_by_unit[1] = 0x64;

        detail::group_issue_attack_order(fx.view(), g_calls, ATK_PARAM1, /*player*/ PLAYER_SIDE,
                                         /*selector*/ 0x20);

        ck_eq((uint32_t)g_attack_target_calls.size(), 1u,
              "T8: only unit 3 (the last) issues, 0x00444b6e-0x00444b8c");
        ck_eq((uint32_t)g_attack_target_calls[0].unit_idx, 3u, "T8: the one issued order is unit 3's");
        ck_eq((uint32_t)g_race_alert_calls, 1u,
              "T8: race_alert_sound_emit STILL fires -- any_issued is loop-wide (bVar1 @[EBP-0x1c]), "
              "not the last iteration's local, 0x00444c8f");
    }

    // =================================================================================================
    // T9 -- THE GROUP INDEX: _G_LLM_STRAT_CTRL_GROUPS[0] is HARDCODED (0x00b63be0/0x00b63be4). Group 0
    // is empty; group 1 holds a unit that WOULD issue if read -- a wrong-index translation fails this.
    // =================================================================================================
    {
        setup_common(fx);
        make_unit(fx, 5, /*proto*/ 1, NON_CARGO_TYPE);
        seed_group0(fx, {});
        seed_group1(fx, {5});

        detail::group_issue_attack_order(fx.view(), g_calls, ATK_PARAM1, TARGET_PLAYER,
                                         /*selector*/ 0x20);

        ck_eq((uint32_t)g_select_weapon_calls.size(), 0u,
              "T9: group[0] is empty -- select_weapon never called even though group[1] has a member");
        ck_eq((uint32_t)g_attack_target_calls.size(), 0u, "T9: no order issued from group[1]'s content");
        ck_eq((uint32_t)g_race_alert_calls, 0u, "T9: no race_alert -- group[0].count==0 read, not [1]");
    }

    // =================================================================================================
    // T10-T18 -- selector==0x80 arm, THE MODIFIER-KEY ASYMMETRY. Baseline unit id=7, player=
    // TARGET_PLAYER (!=PLAYER_SIDE, so self-skip never trips regardless of key state).
    // =================================================================================================
    {
        // T10: RSHIFT bit0 (=1) held, nothing else -- attack_unit fires with select_weapon's result.
        setup_common(fx);
        make_unit(fx, 7, /*proto*/ 1, NON_CARGO_TYPE);
        seed_group0(fx, {7});
        fx.key_rshift_held = 1;

        detail::group_issue_attack_order(fx.view(), g_calls, ATK_PARAM1, TARGET_PLAYER,
                                         /*selector*/ 0x80);

        ck_eq((uint32_t)g_attack_unit_calls.size(), 1u,
              "T10: RSHIFT bit0 held -- order_attack_unit fires, 0x004449cb/0x00444a27");
        ck_eq(g_attack_unit_calls[0].weapon_idx, (uint32_t)WEAPON_DEFAULT,
              "T10: attack_unit's weapon is select_weapon's FORWARDED result, not the constant 4");
        ck_eq((uint32_t)g_attack_target_calls.size(), 0u, "T10: attack_target NOT called");
    }
    {
        // T11: RSHIFT byte=2 (bit1 only) -- NOT held (RSHIFT tests bit0 ONLY) -- falls through to
        // attack_target with the CONSTANT weapon 4.
        setup_common(fx);
        make_unit(fx, 7, /*proto*/ 1, NON_CARGO_TYPE);
        seed_group0(fx, {7});
        fx.key_rshift_held = 2;

        detail::group_issue_attack_order(fx.view(), g_calls, ATK_PARAM1, TARGET_PLAYER,
                                         /*selector*/ 0x80);

        ck_eq((uint32_t)g_attack_unit_calls.size(), 0u,
              "T11: RSHIFT=2 (bit1 only) -- NOT held, attack_unit NOT fired, 0x004449cb tests bit0 "
              "ONLY (unlike LCTRL/LALT)");
        ck_eq((uint32_t)g_attack_target_calls.size(), 1u, "T11: falls through to attack_target");
        ck_eq(g_attack_target_calls[0].weapon_idx, CONST_WEAPON,
              "T11: attack_target's weapon is the CONSTANT 4, not select_weapon's result, 0x00444a63");
    }
    {
        // T12: LSHIFT bit0 (=1) held -- attack_unit fires.
        setup_common(fx);
        make_unit(fx, 7, /*proto*/ 1, NON_CARGO_TYPE);
        seed_group0(fx, {7});
        fx.key_lshift_held = 1;

        detail::group_issue_attack_order(fx.view(), g_calls, ATK_PARAM1, TARGET_PLAYER,
                                         /*selector*/ 0x80);

        ck_eq((uint32_t)g_attack_unit_calls.size(), 1u,
              "T12: LSHIFT bit0 held -- order_attack_unit fires, 0x004449d4");
    }
    {
        // T13: LSHIFT byte=2 (bit1 only) -- NOT held -- falls through to attack_target (const weapon 4).
        setup_common(fx);
        make_unit(fx, 7, /*proto*/ 1, NON_CARGO_TYPE);
        seed_group0(fx, {7});
        fx.key_lshift_held = 2;

        detail::group_issue_attack_order(fx.view(), g_calls, ATK_PARAM1, TARGET_PLAYER,
                                         /*selector*/ 0x80);

        ck_eq((uint32_t)g_attack_unit_calls.size(), 0u,
              "T13: LSHIFT=2 (bit1 only) -- NOT held, LSHIFT tests bit0 ONLY, 0x004449d4");
        ck_eq((uint32_t)g_attack_target_calls.size(), 1u, "T13: falls through to attack_target");
        ck_eq(g_attack_target_calls[0].weapon_idx, CONST_WEAPON, "T13: weapon is the constant 4");
    }
    {
        // T14 -- THE SHARPEST FACT: shift held (LSHIFT=1) AND LCTRL byte=2 (bit1 ONLY). LCTRL tests
        // BOTH bits, so this reads as HELD and suppresses attack_unit even though the byte is not 1.
        setup_common(fx);
        make_unit(fx, 7, /*proto*/ 1, NON_CARGO_TYPE);
        seed_group0(fx, {7});
        fx.key_lshift_held = 1;
        fx.key_lctrl_held  = 2;

        detail::group_issue_attack_order(fx.view(), g_calls, ATK_PARAM1, TARGET_PLAYER,
                                         /*selector*/ 0x80);

        ck_eq((uint32_t)g_attack_unit_calls.size(), 0u,
              "T14: shift held + LCTRL=2 (bit1 only) reads as HELD -- attack_unit suppressed, "
              "0x004449e6 tests bit 0x2 (unlike RSHIFT/LSHIFT, which never test it)");
        ck_eq((uint32_t)g_attack_target_calls.size(), 1u,
              "T14: falls to the LALT check (LALT=0 here) -> attack_target, const weapon 4");
        ck_eq(g_attack_target_calls[0].weapon_idx, CONST_WEAPON, "T14: weapon is the constant 4");
    }
    {
        // T15: shift held + LCTRL bit0 (=1) -- also reads HELD (symmetric confirmation of the LCTRL
        // TEST byte,0x1 half at 0x004449dd).
        setup_common(fx);
        make_unit(fx, 7, /*proto*/ 1, NON_CARGO_TYPE);
        seed_group0(fx, {7});
        fx.key_lshift_held = 1;
        fx.key_lctrl_held  = 1;

        detail::group_issue_attack_order(fx.view(), g_calls, ATK_PARAM1, TARGET_PLAYER,
                                         /*selector*/ 0x80);

        ck_eq((uint32_t)g_attack_unit_calls.size(), 0u,
              "T15: shift held + LCTRL=1 (bit0) also reads HELD -- attack_unit suppressed, 0x004449dd");
        ck_eq((uint32_t)g_attack_target_calls.size(), 1u, "T15: falls through to attack_target");
    }
    {
        // T16: LALT bit0 (=1) held, shift NOT pressed -- attack_target_alt fires with the CONSTANT
        // weapon 4 (not select_weapon's result).
        setup_common(fx);
        make_unit(fx, 7, /*proto*/ 1, NON_CARGO_TYPE);
        seed_group0(fx, {7});
        fx.key_lalt_held = 1;

        detail::group_issue_attack_order(fx.view(), g_calls, ATK_PARAM1, TARGET_PLAYER,
                                         /*selector*/ 0x80);

        ck_eq((uint32_t)g_attack_target_alt_calls.size(), 1u,
              "T16: LALT bit0 held -- order_attack_target_alt fires, 0x00444a2e/0x00444a61");
        ck_eq(g_attack_target_alt_calls[0].weapon_idx, CONST_WEAPON,
              "T16: attack_target_alt's weapon is the CONSTANT 4, not select_weapon's result");
        ck_eq((uint32_t)g_attack_target_calls.size(), 0u, "T16: attack_target NOT called");
        ck_eq((uint32_t)g_attack_unit_calls.size(), 0u, "T16: attack_unit NOT called");
    }
    {
        // T17: LALT byte=2 (bit1 only) -- ALSO reads HELD (LALT tests both bits, like LCTRL) --
        // attack_target_alt fires.
        setup_common(fx);
        make_unit(fx, 7, /*proto*/ 1, NON_CARGO_TYPE);
        seed_group0(fx, {7});
        fx.key_lalt_held = 2;

        detail::group_issue_attack_order(fx.view(), g_calls, ATK_PARAM1, TARGET_PLAYER,
                                         /*selector*/ 0x80);

        ck_eq((uint32_t)g_attack_target_alt_calls.size(), 1u,
              "T17: LALT=2 (bit1 only) reads HELD too -- attack_target_alt fires, 0x00444a37 tests "
              "bit 0x2");
    }
    {
        // T18: no keys held at all -- the plain else-else: order_attack_target with the constant 4.
        setup_common(fx);
        make_unit(fx, 7, /*proto*/ 1, NON_CARGO_TYPE);
        seed_group0(fx, {7});

        detail::group_issue_attack_order(fx.view(), g_calls, ATK_PARAM1, TARGET_PLAYER,
                                         /*selector*/ 0x80);

        ck_eq((uint32_t)g_attack_target_calls.size(), 1u,
              "T18: no keys held -- order_attack_target fires, 0x00444a63-0x00444a7f");
        ck_eq(g_attack_target_calls[0].weapon_idx, CONST_WEAPON, "T18: weapon is the constant 4");
        ck_eq((uint32_t)g_attack_target_alt_calls.size(), 0u, "T18: attack_target_alt NOT called");
        ck_eq((uint32_t)g_attack_unit_calls.size(), 0u, "T18: attack_unit NOT called");
    }

    // =================================================================================================
    // T19 -- selector==0x80 arm, the cargo/0x64/self-skip gates COMBINED in one loop: cargo-heli skips
    // before select_weapon is even called; a 0x64 select_weapon result skips after the call; the
    // param_1-matching unit skips via the self-exclude; and the one surviving unit issues via
    // attack_unit (RSHIFT held). Proves all three gates apply inside this arm specifically, not only
    // the 0x20/0x40 arms exercised above.
    // =================================================================================================
    {
        setup_common(fx);
        make_unit(fx, 1, /*proto*/ 1, UNIT_TYPE_A_HELI_CARGO);           // cargo -- pre-select_weapon skip
        make_unit(fx, 2, /*proto*/ 2, NON_CARGO_TYPE);                   // 0x64 -- select_weapon skip
        make_unit(fx, (int32_t)ATK_PARAM1, /*proto*/ 3, NON_CARGO_TYPE); // self-skip
        make_unit(fx, 4, /*proto*/ 4, NON_CARGO_TYPE);                   // issues via attack_unit
        seed_group0(fx, {1, 2, (uint16_t)ATK_PARAM1, 4});
        g_weapon_return_by_unit[2] = 0x64;
        fx.key_rshift_held         = 1;

        detail::group_issue_attack_order(fx.view(), g_calls, ATK_PARAM1, /*player*/ PLAYER_SIDE,
                                         /*selector*/ 0x80);

        ck_eq((uint32_t)g_select_weapon_calls.size(), 3u,
              "T19: select_weapon called for units 2, param_1-match, and 4 -- NOT the cargo unit");
        ck_eq((uint32_t)g_attack_unit_calls.size(), 1u,
              "T19: only unit 4 issues, via attack_unit (RSHIFT held)");
        ck_eq((uint32_t)g_attack_unit_calls[0].unit_idx, 4u, "T19: the issuing unit is unit 4");
        ck_eq(g_attack_unit_calls[0].weapon_idx, (uint32_t)WEAPON_DEFAULT,
              "T19: weapon is select_weapon's forwarded result");
        ck_eq((uint32_t)g_attack_target_calls.size(), 0u, "T19: attack_target NOT called");
        ck_eq((uint32_t)g_attack_target_alt_calls.size(), 0u, "T19: attack_target_alt NOT called");
        ck_eq((uint32_t)g_race_alert_calls, 1u, "T19: race_alert_sound_emit fires once");
    }

    // =====================================================================================================
    // ---- llm_strat_group_issue_enter_building_order @0x004451d2 -------------------------------------------
    // =====================================================================================================

    // =================================================================================================
    // T20 -- THE BOARDING GATE, both cargo-heli constants, combined with a non-cargo control unit that
    // proves the gate is an OR (not a plain AND): "not cargo-heli OR (cargo-heli AND not boarding)".
    // Five units, group order A,B,C,D,E:
    //   A: A_HELI_CARGO, boarding -> SKIP
    //   B: H_HELI_CARGO, boarding -> SKIP
    //   C: A_HELI_CARGO, NOT boarding -> issue
    //   D: H_HELI_CARGO, NOT boarding -> issue
    //   E: non-cargo, is_boarding mocked to return NONZERO (poison) -> issues anyway AND
    //      llm_unit_state_is_boarding must NEVER be called for E at all.
    // =================================================================================================
    {
        setup_common(fx);
        make_unit(fx, 1, /*proto*/ 1, UNIT_TYPE_A_HELI_CARGO, /*state*/ 101);
        make_unit(fx, 2, /*proto*/ 2, UNIT_TYPE_H_HELI_CARGO, /*state*/ 102);
        make_unit(fx, 3, /*proto*/ 3, UNIT_TYPE_A_HELI_CARGO, /*state*/ 103);
        make_unit(fx, 4, /*proto*/ 4, UNIT_TYPE_H_HELI_CARGO, /*state*/ 104);
        make_unit(fx, 5, /*proto*/ 5, NON_CARGO_TYPE, /*state*/ 105);
        seed_group0(fx, {1, 2, 3, 4, 5});
        g_is_boarding_result_by_state[101] = 1; // boarding
        g_is_boarding_result_by_state[102] = 1; // boarding
        g_is_boarding_result_by_state[103] = 0; // not boarding
        g_is_boarding_result_by_state[104] = 0; // not boarding
        g_is_boarding_result_by_state[105] = 1; // poison -- must never be read (non-cargo unit E)

        detail::group_issue_enter_building_order(fx.view(), g_calls, EB_PARAM1, EB_PARAM2, EB_PARAM3);

        ck(g_is_boarding_calls.size() == 4 && g_is_boarding_calls[0] == 101 &&
               g_is_boarding_calls[1] == 102 && g_is_boarding_calls[2] == 103 &&
               g_is_boarding_calls[3] == 104,
           "T20: llm_unit_state_is_boarding called exactly for the four cargo-heli units (states "
           "101,102,103,104), 0x004452ad -- NEVER for the non-cargo unit E");
        ck_eq((uint32_t)g_exit_storage_calls.size(), 3u,
              "T20: units C(3), D(4), E(5) issue -- A and B skip on boarding, 0x004452b8/0x004452bd");
        ck(g_exit_storage_calls[0].unit_idx == 3 && g_exit_storage_calls[1].unit_idx == 4 &&
               g_exit_storage_calls[2].unit_idx == 5,
           "T20: the issuing order is exactly {3,4,5} in loop order");
        for (const auto &c : g_exit_storage_calls) {
            ck(c.player == PLAYER_SIDE && c.a2 == (int32_t)EB_PARAM1 && c.param_4 == EB_PARAM2 &&
                   c.param_5 == EB_PARAM3,
               "T20: order_exit_storage(PlayerSide, unit_id, param_1, param_2, param_3) -- positional "
               "register mapping EAX/EDX/EBX/ECX/stack, 0x004452bd-0x004452da");
        }
        ck_eq((uint32_t)g_ack_voice_calls, 1u,
              "T20: group_order_ack_voice fires once (any_issued true), 0x004452f1");
    }

    // =================================================================================================
    // T21 -- TRAILING EMIT, "only the LAST unit issues": units 1 and 2 are cargo-heli and boarding
    // (skip), unit 3 is cargo-heli and NOT boarding (issues) -- ack_voice still fires.
    // =================================================================================================
    {
        setup_common(fx);
        make_unit(fx, 1, /*proto*/ 1, UNIT_TYPE_A_HELI_CARGO, /*state*/ 201);
        make_unit(fx, 2, /*proto*/ 2, UNIT_TYPE_H_HELI_CARGO, /*state*/ 202);
        make_unit(fx, 3, /*proto*/ 3, UNIT_TYPE_A_HELI_CARGO, /*state*/ 203);
        seed_group0(fx, {1, 2, 3});
        g_is_boarding_result_by_state[201] = 1;
        g_is_boarding_result_by_state[202] = 1;
        g_is_boarding_result_by_state[203] = 0;

        detail::group_issue_enter_building_order(fx.view(), g_calls, EB_PARAM1, EB_PARAM2, EB_PARAM3);

        ck_eq((uint32_t)g_exit_storage_calls.size(), 1u, "T21: only unit 3 (the last) issues");
        ck_eq((uint32_t)g_exit_storage_calls[0].unit_idx, 3u, "T21: the issuing unit is unit 3");
        ck_eq((uint32_t)g_ack_voice_calls, 1u,
              "T21: group_order_ack_voice fires -- any_issued is loop-wide (@[EBP-0x10]), not the "
              "last iteration's local");
    }

    // =================================================================================================
    // T22 -- TRAILING EMIT, the opposite direction: every unit skips (all boarding) -> no order at
    // all, and group_order_ack_voice is NOT called.
    // =================================================================================================
    {
        setup_common(fx);
        make_unit(fx, 1, /*proto*/ 1, UNIT_TYPE_A_HELI_CARGO, /*state*/ 301);
        make_unit(fx, 2, /*proto*/ 2, UNIT_TYPE_H_HELI_CARGO, /*state*/ 302);
        seed_group0(fx, {1, 2});
        g_is_boarding_result_by_state[301] = 1;
        g_is_boarding_result_by_state[302] = 1;

        detail::group_issue_enter_building_order(fx.view(), g_calls, EB_PARAM1, EB_PARAM2, EB_PARAM3);

        ck_eq((uint32_t)g_is_boarding_calls.size(), 2u, "T22: is_boarding called for both cargo units");
        ck_eq((uint32_t)g_exit_storage_calls.size(), 0u, "T22: no order issued -- both boarding");
        ck_eq((uint32_t)g_ack_voice_calls, 0u,
              "T22: group_order_ack_voice NOT called when any_issued stays false, 0x004452ef");
    }

    // =================================================================================================
    // T23 -- THE GROUP INDEX (enter_building_order): group[0] empty, group[1] holds a would-issue
    // non-cargo unit -- a wrong-index translation fails this.
    // =================================================================================================
    {
        setup_common(fx);
        make_unit(fx, 9, /*proto*/ 1, NON_CARGO_TYPE);
        seed_group0(fx, {});
        seed_group1(fx, {9});

        detail::group_issue_enter_building_order(fx.view(), g_calls, EB_PARAM1, EB_PARAM2, EB_PARAM3);

        ck_eq((uint32_t)g_is_boarding_calls.size(), 0u, "T23: group[0] empty -- loop body never runs");
        ck_eq((uint32_t)g_exit_storage_calls.size(), 0u, "T23: no order issued from group[1]'s content");
        ck_eq((uint32_t)g_ack_voice_calls, 0u, "T23: no ack_voice -- group[0].count==0 read, not [1]");
    }
}

} // namespace mh::sim::test
