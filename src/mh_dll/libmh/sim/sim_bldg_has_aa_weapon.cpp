//
// sim/sim_bldg_has_aa_weapon.cpp -- see sim_bldg_has_aa_weapon.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_bldg_has_aa_weapon_004d746c.asm), not from the Ghidra .c draft (the draft
// reads correctly here and was used as a map, but the address arithmetic and field identities were
// re-derived independently from the listing per house rules).
//
#include "sim/sim_bldg_has_aa_weapon.h"


namespace mh::sim {

namespace detail {

int32_t bldg_has_aa_weapon(const sim_view &v, uint32_t player, int32_t building_index) {
    // 0x004d747b-0x004d74ab: building_of() reproduces the player*BUILDINGS_PER_PLAYER-stride +
    // building_index*sizeof(building) address arithmetic exactly.
    const uint16_t building_id = building_of(v, player, building_index).building_id;

    // 0x004d74ab-0x004d74be: weapon_id == 0 means unarmed -- short-circuits to false.
    const int32_t weapon_id = v.cfg_buildings[building_id].weapon_id;
    if (weapon_id == 0) return 0;

    // 0x004d74c2-0x004d74d1: TEST ...,WEAPON_TARGET_AIR -- both guards fall through to the same
    // "return 0" path, so this is a single `&&`, not two independent early-outs.
    if ((v.cfg_weapons[weapon_id].target & WEAPON_TARGET_AIR) != 0) return 1;
    return 0;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

int32_t bldg_has_aa_weapon(uint32_t player, int32_t building_index) {
    const sim_view v = state().read;
    return detail::bldg_has_aa_weapon(v, player, building_index);
}


} // namespace mh::sim
