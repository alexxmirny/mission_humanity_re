//
// sim/sim_unit_state_patrol_swap.cpp -- see sim_unit_state_patrol_swap.h. Translated from the
// DISASSEMBLY (tmp/decomp/llm_strat_unit_state_patrol_swap_0047e2e2.asm).
//
#include "sim/sim_unit_state_patrol_swap.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const unit_state_patrol_swap_calls &live_unit_state_patrol_swap_calls() {
    static const unit_state_patrol_swap_calls c = {
        MH_LIBMH_BIND(llm_strat_unit_set_state_order),
    };
    return c;
}

namespace detail {

void unit_state_patrol_swap(const sim_view &v, sim_store &own, const unit_state_patrol_swap_calls &c) {
    unit &u = own.cur_unit(); // _G_LLM_STRAT_CUR_UNIT dereferenced, read+write -- see sim_view::cur_unit's
                              // comment: bound from the SAME resolved pointer as *v.cur_player/*v.cur_index.
                              // The assembly re-derives this same address independently for EACH of the
                              // four field copies below (8 separate MOV EAX,[CUR_UNIT] / MOV EDX,[CUR_UNIT]
                              // pairs, one load per side of each copy) rather than reusing one pointer --
                              // reused here as one local, matching sim_unit_state_die_explode.cpp's
                              // identical simplification of the same re-derivation idiom.

    // 0x0047e2fa-0x0047e356: swap goal<->home, FOUR single-byte field copies in this exact order.
    // ORDER IS LOAD-BEARING: goal_x/goal_y are written FIRST, from the unit's OLD home_x/home_y, and
    // only THEN is home_x/home_y overwritten from the unit's current x/y -- so goal ends up holding
    // the PREVIOUS rally point, not the new one. Reordering these four lines would still compile and
    // would still "swap something", but would make goal_x/goal_y pick up the already-overwritten
    // home_x/home_y instead.
    u.goal_x = u.home_x; // 0x0047e2fa-0x0047e311 (dest[+0x86] = src[+0x88])
    u.goal_y = u.home_y; // 0x0047e311-0x0047e328 (dest[+0x87] = src[+0x89])
    u.home_x = u.x;      // 0x0047e328-0x0047e33f (dest[+0x88] = src[+0x84])
    u.home_y = u.y;      // 0x0047e33f-0x0047e356 (dest[+0x89] = src[+0x85])

    // 0x0047e356-0x0047e376: llm_strat_unit_set_state_order(Unit[proto].move_op_arg, PATROL_SWAP).
    // Param order EAX=move_op_arg (param_1), EDX=0x10 (param_2, PATROL_SWAP) -- EDX is loaded with the
    // literal 0x10 BEFORE the move_op_arg computation runs in the asm, and
    // llm_strat_unit_set_state_order's committed (uint16_t param_1, uint16_t param_2) signature binds
    // param_1<-EAX, param_2<-EDX (addr/mh_calls.gen.h), so this is unambiguous.
    const uint8_t move_op_arg = v.cfg_units[u.unit_proto_id].move_op_arg; // Unit[proto].move_op_arg, uint8_t @0xec
    c.unit_set_state_order(static_cast<uint16_t>(move_op_arg), UNIT_STATE_PATROL_SWAP_ORDER);
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void unit_state_patrol_swap() {
    sim_state st = state();
    detail::unit_state_patrol_swap(st.read, st.own, live_unit_state_patrol_swap_calls());
}


} // namespace mh::sim
