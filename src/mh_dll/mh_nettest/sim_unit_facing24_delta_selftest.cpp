//
// sim_unit_facing24_delta_selftest.cpp -- `simtest` cases for llm_strat_facing24_to_delta
// (sim/sim_unit_facing24_delta.h/.cpp), SIM1A. This is the ONLY possible oracle for this function:
// its shadow site is VACUOUS (it writes only through its two OUT-pointer params, so the differential
// arm compares zero regions -- 195202 calls armed, nothing compared, in the sixth-slice soak). Same
// posture as sim_facing24_from_points_selftest.cpp, its inverse-direction sibling.
//
// PURE LOOKUP, NO STATE. detail::facing24_to_delta(facing24, &dx, &dy) is called with literal args;
// there is no sim_view/sim_store to build.
//
// THE EXPECTED TABLE BELOW WAS DERIVED INDEPENDENTLY FROM THE DISASSEMBLY, not read off the .cpp
// (which would only prove the test agrees with the code it is testing). Trace of
// tmp/decomp/llm_strat_facing24_to_delta_0049610f.asm: out_dx = EDX = [EBP-0x14], out_dy = EBX =
// [EBP-0x10]; the if/else ladder dispatches on facing24 alone and each leaf stores two literal
// dwords:
//   LAB_00496200 (==1):  dx=0,  dy=1        LAB_004961a7 (==13): dx=0,  dy=-1(0xffffffff)
//   LAB_00496214 (==4):  dx=-1, dy=1        LAB_004961be (==16): dx=1,  dy=-1
//   LAB_00496228 (==7):  dx=-1, dy=0        LAB_004961d5 (==19): dx=1,  dy=0
//   LAB_0049623c (==10): dx=-1, dy=-1       LAB_004961ec (==22): dx=1,  dy=1
//   LAB_00496250 (default, reached by every other value INCLUDING >=23): dx=0, dy=0
// The ladder never bounds facing24 from above except the explicit `==22` (JZ) test, so 25 -- which
// satisfies the tempting `facing24 % 3 == 1` restatement -- takes the default and yields (0,0), the
// header's own hazard case. Covered explicitly below.
//
#include "sim/sim_unit_facing24_delta.h"

#include <cstdio>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

void check_delta(uint32_t facing24, int32_t want_dx, int32_t want_dy) {
    // Seed the out-params with a poison value no branch writes, so a function that returned without
    // writing an axis would be caught rather than reading as (0,0).
    int32_t dx = 0x0badf00d;
    int32_t dy = 0x0badf00d;
    detail::facing24_to_delta(facing24, &dx, &dy);
    char what[96];
    std::snprintf(what, sizeof(what), "facing24_to_delta(%u).dx", facing24);
    ck_eq((uint32_t)dx, (uint32_t)want_dx, what);
    std::snprintf(what, sizeof(what), "facing24_to_delta(%u).dy", facing24);
    ck_eq((uint32_t)dy, (uint32_t)want_dy, what);
}

} // namespace

void run_unit_facing24_delta_tests() {
    // The eight on-axis codes (asm-derived above).
    check_delta(1, 0, 1);
    check_delta(4, -1, 1);
    check_delta(7, -1, 0);
    check_delta(10, -1, -1);
    check_delta(13, 0, -1);
    check_delta(16, 1, -1);
    check_delta(19, 1, 0);
    check_delta(22, 1, 1);

    // Every OTHER value in [0,24] -> the shared default (0,0). Enumerated (not looped-with-a-skip)
    // so a reader can see exactly which inputs are asserted default, including the ones adjacent to a
    // real branch (2,3 around 1; 5,6,8,9 around 4/7; 11,12,14,15 around 13; 17,18 below 19; 20,21,23
    // above 19/around 22 -- the fall-through gaps the asm's JC/JMP paths take).
    const uint32_t defaults[] = {0, 2, 3, 5, 6, 8, 9, 11, 12, 14, 15, 17, 18, 20, 21, 23, 24};
    for (uint32_t f : defaults) check_delta(f, 0, 0);

    // Hazard cases (header banner): values that satisfy `f % 3 == 1` but are NOT ladder branches must
    // STILL default -- the ladder is eight literal tests, not a modulus. 25 % 3 == 1; 0xFFFFFFFF is
    // the far end of the unsigned domain (also not a branch). Both -> (0,0).
    check_delta(25, 0, 0);
    check_delta(28, 0, 0); // 28 % 3 == 1, likewise not a branch
    check_delta(0xFFFFFFFFu, 0, 0);
}

} // namespace mh::sim::test
