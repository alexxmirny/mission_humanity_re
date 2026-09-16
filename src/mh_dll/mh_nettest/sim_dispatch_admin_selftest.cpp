//
// sim_dispatch_admin_selftest.cpp -- `simtest` cases for the 23 ADMIN arms of
// llm_strat_order_queue_dispatch, i.e. for dispatch_admin_order in
// libmh/sim/sim_order_dispatch_admin.cpp.
//
// The kind-0xf0 lockstep body lives in the same translation unit but is NOT tested here: it is not
// an arm of this table (the router jumps straight to it) and its cases are in
// sim_dispatch_selftest.cpp with the rest of the spine.
//
// Read sim_dispatch_selftest.cpp first for the recording table and the shared idioms.
//
// THE ADMIN TABLE IS THE CATCH-ALL. Every kind nibble the router does not recognise lands here, so
// these arms run far more often than the building ones and their opcode is the ONLY thing selecting
// between them.
//
#include "sim/sim_order_dispatch.h"

#include "sim_dispatch_calls.gen.h"
#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;
using rec::dc;

constexpr int32_t PLAYER = 3;
constexpr int32_t OBJ    = 5;

// The admin path is reached with whatever kind the record carried; nothing in the table re-tests it,
// so the cases use 0x00 (one of the thirteen nibbles that route here).
dispatch_ctx ctx_for(int32_t slot = 0) {
    dispatch_ctx x;
    x.slot         = slot;
    x.player       = (uint32_t)PLAYER;
    x.kind         = 0x00u;
    x.object_index = OBJ;
    return x;
}

void put_order(sim_fixture &f, int32_t slot, uint16_t order_code) {
    order q{};
    q.owner_and_kind = (uint16_t)PLAYER;
    q.unit_index     = (uint16_t)OBJ;
    q.order_code     = order_code;
    for (int32_t &a : q.args) a = -1;
    f.order_queue[slot] = q;
}

// ---- the default arm is EMPTY, and that is a behaviour worth pinning ----------------------------
//
// Unlike table A's, this table's index 0 is a jump straight to the loop tail. An unrecognised admin
// opcode must therefore do NOTHING -- not fall into arm 1, not log, not touch the record. A
// switch whose default accidentally shared a body with a neighbour would be invisible on the rig
// (the opcode never occurs in a normal game) and wrong the first time a mod sent one.
void test_default_arm_is_empty() {
    const uint16_t unrecognised[] = {0x00, 0x33, 0x36, 0xb3, 0xfc, 0xffff};
    for (uint16_t oc : unrecognised) {
        sim_fixture f;
        sim_view    v   = f.view();
        sim_store   own = f.store();
        put_order(f, 0, oc);

        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_admin_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().events.size(), 0,
              "admin default arm: an unrecognised order_code does NOTHING");
    }
}

// ---- arm 3, DEBUG_ROLL_RANDOM -- the simplest real arm ------------------------------------------
// One unconditional call and no state. It is also the fingerprint the router test next door uses to
// prove a record reached the admin table, so it is worth pinning here in its own right.
void test_debug_roll_random() {
    sim_fixture f;
    sim_view    v   = f.view();
    sim_store   own = f.store();
    put_order(f, 0, 0xe7);

    dc().reset();
    dispatch_calls c = rec::recording_calls();
    detail::dispatch_admin_order(v, own, c, ctx_for());
    ck_eq((uint32_t)dc().count(rec::DC_llm_debug_roll_random), 1,
          "admin arm 3 (0xe7): rolls the debug random exactly once");
    ck_eq((uint32_t)dc().events.size(), 1, "admin arm 3: and does nothing else");
}

// ---- arms 1, 2, 4-22 -----------------------------------------------------------------------------
//
// Read against tmp/decomp_sim/dispatch_admin.asm (case_1..case_22, 0x00469782-0x0046997e), the same
// listing sim_order_dispatch_admin.cpp's own per-arm comments were transcribed from. Two of the
// global facts this batch was briefed on do NOT apply to this table, and it is worth saying why
// rather than silently skipping them:
//
//   FACT 2 (NaN energy gates): none of the 23 admin arms reads an `energy` field at all -- the
//   FLDZ;FCOMP;JNC idiom belongs to the BUILDING table's default arm (case_0, 0x00469545) and to
//   several of its numbered arms, none of which are in this file.
//
//   FACT 3 (a guard on args[5]/[10] protecting a store of args[6]/[11]): scanning every case_N
//   block in the admin listing, the highest arg index ANY admin arm reads is args[5] (case_6,
//   UNIT_CREATE) and no arm reads args[6], args[10] or args[11] at all -- the mismatch this project
//   was bitten by once lives in the building table (sim_order_dispatch_bldg.cpp), not here.
//
// A second helper, distinct from ctx_for(), is needed for the truncation cases: several arms cast
// x.player down to a narrower callee parameter (uint16_t in case_6, uint8_t in case_16's two calls,
// uint16_t in case_7's ADD half), and ctx_for() always hands back the fixed PLAYER=3 -- too small a
// value to ever exercise a truncation. This variant seeds whatever player value a case needs,
// including one with high bits set.
dispatch_ctx ctx_for_player(uint32_t player, int32_t object_index = OBJ, int32_t slot = 0) {
    dispatch_ctx x;
    x.slot         = slot;
    x.player       = player;
    x.kind         = 0x00u;
    x.object_index = object_index;
    return x;
}

// ---- arms 1 and 2, UNIT_STATUS_BIT_SET / UNIT_STATUS_BIT_CLEAR (0x34 / 0x35) --------------------
// case_1 @0x00469782 / case_2 @0x0046979c: `MOV EBX,args[0]` puts the FULL 32-bit record field into
// the register the call site loads, but the callee's committed parameter is `uint8_t` -- the
// narrowing happens at the boundary the C++ models with `(uint8_t)rec.args[0]`. High bits set on
// args[0] is exactly the input the rig cannot manufacture (a normal enqueuer never sets them), and
// it is the one input that tells a widened translation from a correctly truncated one apart.
void test_unit_status_bit_set_clear() {
    // arm 1: SET
    {
        sim_fixture f;
        sim_view    v   = f.view();
        sim_store   own = f.store();
        put_order(f, 0, 0x34);
        f.order_queue[0].args[0] = 0x77aa1188; // low byte 0x88; high bits must be dropped

        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_admin_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().count(rec::DC_llm_unit_status_bit_set), 1,
              "admin arm 1 (0x34): sets the status bit exactly once");
        const rec::dc_event *e = dc().last(rec::DC_llm_unit_status_bit_set);
        ck(e != nullptr && e->a[0] == PLAYER && e->a[1] == OBJ && e->a[2] == 0x88,
           "admin arm 1: (player, object_index, args[0] truncated to uint8_t -- 0x77aa1188 -> 0x88)");
        ck_eq((uint32_t)dc().events.size(), 1, "admin arm 1: and does nothing else");
    }
    // arm 2: CLEAR -- a different low byte, so a translation that called SET for both opcodes (or
    // vice versa) disagrees with one of the two cases.
    {
        sim_fixture f;
        sim_view    v   = f.view();
        sim_store   own = f.store();
        put_order(f, 0, 0x35);
        f.order_queue[0].args[0] = 0x11223399; // low byte 0x99

        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_admin_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().count(rec::DC_llm_unit_status_bit_clear), 1,
              "admin arm 2 (0x35): clears the status bit exactly once");
        const rec::dc_event *e = dc().last(rec::DC_llm_unit_status_bit_clear);
        ck(e != nullptr && e->a[0] == PLAYER && e->a[1] == OBJ && e->a[2] == 0x99,
           "admin arm 2: (player, object_index, args[0] truncated to uint8_t -- 0x11223399 -> 0x99)");
        ck_eq((uint32_t)dc().events.size(), 1, "admin arm 2: and does nothing else");
        ck_eq((uint32_t)dc().count(rec::DC_llm_unit_status_bit_set), 0,
              "admin arm 2: and NOT the SET arm");
    }
}

// ---- arms 4 and 5, GAME_SPEED_INCREASE / GAME_SPEED_DECREASE (0xe8 / 0xe9) -----------------------
// case_4 @0x004697c0 / case_5 @0x004697ce: `MOVZX EAX,[EBP-0x34]` then a single call -- player only,
// no record field at all. ctx_for() always hands back PLAYER=3, which would not catch a handler that
// ignored x.player and called with a hardcoded constant, so this uses a distinct player value.
void test_game_speed() {
    {
        sim_fixture f;
        sim_view    v   = f.view();
        sim_store   own = f.store();
        put_order(f, 0, 0xe8);

        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_admin_order(v, own, c, ctx_for_player(0x0b));
        ck_eq((uint32_t)dc().count(rec::DC_llm_game_speed_increase), 1,
              "admin arm 4 (0xe8): increases game speed exactly once");
        const rec::dc_event *e = dc().last(rec::DC_llm_game_speed_increase);
        ck(e != nullptr && e->a[0] == 0x0b, "admin arm 4: the player IS x.player, not a constant");
        ck_eq((uint32_t)dc().events.size(), 1, "admin arm 4: and does nothing else");
    }
    {
        sim_fixture f;
        sim_view    v   = f.view();
        sim_store   own = f.store();
        put_order(f, 0, 0xe9);

        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_admin_order(v, own, c, ctx_for_player(0x0c));
        ck_eq((uint32_t)dc().count(rec::DC_llm_game_speed_decrease), 1,
              "admin arm 5 (0xe9): decreases game speed exactly once");
        const rec::dc_event *e = dc().last(rec::DC_llm_game_speed_decrease);
        ck(e != nullptr && e->a[0] == 0x0c, "admin arm 5: the player IS x.player, not a constant");
        ck_eq((uint32_t)dc().events.size(), 1, "admin arm 5: and does nothing else");
        ck_eq((uint32_t)dc().count(rec::DC_llm_game_speed_increase), 0,
              "admin arm 5: and NOT the increase arm");
    }
}

// ---- arm 6, UNIT_CREATE (0xeb) --------------------------------------------------------------------
// case_6 @0x004696c5: `CMP Unit[args[1]].type,0xe / JG` picks the branch -- type <= 0xe (JG not
// taken, falls straight into the block at 0x004696dc) -> llm_unit_create_soldier; type > 0xe (JG
// taken to 0x0046970c) -> llm_strat_unit_create. Both branches push the SAME five values in the SAME
// order: x=args[4], y=args[5], unit=args[1], player (truncated to uint16_t), is_ship=1 (the literal
// EAX=1 pushed before either call). Each case below sits exactly ON the boundary (0xe and 0xf) so a
// translation with `>=` instead of `>` disagrees with one of the two.
void test_unit_create() {
    constexpr uint16_t PROTO_SOLDIER = 12;
    constexpr uint16_t PROTO_SHIP    = 40;

    // type == 0xe exactly: JG is NOT taken (0xe is not > 0xe) -> the soldier branch.
    {
        sim_fixture f;
        f.cfg_units[PROTO_SOLDIER].type = 0xe;
        sim_view  v                     = f.view();
        sim_store own                   = f.store();
        put_order(f, 0, 0xeb);
        f.order_queue[0].args[1] = PROTO_SOLDIER;
        f.order_queue[0].args[4] = -3;  // x -- negative, so also exercises the uint32_t
                                        // reinterpretation the call boundary performs
        f.order_queue[0].args[5] = 777; // y

        dc().reset();
        dispatch_calls c = rec::recording_calls();
        // High bits on the player on top of the boundary type -- proves the uint16_t truncation and
        // the type gate are independent of each other.
        detail::dispatch_admin_order(v, own, c, ctx_for_player(0x9abc0007));
        ck_eq((uint32_t)dc().count(rec::DC_llm_unit_create_soldier), 1,
              "admin arm 6 (0xeb), type==0xe: creates via the SOLDIER path exactly once");
        ck_eq((uint32_t)dc().count(rec::DC_llm_strat_unit_create), 0,
              "admin arm 6, type==0xe: and NOT the strat-create path");
        const rec::dc_event *e = dc().last(rec::DC_llm_unit_create_soldier);
        ck(e != nullptr && (uint32_t)e->a[0] == 0xFFFFFFFDu && e->a[1] == 777 &&
               e->a[2] == PROTO_SOLDIER && e->a[3] == 0x0007 && e->a[4] == 1,
           "admin arm 6, type==0xe: (x=args[4] as uint32_t, y=args[5], unit=args[1], "
           "player truncated to uint16_t -- 0x9abc0007 -> 0x0007, is_ship=1)");
        ck_eq((uint32_t)dc().events.size(), 1, "admin arm 6, type==0xe: and does nothing else");
    }
    // type == 0xf: JG IS taken (0xf > 0xe) -> the strat-create branch. x and y swapped relative to
    // the case above, so a translation that reused one branch's operand order for the other fails.
    {
        sim_fixture f;
        f.cfg_units[PROTO_SHIP].type = 0xf;
        sim_view  v                  = f.view();
        sim_store own                = f.store();
        put_order(f, 0, 0xeb);
        f.order_queue[0].args[1] = PROTO_SHIP;
        f.order_queue[0].args[4] = 555;
        f.order_queue[0].args[5] = -9;

        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_admin_order(v, own, c, ctx_for_player(0x1234000c));
        ck_eq((uint32_t)dc().count(rec::DC_llm_strat_unit_create), 1,
              "admin arm 6, type==0xf: creates via the STRAT path exactly once");
        ck_eq((uint32_t)dc().count(rec::DC_llm_unit_create_soldier), 0,
              "admin arm 6, type==0xf: and NOT the soldier path");
        const rec::dc_event *e = dc().last(rec::DC_llm_strat_unit_create);
        ck(e != nullptr && e->a[0] == 555 && (uint32_t)e->a[1] == 0xFFFFFFF7u &&
               e->a[2] == PROTO_SHIP && e->a[3] == 0x000c && e->a[4] == 1,
           "admin arm 6, type==0xf: (x=args[4], y=args[5] as uint32_t, unit=args[1], "
           "player truncated to uint16_t -- 0x1234000c -> 0x000c, is_ship=1)");
        ck_eq((uint32_t)dc().events.size(), 1, "admin arm 6, type==0xf: and does nothing else");
    }
}

// ---- arm 7, POPULATION_ADD_REMOVE (0xec) ----------------------------------------------------------
// case_7 @0x00469835: `CMP args[1],0 / JZ skip` first; then `JLE` on the SAME field picks remove
// (<=0, and 0 is already excluded so this is really <0) vs the fallthrough add (>0). The guard slot
// and the store slot are the same field both times (per the source's own note) -- no mismatch to pin
// here, just the three-way split and ONE more truncation: the ADD call's player is `uint16_t`
// (case_7's positive branch, 0x00469859 `MOVZX EAX,[EBP-0x34]`) while the REMOVE call's is the full
// `uint32_t` (0x0046986e, same MOVZX -- but the committed callee prototype differs, see
// sim_order_dispatch.h's dispatch_calls). Same ctx, both truncated and full-width readings checked.
void test_population_add_remove() {
    // == 0: neither call fires.
    {
        sim_fixture f;
        sim_view    v   = f.view();
        sim_store   own = f.store();
        put_order(f, 0, 0xec);
        f.order_queue[0].args[1] = 0;

        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_admin_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().events.size(), 0, "admin arm 7 (0xec), delta==0: does nothing at all");
    }
    // > 0: ADD, with the player truncated to uint16_t.
    {
        sim_fixture f;
        sim_view    v   = f.view();
        sim_store   own = f.store();
        put_order(f, 0, 0xec);
        f.order_queue[0].args[1] = 1234567;

        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_admin_order(v, own, c, ctx_for_player(0xabcd0009));
        ck_eq((uint32_t)dc().count(rec::DC_llm_strat_population_add), 1,
              "admin arm 7, delta>0: adds population exactly once");
        const rec::dc_event *e = dc().last(rec::DC_llm_strat_population_add);
        ck(e != nullptr && e->a[0] == 0x0009 && e->a[1] == 1234567,
           "admin arm 7, delta>0: (player truncated to uint16_t -- 0xabcd0009 -> 0x0009, delta)");
        ck_eq((uint32_t)dc().count(rec::DC_llm_strat_population_remove), 0,
              "admin arm 7, delta>0: and NOT remove");
        ck_eq((uint32_t)dc().events.size(), 1, "admin arm 7, delta>0: and does nothing else");
    }
    // < 0: REMOVE, with the player passed FULL WIDTH -- the asymmetry with the add branch above.
    {
        sim_fixture f;
        sim_view    v   = f.view();
        sim_store   own = f.store();
        put_order(f, 0, 0xec);
        f.order_queue[0].args[1] = -1234567;

        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_admin_order(v, own, c, ctx_for_player(0xabcd0009));
        ck_eq((uint32_t)dc().count(rec::DC_llm_strat_population_remove), 1,
              "admin arm 7, delta<0: removes population exactly once");
        const rec::dc_event *e = dc().last(rec::DC_llm_strat_population_remove);
        ck(e != nullptr && (uint32_t)e->a[0] == 0xabcd0009u && e->a[1] == -1234567,
           "admin arm 7, delta<0: (player NOT truncated here -- full 0xabcd0009, delta)");
        ck_eq((uint32_t)dc().count(rec::DC_llm_strat_population_add), 0,
              "admin arm 7, delta<0: and NOT add");
        ck_eq((uint32_t)dc().events.size(), 1, "admin arm 7, delta<0: and does nothing else");
    }
}

// ---- arm 8, RESOURCE_ADD_SPEND (0xed) ---------------------------------------------------------
// case_8 @0x004697dc: the same three-way shape as arm 7, keyed on args[2] this time, resource index
// args[3], amount args[2] -- and BOTH callees take the player as plain int32_t (no truncation), so
// the thing worth pinning here is the ARGUMENT ORDER (player, resource_index, amount), not a width.
void test_resource_add_spend() {
    {
        sim_fixture f;
        sim_view    v   = f.view();
        sim_store   own = f.store();
        put_order(f, 0, 0xed);
        f.order_queue[0].args[2] = 0;

        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_admin_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().events.size(), 0, "admin arm 8 (0xed), amount==0: does nothing at all");
    }
    {
        sim_fixture f;
        sim_view    v   = f.view();
        sim_store   own = f.store();
        put_order(f, 0, 0xed);
        f.order_queue[0].args[2] = 42; // amount
        f.order_queue[0].args[3] = 6;  // resource index -- distinct from the amount

        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_admin_order(v, own, c, ctx_for_player(0x15));
        ck_eq((uint32_t)dc().count(rec::DC_llm_resource_add), 1,
              "admin arm 8, amount>0: adds the resource exactly once");
        const rec::dc_event *e = dc().last(rec::DC_llm_resource_add);
        ck(e != nullptr && e->a[0] == 0x15 && e->a[1] == 6 && e->a[2] == 42,
           "admin arm 8, amount>0: (player, resource_index=args[3], amount=args[2] -- in that order)");
        ck_eq((uint32_t)dc().count(rec::DC_game_SpendResource), 0,
              "admin arm 8, amount>0: and NOT spend");
        ck_eq((uint32_t)dc().events.size(), 1, "admin arm 8, amount>0: and does nothing else");
    }
    {
        sim_fixture f;
        sim_view    v   = f.view();
        sim_store   own = f.store();
        put_order(f, 0, 0xed);
        f.order_queue[0].args[2] = -42; // amount
        f.order_queue[0].args[3] = 6;   // resource index

        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_admin_order(v, own, c, ctx_for_player(0x16));
        ck_eq((uint32_t)dc().count(rec::DC_game_SpendResource), 1,
              "admin arm 8, amount<0: spends the resource exactly once");
        const rec::dc_event *e = dc().last(rec::DC_game_SpendResource);
        ck(e != nullptr && e->a[0] == 0x16 && e->a[1] == 6 && e->a[2] == -42,
           "admin arm 8, amount<0: (player, resource_index=args[3], amount=args[2] -- in that order)");
        ck_eq((uint32_t)dc().count(rec::DC_llm_resource_add), 0,
              "admin arm 8, amount<0: and NOT add");
        ck_eq((uint32_t)dc().events.size(), 1, "admin arm 8, amount<0: and does nothing else");
    }
}

// ---- arms 9, 10, 11, 12 -- the progress/project family (0xee-0xf1) -------------------------------
// case_9/case_10/case_11 @0x0046987c/0x004698a3/0x00469896: one call each, player only. case_12
// @0x004698b0 has NO player at all -- `llm_progress_recheck_planet_system_all_players()` takes zero
// arguments (the name says why: it is not per-player).
void test_progress_family() {
    {
        sim_fixture f;
        sim_view    v   = f.view();
        sim_store   own = f.store();
        put_order(f, 0, 0xee);

        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_admin_order(v, own, c, ctx_for_player(0x2a));
        ck_eq((uint32_t)dc().count(rec::DC_llm_progress_collect_available_projects), 1,
              "admin arm 9 (0xee): collects available projects exactly once");
        const rec::dc_event *e = dc().last(rec::DC_llm_progress_collect_available_projects);
        ck(e != nullptr && e->a[0] == 0x2a, "admin arm 9: the player IS x.player");
        ck_eq((uint32_t)dc().events.size(), 1, "admin arm 9: and does nothing else");
    }
    {
        sim_fixture f;
        sim_view    v   = f.view();
        sim_store   own = f.store();
        put_order(f, 0, 0xef);

        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_admin_order(v, own, c, ctx_for_player(0x2b));
        ck_eq((uint32_t)dc().count(rec::DC_llm_progress_recheck_buildings), 1,
              "admin arm 10 (0xef): rechecks buildings exactly once");
        const rec::dc_event *e = dc().last(rec::DC_llm_progress_recheck_buildings);
        ck(e != nullptr && e->a[0] == 0x2b, "admin arm 10: the player IS x.player");
        ck_eq((uint32_t)dc().events.size(), 1, "admin arm 10: and does nothing else");
    }
    {
        sim_fixture f;
        sim_view    v   = f.view();
        sim_store   own = f.store();
        put_order(f, 0, 0xf0);

        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_admin_order(v, own, c, ctx_for_player(0x2c));
        ck_eq((uint32_t)dc().count(rec::DC_llm_progress_recheck_projects), 1,
              "admin arm 11 (0xf0 as an order_code -- unrelated to ORDER_KIND_LOCKSTEP 0xf0, a "
              "different field): rechecks projects exactly once");
        const rec::dc_event *e = dc().last(rec::DC_llm_progress_recheck_projects);
        ck(e != nullptr && e->a[0] == 0x2c, "admin arm 11: the player IS x.player");
        ck_eq((uint32_t)dc().events.size(), 1, "admin arm 11: and does nothing else");
    }
    {
        sim_fixture f;
        sim_view    v   = f.view();
        sim_store   own = f.store();
        put_order(f, 0, 0xf1);

        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_admin_order(v, own, c, ctx_for_player(0x2d));
        ck_eq((uint32_t)dc().count(rec::DC_llm_progress_recheck_planet_system_all_players), 1,
              "admin arm 12 (0xf1): rechecks the whole planet system exactly once");
        const rec::dc_event *e = dc().last(rec::DC_llm_progress_recheck_planet_system_all_players);
        ck(e != nullptr && e->na == 0, "admin arm 12: takes NO arguments -- not even the player");
        ck_eq((uint32_t)dc().events.size(), 1, "admin arm 12: and does nothing else");
    }
}

// ---- arm 13, BLDG_QUEUE_CONSTRUCT (0xf2) ----------------------------------------------------------
// case_13 @0x00469757: four distinct int32_t values (player, building_type=args[0], x=args[4],
// y=args[5]) with no truncation anywhere -- the only thing to get wrong is the ORDER, so all four
// values are distinct, nonzero and non-symmetric, and one is negative to prove sign survives.
void test_bldg_queue_construction() {
    sim_fixture f;
    sim_view    v   = f.view();
    sim_store   own = f.store();
    put_order(f, 0, 0xf2);
    f.order_queue[0].args[0] = 9;    // building_type
    f.order_queue[0].args[4] = -17;  // x
    f.order_queue[0].args[5] = 3001; // y

    dc().reset();
    dispatch_calls c = rec::recording_calls();
    detail::dispatch_admin_order(v, own, c, ctx_for_player(0x33));
    ck_eq((uint32_t)dc().count(rec::DC_llm_bldg_queue_construction), 1,
          "admin arm 13 (0xf2): queues the construction exactly once");
    const rec::dc_event *e = dc().last(rec::DC_llm_bldg_queue_construction);
    ck(e != nullptr && e->a[0] == 0x33 && e->a[1] == 9 && e->a[2] == -17 && e->a[3] == 3001,
       "admin arm 13: (player, building_type=args[0], x=args[4], y=args[5] -- in that order)");
    ck_eq((uint32_t)dc().events.size(), 1, "admin arm 13: and does nothing else");
}

// ---- arm 14, UNIT_RECRUIT (0xf3) --------------------------------------------------------------
// case_14 @0x0046973f: player and args[1] (the unit type id), no branching.
void test_unit_recruit() {
    sim_fixture f;
    sim_view    v   = f.view();
    sim_store   own = f.store();
    put_order(f, 0, 0xf3);
    f.order_queue[0].args[1] = 424242;

    dc().reset();
    dispatch_calls c = rec::recording_calls();
    detail::dispatch_admin_order(v, own, c, ctx_for_player(0x37));
    ck_eq((uint32_t)dc().count(rec::DC_llm_unit_recruit), 1,
          "admin arm 14 (0xf3): recruits the unit exactly once");
    const rec::dc_event *e = dc().last(rec::DC_llm_unit_recruit);
    ck(e != nullptr && e->a[0] == 0x37 && e->a[1] == 424242,
       "admin arm 14: (player, unit_type_id=args[1])");
    ck_eq((uint32_t)dc().events.size(), 1, "admin arm 14: and does nothing else");
}

// ---- arm 15, DIPLOMACY_SET_RELATION (0xf4) --------------------------------------------------------
// case_15 @0x004698ba: `MOVZX EBX,byte ptr args[3]` -- the relation value is read as a BYTE straight
// out of the record (not even a 32-bit load truncated later), which the committed uint8_t parameter
// models. args[2] (the other player) stays full-width int32_t.
void test_diplomacy_set_relation() {
    sim_fixture f;
    sim_view    v   = f.view();
    sim_store   own = f.store();
    put_order(f, 0, 0xf4);
    f.order_queue[0].args[2] = 12345;      // other_player
    f.order_queue[0].args[3] = 0x998800f1; // relation -- low byte 0xf1, high bits must be dropped

    dc().reset();
    dispatch_calls c = rec::recording_calls();
    detail::dispatch_admin_order(v, own, c, ctx_for_player(0x18));
    ck_eq((uint32_t)dc().count(rec::DC_llm_diplomacy_set_relation), 1,
          "admin arm 15 (0xf4): sets the relation exactly once");
    const rec::dc_event *e = dc().last(rec::DC_llm_diplomacy_set_relation);
    ck(e != nullptr && e->a[0] == 0x18 && e->a[1] == 12345 && e->a[2] == 0xf1,
       "admin arm 15: (player, other_player=args[2], relation=args[3] truncated to uint8_t -- "
       "0x998800f1 -> 0xf1)");
    ck_eq((uint32_t)dc().events.size(), 1, "admin arm 15: and does nothing else");
}

// ---- arm 16, PLAYER_SET_AI_HUMAN (0xf5) -----------------------------------------------------------
// case_16 @0x004698dc: `MOVZX EDX,word ptr [PlayerSide] / CMP EDX,args[2]` -- PlayerSide is a 16-bit
// field read with MOVZX, i.e. ZERO-extended into the comparison, which is what the source's
// `(int32_t)(uint16_t)*v.player_side` reproduces. A translation that instead sign-extended a negative
// PlayerSide (`(int32_t)*v.player_side`) disagrees with this exact case: PlayerSide == -1 reads as
// 0xffff (65535) under the original's zero-extend, not -1.
void test_player_set_ai_human() {
    // side != args[2]: neither call fires. Fixture default player_side (7) against a mismatching 3.
    {
        sim_fixture f;
        sim_view    v   = f.view();
        sim_store   own = f.store();
        put_order(f, 0, 0xf5);
        f.order_queue[0].args[2] = 3; // != fixture's player_side (7)
        f.order_queue[0].args[3] = 0;

        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_admin_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().events.size(), 0,
              "admin arm 16 (0xf5), side mismatch: neither set_ai nor set_human fires");
    }
    // THE ZERO-EXTEND: player_side == -1 (0xffff as a raw 16-bit field) must compare EQUAL to
    // args[2] == 65535, not to args[2] == -1.
    {
        sim_fixture f;
        sim_view    v   = f.view();
        sim_store   own = f.store();
        put_order(f, 0, 0xf5);
        f.player_side            = -1;    // raw bits 0xffff
        f.order_queue[0].args[2] = 65535; // the ZERO-extended reading of those bits
        f.order_queue[0].args[3] = 0;     // -> set_ai

        dc().reset();
        dispatch_calls c = rec::recording_calls();
        // Player also carries high bits, to prove the (uint8_t) truncation on the call and the
        // (uint16_t) zero-extend on the gate are two independent casts, not one shared one.
        detail::dispatch_admin_order(v, own, c, ctx_for_player(0xbeef0009));
        ck_eq((uint32_t)dc().count(rec::DC_llm_game_player_set_ai), 1,
              "admin arm 16, side==-1 read as 0xffff==65535, args[3]==0: sets AI exactly once");
        const rec::dc_event *e = dc().last(rec::DC_llm_game_player_set_ai);
        ck(e != nullptr && e->a[0] == 0x09,
           "admin arm 16: player truncated to uint8_t -- 0xbeef0009 -> 0x09");
        ck_eq((uint32_t)dc().events.size(), 1, "admin arm 16: and does nothing else");
    }
    // Same match, args[3] != 0 -> set_human instead.
    {
        sim_fixture f;
        sim_view    v   = f.view();
        sim_store   own = f.store();
        put_order(f, 0, 0xf5);
        f.player_side            = -1;
        f.order_queue[0].args[2] = 65535;
        f.order_queue[0].args[3] = 7; // any nonzero -> set_human

        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_admin_order(v, own, c, ctx_for_player(0xcafe000a));
        ck_eq((uint32_t)dc().count(rec::DC_llm_game_player_set_human), 1,
              "admin arm 16, side match, args[3]!=0: sets human exactly once");
        const rec::dc_event *e = dc().last(rec::DC_llm_game_player_set_human);
        ck(e != nullptr && e->a[0] == 0x0a,
           "admin arm 16: player truncated to uint8_t -- 0xcafe000a -> 0x0a");
        ck_eq((uint32_t)dc().count(rec::DC_llm_game_player_set_ai), 0,
              "admin arm 16: and NOT set_ai");
        ck_eq((uint32_t)dc().events.size(), 1, "admin arm 16: and does nothing else");
    }
}

// ---- arms 17, 18, 19 -- the energy/damage family (0xf6-0xf8) -------------------------------------
// case_17/18/19 @0x00469910/0x0046992b/0x00469946: identical shape, three different callees, args[2]
// then args[3] both times. Distinct, non-symmetric values across all three catch a swapped pair
// AND a case that called the wrong one of the three (same argument count and order for all three).
void test_energy_and_damage_family() {
    {
        sim_fixture f;
        sim_view    v   = f.view();
        sim_store   own = f.store();
        put_order(f, 0, 0xf6);
        f.order_queue[0].args[2] = 0x1001; // target_selector / player_and_flags
        f.order_queue[0].args[3] = 0x2002; // target_index

        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_admin_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().count(rec::DC_llm_unit_bldg_energy_refill_full), 1,
              "admin arm 17 (0xf6): refills energy exactly once");
        const rec::dc_event *e = dc().last(rec::DC_llm_unit_bldg_energy_refill_full);
        ck(e != nullptr && e->a[0] == 0x1001 && e->a[1] == 0x2002,
           "admin arm 17: (player_and_flags=args[2], target_index=args[3])");
        ck_eq((uint32_t)dc().events.size(), 1, "admin arm 17: and does nothing else");
    }
    {
        sim_fixture f;
        sim_view    v   = f.view();
        sim_store   own = f.store();
        put_order(f, 0, 0xf7);
        f.order_queue[0].args[2] = 0x3003; // target_selector
        f.order_queue[0].args[3] = -55;    // target_index -- negative, int32_t on this callee

        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_admin_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().count(rec::DC_llm_unit_bldg_apply_scaled_damage), 1,
              "admin arm 18 (0xf7): applies scaled damage exactly once");
        const rec::dc_event *e = dc().last(rec::DC_llm_unit_bldg_apply_scaled_damage);
        ck(e != nullptr && e->a[0] == 0x3003 && e->a[1] == -55,
           "admin arm 18: (target_selector=args[2], target_index=args[3])");
        ck_eq((uint32_t)dc().count(rec::DC_llm_unit_bldg_energy_refill_full), 0,
              "admin arm 18: and NOT energy refill");
        ck_eq((uint32_t)dc().events.size(), 1, "admin arm 18: and does nothing else");
    }
    {
        sim_fixture f;
        sim_view    v   = f.view();
        sim_store   own = f.store();
        put_order(f, 0, 0xf8);
        f.order_queue[0].args[2] = 0x4004; // target_ref
        f.order_queue[0].args[3] = -66;    // target_index

        dc().reset();
        dispatch_calls c = rec::recording_calls();
        detail::dispatch_admin_order(v, own, c, ctx_for());
        ck_eq((uint32_t)dc().count(rec::DC_llm_unit_bldg_apply_lethal_damage), 1,
              "admin arm 19 (0xf8): applies lethal damage exactly once");
        const rec::dc_event *e = dc().last(rec::DC_llm_unit_bldg_apply_lethal_damage);
        ck(e != nullptr && e->a[0] == 0x4004 && e->a[1] == -66,
           "admin arm 19: (target_ref=args[2], target_index=args[3])");
        ck_eq((uint32_t)dc().count(rec::DC_llm_unit_bldg_apply_scaled_damage), 0,
              "admin arm 19: and NOT scaled damage");
        ck_eq((uint32_t)dc().events.size(), 1, "admin arm 19: and does nothing else");
    }
}

// ---- arm 20, FOW_REVEAL_FULL (0xf9) -----------------------------------------------------------
// case_20 @0x00469961: `MOV EAX,args[2] / CALL` -- the ONE admin arm whose sole argument comes from
// the RECORD, not from x.player like every neighbour. The player is set to a value the call must NOT
// see, so a translation that (like every arm around this one) passed x.player instead is caught.
void test_fow_reveal_full() {
    sim_fixture f;
    sim_view    v   = f.view();
    sim_store   own = f.store();
    put_order(f, 0, 0xf9);
    f.order_queue[0].args[2] = 999999;

    dc().reset();
    dispatch_calls c = rec::recording_calls();
    detail::dispatch_admin_order(v, own, c, ctx_for_player(77)); // 77 must NOT show up below
    ck_eq((uint32_t)dc().count(rec::DC_llm_map_fow_reveal_full), 1,
          "admin arm 20 (0xf9): reveals FOW exactly once");
    const rec::dc_event *e = dc().last(rec::DC_llm_map_fow_reveal_full);
    ck(e != nullptr && e->a[0] == 999999,
       "admin arm 20: the argument is args[2] (999999), NOT x.player (77)");
    ck_eq((uint32_t)dc().events.size(), 1, "admin arm 20: and does nothing else");
}

// ---- arm 21, CREDIT_CONQUEST_KILLS (0xfa) -------------------------------------------------------
// case_21 @0x00469889: player only.
void test_credit_conquest_kills() {
    sim_fixture f;
    sim_view    v   = f.view();
    sim_store   own = f.store();
    put_order(f, 0, 0xfa);

    dc().reset();
    dispatch_calls c = rec::recording_calls();
    detail::dispatch_admin_order(v, own, c, ctx_for_player(0x41));
    ck_eq((uint32_t)dc().count(rec::DC_llm_combat_credit_planet_conquest_kills), 1,
          "admin arm 21 (0xfa): credits conquest kills exactly once");
    const rec::dc_event *e = dc().last(rec::DC_llm_combat_credit_planet_conquest_kills);
    ck(e != nullptr && e->a[0] == 0x41, "admin arm 21: the player IS x.player");
    ck_eq((uint32_t)dc().events.size(), 1, "admin arm 21: and does nothing else");
}

// ---- arm 22, UNIT_NOTIFY_STATUS (0xfb) --------------------------------------------------------
// case_22 @0x00469972: `XOR EBX,EBX` -- the status_code argument is the LITERAL 0, not read from the
// record at all; player and object_index are the two ctx locals, in that order.
void test_unit_notify_status() {
    sim_fixture f;
    sim_view    v   = f.view();
    sim_store   own = f.store();
    put_order(f, 0, 0xfb);

    dc().reset();
    dispatch_calls c = rec::recording_calls();
    detail::dispatch_admin_order(v, own, c, ctx_for()); // PLAYER=3, OBJ=5 -- distinct, catches a swap
    ck_eq((uint32_t)dc().count(rec::DC_llm_strat_unit_notify_status), 1,
          "admin arm 22 (0xfb): notifies unit status exactly once");
    const rec::dc_event *e = dc().last(rec::DC_llm_strat_unit_notify_status);
    ck(e != nullptr && e->a[0] == PLAYER && e->a[1] == OBJ && e->a[2] == 0,
       "admin arm 22: (player, object_index, status_code=0 -- the literal, not a record field)");
    ck_eq((uint32_t)dc().events.size(), 1, "admin arm 22: and does nothing else");
}

} // namespace

void run_dispatch_admin_tests() {
    test_default_arm_is_empty();
    test_debug_roll_random();
    test_unit_status_bit_set_clear();
    test_game_speed();
    test_unit_create();
    test_population_add_remove();
    test_resource_add_spend();
    test_progress_family();
    test_bldg_queue_construction();
    test_unit_recruit();
    test_diplomacy_set_relation();
    test_player_set_ai_human();
    test_energy_and_damage_family();
    test_fow_reveal_full();
    test_credit_conquest_kills();
    test_unit_notify_status();
}

} // namespace mh::sim::test
