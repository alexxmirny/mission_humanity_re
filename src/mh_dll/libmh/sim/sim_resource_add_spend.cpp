#include "sim/sim_resource_add_spend.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const resource_add_spend_calls &live_resource_add_spend_calls() {
    static const resource_add_spend_calls c = {
        MH_LIBMH_BIND(game_UpdateResourceStats),
    };
    return c;
}

namespace detail {

// llm_resource_add @0x00497f4a. 0x00497f69-0x00497f7e: unconditional
// `player_resources[player][resource_index] += amount`, then 0x00497f7e-0x00497f87:
// game_UpdateResourceStats(player, amount, resource_index) -- no clamp, no branch anywhere in the body.
void resource_add(sim_store &own, const resource_add_spend_calls &c, int32_t player,
                  int32_t resource_index, int32_t amount) {
    int32_t &slot = own.player_resource_at((uint32_t)player, resource_index);
    slot += amount;
    c.update_resource_stats((uint32_t)player, (uint32_t)amount, (uint32_t)resource_index);
}

// game_SpendResource @0x00497f94. 0x00497fb3-0x00497fc8: `spend = amount;` then `CMP EAX,mem / JLE` --
// EAX holds `amount`, mem is the CURRENT player_resources[player][res_id] value; JLE (taken when
// `amount <= current`) skips straight to LAB_00497fdf, i.e. the clamp block at 0x00497fca-0x00497fdc
// runs ONLY when `current < amount`, and there it overwrites `spend` with `current` itself (whatever
// its sign -- see the header banner). 0x00497fdf-0x00497ff4: `player_resources[player][res_id] -=
// spend` unconditionally (both paths converge here). 0x00497ff4-0x00497fff:
// game_UpdateResourceStats(player, -spend, res_id) -- the NEGATED spend amount, read off the `NEG EBX`
// at 0x00497ff7 immediately before the call, then EBX (amount slot) -> EDX (res_id) -> EAX (player)
// pushed into the call per the asm's own register setup.
void spend_resource(sim_store &own, const resource_add_spend_calls &c, int32_t player, int32_t res_id,
                    int32_t amount) {
    int32_t &slot  = own.player_resource_at((uint32_t)player, res_id);
    int32_t  spend = amount;
    if (slot < amount) spend = slot; // 0x00497fc2-0x00497fdc
    slot -= spend;                   // 0x00497fee
    // 0x00497ff7 `NEG EBX` -- reproduced as unsigned 0-wraparound rather than signed `-spend` so the
    // INT32_MIN edge case matches the instruction's bit pattern instead of invoking signed-overflow UB.
    c.update_resource_stats((uint32_t)player, (uint32_t)0u - (uint32_t)spend, (uint32_t)res_id);
}

} // namespace detail

// ---- the public wrappers ------------------------------------------------------------------------

void resource_add(int32_t player, int32_t resource_index, int32_t amount) {
    sim_state st = state();
    detail::resource_add(st.own, live_resource_add_spend_calls(), player, resource_index, amount);
}

void spend_resource(int32_t player, int32_t res_id, int32_t amount) {
    sim_state st = state();
    detail::spend_resource(st.own, live_resource_add_spend_calls(), player, res_id, amount);
}


} // namespace mh::sim
