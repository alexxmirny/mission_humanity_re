//
// ai/ai_train_flush.h -- cancel the AI's pending unit-training entries (RI-AI / AI1B layer 2).
//
// llm_strat_ai_queue_flush_unit_train_entries_2 @0x004e2c98. A 20-byte head (stack probe, save
// EBX/ECX/EDX/ESI, ESI = player, EBX = 0) that JMPs into a shared tail at 0x004e2c67 -- the tail
// belongs to llm_strat_ai_queue_flush_unit_train_entries @0x004e2c8e, whose own 10-byte head falls
// straight through into this one. Two entry points, one body.
//
// The body walks ai_bldg_queue[0 .. ai_bldg_queue_count) and, for every entry whose status byte is
// EXACTLY ZERO -- a pending, uncommitted TRAIN entry -- stamps it 0xc0 (committed + removed) and
// gives back the two training tallies the enqueue had bumped:
//
//     --ai_train_queued_by_unit_type[entry.tick_or_unit_id]
//     --ai_train_queued_by_ai_unit[ Unit[entry.tick_or_unit_id].ai_unit ]
//
// Entries with any bit set are skipped. It writes player_data and nothing else, reads the cfg Unit
// table, and makes no outward call.
//
// THE `status == 1` ARM IS DEAD, PROVABLY. The body tests the same byte twice, and the first test
// is `TEST byte ptr [..],0x1 / JNZ` @0x004e2bf4 -- so anything reaching the second test has bit 0
// CLEAR, and `CMP byte ptr [..],0x1 / JNZ` @0x004e2bfd can never match. The `OR .. ,0xc0`
// @0x004e2c06 it guards is unreachable code. Both tests address the identical byte: EAX is
// 166140*player at both sites (0x004e2c67-0x004e2c7b for the first, recomputed at
// 0x004e2c0d-0x004e2c21 for the second) and both add 18*i. It is documented here rather than
// emitted below, because emitting a branch that provably cannot be taken is not fidelity.
//
// The subsequent `CMP byte ptr [..],0x0 / JNZ` @0x004e2c28 therefore does all the filtering, which
// is why the predicate below is `status == 0` and not `(status & 1) == 0`.
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

// What the flush stamps: committed | removed, in one OR (0x004e2c31). Same value
// ai_queue_reconcile.h calls QUEUE_STATUS_SETTLED; spelled locally so this module does not depend
// on that one for a constant it derives from its own listing.
inline constexpr uint8_t TRAIN_FLUSH_STAMP = 0xc0;

namespace detail {

struct train_flush_report {
    int32_t scanned = 0; // ai_bldg_queue_count as the loop head first read it
    int32_t flushed = 0; // entries that matched status == 0 and were stamped
};

// The logic over an EXPLICIT state, so `net_selftest.exe aitest` can drive it over heap buffers
// with no game. Takes no call set: the body makes no outward call.
train_flush_report queue_flush_unit_train_entries_2(const ai_view &v, const ai_store &own,
                                                    int32_t player);

} // namespace detail

void queue_flush_unit_train_entries_2(int32_t player);

// THE SIBLING ENTRY, llm_strat_ai_queue_flush_unit_train_entries @0x004e2c8e (AI1B layer 5,
// 2026-08-05). Ten bytes -- `PUSH 4 / CALL assert_stack_capacity` -- and then a FALL-THROUGH, with
// no JMP and no CALL, into 0x004e2c98 above. So it is the SAME FUNCTION reached through a second
// door, and the body below does exactly that: it calls detail::queue_flush_unit_train_entries_2.
// Nothing is duplicated, and the stack probe is inert (reimpl-loop settles that once for all).
//
// Ghidra attributes the SHARED TAIL (0x004e2bef-0x004e2c8d) to THIS entry's body rather than to
// 0x004e2c98's, which is why the exported .asm for 0x004e2c8e looks like the whole loop and the one
// for 0x004e2c98 looks like a 20-byte stub -- and why the `_2` shadow site has to declare
// player_data by hand. Read both listings together or neither makes sense.
//
// THE ARMING HAZARD LIVES HERE. Because the edge into 0x004e2c98 is a fall-through, no
// reference-derived call graph holds it: check_arming_set.py returned a FALSE SAFE on this pair
// before tools/data/fallthrough_entries.json existed. The two sites must never share a fragment.
void queue_flush_unit_train_entries(int32_t player);

} // namespace mh::ai
