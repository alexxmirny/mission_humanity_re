//
// sim/sim_unit_select_weapon.cpp -- see sim_unit_select_weapon.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_unit_select_weapon_0048ba9e.asm), not from the Ghidra .c draft (whose return
// expression is decompiler garbage from a stale register -- see the header).
//
#include "sim/sim_unit_select_weapon.h"

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace detail {

uint8_t unit_select_weapon(const sim_view &v, uint16_t player, int32_t unit_index, uint32_t target_mask) {
    const unit &u = unit_of(v, player, unit_index);

    // (weapon_id != 0 && slot < UNIT_WEAPON_SLOTS), evaluated in exactly that order -- see the
    // header's derivation of the asm's CMP/JZ-then-CMP/JC sequence.
    for (int32_t slot = 0; u.weapons[slot].weapon_id != 0 && slot < UNIT_WEAPON_SLOTS; ++slot) {
        if (u.weapons[slot].enabled_2 != 0) {
            const uint8_t weapon_id = u.weapons[slot].weapon_id;
            const uint8_t target    = v.cfg_weapons[weapon_id].target;
            if ((target_mask & static_cast<uint32_t>(target)) != 0) {
                return UNIT_SELECT_WEAPON_FOUND;
            }
        }
    }
    return UNIT_SELECT_WEAPON_NOT_FOUND;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

uint8_t unit_select_weapon(uint16_t player, int32_t unit_index, uint32_t target_mask) {
    sim_state st = state();
    return detail::unit_select_weapon(st.read, player, unit_index, target_mask);
}


} // namespace mh::sim
