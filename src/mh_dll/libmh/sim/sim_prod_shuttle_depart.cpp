//
// sim/sim_prod_shuttle_depart.cpp -- see sim_prod_shuttle_depart.h. Translated from the DISASSEMBLY
// (tmp/decomp_sim/llm_prod_shuttle_depart_0048e16a.asm), not from the Ghidra .c draft.
//
#include "sim/sim_prod_shuttle_depart.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const prod_shuttle_depart_calls &live_prod_shuttle_depart_calls() {
    static const prod_shuttle_depart_calls c = {
        MH_LIBMH_BIND(llm_strat_unit_load_into_shuttle_cargo),
        MH_LIBMH_BIND(llm_prod_bldg_depart_finalize),
    };
    return c;
}

namespace detail {

int32_t prod_shuttle_depart(const sim_view &v, sim_store &own, const prod_shuttle_depart_calls &c,
                            uint16_t player, int32_t building_index, int32_t dest_planet) {
    // 0x0048e189-0x0048e1a3: sub_id read ONCE, cached.
    const uint8_t sub_id = building_of(v, static_cast<uint32_t>(player), building_index).sub_id;

    // 0x0048e1a6-0x0048e1bf: docked_count read ONCE, cached -- the loop condition below compares
    // against this cached value, not a fresh memory read each pass (see header banner).
    const int32_t docked_count = storage_of(v, static_cast<uint32_t>(player), sub_id).docked_count;

    // 0x0048e1c9-0x0048e20d: detach every docked unit. docked_units[0] is reloaded FRESH every
    // iteration (0x0048e1e2-0x0048e1f5) -- always index 0, on the assumption the callee shifts the
    // list down by one on success. The dead accumulator [EBP-0x14] (ADD dword ptr [EBP-0x14],EAX
    // after each call) is a genuine store in the assembly that is never read again anywhere in the
    // function -- verified dead, not reproduced (see header banner).
    for (int32_t i = 0; i < docked_count; ++i) {
        const uint16_t unit_id = static_cast<uint16_t>(
            storage_of(v, static_cast<uint32_t>(player), sub_id).docked_units[0]);
        c.unit_load_into_shuttle_cargo(player, building_index, unit_id);
    }

    // 0x0048e242-0x0048e24b: off-planet departure only. The discarded CMP at 0x0048e239-0x0048e23c
    // (dead accumulator vs. passengers_reserved) has no effect on this branch -- its flags are
    // clobbered before any Jcc reads them (see header banner) -- so only the dest_planet !=
    // G_PLANET_INDEX test gates entry.
    if (dest_planet != *v.planet_index) {
        // 0x0048e254-0x0048e29d: 50 cargo-manifest entries, stride 14 bytes, clear the upper nibble
        // of each entry's second byte. shuttle_slot is recomputed fresh every iteration
        // (0x0048e264-0x0048e27e), matching the assembly literally.
        for (int32_t i = 0; i < 50; ++i) {
            const uint8_t shuttle_slot =
                building_of(v, static_cast<uint32_t>(player), building_index).shuttle_slot;
            uint8_t &entry_byte =
                own.prod_shuttle_slot_at(player, shuttle_slot).cargo_manifest_raw[i * 14 + 1];
            entry_byte &= 0xf;
        }
    }

    // 0x0048e29f-0x0048e2ae: delegate, return value discarded.
    c.prod_bldg_depart_finalize(player, building_index, dest_planet);

    // 0x0048e2ae-0x0048e2b8: unconditional, not derived from anything computed above.
    return 1;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

int32_t prod_shuttle_depart(uint16_t player, int32_t building_index, int32_t dest_planet) {
    sim_state st = state();
    return detail::prod_shuttle_depart(st.read, st.own, live_prod_shuttle_depart_calls(), player,
                                       building_index, dest_planet);
}


} // namespace mh::sim
