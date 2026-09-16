//
// sim/sim_dmp_enqueue_scripted_order.cpp -- see sim_dmp_enqueue_scripted_order.h. Translated from the
// DISASSEMBLY (tmp/decomp_sim/llm_strat_dmp_enqueue_scripted_order_0046d873.asm); the Ghidra .c draft
// (tmp/decomp_sim/llm_strat_dmp_enqueue_scripted_order_0046d873.c) agrees call-for-call and was used
// only as a cross-check.
//
#include "sim/sim_dmp_enqueue_scripted_order.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const dmp_enqueue_scripted_order_calls &live_dmp_enqueue_scripted_order_calls() {
    static const dmp_enqueue_scripted_order_calls c = {
        MH_LIBMH_BIND(llm_strat_order_scratch_reset),
        MH_LIBMH_BIND(llm_strat_order_scratch_set_field),
        MH_LIBMH_BIND(llm_strat_order_enqueue),
    };
    return c;
}

namespace detail {

uint32_t dmp_enqueue_scripted_order(const dmp_enqueue_scripted_order_calls &c, uint32_t x, uint32_t y,
                                    uint32_t unit_type_id, uint16_t owner_and_kind) {
    // 0x0046d894: reset the scratch-args channel before staging this order's fields.
    c.order_scratch_reset();
    // 0x0046d899-0x0046d8bb: stage (x, y, unit_type_id) into scratch fields (4, 5, 1) -- confirmed
    // against sim_order_dispatch_admin.cpp's admin_arm::UNIT_CREATE handler, which reads exactly
    // args[4]/args[5]/args[1] for this order code. See the header note for the cross-derivation.
    c.order_scratch_set_field(4, (int32_t)x);
    c.order_scratch_set_field(5, (int32_t)y);
    c.order_scratch_set_field(1, (int32_t)unit_type_id);
    // 0x0046d8c0-0x0046d8d0: unit_index=0 (no existing unit to target), owner_and_kind passed through
    // unchanged (no local kind mask -- see the header note), param0=order_code=0xeb/0xeb -- the fixed
    // UNIT_CREATE order (/llm/E_ORDER_CODE, sim_order_dispatch.h's admin_arm::UNIT_CREATE).
    c.order_enqueue(0, owner_and_kind, 0xeb, 0xeb);
    // 0x0046d8d5: the original never tests order_enqueue's own result -- always returns 1.
    return 1;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

uint32_t dmp_enqueue_scripted_order(uint32_t x, uint32_t y, uint32_t unit_type_id,
                                    uint16_t owner_and_kind) {
    return detail::dmp_enqueue_scripted_order(live_dmp_enqueue_scripted_order_calls(), x, y,
                                              unit_type_id, owner_and_kind);
}


} // namespace mh::sim
