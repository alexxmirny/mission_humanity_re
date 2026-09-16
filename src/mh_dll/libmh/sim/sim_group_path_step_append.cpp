//
// sim/sim_group_path_step_append.cpp -- see sim_group_path_step_append.h. Translated from the
// DISASSEMBLY (tmp/decomp_sim/llm_strat_group_path_step_append_0041fdda.asm), address-by-address.
//
#include "sim/sim_group_path_step_append.h"

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace detail {

void group_path_step_append(const sim_view &v, sim_store &own, int32_t order_idx, uint32_t heading) {
    const int32_t owner     = *v.group_order_owner;
    int32_t      &build_idx = own.group_path_build_idx_mut();

    if (build_idx > 0) {
        // 0x0041fe00-0x0041fe26: compare the entry before the cursor's heading to the new one.
        const uint8_t prev_heading =
            own.path_buffer_at(static_cast<uint32_t>(owner), order_idx, build_idx - 1).heading;
        if (prev_heading == heading) {
            // 0x0041fe2a-0x0041fe4c: extend that entry's run instead of appending a new one. No
            // saturation guard here (unlike group_path_step_record's coalescing path) -- an overflow
            // past 255 wraps to 0, reproduced literally, not "fixed" to saturate.
            ++own.path_buffer_at(static_cast<uint32_t>(owner), order_idx, build_idx - 1).run_length;
            return;
        }
    }

    // 0x0041fe4e-0x0041fe98: append a fresh entry (run_length=1) and advance the cursor.
    own.path_buffer_at(static_cast<uint32_t>(owner), order_idx, build_idx).run_length = 1;
    own.path_buffer_at(static_cast<uint32_t>(owner), order_idx, build_idx).heading =
        static_cast<uint8_t>(heading);
    ++build_idx;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void group_path_step_append(int32_t order_idx, uint32_t heading) {
    sim_state st = state();
    detail::group_path_step_append(st.read, st.own, order_idx, heading);
}


} // namespace mh::sim
