//
// sim/sim_rng_next.cpp -- see sim_rng_next.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_rng_next_004b4d01.asm), not from the Ghidra .c draft.
//
#include <intrin.h> // _ReturnAddress -- the draw-sequence trace's caller tag

#include "sim/rng_trace.h"
#include "sim/sim_rng_next.h"

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace detail {

int32_t rng_next(sim_store &own, int32_t channel, int32_t lo, int32_t hi) {
    // 0x004b4d06-0x004b4d1e: read-modify-write the per-channel PRNG state through ONE reference (read
    // the old value, then write the new one back through the SAME accessor) -- not read-only-then-a-
    // separate-write. `channel` is used exactly as the asm's `EBX*0x4 + 0x603ecc` addressing does: a
    // raw (possibly out-of-range, per the original -- not bounds-checked here either) index, cast to
    // unsigned to match the mod-2^32 address arithmetic the x86 addressing mode performs.
    uint32_t &state = own.rng_state_at(static_cast<uint32_t>(channel));

    // 0x004b4d09-0x004b4d19: s_new = ROR16(state + 0x9248, 3).
    //
    // The asm's ADD EAX,0x9248 is a 32-bit add, but the very next op, ROR AX,0x3, only ever touches
    // the low 16 bits of EAX (AX) -- whatever the ADD deposited above bit 15 is irrelevant to AX, and
    // the trailing AND EAX,0xffff (0x004b4d19) discards it from EAX too before the store. So the
    // whole computation lives in the 16-bit domain: reproduced here with an explicit uint16_t `added`
    // (truncating exactly where the asm's AX register truncates) rather than the Ghidra .c draft's
    // `(short)...+0x9248U` cast chain, which computes the add in a 32-bit int before narrowing and
    // reads as ambiguous about exactly where the truncation happens. ROR by 3 on a 16-bit value is
    // `(v >> 3) | (v << 13)`, masked back to 16 bits (the `<< 13` term already discards its own
    // overflow into bit 16+ when stored into a uint16_t).
    const uint16_t added   = static_cast<uint16_t>(state + 0x9248u);
    const uint16_t rotated = static_cast<uint16_t>((added >> 3) | (added << 13));

    // 0x004b4d1e: store back, zero-extended into the full 32-bit state slot (matches the original's
    // dword store of a value that only ever occupies the low 16 bits -- MOV dword ptr [...],EAX after
    // the AND, not a 16-bit store).
    state = rotated;

    // C-prime: the draw-sequence trace. Observation only, and disarmed unless a host armed a
    // step window -- see sim/rng_trace.h for why the sequence, not the return address, is what
    // gets diffed across arms.
    mh::sim::rng_trace_record(static_cast<int32_t>(channel), rotated, _ReturnAddress());

    // 0x004b4d25-0x004b4d37: EDX = hi - lo (plain 32-bit subtraction, NOT sign-checked -- if a caller
    // passes hi < lo, this wraps to a huge unsigned span exactly as the original's MUL/DIV pair does,
    // both unsigned x86 instructions; not "fixed" here, per translator-brief rule 10). new_state
    // (0..0xffff) * span, full 64-bit unsigned product (the asm's EDX:EAX after MUL), divided by
    // 0xffff (65535, NOT 0x10000), quotient truncated to int, plus lo.
    const uint32_t span    = static_cast<uint32_t>(hi - lo);
    const uint64_t product = static_cast<uint64_t>(rotated) * static_cast<uint64_t>(span);
    return static_cast<int32_t>(product / 0xffffu) + lo;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

int32_t rng_next(int32_t channel, int32_t lo, int32_t hi) {
    sim_state st = state();
    return detail::rng_next(st.own, channel, lo, hi);
}


} // namespace mh::sim
