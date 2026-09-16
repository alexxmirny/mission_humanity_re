//
// sim/libtrans/sim_lt_rng_raw_step.cpp -- see sim_lt_rng_raw_step.h. Translated from the DISASSEMBLY
// (tmp/decomp_lib_trans/llm_rand_below_ai_004d4063.asm, llm_rand_state_advance_004b4d3b.asm,
// llm_rand_prng_tick_slot_004b4cc0.asm), not from Ghidra's C drafts -- the drafts read correctly for
// the VALUE comparisons, but their `(short)`/`(ushort)` cast chains read as ambiguous 32-vs-16-bit
// domain juggling where the asm is unambiguous (same reasoning sim_rng_next.cpp documents for the
// identical recurrence).
//
#include <intrin.h> // _ReturnAddress -- the draw-sequence trace's caller tag

#include "sim/rng_trace.h"
#include "sim/libtrans/sim_lt_rng_raw_step.h"

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace detail {

uint32_t rng_tick_slot(sim_store &own, int32_t slot) {
    // 0x004b4cc7-0x004b4cdc (spec: llm_rand_prng_tick_slot @0x004b4cc0, ADOPTED -- see the header
    // banner): read-modify-write the channel's 16-bit state through ONE reference (read the old
    // value, then write the new one back through the SAME accessor -- not read-only-then-a-separate-
    // write). `slot` is used exactly as the asm's `EBX*0x4 + 0x603ecc` addressing does: a raw (not
    // bounds-checked in the original, none added here) index, cast to unsigned to match the
    // mod-2^32 address arithmetic the x86 addressing mode performs.
    uint32_t &state = own.rng_state_at(static_cast<uint32_t>(slot));

    // 0x004b4cce-0x004b4cd7: s_new = ROR16(state + 0x9248, 3). The ADD is a 32-bit add, but the very
    // next op, ROR AX,0x3, only ever touches the low 16 bits of EAX, and the trailing AND EAX,0xffff
    // (0x004b4cd7) discards anything above bit 15 before the store -- the whole computation lives in
    // the 16-bit domain (identical reasoning to sim_rng_next.cpp's detail::rng_next, same
    // recurrence). ROR by 3 on a 16-bit value is `(v >> 3) | (v << 13)`, masked back to 16 bits (the
    // `<< 13` term already discards its own overflow into bit 16+ when stored into a uint16_t).
    const uint16_t added   = static_cast<uint16_t>(state + 0x9248u);
    const uint16_t rotated = static_cast<uint16_t>((added >> 3) | (added << 13));

    // 0x004b4cdc: store back, zero-extended into the full 32-bit slot (matches the original's dword
    // store of a value that only ever occupies the low 16 bits).
    state = rotated;

    // C-prime: the draw-sequence trace. Observation only, and disarmed unless a host armed a
    // step window -- see sim/rng_trace.h for why the sequence, not the return address, is what
    // gets diffed across arms.
    mh::sim::rng_trace_record(static_cast<int32_t>(slot), rotated, _ReturnAddress());
    return rotated; // 0x004b4cc0's return value: the NEW state, zero-extended.
}

int32_t rand_below_ai(sim_store &own, uint32_t range) {
    // 0x004d4070/0x004d4072: PUSH 0x2 then CALL the tick-slot mixer -- the channel is HARDCODED to 2
    // (the AI channel), never a parameter of this function.
    const uint32_t new_state = rng_tick_slot(own, 2);

    // 0x004d407a/0x004d407d: IMUL EAX,EDX (new_state * range, the 2-operand form -- its low 32 result
    // bits are identical whether the multiply is read as signed or unsigned) then SHR EAX,0x10 (a
    // LOGICAL shift). So this is `(state' * range) >> 16`, i.e. dividing by 0x10000 -- NOT
    // detail::rng_next's `/0xffff`. Computing the product and the shift in uint32_t reproduces both
    // instructions exactly; the uint32_t result is then reinterpreted as the original's signed `int`
    // return (EAX's bit pattern, matching the asm's plain RET with no sign-extension).
    return static_cast<int32_t>((new_state * range) >> 16);
}

double rand_state_advance(const sim_view &v, sim_store &own, int32_t rng_index) {
    // 0x004b4d40/0x004b4d43-0x004b4d58: the SAME tick-slot recurrence as rng_tick_slot above, run on
    // the CALLER-supplied index (not hardcoded, unlike rand_below_ai above). The original does not
    // CALL 0x004b4cc0 here (the asm has zero CALL instructions -- it is its own inlined copy of the
    // identical ADD/ROR/AND/store sequence), but the state update is byte-for-byte the same
    // recurrence, so routing it through the shared rng_tick_slot reproduces it exactly without a
    // second, drifting copy of the arithmetic.
    const uint32_t new_state = rng_tick_slot(own, rng_index);

    // 0x004b4d5f: FILD of the just-stored slot -- a SIGNED 32-bit load of the NEW value (0..0xffff,
    // always positive, but converted here as the FILD does: signed, not unsigned).
    // 0x004b4d66: FDIV by v.rng_norm_divisor (_G_LLM_STRAT_RNG_NORM_DIVISOR @0x00603f0c, 65535.0 --
    // bound on the view per the LT1 bindings commit, not transcribed as a literal). Result in ST0.
    return static_cast<double>(static_cast<int32_t>(new_state)) / *v.rng_norm_divisor;
}

} // namespace detail

// ---- the public wrappers ------------------------------------------------------------------------

int32_t rand_below_ai(uint32_t range) {
    sim_state st = state();
    return detail::rand_below_ai(st.own, range);
}

double rand_state_advance(int32_t rng_index) {
    sim_state st = state();
    return detail::rand_state_advance(st.read, st.own, rng_index);
}


} // namespace mh::sim
