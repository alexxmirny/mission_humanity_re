//
// sim_dmp_enqueue_scripted_order_selftest.cpp -- `simtest` oracle for
// llm_strat_dmp_enqueue_scripted_order @0x0046d873 (sim/sim_dmp_enqueue_scripted_order.h/.cpp,
// RI-SIM / SIM1-G4).
//
// detail::dmp_enqueue_scripted_order(calls, x, y, unit_type_id, owner_and_kind) takes the
// `dmp_enqueue_scripted_order_calls` mock struct directly -- there is no sim_view/sim_store
// parameter at all (pure delegation, no roster read), so this file needs no sim_fixture.
//
// EXPECTED BEHAVIOUR from the header banner's own derivation (sim_dmp_enqueue_scripted_order.h) and
// the disassembly (tmp/decomp_sim/llm_strat_dmp_enqueue_scripted_order_0046d873.asm):
//   0x0046d894: order_scratch_reset() called FIRST, before any other call.
//   0x0046d899-0x0046d8a1: order_scratch_set_field(4, x)             -- EAX=4, EDX=x
//   0x0046d8a6-0x0046d8ae: order_scratch_set_field(5, y)             -- EAX=5, EDX=y
//   0x0046d8b3-0x0046d8bb: order_scratch_set_field(1, unit_type_id)  -- EAX=1, EDX=unit_type_id
//   0x0046d8c0-0x0046d8d0: order_enqueue(unit_index=0, owner_and_kind=<input, UNCHANGED>,
//     param0=0xeb, order_code=0xeb). ECX/EBX both loaded with the immediate 0xeb; EDX loaded from
//     the incoming param_4 (owner_and_kind) with no OR/mask applied; EAX zeroed (unit_index=0).
//   0x0046d8d5: return 1 unconditionally -- order_enqueue's own return value (EAX at 0x0046d8d0) is
//     never read after the call; dword [EBP-0x10] is set to the literal 1.
//
// No branches anywhere in this function -- all five calls (reset, 3x set_field, enqueue) are
// unconditional and always execute in this fixed order. The cases below are therefore entirely
// about pinning ORDER and ARGUMENT MAPPING precisely, not about branch coverage.
//
#include "sim/sim_dmp_enqueue_scripted_order.h"

#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// A single log of WHICH mock fired, in firing order, shared across all three recorders -- this is
// what lets a case see the [reset, set_field, set_field, set_field, enqueue] sequence rather than
// just each mock's own isolated call count.
enum class call_kind : uint32_t { RESET,
                                  SET_FIELD,
                                  ENQUEUE };
std::vector<call_kind> g_call_order;

int32_t g_reset_calls = 0;
void    rec_order_scratch_reset() {
    ++g_reset_calls;
    g_call_order.push_back(call_kind::RESET);
}

struct set_field_call {
    int32_t index;
    int32_t value;
};
std::vector<set_field_call> g_set_field_calls;
void                        rec_order_scratch_set_field(int32_t index, int32_t value) {
    g_set_field_calls.push_back({index, value});
    g_call_order.push_back(call_kind::SET_FIELD);
}

struct enqueue_call {
    uint16_t unit_index;
    uint16_t owner_and_kind;
    int16_t  param0;
    uint16_t order_code;
};
std::vector<enqueue_call> g_enqueue_calls;
int32_t                   g_enqueue_return = 0;
int32_t                   rec_order_enqueue(uint16_t unit_index, uint16_t owner_and_kind, int16_t param0,
                                            uint16_t order_code) {
    g_enqueue_calls.push_back({unit_index, owner_and_kind, param0, order_code});
    g_call_order.push_back(call_kind::ENQUEUE);
    return g_enqueue_return;
}

const dmp_enqueue_scripted_order_calls g_calls = {
    &rec_order_scratch_reset,
    &rec_order_scratch_set_field,
    &rec_order_enqueue,
};

void reset_recorders() {
    g_call_order.clear();
    g_reset_calls = 0;
    g_set_field_calls.clear();
    g_enqueue_calls.clear();
    g_enqueue_return = 0;
}

// Checks the shared call-order log against the fixed 5-call sequence this function's body always
// makes: reset, then the three set_field stores (in scratch-index order 4/5/1), then enqueue.
// 0x0046d894 (reset) precedes 0x0046d899 (first set_field) precedes 0x0046d8c0 (enqueue) in the
// disassembly with no branch between them, so any case that got the order wrong shows up here.
void ck_call_order(const char *what) {
    const std::vector<call_kind> want = {
        call_kind::RESET,
        call_kind::SET_FIELD,
        call_kind::SET_FIELD,
        call_kind::SET_FIELD,
        call_kind::ENQUEUE,
    };
    ck_eq((uint32_t)g_call_order.size(), (uint32_t)want.size(), what);
    const size_t n = g_call_order.size() < want.size() ? g_call_order.size() : want.size();
    for (size_t i = 0; i < n; ++i) {
        ck_eq((uint32_t)g_call_order[i], (uint32_t)want[i], what);
    }
}

} // namespace

void run_dmp_enqueue_scripted_order_tests() {
    // =================================================================================================
    // T1 -- first value set. Pins: call order (reset first, then set_field x3, then enqueue,
    // 0x0046d894-0x0046d8d0), the scratch-field index<->argument mapping with three DISTINCT
    // non-symmetric values (a swapped mapping like field 4<-y would fail this), unit_index==0
    // always (0x0046d8ce), owner_and_kind passed through UNCHANGED including pre-set bits
    // (0x0046d8ca, no OR/mask), param0==order_code==0xeb (0x0046d8c0/0x0046d8c5), and the return
    // value pinned to 1 even though the mock order_enqueue returns something else (0, here) --
    // 0x0046d8d5 never reads order_enqueue's own EAX result.
    // =================================================================================================
    {
        reset_recorders();
        constexpr uint32_t X              = 111;
        constexpr uint32_t Y              = 222;
        constexpr uint32_t UNIT_TYPE_ID   = 333;
        constexpr uint16_t OWNER_AND_KIND = 0x1234; // pre-set bits: must arrive unchanged
        g_enqueue_return                  = 0;      // deliberately NOT 1 -- return must still be 1

        uint32_t ret =
            detail::dmp_enqueue_scripted_order(g_calls, X, Y, UNIT_TYPE_ID, OWNER_AND_KIND);

        ck_call_order("T1: call order is [reset, set_field, set_field, set_field, enqueue], "
                      "0x0046d894-0x0046d8d0");
        ck_eq((uint32_t)g_reset_calls, 1u, "T1: order_scratch_reset called exactly once, 0x0046d894");
        ck_eq((uint32_t)g_set_field_calls.size(), 3u, "T1: order_scratch_set_field called exactly 3x");
        ck_eq((uint32_t)g_set_field_calls[0].index, 4u, "T1: 1st set_field index==4 (EAX), 0x0046d89c");
        ck_eq((uint32_t)g_set_field_calls[0].value, X, "T1: 1st set_field value==x (EDX), 0x0046d899");
        ck_eq((uint32_t)g_set_field_calls[1].index, 5u, "T1: 2nd set_field index==5 (EAX), 0x0046d8a9");
        ck_eq((uint32_t)g_set_field_calls[1].value, Y, "T1: 2nd set_field value==y (EDX), 0x0046d8a6");
        ck_eq((uint32_t)g_set_field_calls[2].index, 1u, "T1: 3rd set_field index==1 (EAX), 0x0046d8b6");
        ck_eq((uint32_t)g_set_field_calls[2].value, UNIT_TYPE_ID,
              "T1: 3rd set_field value==unit_type_id (EDX), 0x0046d8b3");
        ck_eq((uint32_t)g_enqueue_calls.size(), 1u, "T1: order_enqueue called exactly once, 0x0046d8d0");
        ck_eq((uint32_t)g_enqueue_calls[0].unit_index, 0u,
              "T1: order_enqueue unit_index==0 always (EAX zeroed), 0x0046d8ce");
        ck_eq((uint32_t)g_enqueue_calls[0].owner_and_kind, (uint32_t)OWNER_AND_KIND,
              "T1: order_enqueue owner_and_kind == input UNCHANGED, no OR/mask, 0x0046d8ca");
        ck_eq((uint32_t)(uint16_t)g_enqueue_calls[0].param0, 0xebu,
              "T1: order_enqueue param0==0xeb (ECX), 0x0046d8c0");
        ck_eq((uint32_t)g_enqueue_calls[0].order_code, 0xebu,
              "T1: order_enqueue order_code==0xeb (EBX), 0x0046d8c5");
        ck_eq(ret, 1u,
              "T1: return value is always 1 regardless of order_enqueue's own return, 0x0046d8d5");
    }

    // =================================================================================================
    // T2 -- second, entirely different value set, and a NEGATIVE mock return -- proves nothing from
    // T1 leaked (recorders reset), the mapping holds for a second distinct triple, owner_and_kind
    // with a different pre-set bit pattern still arrives unchanged, and the return value is STILL 1
    // even when order_enqueue itself returns -1 (not just some other positive/zero value).
    // =================================================================================================
    {
        reset_recorders();
        constexpr uint32_t X              = 777;
        constexpr uint32_t Y              = 888;
        constexpr uint32_t UNIT_TYPE_ID   = 999;
        constexpr uint16_t OWNER_AND_KIND = 0xABCD;
        g_enqueue_return                  = -1;

        uint32_t ret =
            detail::dmp_enqueue_scripted_order(g_calls, X, Y, UNIT_TYPE_ID, OWNER_AND_KIND);

        ck_call_order("T2: call order is [reset, set_field, set_field, set_field, enqueue] again, "
                      "0x0046d894-0x0046d8d0");
        ck_eq((uint32_t)g_reset_calls, 1u, "T2: order_scratch_reset called exactly once, 0x0046d894");
        ck_eq((uint32_t)g_set_field_calls.size(), 3u, "T2: order_scratch_set_field called exactly 3x");
        ck_eq((uint32_t)g_set_field_calls[0].index, 4u, "T2: 1st set_field index==4 (EAX), 0x0046d89c");
        ck_eq((uint32_t)g_set_field_calls[0].value, X, "T2: 1st set_field value==x (EDX), 0x0046d899");
        ck_eq((uint32_t)g_set_field_calls[1].index, 5u, "T2: 2nd set_field index==5 (EAX), 0x0046d8a9");
        ck_eq((uint32_t)g_set_field_calls[1].value, Y, "T2: 2nd set_field value==y (EDX), 0x0046d8a6");
        ck_eq((uint32_t)g_set_field_calls[2].index, 1u, "T2: 3rd set_field index==1 (EAX), 0x0046d8b6");
        ck_eq((uint32_t)g_set_field_calls[2].value, UNIT_TYPE_ID,
              "T2: 3rd set_field value==unit_type_id (EDX), 0x0046d8b3");
        ck_eq((uint32_t)g_enqueue_calls.size(), 1u, "T2: order_enqueue called exactly once, 0x0046d8d0");
        ck_eq((uint32_t)g_enqueue_calls[0].unit_index, 0u,
              "T2: order_enqueue unit_index==0 always (EAX zeroed), 0x0046d8ce");
        ck_eq((uint32_t)g_enqueue_calls[0].owner_and_kind, (uint32_t)OWNER_AND_KIND,
              "T2: order_enqueue owner_and_kind == input UNCHANGED, no OR/mask, 0x0046d8ca");
        ck_eq((uint32_t)(uint16_t)g_enqueue_calls[0].param0, 0xebu,
              "T2: order_enqueue param0==0xeb (ECX), 0x0046d8c0");
        ck_eq((uint32_t)g_enqueue_calls[0].order_code, 0xebu,
              "T2: order_enqueue order_code==0xeb (EBX), 0x0046d8c5");
        ck_eq(ret, 1u,
              "T2: return value is still 1 even when order_enqueue returns -1, 0x0046d8d5");
    }
}

} // namespace mh::sim::test
