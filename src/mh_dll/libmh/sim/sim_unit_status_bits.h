//
// sim/sim_unit_status_bits.h -- the paired set/clear over a unit's flag word (RI-SIM / SIM0 pilot).
//
//   llm_unit_status_bit_set   @0x0044b09e (0x4e)  batch A layer 2 -- order_queue_dispatch case 0x34
//   llm_unit_status_bit_clear @0x0044b0ec (0x55)  batch A layer 2 -- order_queue_dispatch case 0x35
//
// THIS PAIR IS THE PILOT'S ROSTER-WRITE CASE. `units` is the widest shared region in the subsystem
// (47 of the 307 members write it) and these two are the smallest functions that do, which makes
// them the cheapest possible demonstration that a roster write goes through `sim_store::unit_at`
// and not through an address.
//
// WHAT THEY WRITE IS `unit::ai_group_index` (+0xd8), AND THE FIELD NAME IS A MISNOMER HERE. Both
// originals treat it as a 16-bit flag word -- OR word ptr @0x0044b0dd, AND word ptr @0x0044b132 --
// while the AI's group machinery treats the same field as a group INDEX (0xffff = none). The two
// readings are not obviously compatible and the discrepancy is recorded on the Ghidra field comment
// and both plates; it is NOT resolved here and nothing in this translation depends on which is
// right. Do not "fix" the name from inside a translation.
//
// `bit_index` ARRIVES AS A BYTE IN BL and the shift is a plain `SHL EAX,CL`, which the hardware
// masks to 5 bits -- hence the `& 0x1f`. That is the CPU's masking, not a bound the original
// checks, so a bit_index of 32 sets bit 0 rather than doing nothing. Reproduced.
//
// NOT ARMED under SIM0 (see sim_bldg_alive.h); the shadow sites belong to SIM1A.
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {
namespace detail {

// llm_unit_status_bit_set @0x0044b09e. `flags |= (uint16_t)(1u << (bit_index & 31))`.
void unit_status_bit_set(sim_store &own, int32_t unit_player, int32_t unit_index, uint8_t bit_index);

// llm_unit_status_bit_clear @0x0044b0ec.
//
// THE MASK IS BUILT IN 32 BITS AND THEN TRUNCATED, AND THE ORDER MATTERS. The original computes
// `m = 1u << (bit_index & 31)` (0x0044b10e), XORs the whole DWORD with 0xffff (0x0044b118), and only
// then ANDs the low WORD into the field (0x0044b132). For bit_index >= 16 that leaves the low word
// as 0xffff, so the AND is a NO-OP -- a "clear bit 20" call clears nothing. Computing the mask as
// `(uint16_t)~(1u << bit)` instead would clear bit 4 in that case, which is a different function.
void unit_status_bit_clear(sim_store &own, int32_t unit_player, int32_t unit_index,
                           uint8_t bit_index);

} // namespace detail

// Live wrappers: the logic applied to state().own. Signatures match the originals' __watcall shape.
void unit_status_bit_set(int32_t unit_player, int32_t unit_index, uint8_t bit_index);
void unit_status_bit_clear(int32_t unit_player, int32_t unit_index, uint8_t bit_index);

} // namespace mh::sim
