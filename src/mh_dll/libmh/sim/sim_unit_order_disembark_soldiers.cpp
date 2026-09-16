//
// sim/sim_unit_order_disembark_soldiers.cpp -- see sim_unit_order_disembark_soldiers.h. Translated
// from the DISASSEMBLY (tmp/decomp/llm_unit_order_disembark_soldiers_0046e76c.asm).
//
#include "sim/sim_unit_order_disembark_soldiers.h"

#include "addr/mh_calls.gen.h"
#include "addr/mh_rebind.gen.h"
#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state
#include "state/promoted_select.h"
#include "state/rebind_targets.gen.h"

namespace mh::sim {

namespace {

// 0x0046e7b2 / 0x0046e7b7: MOV ECX,0x32 / MOV EBX,0x32 -- the order code this wrapper always issues.
// Named once rather than twice so the two call arguments cannot drift apart.
inline constexpr uint16_t ORDER_DISEMBARK_SOLDIERS = 0x32u;

// 0x0046e7bf: OR AL,0x80 -- the owner-and-kind tag bit this wrapper ORs onto the player index before
// handing it to dispatch. Same shape as the 0x40 the building-side wrapper uses (see
// sim_bldg_instant_construct_find_slot.cpp); the bit's meaning is dispatch's business, not this
// function's, so no semantic name is invented for it here.
inline constexpr uint32_t DISPATCH_OWNER_TAG_UNIT = 0x80u;

int32_t dispatch_inert(uint16_t, uint32_t, uint16_t, uint16_t) { return 0; }

} // namespace

const unit_order_disembark_soldiers_calls &live_unit_order_disembark_soldiers_calls() {
    static const unit_order_disembark_soldiers_calls c = {
        MH_PROMOTED_ROW(llm_strat_order_dispatch),
    };
    return c;
}

const unit_order_disembark_soldiers_calls &inert_unit_order_disembark_soldiers_calls() {
    static const unit_order_disembark_soldiers_calls c = {
        dispatch_inert,
    };
    return c;
}

namespace detail {

void unit_order_disembark_soldiers(const sim_view &v, const unit_order_disembark_soldiers_calls &c,
                                   uint32_t player, int32_t unit_idx) {
    // 0x0046e789-0x0046e79c: the roster read uses the player index TRUNCATED TO 16 BITS. Reproduced
    // rather than normalised -- the index arithmetic below it is what the original computes, and a
    // caller passing dirty high bits must land in the same row we do.
    const unit &u = unit_of(v, (uint32_t)(uint16_t)player, unit_idx);

    // 0x0046e7a3-0x0046e7b0: CMP dword ptr [cfg Unit[proto] + 0x227],0x0 / JLE. SIGNED and
    // STRICTLY-GREATER: zero AND negative both skip the dispatch entirely.
    if ((int32_t)v.cfg_units[u.unit_proto_id].soldier_count <= 0) return;

    // 0x0046e7b2-0x0046e7c8. EAX = (uint16_t)unit_idx, EDX = (uint16_t)(player | 0x80),
    // EBX = ECX = 0x32. The return value is discarded, exactly as the original discards EAX.
    (void)c.order_dispatch((uint16_t)unit_idx, (uint32_t)(uint16_t)(player | DISPATCH_OWNER_TAG_UNIT),
                           ORDER_DISEMBARK_SOLDIERS, ORDER_DISEMBARK_SOLDIERS);
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void unit_order_disembark_soldiers(uint32_t player, int32_t unit_idx) {
    const sim_view v = state().read;
    detail::unit_order_disembark_soldiers(v, live_unit_order_disembark_soldiers_calls(), player,
                                          unit_idx);
}


} // namespace mh::sim
