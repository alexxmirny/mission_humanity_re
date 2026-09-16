//
// sim/sim_group_scratch_add_unit.cpp -- see sim_group_scratch_add_unit.h. Translated from the
// DISASSEMBLY (tmp/decomp_sim/llm_strat_group_scratch_add_unit_and_normalize_heading_0048d283.asm).
//
#include "sim/sim_group_scratch_add_unit.h"

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace detail {

void group_scratch_add_unit_and_normalize_heading(const sim_view &v, sim_store &own, int32_t player,
                                                  int32_t unit_index, int32_t *io_count) {
    // 0x0048d2a2-0x0048d2b1: append the unit index at the running cursor.
    own.group_move_scratch_at(*io_count).unit_idx = unit_index;

    // 0x0048d2b1-0x0048d2e7: normalize move_heading through the remap table. The table is
    // int32_t[24] (SHL EAX,0x2 before the table access in the asm); only its LOW BYTE is stored back
    // -- see the header derivation for why this is the faithful reading, not a truncation bug.
    unit         &u              = own.unit_at(static_cast<uint32_t>(player), unit_index);
    const int32_t remapped_value = v.group_step_heading_remap[u.move_heading];
    u.move_heading               = static_cast<uint8_t>(remapped_value);

    // 0x0048d2e7-0x0048d2ea.
    *io_count += 1;
}

} // namespace detail

void group_scratch_add_unit_and_normalize_heading(int32_t player, int32_t unit_index, int32_t *io_count) {
    sim_state st = state();
    detail::group_scratch_add_unit_and_normalize_heading(st.read, st.own, player, unit_index, io_count);
}


} // namespace mh::sim
