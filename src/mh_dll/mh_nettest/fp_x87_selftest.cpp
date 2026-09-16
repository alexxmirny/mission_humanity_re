//
// fp_x87_selftest.cpp -- `fptest`: mh/fp/x87.h still equals the assembly it replaced, and the one
// replacement that was REFUSED stays refused.
//
// WHY THIS FILE HOLDS A COPY OF THE ASSEMBLY. CRT-X87 step 1 hoisted 60 inline `__asm` blocks into
// two named helpers. Step 2 asked whether each could become C++; an offline probe said yes to both,
// and this suite -- the same comparison run through the real header -- said yes to ONE. That is the
// whole reason the reference arm lives here rather than in a scratchpad: a probe proves a claim once,
// on one compiler version, at one optimisation level, in codegen that may not resemble the real
// thing's. MEASURED (2026-09-08): the probe swept ~200k inputs at both precision settings with
// libmh's own flags and reported ZERO divergences; compiled as part of this header, against real
// call sites, the identical comparison reported 1743 at PC=64 on its first run. Whether an x87
// intermediate stays at 80 bits is an inlining/spill decision, not a property of the source.
//
// So the ORIGINAL assembly lives here, verbatim. The helpers are the subject. Every run re-derives
// the equality instead of trusting a note in a header, which means a compiler upgrade, a flag change,
// or someone "simplifying" a helper is caught by a red suite rather than by a desync three sessions
// later.
//
// WHAT THE EXPERIMENT ESTABLISHED, and what this suite keeps checking:
//   * ONLY `trunc_i32` became C++. It agrees with the assembly at BOTH x87 precision settings, which
//     matters because the DLL harness pins PC=53 (harness.cpp `pin_fpu`, default on) while the bare
//     x87 default is PC=64 -- the code runs under both, and a helper correct at only one would look
//     fine for months.
//   * The out-of-range and non-finite edge agrees BY VALUE -- INT_MAX+1, +/-1e30, +/-inf and NaN all
//     produce 0x80000000 through `fistp` and through the cast. MSVC compiles the cast to a call to
//     __ftol2_sse, not to an inline `fistp`, so this is two different code paths agreeing rather than
//     one path compared with itself.
//   * `trunc_i32_mul` STAYED ASSEMBLY, and case M pins why. THIS SUITE IS WHAT REFUSED IT: an offline
//     probe with both arms as static functions in one TU reported zero divergences and kept the
//     product in ST(0), but compiled as part of x87.h against real call sites MSVC spills it to a
//     64-bit temp -- and this file reported 1743 divergences at PC=64 on its first run. The probe's
//     codegen was not representative. Case M keeps the refusal measurable so it is not re-litigated.
//   * THE NEGATIVE ARM (case N) is the reason any zero here is evidence. It forces the product
//     through a volatile double -- rounding the intermediate to 53 bits -- and requires that to
//     CHANGE the answer at PC=64. If it ever stops diverging, the sweep has gone blind.
//
// CASES C1..C5, added 2026-09-11 (CRT-X87-CPP step 2), cover the five `fcompp`/`fnstsw` compare
// helpers in mh/fp/x87_shapes.h -- five separate proofs, because they test five different flags.
// Three cleared and are C++ now (C3/C4/C5); two are pinned REFUSALS in the shape of case M (C1/C2).
// Each case runs at both precision settings and carries three arms besides the subject:
//   * the WRONG SPELLING -- plausible C++ that differs only on an unordered compare. It must keep
//     diverging, or the sweep is not actually reaching the NaN placements it claims to cover.
//   * the WIDTH probe -- the same predicate with the quotient forced through a volatile double.
//     Asserted in BOTH directions: > 0 where a 53-vs-64-bit difference is reachable, and == 0 where
//     the operand widths say it cannot be, so a wrong bound goes red instead of passing quietly.
//   * for C1/C2, the REFUSED C++ spelling, required to keep diverging.
//
// STILL NOT COVERED HERE: the `fild qword`/`fistp qword` width, which is PC-dependent above 2^53 --
// a 64-bit integer round-trip through the x87 stack loses low bits at PC=53 and keeps them at PC=64.
// The blocks outside mh/fp/ include it, so a future hoist must not lean on this suite's green.
//
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

#include "fp/x87.h"
#include "fp/x87_shapes.h"

namespace {

int g_checks = 0, g_fails = 0;

void ck(bool ok, const char *what) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        printf("  FAIL: %s\n", what);
    }
}

// ---- THE REFERENCE ARM: the original blocks, byte for byte as they stood before step 2 --------------

int32_t ref_trunc_i32(double value) {
    int32_t  r        = 0;
    uint16_t cw_save  = 0;
    uint16_t cw_trunc = 0;
    // clang-format off
    __asm {
        fld     qword ptr [value]
        fstcw   cw_save
        mov     ax, cw_save
        mov     ah, 0x1f            ; RC = 11 truncate, PC = 11 extended
        mov     cw_trunc, ax
        fldcw   cw_trunc
        frndint
        fldcw   cw_save             ; restore BEFORE the store
        fistp   dword ptr [r]
    }
    // clang-format on
    return r;
}

int32_t ref_trunc_i32_mul(int32_t a, double b) {
    int32_t  r        = 0;
    uint16_t cw_save  = 0;
    uint16_t cw_trunc = 0;
    // clang-format off
    __asm {
        fild    dword ptr [a]
        fmul    qword ptr [b]
        fstcw   cw_save
        mov     ax, cw_save
        mov     ah, 0x1f
        mov     cw_trunc, ax
        fldcw   cw_trunc
        frndint
        fldcw   cw_save
        fistp   dword ptr [r]
    }
    // clang-format on
    return r;
}

// The negative arm's stand-in for a compiler that decides to spill the product: the same arithmetic
// with the intermediate deliberately rounded to 53 bits.
int32_t forced53_mul(int32_t a, double b) {
    volatile double t = static_cast<double>(a);
    volatile double p = t * b;
    return static_cast<int32_t>(p);
}

uint16_t read_cw() {
    uint16_t cw = 0;
    // clang-format off
    __asm { fnstcw cw }
    // clang-format on
    return cw;
}

// The inputs the experiment proved width-sensitive: k * (1/k) lands exactly halfway below 1.0, so a
// 53-bit intermediate rounds it up to 1.0 (ties-to-even) while a 64-bit one leaves it just under.
const int SENSITIVE[] = {3, 7, 9};

void sweep(const char *label) {
    char msg[192];

    int diff_trunc = 0, diff_mul = 0, diff_cand = 0;
    for (int i = -60000; i <= 60000; i += 7) {
        const double h = i * 0.5, t = i * 0.3333333333333333, e = static_cast<double>(i);
        if (mh::fp::trunc_i32(h) != ref_trunc_i32(h)) ++diff_trunc;
        if (mh::fp::trunc_i32(t) != ref_trunc_i32(t)) ++diff_trunc;
        if (mh::fp::trunc_i32(e) != ref_trunc_i32(e)) ++diff_trunc;
    }
    // trunc_i32_mul is still ASSEMBLY, so this arm is assembly against assembly -- cheap, and it is
    // what catches an edit to either copy. The interesting comparison is `diff_cand` below.
    for (int i = -20000; i <= 20000; i += 3) {
        if (mh::fp::trunc_i32_mul(i, 0.1) != ref_trunc_i32_mul(i, 0.1)) ++diff_mul;
        if (mh::fp::trunc_i32_mul(i, 1.0 / 3.0) != ref_trunc_i32_mul(i, 1.0 / 3.0)) ++diff_mul;
        if (mh::fp::trunc_i32_mul(i, 0.6666666666666666) != ref_trunc_i32_mul(i, 0.6666666666666666))
            ++diff_mul;
    }
    for (int k = 2; k < 4000; ++k) {
        if (mh::fp::trunc_i32_mul(k, 1.0 / k) != ref_trunc_i32_mul(k, 1.0 / k)) ++diff_mul;
        if (forced53_mul(k, 1.0 / k) != ref_trunc_i32_mul(k, 1.0 / k)) ++diff_cand;
    }

    std::snprintf(msg, sizeof(msg), "%s: S1 trunc_i32 matches the assembly over 51k inputs (%d diff)",
                  label, diff_trunc);
    ck(diff_trunc == 0, msg);
    std::snprintf(msg, sizeof(msg),
                  "%s: M trunc_i32_mul (still assembly) matches its reference copy (%d diff)", label,
                  diff_mul);
    ck(diff_mul == 0, msg);

    // M: THE REFUSAL, pinned per precision setting. A 53-bit intermediate is exactly what the C++
    // spelling produced when compiled as part of x87.h, and the count is the point: harmless at
    // PC=53, wrong on hundreds of ordinary inputs at PC=64.
    const bool pc64 = ((read_cw() >> 8) & 3) == 3;
    std::snprintf(msg, sizeof(msg),
                  "%s: M -- a 53-bit intermediate diverges on %d of 3998 k*(1/k) inputs%s", label,
                  diff_cand, pc64 ? " (must be > 0 here)" : " (must be 0 here)");
    ck(pc64 ? diff_cand > 0 : diff_cand == 0, msg);

    // The edge, by value rather than by "no difference".
    const double EDGE[]    = {2147483647.0, 2147483648.0, -2147483648.0, -2147483649.0, 1e30, -1e30,
                              HUGE_VAL, -HUGE_VAL, std::nan(""), 0.0, -0.0};
    int          diff_edge = 0;
    for (double v : EDGE)
        if (mh::fp::trunc_i32(v) != ref_trunc_i32(v)) ++diff_edge;
    std::snprintf(msg, sizeof(msg),
                  "%s: the out-of-range / non-finite edge agrees on all 11 values (%d diff)", label,
                  diff_edge);
    ck(diff_edge == 0, msg);
    ck(ref_trunc_i32(2147483648.0) == INT32_MIN,
       "the assembly really does store the integer indefinite out of range (premise, not inferred)");
}

// ==== CASE C: THE FIVE `fcompp`/`fnstsw` COMPARE HELPERS ==========================================
//
// CRT-X87-CPP step 2. These are FIVE SEPARATE PROOFS, not one, because they do not test the same
// flag -- measured off the disassembly, they are `setnc`/CF, JBE/CF|ZF, JA/!(CF|ZF), JNC/CF and two
// raw status-word reads, and the correct C++ spelling differs per flag. The pre-proven
// `!(x >= y)` shape covers only the RAW-C0 ones; a strict-and-ordered JA/JBE arm is plain `>`.
//
// Each case below holds the helper's assembly VERBATIM as it stood before the replacement (the
// x87_shapes.h bodies of 2026-09-08), and the subject is the header's current body -- so the
// comparison is through the REAL header, for the reason the file header states: the same
// comparison run offline agreed and run through the header did not.
//
// EVERY ONE OF THESE HELPERS ALSO CONTAINS ARITHMETIC (a divide, and C1 a divide + subtract), so
// these are NOT purely a flag question: the original computes the quotient in the x87 register at
// the live precision control, and a C++ `double` expression need not. That is why each case reports
// its divergence count PER PRECISION SETTING, and why the `w53` arm below exists.

int32_t ref_soldier_strip(int32_t soldier_count, double energy_max, double cur_energy) {
    uint8_t result = 0;
    // clang-format off
    __asm {
        fild    dword ptr [soldier_count]  ; ST(0) = (double)soldier_count         (0x0047e587)
        fdivr   qword ptr [energy_max]     ; ST(0) = energy_max / soldier_count    (0x0047e59c)
        fsubr   qword ptr [energy_max]     ; ST(0) = energy_max - ST(0)            (0x0047e5b1)
        fcomp   qword ptr [cur_energy]     ; compare ST(0) (threshold) vs cur_energy, pop (0x0047e5bc)
        fnstsw  ax
        sahf
        setnc   result                     ; CF=0 (threshold >= cur_energy) -> continue
    }
    // clang-format on
    return result != 0;
}

int32_t ref_capacity_short(int32_t capacity, double holdings, const float *threshold) {
    int32_t  cap = capacity;
    uint16_t sw  = 0;
    // clang-format off
    __asm {
        mov     eax, threshold
        fild    dword ptr [cap]         ; ST(0) = (double)(int)capacity   (0x004e38ca)
        fdiv    qword ptr [holdings]    ; ST(0) = capacity / holdings     (0x004e38d1)
        fld     dword ptr [eax]         ; ST(0) = fSiloRatio, ST(1) = ratio (0x004e38d4)
        fcompp                          ; compare ST(0) with ST(1), pop both (0x004e38da)
        fnstsw  sw                      ;                                 (0x004e38dc)
    }
    // clang-format on
    return ((sw & 0x4100u) == 0) ? 1 : 0;
}

int32_t ref_housing_short(int32_t housing_prev, int32_t pop_total, const float *extra_space) {
    uint16_t sw = 0;
    // clang-format off
    __asm {
        fild    dword ptr [housing_prev]  ; ST(0) = housing_prev                     (0x004e408c)
        fild    dword ptr [pop_total]     ; ST(0) = pop_total, ST(1) = housing_prev  (0x004e4092)
        fdivp   st(1), st                 ; ST(1) = housing_prev / pop_total, pop    (0x004e4098)
        mov     eax, extra_space
        fld     dword ptr [eax]           ; ST(0) = extra_space, ST(1) = ratio       (0x004e409a)
        fcompp                            ; compare extra_space to ratio, pop both   (0x004e40a0)
        fnstsw  sw                        ;                                          (0x004e40a2)
    }
    // clang-format on
    return (sw & 0x4100u) == 0;
}

double ref_labor_utilization(int32_t avail, int32_t total, const float *max_fuck_ratio) {
    double util;
    // clang-format off
    __asm {
        fild    dword ptr [avail]        ; ST(0) = avail                            (0x004e3d59)
        fild    dword ptr [total]        ; ST(0) = total, ST(1) = avail             (0x004e3d65)
        fdivp   st(1), st                ; ST(1) = avail / total, pop               (0x004e3d68)
        fstp    qword ptr [util]         ;                                          (0x004e3d80)

        mov     eax, max_fuck_ratio
        fld     dword ptr [eax]          ; ST(0) = max_fuck_ratio                   (0x004e3d86)
        fcomp   qword ptr [util]         ; compare max_fuck_ratio to util, pop      (0x004e3d8c)
        fnstsw  ax                       ;                                          (0x004e3d92)
        sahf
        jbe     clamp1_done              ; skip the flush unless max_fuck_ratio > util (0x004e3d95)
        mov     dword ptr [util], 0
        mov     dword ptr [util+4], 0
    clamp1_done:

        fld1                             ; ST(0) = 1.0                              (0x004e3dc1)
        fcomp   qword ptr [util]         ; compare 1.0 to (possibly just-flushed) util, pop (0x004e3dc3)
        fnstsw  ax                       ;                                          (0x004e3dc9)
        sahf
        jnc     clamp2_done              ; skip the flush unless 1.0 < util (NaN also falls through) (0x004e3dcc)
        mov     dword ptr [util], 0
        mov     dword ptr [util+4], 0x3ff00000
    clamp2_done:
    }
    // clang-format on
    return util;
}

int32_t ref_yield_below_spend_rate(uint32_t spend_total, const float *ai_clock, uint32_t yield) {
    int64_t  n  = (int64_t)(uint64_t)spend_total;
    int64_t  y  = (int64_t)(uint64_t)yield;
    uint16_t sw = 0;
    // clang-format off
    __asm {
        mov     eax, ai_clock
        fild    qword ptr [n]       ; ST(0) = (double)spend_total       (0x004e53e8)
        fdiv    dword ptr [eax]     ; ST(0) = spend_total / ai_clock    (0x004e53eb, a FLOAT divisor)
        fild    qword ptr [y]       ; ST(0) = yield, ST(1) = rate       (0x004e5402)
        fcompp                      ; compare ST(0) with ST(1), pop both (0x004e5405)
        fnstsw  sw                  ;                                   (0x004e5407)
    }
    // clang-format on
    return (sw & 0x0100u) ? 1 : 0;
}

// ---- THE WRONG SPELLINGS. Each is the plausible C++ a translator reaches for, and each differs
// from the original ONLY on an unordered compare. They exist so the sweep can prove it actually
// reaches the NaN placements it claims to cover: if one of these stops diverging, the NaN dimension
// has gone blind and the equalities above mean nothing.
int32_t wrong_soldier_strip(int32_t sc, double emax, double ecur) {
    const double threshold = emax - emax / sc;
    return !(threshold < ecur); // "CF=0" read as the negation of `<` -- true on unordered
}
int32_t wrong_capacity_short(int32_t cap, double hold, const float *thr) {
    return !(static_cast<double>(*thr) <= static_cast<double>(cap) / hold) ? 1 : 0;
}
int32_t wrong_housing_short(int32_t hp, int32_t pt, const float *es) {
    return !(static_cast<double>(*es) <= static_cast<double>(hp) / static_cast<double>(pt));
}
double wrong_labor_utilization(int32_t avail, int32_t total, const float *mr) {
    double util = static_cast<double>(avail) / static_cast<double>(total);
    if (static_cast<double>(*mr) > util) util = 0.0;
    if (util > 1.0) util = 1.0; // the JNC arm read as `>`: misses the unordered flush
    return util;
}
int32_t wrong_yield_below_spend_rate(uint32_t spend, const float *clk, uint32_t yield) {
    const double rate = static_cast<double>(spend) / static_cast<double>(*clk);
    return (static_cast<double>(yield) < rate) ? 1 : 0; // raw C0 read as `<` -- false on unordered
}

// ---- THE REFUSED C++ SPELLINGS (C1, C2). These are FAITHFUL on the flag question -- the negation
// of C0 is `>=`, and JBE's CF|ZF is a strict ordered `>` -- and they still lose, on the ARITHMETIC.
// They live here so the refusal stays measured: fptest requires each to keep diverging from the
// assembly, at BOTH precision settings, exactly as case M does for trunc_i32_mul.
int32_t cpp_soldier_strip(int32_t sc, double emax, double ecur) {
    const double threshold = emax - emax / sc;
    return threshold >= ecur;
}
int32_t cpp_capacity_short(int32_t cap, double hold, const float *thr) {
    const double ratio = static_cast<double>(cap) / hold;
    const double t     = static_cast<double>(*thr);
    return (t > ratio) ? 1 : 0;
}

// ---- THE WIDTH PROBES. The same predicate with the quotient forced through a `volatile double`,
// i.e. rounded to 53 bits before the compare. At PC=64 the original keeps the quotient at 64 bits,
// so if one of these never diverges the sweep cannot see a width difference at all and a zero in
// the candidate column at PC=64 would be a blind zero rather than a proof.
int32_t w53_soldier_strip(int32_t sc, double emax, double ecur) {
    volatile double q = emax / sc;
    volatile double t = emax - q;
    return t >= ecur;
}
int32_t w53_capacity_short(int32_t cap, double hold, const float *thr) {
    volatile double q = static_cast<double>(cap) / hold;
    return (static_cast<double>(*thr) > q) ? 1 : 0;
}
int32_t w53_housing_short(int32_t hp, int32_t pt, const float *es) {
    volatile double q = static_cast<double>(hp) / static_cast<double>(pt);
    return static_cast<double>(*es) > q;
}
double w53_labor_utilization(int32_t avail, int32_t total, const float *mr) {
    volatile double q    = static_cast<double>(avail) / static_cast<double>(total);
    double          util = q;
    if (static_cast<double>(*mr) > util) util = 0.0;
    if (!(1.0 >= util)) util = 1.0;
    return util;
}
int32_t w53_yield_below_spend_rate(uint32_t spend, const float *clk, uint32_t yield) {
    volatile double q = static_cast<double>(spend) / static_cast<double>(*clk);
    return !(static_cast<double>(yield) >= q) ? 1 : 0;
}

bool same_bits(double a, double b) {
    uint64_t x, y;
    std::memcpy(&x, &a, 8);
    std::memcpy(&y, &b, 8);
    return x == y;
}

// The probe generators. `near` is the value the boundary sits at, so the neighbours straddle it --
// that is where an 80-vs-53-bit intermediate can change the verdict, and where `>` and `>=` differ.
void probes_f(float near_v, float *out /* [9] */) {
    out[0] = near_v;
    out[1] = std::nextafterf(near_v, -HUGE_VALF);
    out[2] = std::nextafterf(near_v, HUGE_VALF);
    out[3] = 0.0f;
    out[4] = 1.0f;
    out[5] = -1.0f;
    out[6] = HUGE_VALF;
    out[7] = -HUGE_VALF;
    out[8] = std::nanf("");
}
void probes_d(double near_v, double *out /* [9] */) {
    out[0] = near_v;
    out[1] = std::nextafter(near_v, -HUGE_VAL);
    out[2] = std::nextafter(near_v, HUGE_VAL);
    out[3] = 0.0;
    out[4] = -0.0;
    out[5] = 1.0;
    out[6] = HUGE_VAL;
    out[7] = -HUGE_VAL;
    out[8] = std::nan("");
}

const int32_t  C_INTS[] = {INT32_MIN, -7, -1, 0, 1, 2, 3, 97, 1000, 1 << 20, INT32_MAX};
const uint32_t C_U32S[] = {0u, 1u, 2u, 3u, 97u, 1000u, 1u << 20, 0x7fffffffu, 0x80000000u, 0xffffffffu};
// The out-of-range end is NOT decoration. The x87 register has a 15-bit exponent and NO control-word
// field that narrows it -- PC only sets the significand -- so an intermediate that leaves `double`
// range stays finite in the register and a C++ `double` expression cannot reproduce that at ANY
// precision setting. DBL_MAX / DBL_MIN / the smallest denormal are what make that visible.
const double C_DBLS[] = {0.0,
                         -0.0,
                         1.0,
                         -1.0,
                         3.0,
                         7.0,
                         0.1,
                         1e-300,
                         1e300,
                         DBL_MAX,
                         -DBL_MAX,
                         DBL_MIN,
                         std::numeric_limits<double>::denorm_min(),
                         HUGE_VAL,
                         -HUGE_VAL,
                         std::numeric_limits<double>::quiet_NaN()};

struct arm_counts {
    int  cand = 0, wrong = 0, w53 = 0, refused = 0, n = 0;
    char first[3][128] = {{0}, {0}, {0}};
    int  nfirst        = 0;
    char firstref[128] = {0};
    void note(const char *fmt, double a, double b, double c) {
        if (nfirst < 3) std::snprintf(first[nfirst++], 128, fmt, a, b, c);
    }
    void note_refused(const char *fmt, double a, double b, double c) {
        if (!firstref[0]) std::snprintf(firstref, 128, fmt, a, b, c);
    }
};

// `width_sensitive` is a CLAIM about the case, checked in BOTH directions. Some of these helpers
// cannot express a 53-vs-64-bit difference at all -- see each case -- and for those a zero width
// probe is a fact about the operand types, not a blind sweep; if one ever starts diverging the
// recorded reasoning is wrong and this goes red.
// `refusal_pin` marks a case whose helper is STILL ASSEMBLY: there the subject is an asm-vs-asm
// drift check and the interesting column is `refused` -- the C++ spelling that was measured NOT to
// work, required to keep diverging so the refusal is not re-litigated from a narrower sweep. Same
// shape as case M.
void report(const char *label, const char *case_id, const char *what, const arm_counts &c,
            bool pc64, bool width_sensitive, bool refusal_pin = false) {
    char msg[256];
    std::snprintf(msg, sizeof(msg), "%s: %s %s -- C++ vs the assembly, %d of %d inputs differ",
                  label, case_id, what, c.cand, c.n);
    ck(c.cand == 0, msg);
    for (int i = 0; i < c.nfirst; ++i) printf("      diverged at %s\n", c.first[i]);
    std::snprintf(msg, sizeof(msg),
                  "%s: %s NEGATIVE ARM -- the plausible-but-wrong spelling diverges on %d inputs "
                  "(0 would mean the NaN placements are not being reached)",
                  label, case_id, c.wrong);
    ck(c.wrong > 0, msg);
    if (pc64 && width_sensitive) {
        std::snprintf(msg, sizeof(msg),
                      "%s: %s WIDTH ARM -- a 53-bit intermediate diverges on %d inputs, so the "
                      "sweep can see a width difference where it claims not to find one",
                      label, case_id, c.w53);
        ck(c.w53 > 0, msg);
    } else if (pc64) {
        std::snprintf(msg, sizeof(msg),
                      "%s: %s WIDTH ARM -- this helper's operands CANNOT express a 53-vs-64-bit "
                      "difference (see the case), so the probe must stay at 0; it is %d",
                      label, case_id, c.w53);
        ck(c.w53 == 0, msg);
    }
    printf("  [%s] %s width probe (53-bit intermediate vs the original): %d of %d differ\n", label,
           case_id, c.w53, c.n);
}

void sweep_compares(const char *label, bool pc64) {
    double dprobe[9];
    float  fprobe[9];

    // ---- C1. sim_unit_apply_damage: FCOMP + SAHF + SETNC, i.e. CF=0 -> `threshold >= cur_energy`.
    {
        arm_counts c;
        for (int32_t sc : C_INTS)
            for (double em : C_DBLS) {
                probes_d(em - em / sc, dprobe);
                for (double ce : dprobe) {
                    ++c.n;
                    const int32_t r = ref_soldier_strip(sc, em, ce);
                    if (mh::fp::soldier_strip_should_continue(sc, em, ce) != (r != 0)) {
                        ++c.cand;
                        c.note("sc=%.0f energy_max=%g cur_energy=%g", (double)sc, em, ce);
                    }
                    if (wrong_soldier_strip(sc, em, ce) != r) ++c.wrong;
                    if (cpp_soldier_strip(sc, em, ce) != r) {
                        ++c.refused;
                        c.note_refused("sc=%.0f energy_max=%g cur_energy=%g", (double)sc, em, ce);
                    }
                    if (w53_soldier_strip(sc, em, ce) != r) ++c.w53;
                }
            }
        // WIDTH: sensitive. `energy_max - energy_max/soldier_count` is double arithmetic, so a
        // 64-bit intermediate is a real, reachable difference from a 53-bit one.
        report(label, "C1 soldier_strip_should_continue", "(setnc / CF)", c, pc64, true, true);
    }

    // ---- C2. ai_shortage_gate: FCOMPP + (sw & C3|C0)==0, the JBE arm -> strict ordered `>`.
    {
        arm_counts c;
        for (int32_t cap : C_INTS)
            for (double hold : C_DBLS) {
                probes_f(static_cast<float>(static_cast<double>(cap) / hold), fprobe);
                for (float t : fprobe) {
                    ++c.n;
                    const int32_t r = ref_capacity_short(cap, hold, &t);
                    if (mh::fp::x87_capacity_short(cap, hold, &t) != r) {
                        ++c.cand;
                        c.note("capacity=%.0f holdings=%g threshold=%g", (double)cap, hold, (double)t);
                    }
                    if (wrong_capacity_short(cap, hold, &t) != r) ++c.wrong;
                    if (cpp_capacity_short(cap, hold, &t) != r) {
                        ++c.refused;
                        c.note_refused("capacity=%.0f holdings=%g threshold=%g", (double)cap, hold,
                                       (double)t);
                    }
                    if (w53_capacity_short(cap, hold, &t) != r) ++c.w53;
                }
            }
        // WIDTH: sensitive -- `capacity / holdings` has a DOUBLE divisor, so both the significand
        // and the exponent can leave what a C++ double expression keeps.
        report(label, "C2 x87_capacity_short", "(jbe / CF|ZF)", c, pc64, true, true);
    }

    // ---- C3. ai_worker_rebalance: FCOMPP + the JA arm -> strict ordered `>`, different call shape.
    {
        arm_counts c;
        for (int32_t hp : C_INTS)
            for (int32_t pt : C_INTS) {
                probes_f(static_cast<float>(static_cast<double>(hp) / static_cast<double>(pt)), fprobe);
                for (float es : fprobe) {
                    ++c.n;
                    const int32_t r = ref_housing_short(hp, pt, &es);
                    if (static_cast<int32_t>(mh::fp::x87_housing_short(hp, pt, &es)) != r) {
                        ++c.cand;
                        c.note("housing_prev=%.0f pop_total=%.0f extra_space=%g", (double)hp,
                               (double)pt, (double)es);
                    }
                    if (wrong_housing_short(hp, pt, &es) != r) ++c.wrong;
                    if (w53_housing_short(hp, pt, &es) != r) ++c.w53;
                }
            }
        // WIDTH: INSENSITIVE, and this is an operand-width fact rather than an empty sweep. Both
        // operands are int32 so the quotient never leaves double range, and the threshold is a FLOAT
        // (24-bit significand). For a 53-vs-64-bit intermediate to change a comparison the threshold
        // would have to sit strictly between the two roundings, i.e. within ~2^-64 relative of the
        // exact quotient; a nonzero |v - hp/pt| with v a float and hp,pt int32 is at least ~2^-61.
        // The band is unreachable, so the probe must read 0 -- and if it ever does not, that bound
        // is wrong and this arm says so.
        report(label, "C3 x87_housing_short", "(ja / !(CF|ZF))", c, pc64, false);
    }

    // ---- C4. ai_worker_rebalance: TWO compares in one body -- JBE (strict ordered) then JNC (raw
    // C0, so a NaN IS flushed to 1.0). Compared by BITS, because the subject returns a double and a
    // -0.0 or a NaN that the clamp failed to flush must not pass as equal.
    {
        arm_counts c;
        for (int32_t av : C_INTS)
            for (int32_t tot : C_INTS) {
                probes_f(static_cast<float>(static_cast<double>(av) / static_cast<double>(tot)), fprobe);
                for (float mr : fprobe) {
                    ++c.n;
                    const double r = ref_labor_utilization(av, tot, &mr);
                    if (!same_bits(mh::fp::x87_labor_utilization(av, tot, &mr), r)) {
                        ++c.cand;
                        c.note("avail=%.0f total=%.0f max_ratio=%g", (double)av, (double)tot,
                               (double)mr);
                    }
                    if (!same_bits(wrong_labor_utilization(av, tot, &mr), r)) ++c.wrong;
                    if (!same_bits(w53_labor_utilization(av, tot, &mr), r)) ++c.w53;
                }
            }
        // WIDTH: INSENSITIVE BY CONSTRUCTION -- the ORIGINAL stores the quotient to a double
        // (`fstp qword [util]`) before either compare, so it double-rounds exactly as the C++
        // expression does. There is no 80-bit value left to disagree about, which is why the probe
        // -- itself a forced round to double -- is the same computation as both arms.
        report(label, "C4 x87_labor_utilization", "(jbe + jnc, two compares)", c, pc64, false);
    }

    // ---- C5. ai_mine_plan: FCOMPP + raw C0 -> `!(yield >= rate)`, the pre-proven spelling.
    {
        arm_counts c;
        for (uint32_t sp : C_U32S)
            for (uint32_t y : C_U32S) {
                probes_f(static_cast<float>(static_cast<double>(sp) / static_cast<double>(y)), fprobe);
                for (float clk : fprobe) {
                    ++c.n;
                    const int32_t r = ref_yield_below_spend_rate(sp, &clk, y);
                    if (mh::fp::x87_yield_below_spend_rate(sp, &clk, y) != r) {
                        ++c.cand;
                        c.note("spend_total=%.0f ai_clock=%g yield=%.0f", (double)sp, (double)clk,
                               (double)y);
                    }
                    if (wrong_yield_below_spend_rate(sp, &clk, y) != r) ++c.wrong;
                    if (w53_yield_below_spend_rate(sp, &clk, y) != r) ++c.w53;
                }
            }
        // WIDTH: INSENSITIVE, same operand-width argument as C3. `yield` is a uint32, so the
        // compared value is an EXACT integer below 2^32; the rate is uint32/float, a product of a
        // 32-bit and a 24-bit significand, so a nonzero distance between the rate and an integer is
        // at least ~2^-56 relative -- never inside the ~2^-64 band where 53 and 64 bits disagree.
        report(label, "C5 x87_yield_below_spend_rate", "(raw C0)", c, pc64, false);
    }
}

// ==== CASE W: THE WIDTH/RANGE CLASSIFICATION OF THE UNTESTED x87_shapes.h HELPERS =================
//
// CRT-X87-CPP step 3 PREPARATION, and deliberately NOT a replacement proof. Sixteen helpers have
// never had a verdict taken. Before any is rewritten, ONE question decides which pile it lands in,
// and it is answerable WITHOUT writing a candidate: does the 80-bit intermediate the original keeps
// in the register actually change the answer?
//
// THE INSTRUMENT. For each helper an arm that performs the same arithmetic with every intermediate
// forced through a `volatile double` -- which is what a C++ `double` expression does, since C++
// rounds to the declared type at each step where the original keeps 64 significand bits. THE SINK IS
// `mh::fp::trunc_i32`, the one helper already proven bit-equal to `fld`+the dance+`fistp dword` over
// 51k inputs and the 11-value non-finite edge (the sweep above re-derives it every run) -- so the
// only thing this arm varies is the WIDTH OF THE CHAIN. Reference is the header helper itself, still
// assembly for all sixteen.
//
// HOW TO READ A ROW -- this is the classification the step-3 worklist is built from:
//   diverges at PC=53      -> REFUSE. What differs is the register's 15-bit EXPONENT, not its
//                             significand, and no control-word field narrows an exponent. No PC
//                             guarantee and no C++ spelling recovers it. (Exactly what refused
//                             soldier_strip_should_continue and x87_capacity_short.)
//   diverges at PC=64 only -> C++ CANDIDATE **IF PC=53 IS GUARANTEED**. At 53 the x87 already
//                             computes what C++ computes; at 64 it does not.
//   never diverges         -> C++ candidate, this axis CLEAR.
//
// WHAT A CLEAN ROW DOES NOT SAY. It clears the width/range axis ONLY. The spelling still needs its
// own case per helper, and these survive a clean row: the `fistp qword` + LOW-DWORD narrowing (a C++
// cast of an out-of-range double yields the 32-bit integer indefinite, NOT the low half of the
// 64-bit one), and `std::floor`'s edge on the two floor helpers.
//
// THE ROUND-TRIP MUST STAY AFTER EVERY OP: forcing 53 bits only at the end of a chain under-detects,
// because the original's second operation consumes a 64-bit first result.

// `expect` is DECLARED AT THE CALL SITE, not derived from the measurement -- otherwise the class
// column would be asserting the numbers against themselves. 0 = width/range clear, 1 = clean at
// PC=53 only, 2 = REFUSE (diverges even at PC=53). A row whose measurement stops matching its
// declared class goes red, in EITHER direction: a refusal that silently starts passing is as much a
// finding as a conversion that starts failing.
struct wrow {
    const char *name;
    int         d53, d64, n, expect;
    char        first53[96];
};
wrow g_w[20];
int  g_wn = 0;

// The first PC=53 divergence, BY VALUE. A count alone does not distinguish "the chain left double
// range and came back" from a bug in the probe, and a REFUSE verdict deserves a named input the way
// C1/C2's does.
char g_wfirst[96];
void wfirst(const char *fmt, double a, double b, double c) {
    if (!g_wfirst[0]) std::snprintf(g_wfirst, sizeof(g_wfirst), fmt, a, b, c);
}

void wnote(const char *name, int diff, int n, bool pc64, int expect) {
    for (int i = 0; i < g_wn; ++i)
        if (g_w[i].name == name) {
            if (pc64) g_w[i].d64 = diff;
            else {
                g_w[i].d53 = diff;
                std::memcpy(g_w[i].first53, g_wfirst, sizeof(g_wfirst));
            }
            g_w[i].n    = n;
            g_wfirst[0] = 0;
            return;
        }
    g_w[g_wn].name   = name;
    g_w[g_wn].d53    = pc64 ? -1 : diff;
    g_w[g_wn].d64    = pc64 ? diff : -1;
    g_w[g_wn].n      = n;
    g_w[g_wn].expect = expect;
    if (!pc64) std::memcpy(g_w[g_wn].first53, g_wfirst, sizeof(g_wfirst));
    else g_w[g_wn].first53[0] = 0;
    g_wfirst[0] = 0;
    ++g_wn;
}

// ---- the forced-53 twins, dword-sink family (the sink is the PROVEN trunc_i32) --------------------

int32_t w_target_pip_count(double emax, double ecur, int32_t pip, double scale) {
    volatile double diff = emax - ecur;
    volatile double p    = static_cast<double>(pip) * scale;
    volatile double prod = diff * p; // FMULP st(1),st(0): ST(1) = diff * p
    volatile double q    = prod / emax;
    return mh::fp::trunc_i32(q);
}
int32_t w_trunc_mul_div(double a, double b, double c) {
    volatile double m = a * b;
    volatile double q = m / c;
    return mh::fp::trunc_i32(q);
}
int32_t w_trunc_mul_mul(double a, double b, double c) {
    volatile double m = a * b;
    volatile double p = m * c;
    return mh::fp::trunc_i32(p);
}
int32_t w_trunc_hp_loss_level(double mx, double cur, double scale) {
    volatile double d = mx - cur;
    volatile double q = d / mx;
    volatile double p = q * scale;
    return mh::fp::trunc_i32(p);
}
int32_t w_trunc_scaled_int(int32_t v, double num, double den) {
    volatile double q = num / den;
    volatile double p = q * static_cast<double>(v); // FMULP st(1),st(0): ST(1) = q * v
    return mh::fp::trunc_i32(p);
}

// ---- the two QWORD-LOW helpers keep an assembly twin ----------------------------------------------
// Their sink is `fistp qword` plus a LOW-DWORD take, which no C++ cast is yet proven to reproduce --
// using a C++ sink here would conflate the sink axis with the width axis this table is about. So the
// twin is the original's own instructions with the round-trip inserted, and nothing else changed.

void sweep_widths(bool pc64) {
    int n, d;
    // The k * (1/k) family rides along on every MULTIPLYING helper: it is the shape a width
    // difference actually takes (exactly halfway below 1.0, so 53 bits rounds up and 64 does not),
    // and it is the input that refused trunc_i32_mul. A product sweep over C_DBLS alone would miss it.

    n = d = 0;
    for (double a : C_DBLS)
        for (double b : C_DBLS)
            for (int32_t p : C_INTS) {
                ++n;
                if (mh::fp::target_pip_count(a, b, p, 0.3) != w_target_pip_count(a, b, p, 0.3)) {
                    ++d;
                    wfirst("emax=%g ecur=%g pip=%.0f scale=0.3", a, b, (double)p);
                }
            }
    wnote("target_pip_count", d, n, pc64, 2);

    n = d = 0;
    for (double a : C_DBLS)
        for (double b : C_DBLS)
            for (double c : C_DBLS) {
                ++n;
                if (mh::fp::trunc_mul_div(a, b, c) != w_trunc_mul_div(a, b, c)) {
                    ++d;
                    wfirst("a=%g b=%g c=%g", a, b, c);
                }
            }
    for (int k = 2; k < 4000; ++k) {
        ++n;
        if (mh::fp::trunc_mul_div((double)k, 1.0 / k, 1.0) != w_trunc_mul_div((double)k, 1.0 / k, 1.0))
            ++d;
    }
    wnote("trunc_mul_div", d, n, pc64, 2);

    n = d = 0;
    for (double a : C_DBLS)
        for (double b : C_DBLS)
            for (double c : C_DBLS) {
                ++n;
                if (mh::fp::trunc_mul_mul(a, b, c) != w_trunc_mul_mul(a, b, c)) {
                    ++d;
                    wfirst("a=%g b=%g c=%g", a, b, c);
                }
            }
    for (int k = 2; k < 4000; ++k) {
        ++n;
        if (mh::fp::trunc_mul_mul((double)k, 1.0 / k, 1.0) != w_trunc_mul_mul((double)k, 1.0 / k, 1.0))
            ++d;
    }
    wnote("trunc_mul_mul", d, n, pc64, 2);

    n = d = 0;
    for (double a : C_DBLS)
        for (double b : C_DBLS)
            for (double c : C_DBLS) {
                ++n;
                if (mh::fp::trunc_hp_loss_level(a, b, c) != w_trunc_hp_loss_level(a, b, c)) {
                    ++d;
                    wfirst("max=%g cur=%g scale=%g", a, b, c);
                }
            }
    wnote("trunc_hp_loss_level", d, n, pc64, 2);

    n = d = 0;
    for (int32_t v : C_INTS)
        for (double a : C_DBLS)
            for (double b : C_DBLS) {
                ++n;
                if (mh::fp::trunc_scaled_int(v, a, b) != w_trunc_scaled_int(v, a, b)) {
                    ++d;
                    wfirst("value=%.0f num=%g den=%g", (double)v, a, b);
                }
            }
    for (int k = 2; k < 4000; ++k) {
        ++n;
        if (mh::fp::trunc_scaled_int(k, 1.0, (double)k) != w_trunc_scaled_int(k, 1.0, (double)k)) ++d;
    }
    wnote("trunc_scaled_int", d, n, pc64, 2);
}

void report_widths() {
    char msg[256];
    printf("\n  ---- W: the five REFUSED multi-op chains, still measured every run ----\n");
    printf("  %-26s %8s %8s %9s   %s\n", "helper", "PC=53", "PC=64", "inputs", "class");
    for (int i = 0; i < g_wn; ++i) {
        const wrow &w   = g_w[i];
        const int   got = w.d53 > 0 ? 2 : (w.d64 > 0 ? 1 : 0);
        const char *cls = got == 2   ? "REFUSE -- exponent range"
                          : got == 1 ? "C++ ONLY IF PC=53 GUARANTEED"
                                     : "C++ candidate -- width/range CLEAR";
        printf("  %-26s %8d %8d %9d   %s\n", w.name, w.d53, w.d64, w.n, cls);
        // A REFUSE verdict gets a NAMED input, not just a count -- a count alone cannot tell "the
        // chain left double range and came back" from a bug in the probe.
        if (w.d53 > 0 && w.first53[0]) printf("  %-26s   first PC=53 divergence: %s\n", "", w.first53);
        // THE CLASS PIN. `expect` is declared at the sweep call site, so this is not the table
        // checking its own arithmetic against itself: it is the DECLARED verdict held to the
        // MEASURED one, in both directions. A refusal that quietly starts passing is as much a
        // finding as a conversion that starts failing -- and five of these rows are the standing
        // evidence for helpers that are still assembly, so the refusal has to stay measured rather
        // than becoming a comment nobody re-runs.
        std::snprintf(msg, sizeof(msg),
                      "W: %s measures class %d, its sweep call site declares %d (%d differ at PC=53, "
                      "%d at PC=64, over %d inputs)",
                      w.name, got, w.expect, w.d53, w.d64, w.n);
        ck(got == w.expect, msg);
    }
    printf("  (only the five helpers that are STILL ASSEMBLY appear here. The other eleven\n"
           "   this table classified are C++ now and are covered by cases T1..T13, whose\n"
           "   reference arm is the verbatim pre-conversion assembly -- the correct one. A\n"
           "   converted helper CANNOT stay in this table: its subject arm would no longer be\n"
           "   the assembly, so the probe would compare two 53-bit computations and read 0.)\n");

    // The table is only evidence if a width difference is VISIBLE to this instrument at all. At
    // PC=64 at least one row must diverge, or the round-trip arm is inert and every CLEAR above is a
    // blind zero rather than a finding.
    int nz64 = 0;
    for (int i = 0; i < g_wn; ++i)
        if (g_w[i].d64 > 0) ++nz64;
    std::snprintf(msg, sizeof(msg),
                  "W: NEGATIVE ARM -- %d of %d measured rows show a 53-vs-64-bit difference, so the "
                  "round-trip arm can see one (0 would mean the whole table is blind)",
                  nz64, g_wn);
    ck(nz64 > 0, msg);
}

// ---- CASE T REFERENCE ARMS: the eleven bodies VERBATIM as they stood before step 3 -----------
// Extracted mechanically from the pre-conversion x87_shapes.h, not retyped -- a hand copy of
// an assembly block is exactly the place a silent transcription error would hide, and this
// suite is worthless if its reference arm is not what actually shipped.

int32_t ref_trunc_div_plus1(double angle_deg, double sector_deg_in) {
    const double sector_deg = sector_deg_in; // addressable copy for the asm block below
    int32_t      r          = 0;
    uint16_t     cw_save = 0, cw_trunc = 0;
    // clang-format off  (one instruction per line; clang-format would fold the __asm block)
    __asm {
        fld     qword ptr [angle_deg]      ; ST(0) = angle_deg                         (0x00494791)
        fdiv    qword ptr [sector_deg]     ; ST(0) /= 15.0                             (0x00494794)
        fld1                               ; ST(0) = 1.0, push                         (0x0049479a)
        faddp   st(1), st(0)               ; ST(1) += ST(0); pop -> ST0 = quot + 1.0    (0x0049479c)
        ; --- utils_math_trunc @0x004d0596, inlined: round toward zero -------------------------
        fstcw   cw_save                    ;                                           (0x004d0597)
        mov     ax, cw_save
        mov     ah, 0x1f                   ; RC = 11 truncate, PC = 11 extended        (0x004d059f)
        mov     cw_trunc, ax
        fldcw   cw_trunc                   ;                                           (0x004d05a4)
        frndint                            ;                                           (0x004d05a7)
        fldcw   cw_save                    ; restore BEFORE the store                  (0x004d05a9)
        ; -----------------------------------------------------------------------------------------
        fistp   dword ptr [r]              ;                                           (0x004947a3)
    }
    // clang-format on
    return r;
}

int32_t ref_trunc_add(double a, double b) {
    int32_t  r       = 0;
    uint16_t cw_save = 0, cw_trunc = 0;
    // clang-format off  (one instruction per line; clang-format would fold the __asm block)
    __asm {
        fld     qword ptr [a]
        fadd    qword ptr [b]
        fstcw   cw_save
        mov     ax, cw_save
        mov     ah, 0x1f            ; RC = 11 truncate, PC = 11 extended
        mov     cw_trunc, ax
        fldcw   cw_trunc
        frndint
        fldcw   cw_save             ; restore BEFORE the store
        fistp   dword ptr [r]
    }
    // clang-format on
    return r;
}

int32_t ref_trunc_sub(double a, double b) {
    int32_t  r       = 0;
    uint16_t cw_save = 0, cw_trunc = 0;
    // clang-format off
    __asm {
        fld     qword ptr [a]
        fsub    qword ptr [b]
        fstcw   cw_save
        mov     ax, cw_save
        mov     ah, 0x1f
        mov     cw_trunc, ax
        fldcw   cw_trunc
        frndint
        fldcw   cw_save
        fistp   dword ptr [r]
    }
    // clang-format on
    return r;
}

int32_t ref_trunc_mul(double a, double b) {
    int32_t  r       = 0;
    uint16_t cw_save = 0, cw_trunc = 0;
    // clang-format off  (one instruction per line; clang-format would fold the __asm block)
    __asm {
        fld     qword ptr [a]
        fmul    qword ptr [b]
        fstcw   cw_save
        mov     ax, cw_save
        mov     ah, 0x1f
        mov     cw_trunc, ax
        fldcw   cw_trunc
        frndint
        fldcw   cw_save
        fistp   dword ptr [r]
    }
    // clang-format on
    return r;
}

int32_t ref_trunc_div(double angle_deg, double sector_deg) {
    int32_t  r        = 0;
    uint16_t cw_save  = 0;
    uint16_t cw_trunc = 0;
    // clang-format off  (one instruction per line; clang-format would fold the __asm block)
    __asm {
        fld     qword ptr [angle_deg]      ; ST(0) = angle_deg
        fdiv    qword ptr [sector_deg]     ; ST(0) /= sector_deg
        ; --- utils_math_trunc @0x004d0596, inlined: round toward zero -------------------------
        fstcw   cw_save
        mov     ax, cw_save
        mov     ah, 0x1f                   ; RC = 11 truncate, PC = 11 extended
        mov     cw_trunc, ax
        fldcw   cw_trunc
        frndint
        fldcw   cw_save                    ; restore BEFORE the store
        ; -----------------------------------------------------------------------------------------
        fistp   dword ptr [r]
    }
    // clang-format on
    return r;
}

int32_t ref_trunc_float_to_int32(float value) {
    int64_t  r       = 0;
    uint16_t cw_save = 0, cw_trunc = 0;
    // clang-format off  (one instruction per line; clang-format would fold the __asm block)
    __asm {
        fld     value               ; ST(0) = value                               (0x004d72fb)
        fstcw   cw_save             ;                                             (0x004d0597)
        mov     ax, cw_save
        mov     ah, 0x1f            ; RC = 11 truncate, PC = 11 extended          (0x004d059f)
        mov     cw_trunc, ax
        fldcw   cw_trunc            ;                                             (0x004d05a4)
        frndint                     ;                                            (0x004d05a7)
        fldcw   cw_save             ; restore BEFORE the store                   (0x004d05a9)
        fistp   qword ptr [r]       ;                                             (0x004d7304)
    }
    // clang-format on
    return (int32_t)(uint32_t)(uint64_t)r; // MOV EAX,dword ptr[ESP] -- LOW dword only (0x004d7307)
}

int32_t ref_trunc_add_u32_double_low(uint32_t running_total, const double *addend) {
    int64_t  n       = (int64_t)(uint64_t)running_total; // MOV dword[ESP],EBX / dword[ESP+4],0 (0x004d6d0b-0x004d6d0e)
    int64_t  r       = 0;
    uint16_t cw_save = 0, cw_trunc = 0;
    // clang-format off  (one instruction per line; clang-format would fold the __asm block)
    __asm {
        mov     eax, addend
        fild    qword ptr [n]       ; ST(0) = (double)(uint64_t)running_total   (0x004d6d16)
        fadd    qword ptr [eax]     ; ST(0) += *addend, a DOUBLE                (0x004d6d19)
        ; --- utils_math_trunc @0x004d0596, inlined: round toward zero ---------------------------
        fstcw   cw_save             ;                                          (0x004d0597)
        mov     ax, cw_save
        mov     ah, 0x1f            ; RC = 11 truncate, PC = 11 extended       (0x004d059f)
        mov     cw_trunc, ax
        fldcw   cw_trunc            ;                                          (0x004d05a4)
        frndint                     ;                                         (0x004d05a7)
        fldcw   cw_save             ; restore BEFORE the store                (0x004d05a9)
        ; -----------------------------------------------------------------------------------------
        fistp   qword ptr [r]       ;                                          (0x004d6d25)
    }
    // clang-format on
    return (int32_t)(uint32_t)(uint64_t)r; // MOV EBX,dword ptr[ESP] -- LOW dword only (0x004d6d28)
}

int32_t ref_x87_sqrt_and_trunc(uint32_t radius_sq) {
    // The high dword is an EXPLICIT zero store (`MOV [EBP-0x1c],0` @0x004e8cb2), so the FILD is
    // zero-extended -- a "negative" radius_sq would be read as a large positive, but
    // bldg_max_defense_radius_sq's return is a genuine squared distance, so that never occurs here.
    int64_t  n       = (int64_t)(uint64_t)radius_sq; // [EBP-0x20]/[EBP-0x1c] @0x004e8caf-0x004e8cb2
    int64_t  r       = 0;
    uint16_t cw_save = 0, cw_trunc = 0;
    // clang-format off  (one instruction per line; clang-format would fold the __asm block)
    __asm {
        fild    qword ptr [n]       ; ST(0) = (double)(uint32)radius_sq       (0x004e8cb9)
        ; --- CRT_004da9f0, inlined: the non-negative arm only. The full routine is FTST/FSTSW/SAHF/
        ; JNC -> FSQRT, with the carry (negative-operand) arm popping ST0, calling the math error
        ; handler at 0x004e91d0 and returning AL=1 -- an ERROR FLAG, not a value, which is why there
        ; is no gc. member for it (see ai_state.h's comment on this call site). That arm is provably
        ; unreachable for THIS caller (a squared distance, zero-extended into a 64-bit int above), so
        ; it is a comment here, not a branch. -----------------------------------------------------
        fsqrt                       ; ST(0) = sqrt(ST(0))                     (CRT_004da9f0)
        ; --- utils_math_trunc @0x004d0596, inlined: round toward zero -------------------------
        fstcw   cw_save             ;                                        (0x004d0597)
        mov     ax, cw_save
        mov     ah, 0x1f            ; RC = 11 truncate, PC = 11 extended     (0x004d059f)
        mov     cw_trunc, ax
        fldcw   cw_trunc            ;                                        (0x004d05a4)
        frndint                     ;                                        (0x004d05a7)
        fldcw   cw_save             ; restore BEFORE the store               (0x004d05a9)
        ; -------------------------------------------------------------------------------------
        fistp   qword ptr [r]       ;                                        (0x004e8cc6)
    }
    // clang-format on
    return (int32_t)(uint32_t)(uint64_t)r; // MOV EAX,[EBP-0x20] -- the LOW dword only (0x004e8cc9)
}

int32_t ref_x87_floor_diff_times_ratio(int32_t diff, const float *ratio) {
    double product;
    // clang-format off  (one instruction per line; clang-format would fold the __asm block)
    __asm {
        fild    dword ptr [diff]     ; ST(0) = (double)diff                     (0x004e3beb)
        mov     eax, ratio
        fmul    dword ptr [eax]      ; ST(0) = diff * ratio, still 80-bit        (0x004e3bee)
        fstp    qword ptr [product]  ; round ONCE, to double, for the CRT call   (0x004e3bf7)
    }
    // clang-format on
    return (int32_t)std::floor(product); // + trunc(), a no-op on an already-integral value -- see above
}

int32_t ref_x87_floor_worker_count_times_util(int32_t worker_count, double util) {
    double product;
    // clang-format off
    __asm {
        fild    dword ptr [worker_count]  ; ST(0) = (double)worker_count             (0x004e3fb7)
        fmul    qword ptr [util]          ; ST(0) = worker_count * util, 80-bit      (0x004e3fd1)
        fstp    qword ptr [product]       ; round ONCE, to double, for the CRT call  (0x004e3fdb)
    }
    // clang-format on
    return (int32_t)std::floor(product);
}

double ref_x87_ratio(uint32_t numer, uint32_t denom) {
    // The original's own instruction sequence, so the rounding is the hardware's and not the
    // compiler's choice of SSE2 -- see the header for why that distinction is load-bearing here.
    // Both halves are built the way 0x004e7c8b-0x004e7caa builds them: the accumulator in the low
    // dword, an explicit 0 in the high dword, then FILD of the qword.
    int64_t n64 = (int64_t)(uint64_t)numer;
    int64_t d64 = (int64_t)(uint64_t)denom;
    // NOT named `out` -- that is an x86 instruction mnemonic and MSVC warns C4405 on it inside an
    // __asm block.
    double quotient = 0.0;
    // clang-format off  (one instruction per line; clang-format would fold the __asm block)
    __asm {
        fild    qword ptr [n64]     ; ST(0) = numer            (0x004e7c96)
        fild    qword ptr [d64]     ; ST(0) = denom, ST(1) = numer (0x004e7caa)
        fdivp   st(1), st(0)        ; ST(0) = numer / denom    (0x004e7cad)
        fstp    qword ptr [quotient]     ; round once, to double    (0x004e7caf)
    }
    // clang-format on
    return quotient;
}

// ---- CASE Q REFERENCE ARM: the `fistp qword` + LOW-DWORD sink, verbatim ---------------------------
int32_t ref_qword_low32(double value) {
    int64_t  r       = 0;
    uint16_t cw_save = 0, cw_trunc = 0;
    // clang-format off
    __asm {
        fld     qword ptr [value]
        fstcw   cw_save
        mov     ax, cw_save
        mov     ah, 0x1f
        mov     cw_trunc, ax
        fldcw   cw_trunc
        frndint
        fldcw   cw_save
        fistp   qword ptr [r]
    }
    // clang-format on
    return (int32_t)(uint32_t)(uint64_t)r;
}


int32_t ref_trunc_qword_low(double x) {
    int64_t  r       = 0;
    uint16_t cw_save = 0, cw_trunc = 0;
    // The original never round-trips this value through memory between llm_math_scale_pct's ST0
    // return and utils_math_trunc's FRNDINT (0x004d3417-0x004d341c has no FLD at all); this FLD
    // exists only because our marshalled `c.scale_pct(...)` returns through a `double` (already the
    // interop layer's precision floor -- see every other sim/ TU's `sqrt_fn`/etc.) into a C++ local,
    // so bringing it back onto the FPU stack here reproduces the same 64-bit value with no further
    // loss versus calling this helper directly on ST0.
    // clang-format off
    __asm {
        fld     qword ptr [x]
        fstcw   cw_save
        mov     ax, cw_save
        mov     ah, 0x1f
        mov     cw_trunc, ax
        fldcw   cw_trunc
        frndint
        fldcw   cw_save
        fistp   qword ptr [r]       ;                                          (0x004d3421)
    }
    // clang-format on
    return static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint64_t>(r)));
}

int32_t ref_x87_scale_and_trunc(uint32_t extract_val, const double *weight) {
    // The high dword is an EXPLICIT zero store at 0x004e3218, so the FILD is zero-extended and a
    // "negative" extract_val would be read as a large positive. Reproduced, not narrowed.
    int64_t  n       = (int64_t)(uint64_t)extract_val; // [ESP+0x60] / [ESP+0x64] @0x004e3210-0x004e3218
    int64_t  r       = 0;
    uint16_t cw_save = 0, cw_trunc = 0;
    // clang-format off  (one instruction per line; clang-format would fold the __asm block)
    __asm {
        mov     eax, weight
        fild    qword ptr [n]       ; ST(0) = (double)extract_val            (0x004e3220)
        fmul    qword ptr [eax]     ; ST(0) *= kernel weight, a DOUBLE       (0x004e3224)
        ; --- utils_math_trunc @0x004d0596, inlined: round toward zero -------------------------
        fstcw   cw_save             ;                                        (0x004d0597)
        mov     ax, cw_save
        mov     ah, 0x1f            ; RC = 11 truncate, PC = 11 extended     (0x004d059f)
        mov     cw_trunc, ax
        fldcw   cw_trunc            ;                                        (0x004d05a4)
        frndint                     ;                                        (0x004d05a7)
        fldcw   cw_save             ; restore BEFORE the store               (0x004d05a9)
        ; -------------------------------------------------------------------------------------
        fistp   qword ptr [r]       ;                                        (0x004e322f)
    }
    // clang-format on
    return (int32_t)(uint32_t)(uint64_t)r; // MOV ECX,[ESP+0x60] -- the LOW dword only (0x004e3233)
}

// ==== CASES T1..T11 + Q: THE STEP-3 CONVERSIONS ===================================================
//
// Eleven helpers became C++ once the PC=53 ABI guarantee was granted, and each one's equality is
// re-derived here every run against the assembly it replaced. THE CLASSIFICATION THAT PICKED THEM is
// case W above: the width/range axis was measured first, per helper, and only rows that came back
// clean at PC=53 were converted. Five multi-op chains that diverge AT PC=53 stayed assembly -- the
// guarantee does not rescue an exponent excursion, and their refusals are pinned as T-R1..T-R5.
//
// THREE of the eleven are PC=53-DEPENDENT and say so at their definitions: trunc_div_plus1 (4
// divergences at PC=64), trunc_div (4) and trunc_mul (1743 -- the same count, from the same
// k * (1/k) family, that refused trunc_i32_mul). Their PC=64 columns are printed rather than
// asserted to zero, because at PC=64 they are SUPPOSED to differ; asserting 0 there would either
// fail honestly or, worse, quietly encode that the guarantee is unnecessary.
//
// CASE Q is the second sink. `fistp qword` + a low-dword take is NOT `static_cast<int32_t>`: out of
// int32 range the 32-bit store gives 0x80000000 while the 64-bit store gives the 64-bit indefinite,
// whose low dword is 0. Three helpers end that way, so the sink gets its own proof rather than being
// assumed alongside trunc_i32's.

struct trow {
    const char *name;
    int         d53, d64, n;
    bool        pc53_dependent;
};
trow g_t[16];
int  g_tn = 0;

void tnote(const char *name, int diff, int n, bool pc64, bool dep) {
    for (int i = 0; i < g_tn; ++i)
        if (g_t[i].name == name) {
            if (pc64) g_t[i].d64 = diff;
            else g_t[i].d53 = diff;
            g_t[i].n = n;
            return;
        }
    g_t[g_tn].name           = name;
    g_t[g_tn].d53            = pc64 ? -1 : diff;
    g_t[g_tn].d64            = pc64 ? diff : -1;
    g_t[g_tn].n              = n;
    g_t[g_tn].pc53_dependent = dep;
    ++g_tn;
}

void sweep_conversions(bool pc64) {
    int n, d;

    n = d = 0;
    for (double a : C_DBLS)
        for (double b : C_DBLS) {
            ++n;
            if (mh::fp::trunc_div_plus1(a, b) != ref_trunc_div_plus1(a, b)) ++d;
        }
    tnote("T1  trunc_div_plus1", d, n, pc64, true);

    n = d = 0;
    for (double a : C_DBLS)
        for (double b : C_DBLS) {
            ++n;
            if (mh::fp::trunc_add(a, b) != ref_trunc_add(a, b)) ++d;
        }
    tnote("T2  trunc_add", d, n, pc64, false);

    n = d = 0;
    for (double a : C_DBLS)
        for (double b : C_DBLS) {
            ++n;
            if (mh::fp::trunc_sub(a, b) != ref_trunc_sub(a, b)) ++d;
        }
    tnote("T3  trunc_sub", d, n, pc64, false);

    n = d = 0;
    for (double a : C_DBLS)
        for (double b : C_DBLS) {
            ++n;
            if (mh::fp::trunc_mul(a, b) != ref_trunc_mul(a, b)) ++d;
        }
    for (int k = 2; k < 4000; ++k) {
        ++n;
        if (mh::fp::trunc_mul((double)k, 1.0 / k) != ref_trunc_mul((double)k, 1.0 / k)) ++d;
    }
    tnote("T4  trunc_mul", d, n, pc64, true);

    n = d = 0;
    for (double a : C_DBLS)
        for (double b : C_DBLS) {
            ++n;
            if (mh::fp::trunc_div(a, b) != ref_trunc_div(a, b)) ++d;
        }
    for (int k = 2; k < 4000; ++k) {
        ++n;
        const double q = (double)k * (1.0 / k);
        if (mh::fp::trunc_div(1.0, q) != ref_trunc_div(1.0, q)) ++d;
    }
    tnote("T5  trunc_div", d, n, pc64, true);

    n = d = 0;
    {
        const float FL[] = {0.0f, -0.0f, 1.0f, -1.0f, 0.5f,
                            16777216.0f, 1e30f, -1e30f, 2147483520.0f,
                            -2147483648.0f, HUGE_VALF, -HUGE_VALF, std::nanf("")};
        for (float v : FL) {
            ++n;
            if (mh::fp::trunc_float_to_int32(v) != ref_trunc_float_to_int32(v)) ++d;
        }
        for (int i = -60000; i <= 60000; i += 7) {
            const float v = (float)i * 0.5f;
            ++n;
            if (mh::fp::trunc_float_to_int32(v) != ref_trunc_float_to_int32(v)) ++d;
        }
    }
    tnote("T6  trunc_float_to_int32", d, n, pc64, false);

    n = d = 0;
    for (uint32_t u : C_U32S)
        for (double add : C_DBLS) {
            ++n;
            if (mh::fp::trunc_add_u32_double_low(u, &add) != ref_trunc_add_u32_double_low(u, &add))
                ++d;
        }
    tnote("T7  trunc_add_u32_double_low", d, n, pc64, false);

    n = d = 0;
    for (uint32_t u : C_U32S) {
        ++n;
        if (mh::fp::x87_sqrt_and_trunc(u) != ref_x87_sqrt_and_trunc(u)) ++d;
    }
    for (uint32_t u = 1; u < 400000u; u += 7) {
        ++n;
        if (mh::fp::x87_sqrt_and_trunc(u) != ref_x87_sqrt_and_trunc(u)) ++d;
    }
    tnote("T8  x87_sqrt_and_trunc", d, n, pc64, false);

    n = d = 0;
    {
        const float RA[] = {0.0f, -0.0f, 0.2f, 1.0f, -1.0f,
                            1e30f, -1e30f, HUGE_VALF, -HUGE_VALF, std::nanf("")};
        for (int32_t v : C_INTS)
            for (float r : RA) {
                ++n;
                if (mh::fp::x87_floor_diff_times_ratio(v, &r) != ref_x87_floor_diff_times_ratio(v, &r))
                    ++d;
            }
    }
    tnote("T9  x87_floor_diff_times_ratio", d, n, pc64, false);

    n = d = 0;
    for (int32_t v : C_INTS)
        for (double u : C_DBLS) {
            ++n;
            if (mh::fp::x87_floor_worker_count_times_util(v, u) != ref_x87_floor_worker_count_times_util(v, u))
                ++d;
        }
    tnote("T10 x87_floor_worker_count_times_util", d, n, pc64, false);

    // BY BITS: this one returns a double, so a sign-of-zero or a NaN difference must not pass.
    n = d = 0;
    for (uint32_t a : C_U32S)
        for (uint32_t b : C_U32S) {
            ++n;
            if (!same_bits(mh::fp::x87_ratio(a, b), ref_x87_ratio(a, b))) ++d;
        }
    tnote("T11 x87_ratio", d, n, pc64, false);

    // Q: the second sink, on its own. The edge list is the point -- both out-of-range directions and
    // every non-finite, because that is the ONLY region where it differs from trunc_i32.
    n = d = 0;
    {
        const double QE[] = {0.0,
                             -0.0,
                             1.0,
                             -1.0,
                             2147483647.0,
                             2147483648.0,
                             -2147483648.0,
                             -2147483649.0,
                             4294967295.0,
                             4294967296.0,
                             9007199254740992.0,
                             9.2233720368547758e18,
                             -9.2233720368547758e18,
                             1e30,
                             -1e30,
                             DBL_MAX,
                             -DBL_MAX,
                             HUGE_VAL,
                             -HUGE_VAL,
                             std::nan("")};
        for (double v : QE) {
            ++n;
            if (mh::fp::trunc_i64_low32(v) != ref_qword_low32(v)) ++d;
        }
        for (int i = -60000; i <= 60000; i += 7) {
            const double v = i * 0.5;
            ++n;
            if (mh::fp::trunc_i64_low32(v) != ref_qword_low32(v)) ++d;
        }
    }
    tnote("Q   trunc_i64_low32 (fistp qword sink)", d, n, pc64, false);

    n = d = 0;
    {
        const double QE[] = {0.0, -0.0, 1.0, -1.0, 2147483647.0, 2147483648.0,
                             -2147483649.0, 4294967296.0, 9007199254740992.0,
                             9.2233720368547758e18, -9.2233720368547758e18,
                             1e30, -1e30, DBL_MAX, -DBL_MAX, HUGE_VAL, -HUGE_VAL,
                             std::nan("")};
        for (double v : QE) {
            ++n;
            if (mh::fp::trunc_qword_low(v) != ref_trunc_qword_low(v)) ++d;
        }
        for (int i = -60000; i <= 60000; i += 7) {
            const double v = i * 0.5;
            ++n;
            if (mh::fp::trunc_qword_low(v) != ref_trunc_qword_low(v)) ++d;
        }
    }
    tnote("T12 trunc_qword_low", d, n, pc64, false);

    n = d = 0;
    for (uint32_t u : C_U32S)
        for (double w : C_DBLS) {
            ++n;
            if (mh::fp::x87_scale_and_trunc(u, &w) != ref_x87_scale_and_trunc(u, &w)) ++d;
        }
    for (int k = 2; k < 4000; ++k) {
        const double w = 1.0 / k;
        ++n;
        if (mh::fp::x87_scale_and_trunc((uint32_t)k, &w) != ref_x87_scale_and_trunc((uint32_t)k, &w))
            ++d;
    }
    tnote("T13 x87_scale_and_trunc", d, n, pc64, true);
}

void report_conversions() {
    char msg[224];
    printf("\n  ---- T: the step-3 conversions, C++ vs the assembly it replaced ----\n");
    printf("  %-38s %8s %8s %9s   %s\n", "helper", "PC=53", "PC=64", "inputs", "note");
    for (int i = 0; i < g_tn; ++i) {
        const trow &t = g_t[i];
        printf("  %-38s %8d %8d %9d   %s\n", t.name, t.d53, t.d64, t.n,
               t.pc53_dependent ? "PC=53-DEPENDENT (PC=64 column is EXPECTED nonzero)"
                                : "not PC-dependent");
        // The guarantee is PC=53, so PC=53 is where the equality is asserted -- for every row.
        std::snprintf(msg, sizeof(msg), "%s: equals the assembly at PC=53 (%d of %d differ)", t.name,
                      t.d53, t.n);
        ck(t.d53 == 0, msg);
        // A row claiming to be precision-independent is HELD to it; a PC=53-dependent row is held to
        // the opposite, so "we needed the guarantee" stays a measured claim rather than a habit.
        if (!t.pc53_dependent) {
            std::snprintf(msg, sizeof(msg),
                          "%s: ...and at PC=64 too, so it is genuinely precision-independent (%d)",
                          t.name, t.d64);
            ck(t.d64 == 0, msg);
        } else {
            std::snprintf(msg, sizeof(msg),
                          "%s: PC=64 STILL diverges (%d) -- the PC=53 guarantee is load-bearing here, "
                          "not decoration",
                          t.name, t.d64);
            ck(t.d64 > 0, msg);
        }
    }
}

} // namespace

int run_fptest() {
    printf("=== fptest: mh/fp/x87.h still equals the assembly it replaced (CRT-X87 step 2) ===\n");

    unsigned cur = 0;

    _controlfp_s(&cur, _PC_53, _MCW_PC);
    ck(((read_cw() >> 8) & 3) == 2, "PC=53 is installed (the harness pin_fpu setting)");
    sweep("PC=53");
    sweep_compares("PC=53", false);
    sweep_widths(false);
    sweep_conversions(false);

    _controlfp_s(&cur, _PC_64, _MCW_PC);
    ck(((read_cw() >> 8) & 3) == 3, "PC=64 is installed (the bare x87 default)");
    sweep("PC=64");
    sweep_compares("PC=64", true);
    sweep_widths(true);
    sweep_conversions(true);

    // ---- N. THE NEGATIVE ARM ---------------------------------------------------------------------
    //
    // Everything above is an equality, so it is worth nothing unless a real difference would show. At
    // PC=64 a 53-bit intermediate MUST change these three answers; if it stops doing so, the whole
    // sweep has gone blind and this case says so rather than passing quietly.
    {
        int sensitive = 0;
        for (int k : SENSITIVE)
            if (forced53_mul(k, 1.0 / k) != ref_trunc_i32_mul(k, 1.0 / k)) ++sensitive;
        ck(sensitive == 3,
           "N: at PC=64 a 53-bit intermediate DIVERGES on all 3 adversarial inputs -- the sweep can "
           "see the failure it is claiming not to find");
        ck(forced53_mul(3, 1.0 / 3.0) == 1 && ref_trunc_i32_mul(3, 1.0 / 3.0) == 0,
           "N: ...and concretely, 3 * (1/3) is 1 rounded to 53 bits and 0 kept at 64");
    }

    // ---- P. THE PC=53 GUARANTEE, TESTED RATHER THAN ASSERTED -------------------------------------
    //
    // Eleven helpers are C++ because libmh guarantees PC=53, and three of them are measurably wrong
    // at PC=64 (the T table above prints the counts and REQUIRES them nonzero, so the dependency
    // cannot quietly stop being real). A guarantee that is only written down is not a guarantee, so
    // this arm exercises the install itself.
    //
    // IT STARTS AT PC=64 ON PURPOSE. Asserting "the word is 53" after doing nothing would pass in
    // this process no matter what the installer does -- the CRT already leaves it at 53, which is
    // the whole reason the guarantee had to become an install rather than an observation. Starting
    // at 64 means only a working install can make this green: mutation is built in rather than
    // bolted on.
    {
        _controlfp_s(&cur, _PC_64, _MCW_PC);
        ck(mh::fp::pc_in_force() == 64, "P: the probe starts at PC=64, so the install has work to do");
        const int ok = mh::fp::install_pc53();
        ck(ok == 1, "P: install_pc53 reports success");
        ck(mh::fp::pc_in_force() == 53,
           "P: ...and the control word ACTUALLY reads 53 afterwards -- the libmh.h guarantee is "
           "installed, not inherited");
        // Idempotent, as the contract says.
        ck(mh::fp::install_pc53() == 1 && mh::fp::pc_in_force() == 53,
           "P: calling it twice is safe and still leaves 53 (the contract says idempotent)");
        // ONLY the precision field: rounding must survive untouched, because libmh guarantees a
        // precision and not a whole FP environment -- a host that chose a rounding mode keeps it.
        unsigned before = 0, after = 0;
        _controlfp_s(&before, _RC_UP, _MCW_RC);
        mh::fp::install_pc53();
        _controlfp_s(&after, 0, 0);
        ck((after & _MCW_RC) == _RC_UP,
           "P: the install leaves the ROUNDING mode alone (precision guarantee, not an FP-environment "
           "takeover)");
        _controlfp_s(&cur, _RC_NEAR, _MCW_RC);
    }

    // Back to the CRT default so a later suite in the same process is not surprised.
    _controlfp_s(&cur, _PC_53, _MCW_PC);

    report_widths();
    report_conversions();

    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
