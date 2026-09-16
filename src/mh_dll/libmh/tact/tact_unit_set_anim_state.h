//
// tact/tact_unit_set_anim_state.h -- TACT1C: set a unit's anim_state, with a one-shot debug-overlay
// hook the original leaves as dead code (the CMP's flags are never read).
//
//   llm_tact_unit_set_anim_state @0x00430f03 (0x57)
//   void __watcall llm_tact_unit_set_anim_state(int building_id, byte state)
//
// SHAPE: @0x00430f20-0x00430f3e -- while progress > 0, a CMP of the CURRENT anim_state against the
// incoming `state` computes flags nothing downstream reads (the very next instruction,
// @0x00430f41, is an unconditional IMUL/store sequence with no intervening branch) -- not
// reproduced, same "dead CMP" shape as tact_unit_death_tick.h's step 2. @0x00430f41-0x00430f4e:
// unconditional store, `anim_state = state`, regardless of the guard above.
//
// PROOF: RIG. the measured write closure of llm_tact_unit_set_anim_state -> 1 function, 1 region
// (_G_LLM_TACT_UNITS, depth 0, the function's own direct write), 0 undeclared once that is listed.
// No calls at all (leaf function) -- no effect-seam exposure.
//
#pragma once
#include <cstdint>

#include "tact/tact_state.h"

namespace mh::tact {

namespace detail {

// llm_tact_unit_set_anim_state @0x00430f03.
void unit_set_anim_state(tact_store &own, int32_t building_id, uint8_t state);

} // namespace detail

void unit_set_anim_state(int32_t building_id, uint8_t state);


} // namespace mh::tact
