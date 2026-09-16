#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_strat_fx_anim_tick @0x004418a5. See the header banner above for the full derivation. No
// parameters, no return value -- operates entirely on `v`/`own`'s ambient cur_fx_anim.
void fx_anim_tick(const sim_view &v, sim_store &own);

} // namespace detail

// Live wrapper: the logic applied to state(). Matches the committed prototype
// (sig_llm_strat_fx_anim_tick) exactly.
void fx_anim_tick();

namespace detail {
} // namespace detail

} // namespace mh::sim
