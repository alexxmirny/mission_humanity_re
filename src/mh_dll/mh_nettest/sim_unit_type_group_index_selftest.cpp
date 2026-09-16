//
// sim_unit_type_group_index_selftest.cpp -- `simtest` oracle for llm_strat_unit_type_group_index
// @0x00492b60 (sim/sim_unit_type_group_index.h/.cpp).
//
// This row WAS armed for real (shadow_sim1g1_tail.ini) and made ZERO calls across a 20000-step
// all-AI soak, so this offline oracle is the ONLY evidence this function will ever have.
//
// THE CASE THIS FILE EXISTS FOR: reimpl-verify caught a real bug in the first draft -- it cast
// cfg_units[].type to signed int32_t and used SIGNED comparisons, but the disassembly's whole branch
// chain (0x00492b8b-0x00492bd4) is JC/JBE, i.e. UNSIGNED. A value with bit 31 set diverges: unsigned,
// 0xffffffff is far above every threshold and falls through to GROUP_IDX_NONE; signed, (int32_t)
// 0xffffffff == -1, which is < 0xf AND <= 0xa, so the buggy version routed it into GROUP_IDX_INFANTRY
// instead. T13 below pins the CORRECT (unsigned) result -- 0xffffffff -> GROUP_IDX_NONE -- so a future
// "cleanup" back to signed types fails loudly here.
//
// EXPECTED BEHAVIOUR, read directly off the raw CMP/JC/JBE chain (all UNSIGNED) and cross-checked
// against the header banner's band table:
//   type == 0                          -> GROUP_IDX_NONE           (0x00492bad -> 0x00492bb7 -> 0x00492bdd)
//   1 <= type <= 0xa                   -> GROUP_IDX_INFANTRY        (0x00492bb3 taken)
//   0xb <= type <= 0xe                 -> GROUP_IDX_GROUND_VEHICLE  (0x00492bb3 not taken -> 0x00492bb9)
//   0xf <= type <= 0x10                -> GROUP_IDX_HELI            (0x00492b95 taken)
//   0x11 <= type <= 0x12               -> GROUP_IDX_PLANE           (0x00492b9b taken)
//   type >= 0x13 (incl. 0xffffffff)    -> GROUP_IDX_NONE            (0x00492b9b not taken -> the dead
//                                          CMP/JBE at 0x00492b9d/0x00492ba1: both its outcomes land on
//                                          the same 0x00492bdd, so 0x18 is not a real boundary)
//
#include "sim/sim_unit_type_group_index.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// This function's four result codes, restated here (the .cpp's own copies live in its own anonymous
// namespace and are not reachable from this TU) so every check below reads against the SPEC, not
// against "whatever the .cpp happens to return".
constexpr int32_t GROUP_IDX_NONE           = 0x0;
constexpr int32_t GROUP_IDX_GROUND_VEHICLE = 0xf;
constexpr int32_t GROUP_IDX_INFANTRY       = 0x10;
constexpr int32_t GROUP_IDX_HELI           = 0x11;
constexpr int32_t GROUP_IDX_PLANE          = 0x12;

// The cfg_units[] slot every case seeds and looks up. Nonzero so a translation that always reads slot
// 0 cannot pass by coincidence.
constexpr int32_t UNIT_ID = 7;

int32_t call(sim_fixture &fx, uint32_t type) {
    fx.cfg_units[UNIT_ID].type = type;
    return detail::unit_type_group_index(fx.view(), UNIT_ID);
}

} // namespace

void run_unit_type_group_index_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- type == UNDEFINED(0): the outer JC at 0x00492b8f (type < 0xf, unsigned) sends it into the
    // inner chain, then the JC at 0x00492bad (type < 1) takes the type==0 case straight through
    // LAB_00492bb7 to LAB_00492bdd.
    // =================================================================================================
    {
        fx.reset();
        int32_t g = call(fx, 0);
        ck_eq((uint32_t)g, (uint32_t)GROUP_IDX_NONE, "T1: type 0 (UNDEFINED) -> NONE, JC at 0x00492bad taken");
    }

    // =================================================================================================
    // T2 -- type == 1, the FIRST infantry value: JC at 0x00492bad NOT taken (type >= 1), JBE at
    // 0x00492bb3 (type <= 0xa) taken.
    // =================================================================================================
    {
        fx.reset();
        int32_t g = call(fx, 1);
        ck_eq((uint32_t)g, (uint32_t)GROUP_IDX_INFANTRY, "T2: type 1 is the first INFANTRY value, JBE at 0x00492bb3");
    }

    // =================================================================================================
    // T3 -- type == 0xa, the LAST infantry value: JBE at 0x00492bb3 still taken (boundary high side).
    // =================================================================================================
    {
        fx.reset();
        int32_t g = call(fx, 0xa);
        ck_eq((uint32_t)g, (uint32_t)GROUP_IDX_INFANTRY, "T3: type 0xa is the last INFANTRY value, JBE at 0x00492bb3");
    }

    // =================================================================================================
    // T4 -- type == 0xb, the FIRST ground-vehicle value: JBE at 0x00492bb3 NOT taken (type > 0xa) ->
    // fallthrough JMP at 0x00492bb5 to LAB_00492bb9.
    // =================================================================================================
    {
        fx.reset();
        int32_t g = call(fx, 0xb);
        ck_eq((uint32_t)g, (uint32_t)GROUP_IDX_GROUND_VEHICLE,
              "T4: type 0xb is the first GROUND_VEHICLE value, JBE-not-taken at 0x00492bb3");
    }

    // =================================================================================================
    // T5 -- type == 0xe, the LAST ground-vehicle value: still inside the outer type<0xf branch
    // (0x00492b8f), still past the 0xa threshold at 0x00492bb3.
    // =================================================================================================
    {
        fx.reset();
        int32_t g = call(fx, 0xe);
        ck_eq((uint32_t)g, (uint32_t)GROUP_IDX_GROUND_VEHICLE,
              "T5: type 0xe is the last GROUND_VEHICLE value, still under the outer JC at 0x00492b8f");
    }

    // =================================================================================================
    // T6 -- type == 0xf, the FIRST heli value: outer JC at 0x00492b8f NOT taken (type >= 0xf), JBE at
    // 0x00492b95 (type <= 0x10) taken.
    // =================================================================================================
    {
        fx.reset();
        int32_t g = call(fx, 0xf);
        ck_eq((uint32_t)g, (uint32_t)GROUP_IDX_HELI,
              "T6: type 0xf is the first HELI value, outer JC at 0x00492b8f not taken");
    }

    // =================================================================================================
    // T7 -- type == 0x10, the LAST heli value: JBE at 0x00492b95 still taken (boundary high side).
    // =================================================================================================
    {
        fx.reset();
        int32_t g = call(fx, 0x10);
        ck_eq((uint32_t)g, (uint32_t)GROUP_IDX_HELI, "T7: type 0x10 is the last HELI value, JBE at 0x00492b95");
    }

    // =================================================================================================
    // T8 -- type == 0x11, the FIRST plane value: JBE at 0x00492b95 NOT taken (type > 0x10), JBE at
    // 0x00492b9b (type <= 0x12) taken.
    // =================================================================================================
    {
        fx.reset();
        int32_t g = call(fx, 0x11);
        ck_eq((uint32_t)g, (uint32_t)GROUP_IDX_PLANE,
              "T8: type 0x11 is the first PLANE value, JBE-not-taken at 0x00492b95");
    }

    // =================================================================================================
    // T9 -- type == 0x12, the LAST plane value: JBE at 0x00492b9b still taken (boundary high side).
    // =================================================================================================
    {
        fx.reset();
        int32_t g = call(fx, 0x12);
        ck_eq((uint32_t)g, (uint32_t)GROUP_IDX_PLANE, "T9: type 0x12 is the last PLANE value, JBE at 0x00492b9b");
    }

    // =================================================================================================
    // T10 -- type == 0x13, the FIRST value past PLANE: JBE at 0x00492b9b NOT taken -> falls into the
    // dead-code CMP at 0x00492b9d, whose JBE at 0x00492ba1 (type <= 0x18) is taken -> still
    // LAB_00492bdd (NONE).
    // =================================================================================================
    {
        fx.reset();
        int32_t g = call(fx, 0x13);
        ck_eq((uint32_t)g, (uint32_t)GROUP_IDX_NONE,
              "T10: type 0x13 is the first value past PLANE -> NONE, JBE-not-taken at 0x00492b9b");
    }

    // =================================================================================================
    // T11 -- type == 0x18: the dead CMP's own JBE-taken edge at 0x00492ba1 -- still NONE, pinning half
    // of the header's DEAD CODE claim (this edge changes nothing observable).
    // =================================================================================================
    {
        fx.reset();
        int32_t g = call(fx, 0x18);
        ck_eq((uint32_t)g, (uint32_t)GROUP_IDX_NONE,
              "T11: type 0x18 -- dead-code JBE-taken edge at 0x00492ba1, still NONE");
    }

    // =================================================================================================
    // T12 -- type == 0x19: past the dead CMP's own boundary -> the unconditional JMP fallthrough at
    // 0x00492ba3, which lands on the SAME LAB_00492bdd as T11's JBE-taken edge -- pinning the other
    // half of the DEAD CODE claim (both outcomes of the 0x18 compare agree, so it is truly inert).
    // =================================================================================================
    {
        fx.reset();
        int32_t g = call(fx, 0x19);
        ck_eq((uint32_t)g, (uint32_t)GROUP_IDX_NONE,
              "T12: type 0x19 -- unconditional JMP fallthrough at 0x00492ba3, still NONE (dead code confirmed)");
    }

    // =================================================================================================
    // T13 -- THE REGRESSION PIN. type == 0xffffffff (bit 31 set): the outer compare at
    // 0x00492b8b/0x00492b8f is UNSIGNED (JC = below, not JL = less), so 0xffffffff is NOT < 0xf and the
    // outer branch is NOT taken -- it falls through every remaining JBE (0x00492b95, 0x00492b9b,
    // 0x00492ba1) to LAB_00492bdd (NONE). A signed int32_t comparison would read 0xffffffff as -1,
    // WRONGLY take the outer JC, then WRONGLY satisfy "type <= 0xa" and return GROUP_IDX_INFANTRY
    // instead -- exactly the bug reimpl-verify caught and fixed by keeping the field/constants
    // uint32_t and comparing unsigned throughout (matching ai_army_milestone.cpp's treatment of this
    // same field). If this check ever starts observing GROUP_IDX_INFANTRY here, that regression is
    // back.
    // =================================================================================================
    {
        fx.reset();
        int32_t g = call(fx, 0xffffffffu);
        ck_eq((uint32_t)g, (uint32_t)GROUP_IDX_NONE,
              "T13: type 0xffffffff (bit 31 set) -> NONE via UNSIGNED compare at 0x00492b8b/0x00492b8f -- "
              "a signed re-cast would wrongly route this to GROUP_IDX_INFANTRY");
    }
}

} // namespace mh::sim::test
