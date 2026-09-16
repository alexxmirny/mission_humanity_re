//
// sim/sim_unit_weapons.cpp -- see sim_unit_weapons.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_unit_has_aa_weapon_004d7311.asm, _has_ground_weapon_004d7377.asm,
// _max_weapon_range_004d480f.asm), not from Ghidra's C drafts.
//
#include "sim/sim_unit_weapons.h"


namespace mh::sim {
namespace detail {

int32_t unit_has_weapon_vs(const sim_view &v, uint32_t unit_ref, int32_t unit_index,
                           uint8_t target_mask) {
    const uint32_t player = ref_owner(unit_ref); // AND ECX,0xf @0x004d7322
    const unit    &u      = unit_of(v, player, unit_index);

    // SIGNED bound in the original (INC EDX / CMP EDX,4 / JL @0x004d736c) -- see the header.
    for (int32_t i = 0; i < UNIT_WEAPON_SLOTS; ++i) {
        const unit_weapon &w = u.weapons[i];
        if (w.enabled_2 == 0) continue;                    // 0x004d733a
        if (w.weapon_id == 0) continue;                    // 0x004d7343
        const cfg_weapon &cw = v.cfg_weapons[w.weapon_id]; // IMUL 0x16c @0x004d7353
        if ((cw.target & target_mask) != 0) return 1;      // TEST ..,mask @0x004d7359
    }
    return 0;
}

uint32_t unit_max_weapon_range(const sim_view &v, uint32_t unit_ref, int32_t unit_index) {
    const uint32_t player = ref_owner(unit_ref); // AND EAX,0xf @0x004d481f
    // MOVZX off units[player][unit_index] + 0x2 @0x004d482e -- the unit's TYPE, and the only thing
    // this function reads from the instance.
    const uint16_t  proto = unit_of(v, player, unit_index).unit_proto_id;
    const cfg_unit &cu    = v.cfg_units[proto];

    uint32_t best = 0; // XOR ECX,ECX @0x004d4836
    // UNSIGNED bound in the original (CMP EDX,4 / JC @0x004d487c) -- see the header.
    for (uint32_t i = 0; i < (uint32_t)CFG_UNIT_WEAPON_SLOTS; ++i) {
        if (cfg_unit_weapon_enabled(cu, (int32_t)i) == 0) continue; // 0x004d484d
        const uint8_t     id = cfg_unit_weapon_id(cu, (int32_t)i);  // 0x004d4856
        const cfg_weapon &cw = v.cfg_weapons[id];                   // IMUL 0x16c @0x004d485d
        // range_max is per-player and the compare is UNSIGNED (CMP ECX,.. / JNC @0x004d486d).
        const uint32_t r = (uint32_t)cw.range_max[player];
        if (best < r) best = r;
    }
    return best;
}

} // namespace detail

int32_t unit_has_aa_weapon(uint32_t unit_ref, int32_t unit_index) {
    const sim_view v = state().read;
    return detail::unit_has_aa_weapon(v, unit_ref, unit_index);
}

int32_t unit_has_ground_weapon(uint32_t unit_ref, int32_t unit_index) {
    const sim_view v = state().read;
    return detail::unit_has_ground_weapon(v, unit_ref, unit_index);
}

uint32_t unit_max_weapon_range(uint32_t unit_ref, int32_t unit_index) {
    const sim_view v = state().read;
    return detail::unit_max_weapon_range(v, unit_ref, unit_index);
}


} // namespace mh::sim
