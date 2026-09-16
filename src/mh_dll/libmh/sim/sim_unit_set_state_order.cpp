//
// sim/sim_unit_set_state_order.cpp -- see sim_unit_set_state_order.h. Translated from the DISASSEMBLY
// (tmp/decomp_sim/llm_strat_unit_set_state_order_00486657.asm); the exported .c draft agrees with the
// assembly here (two passthrough writes to the ambient current unit's state/order fields), so this is
// another case, like sim_unit_set_state_order_of.cpp's, where the draft's logic was worth re-deriving
// as asked and turned out correct -- only its plate's "movement states commit the order on arrival"
// sentence does not describe this function's own (branch-free) body, see the header note.
//
#include "sim/sim_unit_set_state_order.h"

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace detail {

void unit_set_state_order(sim_store &own, uint16_t state, uint16_t order) {
    // 0x0048667a-0x00486681: MOV word ptr [_G_LLM_STRAT_CUR_UNIT+0x6],AX -- cur_unit().state = param_1.
    // 0x00486687-0x0048668e: MOV word ptr [_G_LLM_STRAT_CUR_UNIT+0x4],AX -- cur_unit().order = param_2.
    // _G_LLM_STRAT_CUR_UNIT is re-loaded fresh before each store (two separate `MOV EDX,[0x00e162e0]`),
    // matching the original's own "no address kept across stores" idiom the explicit-target sibling
    // (_of) also transcribes -- own.cur_unit() re-resolves the pointer on every call, so no base is
    // cached here either.
    own.cur_unit().state = state;
    own.cur_unit().order = order;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------
//
// Parameters are named `state`/`order` to match the batch context's own naming for this function's
// two params (and the struct-field write each maps to) -- `state` shadows the free function
// `mh::sim::state()`, so the call below is qualified rather than bare, same fix
// sim_unit_set_state_order_of.cpp's own wrapper documents for its identically-shadowed `state`
// parameter.

void unit_set_state_order(uint16_t state, uint16_t order) {
    sim_state st = mh::sim::state();
    detail::unit_set_state_order(st.own, state, order);
}


} // namespace mh::sim
