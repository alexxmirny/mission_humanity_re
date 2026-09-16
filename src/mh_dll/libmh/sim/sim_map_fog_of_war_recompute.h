#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_map_fog_of_war_recompute @0x00428b11. See the header banner above for the full per-pass
// derivation and the `flags.f` byte-decomposition note.
void fog_of_war_recompute(const sim_view &v, sim_store &own);

} // namespace detail

// Live wrapper: the logic applied to state(). Matches the committed prototype
// (sig_llm_map_fog_of_war_recompute) exactly -- no parameters, no return.
void fog_of_war_recompute();

namespace detail {
} // namespace detail

} // namespace mh::sim
