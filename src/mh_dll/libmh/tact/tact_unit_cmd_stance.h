#pragma once
#include <cstdint>

#include "tact/tact_state.h"

namespace mh::tact {

// The one outward call, indirected for offline testability (mocking the DISPATCH, not the wall-clock
// write inside it) -- same shape as tact_unit_cmd_advance_with_defstat.h's `_calls` struct.
struct unit_cmd_stance_calls {
    void (*unit_cmd_advance)(int32_t unit_idx, int32_t cmd_slot_index); // llm_tact_unit_cmd_advance
};

const unit_cmd_stance_calls &live_unit_cmd_stance_calls();

namespace detail {

// llm_tact_unit_cmd_stance_on @0x00430580.
//
// 1. @0x0043059d-0x004305b4: status |= 0x4.
// 2. @0x004305ba-0x004305ec: if cmd_queue[cmd_index].op == 0x1e AND progress == 0, skip the
//    dispatch below (already-queued stance-on marker not yet started).
// 3. @0x004305f0-0x00430606: otherwise llm_tact_unit_cmd_advance(unit_idx, cmd_index).
void unit_cmd_stance_on(tact_store &own, const unit_cmd_stance_calls &c, int32_t unit_idx);

// llm_tact_unit_cmd_stance_off @0x0043060f -- the mirror: status &= ~0x4, marker op 0x1c.
void unit_cmd_stance_off(tact_store &own, const unit_cmd_stance_calls &c, int32_t unit_idx);

} // namespace detail

void unit_cmd_stance_on(int32_t unit_idx);
void unit_cmd_stance_off(int32_t unit_idx);


} // namespace mh::tact
