//
// tact/tact_unit_cmd_queue_resubmit_run.h -- TACT1B: re-issue a block of queue entries
// (the RUN button's replay), then dequeue-advance the original slot.
//
//   llm_tact_unit_cmd_queue_resubmit_run @0x0043069e (0x127)
//
#pragma once
#include <cstdint>

#include "tact/tact_state.h"

namespace mh::tact {
namespace detail {

// llm_tact_unit_cmd_queue_resubmit_run @0x0043069e.
//
// THE LOOP BOUND IS INCLUSIVE, NOT A FENCEPOST BUG -- preserved literally (Law 2). The top-of-loop
// test at 0x004306c8-0x004306df re-reads cmd_queue[queue_slot].arg0 (the run length op 0x47
// stamped there, see mh_llm_tact_unit_cmd_entry::arg0's field doc) and compares it against the
// iteration counter `i` with JGE: the body runs whenever `arg0 >= i`. Since `i` starts at 0 and
// arg0 is an unsigned uint16 field (always >= 0), the FIRST iteration always executes regardless of
// arg0's value, and the loop as a whole runs `arg0 + 1` times, not `arg0` times -- i.e. `for (i = 0;
// i <= arg0; ++i)`, not `i < arg0`.
//
// 1. Loop (arg0+1 iterations, see above): starting at `cur_slot = queue_slot`, re-enqueue that
//    slot's own current {op, interrupt_flag, arg0, arg1, arg2, arg3} via
//    llm_tact_unit_enqueue_command (frontier, @0x00430791) -- INCLUDING the first pass, which
//    resubmits queue_slot's own entry using its own run-length value as the new command's arg0.
//    `cur_slot` then advances by one, wrapping 0x80 -> 0 (@0x00430796-0x004307a5).
// 2. @0x004307b1-0x004307b7: dispatch llm_tact_unit_cmd_advance(unit_idx, queue_slot) (frontier) --
//    the ORIGINAL queue_slot parameter, not the walked cur_slot.
void unit_cmd_queue_resubmit_run(tact_store &own, int32_t unit_idx, int32_t queue_slot);

} // namespace detail

void unit_cmd_queue_resubmit_run(int32_t unit_idx, int32_t queue_slot);


} // namespace mh::tact
