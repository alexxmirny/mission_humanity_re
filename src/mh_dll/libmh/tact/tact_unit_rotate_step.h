//
// tact/tact_unit_rotate_step.h -- TACT1B: the single rotation step llm_tact_unit_rotate_tick
// dispatches, until facing_dir reaches face_cmd_target_dir.
//
//   llm_tact_unit_rotate_step @0x00430df1 (0x112)
//
#pragma once
#include <cstdint>

#include "tact/tact_state.h"

namespace mh::tact {
namespace detail {

// llm_tact_unit_rotate_step @0x00430df1.
//
// 1. @0x00430e15-0x00430e1c: if `progress` (the animation-progress byte a prior step left mid-turn)
//    is nonzero, do nothing at all -- a turn already in flight is not re-stepped.
// 2. @0x00430e22-0x00430e30: snapshot `type` (the character-class index) for the rotate-speed add
//    at the end.
// 3. @0x00430e33-0x00430e3b: llm_tact_unit_vision_remove(unit_idx) -- drop the FOV stencil before
//    the facing changes.
// 4. @0x00430e3b-0x00430e70: `delta = target_dir - facing_dir`, then two range folds preserved
//    LITERALLY (not re-derived as "shortest path" -- the exact arithmetic the original performs):
//    if delta > 12, delta = 12 - delta; if delta < -11, delta = -delta - 12. Only delta's SIGN is
//    consulted afterward.
// 5. @0x00430e73-0x00430ed5: delta < 0 steps facing_dir DOWN with wraparound (1 -> 0x18, else
//    facing_dir--); delta >= 0 steps it UP with wraparound (0x18 -> 1, else facing_dir++).
// 6. @0x00430ed5-0x00430edd: llm_tact_unit_vision_add(unit_idx) -- rebuild the FOV stencil at the
//    new facing.
// 7. @0x00430edd-0x00430ef4: move_state_timer += character_types[type].rotate (accumulates the
//    class's per-step rotate-time constant; NOT an overwrite, unlike the door path in
//    tact_unit_move_tick's move_state_timer).
void unit_rotate_step(tact_store &own, int32_t unit_idx, int32_t target_dir);

} // namespace detail

void unit_rotate_step(int32_t unit_idx, int32_t target_dir);


} // namespace mh::tact
