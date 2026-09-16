#pragma once
#include <cstdint>

#include "ai/ai_state.h"
// INVENTION_TYPE_UNIT (the cfg_final_struct_Invention::type value this function's Progress scan
// filters for, CMP at 0x004e87cc) IS ALREADY DEFINED, in ai_train_plan.h. This header declared its
// own copy on 2026-08-07 and the two collided at compile time in the selftest TU -- the translator
// had re-derived a name that already existed rather than finding it, which is exactly what brief
// rule 4 ("no new helpers; the shared set is pre-declared") exists to prevent. Include the owner.
#include "ai/ai_train_plan.h" // INVENTION_TYPE_UNIT -- one definition, in the file that named it

namespace mh::ai {

// The reinforcement spawner's own passable test, 0x004e8995-0x004e89a5: accepts {0, 5}. THIS IS NOT
// {PASSABLE_BLOCKED_A, PASSABLE_BLOCKED_B} = {0, 6} from ai_state.h -- a different value set (5, not
// 6) AND the opposite polarity (this test ACCEPTS the pair as spawnable; the footprint scanner
// elsewhere REJECTS its pair as unbuildable). Do not reuse the ai_state.h constants at this call site.
inline constexpr uint8_t REINFORCEMENT_SPAWN_PASSABLE_A = 0;
inline constexpr uint8_t REINFORCEMENT_SPAWN_PASSABLE_B = 5;

// The trim loop's target size (while (count > 4) ...) and its min-score seed (0xf4240 = 1,000,000),
// read off 0x004e88c0 and 0x004e88c6.
inline constexpr int32_t REINFORCEMENT_CANDIDATE_TRIM_MAX = 4;
inline constexpr int32_t REINFORCEMENT_TRIM_SEED          = 1000000;

// The spawned_count cap, 0x004e89cf (CMP ..., 0x64).
inline constexpr int32_t REINFORCEMENT_SPAWN_CAP = 100;

// The two candidate scratch arrays' PHYSICAL stack size (0x200 bytes each = 128 dwords, at
// [EBP-0x220]/[EBP-0x420]). NOT a bound the original enforces -- see ai_reinforcements.cpp's
// uncertainties; this is capacity only, reproduced because the arrays have to be sized *something*.
inline constexpr int32_t REINFORCEMENT_CANDIDATE_SCRATCH_CAP = 128;

// The logic over an EXPLICIT state, so `net_selftest.exe aitest` can drive it over heap buffers with
// no game and no rig. The wrappers below are these applied to state(); the split costs one inlined
// call each.
namespace detail {

// llm_strat_ai_score_reinforcement_unit @0x004d352c (0xdc bytes). PURE: no store, no outward call
// (its one CALL, utils_math_trunc, is inlined via the shared weapon_power_add_and_trunc helper --
// see the .cpp) -- so it takes no `ai_calls` at all, the same shape as
// ai_target_ref_predicates.cpp's target_ref_is_alive.
//
// Sums Unit[unit_proto_id]'s ground-capable weapon power (Weapon[id].target & 1) across up to four
// weapon slots -- THE SCAN STOPS AT THE FIRST EMPTY SLOT, it does not skip and keep going (matches
// unit_squad_firepower_value's break-vs-continue trap in ai_army_milestone.h, same instruction
// shape) -- then scales the total by Unit[unit_proto_id].type using the IDENTICAL infantry-tier
// ladder ai_army_milestone.cpp's unit_squad_firepower_value uses (x2/x3/x4/x5 for the two races'
// infantry tiers 2..5, zero for the heli mother..cargo band, x1 otherwise): re-derived independently
// from this function's own jump targets and confirmed to reuse the same UNIT_TYPE_* constants.
uint32_t score_reinforcement_unit(const ai_view &v, int32_t player, int32_t unit_proto_id);

// llm_strat_ai_create_reinforcement_unit @0x0046d8e6 (0x6c bytes). A two-way branch and nothing
// else: Unit[unit_proto_id].type > 0xe (SIGNED compare, JG) calls gc.unit_create; otherwise
// gc.unit_create_soldier. Both register loads for `unit_proto_id`/`player` are MOVZX from 16-bit
// stack slots, so both are TRUNCATED to 16 bits before the call, and both calls get the literal `1`
// as their fifth (stack) argument.
void create_reinforcement_unit(const ai_view &v, const ai_calls &gc, uint32_t x, uint32_t y,
                               uint32_t unit_proto_id, uint16_t player);

// llm_strat_ai_invasion_spawn_reinforcements @0x004e8773 (0x2ac bytes). Per-player reinforcement
// spawner for an invasion-force-only AI (player_data::ai_invasion_force != 0):
//
//  1. Bails with 0 if player_data::ai_invasion_points is already <= 0 (SIGNED gate).
//  2. Scans Progress[0 ..= cfg_progress_sec->total] (INCLUSIVE bound, JBE) for rows that are (a) an
//     INVENTION_TYPE_UNIT row, (b) available to this player (progress_of(...).available != 0), (c)
//     score nonzero via score_reinforcement_unit -- called ONCE here as a filter -- and (d) not one
//     of the six heli mother/cargo/shuttle types (both races; written as the
//     [UNIT_TYPE_A_HELI_MOTHER, UNIT_TYPE_H_HELI_CARGO] range, which is exactly the six values the
//     original tests as six individual equalities). Accepted candidates are re-scored a SECOND time
//     (score_reinforcement_unit called again) to fill the stored score -- see the .cpp's
//     uncertainties for why this double call is preserved rather than deduplicated.
//  3. If nothing survived the scan: zeroes ai_invasion_points and returns 0. (The original also
//     calls the proven-no-op llm_debug_log_msg_stub here; we no longer do -- SIMABI-HOOKS.)
//  4. Otherwise trims the candidate set down to REINFORCEMENT_CANDIDATE_TRIM_MAX by repeatedly
//     swap-removing the lowest-scoring entry -- see the .cpp for the UNINITIALISED MIN-INDEX finding
//     (the acknowledged R3 finding; preserved, not guarded).
//  5. Spawns reinforcements one at a time, round-robining through the trimmed candidates, each at the
//     first PASSABLE tile a persistent, NEVER-RESET spiral cursor finds outward from the player's
//     home tile (see the .cpp: the cursor survives across spawn attempts, and its offset index has no
//     bound against the spiral table's extent). Stops when ai_invasion_points reaches 0 (ordinary
//     exit, returns the spawn count, no zeroing) or when 100 units have spawned (zeroes
//     ai_invasion_points, returns 100).
int32_t invasion_spawn_reinforcements(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                      int32_t player);

} // namespace detail

uint32_t score_reinforcement_unit(int32_t player, int32_t unit_proto_id);
void     create_reinforcement_unit(uint32_t x, uint32_t y, uint32_t unit_proto_id, uint16_t player);
int32_t  invasion_spawn_reinforcements(int32_t player);

namespace detail {
} // namespace detail

} // namespace mh::ai
