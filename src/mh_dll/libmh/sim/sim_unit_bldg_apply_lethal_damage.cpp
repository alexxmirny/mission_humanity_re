//
// sim/sim_unit_bldg_apply_lethal_damage.cpp -- see sim_unit_bldg_apply_lethal_damage.h. Translated
// from the DISASSEMBLY (tmp/decomp/llm_unit_bldg_apply_lethal_damage_0049aa6e.asm), not from the
// Ghidra .c draft (the draft's shape/plate agree and are quoted in the header for evidence, but the
// assembly is what was walked address-by-address).
//
#include "sim/sim_unit_bldg_apply_lethal_damage.h"


namespace mh::sim {

namespace detail {

void unit_bldg_apply_lethal_damage(const sim_view &v, sim_store &own, uint32_t target_ref,
                                   int32_t target_index) {
    // 0x0049aa8b-0x0049aa91: player = target_ref & 0xf, computed ONCE and reused, masked, in BOTH
    // arms below -- neither arm ever indexes with the raw target_ref value. Reproduced via
    // sim_state.h's own ref_owner() helper (see the header banner's precedent note).
    const uint32_t player = ref_owner(target_ref);

    // 0x0049aa94-0x0049aa9b: TEST target_ref,0x80; JZ <building label>. Bit 0x80 CLEAR -> building
    // arm; bit 0x80 SET -> unit arm (falls straight through from entry, no jump at all).
    if ((target_ref & 0x80u) != 0u) {
        // ---- unit arm (0x0049aa9d-0x0049aadc) ---------------------------------------------------
        unit &u = own.unit_at(player, target_index);

        // 0x0049aa9d-0x0049aabd: units[player][target_index].unit_proto_id selects the cfg row;
        // 0x0049aaca-0x0049aad6: Unit[unit_proto_id].energy is FLD'd, FADD'd against
        // units[player][target_index].pending_damage, and FSTP'd back -- an ACCUMULATE, not a copy
        // (contrast sim_unit_bldg_energy_refill_full.cpp's plain-copy sibling handler).
        u.pending_damage = v.cfg_units[u.unit_proto_id].energy + u.pending_damage;
    } else {
        // ---- building arm (0x0049aade-0x0049ab1d) -----------------------------------------------
        building &b = own.building_at(player, target_index);

        // 0x0049aade-0x0049ab17: same shape as the unit arm, over buildings[player][target_index]
        // .building_id / Building[building_id].energy / .pending_damage.
        b.pending_damage = v.cfg_buildings[b.building_id].energy + b.pending_damage;
    }
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void unit_bldg_apply_lethal_damage(uint32_t target_ref, int32_t target_index) {
    sim_state st = state();
    detail::unit_bldg_apply_lethal_damage(st.read, st.own, target_ref, target_index);
}


} // namespace mh::sim


namespace mh::sim {
} // namespace mh::sim
