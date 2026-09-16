//
// ai/ai_bldg_weapon_range.h -- "how far does this building of mine shoot?" (RI-AI / AI1B layer 2).
//
// llm_strat_ai_building_defense_weapon_range @0x004d84ba. 108 bytes, no branch except the one
// early-out, no call, no write: a three-hop lookup
//
//     buildings[player][building_index].building_id
//         -> Building[that id].weapon_id
//             -> Weapon[that id].range_max[player]
//
// and 0 when the building type mounts no weapon (weapon_id == 0). Its single caller is
// llm_strat_ai_score_defense_coverage; the sibling that does the same lookup INLINE is
// llm_strat_ai_turret_threat_rescan, already translated in ai_turret_threat.cpp.
//
// THE SUBSCRIPT ON range_max IS A PLAYER INDEX, and that is the one thing here worth getting
// wrong. `Weapon::range_max` is int[9], not a scalar, and the original adds `player * 4` into the
// Weapon row before the load (MOV EAX,EBX @0x004d8518 with EBX = the first parameter, then
// MOV EAX,dword ptr [EDX + EAX*0x4 + 0xc3a556] @0x004d851a). Same shape as ai_turret_threat.cpp's
// `w.range_max[p]`, and for the same reason -- weapon range is upgraded per player.
//
// THE RETURN PATH IS A SHARED WATCOM EPILOGUE. Both exits JMP/JZ to 0x004d74d8, which belongs to
// another function in the image; it is `return`, not a call. On the weapon_id == 0 exit EAX still
// holds the zero that TEST EAX,EAX @0x004d850a had just examined, so the function returns 0.
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

namespace detail {

// The logic over an EXPLICIT state, so `net_selftest.exe aitest` can drive it over heap buffers
// with no game. Takes no call set: the body makes no outward call at all.
int32_t building_defense_weapon_range(const ai_view &v, uint32_t player, int32_t building_index);

} // namespace detail

int32_t building_defense_weapon_range(int32_t player, int32_t building_index);

} // namespace mh::ai
