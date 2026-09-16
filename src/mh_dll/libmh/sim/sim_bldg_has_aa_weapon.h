//
// sim/sim_bldg_has_aa_weapon.h -- one building-roster AA-capability predicate (RI-SIM / SIM1D).
//
//   llm_strat_bldg_has_aa_weapon @0x004d746c (0x72)
//
// Committed prototype (addr/mh_export.gen.h's sig_llm_strat_bldg_has_aa_weapon / addr/mh_calls.gen.h's
// trampoline): `int32_t __watcall llm_strat_bldg_has_aa_weapon(uint32_t player, int32_t building_index)`.
//
// Takes ONLY a `sim_view` -- no store, no callees at all beyond the inert stack-probe
// (utils_assert_stack_capacity, translator brief rule 6), same posture as
// sim_bldg_roster_queries.h's four siblings and sim_unit_weapons.h's unit-level counterpart
// (llm_strat_unit_has_aa_weapon @0x004d7311), which this function structurally mirrors one roster
// level up: BUILDING instance -> its single mounted weapon (via the cfg Building TYPE record's
// `weapon_id`), rather than a unit instance's four weapon slots.
//
// ---- THE LOOKUP CHAIN (0x004d746b-0x004d74d8) --------------------------------------------------
//   1. `building_id = buildings[player][building_index].building_id` (0x004d747b-0x004d74ab, the
//      address arithmetic is player*BUILDINGS_PER_PLAYER-stride + building_index*sizeof(building) --
//      cross-checked against `buildings` base 0xc3d2a0 + field offset 0x2 (mh_map_object_building's
//      `building_id` field), so this is exactly `building_of(v, player, building_index).building_id`).
//   2. `weapon_id = Building[building_id].weapon_id` (0x004d74ab-0x004d74b8: IMUL by 0x842 ==
//      sizeof(cfg_final_struct_Building), then a dword load at +0x6e9 -- matches the ALREADY-NAMED
//      `weapon_id` field's own doc comment, which explicitly cites this function as one of its seven
//      readers). weapon_id == 0 means unarmed -> short-circuits to false (0x004d74be-0x004d74c0),
//      matching cfg_building's field comment ("0 = unarmed").
//   3. If nonzero, `Weapon[weapon_id].target & 0x2` (0x004d74c2-0x004d74c8: IMUL by 0x16c ==
//      sizeof(cfg_final_struct_Weapon), TEST byte at +0x1 against WEAPON_TARGET_AIR) -- identical
//      target-mask scheme to llm_strat_unit_has_aa_weapon (sim_unit_weapons.h/.cpp), same
//      WEAPON_TARGET_AIR constant.
//
// Return is a plain 0/1 int (EBX seeded 0, set to 1 only on the AND'd condition, never anything
// else) -- both TEST-then-JZ branches (weapon_id==0 short-circuit, and the target-bit test) fall
// through to the same "return 0" path, so the two guards are a single `&&`, not independent
// early-outs with different reset behaviour.
//
// No floats, no writes, no calls beyond the inert stack probe. The opening `CALL
// utils_assert_stack_capacity` is not reproduced (translator brief rule 6).
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {
namespace detail {

// llm_strat_bldg_has_aa_weapon @0x004d746c. True iff buildings[player][building_index]'s cfg
// Building TYPE record mounts a weapon (weapon_id != 0) whose cfg Weapon target mask has the
// WEAPON_TARGET_AIR bit set. See the header banner for the full derivation.
int32_t bldg_has_aa_weapon(const sim_view &v, uint32_t player, int32_t building_index);

} // namespace detail

// Live wrapper: the logic applied to state().read. Matches the committed prototype
// (sig_llm_strat_bldg_has_aa_weapon) exactly.
int32_t bldg_has_aa_weapon(uint32_t player, int32_t building_index);

namespace detail {
} // namespace detail

} // namespace mh::sim
