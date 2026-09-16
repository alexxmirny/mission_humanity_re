//
// sim_dispatch_bldg2_selftest.cpp -- `simtest` cases for BUILDING-order arms 14..25 (the departure,
// cargo-transfer and construction half of table A). Arms 1..13 and the default arm are in
// sim_dispatch_bldg_selftest.cpp; the spine is in sim_dispatch_selftest.cpp.
//
// Split purely by size: 26 arms is more than one file wants, and two files is what let the two
// halves be written at once. There is no behavioural boundary at 14 -- read the sibling file for the
// shared helpers and idioms, which are deliberately duplicated rather than shared, so that a change
// to one half's setup cannot silently alter the other half's expectations.
//
#include "sim/sim_order_dispatch.h"

#include "sim_dispatch_calls.gen.h"
#include "sim_test_support.h"

#include <limits>
#include <vector>

namespace mh::sim::test {
namespace {

using namespace mh::sim;
using rec::dc;

constexpr int32_t PLAYER = 3;
constexpr int32_t BLDG   = 5;

// `kind` is ALWAYS 0x40 on this path -- the router reaches dispatch_building_order for that nibble
// and no other.
dispatch_ctx ctx_for(int32_t slot = 0) {
    dispatch_ctx x;
    x.slot         = slot;
    x.player       = (uint32_t)PLAYER;
    x.kind         = ORDER_KIND_BLDG;
    x.object_index = BLDG;
    return x;
}

void put_order(sim_fixture &f, int32_t slot, int16_t param0) {
    order q{};
    q.owner_and_kind = (uint16_t)(ORDER_KIND_BLDG | (uint32_t)PLAYER);
    q.unit_index     = (uint16_t)BLDG;
    q.param0         = param0;
    for (int32_t &a : q.args) a = -1;
    f.order_queue[slot] = q;
}

void seed_live_building(sim_fixture &f) {
    f.b(PLAYER, BLDG)              = building{};
    f.b(PLAYER, BLDG).energy       = 1.0;
    f.b(PLAYER, BLDG).online_state = 1;
}

// ---- constants this half of the table needs, duplicated from sim_order_dispatch_bldg.cpp's ------
// anonymous namespace (not visible from this TU -- same reason the .cpp itself gives for
// duplicating BLDG_TYPE_* there instead of exporting them through sim_order_dispatch.h).

constexpr int32_t BUILDING_ID_CFG = 9; // a distinct, nonzero cfg Building[] slot
constexpr int32_t SESSION_MODE_SP = 1; // _G_LLM_GAME_SESSION_MODE == 1 is single-player, EXACTLY

// Building[].type values the transfer arms compare against. A_/H_ pairs are the +0x14 race offset
// (the strategic-sim notes); values read off the CMPs in sim_order_dispatch_bldg.cpp's own transcription.
constexpr uint8_t BT_NONE      = 0x00; // e.g. a garage -- never a transfer terminal
constexpr uint8_t BT_A_MOTHER  = 0x06;
constexpr uint8_t BT_A_PORT    = 0x0c;
constexpr uint8_t BT_A_SHUTTLE = 0x0d;
constexpr uint8_t BT_H_MOTHER  = 0x1a;
constexpr uint8_t BT_H_PORT    = 0x20;
constexpr uint8_t BT_H_SHUTTLE = 0x21;

// ---- index 14 (param0 0x89) @0x0046893b: start a research project -------------------------------

void test_start_research() {
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).energy = 0.0;
        sim_view  v              = f.view();
        sim_store own            = f.store();
        put_order(f, 0, 0x89);
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().events.size(), 0, "start research: energy == 0 -> nothing");
    }
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).online_state = 0;
        sim_view  v                    = f.view();
        sim_store own                  = f.store();
        put_order(f, 0, 0x89);
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().events.size(), 0, "start research: online_state == 0 -> nothing");
    }
    // The duplicate-request guard needs BOTH conditions -- same project id AND already in the
    // researching state. Either alone must still start a fresh request.
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).state                 = 0x89; // BLDG_STATE_RESEARCHING
        sim_view  v                             = f.view();
        sim_store own                           = f.store();
        own.lab_at(PLAYER, 0).active_project_id = 60;
        put_order(f, 0, 0x89);
        f.order_queue[0].args[7] = 60; // SAME project
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().events.size(), 0,
              "start research: same project + already researching -> nothing, not even TryStartProject");
    }
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).state                 = 0x11; // NOT researching
        sim_view  v                             = f.view();
        sim_store own                           = f.store();
        own.lab_at(PLAYER, 0).active_project_id = 60;
        put_order(f, 0, 0x89);
        f.order_queue[0].args[7] = 60; // same project, but the state doesn't match
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().count(rec::DC_game_TryStartProject), 1,
              "start research: same project but NOT already researching -> the guard does not fire");
    }
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).state                 = 0x89;
        sim_view  v                             = f.view();
        sim_store own                           = f.store();
        own.lab_at(PLAYER, 0).active_project_id = 60;
        put_order(f, 0, 0x89);
        f.order_queue[0].args[7] = 61; // researching, but a DIFFERENT project
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().count(rec::DC_game_TryStartProject), 1,
              "start research: already researching, but a DIFFERENT project -> not a duplicate");
    }
    // Happy path: TryStartProject succeeds (default ret 0).
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).cycle_progress = 0.75;
        sim_view  v                      = f.view();
        sim_store own                    = f.store();
        put_order(f, 0, 0x89);
        f.order_queue[0].args[7] = 42;
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().count(rec::DC_llm_bldg_finish_current_order), 1,
              "start research: finishes the current order");
        ck_eq((uint32_t)own.lab_at(PLAYER, 0).active_project_id, 42,
              "start research: the lab's active project becomes args[7]");
        ck_eq((uint32_t)f.b(PLAYER, BLDG).state, 0x89, "start research: state becomes RESEARCHING");
        ck_eq((uint32_t)f.b(PLAYER, BLDG).online_state, 2,
              "start research: online_state becomes 2 (the sub-phase counter)");
        ck_eq_d(f.b(PLAYER, BLDG).cycle_progress, 0.0, "start research: cycle progress is reset");
    }
    // Failure path: TryStartProject refuses, and the message is gated on being the LOCAL player.
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).building_id       = BUILDING_ID_CFG;
        f.cfg_buildings[BUILDING_ID_CFG].id = 200; // the building's own name text id
        const wchar_t *NAME_PTR             = (const wchar_t *)0x00abc100;
        const wchar_t *REASON_PTR           = (const wchar_t *)0x00abc200;
        f.text_ptrs[200]                    = NAME_PTR;
        f.text_ptrs[70]                     = REASON_PTR;
        f.player_side                       = (int16_t)PLAYER; // local
        sim_view  v                         = f.view();
        sim_store own                       = f.store();
        put_order(f, 0, 0x89);
        f.order_queue[0].args[7] = 42;
        dc().reset();
        dc().ret[rec::DC_game_TryStartProject] = 70; // rc IS the reason text id
        dispatch_calls c                       = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().count(rec::DC_llm_bldg_finish_current_order), 0,
              "start research: a refusal never finishes the order");
        const rec::dc_event *msg = dc().last(rec::DC_w_sprintf__vss);
        ck(msg != nullptr, "start research: local player + refusal -> the message is formatted");
        ck(msg != nullptr && msg->a[2] == (long long)(intptr_t)NAME_PTR,
           "start research: the building's own name is the first %s");
        ck(msg != nullptr && msg->a[3] == (long long)(intptr_t)REASON_PTR,
           "start research: the rc IS the reason text id, the second %s");
        ck_eq((uint32_t)dc().count(rec::DC_game_ui_PrintTextMessage), 1, "start research: and it is shown");
        ck(dc().first_index(rec::DC_w_sprintf__vss) < dc().first_index(rec::DC_game_ui_PrintTextMessage),
           "start research: formatted BEFORE it is shown");
    }
    {
        sim_fixture f;
        seed_live_building(f);
        // f.player_side stays 7 (the fixture default) -- NOT the local player.
        sim_view  v   = f.view();
        sim_store own = f.store();
        put_order(f, 0, 0x89);
        f.order_queue[0].args[7] = 42;
        dc().reset();
        dc().ret[rec::DC_game_TryStartProject] = 70;
        dispatch_calls c                       = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().count(rec::DC_game_TryStartProject), 1, "start research: the attempt still happens");
        ck_eq((uint32_t)dc().events.size(), 1,
              "start research: but a non-local player's refusal prints NOTHING");
    }
}

// ---- index 15 (param0 0xd2) @0x00467fd9: launch a SHUTTLE ----------------------------------------

void test_shuttle_depart() {
    // Fuel gate #1, from the ACTUAL guard (0x0046800d: `CMP fuel[0].id,0`, feeding a
    // continue-condition of `fuel[0].id != 0 || args[6] == planet_index`, confirmed against
    // Ghidra's own decompile of this exact site): a shuttle with NO fuel type CONFIGURED
    // (fuel[0].id == 0) is the one that is RESTRICTED to its current planet -- a configured fuel
    // type is what a shuttle needs before it may even ATTEMPT to leave. On-planet travel is
    // unaffected either way, since the gate's second operand is false whenever args[6] ==
    // planet_index.
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).building_id               = BUILDING_ID_CFG;
        f.b(PLAYER, BLDG).shuttle_slot              = 6;
        f.cfg_buildings[BUILDING_ID_CFG].type       = BT_A_SHUTTLE;
        f.cfg_buildings[BUILDING_ID_CFG].fuel[0].id = 0; // NO fuel type configured
        f.session_mode                              = SESSION_MODE_SP;
        f.planet_index                              = 4;
        sim_view  v                                 = f.view();
        sim_store own                               = f.store();
        put_order(f, 0, 0xd2);
        f.order_queue[0].args[6] = 4; // ON-planet -- unaffected by fuel[0].id either way
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck((dc().count(rec::DC_llm_prod_shuttle_fuel_check) == 1),
           "shuttle depart: fuel[0].id == 0 does not block ON-planet travel");
    }
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).building_id               = BUILDING_ID_CFG;
        f.cfg_buildings[BUILDING_ID_CFG].type       = BT_A_SHUTTLE;
        f.cfg_buildings[BUILDING_ID_CFG].fuel[0].id = 0; // NO fuel type configured
        f.session_mode                              = SESSION_MODE_SP;
        f.planet_index                              = 4;
        sim_view  v                                 = f.view();
        sim_store own                               = f.store();
        put_order(f, 0, 0xd2);
        f.order_queue[0].args[6] = 99; // OFF-planet
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().events.size(), 0,
              "shuttle depart: fuel[0].id == 0 -> refused off-planet, EVEN in single player -- a "
              "translation of the .cpp's own prose comment (\"may fly anywhere\") backwards would "
              "pass here; the assembly (and this case) says the opposite");
    }
    // A configured fuel type clears gate #1, but gate #2 (session_mode) still applies to it
    // independently: off-planet + a fuel type + NOT single-player is still refused.
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).building_id               = BUILDING_ID_CFG;
        f.cfg_buildings[BUILDING_ID_CFG].type       = BT_A_SHUTTLE;
        f.cfg_buildings[BUILDING_ID_CFG].fuel[0].id = 7; // a fuel type IS configured
        f.session_mode                              = 3;
        f.planet_index                              = 4;
        sim_view  v                                 = f.view();
        sim_store own                               = f.store();
        put_order(f, 0, 0xd2);
        f.order_queue[0].args[6] = 99; // off-planet
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().events.size(), 0,
              "shuttle depart: fuel[0].id != 0 clears gate #1, but session_mode != 1 still blocks "
              "off-planet travel at gate #2");
    }
    // Gate #2: session_mode != SP blocks off-planet travel, for BOTH non-SP values (settled fact:
    // gated on == 1 exactly, never != 3).
    //
    // THIS LOOP WAS VACUOUS UNTIL 2026-08-08 and its own comment said why without noticing: it set
    // `fuel[0].id = 0` "to bypass gate #1", which is backwards -- id == 0 is what MAKES gate #1
    // refuse an off-planet destination, so the arm returned one line earlier and the "no calls"
    // assertion was satisfied by the wrong gate. The mutation campaign caught it: weakening gate #2
    // from `== 1` to `!= 3` (which differs at mode 2 and nowhere else) SURVIVED. Isolating gate #2
    // means CLEARING gate #1, i.e. a fuel type configured.
    for (int32_t mode : {2, 3}) {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).building_id               = BUILDING_ID_CFG;
        f.cfg_buildings[BUILDING_ID_CFG].type       = BT_A_SHUTTLE;
        f.cfg_buildings[BUILDING_ID_CFG].fuel[0].id = 7; // CLEARS gate #1, so only gate #2 can refuse
        f.session_mode                              = mode;
        f.planet_index                              = 4;
        sim_view  v                                 = f.view();
        sim_store own                               = f.store();
        put_order(f, 0, 0xd2);
        f.order_queue[0].args[6] = 99;
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().events.size(), 0, "shuttle depart: off-planet + session_mode != 1 -> nothing");
    }
    // Type gate.
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).building_id         = BUILDING_ID_CFG;
        f.cfg_buildings[BUILDING_ID_CFG].type = BT_A_PORT; // not a shuttle
        f.session_mode                        = SESSION_MODE_SP;
        f.planet_index                        = 4;
        sim_view  v                           = f.view();
        sim_store own                         = f.store();
        put_order(f, 0, 0xd2);
        f.order_queue[0].args[6] = 4;
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().events.size(), 0, "shuttle depart: not a shuttle type -> nothing");
    }
    // Damaged: below cfg energy -> the fixed INSUFFICIENT_ENERGY message, no fuel check at all.
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).building_id           = BUILDING_ID_CFG;
        f.b(PLAYER, BLDG).energy                = 0.4;
        f.cfg_buildings[BUILDING_ID_CFG].type   = BT_H_SHUTTLE;
        f.cfg_buildings[BUILDING_ID_CFG].energy = 1.0;
        f.cfg_buildings[BUILDING_ID_CFG].id     = 55;
        f.player_side                           = (int16_t)PLAYER;
        f.session_mode                          = SESSION_MODE_SP;
        f.planet_index                          = 4;
        const wchar_t *NAME_PTR                 = (const wchar_t *)0x00abd100;
        const wchar_t *REASON_PTR               = (const wchar_t *)0x00abd200;
        f.text_ptrs[55]                         = NAME_PTR;
        f.text_ptrs[136]                        = REASON_PTR; // G_TEXT_PTRS index 136: TEXT_ID_INSUFFICIENT_ENERGY
        sim_view  v                             = f.view();
        sim_store own                           = f.store();
        put_order(f, 0, 0xd2);
        f.order_queue[0].args[6] = 4;
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().count(rec::DC_llm_prod_shuttle_fuel_check), 0,
              "shuttle depart: a damaged shuttle never reaches the fuel check");
        const rec::dc_event *msg = dc().last(rec::DC_w_sprintf__vss);
        ck(msg != nullptr && msg->a[3] == (long long)(intptr_t)REASON_PTR,
           "shuttle depart: the reason is the FIXED insufficient-energy text (136), not a callee status");
    }
    // The "double NaN" interaction: NaN energy clears the outer energy_gate (JNC) but ALSO
    // satisfies fcom_below_or_unordered against cfg energy, so the net effect is the damaged
    // branch, never the fuel check.
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).building_id           = BUILDING_ID_CFG;
        f.b(PLAYER, BLDG).energy                = std::numeric_limits<double>::quiet_NaN();
        f.cfg_buildings[BUILDING_ID_CFG].type   = BT_A_SHUTTLE;
        f.cfg_buildings[BUILDING_ID_CFG].energy = 1.0;
        f.session_mode                          = SESSION_MODE_SP;
        f.planet_index                          = 4;
        sim_view  v                             = f.view();
        sim_store own                           = f.store();
        put_order(f, 0, 0xd2);
        f.order_queue[0].args[6] = 4;
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().count(rec::DC_llm_prod_shuttle_fuel_check), 0,
              "shuttle depart: NaN energy passes the outer gate but ALSO reads as damaged -- never "
              "reaches the fuel check");
    }
    // Fuel check failure: prints NOTHING (unlike index 16/17) and stops immediately.
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).building_id         = BUILDING_ID_CFG;
        f.b(PLAYER, BLDG).shuttle_slot        = 6;
        f.cfg_buildings[BUILDING_ID_CFG].type = BT_A_SHUTTLE;
        f.player_side                         = (int16_t)PLAYER;
        f.session_mode                        = SESSION_MODE_SP;
        f.planet_index                        = 4;
        sim_view  v                           = f.view();
        sim_store own                         = f.store();
        put_order(f, 0, 0xd2);
        f.order_queue[0].args[6] = 4;
        dc().reset();
        dc().ret[rec::DC_llm_prod_shuttle_fuel_check] = 5;
        dispatch_calls c                              = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().events.size(), 1,
              "shuttle depart: a fuel-check failure prints NOTHING -- the only event is the check itself");
    }
    // Slot unbound: bind is attempted, and since the stub does not actually bind it, the arm stops
    // right after -- but fuel_apply STILL ran, because it happens BEFORE the slot test.
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).building_id         = BUILDING_ID_CFG;
        f.cfg_buildings[BUILDING_ID_CFG].type = BT_A_SHUTTLE;
        f.session_mode                        = SESSION_MODE_SP;
        f.planet_index                        = 4;
        sim_view  v                           = f.view();
        sim_store own                         = f.store();
        put_order(f, 0, 0xd2);
        f.order_queue[0].args[6] = 4;
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().count(rec::DC_llm_prod_shuttle_fuel_apply), 1,
              "shuttle depart: fuel is applied BEFORE the slot test, even though the slot never binds");
        ck_eq((uint32_t)dc().count(rec::DC_llm_prod_shuttle_slot_bind_default), 1,
              "shuttle depart: a bind is attempted when unbound");
        ck_eq((uint32_t)dc().count(rec::DC_llm_prod_shuttle_depart), 0,
              "shuttle depart: and it never departs, since the slot is still unbound");
    }
    // Happy path: bound slot, on-planet (skips bind_planet), and the ORDER of the tail two calls.
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).building_id         = BUILDING_ID_CFG;
        f.b(PLAYER, BLDG).shuttle_slot        = 6;
        f.cfg_buildings[BUILDING_ID_CFG].type = BT_A_SHUTTLE;
        f.session_mode                        = SESSION_MODE_SP;
        f.planet_index                        = 4;
        sim_view  v                           = f.view();
        sim_store own                         = f.store();
        put_order(f, 0, 0xd2);
        f.order_queue[0].args[6] = 4; // ON-planet: skips llm_strat_prod_bind_planet entirely
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().count(rec::DC_llm_strat_prod_bind_planet), 0,
              "shuttle depart: on-planet travel never calls bind_planet");
        ck_eq((uint32_t)dc().count(rec::DC_llm_prod_shuttle_depart), 1, "shuttle depart: departs");
        ck_eq((uint32_t)dc().count(rec::DC_llm_bldg_transfer_notify_noop), 1,
              "shuttle depart: and notifies -- UNIQUE to index 15's happy path (16/17 do not)");
        ck(dc().first_index(rec::DC_llm_prod_shuttle_depart) <
               dc().first_index(rec::DC_llm_bldg_transfer_notify_noop),
           "shuttle depart: departs BEFORE it notifies");
    }
    // Off-planet + bind_planet refuses -> never departs.
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).building_id               = BUILDING_ID_CFG;
        f.b(PLAYER, BLDG).shuttle_slot              = 6;
        f.cfg_buildings[BUILDING_ID_CFG].type       = BT_A_SHUTTLE;
        f.cfg_buildings[BUILDING_ID_CFG].fuel[0].id = 7; // a fuel type: clears gate #1 for off-planet
        f.session_mode                              = SESSION_MODE_SP;
        f.planet_index                              = 4;
        sim_view  v                                 = f.view();
        sim_store own                               = f.store();
        put_order(f, 0, 0xd2);
        f.order_queue[0].args[6] = 99; // off-planet, SP -> allowed past gate #2
        dc().reset();
        dc().ret[rec::DC_llm_strat_prod_bind_planet] = 0; // refuses
        dispatch_calls c                             = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().count(rec::DC_llm_strat_prod_bind_planet), 1,
              "shuttle depart: off-planet + bound slot -> tries to bind the new planet");
        ck_eq((uint32_t)dc().count(rec::DC_llm_prod_shuttle_depart), 0,
              "shuttle depart: a refused bind never departs");
    }
}

// ---- indices 16 (0xd3) and 17 (0xd4): the shared body -------------------------------------------

// Index 17's whole body is a DEAD CMP whose flags the first instruction of index 16 clobbers, so it
// falls straight through -- the SAME order sent as either opcode must produce the SAME calls. This
// is the one fact this whole file exists to catch if a translation ever gives 0xd4 its own body.
void test_mother_port_fallthrough_shared_body() {
    auto capture = [](int16_t param0) {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).building_id           = BUILDING_ID_CFG;
        f.b(PLAYER, BLDG).energy                = 2.0;
        f.b(PLAYER, BLDG).shuttle_slot          = 6;         // nonzero: skip the bind_default detour on both runs
        f.cfg_buildings[BUILDING_ID_CFG].type   = BT_A_PORT; // a port: no mother-only damage check
        f.cfg_buildings[BUILDING_ID_CFG].energy = 1.0;
        f.session_mode                          = SESSION_MODE_SP;
        f.planet_index                          = 4;
        sim_view  v                             = f.view();
        sim_store own                           = f.store();
        put_order(f, 0, param0);
        f.order_queue[0].args[6] = 4; // same planet: also skips the bind_planet detour

        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        return dc().events; // copy out before the NEXT run resets the shared log
    };

    const std::vector<rec::dc_event> e16 = capture(0xd3);
    const std::vector<rec::dc_event> e17 = capture(0xd4);

    ck((e16.size() > 0), "sanity: the shared body actually calls something on this input");
    ck_eq((uint32_t)e17.size(), (uint32_t)e16.size(),
          "0xd4 falls through into 0xd3's body: the SAME number of calls happen");
    for (size_t i = 0; i < e16.size() && i < e17.size(); ++i) {
        ck_eq((uint32_t)e17[i].fn, (uint32_t)e16[i].fn, "0xd4/0xd3: each call is the SAME callee, in order");
        ck_eq((uint32_t)e17[i].na, (uint32_t)e16[i].na, "0xd4/0xd3: same argument count");
        for (int j = 0; j < e16[i].na && j < 6; ++j)
            ck_eq((uint32_t)e17[i].a[j], (uint32_t)e16[i].a[j], "0xd4/0xd3: same argument value");
    }
}

// The rest of 16/17's behaviour: gates, the mother-only damage sub-check, and the tail that
// differs from index 15 (a different finalize callee, no transfer-notify noop).
void test_mother_port_depart_behavior() {
    for (int32_t mode : {2, 3}) {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).building_id         = BUILDING_ID_CFG;
        f.cfg_buildings[BUILDING_ID_CFG].type = BT_A_PORT;
        f.session_mode                        = mode;
        f.planet_index                        = 4;
        sim_view  v                           = f.view();
        sim_store own                         = f.store();
        put_order(f, 0, 0xd3);
        f.order_queue[0].args[6] = 99; // off-planet
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().events.size(), 0, "mother/port depart: off-planet + session_mode != 1 -> blocked");
    }
    // Same gate, ON-planet: the AND's second half is false, so it is NOT blocked even in mode 3.
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).building_id         = BUILDING_ID_CFG;
        f.b(PLAYER, BLDG).shuttle_slot        = 6;
        f.cfg_buildings[BUILDING_ID_CFG].type = BT_A_PORT;
        f.session_mode                        = 3;
        f.planet_index                        = 4;
        sim_view  v                           = f.view();
        sim_store own                         = f.store();
        put_order(f, 0, 0xd3);
        f.order_queue[0].args[6] = 4; // ON-planet
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().count(rec::DC_llm_prod_shuttle_fuel_check), 1,
              "mother/port depart: on-planet travel is allowed even in session_mode 3 -- the gate is "
              "a compound AND, not session_mode alone");
    }
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).building_id         = BUILDING_ID_CFG;
        f.cfg_buildings[BUILDING_ID_CFG].type = BT_NONE;
        f.session_mode                        = SESSION_MODE_SP;
        sim_view  v                           = f.view();
        sim_store own                         = f.store();
        put_order(f, 0, 0xd4);
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().events.size(), 0, "mother/port depart: not a port or mother -> nothing");
    }
    // The damage sub-check applies ONLY to a mother -- a port at the same low energy proceeds.
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).building_id           = BUILDING_ID_CFG;
        f.b(PLAYER, BLDG).energy                = 0.4;
        f.cfg_buildings[BUILDING_ID_CFG].type   = BT_A_MOTHER;
        f.cfg_buildings[BUILDING_ID_CFG].energy = 1.0;
        f.player_side                           = (int16_t)PLAYER;
        f.session_mode                          = SESSION_MODE_SP;
        sim_view  v                             = f.view();
        sim_store own                           = f.store();
        put_order(f, 0, 0xd4);
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().count(rec::DC_llm_prod_shuttle_fuel_check), 0,
              "mother/port depart: a damaged MOTHER never reaches the fuel check");
        ck_eq((uint32_t)dc().count(rec::DC_game_ui_PrintTextMessage), 1,
              "mother/port depart: and the local player is told why");
    }
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).building_id           = BUILDING_ID_CFG;
        f.b(PLAYER, BLDG).energy                = 0.4; // same low energy as the mother case above
        f.b(PLAYER, BLDG).shuttle_slot          = 6;
        f.cfg_buildings[BUILDING_ID_CFG].type   = BT_A_PORT; // a PORT, not a mother
        f.cfg_buildings[BUILDING_ID_CFG].energy = 1.0;
        f.session_mode                          = SESSION_MODE_SP;
        f.planet_index                          = 4;
        sim_view  v                             = f.view();
        sim_store own                           = f.store();
        put_order(f, 0, 0xd3);
        f.order_queue[0].args[6] = 4;
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().count(rec::DC_llm_prod_shuttle_fuel_check), 1,
              "mother/port depart: a damaged PORT is NOT gated by the mother-only damage check");
    }
    // Boundary: exactly AT the required energy is NOT damaged (>=, not >).
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).building_id           = BUILDING_ID_CFG;
        f.b(PLAYER, BLDG).energy                = 1.0;
        f.b(PLAYER, BLDG).shuttle_slot          = 6;
        f.cfg_buildings[BUILDING_ID_CFG].type   = BT_H_MOTHER;
        f.cfg_buildings[BUILDING_ID_CFG].energy = 1.0; // EQUAL
        f.session_mode                          = SESSION_MODE_SP;
        f.planet_index                          = 4;
        sim_view  v                             = f.view();
        sim_store own                           = f.store();
        put_order(f, 0, 0xd4);
        f.order_queue[0].args[6] = 4;
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().count(rec::DC_llm_prod_shuttle_fuel_check), 1,
              "mother/port depart: energy exactly equal to cfg energy is NOT damaged");
    }
    // The "double NaN" interaction, same shape as index 15's.
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).building_id           = BUILDING_ID_CFG;
        f.b(PLAYER, BLDG).energy                = std::numeric_limits<double>::quiet_NaN();
        f.cfg_buildings[BUILDING_ID_CFG].type   = BT_A_MOTHER;
        f.cfg_buildings[BUILDING_ID_CFG].energy = 1.0;
        f.session_mode                          = SESSION_MODE_SP;
        sim_view  v                             = f.view();
        sim_store own                           = f.store();
        put_order(f, 0, 0xd4);
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().count(rec::DC_llm_prod_shuttle_fuel_check), 0,
              "mother/port depart: NaN energy passes the outer gate but ALSO reads as damaged on a "
              "mother, so it still never reaches the fuel check");
    }
    // Fuel check failure IS reported here -- unlike index 15's silent refusal.
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).building_id         = BUILDING_ID_CFG;
        f.b(PLAYER, BLDG).shuttle_slot        = 6;
        f.cfg_buildings[BUILDING_ID_CFG].type = BT_A_PORT;
        f.player_side                         = (int16_t)PLAYER;
        f.session_mode                        = SESSION_MODE_SP;
        f.planet_index                        = 4;
        sim_view  v                           = f.view();
        sim_store own                         = f.store();
        put_order(f, 0, 0xd3);
        f.order_queue[0].args[6] = 4;
        dc().reset();
        dc().ret[rec::DC_llm_prod_shuttle_fuel_check] = 33; // a nonzero rc
        dispatch_calls c                              = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().count(rec::DC_game_ui_PrintTextMessage), 1,
              "mother/port depart: unlike index 15, a fuel-check failure IS reported");
        ck_eq((uint32_t)dc().count(rec::DC_llm_prod_shuttle_fuel_apply), 0,
              "mother/port depart: and the fuel is never applied");
    }
    // Happy path: a DIFFERENT finalize callee from index 15, and NO transfer-notify noop.
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).building_id         = BUILDING_ID_CFG;
        f.b(PLAYER, BLDG).shuttle_slot        = 6;
        f.cfg_buildings[BUILDING_ID_CFG].type = BT_H_PORT;
        f.session_mode                        = SESSION_MODE_SP;
        f.planet_index                        = 4;
        sim_view  v                           = f.view();
        sim_store own                         = f.store();
        put_order(f, 0, 0xd3);
        f.order_queue[0].args[6] = 4;
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().count(rec::DC_llm_prod_bldg_depart_finalize), 1,
              "mother/port depart: finalizes via llm_prod_bldg_depart_finalize, NOT index 15's "
              "llm_prod_shuttle_depart");
        ck_eq((uint32_t)dc().count(rec::DC_llm_prod_shuttle_depart), 0,
              "mother/port depart: index 15's callee never runs here");
        ck_eq((uint32_t)dc().count(rec::DC_llm_bldg_transfer_notify_noop), 0,
              "mother/port depart: and there is NO transfer-notify noop, unlike index 15's happy path");
    }
}

// ---- index 18 (param0 0xd6) @0x004678d3: claim a cargo slot ---------------------------------------

void test_cargo_bind_slot() {
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).building_id         = BUILDING_ID_CFG;
        f.b(PLAYER, BLDG).shuttle_slot        = 3; // already bound
        f.cfg_buildings[BUILDING_ID_CFG].type = BT_A_PORT;
        f.session_mode                        = SESSION_MODE_SP;
        sim_view  v                           = f.view();
        sim_store own                         = f.store();
        put_order(f, 0, 0xd6);
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().events.size(), 0,
              "cargo bind slot: shuttle_slot != 0 -> nothing (the INVERTED gate)");
    }
    // The canonical "NaN energy runs the body" case for this file: a single energy_gate, no
    // secondary FP compare downstream, so a NaN energy should reach the happy path cleanly.
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).energy              = std::numeric_limits<double>::quiet_NaN();
        f.b(PLAYER, BLDG).building_id         = BUILDING_ID_CFG;
        f.cfg_buildings[BUILDING_ID_CFG].type = BT_H_MOTHER;
        f.session_mode                        = SESSION_MODE_SP;
        sim_view  v                           = f.view();
        sim_store own                         = f.store();
        put_order(f, 0, 0xd6);
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().count(rec::DC_llm_prod_shuttle_slot_bind_default), 1,
              "cargo bind slot: a NaN energy RUNS the body (FLDZ/FCOMP/SAHF/JNC treats unordered as "
              "not-<=-0)");
    }
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).energy              = 0.0;
        f.b(PLAYER, BLDG).building_id         = BUILDING_ID_CFG;
        f.cfg_buildings[BUILDING_ID_CFG].type = BT_A_PORT;
        f.session_mode                        = SESSION_MODE_SP;
        sim_view  v                           = f.view();
        sim_store own                         = f.store();
        put_order(f, 0, 0xd6);
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().events.size(), 0, "cargo bind slot: energy == 0 exactly -> nothing");
    }
    // session_mode != SP -> nothing, UNCONDITIONALLY -- no off-planet compound here, unlike 15/16/17.
    for (int32_t mode : {2, 3}) {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).building_id         = BUILDING_ID_CFG;
        f.cfg_buildings[BUILDING_ID_CFG].type = BT_A_PORT;
        f.session_mode                        = mode;
        sim_view  v                           = f.view();
        sim_store own                         = f.store();
        put_order(f, 0, 0xd6);
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().events.size(), 0, "cargo bind slot: session_mode != 1 -> nothing (simple gate)");
    }
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).building_id         = BUILDING_ID_CFG;
        f.cfg_buildings[BUILDING_ID_CFG].type = BT_NONE;
        f.session_mode                        = SESSION_MODE_SP;
        sim_view  v                           = f.view();
        sim_store own                         = f.store();
        put_order(f, 0, 0xd6);
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().events.size(), 0, "cargo bind slot: not a port/mother -> nothing");
    }
    // Happy path: bind then notify, in that order, and nothing else.
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).building_id         = BUILDING_ID_CFG;
        f.cfg_buildings[BUILDING_ID_CFG].type = BT_H_PORT;
        f.session_mode                        = SESSION_MODE_SP;
        sim_view  v                           = f.view();
        sim_store own                         = f.store();
        put_order(f, 0, 0xd6);
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().count(rec::DC_llm_prod_shuttle_slot_bind_default), 1,
              "cargo bind slot: binds the default slot");
        ck_eq((uint32_t)dc().count(rec::DC_llm_bldg_transfer_notify_noop), 1,
              "cargo bind slot: notifies the transfer UI");
        ck_eq((uint32_t)dc().events.size(), 2, "cargo bind slot: and nothing else happens");
        ck(dc().first_index(rec::DC_llm_prod_shuttle_slot_bind_default) <
               dc().first_index(rec::DC_llm_bldg_transfer_notify_noop),
           "cargo bind slot: the bind happens BEFORE the notify");
    }
}

// ---- index 19 (param0 0xdb) @0x00467aff: load passengers ------------------------------------------

void test_load_passengers() {
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).energy = 0.0;
        sim_view  v              = f.view();
        sim_store own            = f.store();
        put_order(f, 0, 0xdb);
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().events.size(), 0, "load passengers: energy == 0 -> nothing");
    }
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).online_state = 0;
        sim_view  v                    = f.view();
        sim_store own                  = f.store();
        put_order(f, 0, 0xdb);
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().events.size(), 0, "load passengers: online_state == 0 -> nothing");
    }
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).building_id         = BUILDING_ID_CFG;
        f.cfg_buildings[BUILDING_ID_CFG].type = BT_A_PORT;
        f.session_mode                        = 3;
        sim_view  v                           = f.view();
        sim_store own                         = f.store();
        put_order(f, 0, 0xdb);
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().events.size(), 0, "load passengers: session_mode != 1 -> nothing");
    }
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).building_id         = BUILDING_ID_CFG;
        f.cfg_buildings[BUILDING_ID_CFG].type = BT_NONE;
        f.session_mode                        = SESSION_MODE_SP;
        sim_view  v                           = f.view();
        sim_store own                         = f.store();
        put_order(f, 0, 0xdb);
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().events.size(), 0, "load passengers: not a transfer terminal -> nothing");
    }
    // Unbound slot: bind attempted, and since the stub does not actually bind it the load call
    // never happens -- the same "bind, then re-check" shape as index 15/18.
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).building_id         = BUILDING_ID_CFG;
        f.cfg_buildings[BUILDING_ID_CFG].type = BT_H_MOTHER;
        f.session_mode                        = SESSION_MODE_SP;
        sim_view  v                           = f.view();
        sim_store own                         = f.store();
        put_order(f, 0, 0xdb);
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().count(rec::DC_llm_prod_shuttle_slot_bind_default), 1,
              "load passengers: attempts the default bind when unbound");
        ck_eq((uint32_t)dc().count(rec::DC_llm_prod_shuttle_load_passengers), 0,
              "load passengers: and does not load, since it is still unbound");
    }
    // Bound: the count is passed as ONLY the low 16 bits -- a nonzero high word catches a
    // translation that forwards the full dword.
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).building_id         = BUILDING_ID_CFG;
        f.b(PLAYER, BLDG).shuttle_slot        = 5;
        f.cfg_buildings[BUILDING_ID_CFG].type = BT_A_PORT;
        f.session_mode                        = SESSION_MODE_SP;
        sim_view  v                           = f.view();
        sim_store own                         = f.store();
        put_order(f, 0, 0xdb);
        f.order_queue[0].args[1] = 0x1234abcd;
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        const rec::dc_event *ev = dc().last(rec::DC_llm_prod_shuttle_load_passengers);
        ck(ev != nullptr, "load passengers: the callee ran");
        ck(ev != nullptr && ev->a[0] == PLAYER && ev->a[1] == BLDG,
           "load passengers: player and object index passed through");
        ck(ev != nullptr && ev->a[2] == 0xabcd,
           "load passengers: args[1] narrowed to its LOW 16 BITS (MOVZX), high word dropped");
    }
}

// ---- index 20 (param0 0xdc) @0x00467c40: unload passengers -----------------------------------------

void test_unload_passengers() {
    // shuttle_slot is checked BEFORE session_mode/type, and unlike index 19 there is NO
    // bind-on-demand at all: an unbound slot returns immediately with ZERO calls.
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).building_id         = BUILDING_ID_CFG;
        f.cfg_buildings[BUILDING_ID_CFG].type = BT_A_PORT;
        f.session_mode                        = SESSION_MODE_SP;
        sim_view  v                           = f.view();
        sim_store own                         = f.store();
        put_order(f, 0, 0xdc);
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().events.size(), 0,
              "unload passengers: shuttle_slot == 0 -> nothing at all, NO bind attempt (unlike index 19)");
    }
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).energy       = 0.0;
        f.b(PLAYER, BLDG).shuttle_slot = 4;
        sim_view  v                    = f.view();
        sim_store own                  = f.store();
        put_order(f, 0, 0xdc);
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().events.size(), 0, "unload passengers: energy == 0 -> nothing");
    }
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).shuttle_slot        = 4;
        f.b(PLAYER, BLDG).building_id         = BUILDING_ID_CFG;
        f.cfg_buildings[BUILDING_ID_CFG].type = BT_A_PORT;
        f.session_mode                        = 3;
        sim_view  v                           = f.view();
        sim_store own                         = f.store();
        put_order(f, 0, 0xdc);
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().events.size(), 0, "unload passengers: session_mode != 1 -> nothing");
    }
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).shuttle_slot        = 4;
        f.b(PLAYER, BLDG).building_id         = BUILDING_ID_CFG;
        f.cfg_buildings[BUILDING_ID_CFG].type = BT_NONE;
        f.session_mode                        = SESSION_MODE_SP;
        sim_view  v                           = f.view();
        sim_store own                         = f.store();
        put_order(f, 0, 0xdc);
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().events.size(), 0, "unload passengers: not a transfer terminal -> nothing");
    }
    // Happy path: the FULL dword, not the MOVZX index 19 uses.
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).shuttle_slot        = 4;
        f.b(PLAYER, BLDG).building_id         = BUILDING_ID_CFG;
        f.cfg_buildings[BUILDING_ID_CFG].type = BT_H_MOTHER;
        f.session_mode                        = SESSION_MODE_SP;
        sim_view  v                           = f.view();
        sim_store own                         = f.store();
        put_order(f, 0, 0xdc);
        f.order_queue[0].args[1] = -19088744; // 0xfedcba98 as int32_t: negative, nonzero both halves
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        const rec::dc_event *ev = dc().last(rec::DC_llm_prod_shuttle_unload_passengers);
        ck(ev != nullptr, "unload passengers: the callee ran");
        ck((ev != nullptr && (uint32_t)ev->a[2] == (uint32_t)-19088744),
           "unload passengers: args[1] passed as a FULL dword, not truncated");
    }
}

// ---- index 21 (param0 0xdd) @0x00467d5d: load cargo -------------------------------------------------

void test_load_resource() {
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).online_state = 0;
        sim_view  v                    = f.view();
        sim_store own                  = f.store();
        put_order(f, 0, 0xdd);
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().events.size(), 0, "load resource: online_state == 0 -> nothing");
    }
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).building_id         = BUILDING_ID_CFG;
        f.cfg_buildings[BUILDING_ID_CFG].type = BT_A_PORT;
        f.session_mode                        = 2;
        sim_view  v                           = f.view();
        sim_store own                         = f.store();
        put_order(f, 0, 0xdd);
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().events.size(), 0, "load resource: session_mode == 2 -> nothing");
    }
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).building_id         = BUILDING_ID_CFG;
        f.cfg_buildings[BUILDING_ID_CFG].type = BT_NONE;
        f.session_mode                        = SESSION_MODE_SP;
        sim_view  v                           = f.view();
        sim_store own                         = f.store();
        put_order(f, 0, 0xdd);
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().events.size(), 0, "load resource: not a transfer terminal -> nothing");
    }
    // THE TAIL NOOP RUNS EVEN WHEN NOTHING WAS LOADED: an unbound slot never loads, but the tail
    // is OUTSIDE the slot test.
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).building_id         = BUILDING_ID_CFG;
        f.cfg_buildings[BUILDING_ID_CFG].type = BT_H_PORT;
        f.session_mode                        = SESSION_MODE_SP;
        sim_view  v                           = f.view();
        sim_store own                         = f.store();
        put_order(f, 0, 0xdd);
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().count(rec::DC_llm_prod_shuttle_slot_bind_default), 1,
              "load resource: attempts the bind when unbound");
        ck_eq((uint32_t)dc().count(rec::DC_llm_prod_shuttle_load_resource), 0,
              "load resource: no load, still unbound");
        ck_eq((uint32_t)dc().count(rec::DC_llm_bldg_load_resource_tail_noop), 1,
              "load resource: the tail noop STILL runs -- it is OUTSIDE the slot test");
    }
    // Bound: both args narrowed to 16 bits; args[3] supplies the FIRST passed value, args[2] the
    // second -- a plausible swap this pins down.
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).building_id         = BUILDING_ID_CFG;
        f.b(PLAYER, BLDG).shuttle_slot        = 7;
        f.cfg_buildings[BUILDING_ID_CFG].type = BT_A_MOTHER;
        f.session_mode                        = SESSION_MODE_SP;
        sim_view  v                           = f.view();
        sim_store own                         = f.store();
        put_order(f, 0, 0xdd);
        f.order_queue[0].args[3] = 0x1000beef;
        f.order_queue[0].args[2] = 0x2000cafe;
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        const rec::dc_event *ev = dc().last(rec::DC_llm_prod_shuttle_load_resource);
        ck(ev != nullptr, "load resource: the callee ran");
        ck((ev != nullptr && ev->a[2] == 0xbeef),
           "load resource: args[3] truncated to 16 bits, passed 3rd");
        ck((ev != nullptr && ev->a[3] == 0xcafe),
           "load resource: args[2] truncated to 16 bits, passed 4th");
        ck_eq((uint32_t)dc().count(rec::DC_llm_bldg_load_resource_tail_noop), 1,
              "load resource: and the tail noop still runs");
    }
}

// ---- index 22 (param0 0xde) @0x00467eb1: unload cargo -------------------------------------------------

void test_unload_resource() {
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).building_id         = BUILDING_ID_CFG;
        f.cfg_buildings[BUILDING_ID_CFG].type = BT_A_PORT;
        f.session_mode                        = SESSION_MODE_SP;
        sim_view  v                           = f.view();
        sim_store own                         = f.store();
        put_order(f, 0, 0xde);
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().events.size(), 0,
              "unload resource: shuttle_slot == 0 -> nothing, no bind attempt (same shape as index 20)");
    }
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).energy       = 0.0;
        f.b(PLAYER, BLDG).shuttle_slot = 2;
        sim_view  v                    = f.view();
        sim_store own                  = f.store();
        put_order(f, 0, 0xde);
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().events.size(), 0, "unload resource: energy == 0 -> nothing");
    }
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).shuttle_slot        = 2;
        f.b(PLAYER, BLDG).building_id         = BUILDING_ID_CFG;
        f.cfg_buildings[BUILDING_ID_CFG].type = BT_A_PORT;
        f.session_mode                        = 3;
        sim_view  v                           = f.view();
        sim_store own                         = f.store();
        put_order(f, 0, 0xde);
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().events.size(), 0, "unload resource: session_mode != 1 -> nothing");
    }
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).shuttle_slot        = 2;
        f.b(PLAYER, BLDG).building_id         = BUILDING_ID_CFG;
        f.cfg_buildings[BUILDING_ID_CFG].type = BT_NONE;
        f.session_mode                        = SESSION_MODE_SP;
        sim_view  v                           = f.view();
        sim_store own                         = f.store();
        put_order(f, 0, 0xde);
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().events.size(), 0, "unload resource: not a transfer terminal -> nothing");
    }
    // ASYMMETRIC truncation: args[3] narrowed to 16 bits, args[2] passed as a FULL dword -- the
    // OPPOSITE pairing from index 21 (both narrowed). A translation that "fixed" it to match 21
    // disagrees exactly here.
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).shuttle_slot        = 8;
        f.b(PLAYER, BLDG).building_id         = BUILDING_ID_CFG;
        f.cfg_buildings[BUILDING_ID_CFG].type = BT_H_PORT;
        f.session_mode                        = SESSION_MODE_SP;
        sim_view  v                           = f.view();
        sim_store own                         = f.store();
        put_order(f, 0, 0xde);
        f.order_queue[0].args[3] = 0x1000dead; // narrowed to 16 bits
        f.order_queue[0].args[2] = -12345678;  // passed FULL width, sign and all
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        const rec::dc_event *ev = dc().last(rec::DC_llm_prod_shuttle_unload_resource);
        ck(ev != nullptr, "unload resource: the callee ran");
        ck((ev != nullptr && ev->a[2] == 0xdead), "unload resource: args[3] narrowed to 16 bits");
        ck((ev != nullptr && (uint32_t)ev->a[3] == (uint32_t)-12345678),
           "unload resource: args[2] passed as a full dword");
    }
}

// ---- index 23 (param0 0xdf) @0x004679e9: empty the cargo hold -----------------------------------------

void test_flush_cargo_hold() {
    // NO online_state gate on this arm -- the one thing that separates it from every neighbour: a
    // dead-looking building (online_state == 0) with a bound slot still flushes.
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).online_state        = 0;
        f.b(PLAYER, BLDG).shuttle_slot        = 9;
        f.b(PLAYER, BLDG).building_id         = BUILDING_ID_CFG;
        f.cfg_buildings[BUILDING_ID_CFG].type = BT_A_PORT;
        f.session_mode                        = SESSION_MODE_SP;
        sim_view  v                           = f.view();
        sim_store own                         = f.store();
        put_order(f, 0, 0xdf);
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().count(rec::DC_llm_strat_bldg_flush_cargo_hold), 1,
              "flush cargo hold: online_state == 0 does NOT gate this arm (unlike 19/20/21/22)");
    }
    // Energy and shuttle_slot ARE gated, together in one condition.
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).energy       = 0.0;
        f.b(PLAYER, BLDG).shuttle_slot = 9;
        sim_view  v                    = f.view();
        sim_store own                  = f.store();
        put_order(f, 0, 0xdf);
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().events.size(), 0,
              "flush cargo hold: energy == 0 -> nothing, even with a slot bound");
    }
    {
        sim_fixture f;
        seed_live_building(f);
        // shuttle_slot left at 0
        sim_view  v   = f.view();
        sim_store own = f.store();
        put_order(f, 0, 0xdf);
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().events.size(), 0,
              "flush cargo hold: shuttle_slot == 0 -> nothing, and no bind attempt either");
    }
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).shuttle_slot        = 9;
        f.b(PLAYER, BLDG).building_id         = BUILDING_ID_CFG;
        f.cfg_buildings[BUILDING_ID_CFG].type = BT_A_PORT;
        f.session_mode                        = 3;
        sim_view  v                           = f.view();
        sim_store own                         = f.store();
        put_order(f, 0, 0xdf);
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().events.size(), 0, "flush cargo hold: session_mode != 1 -> nothing");
    }
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).shuttle_slot        = 9;
        f.b(PLAYER, BLDG).building_id         = BUILDING_ID_CFG;
        f.cfg_buildings[BUILDING_ID_CFG].type = BT_NONE;
        f.session_mode                        = SESSION_MODE_SP;
        sim_view  v                           = f.view();
        sim_store own                         = f.store();
        put_order(f, 0, 0xdf);
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().events.size(), 0, "flush cargo hold: not a transfer terminal -> nothing");
    }
    // Happy path, and the ORDER: flush then notify.
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).shuttle_slot        = 9;
        f.b(PLAYER, BLDG).building_id         = BUILDING_ID_CFG;
        f.cfg_buildings[BUILDING_ID_CFG].type = BT_H_MOTHER;
        f.session_mode                        = SESSION_MODE_SP;
        sim_view  v                           = f.view();
        sim_store own                         = f.store();
        put_order(f, 0, 0xdf);
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().count(rec::DC_llm_strat_bldg_flush_cargo_hold), 1,
              "flush cargo hold: flushes exactly once");
        ck_eq((uint32_t)dc().count(rec::DC_llm_bldg_transfer_notify_noop), 1,
              "flush cargo hold: and notifies exactly once");
        ck_eq((uint32_t)dc().events.size(), 2, "flush cargo hold: nothing else happens");
        ck(dc().first_index(rec::DC_llm_strat_bldg_flush_cargo_hold) <
               dc().first_index(rec::DC_llm_bldg_transfer_notify_noop),
           "flush cargo hold: the flush happens BEFORE the notify");
    }
}

// ---- index 24 (param0 0xe6) @0x00467364: purge the dock's dead/pending entries ------------------------

void test_purge_dead_docked() {
    // No gate at all: energy 0 AND online_state 0, both simultaneously "dead", still runs.
    sim_fixture f;
    f.b(PLAYER, BLDG)              = building{};
    f.b(PLAYER, BLDG).energy       = 0.0;
    f.b(PLAYER, BLDG).online_state = 0;
    // Distinct from BLDG (5) and nonzero, to catch a translation that passes object_index to BOTH
    // callees instead of routing one of them through the sub-roster id.
    f.b(PLAYER, BLDG).sub_id = 11;
    sim_view  v              = f.view();
    sim_store own            = f.store();
    put_order(f, 0, 0xe6);
    dc().reset();
    dispatch_calls c = rec::recording_calls();
    detail::dispatch_building_order(v, own, c, ctx_for());

    ck_eq((uint32_t)dc().count(rec::DC_llm_strat_storage_purge_dead_docked), 1,
          "purge dead docked: no energy/online gate at all -- a dead-looking building still runs it");
    ck_eq((uint32_t)dc().count(rec::DC_llm_storage_cancel_pending_docked), 1,
          "purge dead docked: and cancels pending docked entries too");
    ck_eq((uint32_t)dc().events.size(), 2, "purge dead docked: and nothing else");

    const rec::dc_event *purge  = dc().last(rec::DC_llm_strat_storage_purge_dead_docked);
    const rec::dc_event *cancel = dc().last(rec::DC_llm_storage_cancel_pending_docked);
    ck(purge != nullptr && cancel != nullptr, "purge dead docked: both callees are on the log");
    ck((purge != nullptr && purge->a[0] == PLAYER), "purge dead docked: purge takes the player");
    ck((purge != nullptr && purge->a[1] == 11),
       "purge dead docked: purge takes the SUB-ROSTER id (sub_id), not the building index");
    ck((cancel != nullptr && cancel->a[0] == PLAYER), "purge dead docked: cancel takes the player");
    ck((cancel != nullptr && cancel->a[1] == BLDG),
       "purge dead docked: cancel takes the BUILDING index (object_index) -- different from purge's argument");
}

// ---- index 25 (param0 0xea) @0x00467404: place a building -----------------------------------------

void test_instant_construct() {
    constexpr int32_t TILE_X  = 33;
    constexpr int32_t TILE_Y  = 44;
    constexpr int32_t BTYPE   = 11;
    constexpr int32_t PAYLOAD = 22;

    auto seed_order = [](sim_fixture &f) {
        put_order(f, 0, (int16_t)0xea);
        f.order_queue[0].args[0] = BTYPE;
        f.order_queue[0].args[1] = PAYLOAD;
        f.order_queue[0].args[4] = TILE_X;
        f.order_queue[0].args[5] = TILE_Y;
    };

    // Happy path: clear, a primary mother present on this planet, no tutorial gate, pay succeeds.
    // No gate of any kind guards this arm -- not even energy -- so the fixture's building is left
    // at its all-zero default throughout this whole function.
    {
        sim_fixture f;
        seed_order(f);
        f.planet_index                            = 2;
        f.profiles[PLAYER].primary_mother_bldg[2] = 77; // nonzero: a mother exists
        f.tutorial_step                           = 0;
        sim_view  v                               = f.view();
        sim_store own                             = f.store();
        dc().reset();
        dc().ret[rec::DC_llm_bldg_footprint_is_clear] = 1; // clear
        dispatch_calls c                              = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());

        const rec::dc_event *clear = dc().last(rec::DC_llm_bldg_footprint_is_clear);
        ck(clear != nullptr, "instant construct: checks the footprint");
        ck((clear != nullptr && clear->a[0] == TILE_X), "instant construct: footprint takes args[4] (tile x) first");
        ck((clear != nullptr && clear->a[1] == TILE_Y), "instant construct: then args[5] (tile y)");
        ck((clear != nullptr && clear->a[2] == BTYPE), "instant construct: then args[0] (building type)");
        ck((clear != nullptr && clear->a[3] == 8), "instant construct: and a fixed radius, 8");

        ck_eq((uint32_t)dc().count(rec::DC_llm_bldg_pay_build_cost), 1, "instant construct: pays the build cost");
        const rec::dc_event *pay = dc().last(rec::DC_llm_bldg_pay_build_cost);
        ck((pay != nullptr && pay->a[0] == PLAYER), "instant construct: pay takes the player");
        ck((pay != nullptr && pay->a[1] == BTYPE), "instant construct: and args[0] as the type to build");

        const rec::dc_event *fin = dc().last(rec::DC_llm_bldg_construct_finalize);
        ck(fin != nullptr, "instant construct: commits the building");
        ck((fin != nullptr && fin->a[0] == PAYLOAD), "instant construct: finalize's 1st arg is args[1] (payload)");
        ck((fin != nullptr && fin->a[1] == TILE_Y), "instant construct: 2nd arg is args[5] (tile y), NOT args[4]");
        ck((fin != nullptr && fin->a[2] == PLAYER), "instant construct: 3rd arg is the player");
        ck((fin != nullptr && fin->a[3] == 1), "instant construct: 4th arg is the constant 1");
        ck((fin != nullptr && fin->a[4] == TILE_X), "instant construct: 5th arg is args[4] (tile x)");
        ck((fin != nullptr && fin->a[5] == BTYPE), "instant construct: 6th arg is args[0] (building type) again");

        ck_eq((uint32_t)dc().count(rec::DC_llm_strat_ai_notify_bldg_constructed), 0,
              "instant construct: the paid path does NOT run the refused notification");
        ck_eq((uint32_t)dc().count(rec::DC_llm_snd_play), 0, "instant construct: nor the refusal sound");
    }

    // Footprint blocked -> the refused tail, no pay attempt, no finalize.
    {
        sim_fixture f;
        seed_order(f);
        f.planet_index                            = 2;
        f.profiles[PLAYER].primary_mother_bldg[2] = 77;
        f.sim_active                              = 0; // keep the sound OUT of this case
        sim_view  v                               = f.view();
        sim_store own                             = f.store();
        dc().reset();
        dc().ret[rec::DC_llm_bldg_footprint_is_clear] = 0; // blocked
        dispatch_calls c                              = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());

        ck_eq((uint32_t)dc().count(rec::DC_llm_bldg_pay_build_cost), 0,
              "instant construct: a blocked footprint never even tries to pay");
        ck_eq((uint32_t)dc().count(rec::DC_llm_bldg_construct_finalize), 0,
              "instant construct: and never commits");
        const rec::dc_event *notif = dc().last(rec::DC_llm_strat_ai_notify_bldg_constructed);
        ck(notif != nullptr, "instant construct: the refused tail still notifies");
        ck((notif != nullptr && notif->a[0] == PLAYER), "refused notify: player, 1st");
        ck((notif != nullptr && notif->a[1] == TILE_X), "refused notify: args[4] (tile x), 2nd");
        ck((notif != nullptr && notif->a[2] == 0), "refused notify: a fixed 0, 3rd");
        ck((notif != nullptr && notif->a[3] == BTYPE), "refused notify: args[0] (type), 4th");
        ck((notif != nullptr && notif->a[4] == TILE_Y), "refused notify: args[5] (tile y), 5th");
        ck((notif != nullptr && notif->a[5] == 2), "refused notify: a fixed 2, 6th");
    }

    // Pay fails (rc != 0) -> the SAME refused tail runs, even though the footprint WAS clear.
    {
        sim_fixture f;
        seed_order(f);
        f.planet_index                            = 2;
        f.profiles[PLAYER].primary_mother_bldg[2] = 77;
        sim_view  v                               = f.view();
        sim_store own                             = f.store();
        dc().reset();
        dc().ret[rec::DC_llm_bldg_footprint_is_clear] = 1;
        dc().ret[rec::DC_llm_bldg_pay_build_cost]     = 5; // unaffordable
        dispatch_calls c                              = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().count(rec::DC_llm_bldg_construct_finalize), 0,
              "instant construct: an unpaid build never commits");
        ck_eq((uint32_t)dc().count(rec::DC_llm_strat_ai_notify_bldg_constructed), 1,
              "instant construct: and still runs the refused tail");
    }

    // No primary mother on this planet -> refused, without even trying to pay.
    {
        sim_fixture f;
        seed_order(f);
        f.planet_index                            = 3;
        f.profiles[PLAYER].primary_mother_bldg[3] = 0; // none
        sim_view  v                               = f.view();
        sim_store own                             = f.store();
        dc().reset();
        dc().ret[rec::DC_llm_bldg_footprint_is_clear] = 1;
        dispatch_calls c                              = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().count(rec::DC_llm_bldg_pay_build_cost), 0,
              "instant construct: no mother on the planet -> never tries to pay");
        ck_eq((uint32_t)dc().count(rec::DC_llm_strat_ai_notify_bldg_constructed), 1,
              "instant construct: and still refuses");
    }

    // The refusal SOUND is gated on sim_active AND is_local_player, on top of being refused at
    // all -- three independent switches, each isolated.
    {
        sim_fixture f;
        seed_order(f);
        f.planet_index = 2; // primary_mother_bldg[2] left 0 -> refused
        f.sim_active   = 1;
        f.player_side  = (int16_t)PLAYER; // local
        sim_view  v    = f.view();
        sim_store own  = f.store();
        dc().reset();
        dc().ret[rec::DC_llm_bldg_footprint_is_clear] = 1;
        dispatch_calls c                              = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().count(rec::DC_llm_snd_play), 1,
              "instant construct: refused + sim_active + local -> the refusal blip plays");
        const rec::dc_event *snd = dc().last(rec::DC_llm_snd_play);
        ck((snd != nullptr && snd->a[0] == 0xb3 && snd->a[1] == 100),
           "instant construct: the specific refusal sound id (0xb3) and volume (100)");
    }
    {
        sim_fixture f;
        seed_order(f);
        f.planet_index = 2;
        f.sim_active   = 0; // NOT active
        f.player_side  = (int16_t)PLAYER;
        sim_view  v    = f.view();
        sim_store own  = f.store();
        dc().reset();
        dc().ret[rec::DC_llm_bldg_footprint_is_clear] = 1;
        dispatch_calls c                              = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().count(rec::DC_llm_snd_play), 0,
              "instant construct: refused but sim_active == 0 -> no blip");
    }
    {
        sim_fixture f;
        seed_order(f);
        f.planet_index = 2;
        f.sim_active   = 1;
        // f.player_side stays the fixture default (7) -- NOT local
        sim_view  v   = f.view();
        sim_store own = f.store();
        dc().reset();
        dc().ret[rec::DC_llm_bldg_footprint_is_clear] = 1;
        dispatch_calls c                              = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().count(rec::DC_llm_snd_play), 0,
              "instant construct: refused but not the local player -> no blip");
    }

    // Tutorial gate: a non-local player cannot build during the tutorial even with everything
    // else satisfied; the local player can.
    {
        sim_fixture f;
        seed_order(f);
        f.planet_index                            = 2;
        f.profiles[PLAYER].primary_mother_bldg[2] = 77;
        f.tutorial_step                           = 1; // mid-tutorial
        // f.player_side stays 7 -- NOT local
        sim_view  v   = f.view();
        sim_store own = f.store();
        dc().reset();
        dc().ret[rec::DC_llm_bldg_footprint_is_clear] = 1;
        dispatch_calls c                              = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().count(rec::DC_llm_bldg_pay_build_cost), 0,
              "instant construct: mid-tutorial + not the local player -> refused before paying");
    }
    {
        sim_fixture f;
        seed_order(f);
        f.planet_index                            = 2;
        f.profiles[PLAYER].primary_mother_bldg[2] = 77;
        f.tutorial_step                           = 1;
        f.player_side                             = (int16_t)PLAYER; // local -- the tutorial gate is bypassed
        sim_view  v                               = f.view();
        sim_store own                             = f.store();
        dc().reset();
        dc().ret[rec::DC_llm_bldg_footprint_is_clear] = 1;
        dispatch_calls c                              = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().count(rec::DC_llm_bldg_pay_build_cost), 1,
              "instant construct: mid-tutorial but IS the local player -> allowed through");
    }
}

} // namespace

void run_dispatch_bldg2_tests() {
    test_start_research();
    test_shuttle_depart();
    test_mother_port_fallthrough_shared_body();
    test_mother_port_depart_behavior();
    test_cargo_bind_slot();
    test_load_passengers();
    test_unload_passengers();
    test_load_resource();
    test_unload_resource();
    test_flush_cargo_hold();
    test_purge_dead_docked();
    test_instant_construct();
}

} // namespace mh::sim::test
