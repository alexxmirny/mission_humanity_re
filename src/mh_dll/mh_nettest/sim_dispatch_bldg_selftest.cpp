//
// sim_dispatch_bldg_selftest.cpp -- `simtest` cases for the 26 BUILDING-order arms (kind 0x40) of
// llm_strat_order_queue_dispatch, i.e. for libmh/sim/sim_order_dispatch_bldg.cpp.
//
// The spine (decode, loop, router, retention, lockstep) is covered in sim_dispatch_selftest.cpp and
// is NOT re-tested here. Read that file first: it explains the recording table and holds the shared
// idioms these cases copy.
//
// EVERY CASE CALLS THE ARM DIRECTLY -- detail::dispatch_building_order(v, own, c, x) -- rather than
// going through order_queue_dispatch. The router is already exhaustively tested next door, and
// entering through the arm keeps a failure here pointing at the arm.
//
#include "sim/sim_order_dispatch.h"

#include "sim_dispatch_calls.gen.h"
#include "sim_test_support.h"

#include <limits>
#include <string>

namespace mh::sim::test {
namespace {

using namespace mh::sim;
using rec::dc;

constexpr int32_t PLAYER = 3;
constexpr int32_t BLDG   = 5;

// The four values the loop keeps in stack slots for the whole of one iteration. `kind` is ALWAYS
// 0x40 on this path -- the router reaches dispatch_building_order for that nibble and no other --
// and arm 1's preserved defect depends on that being literally true, so it is not a free parameter.
dispatch_ctx ctx_for(int32_t slot = 0) {
    dispatch_ctx x;
    x.slot         = slot;
    x.player       = (uint32_t)PLAYER;
    x.kind         = ORDER_KIND_BLDG;
    x.object_index = BLDG;
    return x;
}

// A record carrying one building opcode, with every args[] slot at the "leave unchanged" sentinel.
void put_order(sim_fixture &f, int32_t slot, int16_t param0) {
    order q{};
    q.owner_and_kind = (uint16_t)(ORDER_KIND_BLDG | (uint32_t)PLAYER);
    q.unit_index     = (uint16_t)BLDG;
    q.param0         = param0;
    for (int32_t &a : q.args) a = -1;
    f.order_queue[slot] = q;
}

// A building that clears the two gates most arms open with: positive energy and online.
void seed_live_building(sim_fixture &f) {
    f.b(PLAYER, BLDG)              = building{};
    f.b(PLAYER, BLDG).energy       = 1.0;
    f.b(PLAYER, BLDG).online_state = 1;
}

// ---- constants arms 1..13 touch, duplicated from sim_order_dispatch_bldg.cpp's own private ones --
//
// That file's constants live in an anonymous namespace scoped to its own TU, so this file cannot see
// them and copies the values it needs -- same names, same source (the CMPs the arms compare against),
// not re-derived.
constexpr uint16_t ST_CONSTRUCTION        = 0x64; // BLDG_STATE_CONSTRUCTION
constexpr uint16_t ST_CHARGE_GATE         = 0x69; // BLDG_STATE_CHARGE_GATE
constexpr uint16_t ST_CHARGE_STEP         = 0x6a; // BLDG_STATE_CHARGE_STEP
constexpr uint16_t ST_DISMANTLING         = 0x6b; // BLDG_STATE_DISMANTLING
constexpr uint16_t ST_PROD_PICK_NEXT      = 0x6c; // BLDG_STATE_PROD_PICK_NEXT
constexpr uint16_t ST_PROD_WORKING        = 0x6d; // BLDG_STATE_PROD_WORKING
constexpr uint16_t ST_PROD_BLOCKED_NOTIFY = 0x6e; // BLDG_STATE_PROD_BLOCKED_NOTIFY
constexpr uint16_t ST_MINE_SCAN_DEPOSITS  = 0x72; // BLDG_STATE_MINE_SCAN_DEPOSITS
constexpr uint16_t ST_IDLE_NOOP           = 0x77; // BLDG_STATE_IDLE_NOOP
constexpr uint16_t ST_HANGAR_RECHARGE_CHK = 0x78; // BLDG_STATE_HANGAR_RECHARGE_CHK
constexpr uint16_t ST_TURRET_ATTACK       = 0x7b; // BLDG_STATE_TURRET_ATTACK
constexpr uint16_t ST_UPGRADING           = 0x82; // BLDG_STATE_UPGRADING

constexpr uint8_t TYPE_A_MOTHER  = 0x06;
constexpr uint8_t TYPE_A_GARAGE  = 0x08;
constexpr uint8_t TYPE_A_PORT    = 0x0c;
constexpr uint8_t TYPE_A_SHUTTLE = 0x0d;
constexpr uint8_t TYPE_H_MOTHER  = 0x1a;
constexpr uint8_t TYPE_H_GARAGE  = 0x1c;
constexpr uint8_t TYPE_H_PORT    = 0x20;
constexpr uint8_t TYPE_H_SHUTTLE = 0x21;
constexpr uint8_t TYPE_OTHER     = 0x01; // not a mother, a port, a shuttle, or a garrison type

// ---- the default arm, and the two gates every arm shares ----------------------------------------
//
// Index 0 has a REAL BODY (0x00469545), so every unrecognised param0 runs it -- this is not a
// no-op fall-through, and a translation that treated it as one would silently stop resetting the
// building's state. It is also the cheapest place to pin the shared energy/online gates.
void test_default_arm() {
    // The happy path: finish the current order, take the opcode AS the new state, reset progress.
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).state          = 0x11;
        f.b(PLAYER, BLDG).cycle_progress = 0.5;
        sim_view  v                      = f.view();
        sim_store own                    = f.store();
        put_order(f, 0, 0x50); // not a table-A opcode

        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());

        ck_eq((uint32_t)dc().count(rec::DC_llm_bldg_finish_current_order), 1,
              "bldg default arm: finishes the current order");
        ck_eq((uint32_t)f.b(PLAYER, BLDG).state, 0x50,
              "bldg default arm: the unrecognised opcode BECOMES the building state");
        ck_eq_d(f.b(PLAYER, BLDG).cycle_progress, 0.0,
                "bldg default arm: the cycle progress is reset");
    }

    // The energy gate is `<= 0` -- exactly zero is dead.
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).energy = 0.0;
        sim_view  v              = f.view();
        sim_store own            = f.store();
        put_order(f, 0, 0x50);

        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().events.size(), 0, "bldg default arm: energy == 0 exactly -> nothing");
    }

    // A NaN energy RUNS the body. Same x87 idiom as the loop's gate (FLDZ; FCOMP; FNSTSW; SAHF;
    // JNC), same reason: C0 -> CF is set for unordered as well as for less-than. Unreachable from
    // the rig, which is the whole argument for having this oracle.
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).energy = std::numeric_limits<double>::quiet_NaN();
        sim_view  v              = f.view();
        sim_store own            = f.store();
        put_order(f, 0, 0x50);

        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().count(rec::DC_llm_bldg_finish_current_order), 1,
              "bldg default arm: a NaN energy RUNS the body");
    }

    // The second gate: offline buildings do nothing, whatever their energy.
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).online_state = 0;
        sim_view  v                    = f.view();
        sim_store own                  = f.store();
        put_order(f, 0, 0x50);

        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().events.size(), 0, "bldg default arm: online_state == 0 -> nothing");
    }
}

// ---- index 1 (param0 0x6a) @0x00468d78: PAY_CYCLE_UPKEEP -----------------------------------------
void test_pay_cycle_upkeep() {
    // The two shared gates, and the NaN case the rig can never send (0x00468d88-0x00468d93).
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).energy = 0.0;
        sim_view  v              = f.view();
        sim_store own            = f.store();
        put_order(f, 0, 0x6a);
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().events.size(), 0, "PAY_CYCLE_UPKEEP: energy == 0 exactly -> nothing");
    }
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).online_state = 0;
        sim_view  v                    = f.view();
        sim_store own                  = f.store();
        put_order(f, 0, 0x6a);
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().events.size(), 0, "PAY_CYCLE_UPKEEP: online_state == 0 -> nothing");
    }
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).energy = std::numeric_limits<double>::quiet_NaN();
        sim_view  v              = f.view();
        sim_store own            = f.store();
        put_order(f, 0, 0x6a);
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().count(rec::DC_llm_strat_bldg_pay_cycle_inputs), 1,
              "PAY_CYCLE_UPKEEP: a NaN energy RUNS the body");
    }

    // rc != 0: the message prints only when the order's owner is the LOCAL player (PlayerSide).
    // Closed by the fixture default (player_side == 7, orders are owned by PLAYER == 3).
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).building_id = 9;
        sim_view  v                   = f.view();
        sim_store own                 = f.store();
        put_order(f, 0, 0x6a);

        dc().reset();
        dispatch_calls c                                  = rec::recording_calls();
        dc().ret[rec::DC_llm_strat_bldg_pay_cycle_inputs] = 51; // a nonzero text id: "payment failed"
        detail::dispatch_building_order(v, own, c, ctx_for());

        ck_eq((uint32_t)dc().count(rec::DC_llm_strat_bldg_pay_cycle_inputs), 1,
              "PAY_CYCLE_UPKEEP: pays once");
        ck_eq((uint32_t)dc().count(rec::DC_w_sprintf__vss), 0,
              "PAY_CYCLE_UPKEEP: rc != 0, not the local player -> no message formatted");
        ck_eq((uint32_t)dc().count(rec::DC_game_ui_PrintTextMessage), 0,
              "PAY_CYCLE_UPKEEP: and none printed");
        ck_eq((uint32_t)dc().count(rec::DC_llm_bldg_finish_current_order), 0,
              "PAY_CYCLE_UPKEEP: a failed payment never finishes the order");
    }

    // rc != 0, LOCAL player: the message DOES print, and it is built from the FAILING building's
    // own name id (cfg_buildings[building_id].id) and the rc as the reason id -- not swapped, and
    // not the acting building's array slot (BLDG == 5) or PLAYER (3), which is what a copy-paste of
    // the wrong index would produce instead.
    {
        static const wchar_t NAME[]   = L"pay-cycle-name";
        static const wchar_t REASON[] = L"pay-cycle-reason";
        sim_fixture          f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).building_id = 9;
        f.cfg_buildings[9].id         = 60;
        f.text_ptrs[60]               = NAME;
        f.text_ptrs[51]               = REASON;
        f.player_side                 = PLAYER; // opens the message gate
        sim_view  v                   = f.view();
        sim_store own                 = f.store();
        put_order(f, 0, 0x6a);

        dc().reset();
        dispatch_calls c                                  = rec::recording_calls();
        dc().ret[rec::DC_llm_strat_bldg_pay_cycle_inputs] = 51;
        detail::dispatch_building_order(v, own, c, ctx_for());

        ck_eq((uint32_t)dc().count(rec::DC_w_sprintf__vss), 1,
              "PAY_CYCLE_UPKEEP: rc != 0, IS the local player -> the message is formatted");
        ck_eq((uint32_t)dc().count(rec::DC_game_ui_PrintTextMessage), 1,
              "PAY_CYCLE_UPKEEP: and printed");
        const rec::dc_event *fmt = dc().last(rec::DC_w_sprintf__vss);
        ck(fmt != nullptr && (const wchar_t *)(intptr_t)fmt->a[2] == NAME,
           "PAY_CYCLE_UPKEEP: the name comes from text_ptrs[cfg_buildings[building_id].id]");
        ck(fmt != nullptr && (const wchar_t *)(intptr_t)fmt->a[3] == REASON,
           "PAY_CYCLE_UPKEEP: the reason comes from text_ptrs[rc], rc itself");
        ck_eq((uint32_t)dc().count(rec::DC_llm_bldg_finish_current_order), 0,
              "PAY_CYCLE_UPKEEP: still no finish -- printing a message is not paying");
    }

    // rc == 0: the happy path, AND THE PRESERVED ORIGINAL BUG (0x00468e68) in the same case, since
    // the bug's code only runs once payment succeeds.
    //
    // building_id (9) is DISTINCT from BLDG (the buildings[] array slot, 5) and from 0x40 (the kind
    // nibble every order on this path carries, 64) -- three different numbers so a translation that
    // confused any two of them fails a DIFFERENT assertion than one that got the real bug right.
    // cfg_buildings[building_id=9].builder_count is 0 (so the GUARD -- correctly indexed by
    // building_id -- is trivially satisfied by any positive current_workers, and AS A SIDE EFFECT
    // also forces the population-gated re-staffing branch off deterministically: see the note below).
    // cfg_buildings[0x40].builder_count is 9, unrelated to building_id's 0 -- DIFFERENT VALUES, which
    // is what the task brief asks this case to pin.
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).building_id       = 9;
        f.b(PLAYER, BLDG).current_workers   = 5;
        f.b(PLAYER, BLDG).state             = 0x11; // anything but CHARGE_STEP/CHARGE_GATE
        f.b(PLAYER, BLDG).cycle_progress    = 0.5;
        f.cfg_buildings[9].builder_count    = 0;
        f.cfg_buildings[0x40].builder_count = 9; // the KIND nibble's slot -- what the bug actually reads
        sim_view  v                         = f.view();
        sim_store own                       = f.store();
        put_order(f, 0, 0x6a);

        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());

        ck_eq((uint32_t)dc().count(rec::DC_llm_strat_bldg_pay_cycle_inputs), 1,
              "PAY_CYCLE_UPKEEP: pays once");
        ck_eq((uint32_t)dc().count(rec::DC_llm_bldg_finish_current_order), 1,
              "PAY_CYCLE_UPKEEP: rc == 0 -> the order finishes");
        ck_eq((uint32_t)f.b(PLAYER, BLDG).state, ST_CHARGE_GATE,
              "PAY_CYCLE_UPKEEP: the FINAL state is the charge gate, not the intermediate charge step");
        ck_eq_d(f.b(PLAYER, BLDG).cycle_progress, 0.0, "PAY_CYCLE_UPKEEP: cycle progress is reset");

        // THE BUG ITSELF. current_workers(5) > cfg_buildings[building_id=9].builder_count(0) is a
        // real, correctly-indexed guard, so the call happens; the AMOUNT is
        // current_workers(5) - cfg_buildings[KIND=0x40].builder_count(9) = -4, cast to uint32_t.
        // A "corrected" implementation that indexed the amount by building_id too would compute
        // 5 - 0 = 5 here instead -- a completely different, non-wrapped value -- and fail this check.
        ck_eq((uint32_t)dc().count(rec::DC_llm_strat_bldg_unassign_workers), 1,
              "PAY_CYCLE_UPKEEP: the worker-unassign guard fires");
        const rec::dc_event *un = dc().last(rec::DC_llm_strat_bldg_unassign_workers);
        ck(un != nullptr && un->a[0] == PLAYER && un->a[1] == BLDG,
           "PAY_CYCLE_UPKEEP: unassign targets (player, object_index)");
        ck(un != nullptr && (uint32_t)un->a[2] == 0xFFFFFFFCu,
           "PAY_CYCLE_UPKEEP: PRESERVED BUG -- the amount is 5 - builder_count[kind=0x40](9), not "
           "5 - builder_count[building_id=9](0); a fix here would desync against the original");

        // The re-staffing (assign_workers) branch reads _G_LLM_STRAT_POP_STATS[x.player].human, and
        // the offline fixture's `population` member is ONE record, not MAX_PLAYERS (see this file's
        // final report) -- indexing it at player 3 is out of the fixture's bound. builder_count[9]
        // == 0 forces the branch's SECOND (`&&`) operand false regardless, so the branch is
        // deterministically skipped without this case depending on that unindexable read's value.
        ck_eq((uint32_t)dc().count(rec::DC_llm_strat_bldg_assign_workers), 0,
              "PAY_CYCLE_UPKEEP: builder_count[building_id] == 0 -> the re-staffing branch is closed");

        // builder_count[building_id] == 0 also satisfies the staffed-flag OR by itself.
        ck_eq((uint32_t)dc().count(rec::DC_llm_strat_bldg_set_staffed_flag), 1,
              "PAY_CYCLE_UPKEEP: builder_count[building_id] == 0 -> staffed flag SET, not cleared");
        ck_eq((uint32_t)dc().count(rec::DC_llm_strat_bldg_clear_staffed_flag), 0,
              "PAY_CYCLE_UPKEEP: and clear is NOT the one that fires");
        ck_eq((uint32_t)dc().count(rec::DC_llm_strat_refresh_building), 1,
              "PAY_CYCLE_UPKEEP: refreshed once");
    }
}

// ---- index 2 (param0 0x6b) @0x0046906a: RESTART_CONSTRUCT -----------------------------------------
void test_restart_construct() {
    // The energy gate, and the "already dismantling" gate -- NO online_state gate on this arm.
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).energy = 0.0;
        sim_view  v              = f.view();
        sim_store own            = f.store();
        put_order(f, 0, 0x6b);
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().events.size(), 0, "RESTART_CONSTRUCT: energy == 0 exactly -> nothing");
    }
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).state = ST_DISMANTLING;
        sim_view  v             = f.view();
        sim_store own           = f.store();
        put_order(f, 0, 0x6b);
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().events.size(), 0, "RESTART_CONSTRUCT: already dismantling -> nothing");
    }
    {
        // No online_state gate: even a building with online_state == 0 still runs, unlike almost
        // every other arm on this table (proves this arm's gate list, not a generic assumption).
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).online_state = 0;
        f.b(PLAYER, BLDG).building_id  = 3;
        f.b(PLAYER, BLDG).state        = ST_IDLE_NOOP;
        f.cfg_buildings[3].type        = TYPE_OTHER;
        sim_view  v                    = f.view();
        sim_store own                  = f.store();
        put_order(f, 0, 0x6b);
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().count(rec::DC_llm_bldg_finish_current_order), 1,
              "RESTART_CONSTRUCT: online_state == 0 does NOT gate this arm");
    }

    // A mother is never dismantled, regardless of its current state.
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).building_id = 4;
        f.b(PLAYER, BLDG).state       = ST_IDLE_NOOP;
        f.cfg_buildings[4].type       = TYPE_H_MOTHER;
        sim_view  v                   = f.view();
        sim_store own                 = f.store();
        put_order(f, 0, 0x6b);
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().events.size(), 0, "RESTART_CONSTRUCT: a mother is exempt entirely");
    }

    // state == CONSTRUCTION: the scrap/finish/reset-anim/set-cycle-progress block is SKIPPED
    // entirely -- the arm goes straight to the unassign/state-flip/re-staffing tail.
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).building_id     = 5;
        f.b(PLAYER, BLDG).state           = ST_CONSTRUCTION;
        f.b(PLAYER, BLDG).cycle_progress  = 3.5;           // a sentinel the skipped block would have overwritten
        f.b(PLAYER, BLDG).current_workers = 0;             // keeps the unassign guard closed for this case
        f.cfg_buildings[5].type           = TYPE_H_GARAGE; // a garrison type -- irrelevant, unreached
        f.cfg_buildings[5].builder_count  = 0;
        sim_view  v                       = f.view();
        sim_store own                     = f.store();
        put_order(f, 0, 0x6b);
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());

        ck_eq((uint32_t)dc().count(rec::DC_llm_bldg_scrap_stored_units), 0,
              "RESTART_CONSTRUCT: state == CONSTRUCTION -> the scrap check is skipped, not just false");
        ck_eq((uint32_t)dc().count(rec::DC_llm_bldg_finish_current_order), 0,
              "RESTART_CONSTRUCT: and finish_current_order does not run inside the skipped block");
        ck_eq((uint32_t)dc().count(rec::DC_llm_bldg_reset_construction_anim), 0,
              "RESTART_CONSTRUCT: nor reset_construction_anim");
        ck_eq_d(f.b(PLAYER, BLDG).cycle_progress, 3.5,
                "RESTART_CONSTRUCT: cycle_progress is left exactly alone (not set to build_time_2)");
        ck_eq((uint32_t)f.b(PLAYER, BLDG).state, ST_DISMANTLING,
              "RESTART_CONSTRUCT: the state still flips to DISMANTLING regardless of the branch taken");
        ck_eq((uint32_t)dc().count(rec::DC_llm_bldg_set_connected_flag), 1,
              "RESTART_CONSTRUCT: set_connected_flag runs unconditionally at the tail");
        ck_eq((uint32_t)dc().count(rec::DC_llm_strat_refresh_building), 1,
              "RESTART_CONSTRUCT: refreshed once");
    }

    // state != CONSTRUCTION and the type IS one of the twelve garrison types: the garrison is
    // scrapped, and cycle_progress becomes build_time_2 (dismantling runs the construction timer
    // BACKWARDS from full, hence _2 and not the forward build_time_d).
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).building_id     = 6;
        f.b(PLAYER, BLDG).state           = ST_IDLE_NOOP; // != CONSTRUCTION, != DISMANTLING
        f.b(PLAYER, BLDG).current_workers = 0;
        f.cfg_buildings[6].type           = TYPE_H_GARAGE; // one of the twelve
        f.cfg_buildings[6].build_time_2   = 12.5;
        f.cfg_buildings[6].builder_count  = 0;
        sim_view  v                       = f.view();
        sim_store own                     = f.store();
        put_order(f, 0, 0x6b);
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());

        ck_eq((uint32_t)dc().count(rec::DC_llm_bldg_scrap_stored_units), 1,
              "RESTART_CONSTRUCT: a garrison type (H_GARAGE) -> its stored units are scrapped");
        ck_eq((uint32_t)dc().count(rec::DC_llm_bldg_finish_current_order), 1,
              "RESTART_CONSTRUCT: and the current order finishes");
        ck_eq((uint32_t)dc().count(rec::DC_llm_bldg_reset_construction_anim), 1,
              "RESTART_CONSTRUCT: and the construction anim resets");
        ck_eq_d(f.b(PLAYER, BLDG).cycle_progress, 12.5,
                "RESTART_CONSTRUCT: cycle_progress becomes build_time_2 (the BACKWARDS timer)");
    }

    // state != CONSTRUCTION but the type is NOT one of the twelve (and not a mother): the same
    // finish/reset-anim/cycle_progress sequence still runs, just WITHOUT the scrap call. This is what
    // makes "scrap" a conditional inside the block rather than the block's own gate.
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).building_id     = 7;
        f.b(PLAYER, BLDG).state           = ST_IDLE_NOOP;
        f.b(PLAYER, BLDG).current_workers = 0;
        f.cfg_buildings[7].type           = TYPE_OTHER;
        f.cfg_buildings[7].build_time_2   = 8.0;
        f.cfg_buildings[7].builder_count  = 0;
        sim_view  v                       = f.view();
        sim_store own                     = f.store();
        put_order(f, 0, 0x6b);
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());

        ck_eq((uint32_t)dc().count(rec::DC_llm_bldg_scrap_stored_units), 0,
              "RESTART_CONSTRUCT: a non-garrison, non-mother type -> NOT scrapped");
        ck_eq((uint32_t)dc().count(rec::DC_llm_bldg_finish_current_order), 1,
              "RESTART_CONSTRUCT: but the order still finishes");
        ck_eq_d(f.b(PLAYER, BLDG).cycle_progress, 8.0,
                "RESTART_CONSTRUCT: and cycle_progress still becomes build_time_2");
    }

    // The unassign math uses building_id CORRECTLY (0x00469xxx, the sibling site index 1's bug is
    // contrasted against) -- building_id(8)'s builder_count(2) is what is subtracted, NOT the kind
    // nibble(0x40)'s(99), which is set to a wildly different value here specifically to catch a
    // translation that copied index 1's bug into this arm too.
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).building_id       = 8;
        f.b(PLAYER, BLDG).state             = ST_IDLE_NOOP;
        f.b(PLAYER, BLDG).current_workers   = 5;
        f.cfg_buildings[8].type             = TYPE_OTHER;
        f.cfg_buildings[8].builder_count    = 2;
        f.cfg_buildings[0x40].builder_count = 99; // NOT what this arm reads -- unlike index 1
        sim_view  v                         = f.view();
        sim_store own                       = f.store();
        put_order(f, 0, 0x6b);
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());

        ck_eq((uint32_t)dc().count(rec::DC_llm_strat_bldg_unassign_workers), 1,
              "RESTART_CONSTRUCT: the unassign guard fires (5 > 2)");
        const rec::dc_event *un = dc().last(rec::DC_llm_strat_bldg_unassign_workers);
        ck(un != nullptr && (uint32_t)un->a[2] == 3u,
           "RESTART_CONSTRUCT: amount is 5 - builder_count[building_id=8](2) == 3, NOT "
           "5 - builder_count[kind=0x40](99) -- no preserved-bug copy on this arm");
    }
}

// ---- index 3 (param0 0x6d) @0x0046857c: PROD_QUEUE_ACCUM -----------------------------------------
void test_prod_queue_accum() {
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).energy = 0.0;
        sim_view  v              = f.view();
        sim_store own            = f.store();
        put_order(f, 0, 0x6d);
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().events.size(), 0, "PROD_QUEUE_ACCUM: energy == 0 exactly -> nothing");
    }
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).online_state = 0;
        sim_view  v                    = f.view();
        sim_store own                  = f.store();
        put_order(f, 0, 0x6d);
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().events.size(), 0, "PROD_QUEUE_ACCUM: online_state == 0 -> nothing");
    }

    // The clamp: args[1] + queued_count[0] exceeds the cap (0x32 == 50), so args[1] is REWRITTEN IN
    // THE ORDER RECORD (a write-back into the queue, not a local) down to what fits, and the fit
    // amount is what actually gets added.
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).building_id = 2;
        f.b(PLAYER, BLDG).sub_id      = 2;
        f.b(PLAYER, BLDG).state       = ST_IDLE_NOOP; // none of the three "already producing" states
        sim_view  v                   = f.view();
        sim_store own                 = f.store();
        put_order(f, 0, 0x6d);
        f.order_queue[0].args[0]                     = 3;  // the unit-type slot
        f.order_queue[0].args[1]                     = 10; // the request
        own.production_at(PLAYER, 2).queued_count[0] = 45;

        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());

        ck_eq((uint32_t)(uint16_t)(int16_t)f.order_queue[0].args[1], 5,
              "PROD_QUEUE_ACCUM: args[1] is clamped IN THE ORDER RECORD to 50 - 45 == 5");
        ck_eq((uint32_t)own.production_at(PLAYER, 2).queued_count[0], 50,
              "PROD_QUEUE_ACCUM: the total row gets the CLAMPED amount, not the original request");
        ck_eq((uint32_t)own.production_at(PLAYER, 2).queued_count[3], 5,
              "PROD_QUEUE_ACCUM: and so does the per-type row (slot args[0] == 3)");
        ck_eq((uint32_t)dc().count(rec::DC_llm_bldg_finish_current_order), 1,
              "PROD_QUEUE_ACCUM: not already producing -> the current order finishes");
        ck_eq((uint32_t)f.b(PLAYER, BLDG).state, ST_PROD_PICK_NEXT,
              "PROD_QUEUE_ACCUM: and the state becomes PROD_PICK_NEXT");
        ck_eq((uint32_t)dc().count(rec::DC_llm_strat_refresh_building), 1,
              "PROD_QUEUE_ACCUM: refreshed once");
    }

    // The clamp reduces the request to <= 0: the arm returns WITHOUT touching either queued_count
    // row, even though the write-back into the order record still happened.
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).building_id = 2;
        f.b(PLAYER, BLDG).sub_id      = 2;
        sim_view  v                   = f.view();
        sim_store own                 = f.store();
        put_order(f, 0, 0x6d);
        f.order_queue[0].args[0]                     = 3;
        f.order_queue[0].args[1]                     = 1;
        own.production_at(PLAYER, 2).queued_count[0] = 50; // already at the cap

        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());

        ck_eq((uint32_t)(uint16_t)(int16_t)f.order_queue[0].args[1], 0,
              "PROD_QUEUE_ACCUM: clamped to exactly 0 (50 - 50)");
        ck_eq((uint32_t)own.production_at(PLAYER, 2).queued_count[0], 50,
              "PROD_QUEUE_ACCUM: args[1] <= 0 after the clamp -> the total row is untouched");
        ck_eq((uint32_t)dc().events.size(), 0,
              "PROD_QUEUE_ACCUM: and nothing at all is called");
    }

    // Already producing (state == PROD_WORKING): the queue still accumulates, but the
    // finish/state-flip/refresh sequence does NOT run -- it is gated on NOT already being in one of
    // the three producing states.
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).building_id = 2;
        f.b(PLAYER, BLDG).sub_id      = 2;
        f.b(PLAYER, BLDG).state       = ST_PROD_WORKING;
        sim_view  v                   = f.view();
        sim_store own                 = f.store();
        put_order(f, 0, 0x6d);
        f.order_queue[0].args[0] = 3;
        f.order_queue[0].args[1] = 4;

        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());

        ck_eq((uint32_t)own.production_at(PLAYER, 2).queued_count[0], 4,
              "PROD_QUEUE_ACCUM: already producing -> the queue still accumulates");
        ck_eq((uint32_t)dc().events.size(), 0,
              "PROD_QUEUE_ACCUM: but nothing is called (no re-finish of an order already in flight)");
        ck_eq((uint32_t)f.b(PLAYER, BLDG).state, ST_PROD_WORKING,
              "PROD_QUEUE_ACCUM: and the state is left exactly where it was");
    }
}

// ---- index 4 (param0 0x6f) @0x0046876d: PROD_ITEM_COMPLETE ---------------------------------------
void test_prod_item_complete() {
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).energy = 0.0;
        sim_view  v              = f.view();
        sim_store own            = f.store();
        put_order(f, 0, 0x6f);
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().events.size(), 0, "PROD_ITEM_COMPLETE: energy == 0 exactly -> nothing");
    }
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).online_state = 0;
        sim_view  v                    = f.view();
        sim_store own                  = f.store();
        put_order(f, 0, 0x6f);
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().events.size(), 0, "PROD_ITEM_COMPLETE: online_state == 0 -> nothing");
    }

    // Still queued: ONE is dropped from the type's own row and ONE from the total row -- and NOTHING
    // ELSE happens. This arm has no llm_bldg_finish_current_order call anywhere on this path.
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).building_id = 2;
        f.b(PLAYER, BLDG).sub_id      = 2;
        sim_view  v                   = f.view();
        sim_store own                 = f.store();
        put_order(f, 0, 0x6f);
        f.order_queue[0].args[0]                     = 3;
        own.production_at(PLAYER, 2).queued_count[3] = 7;
        own.production_at(PLAYER, 2).queued_count[0] = 20;

        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());

        ck_eq((uint32_t)own.production_at(PLAYER, 2).queued_count[3], 6,
              "PROD_ITEM_COMPLETE: still queued -> the type's own row drops by one");
        ck_eq((uint32_t)own.production_at(PLAYER, 2).queued_count[0], 19,
              "PROD_ITEM_COMPLETE: and so does the total row");
        ck_eq((uint32_t)dc().events.size(), 0, "PROD_ITEM_COMPLETE: and nothing else is called");
    }

    // Nothing queued for that type, and it is NOT the one currently building: no-op.
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).building_id = 2;
        f.b(PLAYER, BLDG).sub_id      = 2;
        f.b(PLAYER, BLDG).state       = ST_PROD_WORKING;
        sim_view  v                   = f.view();
        sim_store own                 = f.store();
        put_order(f, 0, 0x6f);
        f.order_queue[0].args[0]                      = 3;
        own.production_at(PLAYER, 2).active_unit_type = 9; // != args[0]
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().events.size(), 0,
              "PROD_ITEM_COMPLETE: nothing queued, and not the active build -> nothing");
    }

    // Nothing queued, state != PROD_WORKING: also a no-op, even if active_unit_type happens to match.
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).building_id = 2;
        f.b(PLAYER, BLDG).sub_id      = 2;
        f.b(PLAYER, BLDG).state       = ST_IDLE_NOOP;
        sim_view  v                   = f.view();
        sim_store own                 = f.store();
        put_order(f, 0, 0x6f);
        f.order_queue[0].args[0]                      = 3;
        own.production_at(PLAYER, 2).active_unit_type = 3;
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().events.size(), 0,
              "PROD_ITEM_COMPLETE: state != PROD_WORKING -> nothing, even if the type matches");
    }

    // Nothing queued, IS the active build: the build is cancelled and both callees fire, with the
    // ACTIVE UNIT TYPE (not args[0], though they are equal here by construction) as the argument.
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).building_id = 2;
        f.b(PLAYER, BLDG).sub_id      = 2;
        f.b(PLAYER, BLDG).state       = ST_PROD_WORKING;
        sim_view  v                   = f.view();
        sim_store own                 = f.store();
        put_order(f, 0, 0x6f);
        f.order_queue[0].args[0]                      = 3;
        own.production_at(PLAYER, 2).active_unit_type = 3;

        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());

        ck_eq((uint32_t)f.b(PLAYER, BLDG).state, ST_PROD_PICK_NEXT,
              "PROD_ITEM_COMPLETE: the active build is cancelled -> back to PROD_PICK_NEXT");
        const rec::dc_event *ap = dc().last(rec::DC_llm_unit_apply_production_completion);
        ck(ap != nullptr && ap->a[0] == PLAYER && ap->a[1] == 3,
           "PROD_ITEM_COMPLETE: apply_production_completion(player, active_unit_type)");
        const rec::dc_event *ai = dc().last(rec::DC_llm_strat_ai_notify_unit_lifecycle);
        ck(ai != nullptr && ai->a[0] == PLAYER && ai->a[1] == 3 && ai->a[2] == 0 && ai->a[3] == 2,
           "PROD_ITEM_COMPLETE: notify_unit_lifecycle(player, active_unit_type, 0, 2)");
    }
}

// ---- index 5 (param0 0x74) @0x004694d3: MINE_RESCAN -----------------------------------------------
void test_mine_rescan() {
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).online_state = 0;
        sim_view  v                    = f.view();
        sim_store own                  = f.store();
        put_order(f, 0, 0x74);
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().events.size(), 0, "MINE_RESCAN: online_state == 0 -> nothing");
    }
    {
        sim_fixture f;
        seed_live_building(f);
        sim_view  v   = f.view();
        sim_store own = f.store();
        put_order(f, 0, 0x74);
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().count(rec::DC_llm_bldg_finish_current_order), 1,
              "MINE_RESCAN: the current order finishes");
        ck_eq((uint32_t)f.b(PLAYER, BLDG).state, ST_MINE_SCAN_DEPOSITS,
              "MINE_RESCAN: and the state becomes MINE_SCAN_DEPOSITS");
        ck_eq((uint32_t)dc().events.size(), 1, "MINE_RESCAN: and NOTHING else is called");
    }
}

// ---- index 6 (param0 0x79) @0x00468fc9: HANGAR_RECHARGE -------------------------------------------
void test_hangar_recharge() {
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).online_state = 0;
        sim_view  v                    = f.view();
        sim_store own                  = f.store();
        put_order(f, 0, 0x79);
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().events.size(), 0, "HANGAR_RECHARGE: online_state == 0 -> nothing");
    }
    {
        // Only out of IDLE_NOOP -- any other state, including ones that sound plausible, is a no-op.
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).state = ST_HANGAR_RECHARGE_CHK; // already mid-recharge
        sim_view  v             = f.view();
        sim_store own           = f.store();
        put_order(f, 0, 0x79);
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().events.size(), 0, "HANGAR_RECHARGE: state != IDLE_NOOP -> nothing");
    }
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).state          = ST_IDLE_NOOP;
        f.b(PLAYER, BLDG).cycle_progress = 4.0; // a sentinel the reset must clobber
        sim_view  v                      = f.view();
        sim_store own                    = f.store();
        put_order(f, 0, 0x79);
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)f.b(PLAYER, BLDG).state, ST_HANGAR_RECHARGE_CHK,
              "HANGAR_RECHARGE: IDLE_NOOP -> HANGAR_RECHARGE_CHK");
        ck_eq_d(f.b(PLAYER, BLDG).cycle_progress, 0.0, "HANGAR_RECHARGE: cycle_progress is reset");
        ck_eq((uint32_t)dc().count(rec::DC_llm_strat_refresh_building), 1,
              "HANGAR_RECHARGE: refreshed once");
        ck_eq((uint32_t)dc().count(rec::DC_llm_bldg_finish_current_order), 0,
              "HANGAR_RECHARGE: NOT via finish_current_order -- this arm never calls it");
    }
}

// ---- index 7 (param0 0x7b) @0x004676a9: TURRET_ACQUIRE ---------------------------------------------
//
// The draft's worst divergence in this table (CONCAT22-paired two of six independent args); the
// asm at 0x004677bf-0x00467834 pushes four plain /32 dwords and passes two more in EBX/ECX, so this
// is the arm most worth pinning six DISTINCT values for.
int32_t g_bldg_fx = 0, g_bldg_fy = 0, g_tgt_fx = 0, g_tgt_fy = 0;

void turret_bldg_coords_stub(uint16_t p, int32_t idx, int32_t *ox, int32_t *oy) {
    rec::dc_stub_llm_strat_bldg_get_coords(p, idx, ox, oy);
    // The SAME callee serves both the acting building's own coords AND a BUILDING-kind target's --
    // told apart by which (player, index) it was called with, since the recording stub alone cannot
    // write through the out-pointers at all.
    if (p == (uint16_t)PLAYER && idx == BLDG) {
        *ox = g_bldg_fx;
        *oy = g_bldg_fy;
    } else {
        *ox = g_tgt_fx;
        *oy = g_tgt_fy;
    }
}
void turret_unit_coords_stub(uint16_t p, int32_t idx, int32_t *ox, int32_t *oy) {
    rec::dc_stub_llm_strat_unit_get_coords(p, idx, ox, oy);
    *ox = g_tgt_fx;
    *oy = g_tgt_fy;
}

void test_turret_acquire() {
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).energy = 0.0;
        sim_view  v              = f.view();
        sim_store own            = f.store();
        put_order(f, 0, 0x7b);
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().events.size(), 0, "TURRET_ACQUIRE: energy == 0 exactly -> nothing");
    }
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).online_state = 0;
        sim_view  v                    = f.view();
        sim_store own                  = f.store();
        put_order(f, 0, 0x7b);
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().events.size(), 0, "TURRET_ACQUIRE: online_state == 0 -> nothing");
    }

    // A BUILDING target (args[5] & 0x80 == 0) that does not exist (building_id == 0, the fixture's
    // zero-init default): the arm's OWN coords are still fetched first, but stops there.
    {
        sim_fixture f;
        seed_live_building(f);
        sim_view  v   = f.view();
        sim_store own = f.store();
        put_order(f, 0, 0x7b);
        f.order_queue[0].args[5] = 0x24; // low nibble 4 (target player), 0x80 clear -> BUILDING
        f.order_queue[0].args[6] = 11;

        dc().reset();
        dispatch_calls c            = rec::recording_calls();
        c.llm_strat_bldg_get_coords = &turret_bldg_coords_stub;
        detail::dispatch_building_order(v, own, c, ctx_for());

        ck_eq((uint32_t)dc().count(rec::DC_llm_strat_bldg_get_coords), 1,
              "TURRET_ACQUIRE: the OWN building's coords are fetched unconditionally");
        ck_eq((uint32_t)dc().count(rec::DC_llm_strat_dist_out_of_range), 0,
              "TURRET_ACQUIRE: building_id == 0 (no such building) -> stops before the range check");
    }

    // A BUILDING target that exists but has energy <= 0: also stops before the range check.
    {
        sim_fixture f;
        seed_live_building(f);
        sim_view  v   = f.view();
        sim_store own = f.store();
        put_order(f, 0, 0x7b);
        f.order_queue[0].args[5] = 0x24;
        f.order_queue[0].args[6] = 11;
        f.b(4, 11).building_id   = 1;
        f.b(4, 11).energy        = 0.0;

        dc().reset();
        dispatch_calls c            = rec::recording_calls();
        c.llm_strat_bldg_get_coords = &turret_bldg_coords_stub;
        detail::dispatch_building_order(v, own, c, ctx_for());

        ck_eq((uint32_t)dc().count(rec::DC_llm_strat_dist_out_of_range), 0,
              "TURRET_ACQUIRE: target building energy == 0 -> stops before the range check");
    }

    // A valid BUILDING target, and llm_strat_dist_out_of_range says OUT OF RANGE: nothing commits.
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).sub_id = 3;
        sim_view  v              = f.view();
        sim_store own            = f.store();
        put_order(f, 0, 0x7b);
        f.order_queue[0].args[5]              = 0x24;
        f.order_queue[0].args[6]              = 11;
        f.b(4, 11).building_id                = 1;
        f.b(4, 11).energy                     = 1.0;
        own.turret_at(PLAYER, 3).attack_range = 7;
        g_bldg_fx                             = 320;
        g_bldg_fy                             = 640;
        g_tgt_fx                              = 960;
        g_tgt_fy                              = 1280;

        dc().reset();
        dispatch_calls c                              = rec::recording_calls();
        c.llm_strat_bldg_get_coords                   = &turret_bldg_coords_stub;
        c.llm_strat_unit_get_coords                   = &turret_unit_coords_stub;
        dc().ret[rec::DC_llm_strat_dist_out_of_range] = 1; // out of range
        detail::dispatch_building_order(v, own, c, ctx_for());

        const rec::dc_event *d = dc().last(rec::DC_llm_strat_dist_out_of_range);
        ck(d != nullptr && d->a[0] == 0 && d->a[1] == 7 && d->a[2] == 10 && d->a[3] == 20 &&
               d->a[4] == 30 && d->a[5] == 40,
           "TURRET_ACQUIRE: SIX independent args (0, attack_range, bldg_x/32, bldg_y/32, "
           "target_x/32, target_y/32) -- a CONCAT22-style pairing would disagree with at least one");
        ck_eq((uint32_t)dc().count(rec::DC_llm_strat_bldg_notify_ui), 0,
              "TURRET_ACQUIRE: out of range -> no UI notify");
        ck_eq((uint32_t)own.turret_at(PLAYER, 3).counter_ref, 0,
              "TURRET_ACQUIRE: out of range -> the turret's counter_ref is untouched");
        ck_eq((uint32_t)f.b(PLAYER, BLDG).state, 0,
              "TURRET_ACQUIRE: out of range -> the building's state is untouched (still zero-init)");
    }

    // The happy path: in range -> the counter ref/slot commit, the state becomes TURRET_ATTACK, and
    // the UI is notified. counter_ref is the order's FULL 16-bit args[5] (owner nibble AND the 0x80
    // kind bit both survive the truncation), not just the masked owner nibble used for routing.
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).sub_id = 3;
        sim_view  v              = f.view();
        sim_store own            = f.store();
        put_order(f, 0, 0x7b);
        f.order_queue[0].args[5]              = 0x24;
        f.order_queue[0].args[6]              = 11;
        f.b(4, 11).building_id                = 1;
        f.b(4, 11).energy                     = 1.0;
        own.turret_at(PLAYER, 3).attack_range = 7;
        g_bldg_fx                             = 320;
        g_bldg_fy                             = 640;
        g_tgt_fx                              = 960;
        g_tgt_fy                              = 1280;

        dc().reset();
        dispatch_calls c            = rec::recording_calls();
        c.llm_strat_bldg_get_coords = &turret_bldg_coords_stub;
        c.llm_strat_unit_get_coords = &turret_unit_coords_stub;
        // dist_out_of_range's default recorded return is 0 -- in range.
        detail::dispatch_building_order(v, own, c, ctx_for());

        ck_eq((uint32_t)own.turret_at(PLAYER, 3).counter_ref, 0x24,
              "TURRET_ACQUIRE: counter_ref == the order's full args[5] (0x24), not just the nibble");
        ck_eq((uint32_t)own.turret_at(PLAYER, 3).counter_target_slot, 11,
              "TURRET_ACQUIRE: counter_target_slot == args[6]");
        ck_eq((uint32_t)f.b(PLAYER, BLDG).state, ST_TURRET_ATTACK,
              "TURRET_ACQUIRE: the building's state becomes TURRET_ATTACK");
        const rec::dc_event *nu = dc().last(rec::DC_llm_strat_bldg_notify_ui);
        ck(nu != nullptr && nu->a[0] == PLAYER && nu->a[1] == BLDG,
           "TURRET_ACQUIRE: notify_ui(player, object_index)");
    }

    // A UNIT target (args[5] & 0x80 != 0): the OTHER coord getter is used for the target, and the
    // low-BYTE 0x80 test is what the original TESTs (not merely "args[5] nonzero above 0xf").
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).sub_id = 3;
        sim_view  v              = f.view();
        sim_store own            = f.store();
        put_order(f, 0, 0x7b);
        f.order_queue[0].args[5]              = 0x82; // low nibble 2, 0x80 SET -> UNIT
        f.order_queue[0].args[6]              = 9;
        f.u(2, 9).unit_proto_id               = 5;
        f.u(2, 9).energy                      = 1.0;
        own.turret_at(PLAYER, 3).attack_range = 7;
        g_bldg_fx                             = 320;
        g_bldg_fy                             = 640;
        g_tgt_fx                              = 960;
        g_tgt_fy                              = 1280;

        dc().reset();
        dispatch_calls c            = rec::recording_calls();
        c.llm_strat_bldg_get_coords = &turret_bldg_coords_stub;
        c.llm_strat_unit_get_coords = &turret_unit_coords_stub;
        detail::dispatch_building_order(v, own, c, ctx_for());

        ck_eq((uint32_t)dc().count(rec::DC_llm_strat_unit_get_coords), 1,
              "TURRET_ACQUIRE: 0x80 set -> the UNIT coord getter runs, not the building one (again)");
        ck_eq((uint32_t)dc().count(rec::DC_llm_strat_bldg_get_coords), 1,
              "TURRET_ACQUIRE: -- the SECOND get_coords call is the unit one, not a second building one");
        const rec::dc_event *d = dc().last(rec::DC_llm_strat_dist_out_of_range);
        ck(d != nullptr && d->a[4] == 30 && d->a[5] == 40,
           "TURRET_ACQUIRE: the unit branch's coords reach the range check too");
        ck_eq((uint32_t)own.turret_at(PLAYER, 3).counter_ref, 0x82,
              "TURRET_ACQUIRE: counter_ref carries the 0x80 UNIT bit through to storage");
    }

    // A UNIT target that does not exist (unit_proto_id == 0): stops before the range check.
    {
        sim_fixture f;
        seed_live_building(f);
        sim_view  v   = f.view();
        sim_store own = f.store();
        put_order(f, 0, 0x7b);
        f.order_queue[0].args[5] = 0x82;
        f.order_queue[0].args[6] = 9;
        // f.u(2, 9) is left zero-init: unit_proto_id == 0.

        dc().reset();
        dispatch_calls c            = rec::recording_calls();
        c.llm_strat_bldg_get_coords = &turret_bldg_coords_stub;
        c.llm_strat_unit_get_coords = &turret_unit_coords_stub;
        detail::dispatch_building_order(v, own, c, ctx_for());

        ck_eq((uint32_t)dc().count(rec::DC_llm_strat_dist_out_of_range), 0,
              "TURRET_ACQUIRE: target unit_proto_id == 0 -> stops before the range check");
    }
}

// ---- index 8 (param0 0x7e) @0x00467394: ASSIGN_WORKERS -------------------------------------------
void test_assign_workers() {
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).energy = 0.0;
        sim_view  v              = f.view();
        sim_store own            = f.store();
        put_order(f, 0, 0x7e);
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().events.size(), 0, "ASSIGN_WORKERS: energy == 0 exactly -> nothing");
    }
    {
        // NO online_state gate -- unlike almost every other arm, this one runs even offline.
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).online_state = 0;
        sim_view  v                    = f.view();
        sim_store own                  = f.store();
        put_order(f, 0, 0x7e);
        f.order_queue[0].args[1] = 77;
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().count(rec::DC_llm_strat_bldg_assign_workers), 1,
              "ASSIGN_WORKERS: online_state == 0 does NOT gate this arm");
        const rec::dc_event *e = dc().last(rec::DC_llm_strat_bldg_assign_workers);
        ck(e != nullptr && e->a[0] == PLAYER && e->a[1] == BLDG && e->a[2] == 77,
           "ASSIGN_WORKERS: args[1] passes straight through as the amount");
        ck_eq((uint32_t)dc().events.size(), 1, "ASSIGN_WORKERS: and nothing else is called");
    }
}

// ---- index 9 (param0 0x7f) @0x004673cc: UNASSIGN_WORKERS -----------------------------------------
void test_unassign_workers() {
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).energy = 0.0;
        sim_view  v              = f.view();
        sim_store own            = f.store();
        put_order(f, 0, 0x7f);
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().events.size(), 0, "UNASSIGN_WORKERS: energy == 0 exactly -> nothing");
    }
    {
        // NO online_state gate, and args[1] is cast straight to uint32_t -- a NEGATIVE args[1] (a
        // value put_order's default sentinel already uses everywhere else) wraps rather than clamps
        // to zero, which is what the raw `(uint32_t)o.args[1]` cast in the source actually does.
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).online_state = 0;
        sim_view  v                    = f.view();
        sim_store own                  = f.store();
        put_order(f, 0, 0x7f);
        f.order_queue[0].args[1] = -5;
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().count(rec::DC_llm_strat_bldg_unassign_workers), 1,
              "UNASSIGN_WORKERS: online_state == 0 does NOT gate this arm");
        const rec::dc_event *e = dc().last(rec::DC_llm_strat_bldg_unassign_workers);
        ck(e != nullptr && e->a[0] == PLAYER && e->a[1] == BLDG && (uint32_t)e->a[2] == 0xFFFFFFFBu,
           "UNASSIGN_WORKERS: args[1] == -5 casts straight to (uint32_t)-5, it is not clamped");
        ck_eq((uint32_t)dc().events.size(), 1, "UNASSIGN_WORKERS: and nothing else is called");
    }
}

// ---- index 10 (param0 0x80) @0x00467560: SET_STAFFED ----------------------------------------------
void test_set_staffed() {
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).energy = 0.0;
        sim_view  v              = f.view();
        sim_store own            = f.store();
        put_order(f, 0, 0x80);
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().events.size(), 0, "SET_STAFFED: energy == 0 exactly -> nothing");
    }
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).online_state = 0; // no online gate on this arm either
        sim_view  v                    = f.view();
        sim_store own                  = f.store();
        put_order(f, 0, 0x80);
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        const rec::dc_event *e = dc().last(rec::DC_llm_strat_bldg_set_staffed_flag);
        ck(e != nullptr && e->a[0] == PLAYER && e->a[1] == BLDG,
           "SET_STAFFED: set_staffed_flag(player, object_index), online_state == 0 does not gate it");
        ck_eq((uint32_t)dc().events.size(), 1, "SET_STAFFED: and nothing else is called");
    }
}

// ---- index 11 (param0 0x81) @0x0046758e: CLEAR_STAFFED --------------------------------------------
void test_clear_staffed() {
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).energy = 0.0;
        sim_view  v              = f.view();
        sim_store own            = f.store();
        put_order(f, 0, 0x81);
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().events.size(), 0, "CLEAR_STAFFED: energy == 0 exactly -> nothing");
    }

    // The six exempt types (a port, a shuttle pad, or a mother is never de-staffed this way) -- one
    // human-race and one alien-race representative, since the A_/H_ pairing is a real +0x14 race
    // offset and a translation could drop one race's half.
    for (uint8_t t : {TYPE_A_PORT, TYPE_H_SHUTTLE, TYPE_A_MOTHER}) {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).building_id = 6;
        f.cfg_buildings[6].type       = t;
        sim_view  v                   = f.view();
        sim_store own                 = f.store();
        put_order(f, 0, 0x81);
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().events.size(), 0, "CLEAR_STAFFED: an exempt type -> nothing");
    }

    // A non-exempt type: it clears, and NO online_state gate applies here either.
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).building_id  = 6;
        f.b(PLAYER, BLDG).online_state = 0;
        f.cfg_buildings[6].type        = TYPE_OTHER;
        sim_view  v                    = f.view();
        sim_store own                  = f.store();
        put_order(f, 0, 0x81);
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        const rec::dc_event *e = dc().last(rec::DC_llm_strat_bldg_clear_staffed_flag);
        ck(e != nullptr && e->a[0] == PLAYER && e->a[1] == BLDG,
           "CLEAR_STAFFED: a non-exempt type clears, even offline");
    }
}

// ---- index 12 (param0 0x82) @0x00468aeb: START_UPGRADE ---------------------------------------------
void test_start_upgrade() {
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).energy = 0.0;
        sim_view  v              = f.view();
        sim_store own            = f.store();
        put_order(f, 0, 0x82);
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().events.size(), 0, "START_UPGRADE: energy == 0 exactly -> nothing");
    }
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).online_state = 0;
        sim_view  v                    = f.view();
        sim_store own                  = f.store();
        put_order(f, 0, 0x82);
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().events.size(), 0, "START_UPGRADE: online_state == 0 -> nothing");
    }
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).state = ST_UPGRADING; // already upgrading
        sim_view  v             = f.view();
        sim_store own           = f.store();
        put_order(f, 0, 0x82);
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().events.size(), 0, "START_UPGRADE: already upgrading -> nothing");
    }

    // The "at full charge" test is EQUAL-OR-UNORDERED (fcom_eq_or_unordered), not "less than or
    // equal" and not plain equality via `==` (which would also accept NaN incorrectly on most FPUs,
    // but for the WRONG structural reason -- see the two strict-comparison spelling in the source).
    // Four points on that boundary, checked by whether pay_build_cost gets called at all.
    {
        struct case_t {
            double      energy, cfg_energy;
            const char *what;
            bool        proceeds;
        };
        const case_t cases[] = {
            {40.0, 40.0, "exactly equal -> proceeds", true},
            {39.0, 40.0, "strictly LESS -> does not proceed", false},
            {41.0, 40.0, "strictly GREATER -> does not proceed", false},
            {std::numeric_limits<double>::quiet_NaN(), 40.0, "NaN -> proceeds (unordered)", true},
        };
        for (const case_t &tc : cases) {
            sim_fixture f;
            seed_live_building(f);
            f.b(PLAYER, BLDG).building_id = 9;
            f.b(PLAYER, BLDG).energy      = tc.energy;
            f.cfg_buildings[9].energy     = tc.cfg_energy;
            sim_view  v                   = f.view();
            sim_store own                 = f.store();
            put_order(f, 0, 0x82);
            dc().reset();
            dispatch_calls c = rec::recording_calls();
            detail::dispatch_building_order(v, own, c, ctx_for());
            ck_eq((uint32_t)dc().count(rec::DC_llm_bldg_pay_build_cost), tc.proceeds ? 1u : 0u,
                  (std::string("START_UPGRADE: full-charge test, ") + tc.what).c_str());
        }
    }

    // rc != 0, not the local player: no message, no finish.
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).building_id = 9;
        f.b(PLAYER, BLDG).energy      = 40.0;
        f.cfg_buildings[9].energy     = 40.0;
        sim_view  v                   = f.view();
        sim_store own                 = f.store();
        put_order(f, 0, 0x82);

        dc().reset();
        dispatch_calls c                          = rec::recording_calls();
        dc().ret[rec::DC_llm_bldg_pay_build_cost] = 61;
        detail::dispatch_building_order(v, own, c, ctx_for());

        ck_eq((uint32_t)dc().count(rec::DC_w_sprintf__vss), 0,
              "START_UPGRADE: rc != 0, not the local player -> no message");
        ck_eq((uint32_t)dc().count(rec::DC_llm_bldg_finish_current_order), 0,
              "START_UPGRADE: and no finish");
    }

    // The happy path, USING THE CORRECT INDEX -- upgrade_id, not building_id -- for every
    // builder_count read, in direct contrast with index 1's preserved bug: building_id(9)'s
    // builder_count(99) is deliberately set to a value that would give a wildly different (and
    // wrapped) result if this arm made index 1's mistake.
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).building_id     = 9;
        f.b(PLAYER, BLDG).energy          = 40.0;
        f.b(PLAYER, BLDG).current_workers = 5;
        f.cfg_buildings[9].energy         = 40.0;
        f.cfg_buildings[9].upgrade_index  = 42;
        f.cfg_buildings[9].builder_count  = 99; // NOT what this arm's re-staffing reads
        f.cfg_buildings[42].builder_count = 2;  // the UPGRADE's own builder_count -- what it reads
        sim_view  v                       = f.view();
        sim_store own                     = f.store();
        put_order(f, 0, 0x82);

        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());

        ck_eq((uint32_t)dc().count(rec::DC_llm_bldg_pay_build_cost), 1,
              "START_UPGRADE: pays for the UPGRADE target (upgrade_id), rc == 0 by default");
        const rec::dc_event *pay = dc().last(rec::DC_llm_bldg_pay_build_cost);
        ck(pay != nullptr && pay->a[0] == PLAYER && pay->a[1] == 42,
           "START_UPGRADE: pay_build_cost(player, upgrade_id), not (player, building_id)");
        ck_eq((uint32_t)dc().count(rec::DC_llm_bldg_finish_current_order), 1,
              "START_UPGRADE: rc == 0 -> the order finishes");
        ck_eq((uint32_t)f.b(PLAYER, BLDG).state, ST_UPGRADING,
              "START_UPGRADE: the state becomes UPGRADING");
        ck_eq_d(f.b(PLAYER, BLDG).cycle_progress, 0.0, "START_UPGRADE: cycle progress is reset");

        ck_eq((uint32_t)dc().count(rec::DC_llm_strat_bldg_unassign_workers), 1,
              "START_UPGRADE: the unassign guard fires (5 > builder_count[upgrade_id]=2)");
        const rec::dc_event *un = dc().last(rec::DC_llm_strat_bldg_unassign_workers);
        ck(un != nullptr && (uint32_t)un->a[2] == 3u,
           "START_UPGRADE: amount is 5 - builder_count[upgrade_id=42](2) == 3, NOT "
           "5 - builder_count[building_id=9](99), which would wrap to a huge unsigned value");

        const rec::dc_event *rel = dc().last(rec::DC_llm_strat_ai_queue_release_order);
        ck(rel != nullptr && rel->a[0] == PLAYER && rel->a[1] == BLDG && rel->a[2] == 0,
           "START_UPGRADE: queue_release_order(player, object_index, 0) at the tail -- unique to "
           "this arm among indices 1..13");
        ck_eq((uint32_t)dc().count(rec::DC_llm_strat_refresh_building), 1,
              "START_UPGRADE: refreshed once");
    }
}

// ---- index 13 (param0 0x83) @0x00469535: CANCEL_RESET ----------------------------------------------
//
// The one arm in this half with NO GATE AT ALL -- not even the energy gate every other arm in the
// table shares. Both "gates" every sibling arm has are violated simultaneously here on purpose.
void test_cancel_reset() {
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).energy       = 0.0; // would kill every OTHER arm in this table
        f.b(PLAYER, BLDG).online_state = 0;   // ditto
        sim_view  v                    = f.view();
        sim_store own                  = f.store();
        put_order(f, 0, 0x83);
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        const rec::dc_event *e = dc().last(rec::DC_llm_bldg_finish_current_order);
        ck(e != nullptr && e->a[0] == PLAYER && e->a[1] == BLDG,
           "CANCEL_RESET: finishes the order even at energy == 0 and online_state == 0");
        ck_eq((uint32_t)dc().events.size(), 1, "CANCEL_RESET: and calls nothing else");
    }
    {
        sim_fixture f;
        seed_live_building(f);
        f.b(PLAYER, BLDG).energy = std::numeric_limits<double>::quiet_NaN();
        sim_view  v              = f.view();
        sim_store own            = f.store();
        put_order(f, 0, 0x83);
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_building_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().count(rec::DC_llm_bldg_finish_current_order), 1,
              "CANCEL_RESET: a NaN energy changes nothing -- there is no gate to differ on");
    }
}

} // namespace

void run_dispatch_bldg_tests() {
    test_default_arm();
    test_pay_cycle_upkeep();
    test_restart_construct();
    test_prod_queue_accum();
    test_prod_item_complete();
    test_mine_rescan();
    test_hangar_recharge();
    test_turret_acquire();
    test_assign_workers();
    test_unassign_workers();
    test_set_staffed();
    test_clear_staffed();
    test_start_upgrade();
    test_cancel_reset();
}

} // namespace mh::sim::test
