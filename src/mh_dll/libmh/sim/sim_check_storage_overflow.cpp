//
// sim/sim_check_storage_overflow.cpp -- see sim_check_storage_overflow.h. Translated from the
// DISASSEMBLY (tmp/decomp/llm_strat_check_storage_overflow_0043fe9d.asm), not from the Ghidra .c
// draft.
//
#include "sim/sim_check_storage_overflow.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const check_storage_overflow_calls &live_check_storage_overflow_calls() {
    static const check_storage_overflow_calls c = {
        MH_LIBMH_BIND(llm_strat_decay_excess_resources),
    };
    return c;
}

namespace detail {

void check_storage_overflow(const sim_view &v, const check_storage_overflow_calls &c, int32_t player) {
    // 0x0043febf-0x0043ff00: flat for (i = 0; i < 10; ++i), no early exit -- every slot is checked
    // regardless of whether an earlier one triggered a decay.
    for (int32_t i = 0; i < 10; ++i) {
        // 0x0043fecf-0x0043fef3: v.storage_stats[player].cap_prev[i] < player_resources[player][i]
        // -> decay the excess. JGE skips the CALL when cap_prev >= holdings; only the strict
        // less-than case falls through, matching the branch polarity exactly.
        if (v.storage_stats[player].cap_prev[i] < player_resource_of(v, player, i)) {
            // 0x0043fef5-0x0043fefb: EDX=i, EAX=player (re-read from the entry stash). Return value
            // unused by the original caller (nothing reads EAX before the next iteration), discarded
            // here too.
            c.decay_excess_resources(player, i);
        }
    }
}

} // namespace detail

// ---- the public surface --------------------------------------------------------------------------

void check_storage_overflow(int32_t player) {
    const sim_view v = state().read;
    detail::check_storage_overflow(v, live_check_storage_overflow_calls(), player);
}


} // namespace mh::sim


namespace mh::sim {
} // namespace mh::sim
