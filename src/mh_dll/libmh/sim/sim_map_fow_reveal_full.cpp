//
// sim/sim_map_fow_reveal_full.cpp -- see sim_map_fow_reveal_full.h. Translated from the
// DISASSEMBLY (tmp/decomp/llm_map_fow_reveal_full_0049ab26.asm), which agrees with the Ghidra .c
// draft's overall shape here (unlike several sibling functions this slice, this one's local naming
// is not misleading) -- kept as the assembly citation regardless, per the standing rule that the
// asm is the spec.
//
#include "sim/sim_map_fow_reveal_full.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const map_fow_reveal_full_calls &live_map_fow_reveal_full_calls() {
    static const map_fow_reveal_full_calls c = {
        MH_LIBMH_BIND(map_fow_UpdateFoWPlus),
    };
    return c;
}

namespace detail {

void map_fow_reveal_full(const sim_view &v, const map_fow_reveal_full_calls &c, uint32_t player) {
    // 0x0049ab41-0x0049ab8c: nested stride-6 walk over the whole map, outer=x bounded by `width`
    // (0x00825084, sim_view::map_width), inner=y bounded by `height` (0x00825064,
    // sim_view::map_height) -- see the header's register-provenance note for why outer pairs with
    // width/x and inner with height/y. Sight radius is the literal 10 every call.
    for (int32_t x = 0; x < *v.map_width; x += 6) {
        for (int32_t y = 0; y < *v.map_height; y += 6) {
            c.fow_update_fow_plus(player, static_cast<uint32_t>(x), static_cast<uint32_t>(y), 10);
        }
    }
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void map_fow_reveal_full(uint32_t player) {
    const sim_view v = state().read;
    detail::map_fow_reveal_full(v, live_map_fow_reveal_full_calls(), player);
}


} // namespace mh::sim
