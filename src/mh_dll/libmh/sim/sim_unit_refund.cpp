#include "sim/sim_unit_refund.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"
#include "fp/x87.h" // CRT-X87: the shared x87 truncation helpers

namespace mh::sim {

const unit_refund_build_cost_by_health_calls &live_unit_refund_build_cost_by_health_calls() {
    static const unit_refund_build_cost_by_health_calls c = {
        MH_LIBMH_BIND(llm_resource_add),
    };
    return c;
}

namespace {

// _DAT_005014be, bytes `00 00 00 00 00 00 e0 3f` == 0.5 (double), read out of the image and verified
// by the conductor (get-bytes at the VA) -- see the batch context file. A read-only image constant,
// not part of the state view.
inline constexpr double ENERGY_RATIO_SCALE = 0.5; // _DAT_005014be

// utils_math_trunc @0x004d0596 (`MH_UNAVAILABLE__parameter_storage_not_marshallable` in
// mh_calls.gen.h -- ST0 in, ST0 out, x87-register-only, no stack-passable signature). This function's
// one call site (0x0048d7ae) is an ORDINARY call to it, not a compiler-inlined copy of its body --
// same situation sim_unit_update_soldiers.cpp's trunc_axis_delta documents, reproduced here as the
// identical instruction sequence rather than reached through mh::call or substituted with std::trunc.
//
// Forms `(double)val * ratio` ENTIRELY IN THE X87 REGISTER: FILD loads `val` as an exact 80-bit
// integer, then FMUL multiplies it by `ratio` loaded straight from the C++ double argument -- the FPU
// internally extends that 64-bit load to its own 80-bit working precision for the multiply, so the
// product left in ST0 is the (float10)val * (float10)ratio product the batch context describes, even
// though `ratio` itself was already rounded down to a 64-bit double by its own computation (see the
// caller) before this call. Truncates toward zero via the same control-word swap utils_math_trunc's
// own body performs, and stores with FISTP.
//
// WIDTH VERIFIED AT THIS SITE, NOT ASSUMED FROM THE SIBLING (per the batch context's own warning):
// opcode bytes at 0x0048d7b3 are `db 5d d0` -> 0xDB, ModRM 0x5D (mod=01,reg=011,rm=101) -> reg field 3
// -> 0xDB /3 == FISTP m32int. That is the SAME 32-bit form sim_unit_update_soldiers.cpp's
// trunc_axis_delta uses -- here the two precedents happen to agree, they do not always (that file's
// own header notes the ai_mine_yield.cpp / ai_group_muster_pick.cpp / ai_opponent_relations.cpp
// precedents use the 64-bit 0xDF/7 FISTP form instead).
//
// UNGUARDED DIVISOR, DOWNSTREAM CONSEQUENCE: if the caller's `ratio` is +-Infinity or NaN
// (cfg_units[proto].energy == 0.0 in the caller's division -- see there), FMUL propagates that
// through the product; FRNDINT raises the (masked, so non-trapping) invalid-operation exception on a
// non-finite ST0 and the masked response leaves the value unconverted; FISTP of a non-finite or
// out-of-range operand then stores the x87 "integer indefinite" pattern, 0x80000000 (INT_MIN). So a
// 0-energy cfg unit type sends llm_resource_add an amount of INT_MIN for every resource slot it walks
// -- not a trap, not a skip. Reproduced as-is; no guard is added here or in the caller (see
// uncertainties[] in the structured report).
int32_t refund_amount(int32_t val, double ratio) {
    return ::mh::fp::trunc_i32_mul(val, ratio);
}

} // namespace

namespace detail {

void unit_refund_build_cost_by_health(const sim_view &v, const unit_refund_build_cost_by_health_calls &c,
                                      int32_t player, int32_t unit_index) {
    // 0x0048d723-0x0048d73a: unit_proto_id, MOVZX word -- zero-extended, matches uint16_t -> uint32_t.
    const unit     &u     = unit_of(v, (uint32_t)player, unit_index);
    const uint32_t  proto = u.unit_proto_id;
    const cfg_unit &cu    = v.cfg_units[proto];

    // 0x0048d74d-0x0048d766: ratio = (unit.energy * 0.5) / cfg_units[proto].energy, computed in the
    // x87 register but STORED TO A 64-BIT DOUBLE LOCAL by its own FSTP ("FSTP double ptr
    // [EBP + -0x2c]", not "FSTP tbyte ptr") -- a real precision step, not a translation artifact.
    // Plain C++ arithmetic against a `double` local reproduces it exactly under this TU's
    // /arch:IA32 /fp:precise build (the assignment forces the same store-and-round-to-64-bit the FSTP
    // performs; see sim_state.h's FP LANDMINE banner). `energy` on BOTH sides is the HP-like stat, not
    // the POWER resource -- see docs/conventions.md#energy-is-not-power.
    //
    // cfg_units[proto].energy is UNGUARDED here, exactly as the original leaves it -- see
    // refund_amount()'s comment on what a 0 divisor produces downstream. Not fixed.
    const double ratio = (u.energy * ENERGY_RATIO_SCALE) / cu.energy;

    // 0x0048d769-0x0048d7ca: walk cfg_units[proto].resource[i], i = 0, 1, 2, ... . THE CONJUNCTION IS
    // EVALUATED id-FIRST: 0x0048d77f reads resource[i].id and 0x0048d788/0x0048d78c test it against 0
    // (the UNDEFINED sentinel) BEFORE 0x0048d78e/0x0048d792 tests i<CFG_RESOURCE_SLOTS(7) -- i.e. the
    // READ at index i always happens on entry to the loop body, even for i==7. The shipped cfg parser
    // only ever fills 4 of the 7 slots (addr/mh_structs.gen.h's own comment on this field: "entries
    // 4..6 read back zero, which is what stops every walk early"), so i==7 is unreachable with any
    // shipped cfg -- but if it WERE reached, this would read cfg_unit::resource_2[0].id, one field
    // past `resource`'s declared 7-entry extent (the two arrays are adjacent with no padding: resource
    // ends at offset 0x1df, exactly where resource_2 begins). Reproduced literally, not clamped to
    // CFG_RESOURCE_SLOTS ahead of the read; see uncertainties[] in the structured report.
    for (int32_t i = 0;; ++i) {
        const int32_t resource_id = (int32_t)cu.resource[i].id; // 0x0048d77f -- can read resource[7]
        if (resource_id == 0)                                   // 0x0048d788/0x0048d78c: UNDEFINED
            break;
        if (!(i < CFG_RESOURCE_SLOTS)) // 0x0048d78e/0x0048d792: bound check, evaluated SECOND
            break;

        const int32_t val    = cu.resource[i].val; // 0x0048d7a5 FILD dword
        const int32_t amount = refund_amount(val, ratio);
        c.resource_add(player, resource_id, amount); // 0x0048d7bf
    }
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void unit_refund_build_cost_by_health(int32_t player, int32_t unit_index) {
    const sim_view v = state().read;
    detail::unit_refund_build_cost_by_health(v, live_unit_refund_build_cost_by_health_calls(), player,
                                             unit_index);
}


} // namespace mh::sim
