//
// sim_unit_order_disembark_soldiers_selftest.cpp -- `simtest` oracle for
// llm_unit_order_disembark_soldiers @0x0046e76c (sim/sim_unit_order_disembark_soldiers.h/.cpp),
// X-TL-DRAIN step 4.
//
// THIS FILE IS THE FUNCTION'S ONLY ORACLE, and the reason is recorded rather than assumed. Its shadow
// site is DELIBERATELY VACUOUS: the body returns void and writes nothing, so with its one outward
// call (llm_strat_order_dispatch) necessarily inert in the arm -- a second real dispatch re-runs
// llm_strat_order_integrity_check, whose SESSION_MODE 3 failure path forces a return to the main menu
// -- there is no compared surface left at all. gen_shadow_ini.py classifies the site VACUOUS from the
// manifest and holds it out of every arm set, which is correct. A recording stub, by contrast, sees
// exactly the two things that matter: WHETHER the dispatch happens, and with WHICH four arguments.
//
// EVERY EXPECTED VALUE COMES FROM THE DISASSEMBLY
// (tmp/decomp/llm_unit_order_disembark_soldiers_0046e76c.asm), re-cited per case:
//     0x0046e789  MOVZX EAX,word ptr [EBP-0x14]      player TRUNCATED to 16 bits for the roster read
//     0x0046e79c  MOVZX EAX,word ptr [EAX+0xdd8c4a]  units[player][unit_idx].unit_proto_id
//     0x0046e7a9  CMP dword ptr [EAX+0xe4a2bf],0x0   cfg Unit[proto].soldier_count
//     0x0046e7b0  JLE                                <= 0 -> return, SIGNED
//     0x0046e7b2  MOV ECX,0x32  /  0x0046e7b7 MOV EBX,0x32
//     0x0046e7bf  OR AL,0x80    /  0x0046e7c1 MOVZX EDX,AX
//     0x0046e7c4  MOVZX EAX,word ptr [EBP-0x18]      unit_idx, low 16 bits
//
#include <array>
#include <vector>

#include "sim/sim_unit_order_disembark_soldiers.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// Distinct, non-symmetric seeds: no two share a value, so a swapped-argument translation cannot pass
// by accident. PLAYER is chosen with low bits SET so the `| 0x80` is observable as an OR and not as
// an assignment, and PROTO_WITH / PROTO_WITHOUT differ so a wrong roster index is observable too.
constexpr uint32_t PLAYER        = 3;
constexpr int32_t  UNIT_IDX      = 17;
constexpr uint16_t PROTO_WITH    = 21; // soldier-carrying unit type
constexpr uint16_t PROTO_WITHOUT = 22; // not a carrier
constexpr uint16_t ORDER_CODE    = 0x32;
constexpr uint32_t OWNER_TAG     = 0x80;

struct dsb_recorder {
    int32_t                              n = 0;
    std::vector<std::array<uint32_t, 4>> args; // (unit_id, player, op_code, arg)
    int32_t                              ret = 0;
    void                                 reset() { *this = dsb_recorder{}; }
};
dsb_recorder g_dsb;

const unit_order_disembark_soldiers_calls &rec_dsb_calls() {
    static const unit_order_disembark_soldiers_calls c = {
        [](uint16_t unit_id, uint32_t player, uint16_t op_code, uint16_t arg) -> int32_t {
            g_dsb.n++;
            g_dsb.args.push_back({(uint32_t)unit_id, player, (uint32_t)op_code, (uint32_t)arg});
            return g_dsb.ret;
        },
    };
    return c;
}

// Seeds the roster so `units[PLAYER][UNIT_IDX].unit_proto_id == proto`, and gives BOTH cfg rows a
// soldier_count so a translation reading the wrong proto row lands on a different number rather than
// on zero.
void seed(sim_fixture &f, uint16_t proto, int32_t soldier_count_for_proto) {
    f.u(PLAYER, UNIT_IDX).unit_proto_id      = proto;
    f.cfg_units[PROTO_WITH].soldier_count    = 0;
    f.cfg_units[PROTO_WITHOUT].soldier_count = 0;
    f.cfg_units[proto].soldier_count         = soldier_count_for_proto;
}

// ==== the soldier_count gate =====================================================================

void test_positive_soldier_count_dispatches_once() {
    sim_fixture f;
    g_dsb.reset();
    seed(f, PROTO_WITH, 4);

    sim_view v = f.view();
    detail::unit_order_disembark_soldiers(v, rec_dsb_calls(), PLAYER, UNIT_IDX);

    ck_eq((uint32_t)g_dsb.n, 1u,
          "soldier_count > 0 -> exactly one llm_strat_order_dispatch (0x0046e7b0 JLE not taken)");
}

void test_zero_soldier_count_dispatches_nothing() {
    sim_fixture f;
    g_dsb.reset();
    seed(f, PROTO_WITH, 0);

    sim_view v = f.view();
    detail::unit_order_disembark_soldiers(v, rec_dsb_calls(), PLAYER, UNIT_IDX);

    ck_eq((uint32_t)g_dsb.n, 0u,
          "soldier_count == 0 -> nothing at all happens (0x0046e7a9 CMP ...,0x0 / 0x0046e7b0 JLE)");
}

void test_negative_soldier_count_dispatches_nothing() {
    sim_fixture f;
    g_dsb.reset();
    seed(f, PROTO_WITH, -1);

    sim_view v = f.view();
    detail::unit_order_disembark_soldiers(v, rec_dsb_calls(), PLAYER, UNIT_IDX);

    // THE MUTATION-SEPARATING CASE. `JLE` is a SIGNED branch, so a negative count skips exactly like
    // a zero one. A translation written as `!= 0`, or one that read the field unsigned, dispatches
    // here and passes every other case in this file.
    ck_eq((uint32_t)g_dsb.n, 0u,
          "soldier_count < 0 -> ALSO nothing: the branch is JLE (signed), not JZ/JE "
          "(0x0046e7b0)");
}

void test_gate_reads_the_units_proto_row_not_a_fixed_one() {
    sim_fixture f;
    g_dsb.reset();
    // The unit points at the NON-carrier row; the carrier row has a large positive count. A
    // translation that indexed cfg_units by anything other than this unit's unit_proto_id would find
    // the carrier's 9 and dispatch.
    seed(f, PROTO_WITHOUT, 0);
    f.cfg_units[PROTO_WITH].soldier_count = 9;

    sim_view v = f.view();
    detail::unit_order_disembark_soldiers(v, rec_dsb_calls(), PLAYER, UNIT_IDX);

    ck_eq((uint32_t)g_dsb.n, 0u,
          "the count is read at cfg Unit[units[player][unit_idx].unit_proto_id], i.e. through the "
          "roster indirection (0x0046e79c -> 0x0046e7a3 -> 0x0046e7a9)");
}

// ==== the four dispatch arguments ================================================================

void test_dispatch_argument_values() {
    sim_fixture f;
    g_dsb.reset();
    seed(f, PROTO_WITH, 1);

    sim_view v = f.view();
    detail::unit_order_disembark_soldiers(v, rec_dsb_calls(), PLAYER, UNIT_IDX);

    ck(g_dsb.args.size() == 1 && g_dsb.args[0][0] == (uint32_t)UNIT_IDX,
       "arg 1 (EAX) = unit_idx, low 16 bits (0x0046e7c4 MOVZX EAX,word ptr [EBP-0x18])");
    ck(g_dsb.args.size() == 1 && g_dsb.args[0][1] == (PLAYER | OWNER_TAG),
       "arg 2 (EDX) = (uint16_t)(player | 0x80) -- an OR onto the player index, so the player's own "
       "low bits SURVIVE (PLAYER=3 -> 0x83, not 0x80) (0x0046e7bf OR AL,0x80 / 0x0046e7c1 MOVZX)");
    ck(g_dsb.args.size() == 1 && g_dsb.args[0][2] == ORDER_CODE && g_dsb.args[0][3] == ORDER_CODE,
       "args 3 and 4 (EBX, ECX) are BOTH 0x32 (0x0046e7b2 / 0x0046e7b7)");
}

void test_player_is_truncated_to_16_bits_for_the_roster_read() {
    sim_fixture f;
    g_dsb.reset();
    seed(f, PROTO_WITH, 2);

    sim_view v = f.view();
    // 0x0046e789 reads the player argument as a WORD. Passing 0x10000 + PLAYER must therefore behave
    // exactly as PLAYER does: the same roster row (so the same proto, so the same gate verdict) and
    // a dispatch tag of (uint16_t)(0x10003 | 0x80) == 0x83, with the high half gone.
    //
    // A translation that dropped the truncation would index units[0x10003], far past the fixture's
    // exactly-sized vector -- ASan turns that into a named stack trace on run #1 rather than the
    // delayed, intermittent corruption a plain build would give.
    detail::unit_order_disembark_soldiers(v, rec_dsb_calls(), 0x10000u + PLAYER, UNIT_IDX);

    ck_eq((uint32_t)g_dsb.n, 1u,
          "player = 0x10003 reads roster row 3 (the word truncation at 0x0046e789), so the gate "
          "sees the same carrier unit and dispatches");
    ck(g_dsb.args.size() == 1 && g_dsb.args[0][1] == (PLAYER | OWNER_TAG),
       "... and the dispatch tag is also 16-bit: (uint16_t)(0x10003 | 0x80) == 0x83");
}

void test_return_value_of_dispatch_is_discarded_and_body_writes_nothing() {
    sim_fixture f;
    g_dsb.reset();
    g_dsb.ret = -12345; // the original never reads EAX back from this call
    seed(f, PROTO_WITH, 3);
    f.u(PLAYER, UNIT_IDX).state  = 0x2b2b;
    f.u(PLAYER, UNIT_IDX).goal_x = 0x5c;
    f.b(PLAYER, 1).building_id   = 0x3131;

    sim_view v = f.view();
    detail::unit_order_disembark_soldiers(v, rec_dsb_calls(), PLAYER, UNIT_IDX);

    ck_eq((uint32_t)g_dsb.n, 1u, "dispatch still happens with a hostile return value");
    ck_eq((uint32_t)f.u(PLAYER, UNIT_IDX).state, 0x2b2bu, "no write to the unit's state");
    ck_eq((uint32_t)f.u(PLAYER, UNIT_IDX).goal_x, 0x5cu, "no write to the unit's goal");
    ck_eq((uint32_t)f.b(PLAYER, 1).building_id, 0x3131u, "no write to `buildings`");
}

} // namespace

void run_unit_order_disembark_soldiers_tests() {
    test_positive_soldier_count_dispatches_once();
    test_zero_soldier_count_dispatches_nothing();
    test_negative_soldier_count_dispatches_nothing();
    test_gate_reads_the_units_proto_row_not_a_fixed_one();

    test_dispatch_argument_values();
    test_player_is_truncated_to_16_bits_for_the_roster_read();
    test_return_value_of_dispatch_is_discarded_and_body_writes_nothing();
}

} // namespace mh::sim::test
