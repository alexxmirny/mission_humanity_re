//
// crt/crt_st0_sweep.h -- the input sweep for the ST0 sin/cos equivalence proof (LIB-REF-SPLIT).
//
// ONE GENERATOR, TWO CONSUMERS, AND THAT IS THE WHOLE POINT. The hosted probe
// (seams/reimpl_probe.cpp) walks this list calling the ORIGINAL at its VA and the vendored body in
// crt/crt_math.h, and the permanent offline arm (mh_nettest/crt_vendor_selftest.cpp) walks the SAME
// list comparing the vendored body against the goldens that probe produced. If the two walked
// different lists the goldens would be indexed against inputs nobody re-derived, which is a fixture
// that rots silently -- so the list lives here and neither consumer owns it.
//
// WHY THE GOLDENS EXIST AT ALL. Every other body in crt_math.h is proved against a reference arm
// transcribed from the original's machine code into the test TU (LIB-CRT's doctrine, and the reason
// crt_vendor_selftest can run with no game). That does not work here: the vendored sin/cos IS that
// transcription, so a transcribed reference would be the same instructions compared against
// themselves -- an oracle that agrees with the bug by construction. The non-circular reference is
// the ORIGINAL MACHINE CODE AT ITS ADDRESS, which only exists in a hosted process. So it is measured
// once, hosted, and the measurement is committed.
//
// ---- WHAT THE SWEEP HAS TO COVER, and why each band is here ------------------------------------
//
//   A. THE CALLERS' REAL RANGE. ai_group_task_movement.cpp / ai_group_task_workers.cpp compute
//      `rand_state_advance(2) * loiter_angle_scale` and floor the sin/cos into tile offsets. That is
//      a small multiple of a scale factor, so the band that actually ships is a few turns either
//      side of zero -- swept densely, because this is the band a divergence would reach the lockstep
//      hash through.
//   B. THE C2 PATH. FSIN/FCOS refuse and set C2 when |x| >= 2^63, leaving the operand untouched;
//      that is the ONLY way the FPREM reduction loop in the vendored body executes at all. Without
//      inputs above that threshold the loop is dead code in every run and the sweep would prove the
//      easy half. Several magnitudes past it, so the loop iterates a different number of times.
//   C. EXACT QUADRANT POINTS. 0, +-pi/2, +-pi, +-2pi: where sin and cos take their extreme values
//      and where a reduction that is off by one ulp of the divisor shows up as a sign flip.
//   D. THE DEGENERATE INPUTS. +-0 (sign of zero survives sin), the smallest normals, and the
//      largest finite double -- which is also band B, but reached from the other end.
//
// NaN and the infinities are deliberately ABSENT. FSIN/FCOS return the indefinite QNaN for them and
// set C1, and the original's helper does not look at C1 -- so both arms produce a QNaN whose payload
// is a property of the FPU, not of either body, and comparing payload bits would pin hardware rather
// than equivalence. The callers cannot produce them either: `rand_state_advance(2) * scale` is
// finite for every reachable state.
#pragma once
#include <cstdint>

namespace mh::crt {

// The count is a compile-time constant so the goldens table can be sized against it and a mismatch
// is a build error rather than a short walk.
inline constexpr int ST0_SWEEP_N = 96;

// Deterministic, index-addressed, and free of any floating-point arithmetic that could itself differ
// between the two builds: every value is either an exact small integer scaled by a literal, or a bit
// pattern stated directly. A sweep that computed its own inputs with the arithmetic under test would
// be measuring that arithmetic twice.
inline double st0_sweep_input(int i) {
    // Band C + D: the exact points, stated as bit patterns where the value has no short decimal.
    static const uint64_t kExact[] = {
        0x0000000000000000ull, // +0.0
        0x8000000000000000ull, // -0.0
        0x3FF921FB54442D18ull, // +pi/2
        0xBFF921FB54442D18ull, // -pi/2
        0x400921FB54442D18ull, // +pi
        0xC00921FB54442D18ull, // -pi
        0x401921FB54442D18ull, // +2pi
        0xC01921FB54442D18ull, // -2pi
        0x0010000000000000ull, // smallest positive normal
        0x8010000000000000ull, // smallest negative normal
        0x7FEFFFFFFFFFFFFFull, // largest finite -- also band B, from the far end
        0xFFEFFFFFFFFFFFFFull,
    };
    constexpr int kExactN = (int)(sizeof(kExact) / sizeof(kExact[0]));
    if (i < kExactN) {
        double d = 0.0;
        // memcpy would drag <cstring> into a header two very different TUs include; a union punt is
        // the same thing and keeps this header dependency-free.
        union {
            uint64_t u;
            double   d;
        } p;
        p.u = kExact[i];
        d   = p.d;
        return d;
    }
    i -= kExactN;

    // Band A: the callers' band. 60 points across +-3 turns, at 1/10 radian, both signs.
    if (i < 60) {
        const int k = i - 30; // -30 .. 29
        return (double)k / 10.0 * 2.0;
    }
    i -= 60;

    // Band B: past 2^63, where FSIN/FCOS set C2 and the FPREM loop is the only way an answer exists.
    // 2^63 exactly, then powers climbing away from it, then a few odd multiples so the reduction
    // lands on different remainders rather than repeating one shape.
    static const double kBig[] = {
        9223372036854775808.0,     // 2^63, the threshold itself
        9223372036854775808.0 * 2, // 2^64
        9223372036854775808.0 * 3,
        9223372036854775808.0 * 17,
        9223372036854775808.0 * 1024,
        1.0e20,
        1.0e30,
        1.0e60,
        1.0e100,
        1.0e200,
        -9223372036854775808.0,
        -1.0e20,
        -1.0e30,
        -1.0e100,
        -1.0e200,
        1.7976931348623157e308, // near the top, so the loop runs its longest
        -1.7976931348623157e308,
        123456789012345678901234.0,
        -987654321098765432109876.0,
        3.0e18, // just UNDER the threshold: FSIN answers directly, no loop -- the control
        -3.0e18,
        1.0e15,
        -1.0e15,
        1.0e10,
    };
    constexpr int kBigN = (int)(sizeof(kBig) / sizeof(kBig[0]));
    return kBig[i < kBigN ? i : kBigN - 1];
}

} // namespace mh::crt
