#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_strat_bldg_anim_tick @0x00476447. See the header derivation.
void bldg_anim_tick(const sim_view &v, sim_store &own);

// llm_strat_bldg_anim_state_helipad_a @0x00477231. Same mechanism as bldg_anim_tick, with the two
// documented differences (guard, zero-duration skip).
void bldg_anim_state_helipad_a(const sim_view &v, sim_store &own);

// llm_strat_bldg_anim_state_airfield_h @0x00477bed. Same mechanism, the THIRD guard/skip combination
// (see header).
void bldg_anim_state_airfield_h(const sim_view &v, sim_store &own);

// llm_strat_bldg_anim_state_turret @0x00476605. Genuine no-op -- see the header derivation.
void bldg_anim_state_turret();

} // namespace detail

// Live wrappers -- match each original's committed void(void) prototype exactly.
void bldg_anim_tick();
void bldg_anim_state_helipad_a();
void bldg_anim_state_airfield_h();
void bldg_anim_state_turret();

namespace detail {
} // namespace detail

} // namespace mh::sim
