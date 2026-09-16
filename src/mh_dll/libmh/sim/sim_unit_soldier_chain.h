//
// sim/sim_unit_soldier_chain.h -- three small operations on a unit's mounted-soldier linked list
// (RI-SIM / SIM1A): drop the tail, unlink a named soldier, and stamp a heading/sprite
// frame across the whole chain. See the .cpp for the full per-instruction derivation of each.
//
// SHARED SHAPE. All three walk the SAME chain sim_unit_update_soldiers.cpp / sim_unit_passive_engage.cpp
// already read: head = units[player][unit_index].unit_above (a byte[2], reassembled little-endian, same
// pattern those two siblings use), links via _G_LLM_STRAT_SOLDIERS[player][*].next_soldier, terminated
// by next_soldier==0. Record [0] is the per-player roster's dual-purpose sentinel: its `owner_unit`
// field (int16_t) doubles as the side's live-soldier COUNT (remove_last/unlink both decrement it), a
// fact this header does not re-derive -- see sim_state.h's `soldiers` member comment and
// sim_unit_update_soldiers.cpp's identical note.
//
// No callees: the only CALL in any of the three bodies is the inert utils_assert_stack_capacity
// prologue (translator-brief rule 6 -- omitted). No floats anywhere in any of the three (checked the
// .asm: zero x87 opcodes in all three bodies).
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_strat_unit_soldier_remove_last @0x00489595 (0xe6 bytes), void __watcall(uint player, int
// unit_index).
//
// Walks the chain to its TAIL via a do-while carrying TWO lagging cursors (`prev2`/`prev1` below, one
// and two steps behind the walker) and drops it: `prev1`'s record (the tail) is blanked
// (next_soldier=0, owner_unit=0), `prev2`'s record has its next_soldier zeroed (detaching the tail from
// whatever pointed at it), and the per-player count at record 0 is decremented.
//
// DEGENERATE ENDPOINTS (see the .cpp for the full derivation against the .asm): for a chain of length 0
// (unit_above==0) both cursors end at record 0, so this decrements record 0's owner_unit to -1 (0xffff
// as the stored int16_t) -- a genuine original bug (calling this on an empty chain corrupts the count),
// transcribed as written. For a chain of length 1, `prev2` ends at record 0 and `prev1` at the sole
// soldier: the "detach" write lands on record 0's next_soldier (a no-op on the real chain) rather than
// on units[player][unit_index].unit_above, which this function NEVER writes in any case -- so after
// removing a unit's only soldier, unit_above is left stale, still pointing at the now-blanked record.
// Both are preserved, not repaired.
void unit_soldier_remove_last(const sim_view &v, sim_store &own, uint32_t player, int32_t unit_index);

// llm_strat_unit_soldier_unlink @0x0048967b (0x14c bytes), void __mh_watcall_ebx_volatile(ushort
// player, int unit_idx, uint soldier_idx).
//
// Removes the NAMED `soldier_idx` from unit_idx's chain -- a DIFFERENT operation from remove_last above
// (task hazard: the two decompiles read alike but diverge exactly where it matters). HEAD CASE
// (unit_above == soldier_idx): rewrites units[player][unit_idx].unit_above to the removed head's
// next_soldier -- the ONE write in this whole slice that touches the unit record itself, not just the
// soldier roster. ELSE: a scan for the predecessor with NO bound and NO null check (0x00489710 loop) --
// if soldier_idx is not actually linked into this chain, it walks off record 0 and keeps going,
// transcribed exactly, not guarded. Either way, the removed record is then blanked and the per-player
// count at record 0 decremented, same tail as remove_last.
void unit_soldier_unlink(const sim_view &v, sim_store &own, uint16_t player, int32_t unit_idx,
                         uint32_t soldier_idx);

// llm_strat_unit_soldiers_set_heading @0x00489ab6 (0x8a bytes), void __watcall(ushort player, int
// unit_index, byte sprite_frame).
//
// Stamps `sprite_frame` onto every soldier in unit_index's chain. UNCONDITIONAL do-while (0x00489af9):
// runs at least once even when unit_above==0 (empty chain), which means it writes
// _G_LLM_STRAT_SOLDIERS[player][0].sprite_frame -- a real write to the per-player COUNT sentinel's
// record, on a field distinct from the count itself (owner_unit). Confirmed against the .asm (see the
// .cpp) and reproduced, not guarded away.
void unit_soldiers_set_heading(const sim_view &v, sim_store &own, uint16_t player, int32_t unit_index,
                               uint8_t sprite_frame);

} // namespace detail

// Live wrappers: the logic applied to state(). Match the originals' committed __watcall /
// __mh_watcall_ebx_volatile shapes (sig_llm_strat_unit_soldier_remove_last /
// sig_llm_strat_unit_soldier_unlink / sig_llm_strat_unit_soldiers_set_heading in mh_export.gen.h).
void unit_soldier_remove_last(uint32_t player, int32_t unit_index);
void unit_soldier_unlink(uint16_t player, int32_t unit_idx, uint32_t soldier_idx);
void unit_soldiers_set_heading(uint16_t player, int32_t unit_index, uint8_t sprite_frame);

namespace detail {
} // namespace detail

} // namespace mh::sim
