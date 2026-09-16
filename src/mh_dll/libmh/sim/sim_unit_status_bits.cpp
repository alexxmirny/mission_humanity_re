//
// sim/sim_unit_status_bits.cpp -- see sim_unit_status_bits.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_unit_status_bit_set_0044b09e.asm, llm_unit_status_bit_clear_0044b0ec.asm), not
// from Ghidra's C drafts.
//
#include "sim/sim_unit_status_bits.h"

namespace mh::sim {
namespace detail {

// THE TRUNCATION IS AN EXPLICIT TEST IN BOTH FUNCTIONS BELOW, NOT A CAST, AND THAT IS NOT STYLE.
// The obvious spelling -- `(uint16_t)(1u << (bit_index & 31u))` -- IS MISCOMPILED BY MSVC /O2 on
// x86: it narrows the shift to 16 bits and masks the count to FOUR bits, so a bit_index of 16 comes
// back as 1 instead of 0. Measured 2026-08-08 and reduced to a 20-line standalone repro
// (tools/oneoff/2026-08-08-msvc-shift-truncation-repro.cpp): counts 0..15 and 32 are correct, 16..31
// are wrong, and the hardware would not do this either (`SHL r16, CL` masks CL to FIVE bits).
//
// It is inlining-context dependent, which is what makes it worth a comment rather than a fix and a
// shrug: the same source compiled correctly inside net_selftest's plain build and wrongly inside its
// ASan build, so this presented as "ASan disagrees with Release" for half an hour before the repro
// showed both were the same bug seen at different inlining depths. `simtest` is what caught it --
// there is no rig scenario that passes a bit index above 15.
inline uint16_t status_bit_mask(uint8_t bit_index) {
    // MOV EAX,1 / SHL EAX,CL @0x0044b0c0 -- the hardware masks CL to 5 bits, so the mask is a full
    // 32-bit value and only its low half reaches the WORD store. Bits 16..31 therefore land nowhere.
    const uint32_t shift = bit_index & 31u;
    return (shift < 16u) ? (uint16_t)(1u << shift) : (uint16_t)0u;
}

void unit_status_bit_set(sim_store &own, int32_t unit_player, int32_t unit_index,
                         uint8_t bit_index) {
    const uint16_t mask = status_bit_mask(bit_index);
    unit          &u    = own.unit_at((uint32_t)unit_player, unit_index);
    // OR word ptr [.. + 0xdd8d20],AX @0x0044b0dd -- a WORD store, so only the low 16 bits land.
    u.ai_group_index = (uint16_t)(u.ai_group_index | mask);
}

void unit_status_bit_clear(sim_store &own, int32_t unit_player, int32_t unit_index,
                           uint8_t bit_index) {
    // The XOR is on the full DWORD (0x0044b118) and the AND is on the low WORD (0x0044b132), which
    // is what makes bit_index >= 16 a no-op rather than a clear of bit_index - 16. So the low half
    // of `1 << shift` is complemented, and for shift >= 16 that half is 0 and the complement is
    // 0xffff -- the same explicit form as the set path, and for the same compiler reason.
    const uint16_t mask = (uint16_t)(status_bit_mask(bit_index) ^ 0xffffu);
    unit          &u    = own.unit_at((uint32_t)unit_player, unit_index);
    u.ai_group_index    = (uint16_t)(u.ai_group_index & mask);
}

} // namespace detail

void unit_status_bit_set(int32_t unit_player, int32_t unit_index, uint8_t bit_index) {
    sim_state st = state();
    detail::unit_status_bit_set(st.own, unit_player, unit_index, bit_index);
}

void unit_status_bit_clear(int32_t unit_player, int32_t unit_index, uint8_t bit_index) {
    sim_state st = state();
    detail::unit_status_bit_clear(st.own, unit_player, unit_index, bit_index);
}

} // namespace mh::sim
