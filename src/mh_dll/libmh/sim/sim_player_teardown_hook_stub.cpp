//
// sim/sim_player_teardown_hook_stub.cpp -- see sim_player_teardown_hook_stub.h. Translated from the
// DISASSEMBLY (tmp/decomp/llm_player_teardown_hook_stub_0049800c.asm), not from the Ghidra .c draft.
//
#include "sim/sim_player_teardown_hook_stub.h"


namespace mh::sim {

namespace detail {

int32_t player_teardown_hook_stub(int32_t player_index) {
    // The whole original: PUSH EBP/MOV EBP,ESP/PUSH 0x24/CALL assert_stack_capacity (inert, brief
    // rule 6) / callee-save pushes (unused registers, not reproduced) / spill player_index to its
    // stack home (0x00498024, a compiler artifact with no observable effect) / a local set to 0
    // (0x00498027) / that local returned in EAX (0x0049802e-0x00498031) (0x0049800c-0x0049803a).
    // Nothing reads or writes any game state; the parameter is genuinely unused beyond the spill.
    (void)player_index;
    return 0;
}

} // namespace detail

// ---- the public surface --------------------------------------------------------------------------

int32_t player_teardown_hook_stub(int32_t player_index) {
    return detail::player_teardown_hook_stub(player_index);
}


} // namespace mh::sim


namespace mh::sim {
} // namespace mh::sim
