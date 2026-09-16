//
// sim/sim_map_zoom_scale_x_get.cpp -- see sim_map_zoom_scale_x_get.h. Translated from the
// DISASSEMBLY (tmp/decomp/llm_map_zoom_scale_x_get_004a8016.asm); the exported .c draft agrees
// (CONCAT44 of the two loaded dwords is exactly "read the double", no divergence to note).
//
#include "sim/sim_map_zoom_scale_x_get.h"

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace detail {

double zoom_scale_x_get(const sim_view &v) {
    // 0x004a802f-0x004a803f: MOV EAX,[lo] / MOV EAX,[hi] into a stack temp, then FLD that temp --
    // the round trip through the stack has no observable effect; the net operation is "load the
    // double at _G_LLM_MAP_ZOOM_SCALE_X and return it".
    return *v.zoom_scale_x;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

double zoom_scale_x_get() {
    sim_state st = state();
    return detail::zoom_scale_x_get(st.read);
}


} // namespace mh::sim
