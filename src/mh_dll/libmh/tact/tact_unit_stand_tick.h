//
// tact/tact_unit_stand_tick.h -- TACT1C: the per-tick driver that STANDS A UNIT BACK UP. While the
// unit is mid kneel/turn (anim_state 2 or 3) it advances the stand animation and, on completion,
// puts the unit back in anim_state 0; in any other state there is no animation to run, so it only
// watches for a queued STAND (op 5) command to dequeue.
//
//   llm_tact_unit_stand_tick @0x0043047d (0x103)
//   void __watcall llm_tact_unit_stand_tick(int unit_idx)
//
// SHAPE, transcribed literally from @0x0043049a-0x00430577. THE ARM SELECTION READS BACKWARDS FROM
// THE JUMP CONDITIONS -- both tests jump to LAB_004304ba, whose whole body is `JMP 0x004304fb`, the
// animation arm. This header and the body stated it inverted until 2026-09-04, which is why a
// kneeling unit could never stand:
//   1. anim_state == 2 or 3 (kneeling/turning): ++progress; unit_set_anim_state(unit_idx, 3)
//      UNCONDITIONALLY (regardless of whether the threshold below is crossed this call).
//   2. ... and if progress > 0xf: progress = 0; unit_set_anim_state(unit_idx, 0); then if
//      cmd_queue[cmd_index].op == 5, call unit_cmd_advance(cmd_index) to dequeue it.
//   3. any other anim_state: if cmd_queue[cmd_index].op == 5 (a STAND with nothing to animate),
//      dequeue it the same way. Return either way -- no animation progress touched on this arm.
//
// PROOF: RIG. the measured write closure of llm_tact_unit_stand_tick -> 4 functions reachable, 2
// regions: _G_LLM_TACT_UNITS (depth 0, own), _G_LLM_STRAT_PATH_SLOT_FLAGS (depth 1, written by
// llm_tact_unit_cmd_advance -- already a declared migration write elsewhere in this closure). No
// outward-effect reach.
//
#pragma once
#include <cstdint>

#include "tact/tact_state.h"

namespace mh::tact {

// The two outward callees, indirected through the calls struct so the offline oracle (and any
// future review) can drive them deterministically -- same convention as every other cross-TU call
// in this domain, regardless of whether the callee is itself translated yet.
struct unit_stand_tick_calls {
    void (*unit_set_anim_state)(int32_t building_id, uint8_t state);      // llm_tact_unit_set_anim_state @0x00430f03
    void (*unit_cmd_advance)(int32_t unit_index, int32_t cmd_slot_index); // llm_tact_unit_cmd_advance @0x00431229
};

const unit_stand_tick_calls &live_unit_stand_tick_calls();

namespace detail {

// llm_tact_unit_stand_tick @0x0043047d.
void unit_stand_tick(tact_store &own, const unit_stand_tick_calls &c, int32_t unit_idx);

} // namespace detail

void unit_stand_tick(int32_t unit_idx);


} // namespace mh::tact
