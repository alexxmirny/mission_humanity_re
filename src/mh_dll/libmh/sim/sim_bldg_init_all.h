#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The one outward call. Routed through an injectable table like every other module here, so the
// offline oracle can drive the body with a recording stub and count/inspect the calls.
struct bldg_init_all_calls {
    void (*bldg_init_defaults)(uint32_t); // llm_strat_bldg_init_defaults @0x0045a068
};

const bldg_init_all_calls &live_bldg_init_all_calls();

namespace detail {

// llm_strat_bldg_init_all @0x0045a017. Reads cfg `Building[1..99].type`, calls
// `c.bldg_init_defaults(i & 0xffff)` for each defined one. Writes nothing of its own.
void bldg_init_all(const sim_view &v, const bldg_init_all_calls &c);


} // namespace detail

// Live wrapper: the logic applied to state(). Matches sig_llm_strat_bldg_init_all exactly
// (void __watcall(void)).
void bldg_init_all();

} // namespace mh::sim
