#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_fx_anim_chain_find_tail @0x0045583c. See the header banner above for the full derivation.
// Returns the tail frame's index (chain end, next==0) or start_frame itself (cycle guard).
int32_t fx_anim_chain_find_tail(const sim_view &v, int32_t start_frame);

} // namespace detail

// Live wrapper: the logic applied to state().read. Matches the committed prototype
// (sig_llm_fx_anim_chain_find_tail) exactly.
int32_t fx_anim_chain_find_tail(int32_t start_frame);

namespace detail {
} // namespace detail

} // namespace mh::sim
