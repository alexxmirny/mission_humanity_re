#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_strat_bldg_anim_state_port @0x004786f5. See the header derivation.
void bldg_anim_state_port(const sim_view &v, sim_store &own);

// llm_strat_bldg_anim_state_port_h @0x00478bfe. See the header derivation.
void bldg_anim_state_port_h(const sim_view &v, sim_store &own);

} // namespace detail

// Live wrappers -- match each original's committed void(void) prototype exactly.
void bldg_anim_state_port();
void bldg_anim_state_port_h();

namespace detail {
} // namespace detail

} // namespace mh::sim
