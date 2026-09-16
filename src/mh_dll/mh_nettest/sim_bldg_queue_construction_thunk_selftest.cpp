//
// sim_bldg_queue_construction_thunk_selftest.cpp -- offline `simtest` oracle for the pure forwarder
//   llm_strat_bldg_queue_construction_thunk @0x004e4e36  (sim/sim_ai_bldg_queue_construction.h/.cpp)
//
// WHY THIS FILE EXISTS. The thunk was translated and passed reimpl-verify weeks ago but was left
// `reviewed` (no execution evidence) because arming its shadow site previously CRASHED the process
// (an ESI-ambient-register prototype issue in the export/shadow marshalling
// glue). This file is the offline alternative: mock the thunk's ONE outward call via its own
// `bldg_queue_construction_thunk_calls` stub table (the header's own precedent for why this callee is
// indirected through a one-member calls struct rather than called directly -- see the header's
// "Per sim_bldg_defense_cost.h's own precedent" paragraph) and drive `detail::bldg_queue_construction_
// thunk` directly, with no live game process involved.
//
// SCOPE OF WHAT THIS PROVES, AND WHAT IT DOES NOT. Per the header banner, the thunk "writes nothing
// directly" and has no branches of its own -- it is a genuine tail-call forwarder (`MOV EAX,ESI ; CALL
// llm_strat_bldg_queue_construction ; JMP <shared epilogue>`). So the entire behavioural surface this
// oracle can (and must) prove is: the C++ body forwards all four parameters to the callee UNCHANGED,
// in the SAME ORDER, at each parameter's DECLARED WIDTH (int32_t, int32_t, int16_t, uint16_t), exactly
// once per call, and discards whatever the callee returns (the thunk's own committed prototype is
// `void`). It does NOT and CANNOT exercise -- and is not the job of this file to explain -- the actual
// G22 crash, which lived in the export/shadow marshalling glue (the non-standard ESI-for-`player`
// storage assignment reaching the shadow trampoline), entirely outside an offline `detail::` call's
// reach. `llm_strat_bldg_queue_construction` itself (the larger sibling in this same file) is already
// `verified`/T1 via a real armed shadow run and is NOT re-tested here.
//
// EVERY EXPECTED VALUE below is either a directly-asserted argument (this IS the assertion, not a
// derived one) or is cited to the header banner's own text (the "writes nothing directly" / "return
// value ... is discarded" claims). Argument values are chosen distinct, non-default, and
// non-symmetric (test 1's 4-tuple has no two equal fields and none is 0/1) so that an argument-order
// swap or a width truncation/sign-extension bug would be caught, and a second, differently-shaped
// 4-tuple (test 2) rules out a swap that happened to agree with test 1's values by coincidence.
//
#include "sim/sim_ai_bldg_queue_construction.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- recorder --------------------------------------------------------------------------------
// One-member `_calls` stub table (per the header's own precedent), converting a captureless lambda
// into `bldg_queue_construction_thunk_calls::queue_construction`'s function-pointer slot.
struct call_rec {
    int32_t  player;
    int32_t  building_type;
    int16_t  x;
    uint16_t y;
};

struct thunk_recorder {
    std::vector<call_rec> calls;
    // control knob -- llm_strat_bldg_queue_construction's own return (0 == appended, 1 == queue
    // full); the thunk's committed prototype is `void`, so this exists purely to prove the thunk
    // does not branch on it (see test_thunk_ignores_callee_return_value below).
    int32_t ret = 0;
    void    reset() { *this = thunk_recorder{}; }
};
thunk_recorder g_th;

const bldg_queue_construction_thunk_calls &rec_th_calls() {
    static const bldg_queue_construction_thunk_calls c = {
        [](int32_t player, int32_t building_type, int16_t x, uint16_t y) -> int32_t {
            g_th.calls.push_back({player, building_type, x, y});
            return g_th.ret;
        },
    };
    return c;
}

// ---- tests -------------------------------------------------------------------------------------

void test_thunk_forwards_all_args_exactly_in_order_and_width() {
    g_th.reset();
    g_th.ret = 0;

    detail::bldg_queue_construction_thunk(rec_th_calls(), 5, 13, (int16_t)-1000, (uint16_t)50000);

    ck_eq((uint32_t)g_th.calls.size(), 1u, "thunk: exactly one forwarded call");
    ck(g_th.calls.size() == 1 && g_th.calls[0].player == 5 && g_th.calls[0].building_type == 13 &&
           g_th.calls[0].x == (int16_t)-1000 && g_th.calls[0].y == (uint16_t)50000,
       "thunk: forwards (player=5, building_type=13, x=-1000, y=50000) to "
       "llm_strat_bldg_queue_construction unchanged, in argument order, at each parameter's "
       "declared width (int32_t/int32_t/int16_t/uint16_t) -- distinct, non-default, non-symmetric "
       "values so an order swap or a truncation/sign-extension bug would be caught");
}

void test_thunk_forwards_a_second_distinct_arg_set_rules_out_coincidental_order() {
    g_th.reset();

    detail::bldg_queue_construction_thunk(rec_th_calls(), 42, 200, (int16_t)7, (uint16_t)3);

    ck(g_th.calls.size() == 1 && g_th.calls[0].player == 42 && g_th.calls[0].building_type == 200 &&
           g_th.calls[0].x == 7 && g_th.calls[0].y == 3,
       "thunk: a second, differently-shaped call (player=42, building_type=200, x=7, y=3) still "
       "maps 1:1 onto (player, building_type, x, y) -- rules out an argument-order swap that "
       "happened to agree with test 1's values by coincidence");
}

void test_thunk_ignores_callee_return_value() {
    g_th.reset();
    g_th.ret = 1; // llm_strat_bldg_queue_construction's "queue already full" return

    detail::bldg_queue_construction_thunk(rec_th_calls(), 9, 21, (int16_t)-1, (uint16_t)1);

    ck_eq((uint32_t)g_th.calls.size(), 1u,
          "thunk(callee returns 1, queue-full): still exactly one forwarded call -- the header "
          "banner's own claim that 'the return value ... is discarded' (the thunk's committed "
          "prototype is void) means nothing about the thunk's behaviour can depend on it");

    g_th.reset();
    g_th.ret = 0; // llm_strat_bldg_queue_construction's "appended" success return

    detail::bldg_queue_construction_thunk(rec_th_calls(), 9, 21, (int16_t)-1, (uint16_t)1);

    ck_eq((uint32_t)g_th.calls.size(), 1u,
          "thunk(callee returns 0, appended): the SAME call shape with the OTHER callee return "
          "value still produces exactly one forwarded call -- confirms the thunk does not branch "
          "on the return value at all");
}

} // namespace

void run_bldg_queue_construction_thunk_tests() {
    test_thunk_forwards_all_args_exactly_in_order_and_width();
    test_thunk_forwards_a_second_distinct_arg_set_rules_out_coincidental_order();
    test_thunk_ignores_callee_return_value();
}

} // namespace mh::sim::test
