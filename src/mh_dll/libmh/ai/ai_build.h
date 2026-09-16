//
// ai/ai_build.h -- the AI's cached build-candidate slots (RI-AI / AI0 pilot, batch A).
//
// One function, and it is in the pilot because it is the one that WRITES. Everything else in AI0's
// slice reads; this one populates 21 cached building-type ids in the AI's own store, which is
// exactly the traffic `ai_store` exists to make visible.
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {
namespace detail {

// llm_strat_ai_init_build_candidate_priorities @0x004dc7be.
//
// 21 `bldg_find_by_ai_build_and_type` lookups, each with a race-selected building-type constant,
// each result cached into a named slot of the player's record.
//
// PRESERVE, DO NOT FIX -- THE LAST FOUR WRITES OVERRUN THE PLAYER RECORD. Categories 0x22/0x23/
// 0x21/0x20 (the unit-housing types) are stored into `players[player + 1]`'s first 16 bytes, not
// `players[player]`'s. That is not a transcription error: llm_strat_ai_maintain_unit_housing and
// six other readers read them back through the identical +1 aliasing, so the behaviour is
// self-consistent, and for player 7 the write lands at 0xfb2640 -- outside the array, in .bss that
// `scan_raw_pointers` reports TOTAL-ORPHAN. `st.players` is a raw pointer precisely so this stays
// expressible. Normalising it to `players[player]` would silently repoint every housing decision.
void init_build_candidate_priorities(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                     int32_t player);

} // namespace detail

void init_build_candidate_priorities(int32_t player);

} // namespace mh::ai
