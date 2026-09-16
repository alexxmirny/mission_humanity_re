//
// sim/sim_unit_state_stop_to_default.cpp -- see sim_unit_state_stop_to_default.h. Translated from the
// DISASSEMBLY (tmp/decomp/llm_strat_unit_state_stop_to_default_0047e1e1.asm), which agrees with the
// Ghidra .c draft on every point here (a short, linear function with no re-derived locals to
// cross-check) -- still transcribed from the asm per the translator brief's rule 1.
//
#include "sim/sim_unit_state_stop_to_default.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const unit_state_stop_to_default_calls &live_unit_state_stop_to_default_calls() {
    static const unit_state_stop_to_default_calls c = {
        MH_LIBMH_BIND(llm_strat_path_free_slot),
        MH_LIBMH_BIND(llm_strat_unit_set_state),
    };
    return c;
}

namespace detail {

void unit_state_stop_to_default(const sim_view &v, sim_store &own, const unit_state_stop_to_default_calls &c) {
    // 0x0047e1f9-0x0047e219: if this unit currently owns a path buffer slot, release it. Read-only on
    // cur_unit (path_slot_id is not written here -- llm_strat_path_free_slot owns that write, outside
    // this function).
    if (v.cur_unit->path_slot_id != 0xffu) {
        c.path_free_slot(*v.cur_player, static_cast<int32_t>(*v.cur_index));
    }

    // 0x0047e21a-0x0047e258: if the unit's current state does not already match its cfg type's
    // default/idle order code, commit that default via the ORIGINAL setter (this function does not
    // write unit.state itself -- llm_strat_unit_set_state writes only that field, per the struct's own
    // comment).
    const cfg_unit &proto = v.cfg_units[v.cur_unit->unit_proto_id];
    if (proto.default_op_code != v.cur_unit->state) {
        c.unit_set_state(proto.default_op_code);
    }

    // 0x0047e258-0x0047e26c: zero the per-tick time budget (both dwords of the double set to 0 --
    // ordinary `own.tick_budget() = 0.0`, per the batch context's note on this exact idiom).
    own.tick_budget() = 0.0;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void unit_state_stop_to_default() {
    sim_state st = state();
    detail::unit_state_stop_to_default(st.read, st.own, live_unit_state_stop_to_default_calls());
}


} // namespace mh::sim
