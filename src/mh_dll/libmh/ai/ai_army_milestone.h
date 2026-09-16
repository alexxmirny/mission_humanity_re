//
// ai/ai_army_milestone.h -- the per-player army-attack-milestone progression (RI-AI / AI1A layer 2).
//
// One function: llm_strat_ai_army_milestone_advance_or_attack @0x004e73b3. Per AI tick it either (a)
// bails outright if a group of this player already has an attack-in-progress goal, (b) waits for
// enough of the AI's own clock to elapse since the last milestone, (c) once elapsed, compares the
// firepower already assembled in ai_groups[2]+[0] against the milestone's required strength and, if
// short, just advances the milestone/timer, or (d) if strong enough, picks the weakest surviving
// hostile opponent, forms a brand-new attack group out of whatever ai_groups[2]/[0] members are
// available (moving them into it as it goes), enqueues the attack-move task, and advances the
// milestone/timer regardless of how small the assembled force ended up being.
//
// THIS IS A SHARED-STATE WRITER, not a pure reader: it writes player_data::ai_attack_milestone_index
// / _clock on every reachable path, and additionally writes a freshly-created ai_groups[] slot's
// goal/target_player_id when it decides to attack. See ai_state.h's ai_store comment for why
// player_data is the AI's own store and not an R2 violation.
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

// The logic over an EXPLICIT state, so `net_selftest.exe aitest` can drive it over heap buffers with
// no game and no rig. The wrapper below is this applied to state(); the split costs one inlined call.
namespace detail {

// llm_strat_ai_army_milestone_advance_or_attack @0x004e73b3.
//
// player_data::ai_clock is the AI's OWN time base (a float, seeded 0.001f at base spawn), NOT the
// game clock -- see ai_state.h. ai_attack_milestone_clock / _index were ai_build_plan[0x23]/[0x24]
// until 2026-08-01; this file uses the now-named fields.
void army_milestone_advance_or_attack(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                      uint32_t player);

// llm_strat_ai_unit_squad_firepower_value @0x004d3437 -- the per-unit term the milestone comparison
// above sums. PURE: no store anywhere in its 245 bytes, so its shadow verdict is its RETURN VALUE.
//
// Sums the unit's ANTI-PERSONNEL weapon power (`Weapon[id].target & 1`) across its four weapon
// slots, then scales the total by the unit's Unit[proto].type. Both halves have a trap:
//
//  * THE SLOT LOOP STOPS AT THE FIRST EMPTY SLOT -- it does NOT skip and keep going. At 0x004d346c a
//    zero weapon_id jumps to 0x004d34ac, which is PAST the loop, while a weapon that fails the
//    anti-personnel test at 0x004d3482 jumps to 0x004d34a6, which is the `INC EDX` continue. Two
//    different exits from one loop body, and the difference is observable whenever slot 0 is empty
//    and a later slot is not.
//    Its byte-similar sibling llm_strat_ai_group_pick_best_weapon_unit @0x004d6c85 does the
//    OPPOSITE: its zero-id branch at 0x004d6cf1 targets 0x004d6d2b, the `INC EDX`, so that one
//    CONTINUES. Same loop shape, same weapon test, same x87 tail, different control flow -- the
//    pool3/pool4 lesson in a second family. Do not copy one into the other.
//
//  * THE TYPE LADDER IS UNSIGNED and reads Unit[proto].type as a DWORD (0x004d34cc). Multipliers,
//    read off the jump targets rather than the decompile's enum names: A_INFANTRY_2..5 (2..5) and
//    H_INFANTRY_2..5 (7..0xa) scale x2/x3/x4/x5 respectively; the whole heli mother..cargo band
//    [0x13,0x18] scales to ZERO; every other type -- including 6, and 0/1 -- is x1.
//
// The accumulator is UNSIGNED 32-bit and round-trips through x87 once per counted weapon; see
// weapon_power_add_and_trunc, which is shared with the sibling above because the two originals'
// FILD/FADD/trunc/FISTP tails are instruction-for-instruction identical.
uint32_t unit_squad_firepower_value(const ai_view &v, int32_t player, int32_t unit_idx);

} // namespace detail

void army_milestone_advance_or_attack(uint32_t player);

uint32_t unit_squad_firepower_value(int32_t player, int32_t unit_idx);

} // namespace mh::ai
