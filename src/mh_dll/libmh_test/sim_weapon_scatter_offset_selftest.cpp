//
// sim_weapon_scatter_offset_selftest.cpp -- `simtest` cases for llm_strat_weapon_scatter_offset
// @0x0048c6b2 (sim/sim_weapon_projectile_spawn.h/.cpp).
//
// WHY THIS FILE IS THE ONLY EVIDENCE THIS FUNCTION WILL EVER GET: it writes no tracked region and
// returns nothing -- both results leave only through the caller's stack out-pointers, so a
// differential shadow arm (armed on this function alone) would compare nothing at all. This offline
// oracle is it.
//
// SCOPE (honest, not exhaustive): the full formula chain -- accuracy_divisor's truncating division
// (incl. a NEGATIVE shooter_experience case that separates trunc-toward-zero from floor), the
// abs(dx)+abs(dy) sum (incl. mixed-sign deltas that separate |dx|+|dy| from dx+dy or |dx+dy|), the two
// chained truncating IDIVs that produce upper_bound, the upper_bound==0 early-out (both outputs
// zeroed, rand_below NOT called -- both a zero-scale weapon and an ordinary truncated-to-zero
// nonzero product are covered), the non-zero branch's two INDEPENDENT rand_below draws in x-then-y
// order sharing the SAME upper_bound argument, and the half_trunc(upper_bound)-draw output formula
// driven across the full roll range (0, 1, midpoint, upper_bound-1) for a positive odd, a positive
// even, and a NEGATIVE odd upper_bound (half_trunc's shift-form idiom only diverges from a naive
// `x>>1` floor shift on an odd negative operand, so the negative-odd case is the one that actually
// exercises it).
//
// DOES NOT COVER (and cannot, from this function alone): this body has NO floating point in it --
// re-confirmed against the .asm below (zero FLD/FMUL/FDIV instructions in the whole 0xe8-byte body).
// The migration brief for this batch predicted FP density from the surrounding subsystem's other
// functions, but this specific function is pure integer arithmetic, so there is nothing here for
// ck_eq_d to pin. DOES NOT COVER: which of the two locals (dx vs dy) receives which raw
// pixel_delta_wrapped out-pointer write is UNOBSERVABLE from this function's own behaviour --
// abs_sum = abs(dy) + abs(dx) is symmetric under exchanging dx and dy, so an internal swap of the
// &dx/&dy arguments passed to pixel_delta_wrapped could not change any assertion this file (or any
// offline oracle) could ever make; noted as a real gap, not an oversight. Also not covered: the
// accuracy_divisor == 0 case (shooter_experience_or_zero in [-19,-10], giving a genuine integer
// divide-by-zero at 0x0048c71f) -- that is the ORIGINAL's own behaviour, reproduced identically by
// the translation, and exercising it here would crash the whole simtest binary rather than produce a
// diagnosable failure.
//
// EXPECTED BEHAVIOUR HAND-DERIVED FROM THE DISASSEMBLY
// (tmp/decomp_sim/llm_strat_weapon_scatter_offset_0048c6b2.asm) -- NOT from the .cpp under test. Every
// constant/bound cited below is an address in that file. The register/stack parameter mapping (from
// the .asm header + confirmed by tracing every read of [EBP-0x2c]/[EBP-0x28]/[EBP-0x24]/[EBP-0x20]):
// EAX=shooter_experience_or_zero, EDX=weapon_missing_scale, EBX=src_x, ECX=src_y,
// [EBP+8]=dst_x, [EBP+0xc]=dst_y, [EBP+0x10]=out_scatter_x, [EBP+0x14]=out_scatter_y -- matching the
// .cpp's `detail::weapon_scatter_offset` parameter order exactly. The full body:
//   0x0048c6d3-0x0048c6ec  pixel_delta_wrapped(src_x, src_y, dst_x, dst_y, &dx, &dy)
//   0x0048c6ec-0x0048c6fd  accuracy_divisor = shooter_experience_or_zero/10 + 1     (signed IDIV, INC)
//   0x0048c700-0x0048c712  abs_sum = abs(dy) + abs(dx)                              (CDQ/XOR/SUB x2)
//   0x0048c714-0x0048c730  upper_bound = weapon_missing_scale*abs_sum/accuracy_divisor/200 (IMUL, 2x IDIV)
//   0x0048c733-0x0048c737  CMP upper_bound,0 / JZ LAB_0048c77f                       (EQUALITY, not <=)
//   0x0048c739-0x0048c75b  *out_scatter_x = half_trunc(upper_bound) - rand_below(upper_bound)
//   0x0048c75b-0x0048c77b  *out_scatter_y = half_trunc(upper_bound) - rand_below(upper_bound)  (2nd,
//                          INDEPENDENT draw, own CALL, same upper_bound)
//   LAB_0048c77f-0x0048c791  *out_scatter_x = 0; *out_scatter_y = 0
// half_trunc(x) is the signed-divide-by-2 idiom (x - (x>>31)) >> 1 (0x0048c744-0x0048c751 /
// 0x0048c766-0x0048c773) -- provably equal to C++'s truncating x/2 for every input, pinned here anyway
// because a naive `x>>1` alone (an arithmetic-shift floor) would only diverge from it on an odd
// NEGATIVE x -- see test_negative_odd_upper_bound_full_range below.
//
#include "sim/sim_weapon_projectile_spawn.h"

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- shared trace: proves CALL ORDER (pixel_delta_wrapped before either rand_below draw, and the
// x-draw before the y-draw) -- same "one shared trace" idiom sim_bldg_state_destroyed_selftest.cpp
// uses for its (much larger) callee spine.
std::vector<const char *> g_trace;
void                      tr(const char *tag) { g_trace.push_back(tag); }

struct scatter_recorder {
    int32_t              n_delta = 0;
    int32_t              px1 = 0, py1 = 0, px2 = 0, py2 = 0; // what pixel_delta_wrapped was handed
    int32_t              dx_out = 0, dy_out = 0;             // what it writes back through the two out-pointers
    std::vector<int32_t> rand_ub;                            // upper_bound argument, one entry per rand_below call
    std::vector<int32_t> rand_ret;                           // canned return value, in call order
};
scatter_recorder g_r;

// Read a recorded upper_bound SAFELY. Every `rand_ub[i]` assertion below presumes the non-zero branch
// was taken and made two draws -- but the whole point of several of those cases is the branch SENSE,
// so a wrong translation is exactly the situation in which the vector is EMPTY. A bare `rand_ub[0]`
// on an empty vector is UB: the mutation campaign's M4 (`upper_bound == 0` weakened to `<= 0`) made
// the process die instead of reporting a named failure, and a crash is an ambiguous mutation result
// -- it can equally mean "the test caught it" or "the test has its own bug" (the same ambiguity
// SIM1D-V hit from the other direction, where a plain-build pass was really an ASan-visible OOB read).
// Returning a sentinel that no real case uses turns that crash into a legible `ck_eq` failure naming
// the property. Added 2026-08-16 after M4.
constexpr int32_t UB_ABSENT = 0x7f5e5e5e;
int32_t           ub_at(size_t i) { return i < g_r.rand_ub.size() ? g_r.rand_ub[i] : UB_ABSENT; }

void reset_all() {
    g_r = scatter_recorder{};
    g_trace.clear();
}

const weapon_scatter_offset_calls &rec_calls() {
    static const weapon_scatter_offset_calls c = {
        [](int32_t src_x, int32_t src_y, int32_t dst_x, int32_t dst_y, int32_t *out_dx, int32_t *out_dy) {
            tr("pixel_delta_wrapped");
            g_r.n_delta++;
            g_r.px1 = src_x;
            g_r.py1 = src_y;
            g_r.px2 = dst_x;
            g_r.py2 = dst_y;
            *out_dx = g_r.dx_out;
            *out_dy = g_r.dy_out;
        },
        [](int32_t upper_bound) -> int32_t {
            tr("rand_below");
            g_r.rand_ub.push_back(upper_bound);
            const size_t i = g_r.rand_ub.size() - 1;
            return i < g_r.rand_ret.size() ? g_r.rand_ret[i] : 0;
        },
    };
    return c;
}

void call_scatter(int32_t shooter_experience_or_zero, int32_t weapon_missing_scale, int32_t src_x,
                  int32_t src_y, int32_t dst_x, int32_t dst_y, int32_t &out_x, int32_t &out_y) {
    detail::weapon_scatter_offset(shooter_experience_or_zero, weapon_missing_scale, src_x, src_y,
                                  dst_x, dst_y, &out_x, &out_y, rec_calls());
}

// ==== call shape: pixel_delta_wrapped's argument order, unpermuted ================================

void test_pixel_delta_args_unpermuted() {
    reset_all();
    g_r.dx_out    = 1;
    g_r.dy_out    = 0;
    int32_t out_x = -1, out_y = -1;
    call_scatter(0, 0, 11, 22, 33, 44, out_x, out_y);
    ck_eq((uint32_t)g_r.n_delta, 1u, "scatter: pixel_delta_wrapped called exactly once (0x0048c6e7 CALL)");
    ck(g_r.px1 == 11 && g_r.py1 == 22 && g_r.px2 == 33 && g_r.py2 == 44,
       "scatter: pixel_delta_wrapped(src_x=11, src_y=22, dst_x=33, dst_y=44) unpermuted -- "
       "EAX=src_x/EDX=src_y/EBX=dst_x/ECX=dst_y set up at 0x0048c6db-0x0048c6e4");
}

// ==== upper_bound == 0: both outputs zeroed, rand_below NOT called ================================

void test_upper_bound_zero_from_positive_truncation() {
    reset_all();
    g_r.dx_out    = 1;
    g_r.dy_out    = 0;                // abs_sum = 1
    int32_t out_x = 777, out_y = 888; // sentinels that must be overwritten
    // weapon_missing_scale=199, accuracy_divisor=1 (shooter_experience_or_zero=0): 199*1/1/200 = 0 --
    // an ordinary truncating divide of a positive nonzero product below 200, not a zero-scale weapon.
    call_scatter(0, 199, 0, 0, 0, 0, out_x, out_y);
    ck_eq((uint32_t)g_r.rand_ub.size(), 0u,
          "scatter(upper_bound=0): rand_below NOT called (0x0048c737 JZ over both draws)");
    ck_eq((uint32_t)out_x, 0u, "scatter(upper_bound=0): *out_scatter_x = 0 (0x0048c782)");
    ck_eq((uint32_t)out_y, 0u, "scatter(upper_bound=0): *out_scatter_y = 0 (0x0048c78b)");
}

void test_upper_bound_zero_from_zero_missing_scale() {
    reset_all();
    g_r.dx_out    = 50;
    g_r.dy_out    = 50; // abs_sum = 100, nonzero if the scale weren't 0
    int32_t out_x = 5, out_y = -5;
    call_scatter(0, 0, 0, 0, 0, 0, out_x, out_y); // a weapon with zero "missing" spread
    ck_eq((uint32_t)g_r.rand_ub.size(), 0u, "scatter(weapon_missing_scale=0): no rand_below calls");
    ck_eq((uint32_t)out_x, 0u, "scatter(weapon_missing_scale=0): out_scatter_x zeroed");
    ck_eq((uint32_t)out_y, 0u, "scatter(weapon_missing_scale=0): out_scatter_y zeroed");
}

// ==== abs_sum = |dx| + |dy|, not dx+dy or |dx+dy| ==================================================

void test_abs_sum_uses_absolute_values_not_raw_sum() {
    reset_all();
    // |dx|=15, |dy|=9 -> abs_sum=24. dx+dy=-6 and |dx+dy|=6 are both WRONG if either were used instead.
    g_r.dx_out    = -15;
    g_r.dy_out    = 9;
    g_r.rand_ret  = {0, 0};
    int32_t out_x = 0, out_y = 0;
    // weapon_missing_scale=200, accuracy_divisor=1 -> upper_bound = 200*abs_sum/1/200 = abs_sum exactly.
    call_scatter(0, 200, 0, 0, 0, 0, out_x, out_y);
    ck_eq((uint32_t)g_r.rand_ub.size(), 2u, "scatter(abs test): non-zero branch, two draws");
    ck_eq((uint32_t)ub_at(0), 24u,
          "scatter: upper_bound = |dx|(15)+|dy|(9) = 24 (0x0048c703-0x0048c712, CDQ/XOR/SUB abs idiom "
          "applied to BOTH deltas), not dx+dy=-6 or |dx+dy|=6");
    ck_eq((uint32_t)ub_at(1), 24u, "scatter: the second rand_below draw gets the SAME upper_bound");
}

// ==== accuracy_divisor = shooter_experience_or_zero/10 + 1, TRUNCATING toward zero =================

void test_accuracy_divisor_negative_experience_truncates_not_floors() {
    reset_all();
    g_r.dx_out    = 1;
    g_r.dy_out    = 0; // abs_sum = 1
    g_r.rand_ret  = {0, 0};
    int32_t out_x = 0, out_y = 0;
    // shooter_experience_or_zero=-25: -25/10 truncates toward zero to -2 (0x0048c6f7 SAR+IDIV is an
    // ordinary signed divide, matching C++ `/`), so accuracy_divisor = -2+1 = -1. weapon_missing_scale
    // =200, abs_sum=1 -> upper_bound = 200*1/(-1)/200 = -1 (NONZERO -> the branch fires, rand_below is
    // called). A FLOOR-style accuracy_divisor would instead be -3+1=-2, giving 200/(-2)/200 = -100/200
    // = 0 (the truncating divide rounds toward zero) -- the ZERO branch, no rand_below call at all. The
    // call COUNT below is therefore itself the trunc-vs-floor assertion.
    call_scatter(-25, 200, 0, 0, 0, 0, out_x, out_y);
    ck_eq((uint32_t)g_r.rand_ub.size(), 2u,
          "scatter(accuracy_divisor trunc): non-zero branch taken -- a floor-style accuracy_divisor "
          "would give upper_bound=0 and zero rand_below calls");
    ck_eq((uint32_t)ub_at(0), (uint32_t)-1, "scatter(accuracy_divisor trunc): upper_bound == -1 exactly");
}

void test_accuracy_divisor_positive_experience() {
    reset_all();
    g_r.dx_out    = 1;
    g_r.dy_out    = 0; // abs_sum = 1
    g_r.rand_ret  = {0, 0};
    int32_t out_x = 0, out_y = 0;
    // shooter_experience_or_zero=35 -> 35/10=3 (trunc), accuracy_divisor=4. weapon_missing_scale=800,
    // abs_sum=1 -> upper_bound = 800*1/4/200 = 1.
    call_scatter(35, 800, 0, 0, 0, 0, out_x, out_y);
    ck_eq((uint32_t)g_r.rand_ub.size(), 2u, "scatter(accuracy_divisor=4): non-zero branch");
    ck_eq((uint32_t)ub_at(0), 1u,
          "scatter(accuracy_divisor=4): upper_bound = 800/4/200 = 1, confirming accuracy_divisor = "
          "35/10+1 = 4 (0x0048c6ec-0x0048c6fd IDIV then INC)");
}

// ==== the non-zero branch's output formula: half_trunc(upper_bound) - draw, full roll range =========
// half_trunc(x) = (x - (x>>31)) >> 1, reproduced as `half_trunc_asm` in the .cpp. Positive/even cases
// below can't separate it from a naive `x>>1`; the negative-odd case can and does.

void test_positive_odd_upper_bound_roll_0() {
    reset_all();
    g_r.dx_out    = 1;
    g_r.dy_out    = 0; // abs_sum=1
    g_r.rand_ret  = {0, 0};
    int32_t out_x = 0, out_y = 0;
    call_scatter(0, 1400, 0, 0, 0, 0, out_x, out_y); // upper_bound = 1400/1/200 = 7
    ck_eq((uint32_t)ub_at(0), 7u, "scatter(upper_bound=7): exact upper_bound argument to rand_below");
    ck_eq((uint32_t)out_x, 3u, "scatter(upper_bound=7, draw=0): half_trunc(7)=3 minus draw 0 = 3");
    ck_eq((uint32_t)out_y, 3u, "scatter(upper_bound=7, draw=0): out_scatter_y matches the same formula");
}

void test_positive_odd_upper_bound_roll_1() {
    reset_all();
    g_r.dx_out    = 1;
    g_r.dy_out    = 0;
    g_r.rand_ret  = {1, 1};
    int32_t out_x = 0, out_y = 0;
    call_scatter(0, 1400, 0, 0, 0, 0, out_x, out_y);
    ck_eq((uint32_t)out_x, 2u, "scatter(upper_bound=7, draw=1): half_trunc(7)=3 minus draw 1 = 2");
    ck_eq((uint32_t)out_y, 2u, "scatter(upper_bound=7, draw=1): out_scatter_y matches the same formula");
}

void test_positive_odd_upper_bound_roll_midpoint() {
    reset_all();
    g_r.dx_out    = 1;
    g_r.dy_out    = 0;
    g_r.rand_ret  = {3, 3};
    int32_t out_x = 0, out_y = 0;
    call_scatter(0, 1400, 0, 0, 0, 0, out_x, out_y);
    ck_eq((uint32_t)out_x, 0u, "scatter(upper_bound=7, draw=3 midpoint): half_trunc(7)=3 minus draw 3 = 0");
    ck_eq((uint32_t)out_y, 0u, "scatter(upper_bound=7, draw=3 midpoint): out_scatter_y matches the same formula");
}

void test_positive_odd_upper_bound_roll_max() {
    reset_all();
    g_r.dx_out    = 1;
    g_r.dy_out    = 0;
    g_r.rand_ret  = {6, 6}; // upper_bound - 1
    int32_t out_x = 0, out_y = 0;
    call_scatter(0, 1400, 0, 0, 0, 0, out_x, out_y);
    ck_eq((uint32_t)out_x, (uint32_t)-3,
          "scatter(upper_bound=7, draw=6=upper_bound-1): half_trunc(7)=3 minus draw 6 = -3");
    ck_eq((uint32_t)out_y, (uint32_t)-3,
          "scatter(upper_bound=7, draw=6=upper_bound-1): out_scatter_y matches the same formula");
}

void test_positive_even_upper_bound_full_range() {
    // upper_bound=10 (weapon_missing_scale=2000, abs_sum=1, accuracy_divisor=1: 2000/1/200=10).
    // half_trunc(10) = (10-0)>>1 = 5 (trivial for a positive even x, still pinned exactly).
    struct {
        int32_t     draw;
        int32_t     want;
        const char *what;
    } cases[] = {
        {0, 5, "scatter(upper_bound=10, draw=0): half_trunc(10)=5 minus draw 0 = 5"},
        {1, 4, "scatter(upper_bound=10, draw=1): half_trunc(10)=5 minus draw 1 = 4"},
        {5, 0, "scatter(upper_bound=10, draw=5 midpoint): half_trunc(10)=5 minus draw 5 = 0"},
        {9, -4, "scatter(upper_bound=10, draw=9=upper_bound-1): half_trunc(10)=5 minus draw 9 = -4"},
    };
    for (const auto &c : cases) {
        reset_all();
        g_r.dx_out    = 1;
        g_r.dy_out    = 0;
        g_r.rand_ret  = {c.draw, c.draw};
        int32_t out_x = 0, out_y = 0;
        call_scatter(0, 2000, 0, 0, 0, 0, out_x, out_y);
        ck_eq((uint32_t)ub_at(0), 10u, "scatter(upper_bound=10): exact upper_bound argument");
        ck_eq((uint32_t)out_x, (uint32_t)c.want, c.what);
        ck_eq((uint32_t)out_y, (uint32_t)c.want, c.what);
    }
}

void test_negative_odd_upper_bound_full_range() {
    // upper_bound=-7 (weapon_missing_scale=-1400, abs_sum=1, accuracy_divisor=1: -1400/1/200=-7).
    // half_trunc(-7) = (-7 - (-1))>>1 = (-6)>>1 = -3 -- the ONE case in this file that actually
    // separates half_trunc_asm's shift-form idiom from a naive `x>>1` floor shift: an arithmetic shift
    // of the ODD negative -7 alone would give floor(-7/2) = -4, not the trunc-toward-zero -3 the
    // (x-sign)>>1 form (and C++'s own `/2`) produce.
    struct {
        int32_t     draw;
        int32_t     want;
        const char *what;
    } cases[] = {
        {0, -3,
         "scatter(upper_bound=-7, draw=0): half_trunc(-7)=-3 minus draw 0 = -3, NOT -4 (a naive x>>1 "
         "floor shift would give half_trunc(-7)=-4)"},
        {1, -4, "scatter(upper_bound=-7, draw=1): half_trunc(-7)=-3 minus draw 1 = -4"},
        {-3, 0, "scatter(upper_bound=-7, draw=-3=half_trunc itself): half_trunc(-7)=-3 minus draw -3 = 0"},
        {-8, 5, "scatter(upper_bound=-7, draw=-8=upper_bound-1): half_trunc(-7)=-3 minus draw -8 = 5"},
    };
    for (const auto &c : cases) {
        reset_all();
        g_r.dx_out    = 1;
        g_r.dy_out    = 0;
        g_r.rand_ret  = {c.draw, c.draw};
        int32_t out_x = 0, out_y = 0;
        call_scatter(0, -1400, 0, 0, 0, 0, out_x, out_y);
        ck_eq((uint32_t)ub_at(0), (uint32_t)-7, "scatter(upper_bound=-7): exact negative upper_bound argument");
        ck_eq((uint32_t)out_x, (uint32_t)c.want, c.what);
        ck_eq((uint32_t)out_y, (uint32_t)c.want, c.what);
    }
}

// ==== call order + out-pointer identity ============================================================

void test_call_order_and_out_pointer_not_swapped() {
    reset_all();
    g_r.dx_out = 1;
    g_r.dy_out = 0; // abs_sum=1
    // ASYMMETRIC draws: if the two out-pointer args (param_7/param_8, [ebp+0x10]/[ebp+0x14]) were
    // swapped internally, out_x and out_y below would come out swapped too.
    g_r.rand_ret  = {2, 5};
    int32_t out_x = 0, out_y = 0;
    call_scatter(0, 1400, 0, 0, 0, 0, out_x, out_y); // upper_bound=7, half_trunc(7)=3
    ck(g_trace.size() == 3 && std::strcmp(g_trace[0], "pixel_delta_wrapped") == 0 &&
           std::strcmp(g_trace[1], "rand_below") == 0 && std::strcmp(g_trace[2], "rand_below") == 0,
       "scatter: call order is pixel_delta_wrapped, THEN rand_below(x), THEN rand_below(y) "
       "(0x0048c6e7, 0x0048c73c, 0x0048c75e)");
    ck_eq((uint32_t)out_x, (uint32_t)(3 - 2), "scatter: out_scatter_x = half_trunc(7)=3 minus the FIRST draw (2) = 1");
    ck_eq((uint32_t)out_y, (uint32_t)(3 - 5),
          "scatter: out_scatter_y = half_trunc(7)=3 minus the SECOND draw (5) = -2, and out_x != out_y "
          "proves param_7/param_8 are not swapped");
}

} // namespace

void run_weapon_scatter_offset_tests() {
    printf("-- llm_strat_weapon_scatter_offset --\n");
    test_pixel_delta_args_unpermuted();
    test_upper_bound_zero_from_positive_truncation();
    test_upper_bound_zero_from_zero_missing_scale();
    test_abs_sum_uses_absolute_values_not_raw_sum();
    test_accuracy_divisor_negative_experience_truncates_not_floors();
    test_accuracy_divisor_positive_experience();
    test_positive_odd_upper_bound_roll_0();
    test_positive_odd_upper_bound_roll_1();
    test_positive_odd_upper_bound_roll_midpoint();
    test_positive_odd_upper_bound_roll_max();
    test_positive_even_upper_bound_full_range();
    test_negative_odd_upper_bound_full_range();
    test_call_order_and_out_pointer_not_swapped();
}

} // namespace mh::sim::test
