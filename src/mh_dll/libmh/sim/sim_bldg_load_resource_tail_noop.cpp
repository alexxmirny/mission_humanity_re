//
// sim/sim_bldg_load_resource_tail_noop.cpp -- see sim_bldg_load_resource_tail_noop.h. Translated
// from the DISASSEMBLY (tmp/decomp/llm_bldg_load_resource_tail_noop_0049f84a.asm), not from the
// Ghidra .c draft.
//
#include "sim/sim_bldg_load_resource_tail_noop.h"


namespace mh::sim {

namespace detail {

void bldg_load_resource_tail_noop() {
    // The whole original (0x0049f84a-0x0049f89f), after the inert assert_stack_capacity prologue:
    // a nine-iteration loop (counter 1..9) that computes an address from the counter and whatever
    // value happened to be in EAX at CALL time, reads one word from it (CMP, result discarded), and
    // falls out with no return value set (RET with no preceding EAX load -- the function is void, not
    // an int-returning "always 9" as a caller-side misreading of the counter would suggest). No
    // memory write, no call, no observable effect. See the header banner for the full derivation.
    //
    // Confirmed dead code: a loop with no memory writes and no calls, whose only caller (building-
    // order case 21/0x15's tail) discards the (nonexistent) return value. Same posture as
    // sim_bldg_transfer_notify_noop.cpp -- a literal empty body is behaviourally identical.
}

} // namespace detail

// ---- the public surface --------------------------------------------------------------------------

void bldg_load_resource_tail_noop() {
    detail::bldg_load_resource_tail_noop();
}


} // namespace mh::sim


namespace mh::sim {
} // namespace mh::sim
