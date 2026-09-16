//
// sim_dispatch_selftest.cpp -- `simtest` cases for the SPINE of llm_strat_order_queue_dispatch
// @0x00466892: the decode, the loop, the kind router, the retention cursor, and the kind-0xf0
// lockstep body. The 49 table arms are in sim_dispatch_bldg_selftest.cpp and
// sim_dispatch_admin_selftest.cpp; this file covers everything that is NOT an arm.
//
// WHY AN OFFLINE ORACLE IS THE RIGHT INSTRUMENT FOR THIS FUNCTION IN PARTICULAR
//
// The shadow site compares STATE. The single behaviour this function exists to get right --
// retention -- is invisible to that comparison on any run where nothing happens to be retained, and
// an idle or lightly-loaded game retains nothing for thousands of consecutive calls. Worse, the
// failure mode that matters (drain the queue and store 0 instead of compacting) agrees with a clean
// state comparison whenever the retained count was going to be 0 anyway. So the rig can say "ALL
// PAIRS IDENTICAL" about a reimplementation that silently loses every deferred order under load.
//
// Here the queue is a std::vector we fill ourselves, so "three records, the middle one deferred"
// is one line of setup rather than a scenario nobody can arrange on demand.
//
// WHAT THE RECORDING TABLE BUYS. dispatch_calls is 75 function pointers; rec::recording_calls()
// (generated -- sim_dispatch_calls.gen.h) binds every one to a stub that records the call and its
// arguments. That makes three things assertable that no state comparison can see: WHICH callees ran,
// IN WHAT ORDER, and WITH WHAT ARGUMENTS. The lockstep body below is the case in point -- its whole
// contract is a sequence, and its state footprint is two fields.
//
#include "sim/sim_order_dispatch.h"

#include "sim_dispatch_calls.gen.h"
#include "sim_test_support.h"

#include <iterator> // std::size
#include <limits>   // quiet_NaN -- the one input the rig can never send

namespace mh::sim::test {
namespace {

using namespace mh::sim;
using rec::dc;

// ---- opcode -> arm, TRANSCRIBED INDEPENDENTLY ---------------------------------------------------
//
// These two tables are the point of the decode cases, and they are only worth writing because they
// are a SECOND transcription. sim_order_dispatch.h's decode walks the scan bytes read out of the
// image; if a case here re-derived the mapping from those same bytes it would agree with any error
// in them and assert nothing. So the pairs below are copied from the LISTING'S OWN `case_N` LABELS
// (cross-checked against the jump tables and against Ghidra's /llm/E_BLDG_ORDER_PARAM0 and
// /llm/E_ORDER_CODE enums -- three independent sources, 49/49 agreement) and hard-coded.
//
// The enum member each line names is the third check: if the header ever renumbers an arm, the
// member and the literal stop agreeing and this file fails to compile or fails at run time.
struct opcode_arm {
    uint16_t opcode;
    int32_t  arm;
};

// Table A, param0. base 0x6a, 25 real scan bytes, arms 1..25 (0 = default).
// clang-format off
constexpr opcode_arm BLDG_MAP[] = {
    {0x6a,  1}, {0x6b,  2}, {0x6d,  3}, {0x6f,  4}, {0x74,  5}, {0x79,  6}, {0x7b,  7},
    {0x7e,  8}, {0x7f,  9}, {0x80, 10}, {0x81, 11}, {0x82, 12}, {0x83, 13}, {0x89, 14},
    {0xd2, 15}, {0xd3, 16}, {0xd4, 17}, {0xd6, 18}, {0xdb, 19}, {0xdc, 20}, {0xdd, 21},
    {0xde, 22}, {0xdf, 23}, {0xe6, 24}, {0xea, 25},
};
// clang-format on
// Table B, order_code. base 0x34, 22 real scan bytes, arms 1..22 (0 = default).
// clang-format off
constexpr opcode_arm ADMIN_MAP[] = {
    {0x34,  1}, {0x35,  2}, {0xe7,  3}, {0xe8,  4}, {0xe9,  5}, {0xeb,  6}, {0xec,  7}, {0xed,  8},
    {0xee,  9}, {0xef, 10}, {0xf0, 11}, {0xf1, 12}, {0xf2, 13}, {0xf3, 14}, {0xf4, 15}, {0xf5, 16},
    {0xf6, 17}, {0xf7, 18}, {0xf8, 19}, {0xf9, 20}, {0xfa, 21}, {0xfb, 22},
};
// clang-format on
void test_decode_tables() {
    // Every real opcode lands on its own arm. Distinctness matters as much as the values: two
    // opcodes sharing an arm would be a scan-table transcription error, and the map above would
    // still "pass" a per-entry check.
    bool seen[26] = {};
    for (const opcode_arm &e : BLDG_MAP) {
        ck_eq((uint32_t)decode_building_order(e.opcode), (uint32_t)e.arm,
              "decode_building_order: opcode -> the listing's own case_N index");
        ck(!seen[e.arm], "decode_building_order: no two opcodes share an arm");
        seen[e.arm] = true;
    }
    ck_eq((uint32_t)std::size(BLDG_MAP), 25, "table A has 25 real entries (the 26th scan byte is "
                                             "the over-read, not an opcode)");

    bool seen_b[23] = {};
    for (const opcode_arm &e : ADMIN_MAP) {
        ck_eq((uint32_t)decode_admin_order(e.opcode), (uint32_t)e.arm,
              "decode_admin_order: opcode -> the listing's own case_N index");
        ck(!seen_b[e.arm], "decode_admin_order: no two opcodes share an arm");
        seen_b[e.arm] = true;
    }
    ck_eq((uint32_t)std::size(ADMIN_MAP), 22, "table B has 22 real entries");

    // EVERY arm is reachable -- the count above only proves the map is 25 long, not that it covers
    // 1..25. A scan table with a duplicated byte would leave an arm with no opcode and no test.
    for (int32_t k = 1; k <= 25; ++k) ck(seen[k], "decode_building_order: every arm 1..25 is reached");
    for (int32_t k = 1; k <= 22; ++k) ck(seen_b[k], "decode_admin_order: every arm 1..22 is reached");

    // ---- the DEFAULT, and the 16-bit guard --------------------------------------------------
    // Below the base, above base+max_delta, and the two interior gaps -- the guard is
    // `(uint16_t)(value - base) > max_delta`, so a value BELOW the base wraps to a huge unsigned
    // number and is rejected by the same comparison. A translation using a signed difference, or
    // testing `value > base + max` on the raw value, passes the top boundary and fails here.
    ck_eq((uint32_t)decode_building_order(0x69), 0, "param0 one BELOW the base -> default (the "
                                                    "16-bit subtract wraps, it does not go negative)");
    ck_eq((uint32_t)decode_building_order(0x00), 0, "param0 0 -> default");
    ck_eq((uint32_t)decode_building_order(0x6c), 0, "param0 in an interior gap -> default");
    ck_eq((uint32_t)decode_building_order(0xea), 25, "param0 at base+max_delta exactly -> the last arm");
    ck_eq((uint32_t)decode_building_order(0xeb), 0, "param0 one past base+max_delta -> default");
    ck_eq((uint32_t)decode_building_order(0xaf), 0, "the scan's over-read byte is NOT an opcode");

    ck_eq((uint32_t)decode_admin_order(0x33), 0, "order_code one below the base -> default");
    ck_eq((uint32_t)decode_admin_order(0x36), 0, "order_code in the big interior gap -> default");
    ck_eq((uint32_t)decode_admin_order(0xfb), 22, "order_code at the top of the range -> arm 22");
    ck_eq((uint32_t)decode_admin_order(0xfc), 0, "order_code past the top -> default");
    ck_eq((uint32_t)decode_admin_order(0xb3), 0, "the scan's over-read byte is NOT an opcode");
}

// ---- shared setup -------------------------------------------------------------------------------

constexpr int32_t  PLAYER    = 3;
constexpr int32_t  OBJ       = 5;
constexpr uint16_t PROTO     = 7;
constexpr int32_t  ARG_UNSET = -1;

// A record whose four routing inputs are all explicit. `param0` 0x50 is deliberately NOT a table-A
// opcode (it decodes to the default arm) and `order_code` 0xe7 IS a table-B opcode (arm 3,
// DEBUG_ROLL_RANDOM) -- between them every handler leaves a different fingerprint, which is what
// makes the router test below able to tell the four destinations apart.
order make_record(uint32_t kind, int16_t param0, uint16_t order_code) {
    order q{};
    q.owner_and_kind = (uint16_t)(kind | (uint32_t)PLAYER);
    q.unit_index     = (uint16_t)OBJ;
    q.param0         = param0;
    q.order_code     = order_code;
    for (int32_t &a : q.args) a = ARG_UNSET; // -1 = leave the unit's field alone
    return q;
}

// A unit that survives the energy gate and takes the SHORTEST path through the defer decision:
// not boarding (the stub returns 0), not mid-slide in a movement state, and a ground-class proto
// whose type <= 0xe short-circuits the turn-completion gate. So `defer` is false and control
// reaches D -- which is what gives the unit path an unconditional fingerprint
// (llm_unit_set_order_param) for any order_code that is not one of the five the path branches on.
void seed_live_unit(sim_fixture &f) {
    f.u(PLAYER, OBJ)               = unit{};
    f.u(PLAYER, OBJ).energy        = 1.0;
    f.u(PLAYER, OBJ).unit_proto_id = PROTO;
    f.cfg_units[PROTO].type        = 5; // <= 0xe: skips the turn gate
    // Distinct from the unit's state (0) so the two "did the state become the move op" tests in D
    // do not fire and add noise to the call log.
    f.cfg_units[PROTO].move_op_arg  = 0x77;
    f.cfg_units[PROTO].move_op_code = 0x78;
}

// A building that survives the default arm's two gates, so kind 0x40 also has a fingerprint.
void seed_live_building(sim_fixture &f) {
    f.b(PLAYER, OBJ)              = building{};
    f.b(PLAYER, OBJ).energy       = 1.0;
    f.b(PLAYER, OBJ).online_state = 1;
}

// ---- the router: all sixteen kind nibbles --------------------------------------------------------
//
// The draft RESTRUCTURED the original's compare/jump chain into if/else-if, which is the one change
// in this TU that could be wrong for a value no scenario ever sends. `kind` is a nibble, so
// "exhaustive" is sixteen cases -- there is no reason to test a sample.
void test_router_all_kinds() {
    for (uint32_t hi = 0; hi <= 0xf; ++hi) {
        const uint32_t kind = hi << 4;

        sim_fixture f;
        seed_live_unit(f);
        seed_live_building(f);
        sim_view  v         = f.view();
        sim_store own       = f.store();
        f.order_queue[0]    = make_record(kind, 0x50, 0xe7);
        f.order_queue_count = 1;

        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::order_queue_dispatch(v, own, c);

        const int bldg  = dc().count(rec::DC_llm_bldg_finish_current_order);
        const int admin = dc().count(rec::DC_llm_debug_roll_random);
        const int unitp = dc().count(rec::DC_llm_unit_set_order_param);

        if (kind == ORDER_KIND_BLDG) {
            ck(bldg == 1 && admin == 0 && unitp == 0, "router: kind 0x40 -> the BUILDING table");
        } else if (kind == ORDER_KIND_STORAGE || kind == ORDER_KIND_UNIT_EX) {
            // 0x20 and 0x80 land on the SAME code -- the storage/dock path is the unit path reached
            // with a different nibble, and nothing below the router re-tests the kind.
            ck(unitp == 1 && admin == 0 && bldg == 0,
               "router: kinds 0x20 and 0x80 both -> the UNIT path");
        } else if (kind == ORDER_KIND_LOCKSTEP) {
            // param0 is 0x50, not 0xd, so the lockstep body returns without doing anything. The
            // assertion that matters is the NEGATIVE one: order_code 0xe7 is a real admin arm, so
            // if 0xf0 had fallen through to the admin table we would see its call here. That is the
            // exact mis-route the contract header warns about, and this is what would catch it.
            ck(admin == 0 && bldg == 0 && unitp == 0,
               "router: kind 0xf0 -> the LOCKSTEP body, NOT the admin table");
        } else {
            ck(admin == 1 && bldg == 0 && unitp == 0,
               "router: every other kind nibble -> the ADMIN table");
        }
    }
}

// ---- the loop and the compaction cursor ----------------------------------------------------------

// Make a unit that DEFERS without boarding: mid-slide (move_microstep < 0x1f) in a movement state.
// That reaches the retain path and then stops, because the `boarding == 0` test at the end of the
// retain block ends the iteration -- so this is retention with no execution mixed in.
void seed_deferring_unit(sim_fixture &f, int32_t idx) {
    f.u(PLAYER, idx)                = unit{};
    f.u(PLAYER, idx).energy         = 1.0;
    f.u(PLAYER, idx).unit_proto_id  = PROTO;
    f.u(PLAYER, idx).state          = 0x0f; // UNIT_STATE_MOVE_WALKER
    f.u(PLAYER, idx).move_microstep = 3;    // < 0x1f: still sliding
    f.cfg_units[PROTO].type         = 5;
}

void test_queue_is_filtered_not_drained() {
    // ---- nothing at all ------------------------------------------------------------------------
    {
        sim_fixture f;
        sim_view    v   = f.view();
        sim_store   own = f.store();
        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::order_queue_dispatch(v, own, c);
        ck_eq((uint32_t)f.order_queue_count, 0, "empty queue: the count stays 0");
        ck_eq((uint32_t)dc().events.size(), 0, "empty queue: nothing is called");
    }

    // ---- every record droppable ----------------------------------------------------------------
    {
        sim_fixture f;
        seed_live_unit(f);
        sim_view  v   = f.view();
        sim_store own = f.store();
        for (int32_t i = 0; i < 4; ++i) f.order_queue[i] = make_record(ORDER_KIND_UNIT_EX, 0, 0xe7);
        f.order_queue_count = 4;

        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::order_queue_dispatch(v, own, c);
        ck_eq((uint32_t)f.order_queue_count, 0, "four executed records: the count ends at 0");
        ck_eq((uint32_t)dc().count(rec::DC_llm_unit_set_order_param), 4,
              "four records: every one of them was executed, not just the first");
    }

    // ---- THE CASE THE RIG CANNOT ARRANGE: one deferred record among four ------------------------
    //
    // This is the whole function. The record at slot 2 defers; the other three execute and drop.
    // A drain-and-store-zero reimplementation passes every other case in this file and fails here.
    {
        sim_fixture f;
        seed_live_unit(f);         // unit OBJ executes
        seed_deferring_unit(f, 9); // unit 9 defers
        sim_view  v   = f.view();
        sim_store own = f.store();

        for (int32_t i = 0; i < 4; ++i) f.order_queue[i] = make_record(ORDER_KIND_UNIT_EX, 0, 0xe7);
        // The deferring record is the third one, and carries a value we can recognise after it moves.
        f.order_queue[2].unit_index = 9;
        f.order_queue[2].param0     = 0x5a5a;
        f.order_queue_count         = 4;

        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::order_queue_dispatch(v, own, c);

        ck_eq((uint32_t)f.order_queue_count, 1,
              "one deferred record among four: the count is the CURSOR (1), not the walked count "
              "(4) and not 0");
        ck_eq((uint32_t)f.order_queue[0].unit_index, 9,
              "the retained record was COMPACTED to slot 0 -- the whole record, not just the count");
        ck_eq((uint32_t)(uint16_t)f.order_queue[0].param0, 0x5a5a,
              "the compaction copied the record's payload, not a fresh one");
        ck_eq((uint32_t)f.u(PLAYER, 9).order_queued, 1,
              "the deferred unit is flagged order_queued BEFORE the record is copied");
        ck_eq((uint32_t)dc().count(rec::DC_llm_unit_set_order_param), 3,
              "the other three were executed; the deferred one was NOT");
    }

    // ---- a record already at the cursor is not self-copied --------------------------------------
    {
        sim_fixture f;
        seed_deferring_unit(f, 9);
        sim_view  v                 = f.view();
        sim_store own               = f.store();
        f.order_queue[0]            = make_record(ORDER_KIND_UNIT_EX, 0x1234, 0xe7);
        f.order_queue[0].unit_index = 9;
        f.order_queue_count         = 1;

        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::order_queue_dispatch(v, own, c);
        ck_eq((uint32_t)f.order_queue_count, 1, "a single deferred record: kept == 1");
        ck_eq((uint32_t)(uint16_t)f.order_queue[0].param0, 0x1234,
              "kept == slot: the record is left exactly where it was");
    }

    // ---- ALL records deferred: the queue is unchanged -------------------------------------------
    {
        sim_fixture f;
        seed_deferring_unit(f, 9);
        sim_view  v   = f.view();
        sim_store own = f.store();
        for (int32_t i = 0; i < 3; ++i) {
            f.order_queue[i]            = make_record(ORDER_KIND_UNIT_EX, (int16_t)(0x100 + i), 0xe7);
            f.order_queue[i].unit_index = 9;
        }
        f.order_queue_count = 3;

        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::order_queue_dispatch(v, own, c);
        ck_eq((uint32_t)f.order_queue_count, 3, "three deferred records: all three survive");
        for (int32_t i = 0; i < 3; ++i)
            ck_eq((uint32_t)(uint16_t)f.order_queue[i].param0, (uint32_t)(0x100 + i),
                  "and they keep their original order");
    }

    // ---- the energy gate DROPS the record, it does not retain it --------------------------------
    // A dead unit's order is skipped by `continue` BEFORE the retain path, so the cursor does not
    // move: the record is lost, which is the original's behaviour.
    {
        sim_fixture f;
        seed_deferring_unit(f, 9);
        f.u(PLAYER, 9).energy       = 0.0; // exactly zero -- the gate is `<= 0`, not `< 0`
        sim_view  v                 = f.view();
        sim_store own               = f.store();
        f.order_queue[0]            = make_record(ORDER_KIND_UNIT_EX, 0, 0xe7);
        f.order_queue[0].unit_index = 9;
        f.order_queue_count         = 1;

        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::order_queue_dispatch(v, own, c);
        ck_eq((uint32_t)f.order_queue_count, 0,
              "energy == 0 exactly: the record is DROPPED, not deferred");
        ck_eq((uint32_t)dc().events.size(), 0, "and nothing was called for it");
    }

    // ---- A NaN ENERGY RUNS THE BODY -------------------------------------------------------------
    // The x87 idiom is FLDZ; FCOMP energy; FNSTSW AX; SAHF; JNC, and C0 -> CF is set for "less
    // than" AND for "unordered". `!(energy <= 0.0)` has that truth table; `0.0 < energy` -- which is
    // what the Ghidra decompile shows -- differs on exactly this input and nothing else. There is no
    // way to reach this case from the rig; it is why the offline oracle is worth having.
    {
        sim_fixture f;
        seed_live_unit(f);
        f.u(PLAYER, OBJ).energy = std::numeric_limits<double>::quiet_NaN();
        sim_view  v             = f.view();
        sim_store own           = f.store();
        f.order_queue[0]        = make_record(ORDER_KIND_UNIT_EX, 0, 0xe7);
        f.order_queue_count     = 1;

        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::order_queue_dispatch(v, own, c);
        ck_eq((uint32_t)dc().count(rec::DC_llm_unit_set_order_param), 1,
              "a NaN energy RUNS the body (SAHF sets CF for unordered as well as for less-than)");
    }
}

// ---- the loop bound is RE-READ each iteration ----------------------------------------------------
//
// `CMP EAX,[0x005d0194]` sits inside the loop head, so a handler that changes the count mid-walk
// changes how far the walk goes. Nothing in the fixture can do that by itself, so the test overrides
// ONE member of the calls table with a stub that shortens the queue -- which is the reason
// recording_calls() returns the table by value rather than handing out a singleton.
sim_fixture *g_shrink_target = nullptr;

// IT MUST STILL RECORD. Overriding a member with a stub that only does the extra thing silently
// removes that callee from the log, so `count(...) == 1` becomes unreachable and the case fails --
// or, worse for a differently-shaped assertion, `count(...) == 0` passes for the wrong reason. Chain
// to the generated stub first, then do the extra work.
void shrink_the_queue() {
    rec::dc_stub_llm_debug_roll_random();
    if (g_shrink_target != nullptr) g_shrink_target->order_queue_count = 1;
}

void test_loop_bound_is_reread() {
    sim_fixture f;
    seed_live_unit(f);
    sim_view  v   = f.view();
    sim_store own = f.store();
    for (int32_t i = 0; i < 5; ++i) f.order_queue[i] = make_record(0x00, 0, 0xe7); // admin arm 3
    f.order_queue_count = 5;

    g_shrink_target = &f;
    dc().reset();
    dispatch_calls c        = rec::recording_calls();
    c.llm_debug_roll_random = &shrink_the_queue; // arm 3's only call, on the first record
    detail::order_queue_dispatch(v, own, c);
    g_shrink_target = nullptr;

    // The first record's handler set the count to 1, so the walk stops after it. A loop that
    // snapshotted the bound at entry would run all five.
    ck_eq((uint32_t)dc().count(rec::DC_llm_debug_roll_random), 1,
          "the loop bound is RE-READ each iteration: a handler that shortens the queue stops the "
          "walk");
    ck_eq((uint32_t)f.order_queue_count, 0,
          "and the tail still stores the cursor (0 retained), overwriting what the handler wrote");
}

// ---- the kind-0xf0 lockstep body -----------------------------------------------------------------
//
// Small, and the highest-consequence body in the three TUs: it latches a flag, publishes a horizon
// and SENDS A PACKET on the lockstep path. Its contract is a SEQUENCE, which is precisely what a
// state comparison cannot check and what the recording table can.
void test_lockstep_extend() {
    // ---- the guard on param0 -------------------------------------------------------------------
    {
        sim_fixture f;
        sim_view    v        = f.view();
        sim_store   own      = f.store();
        f.order_queue[0]     = make_record(ORDER_KIND_LOCKSTEP, 0x0c, 0); // 0x0c, not 0x0d
        f.order_queue_count  = 1;
        f.game_clock         = 100.0;
        f.lockstep_step_size = 4.0;

        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::order_queue_dispatch(v, own, c);
        ck_eq((uint32_t)dc().events.size(), 0, "lockstep: param0 != 0xd does nothing at all");
        ck_eq_d(f.lockstep_horizon, 0.0, "lockstep: and does not publish a horizon");
    }

    // ---- the latch: already extended -----------------------------------------------------------
    {
        sim_fixture f;
        sim_view    v        = f.view();
        sim_store   own      = f.store();
        f.order_queue[0]     = make_record(ORDER_KIND_LOCKSTEP, 0x0d, 0);
        f.order_queue_count  = 1;
        f.net_lockstep_flags = 0x40; // the bit is already set
        f.game_clock         = 100.0;
        f.lockstep_step_size = 4.0;

        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::order_queue_dispatch(v, own, c);
        ck_eq((uint32_t)dc().events.size(), 0,
              "lockstep: the 0x40 latch suppresses a SECOND extension in the same window");
        ck_eq_d(f.lockstep_horizon, 0.0, "lockstep: and no horizon is published");
    }

    // ---- the happy path, including the ORDER of the two calls ----------------------------------
    {
        sim_fixture f;
        sim_view    v       = f.view();
        sim_store   own     = f.store();
        f.order_queue[0]    = make_record(ORDER_KIND_LOCKSTEP, 0x0d, 0);
        f.order_queue_count = 1;
        // Exactly representable, and not equal to each other or to their sum's neighbours, so a
        // draft that used the wrong operand or added the horizon to itself gives a different number.
        f.game_clock         = 1024.0;
        f.lockstep_step_size = 0.25;
        f.net_lockstep_flags = 0x81; // unrelated bits, to prove the OR does not clobber them
        // The extend_ui_enter SPLIT (LIFT-SCREEN): the two mode stores are this body's now, so the
        // fixture has to carry a distinguishable starting mode -- 2 is the live in-game value, and
        // it is neither of the two the split writes.
        f.game_mode   = 2;
        f.player_side = 3;

        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::order_queue_dispatch(v, own, c);

        ck_eq((uint32_t)dc().count(rec::DC_llm_net_lockstep_wait_player_overlay_show), 1,
              "lockstep: the wait overlay is requested exactly once");
        ck_eq((uint32_t)dc().count(rec::DC_ovl_mode_saved_set), 1,
              "lockstep: the saved-mode store happens exactly once");
        ck_eq((uint32_t)dc().count(rec::DC_llm_net_send_lockstep_extend), 1,
              "lockstep: the extension is sent exactly once");
        ck(dc().first_index(rec::DC_ovl_mode_saved_set) <
               dc().first_index(rec::DC_llm_net_lockstep_wait_player_overlay_show),
           "lockstep: the saved-mode store comes BEFORE the overlay (it reads the pre-overlay mode)");
        ck(dc().first_index(rec::DC_llm_net_lockstep_wait_player_overlay_show) <
               dc().first_index(rec::DC_llm_net_send_lockstep_extend),
           "lockstep: the overlay request comes BEFORE the send");

        const rec::dc_event *saved = dc().last(rec::DC_ovl_mode_saved_set);
        ck(saved != nullptr && saved->na == 1 && saved->a[0] == 2,
           "lockstep: what is SAVED is the mode from before the split, not the 8 it ends on");
        const rec::dc_event *ovl = dc().last(rec::DC_llm_net_lockstep_wait_player_overlay_show);
        ck(ovl != nullptr && ovl->na == 1 && ovl->a[0] == 3,
           "lockstep: the overlay names PlayerSide, the player the original passed");
        ck_eq((uint32_t)f.game_mode, 8u, "lockstep: the split leaves GAME_MODE at 8");
        ck_eq_d(f.lockstep_horizon, 1024.25,
                "lockstep: the horizon is game_clock + lockstep_step_size");

        const rec::dc_event *sent = dc().last(rec::DC_llm_net_send_lockstep_extend);
        ck(sent != nullptr && sent->nd == 1 && sent->d[0] == 1024.25,
           "lockstep: the value SENT is the same horizon that was stored, not a recomputation");
        ck_eq((uint32_t)f.net_lockstep_flags, 0xc1,
              "lockstep: the latch bit is OR'd in, leaving the other flag bits alone");
    }
}

// ---- the rig's work indicator, checked here rather than only in a rig log ----------------------
//
// `tally_coverage` is what turns an armed run's "calls=100000 divergences=0" into a claim about
// WHICH of the fifty arms were actually compared, and SIM1C's acceptance test asks for exactly that.
// An instrument that silently reports nothing looks identical to a weak scenario, so it is pinned
// here against records whose expected masks are written out by hand.
void test_coverage_tally() {
    dispatch_coverage cov{};

    order q{};
    q.owner_and_kind = (uint16_t)(ORDER_KIND_BLDG | 3u);
    q.param0         = 0x6a; // table A arm 1
    tally_coverage(q, cov);
    ck_eq(cov.kinds, 1u << 4, "coverage: kind 0x40 sets bit 4");
    ck_eq(cov.bldg_arms, 1u << 1, "coverage: param0 0x6a sets BUILDING arm 1");
    ck_eq(cov.admin_arms, 0, "coverage: and touches no admin arm");

    q.param0 = 0x50; // not an opcode -> the default arm, which has a real body
    tally_coverage(q, cov);
    ck_eq(cov.bldg_arms, (1u << 1) | 1u,
          "coverage: an unrecognised param0 sets bit 0 -- the DEFAULT ARM, which is where the "
          "dispatcher really sends it (bits are ARMS, not opcodes)");

    // The catch-all: every kind the router does not recognise decodes through table B.
    cov              = dispatch_coverage{};
    q.owner_and_kind = (uint16_t)(0x00u | 3u);
    q.order_code     = 0xe7; // table B arm 3
    tally_coverage(q, cov);
    ck_eq(cov.kinds, 1u, "coverage: kind 0x00 sets bit 0");
    ck_eq(cov.admin_arms, 1u << 3, "coverage: order_code 0xe7 sets ADMIN arm 3");
    ck_eq(cov.bldg_arms, 0, "coverage: and touches no building arm");

    // THE THREE KINDS THAT ARE NOT TABLE ARMS. Folding these into the admin mask is the mistake that
    // would make a run look like it had covered arms it never reached -- kind 0xf0 in particular
    // decodes to admin arm 11, a real handler the dispatcher never runs for it.
    for (uint32_t kind : {ORDER_KIND_STORAGE, ORDER_KIND_UNIT_EX, ORDER_KIND_LOCKSTEP}) {
        dispatch_coverage k{};
        order             r{};
        r.owner_and_kind = (uint16_t)(kind | 3u);
        r.order_code     = 0xf0; // a real admin opcode, deliberately
        r.param0         = 0x6a; // and a real building opcode
        tally_coverage(r, k);
        ck_eq(k.kinds, 1u << (kind >> 4), "coverage: the kind bit is set");
        ck_eq(k.admin_arms, 0,
              "coverage: unit/storage/lockstep records claim NO admin arm, even carrying an "
              "order_code that is one");
        ck_eq(k.bldg_arms, 0, "coverage: and no building arm");
    }

    // Accumulation across records -- the masks are a union over the whole run, not the last call.
    cov              = dispatch_coverage{};
    q.owner_and_kind = (uint16_t)(0x00u | 3u);
    for (uint16_t oc : {(uint16_t)0x34, (uint16_t)0xfb, (uint16_t)0x00}) {
        q.order_code = oc;
        tally_coverage(q, cov);
    }
    ck_eq(cov.admin_arms, (1u << 1) | (1u << 22) | 1u,
          "coverage: the masks accumulate (arms 1, 22 and the default)");
}

} // namespace

// ---- D18: the observer the order RECORDER rides in on when this entry is promoted ---------------
//
// The recorder used to be a run-before trampoline on this function's entry. Under `[promote] orders`
// -- the shipping default -- it could not arm, so order recording was silently unavailable in every
// default run. That is the D17 shape applied to an INSTRUMENT rather than a fix, and it is worse in
// one specific way: a displaced fix changes behaviour, while a displaced instrument yields an empty
// measurement that still looks like a measurement.
//
// These cases assert the observer STEP -- that what is registered is what gets called, once per
// dispatch, and that clearing works. What they cannot reach is the promoted entry itself, which goes
// through `state()` and needs a live image; that last link is one line of code and is closed by the
// rig instead, which either produces a recording under `[promote] orders` or does not. Saying so
// rather than letting these cases imply end-to-end coverage is the D17 lesson applied to its own fix.
// Deliberately NOT a test of the recorder: that writes a file and belongs to the harness.
namespace {
int  g_observer_hits = 0;
void count_observer_hit() { ++g_observer_hits; }
} // namespace

void test_dispatch_observer_wiring() {
    // Default state: nothing registered. Asserted rather than assumed, because "the observer fired"
    // means nothing if a stale registration from another case is what fired it.
    set_dispatch_observer(nullptr);
    ck(dispatch_observer() == nullptr, "D18: no observer is registered by default");

    set_dispatch_observer(&count_observer_hit);
    ck(dispatch_observer() == &count_observer_hit, "D18: the registered observer is what comes back");

    g_observer_hits = 0;
    fire_dispatch_observer();
    ck(g_observer_hits == 1, "D18: the dispatch entry's observer step calls what is registered");
    fire_dispatch_observer();
    ck(g_observer_hits == 2, "D18: it fires once per dispatch, not once per run");

    // Clearing has to work, or a test run leaks a registration into the next one -- and in the DLL a
    // stale observer would outlive the configuration that wanted it.
    set_dispatch_observer(nullptr);
    g_observer_hits = 0;
    fire_dispatch_observer();
    ck(g_observer_hits == 0, "D18: a cleared observer is not called");
    ck(dispatch_observer() == nullptr, "D18: dispatch still runs with no observer registered");
}

void run_dispatch_spine_tests() {
    test_dispatch_observer_wiring();
    test_decode_tables();
    test_coverage_tally();
    test_router_all_kinds();
    test_queue_is_filtered_not_drained();
    test_loop_bound_is_reread();
    test_lockstep_extend();
}

} // namespace mh::sim::test
