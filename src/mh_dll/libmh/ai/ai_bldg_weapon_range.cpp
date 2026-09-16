//
// ai/ai_bldg_weapon_range.cpp -- see ai_bldg_weapon_range.h. Translated from the DISASSEMBLY
// (tmp/decomp_ai/llm_strat_ai_building_defense_weapon_range_004d84ba.asm), not from Ghidra's C.
//
// EVERY ABSOLUTE RE-DERIVED against addr/mh_structs.gen.h rather than transcribed:
//   0xc3d2a2 = buildings + 0x2   = buildings[0][0].building_id  (row stride 27300 = 0x6aa4, built by
//              SHL 3 / SUB / SHL 2 / SHL 4 / SUB / SHL 6 / ADD at 0x004d84cd-0x004d84e3; record
//              stride 273 = 0x111, built by SHL 4 / ADD / SHL 4 / ADD at 0x004d84ea-0x004d84f2)
//   0xd9f369 = Building + 0x6e9 = Building[id].weapon_id        (IMUL 0x842 = the Building stride)
//   0xc3a556 = Weapon   + 0x36  = Weapon[id].range_max[0]       (IMUL 0x16c = the Weapon stride)
// so no byte offset and no literal VA appears below (Law 1).
//
// SHADOW ARM. The body writes NOTHING -- the measured matrix row is three cells, all read-only
// (buildings 1 read, Building 1 read, Weapon 1 read) -- so the site's whole verdict is the RETURN
// value, which is exactly why it is worth arming: a return comparison over a function with no
// state to compare is real evidence, unlike a void function with no regions (which is what the
// manifest refuses llm_strat_order_integrity_check for).
//
#include "ai/ai_bldg_weapon_range.h"


namespace mh::ai {
namespace detail {

int32_t building_defense_weapon_range(const ai_view &v, uint32_t player, int32_t building_index) {
    // MOVZX EAX,word ptr [.. + 0xc3d2a2] @0x004d84f7 -- a u16 load, zero-extended.
    const building &b         = building_of(v, player, building_index);
    const uint32_t  bldg_type = (uint32_t)(uint16_t)b.building_id;
    // Unchecked in the original and unchecked here: nothing bounds building_id against
    // BUILDING_TYPE_COUNT before the IMUL 0x842.
    const int32_t weapon_id = v.cfg_buildings[bldg_type].weapon_id;
    // TEST EAX,EAX / JZ 0x004d74d8 @0x004d850a -- the shared epilogue, with EAX still 0.
    if (weapon_id == 0) return 0;
    // `player`, NOT some weapon-local index: the subscript is the first parameter. See the header.
    return v.cfg_weapons[weapon_id].range_max[player];
}

} // namespace detail

int32_t building_defense_weapon_range(int32_t player, int32_t building_index) {
    const ai_state st = state();
    return detail::building_defense_weapon_range(st.read, (uint32_t)player, building_index);
}

// ---- the differential-oracle arm ----------------------------------------------------------------
//
// Nothing to stub -- there is no outward call. Nothing to restore either; the verdict is the return.
//
// WHAT A VACUOUS GREEN LOOKS LIKE HERE. Every call whose building type is unarmed returns 0 down
// the early-out, and so would a translation that had the range lookup wrong entirely. So the arm
// counts the calls that got PAST the weapon_id test (`armed`) and the largest range it returned,
// and those are the numbers to read before the divergence count: `armed == 0` means the site
// verified nothing but "an unarmed building has no range".

} // namespace mh::ai
