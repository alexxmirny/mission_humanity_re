//
// sim/sim_bldg_refresh_all_buildings.cpp -- see sim_bldg_refresh_all_buildings.h. Translated from
// the DISASSEMBLY (tmp/decomp/llm_strat_refresh_all_buildings_00470b56.asm), not from the Ghidra .c
// draft (which reads close to the asm here, modulo the count local's signedness -- see the header).
//
#include "sim/sim_bldg_refresh_all_buildings.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const refresh_all_buildings_calls &live_refresh_all_buildings_calls() {
    static const refresh_all_buildings_calls gc = {
        MH_LIBMH_BIND(llm_strat_refresh_building),
    };
    return gc;
}

namespace detail {

void refresh_all_buildings(const sim_view &v, const refresh_all_buildings_calls &gc, uint32_t player) {
    // 0x00470b6e/0x00470b71: the full 32-bit param is stashed, but every subsequent use re-reads it
    // through a 16-bit MOVZX -- only the low 16 bits ever participate (matches the Ghidra .c draft's
    // `player & 0xffff`).
    const uint16_t p16 = (uint16_t)player;

    // 0x00470b71-0x00470b82: buildings[p16][0].index (mh_map_object_building::index, int16_t),
    // zero-extended (MOVZX, not MOVSX) into a 32-bit countdown of remaining occupied slots.
    const int32_t initial_live_count = (uint16_t)building_of(v, p16, 0).index;
    int32_t       live_count         = initial_live_count;

    // 0x00470b85: slot cursor starts at 1 -- slot 0 is the count field just read, never itself
    // treated as an occupied building.
    int32_t slot = 1;

    // 0x00470b8c-0x00470b96: two ANDed exit conditions, either one ends the walk -- `slot < 100`
    // (BUILDINGS_PER_PLAYER) and `live_count > 0` (SIGNED compare, JG). live_count is decremented
    // only on a hit (0x00470bbf-0x00470bc2), so an inaccurate `.index` count can drive it negative;
    // that is preserved, not clamped -- see the header banner.
    while (slot < v.caps.buildings && live_count > 0) {
        // 0x00470ba2-0x00470bbd: buildings[p16][slot].index, re-read every iteration (translator
        // brief rule 16: `const` on sim_view is not a promise of stability), compared to zero.
        if (building_of(v, p16, slot).index != 0) {
            // 0x00470bbf-0x00470bc2: countdown decrement, then (0x00470bc5-0x00470bcc) the call --
            // EDX=slot loaded BEFORE EAX=player, but both are read before the CALL so evaluation
            // order is not observable here.
            --live_count;
            gc.refresh_building(p16, slot);
        }
        ++slot;
    }
}

} // namespace detail

// ---- the public surface --------------------------------------------------------------------------

void refresh_all_buildings(uint32_t player) {
    const sim_view v = state().read;
    detail::refresh_all_buildings(v, live_refresh_all_buildings_calls(), player);
}


} // namespace mh::sim


namespace mh::sim {
} // namespace mh::sim
