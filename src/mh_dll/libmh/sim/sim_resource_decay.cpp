#include "sim/sim_resource_decay.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"
#include "fp/x87.h" // CRT-X87: the shared x87 truncation helpers

namespace mh::sim {

const resource_decay_calls &live_resource_decay_calls() {
    static const resource_decay_calls c = {
        MH_LIBMH_BIND(game_SpendResource),
    };
    return c;
}

namespace {

// utils_math_trunc @0x004d0596 (`MH_UNAVAILABLE__parameter_storage_not_marshallable` in
// mh_calls.gen.h -- ST0 in, ST0 out, x87-register-only, no stack-passable signature). This function's
// one call site (0x00491651) is an ORDINARY call to it, not a compiler-inlined copy of its body -- same
// situation sim_unit_refund.cpp's refund_amount() documents, reproduced here as the identical
// instruction sequence rather than reached through mh::call or substituted with std::trunc.
//
// Forms `(double)diff * rate` ENTIRELY IN THE X87 REGISTER: FILD loads `diff` as an exact 80-bit
// integer, then FMUL multiplies it by `rate` loaded straight from the C++ double argument -- the FPU
// internally extends that 64-bit load to its own 80-bit working precision for the multiply, matching
// the batch context's `(float10)(diff) * (float10)_DAT_005014f4` reading. Truncates toward zero via the
// same control-word swap utils_math_trunc's own body performs (FSTCW / RC=11,PC=11 / FLDCW / FRNDINT /
// FLDCW restore), then stores with a 32-bit FISTP.
//
// WIDTH VERIFIED AT THIS SITE: opcode bytes at 0x00491656 are `db 5d e8` -> 0xDB, ModRM 0x5D
// (mod=01,reg=011,rm=101) -> reg field 3 -> 0xDB /3 == FISTP m32int, the same 32-bit form
// sim_unit_refund.cpp's refund_amount() uses.
int32_t decay_amount(int32_t diff, double rate) {
    return ::mh::fp::trunc_i32_mul(diff, rate);
}

} // namespace

namespace detail {

bool decay_excess_resources(const sim_view &v, const resource_decay_calls &c, int32_t player,
                            int32_t res) {
    // 0x00491621-0x00491645: diff = player_resources[player][res] - storage_stats[player].cap_prev[res]
    // (two independent stride-scaled address computations, both int32 loads, plain int32 subtract --
    // no float involved yet).
    const int32_t stock = player_resource_of(v, player, res);
    const int32_t cap   = v.storage_stats[player].cap_prev[res];
    const int32_t diff  = stock - cap;

    // 0x00491648-0x00491656: amount = trunc((double)diff * resource_decay_rate). *v.resource_decay_rate
    // is the boot constant 0.1 (see sim_state.h's SIM1F banner).
    const int32_t amount = decay_amount(diff, *v.resource_decay_rate);

    // 0x00491659-0x0049167d: only a STRICTLY POSITIVE amount spends and returns true; zero or negative
    // (stock at or below capacity, or a decay fraction that truncates to 0) does neither.
    if (amount > 0) {
        c.spend_resource(player, res, amount); // 0x00491668
        return true;
    }
    return false;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

bool decay_excess_resources(int32_t player, int32_t res) {
    const sim_view v = state().read;
    return detail::decay_excess_resources(v, live_resource_decay_calls(), player, res);
}


// ---- the rebind ABI shim -------------------------------------------------------------------------
//
// The committed export prototype returns uint8_t; the public wrapper returns bool. Same byte, but
// the binder's static_assert compares types exactly. It lived beside the differential oracle until
// F2D retired it; this is signature adaptation for the binder, not oracle machinery.
namespace rebind_arm {

uint8_t decay_excess_resources(int32_t player, int32_t res) {
    return (uint8_t)mh::sim::decay_excess_resources(player, res);
}

} // namespace rebind_arm

} // namespace mh::sim
