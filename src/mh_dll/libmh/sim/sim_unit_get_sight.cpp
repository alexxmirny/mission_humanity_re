//
// sim/sim_unit_get_sight.cpp -- see sim_unit_get_sight.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_unit_get_sight_004d4888.asm), not from the Ghidra .c draft.
//
#include "sim/sim_unit_get_sight.h"

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace detail {

uint32_t unit_get_sight(const sim_view &v, uint32_t unit_ref, int32_t unit_index) {
    // 0x004d4895: AND EAX,0xf -- real, not a decompiler artifact; see the header banner.
    const unit &u = unit_of(v, ref_owner(unit_ref), unit_index);
    // 0x004d48aa-0x004d48c3: Unit[u.unit_proto_id].sight, zero-extended.
    return (uint32_t)v.cfg_units[u.unit_proto_id].sight;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

uint32_t unit_get_sight(uint32_t unit_ref, int32_t unit_index) {
    const sim_view v = state().read;
    return detail::unit_get_sight(v, unit_ref, unit_index);
}


} // namespace mh::sim
