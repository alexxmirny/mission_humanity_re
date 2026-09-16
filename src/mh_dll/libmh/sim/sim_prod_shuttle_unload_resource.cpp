#include "sim/sim_prod_shuttle_unload_resource.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const prod_shuttle_unload_resource_calls &live_prod_shuttle_unload_resource_calls() {
    static const prod_shuttle_unload_resource_calls c = {
        MH_LIBMH_BIND(llm_resource_add),
        MH_LIBMH_BIND(llm_strat_bldg_shuttle_slot_is_free),
        MH_LIBMH_BIND(llm_strat_bldg_flush_cargo_hold),
    };
    return c;
}

namespace detail {

uint8_t prod_shuttle_unload_resource(const sim_view &v, sim_store &own,
                                     const prod_shuttle_unload_resource_calls &c, uint16_t player,
                                     int32_t building_index, uint16_t resource_id, int32_t cap) {
    // 0x0048e51c-0x0048e536: buildings[player][building_index].shuttle_slot.
    const building &b            = building_of(v, player, building_index);
    const int32_t   shuttle_slot = static_cast<int32_t>(b.shuttle_slot);

    // ONE fetch of the shuttle-slot record, reused for the read below AND the decrement at the tail
    // -- same "never re-resolve a mutable region's base mid-function" hazard note the load-side
    // sibling (sim_prod_shuttle_load_resource.cpp) documents; the original recomputes the address a
    // second time but against the IDENTICAL player/shuttle_slot/resource_id key, so this is
    // behaviourally identical.
    prod_shuttle_slot &s = own.prod_shuttle_slot_at(player, shuttle_slot);

    // 0x0048e55e-0x0048e574: amount = (cap == -1) ? reserved : min(cap, reserved).
    // `amount` starts as the full reserved quantity; cap==-1 is the "unload everything" sentinel
    // (keeps it unchanged); otherwise it is clamped down to cap only when cap is STRICTLY LESS than
    // the reserved amount (signed JL) -- when cap >= reserved, the reserved amount is kept as-is.
    int32_t amount = s.resources_reserved[resource_id];
    if (cap != -1 && cap < amount) amount = cap;

    if (amount == 0) return 0; // 0x0048e574-0x0048e5d4: nothing reserved / clamped to zero -- bail.

    // 0x0048e57a-0x0048e585: llm_resource_add(player, resource_id, amount) (ORIGINAL, via `c`).
    c.resource_add(static_cast<int32_t>(player), static_cast<int32_t>(resource_id), amount);

    // 0x0048e58a-0x0048e5a9: decrement the same reserved counter by the credited amount.
    s.resources_reserved[resource_id] -= amount;

    // 0x0048e5af-0x0048e5c6: if the shuttle slot is now free, flush the cargo hold. Both callees are
    // SIBLING migrated functions in this ring -- called through the ORIGINAL binary here, per Law 4 /
    // the ring's ONE RULE (tmp/ring_context.md), never through mh::sim::.
    const int32_t slot_free = c.bldg_shuttle_slot_is_free(static_cast<int32_t>(player), building_index);
    if (slot_free > 0) c.bldg_flush_cargo_hold(static_cast<uint32_t>(player), building_index);

    return 1; // 0x0048e5cb: credited a nonzero amount back.
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

uint8_t prod_shuttle_unload_resource(uint16_t player, int32_t building_index, uint16_t resource_id,
                                     int32_t cap) {
    sim_state st = state();
    return detail::prod_shuttle_unload_resource(st.read, st.own,
                                                live_prod_shuttle_unload_resource_calls(), player,
                                                building_index, resource_id, cap);
}


} // namespace mh::sim
