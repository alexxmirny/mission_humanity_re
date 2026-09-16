//
// sim/sim_bldg_transfer_notify_noop.cpp -- see sim_bldg_transfer_notify_noop.h. Translated from the
// DISASSEMBLY (tmp/decomp/llm_bldg_transfer_notify_noop_0048f2f8.asm), not from the Ghidra .c draft.
//
#include "sim/sim_bldg_transfer_notify_noop.h"


namespace mh::sim {

namespace detail {

void transfer_notify_noop() {
    // The whole original: PUSH 0x20 / CALL assert_stack_capacity / RET (0x0048f2f8-0x0048f31d).
    // Nothing to reproduce but the fact that the slot exists and does nothing -- same shape as the
    // AI domain's llm_strat_ai_bldg_queue_handle_state2_empty
    // (ai/ai_bldg_queue_dispatch.cpp::bldg_queue_handle_state2_empty).
}

} // namespace detail

// ---- the public surface --------------------------------------------------------------------------

void transfer_notify_noop() {
    detail::transfer_notify_noop();
}


} // namespace mh::sim


namespace mh::sim {
} // namespace mh::sim
