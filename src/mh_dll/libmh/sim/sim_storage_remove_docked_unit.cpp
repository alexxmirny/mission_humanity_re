//
// sim/sim_storage_remove_docked_unit.cpp -- see sim_storage_remove_docked_unit.h. Translated from
// the DISASSEMBLY (tmp/decomp_sim/llm_strat_storage_remove_docked_unit_00489dc4.asm), not from the
// Ghidra .c draft.
//
#include "sim/sim_storage_remove_docked_unit.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const storage_remove_docked_unit_calls &live_storage_remove_docked_unit_calls() {
    static const storage_remove_docked_unit_calls c = {
        MH_LIBMH_BIND(llm_strat_bldg_notify_ui),
    };
    return c;
}

namespace detail {

void storage_remove_docked_unit(const sim_view &v, sim_store &own,
                                const storage_remove_docked_unit_calls &c, uint16_t player,
                                int32_t unit_index, int32_t storage_slot) {
    unit_storage &st = own.storage_at(static_cast<uint32_t>(player), storage_slot);

    // 0x00489de3-0x00489df6: docked_count -= 1, unconditionally, before the soldier-count branch.
    st.docked_count -= 1;

    // 0x00489dfc-0x00489e7f: occupancy -= N, where N is 1 for a non-soldier-carrying unit or the
    // unit's cfg soldier_count otherwise -- see header step 2 on why this reads soldier_count once
    // rather than the asm's two identical re-reads.
    const unit   &u             = unit_of(v, static_cast<uint32_t>(player), unit_index);
    const int32_t soldier_count = v.cfg_units[u.unit_proto_id].soldier_count;
    if (soldier_count == 0) {
        st.occupancy -= 1;
    } else {
        st.occupancy -= soldier_count;
    }

    // 0x00489e86-0x00489eb8: linear scan for unit_index in docked_units[], NO bound check (see
    // header step 3 -- translator-brief rule 14, do not add a bound the original does not have).
    int32_t idx = 0;
    while (st.docked_units[idx] != unit_index) {
        idx += 1;
    }

    // 0x00489ebe-0x00489f28: compact -- shift every later entry down by one. `st.docked_count` is
    // read fresh through the reference on every iteration (matches the asm's re-read at the top of
    // the loop, translator-brief rule 16), so this sees the value AFTER the step-1 decrement.
    for (int32_t i = idx; i < st.docked_count; ++i) {
        st.docked_units[i] = st.docked_units[i + 1];
    }

    // 0x00489f2a-0x00489f4c: unconditional notify. b_index is read after the compaction loop, but
    // nothing in this function writes it, so the value is the same either way.
    c.notify_ui(player, static_cast<uint32_t>(st.b_index));
}

} // namespace detail

// ---- the public surface --------------------------------------------------------------------------

void storage_remove_docked_unit(uint16_t player, int32_t unit_index, int32_t storage_slot) {
    sim_state st = state();
    detail::storage_remove_docked_unit(st.read, st.own, live_storage_remove_docked_unit_calls(), player,
                                       unit_index, storage_slot);
}


} // namespace mh::sim


namespace mh::sim {
} // namespace mh::sim
