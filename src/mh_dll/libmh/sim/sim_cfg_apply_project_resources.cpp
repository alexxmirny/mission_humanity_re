//
// sim/sim_cfg_apply_project_resources.cpp -- see sim_cfg_apply_project_resources.h. Translated from
// the DISASSEMBLY (tmp/decomp/llm_cfg_apply_project_resources_00492e35.asm), which confirms the
// Ghidra .c draft's control-flow shape (id-check-then-bound-check loop, unconditional per-entry
// grant) exactly.
//
#include "sim/sim_cfg_apply_project_resources.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const cfg_apply_project_resources_calls &live_cfg_apply_project_resources_calls() {
    static const cfg_apply_project_resources_calls c = {
        MH_LIBMH_BIND(llm_resource_add),
    };
    return c;
}

namespace detail {

void cfg_apply_project_resources(const sim_view &v, const cfg_apply_project_resources_calls &c,
                                 uint32_t player_id, uint32_t project_index) {
    // 0x00492e59-0x00492ea6: unbounded for(;;) over Projects[project_index].resource[i], id-check
    // FIRST, bound-check SECOND (same order as every resource-walk sibling -- see header banner).
    for (int32_t i = 0;; ++i) {
        const cfg_resource &r = v.cfg_projects[project_index].resource[i];

        const uint32_t id = r.id;             // 0x00492e68
        if (id == 0) break;                   // 0x00492e71/0x00492e75: UNDEFINED sentinel
        if (!(i < CFG_RESOURCE_SLOTS)) break; // 0x00492e77/0x00492e7b: bound check, evaluated SECOND

        // 0x00492e8e-0x00492e9b: val read off the same entry (+4 past id); player masked to 16 bits
        // at the call site (matches every sibling's per-call truncation).
        c.resource_add(static_cast<int32_t>(player_id & 0xffffu), static_cast<int32_t>(id), r.val);
    }
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void cfg_apply_project_resources(uint32_t player_id, uint32_t project_index) {
    const sim_view v = state().read;
    detail::cfg_apply_project_resources(v, live_cfg_apply_project_resources_calls(), player_id,
                                        project_index);
}


} // namespace mh::sim
