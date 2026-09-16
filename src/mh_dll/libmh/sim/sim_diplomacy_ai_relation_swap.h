#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_diplomacy_ai_relation_swap @0x004dc0dd. See the header banner above for the full derivation.
// Read-modify-write over player_data[player].ai_player_relation[toward_player]; returns the OLD
// value, matching the original's EAX-return convention exactly.
int32_t diplomacy_ai_relation_swap(sim_store &own, int32_t player, int32_t toward_player,
                                   int32_t new_relation);

} // namespace detail

// Live wrapper: the logic applied to state().own. Matches the committed prototype
// (sig_llm_diplomacy_ai_relation_swap) exactly.
int32_t diplomacy_ai_relation_swap(int32_t player, int32_t toward_player, int32_t new_relation);

namespace detail {
} // namespace detail

} // namespace mh::sim
