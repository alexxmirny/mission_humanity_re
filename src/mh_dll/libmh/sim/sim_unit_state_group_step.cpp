//
// sim/sim_unit_state_group_step.cpp -- see sim_unit_state_group_step.h. Translated from the
// DISASSEMBLY (tmp/decomp/llm_strat_unit_state_group_step_00482f7f.asm), not from the Ghidra .c
// draft -- the draft's overall shape (a two-way dispatch on Unit[proto].type, A_PLANE/H_PLANE vs
// everything else) agrees with the assembly and is reproduced faithfully; only the asm's own
// double re-derivation of units[cur_player][cur_index].unit_proto_id is collapsed into one read
// (see the header banner).
//
#include "sim/sim_unit_state_group_step.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const unit_state_group_step_calls &live_unit_state_group_step_calls() {
    static const unit_state_group_step_calls c = {
        MH_LIBMH_BIND(llm_strat_unit_group_step_plane),
        MH_LIBMH_BIND(llm_strat_unit_group_step_ground),
    };
    return c;
}

namespace detail {

void unit_state_group_step(const sim_view &v, const unit_state_group_step_calls &c) {
    // 0x00482f97-0x00482fb3 (first read) / 0x00482fc9-0x00482fe5 (identical second re-read, collapsed
    // here into one): units[cur_player][cur_index].unit_proto_id -- read via the SAME roster
    // arithmetic the asm uses (units[cur_player * UNITS_PER_PLAYER + cur_index]) rather than through
    // the equivalent _G_LLM_STRAT_CUR_UNIT pointer, so the access pattern matches the disassembly
    // (both resolve to the identical record; the driver sets cur_unit to exactly this slot).
    const unit &u = v.units[*v.cur_player * v.caps.units + *v.cur_index];

    // 0x00482fba-0x00482fc7 / 0x00482fec-0x00482ff9: Unit[unit_proto_id].type, compared against
    // A_PLANE (0x11) then H_PLANE (0x12).
    const uint32_t type = v.cfg_units[u.unit_proto_id].type;

    // LAB_00482ffb vs LAB_00483002: type==A_PLANE JZ's straight to the plane call; falling through
    // the first compare, type==H_PLANE JNZ's PAST the ground call to the same plane call (so
    // A_PLANE||H_PLANE both take the plane arm); anything else takes the ground arm. A plain
    // disjunction is faithful to that branch structure.
    if (type == UNIT_TYPE_A_PLANE || type == UNIT_TYPE_H_PLANE) {
        c.group_step_plane();
    } else {
        c.group_step_ground();
    }
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void unit_state_group_step() {
    sim_state st = state();
    detail::unit_state_group_step(st.read, live_unit_state_group_step_calls());
}


} // namespace mh::sim
