#include "sim/sim_prod_planet_distance_factor.h"

#include <cstdint>
#include <cstdio>
#include <limits>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

void check_factor(int32_t src_planet, int32_t dest_planet, const char *what) {
    const double got = detail::planet_distance_factor(src_planet, dest_planet);
    ck_eq_d(got, 1.0, what);
}

} // namespace

void run_prod_planet_distance_factor_tests() {
    // ---- T1: SRC == DEST (same planet index, e.g. a shuttle "departing" to its own planet). ----
    // Pins the constant-pool FLD at 0x00490258 (bit pattern 0x00000000/0x3ff00000 = 1.0 exactly).
    check_factor(5, 5, "T1: src_planet==dest_planet (5,5) -> pool constant 1.0, FLD at 0x00490258");

    // ---- T2/T3: distinct planet indices, BOTH orderings. The .asm never reads either register past
    // the stash at 0x00490238 (src_planet)/0x0049023b (dest_planet), so an argument-swap mutation
    // (or any mutation that made the result depend on ordering) must be caught by these two agreeing
    // -- both must land on the identical 1.0 despite the arguments being swapped, not merely equal.
    check_factor(2, 9, "T2: src_planet(2) < dest_planet(9), distinct indices -> still 1.0, "
                       "params never read again past 0x0049023b");
    check_factor(9, 2, "T3: dest_planet(2) < src_planet(9) -- the mirrored ordering of T2 -- still "
                       "the identical 1.0, pinning order-independence (argument swap can't change it)");

    // ---- T4: the zero/first-index boundary, both args at the same end. ----
    check_factor(0, 0, "T4: src_planet==dest_planet==0 (first-index boundary) -> 1.0, no special-case "
                       "branch exists at index 0 (no branch exists at all)");

    // ---- T5: the -1 sentinel this codebase uses elsewhere for "no planet" -- both slots, so a
    // mutation that special-cased the sentinel (e.g. treating -1 as "invalid" and returning something
    // else) would be caught here.
    check_factor(-1, -1, "T5: both args == the -1 \"no planet\" sentinel -> still 1.0, no sentinel "
                         "special-case exists in the .asm");

    // ---- T6/T7: the int32 domain's extreme ends, both orderings, standing in for the "ends of the
    // planet table" the .asm has none of -- pins that nothing clamps, saturates, or crashes on an
    // out-of-any-plausible-range index; the function still falls straight into the same FLD.
    check_factor(std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max(),
                 "T6: src_planet=INT32_MIN, dest_planet=INT32_MAX -> still 1.0, no clamp/bounds check "
                 "exists between the prologue and the FLD");
    check_factor(std::numeric_limits<int32_t>::max(), std::numeric_limits<int32_t>::min(),
                 "T7: mirrored ordering of T6 (src_planet=INT32_MAX, dest_planet=INT32_MIN) -> still "
                 "1.0, same single path");
}

} // namespace mh::sim::test
