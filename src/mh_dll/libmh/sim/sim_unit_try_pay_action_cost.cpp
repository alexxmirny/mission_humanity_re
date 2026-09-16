//
// sim/sim_unit_try_pay_action_cost.cpp -- see sim_unit_try_pay_action_cost.h. Translated from the
// DISASSEMBLY (tmp/decomp_sim/llm_strat_unit_try_pay_action_cost_00493254.asm), not from the Ghidra
// .c draft.
//
#include "sim/sim_unit_try_pay_action_cost.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const unit_try_pay_action_cost_calls &live_unit_try_pay_action_cost_calls() {
    static const unit_try_pay_action_cost_calls c = {
        MH_LIBMH_BIND(game_SpendResource),
    };
    return c;
}

namespace detail {

int32_t unit_try_pay_action_cost(const sim_view &v, const unit_try_pay_action_cost_calls &c,
                                 uint32_t player, int32_t unit_idx) {
    // PRESERVE-BUG (reimpl-verify, 2026-08-22): the original stores the full 32-bit `player` argument
    // once (0x0049326b) but every later use narrows it back through a 16-bit MOVZX (0x00493271,
    // 0x004932d1, 0x00493363) -- the upper 16 bits are provably discarded everywhere. Reproduced by
    // truncating at entry rather than passing the full 32-bit value into unit_of/player_resource_of/
    // game_SpendResource (none of which mask it themselves).
    player = static_cast<uint16_t>(player);

    const unit &u     = unit_of(v, player, unit_idx);
    const auto &costs = v.cfg_units[u.unit_proto_id].resource_2;

    // ---- pass 1, 0x0049328e-0x00493310: can we afford every entry? First shortfall records
    // `.id + 0x89`; a SECOND (or later) shortfall in the same call collapses the code to the flat
    // sentinel 0x89 instead (preserved literally, see the header banner).
    int32_t error_code = 0;
    for (int32_t j = 0; j < CFG_RESOURCE_SLOTS; ++j) {
        if (costs[j].id == 0) break; // terminator
        if (player_resource_of(v, static_cast<int32_t>(player), costs[j].id) < costs[j].val) {
            error_code = (error_code != 0) ? 0x89 : (costs[j].id + 0x89);
        }
    }
    if (error_code != 0) return error_code;

    // ---- pass 2, 0x0049331e-0x00493374: everything affordable -- spend it all, unconditionally.
    for (int32_t j = 0; j < CFG_RESOURCE_SLOTS; ++j) {
        if (costs[j].id == 0) break; // terminator
        c.game_SpendResource(static_cast<int32_t>(player), static_cast<int32_t>(costs[j].id),
                             costs[j].val);
    }
    return 0;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

int32_t unit_try_pay_action_cost(uint32_t player, int32_t unit_idx) {
    sim_state st = state();
    return detail::unit_try_pay_action_cost(st.read, live_unit_try_pay_action_cost_calls(), player,
                                            unit_idx);
}


} // namespace mh::sim
