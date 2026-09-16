//
// sim/sim_prod_shuttle_slot_bind_default.cpp -- see sim_prod_shuttle_slot_bind_default.h. Translated
// from the DISASSEMBLY (tmp/decomp_sim/llm_prod_shuttle_slot_bind_default_0048dea7.asm), not from the
// Ghidra .c draft -- the draft's overall shape reads correctly and is cross-checked against it in the
// header, but its inline comment names the wrong CURRENT symbol for the one outward call (see the
// header's callee note); every field offset, callee, and write order below was independently
// re-derived from the raw opcodes per house rules.
//
#include "sim/sim_prod_shuttle_slot_bind_default.h"

#include "addr/mh_calls.gen.h"  // mh::call::llm_strat_prod_shuttle_slot_release -- bound live below
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const prod_shuttle_slot_bind_default_calls &live_prod_shuttle_slot_bind_default_calls() {
    static const prod_shuttle_slot_bind_default_calls gc = {
        MH_LIBMH_BIND(llm_strat_prod_shuttle_slot_release),
    };
    return gc;
}

namespace detail {

int32_t prod_shuttle_slot_bind_default(const sim_view &v, sim_store &own,
                                       const prod_shuttle_slot_bind_default_calls &c, uint32_t player,
                                       int32_t building_index) {
    // 0x0048debe/0x0048dec1: EAX/EDX stashed in full, but every later use of `player` re-reads
    // through a 16-bit MOVZX -- see the header banner. `building_index` stays a full dword everywhere
    // except the one narrowing store below.
    const uint16_t p16 = (uint16_t)player;

    // 0x0048dec4-0x0048defa: already bound -- no-op, return 0.
    if (building_of(v, p16, building_index).shuttle_slot != 0) return 0;

    // 0x0048defa-0x0048e033: scan slots 1..9 for the first free one (type_ref_id == 0).
    for (int32_t slot = 1; slot < PROD_SHUTTLE_SLOTS_PER_PLAYER; ++slot) {
        if (v.prod_shuttle_slots[p16 * PROD_SHUTTLE_SLOTS_PER_PLAYER + slot].type_ref_id != 0) continue;

        // 0x0048df2e-0x0048df3a: found a free slot -- call the ORIGINAL callee before any write here.
        // See the header's callee note: this is addr/mh_calls.gen.h's CURRENT name for 0x0046318d.
        c.slot_release((int32_t)p16, slot);

        // 0x0048df3a-0x0048e026: the five field writes, in asm order (see header derivation for each
        // displacement). building_id is re-read fresh (post-call), not reused from the guard above.
        const uint16_t building_id = building_of(v, p16, building_index).building_id;

        prod_shuttle_slot &rec = own.prod_shuttle_slot_at(p16, slot);
        rec.type_ref_id        = building_id;                       // 0x0048df67
        rec.src_building_type  = v.cfg_buildings[building_id].type; // 0x0048dfa9
        rec.src_building_index = (int16_t)building_index;           // 0x0048dfc6
        rec.origin_planet      = (int16_t)*v.planet_index;          // 0x0048dfe7
        rec.status             = 0xca;                              // 0x0048e001 ("just bound")

        own.building_at(p16, building_index).shuttle_slot = (uint8_t)slot; // 0x0048e020

        return slot;
    }

    // 0x0048e033: all nine slots occupied.
    return -1;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

int32_t prod_shuttle_slot_bind_default(uint32_t player, int32_t building_index) {
    sim_state st = state();
    return detail::prod_shuttle_slot_bind_default(st.read, st.own, live_prod_shuttle_slot_bind_default_calls(),
                                                  player, building_index);
}


} // namespace mh::sim
