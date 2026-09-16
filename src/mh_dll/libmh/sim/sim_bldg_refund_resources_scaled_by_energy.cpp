//
// sim/sim_bldg_refund_resources_scaled_by_energy.cpp -- see
// sim_bldg_refund_resources_scaled_by_energy.h. Translated from the DISASSEMBLY
// (tmp/decomp_sim/llm_strat_bldg_refund_resources_scaled_by_energy_00479493.asm). Ghidra's own .c
// draft was re-walked field-by-field against the raw CMP/JZ/JL targets and every read/offset it names
// checks out (confirmed independently below against addr/mh_structs.gen.h's static_assert'd offsets),
// so it is cited only as corroboration, not as the source of truth.
//
#include "sim/sim_bldg_refund_resources_scaled_by_energy.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"
#include "fp/x87.h" // CRT-X87: the shared x87 truncation helpers

namespace mh::sim {

const bldg_refund_resources_scaled_by_energy_calls &live_bldg_refund_resources_scaled_by_energy_calls() {
    static const bldg_refund_resources_scaled_by_energy_calls c = {
        MH_LIBMH_BIND(llm_resource_add),
    };
    return c;
}

namespace {

// utils_math_trunc @0x004d0596 (`MH_UNAVAILABLE__parameter_storage_not_marshallable` in
// mh_calls.gen.h -- ST0 in, ST0 out, x87-register-only, no stack-passable signature). This function's
// one call site (0x0047953b) is an ORDINARY call to it, not a compiler-inlined copy of its body --
// same situation sim_unit_refund.cpp's refund_amount() documents, reproduced here as the identical
// instruction sequence rather than reached through mh::call or substituted with std::trunc.
//
// Forms `(double)val * ratio` ENTIRELY IN THE X87 REGISTER: FILD loads `val` as an exact 80-bit
// integer, then FMUL multiplies it by `ratio` loaded straight from the C++ double argument -- the FPU
// internally extends that 64-bit load to its own 80-bit working precision for the multiply, even
// though `ratio` itself was already rounded down to a 64-bit double by its own computation (see the
// caller) before this call. Truncates toward zero via the same control-word swap utils_math_trunc's
// own body performs, and stores with FISTP.
//
// WIDTH VERIFIED AT THIS SITE, NOT ASSUMED FROM THE SIBLING: opcode bytes at 0x00479540 are
// `db 5d d0` -> 0xDB, ModRM 0x5D (mod=01,reg=011,rm=101) -> reg field 3 -> 0xDB /3 == FISTP m32int,
// the SAME 32-bit form sim_unit_refund.cpp's refund_amount() uses.
//
// UNGUARDED DIVISOR, DOWNSTREAM CONSEQUENCE: if the caller's `ratio` is +-Infinity or NaN
// (cfg_buildings[bid].energy == 0.0 in the caller's division -- see there), FMUL propagates that
// through the product; FRNDINT raises the (masked, so non-trapping) invalid-operation exception on a
// non-finite ST0 and the masked response leaves the value unconverted; FISTP of a non-finite or
// out-of-range operand then stores the x87 "integer indefinite" pattern, 0x80000000 (INT_MIN). So a
// 0-energy cfg building type sends llm_resource_add an amount of INT_MIN for every resource slot it
// walks -- not a trap, not a skip. Reproduced as-is; no guard is added here or in the caller (see
// uncertainties[] in the structured report).
int32_t refund_amount(int32_t val, double ratio) {
    return ::mh::fp::trunc_i32_mul(val, ratio);
}

} // namespace

namespace detail {

void bldg_refund_resources_scaled_by_energy(const sim_view                                     &v,
                                            const bldg_refund_resources_scaled_by_energy_calls &c,
                                            int32_t player, int32_t index) {
    // 0x004794aa-0x004794c7: building_id, MOVZX word -- zero-extended, matches uint16_t -> uint32_t.
    const building     &b   = building_of(v, static_cast<uint32_t>(player), index);
    const uint32_t      bid = b.building_id;
    const cfg_building &cb  = v.cfg_buildings[bid];

    // 0x004794ca-0x004794f3: ratio = (buildings[player][index].energy * REFUND_ENERGY_FACTOR) /
    // cfg_buildings[bid].energy, computed in the x87 register but STORED TO A 64-BIT DOUBLE LOCAL by
    // its own FSTP ("FSTP double ptr [EBP + -0x2c]", not "FSTP tbyte ptr") -- a real precision step,
    // not a translation artifact. Plain C++ arithmetic against a `double` local reproduces it exactly
    // under this TU's /arch:IA32 /fp:precise build (the assignment forces the same store-and-round-to-
    // 64-bit the FSTP performs; see sim_state.h's FP LANDMINE banner). Computed EXACTLY ONCE before the
    // loop, not per-iteration (matches the asm's own instruction order). `energy` on BOTH sides is the
    // HP/charge-like stat, not the POWER resource -- see docs/conventions.md#energy-is-not-power.
    //
    // cfg_buildings[bid].energy is UNGUARDED here, exactly as the original leaves it -- see
    // refund_amount()'s comment on what a 0 divisor produces downstream. Not fixed.
    const double ratio = (b.energy * (*v.refund_energy_factor)) / cb.energy;

    // 0x004794f6-0x00479557: walk cfg_buildings[bid].resource[i], i = 0, 1, 2, ... . THE CONJUNCTION
    // IS EVALUATED id-FIRST: 0x0047950c reads resource[i].id and 0x00479515/0x00479519 test it against
    // 0 (the UNDEFINED sentinel) BEFORE 0x0047951b/0x0047951f tests i<CFG_RESOURCE_SLOTS(7) -- i.e. the
    // READ at index i always happens on entry to the loop body, even for a hypothetical i==7 (one field
    // past `resource`'s declared 7-entry extent). The shipped cfg parser only ever fills 4 of the 7
    // slots (entries 4..6 read back zero, which is what stops every walk early in practice), so i==7 is
    // unreachable with any shipped cfg -- reproduced literally, not clamped ahead of the read; see
    // uncertainties[] in the structured report.
    for (int32_t i = 0;; ++i) {
        const int32_t resource_id = static_cast<int32_t>(cb.resource[i].id); // 0x0047950c -- can read
                                                                             // resource[7]
        if (resource_id == 0)                                                // 0x00479515/0x00479519: UNDEFINED
            break;
        if (!(i < CFG_RESOURCE_SLOTS)) // 0x0047951b/0x0047951f: bound check, evaluated SECOND
            break;

        const int32_t val    = cb.resource[i].val; // 0x00479532 FILD dword
        const int32_t amount = refund_amount(val, ratio);
        c.resource_add(player, resource_id, amount); // 0x0047954c
    }
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void bldg_refund_resources_scaled_by_energy(int32_t player, int32_t index) {
    const sim_view v = state().read;
    detail::bldg_refund_resources_scaled_by_energy(v, live_bldg_refund_resources_scaled_by_energy_calls(),
                                                   player, index);
}


} // namespace mh::sim
