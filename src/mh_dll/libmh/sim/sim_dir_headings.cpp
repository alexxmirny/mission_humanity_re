//
// sim/sim_dir_headings.cpp -- see sim_dir_headings.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_dir_from_to_0049482b.asm, tmp/decomp/llm_strat_dir_sector_to_004948ff.asm),
// not from Ghidra's .c: llm_strat_dir_from_to's own .c draft plate still describes an 8-way/45-degree
// octant, which the batch context says is WRONG (the real constant block's SECTOR_DEG is 15.0, a
// 24-way heading) -- every operand and constant below was re-walked against the raw
// FLD/FDIV/FADD/FCOMP/JBE/JC instructions and cross-checked against the already-corrected
// sim_view::dir24_*/dir128_* bindings, not against either draft's prose.
//
#include "sim/sim_dir_headings.h"

#include "addr/mh_calls.gen.h"  // typed callables for the two original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"
#include "fp/x87_shapes.h"  // CRT-X87: the hoisted x87 blocks (the asm moved, it did not change)
#include "crt/crt_select.h" // LIB-CRT: MH_CRT() picks the vendored CRT in the standalone build

namespace mh::sim {

const dir_headings_calls &live_dir_headings_calls() {
    static const dir_headings_calls c = {
        MH_LIBMH_BIND(llm_strat_map_wrapped_delta),
        MH_CRT(llm_math_atan),
    };
    return c;
}

namespace {

// Shared FLD/FDIV/[inlined utils_math_trunc]/FISTP sequence -- byte-for-byte identical between
// dir_from_to (0x004948dd-0x004948eb) and dir_sector_to (0x00494997-0x004949bf) modulo the divisor
// value, so extracted once here rather than duplicated (see the header banner: this is a private
// implementation detail of this TU, not a new outward callee). utils_math_trunc @0x004d0596 is the
// established unmarshallable case (x87-register-only leaf; see sim_facing24_from_points.cpp's
// sector_index_from_angle for the identical inline sequence and its own derivation). UNLIKE that
// sibling, there is no FLD1/FADDP before the trunc call here -- the "+1" is applied by the CALLER
// after this returns, as a plain integer add (see the header banner).
int32_t angle_div_sector_trunc(double angle_deg, double sector_deg) {
    return ::mh::fp::trunc_div(angle_deg, sector_deg);
}

} // namespace

namespace detail {

int32_t dir_from_to(const sim_view &v, const dir_headings_calls &c, int32_t x1, int32_t y1, int32_t x2,
                    int32_t y2) {
    // 0x00494854-0x00494860: call order (x2, y1, x1, y2, &dx, &dy) -- read directly off the register
    // loads, NOT the natural (x1,y1,x2,y2) reading. dx is loaded from the FIRST out-pointer pushed
    // (the one pushed LAST, per x86 push order), dy from the second.
    double dx = 0.0, dy = 0.0;
    c.wrapped_delta(x2, y1, x1, y2, &dx, &dy);

    // 0x00494865-0x00494871: atan(dx / dy), UNGUARDED for dy == 0.0 exactly as the original leaves
    // it (IEEE-754 division yields +-infinity or NaN, propagated rather than trapped on).
    double angle_deg = c.atan(dx / dy);

    // 0x0049487c-0x00494888: radians -> degrees as ONE combined expression (no intermediate store
    // between the FMUL and the FDIV).
    angle_deg = (angle_deg * *v.dir24_rad2deg_num) / *v.dir24_rad2deg_den;

    // 0x0049488d-0x0049489e: FCOMP 0.0 vs dy; JBE (0.0<=dy) skips -- applies when dy < 0.0 (atan's
    // [-90,90] domain needing a half-turn flip for the other two quadrants).
    if (dy < 0.0)
        angle_deg = angle_deg + *v.dir24_half_turn_deg;

    // 0x004948a1-0x004948aa: unconditional pre-quantization bias.
    angle_deg = angle_deg + *v.dir24_bias_deg;

    // 0x004948af-0x004948c0: FCOMP 0.0 vs angle_deg; JBE (0.0<=angle_deg) skips -- applies when
    // angle_deg < 0.0.
    if (angle_deg < 0.0)
        angle_deg = angle_deg + *v.dir24_wrap_add_deg;

    // 0x004948c6-0x004948da: FCOMP angle_deg vs WRAP_LIMIT_DEG; JC (angle_deg<limit) skips -- applies
    // when angle_deg >= WRAP_LIMIT_DEG (WRAP_SUB_DEG is negative, so this is a subtraction).
    if (angle_deg >= *v.dir24_wrap_limit_deg)
        angle_deg = angle_deg + *v.dir24_wrap_sub_deg;

    // 0x004948dd-0x004948f1: trunc(angle_deg / SECTOR_DEG), THEN integer +1 (INC EAX at 0x004948f1)
    // -- see the header banner on why this is not folded into the float-domain "+1" the facing24
    // sibling uses.
    int32_t sector = angle_div_sector_trunc(angle_deg, *v.dir24_sector_deg);
    return sector + 1;
}

int32_t dir_sector_to(const sim_view &v, const dir_headings_calls &c, int32_t x0, int32_t y0, int32_t x1,
                      int32_t y1) {
    // 0x00494914-0x00494934: EAX/EDX/EBX/ECX (x0,y0,x1,y1) spilled to their stack homes then
    // immediately RE-LOADED, in a reshuffled register order, to build the wrapped_delta call below --
    // this IS how the call's argument order differs from the parameter order, not dead code; the
    // intermediate stack round-trip has no OTHER reader anywhere in the body, so it collapses to
    // passing the reshuffled arguments directly, which is what the call below does.

    // 0x00494928-0x00494934: call order (x1, y0, x0, y1, &dx, &dy) -- DIFFERENT scramble from
    // dir_from_to's own (see the header banner and the function's own Ghidra plate, which flags this
    // explicitly).
    double dx = 0.0, dy = 0.0;
    c.wrapped_delta(x1, y0, x0, y1, &dx, &dy);

    // 0x00494939-0x00494945: atan(dx / dy), UNGUARDED for dy == 0.0, same as dir_from_to.
    double angle_deg = c.atan(dx / dy);

    // 0x00494950-0x0049495c: radians -> degrees, one combined expression.
    angle_deg = (angle_deg * *v.dir128_rad2deg_num) / *v.dir128_rad2deg_den;

    // 0x00494961-0x00494972: applies when dy < 0.0.
    if (dy < 0.0)
        angle_deg = angle_deg + *v.dir128_half_turn_deg;

    // 0x00494978-0x0049497e: unconditional pre-quantization bias.
    angle_deg = angle_deg + *v.dir128_bias_deg;

    // 0x00494983-0x00494994: applies when angle_deg < 0.0.
    if (angle_deg < 0.0)
        angle_deg = angle_deg + *v.dir128_wrap_add_deg;

    // 0x0049499a-0x004949ae: applies when angle_deg >= WRAP_LIMIT_DEG (WRAP_SUB_DEG is negative).
    if (angle_deg >= *v.dir128_wrap_limit_deg)
        angle_deg = angle_deg + *v.dir128_wrap_sub_deg;

    // 0x004949b1-0x004949c5: trunc(angle_deg / SECTOR_DEG), THEN integer +1 (INC EAX at 0x004949c5).
    int32_t sector = angle_div_sector_trunc(angle_deg, *v.dir128_sector_deg);
    return sector + 1;
}

} // namespace detail

// ---- the public wrappers ------------------------------------------------------------------------

int32_t dir_from_to(int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    const sim_view v = state().read;
    return detail::dir_from_to(v, live_dir_headings_calls(), x1, y1, x2, y2);
}

int32_t dir_sector_to(int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    const sim_view v = state().read;
    return detail::dir_sector_to(v, live_dir_headings_calls(), x0, y0, x1, y1);
}


} // namespace mh::sim
