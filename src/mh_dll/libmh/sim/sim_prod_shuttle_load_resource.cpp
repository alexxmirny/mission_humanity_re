//
// sim/sim_prod_shuttle_load_resource.cpp -- see sim_prod_shuttle_load_resource.h. Translated from
// the DISASSEMBLY (tmp/decomp_sim/llm_prod_shuttle_load_resource_0048e3bb.asm), not from the Ghidra
// .c draft.
//
#include "sim/sim_prod_shuttle_load_resource.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const prod_shuttle_load_resource_calls &live_prod_shuttle_load_resource_calls() {
    static const prod_shuttle_load_resource_calls c = {
        MH_LIBMH_BIND(game_SpendResource),
    };
    return c;
}

namespace detail {

int32_t prod_shuttle_load_resource(const sim_view &v, sim_store &own,
                                   const prod_shuttle_load_resource_calls &c, uint32_t player,
                                   int32_t building_index, uint32_t resource_id, uint32_t cap) {
    // `player`/`resource_id`/`cap` are re-loaded via a 16-bit zero-extend at every use site in the
    // original -- one local each here, matching sim_bldg_pay_costs.cpp's `p` local for the identical
    // repeated-MOVZX-WORD pattern. `building_index` is NOT truncated (plain 32-bit IMUL throughout),
    // so it is passed through unmodified.
    const uint16_t p   = static_cast<uint16_t>(player);
    const uint16_t rid = static_cast<uint16_t>(resource_id);
    const uint16_t cp  = static_cast<uint16_t>(cap);

    // 0x0048e3dc-0x0048e3f6: buildings[p][building_index].shuttle_slot.
    const building &b    = building_of(v, p, building_index);
    const uint32_t  slot = b.shuttle_slot;

    // 0x0048e3f9-0x0048e422: Building[buildings[p][building_index].building_id].capacity_2[rid].
    const cfg_building &cb = v.cfg_buildings[b.building_id];

    if (cb.capacity_2[rid] == 0) return 0; // 0x0048e42b-0x0048e438: zero-capacity bail, nothing touched.

    // ONE fetch of the shuttle-slot record, reused for the read below AND the write at the tail --
    // translator-brief hazard note: never re-resolve a mutable region's base mid-function.
    prod_shuttle_slot &s = own.prod_shuttle_slot_at(p, static_cast<int32_t>(slot));

    // 0x0048e43d-0x0048e45f: remaining transport room = cfg capacity - already reserved.
    int32_t qty = cb.capacity_2[rid] - s.resources_reserved[rid];

    // 0x0048e462-0x0048e493: clamp down to the player's actual holdings (JLE skips the clamp, i.e.
    // it fires only when qty is strictly greater than the holdings).
    if (player_resource_of(v, p, rid) < qty) qty = player_resource_of(v, p, rid);

    // 0x0048e496-0x0048e4a3: clamp down to the caller's requested cap (JGE skips the clamp).
    if (static_cast<int32_t>(cp) < qty) qty = static_cast<int32_t>(cp);

    if (qty == 0) return 0; // 0x0048e4a6-0x0048e4ea: clamped-to-zero bail, nothing spent/reserved.

    // 0x0048e4ac-0x0048e4e1: spend + reserve, report success.
    c.spend_resource(p, rid, qty);
    s.resources_reserved[rid] += qty;
    return 1;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

int32_t prod_shuttle_load_resource(uint32_t player, int32_t building_index, uint32_t resource_id,
                                   uint32_t cap) {
    sim_state st = state();
    return detail::prod_shuttle_load_resource(st.read, st.own, live_prod_shuttle_load_resource_calls(),
                                              player, building_index, resource_id, cap);
}


} // namespace mh::sim
