//
// ai/ai_queue_remove.h -- remove one entry from the AI build-queue by slot index (RI-AI / AI1B,
// layer 4).
//
// llm_strat_ai_queue_remove_at @0x004e5da2, 98 bytes. Sole caller
// llm_strat_ai_scan_bldg_repair_upgrade, removing a stale repair/upgrade entry before re-queueing.
//
// Sibling of llm_strat_ai_queue_rotate_newest_to_front (ai_queue_rotate.cpp/.h): same
// player_data::ai_bldg_queue[64] array, same struct-assignment-not-byte-copy discipline for the
// 0x12-byte entry. This one shifts every entry ABOVE slot_index down by one and decrements the
// count -- i.e. a left-compact remove, not the rotate's right-rotate:
//
//     for (i = slot_index; i < count - 1; ++i) q[i] = q[i + 1];  --count;
//
// UNLIKE the rotate, there is no separate signed/unsigned early-out pair here: the ONE comparison
// (`CMP ECX,EDX / JC` @0x004e5df5-0x004e5df7, EDX = count - 1, freshly reloaded and decremented
// every pass) is unsigned throughout, and the original has no guard against a corrupt/negative
// count or an out-of-range slot_index -- reproduced as-is, not hardened.
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

namespace detail {

// Takes the STORE, not the view: the count it reads and the entries it moves are the same object.
void queue_remove_at(const ai_store &own, int32_t player, uint32_t slot_index);

} // namespace detail

void queue_remove_at(int32_t player, uint32_t slot_index);

} // namespace mh::ai
