//
// sim_unit_refund_selftest.cpp -- `simtest` cases for the BODY of
// llm_strat_unit_refund_build_cost_by_health @0x0048d706 (sim/sim_unit_refund.h/.cpp,
// RI-SIM / SIM1A).
//
// The function refunds a health-scaled fraction of a unit type's cfg build cost back to the player by
// calling llm_resource_add once per non-empty cfg resource slot. It performs NO sim-state writes (a
// pure read over units / cfg_units plus the one outward call), so this file's assertions are entirely
// on the RECORDED CALL SEQUENCE: how many refunds, in what order, and the exact (player,
// resource_index, amount) each carries. Own recording calls struct, same shape as
// sim_unit_ctrlgroup_member_selftest.cpp's log_t (one member, one static instance, reset per case).
//
// EVERY EXPECTED VALUE IS DERIVED FROM THE DISASSEMBLY
// (tmp/decomp/llm_strat_unit_refund_build_cost_by_health_0048d706.asm), not from the C++ translation.
// The formula, instruction by instruction:
//
//   proto = (uint16_t)unit.unit_proto_id                              @0x0048d733 MOVZX word
//   ratio = (unit.energy * 0.5) / cfg_units[proto].energy             @0x0048d74d FLD /
//                                                                       0x0048d753 FMUL DAT 0.5 /
//                                                                       0x0048d760 FDIV /
//                                                                       0x0048d766 FSTP -> 64-bit local
//   for (i = 0; ; ++i):
//       resource_id = (int32)cfg_units[proto].resource[i].id          @0x0048d77f MOV dword
//       if (resource_id == 0) break;                                  @0x0048d788/8c CMP,0 / JZ
//       if (!(i < 7)) break;                                          @0x0048d78e/92 CMP,7 / JL
//       amount = trunc_toward_zero(resource[i].val * ratio)           @0x0048d7a5 FILD dword val /
//                                                                       0x0048d7ab FMUL ratio /
//                                                                       0x0048d7ae CALL utils_math_trunc
//                                                                                  (RC=11 truncate) /
//                                                                       0x0048d7b3 FISTP dword
//       llm_resource_add(player, resource_id, amount)                 @0x0048d7b6 EDX=id /
//                                                                       0x0048d7b9 EAX=player /
//                                                                       0x0048d7bc EBX=amount /
//                                                                       0x0048d7bf CALL
//
// The __watcall register order at the call (EAX, EDX, EBX) resolves to
// llm_resource_add(player, resource_id, amount) -- matching the committed prototype in
// sim_unit_refund.h. `energy` on both sides of `ratio` is the HP-like stat, NOT the POWER resource.
//
// FP EXACTNESS: every fixture ratio is 0.0, 0.25 or 0.5 (all exactly representable) and every
// resource val is a small integer, so each `val * ratio` is an exact double and its truncation is a
// hand-computable integer. No tolerance is needed or wanted (ck_eq is exact).
//
#include "sim/sim_unit_refund.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// player < MAX_PLAYERS (8), unit_index < UNITS_PER_PLAYER (100); distinct and non-symmetric so a
// swapped-IMUL translation lands in a different record.
constexpr int32_t PLAYER = 2;
constexpr int32_t UNIT   = 7;

struct refund_log {
    struct ev {
        int32_t player;
        int32_t resource_index;
        int32_t amount;
    };
    std::vector<ev> events;
    void            reset() { events.clear(); }
};
refund_log g_log;

const unit_refund_build_cost_by_health_calls &recording_calls() {
    static const unit_refund_build_cost_by_health_calls c = {
        [](int32_t player, int32_t resource_index, int32_t amount) -> void {
            g_log.events.push_back(refund_log::ev{player, resource_index, amount});
        },
    };
    return c;
}

// Seed one cfg resource slot. `id` is the E_RESOURCE key (0 = the UNDEFINED sentinel that stops the
// walk); `val` is the per-slot build cost the refund scales.
void put_resource(sim_fixture &f, uint32_t proto, int32_t slot, uint32_t id, int32_t val) {
    f.cfg_units[proto].resource[slot].id  = id;
    f.cfg_units[proto].resource[slot].val = val;
}

// Assert the recorded refund at `n` is exactly (player, resource_index, amount).
void ck_ev(int32_t n, int32_t player, int32_t resource_index, int32_t amount, const char *what) {
    if ((size_t)n >= g_log.events.size()) {
        ck(false, what);
        return;
    }
    const refund_log::ev &e = g_log.events[(size_t)n];
    ck(e.player == player && e.resource_index == resource_index && e.amount == amount, what);
}

// ---- the proportional refund: three slots, ratio 0.25, stop at the zero-id sentinel --------------
// unit.energy 100, cfg energy 200 -> ratio = (100 * 0.5) / 200 = 0.25 (exact). The three vals give
// 40*0.25=10.0, 33*0.25=8.25, 17*0.25=4.25 -> trunc 10, 8, 4. resource[3].id == 0 stops the walk, so
// exactly three refunds fire, in slot order. Ids (3,6,2), vals (40,33,17) and amounts (10,8,4) are
// all distinct and non-symmetric, so a swapped id/amount or a wrong slot order is caught.
void test_proportional_refund_stops_at_zero_id() {
    sim_fixture f;
    f.u(PLAYER, UNIT).unit_proto_id = 5;
    f.u(PLAYER, UNIT).energy        = 100.0;
    f.cfg_units[5].energy           = 200.0;
    put_resource(f, 5, 0, /*id=*/3, /*val=*/40); // -> 10
    put_resource(f, 5, 1, /*id=*/6, /*val=*/33); // -> 8  (8.25 truncates DOWN)
    put_resource(f, 5, 2, /*id=*/2, /*val=*/17); // -> 4  (4.25 truncates DOWN)
    put_resource(f, 5, 3, /*id=*/0, /*val=*/99); // id 0: STOP (val is a sentinel, must not be read)

    const sim_view v = f.view();
    g_log.reset();
    detail::unit_refund_build_cost_by_health(v, recording_calls(), PLAYER, UNIT);

    ck_eq((uint32_t)g_log.events.size(), 3u, "refund: three non-empty slots -> three refunds");
    ck_ev(0, PLAYER, 3, 10, "refund[0]: (player, id=3, trunc(40*0.25)=10)");
    ck_ev(1, PLAYER, 6, 8, "refund[1]: (player, id=6, trunc(33*0.25)=8, fraction dropped)");
    ck_ev(2, PLAYER, 2, 4, "refund[2]: (player, id=2, trunc(17*0.25)=4, fraction dropped)");
}

// ---- ratio 0.5 (full health) + FINDING: truncation is toward ZERO, not floor -------------------
// unit.energy 50, cfg energy 50 -> ratio = (50 * 0.5) / 50 = 0.5 (exact). val 7 -> 3.5 -> trunc 3.
// val -9 -> -4.5 -> trunc toward zero -> -4 (a FLOOR would give -5). The negative slot exists only to
// pin the ROUNDING MODE of the inlined utils_math_trunc (FRNDINT with RC=11 = truncate toward zero,
// 0x004d059f/a7 in the .asm) -- the shipped cfg never ships a negative build cost, which is exactly
// why the rig cannot exercise this and a floor-vs-trunc mistranslation would slip through unnoticed.
void test_full_health_ratio_and_truncation_toward_zero_FINDING() {
    sim_fixture f;
    f.u(PLAYER, UNIT).unit_proto_id = 9;
    f.u(PLAYER, UNIT).energy        = 50.0;
    f.cfg_units[9].energy           = 50.0;
    put_resource(f, 9, 0, /*id=*/4, /*val=*/7);  // 7 * 0.5 = 3.5 -> trunc 3
    put_resource(f, 9, 1, /*id=*/1, /*val=*/-9); // -9 * 0.5 = -4.5 -> trunc toward zero -> -4
    put_resource(f, 9, 2, /*id=*/0, /*val=*/0);  // STOP

    const sim_view v = f.view();
    g_log.reset();
    detail::unit_refund_build_cost_by_health(v, recording_calls(), PLAYER, UNIT);

    ck_eq((uint32_t)g_log.events.size(), 2u, "refund: two non-empty slots -> two refunds");
    ck_ev(0, PLAYER, 4, 3, "refund[0]: (player, id=4, trunc(7*0.5)=3)");
    ck_ev(1, PLAYER, 1, -4,
          "refund[1] FINDING: (player, id=1, trunc(-9*0.5) = trunc(-4.5) = -4, toward ZERO not -5)");
}

// ---- FINDING: zero health -> ratio 0 -> a refund STILL fires per slot, amount 0 (no skip) ------
// unit.energy 0 -> ratio = (0 * 0.5) / 123 = 0.0. Each amount is trunc(val * 0) = 0, but the loop has
// no "amount == 0 -> skip": it calls llm_resource_add for every non-empty slot regardless. So a
// destroyed unit still enqueues zero-value refunds. cfg energy is a non-zero, non-round 123 to prove
// the ratio really zeroed via the numerator, not a divisor coincidence.
void test_zero_health_still_refunds_zero_per_slot_FINDING() {
    sim_fixture f;
    f.u(PLAYER, UNIT).unit_proto_id = 4;
    f.u(PLAYER, UNIT).energy        = 0.0;
    f.cfg_units[4].energy           = 123.0;
    put_resource(f, 4, 0, /*id=*/5, /*val=*/99); // 99 * 0 = 0
    put_resource(f, 4, 1, /*id=*/7, /*val=*/88); // 88 * 0 = 0
    put_resource(f, 4, 2, /*id=*/0, /*val=*/0);  // STOP

    const sim_view v = f.view();
    g_log.reset();
    detail::unit_refund_build_cost_by_health(v, recording_calls(), PLAYER, UNIT);

    ck_eq((uint32_t)g_log.events.size(), 2u,
          "refund FINDING: zero-health -> two refunds STILL fire (loop does not skip amount 0)");
    ck_ev(0, PLAYER, 5, 0, "refund[0]: (player, id=5, amount 0)");
    ck_ev(1, PLAYER, 7, 0, "refund[1]: (player, id=7, amount 0)");
}

// ---- boundary: the first slot's id is the UNDEFINED sentinel -> no refund at all -----------------
// resource[0].id == 0, so the id check (0x0048d788) breaks before any resource_add. A non-zero cfg
// energy and unit energy prove it is the id check that stops the walk, not a divide-by-zero or an
// empty proto.
void test_first_slot_undefined_yields_no_refund() {
    sim_fixture f;
    f.u(PLAYER, UNIT).unit_proto_id = 6;
    f.u(PLAYER, UNIT).energy        = 80.0;
    f.cfg_units[6].energy           = 160.0;
    put_resource(f, 6, 0, /*id=*/0, /*val=*/12345); // id 0 in the FIRST slot: nothing runs

    const sim_view v = f.view();
    g_log.reset();
    detail::unit_refund_build_cost_by_health(v, recording_calls(), PLAYER, UNIT);

    ck_eq((uint32_t)g_log.events.size(), 0u,
          "refund: resource[0].id == UNDEFINED(0) -> zero refunds (id check breaks first)");
}

// ---- FINDING: the id READ happens BEFORE the i<7 bound check, so a full 7 slots refund exactly 7,
// and the bound (not the sentinel) is what stops it -----------------------------------------------
// All seven resource[] slots carry a non-zero id, so the id check never breaks. The walk processes
// i = 0..6 (seven refunds) and at i == 7 reads resource[7].id (which the translation reproduces as a
// read one past the 7-entry array, landing on resource_2[0]) BEFORE testing i < 7 -- then the i < 7
// check (0x0048d78e/92) fails and breaks. resource_2[0].id is seeded NON-ZERO on purpose: it proves
// the walk is stopped by the BOUND, not by hitting a zero sentinel at slot 7 -- an 8th refund must
// NOT fire even though slot 7's id is non-zero. ratio = (200 * 0.5) / 400 = 0.25.
void test_seven_full_slots_refund_exactly_seven_FINDING() {
    sim_fixture f;
    f.u(PLAYER, UNIT).unit_proto_id = 8;
    f.u(PLAYER, UNIT).energy        = 200.0;
    f.cfg_units[8].energy           = 400.0;
    // ids 1..7, vals 8,16,24,32,40,48,56 -> amounts *0.25 = 2,4,6,8,10,12,14.
    put_resource(f, 8, 0, 1, 8);
    put_resource(f, 8, 1, 2, 16);
    put_resource(f, 8, 2, 3, 24);
    put_resource(f, 8, 3, 4, 32);
    put_resource(f, 8, 4, 5, 40);
    put_resource(f, 8, 5, 6, 48);
    put_resource(f, 8, 6, 7, 56);
    // resource[7] over-read target == resource_2[0]: a non-zero id sentinel that must NOT produce a
    // refund, because the i<7 bound breaks the loop first.
    f.cfg_units[8].resource_2[0].id  = 0x4321;
    f.cfg_units[8].resource_2[0].val = 999999;

    const sim_view v = f.view();
    g_log.reset();
    detail::unit_refund_build_cost_by_health(v, recording_calls(), PLAYER, UNIT);

    ck_eq((uint32_t)g_log.events.size(), 7u,
          "refund FINDING: seven full slots -> exactly seven refunds (the i<7 bound stops the walk, "
          "NOT a zero sentinel at slot 7)");
    ck_ev(0, PLAYER, 1, 2, "refund[0]: (player, id=1, trunc(8*0.25)=2)");
    ck_ev(1, PLAYER, 2, 4, "refund[1]: (player, id=2, trunc(16*0.25)=4)");
    ck_ev(2, PLAYER, 3, 6, "refund[2]: (player, id=3, trunc(24*0.25)=6)");
    ck_ev(3, PLAYER, 4, 8, "refund[3]: (player, id=4, trunc(32*0.25)=8)");
    ck_ev(4, PLAYER, 5, 10, "refund[4]: (player, id=5, trunc(40*0.25)=10)");
    ck_ev(5, PLAYER, 6, 12, "refund[5]: (player, id=6, trunc(48*0.25)=12)");
    ck_ev(6, PLAYER, 7, 14, "refund[6]: (player, id=7, trunc(56*0.25)=14)");
}

} // namespace

void run_unit_refund_tests() {
    test_proportional_refund_stops_at_zero_id();
    test_full_health_ratio_and_truncation_toward_zero_FINDING();
    test_zero_health_still_refunds_zero_per_slot_FINDING();
    test_first_slot_undefined_yields_no_refund();
    test_seven_full_slots_refund_exactly_seven_FINDING();
}

} // namespace mh::sim::test
