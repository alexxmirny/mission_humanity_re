//
// tact/tact_calc_dir24.cpp -- see tact_calc_dir24.h. Translated from the DISASSEMBLY, not from
// Ghidra's C.
//
#include "tact/tact_calc_dir24.h"

#include "addr/mh_calls.gen.h" // MH_CRT(llm_math_atan) -- the one outward call this TU binds live
#include "fp/x87_shapes.h"     // CRT-X87: the hoisted x87 blocks (the asm moved, it did not change)
#include "crt/crt_select.h"    // LIB-CRT: MH_CRT() picks the vendored CRT in the standalone build

namespace mh::tact {

const calc_dir24_calls &live_calc_dir24_calls() {
    static const calc_dir24_calls c = {
        MH_CRT(llm_math_atan),
    };
    return c;
}

namespace {

// 0x00500452's tail (Ghidra: "s__00500452+8") -- see the header banner: a genuine 180.0 double that
// a separate, overlapping string data item obscures the address of.
inline constexpr double DEG_PER_RAD = 180.0;
// 0x00500462, Ghidra symbol "M_PI" -- the game's own truncated pi (3.1415926536 exactly), NOT
// bit-identical to std pi. See the header banner.
inline constexpr double PI_APPROX = 3.1415926536;

// 0x0042e13f-0x0042e167: (angle_rad * DEG_PER_RAD) / PI_APPROX, truncated toward zero, ENTIRELY in
// the x87 register with no intermediate 64-bit store between the multiply/divide and the trunc call
// (no FSTP between FDIV at 0x0042e142 and CALL utils_math_trunc at 0x0042e162) -- same
// extended-precision hazard sim_facing24_from_points.cpp's sector_index_from_angle documents, so
// reproduced the same way: one hand-written __asm block, not a C++ expression + cast.
int32_t atan_rad_to_int_deg(double angle_rad) {
    return ::mh::fp::trunc_mul_div(angle_rad, DEG_PER_RAD, PI_APPROX);
}

} // namespace

namespace detail {

int32_t calc_dir24(int32_t x1, int32_t y1, int32_t x2, int32_t y2, const calc_dir24_calls &c) {
    // 0x0042e106-0x0042e11c: early-out on exact coincidence.
    if (x1 == x2 && y1 == y2) {
        return 1;
    }

    // 0x0042e121-0x0042e13c: both deltas as a real FILD/FSTP round-trip to double (matches a plain
    // C++ `(double)` cast of an exact int value -- no precision loss possible for tile-range values).
    const double dx = (double)(x1 - x2); // 0x0042e121-0x0042e12d
    const double dy = (double)(y2 - y1); // 0x0042e130-0x0042e13c

    // 0x0042e13f-0x0042e14b: atan(dx / dy), through the indirection (see header) rather than
    // MH_CRT(llm_math_atan) directly, so net_selftest.exe tacttest can drive this offline. UNGUARDED
    // for dy == 0.0, exactly as the original leaves it.
    const double angle_rad = c.atan(dx / dy);

    // 0x0042e13f-0x0042e167: see atan_rad_to_int_deg -- truncated to an INT here, unlike the sim
    // sibling which stays in the double domain through the sector division.
    int32_t angle_deg = atan_rad_to_int_deg(angle_rad);

    // 0x0042e16a-0x0042e17b: FCOMP 0.0 vs dy; JBE (0.0<=dy) skips -- applies when dy < 0.0 (atan's
    // [-90,90] domain needing a half-turn flip for the other two quadrants). Plain INTEGER add from
    // here on (dword, not double ptr).
    if (dy < 0.0) {
        angle_deg += 180;
    }

    // 0x0042e17b-0x0042e18b: wrap DOWN when angle_deg >= 360.
    if (angle_deg >= 360) {
        angle_deg += -360;
    }

    // 0x0042e18b-0x0042e198: wrap UP when angle_deg < 0.
    if (angle_deg < 0) {
        angle_deg += 360;
    }

    // 0x0042e198-0x0042e1ac: sector = angle_deg / 15 (IDIV, truncates toward zero -- angle_deg is
    // already >= 0 here, so this matches ordinary C++ integer division unambiguously), then +1.
    int32_t sector = angle_deg / 15;
    sector += 1;

    // 0x0042e1ac-0x0042e1b9: clamp an out-of-range sector back to 1 (reachable only if the wrap steps
    // above left angle_deg >= 360, which they are written to prevent -- preserved literally as a
    // belt-and-braces original check, not simplified away).
    if (sector > 24) {
        sector = 1;
    }

    return sector;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

int32_t calc_dir24(int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    return detail::calc_dir24(x1, y1, x2, y2, live_calc_dir24_calls());
}


} // namespace mh::tact
