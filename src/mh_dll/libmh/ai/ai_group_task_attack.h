//
// ai/ai_group_task_attack.h -- the AI unit-group task machine's four ATTACK arms (RI-AI / AI1C
// layer 3), dispatched from llm_strat_ai_group_task_activate's 25-entry table (see
// ai_group_task_machine.cpp's ACTIVATE_ARMS) for task codes 0x15..0x18.
//
// llm_strat_ai_group_task_attack_nearest_defended @0x004ea0d2 (0x397 bytes) -- task 0x15
// llm_strat_ai_group_task_engage_target           @0x004ea072 (0x60  bytes) -- task 0x16
// llm_strat_ai_group_task_drain_reserve_attack     @0x004e9c98 (0x3da bytes) -- task 0x17
// llm_strat_ai_group_task_attack_random_target     @0x004e9a73 (0x225 bytes) -- task 0x18
//
// They share a translation unit because three of the four (all but engage_target) share one shape
// almost verbatim: scan for a hostile victim player, locate a target building of theirs via up to
// four fallback type-groups, sweep a 21x21 tile window around it queuing an engage-target task on
// every enemy AA turret found within it, then PREEMPT the group onto an engage-target task for the
// primary building -- or, on any failure to find a victim/target, enqueue task 8 (recall_home) and
// dequeue. Side by side, a transposed field in one is visible against the other two.
//
// engage_target IS THE ODD ONE OUT and by far the smallest: it has no scan, no fallback, no
// preempt/enqueue -- it just re-validates the group's already-resolved target
// (active_param_a/active_param_b) via target_ref_is_alive and writes resolved_target_ref/_index
// accordingly. IT TAKES ONLY TWO PARAMETERS (player_id, group_index) despite a callee-save PUSH EBX
// near entry that could mislead a decompiler into counting three -- ECX is never referenced in the
// body and the sole call site (the activate dispatcher) leaves an unrelated task_code/byte-offset
// pair in EBX/ECX. Ghidra's own committed prototype carried a phantom third parameter until
// 2026-08-05 (EN v224, ghidra_findings 2026-08-05-1343-1); do not reintroduce it.
//
// TWO THINGS THAT ARE EASY TO MISTRANSLATE FROM GHIDRA'S OWN .c, BOTH FIXED BY READING THE .asm:
//
//  1. THE "TWO-VARIABLE" VICTIM LATCH IS ONE VARIABLE. attack_nearest_defended's and
//     attack_random_target's Ghidra decompile renders the hostile-victim scan with what LOOKS like
//     two SSA variables (`uVar3`/`local_1c` the "latch" and `player_id_00`/`uVar4` the "last
//     scanned"), plus a `player_id_00 = uVar3;` reconciliation stapled on right after the loop. The
//     .asm has only ONE storage location throughout (ESI in attack_random_target, [EBP-0x18] in
//     attack_nearest_defended): `if (latch == -1) latch = p;` on first qualifying, and an
//     UNCONDITIONAL `latch = p` at the score-check break site that is a same-iteration no-op every
//     time it executes (the score test that guards it reads player_data[latch], and latch cannot
//     have changed between being set to p on this iteration and the test three instructions later).
//     So there is exactly one victim variable, matching the assembly, not two reconciled at the end
//     -- translate it that way; do not invent a second local.
//
//  2. drain_reserve_attack's Ghidra decompile renders its four find_nearest_building_of_types calls'
//     query coordinates as `*(int*)(player_data[0].ai_tile_flags_grid + player_id*0x288fc - 0x18)`
//     (and `-0x14`). That is Ghidra anchoring the address off a DIFFERENT field than the other three
//     functions use for the textually-identical value: ai_tile_flags_grid starts at +0x3c within
//     player_data, and 0x3c-0x18 = 0x24 = ai_home_tile_x's own offset (0x3c-0x14 = 0x28 =
//     ai_home_tile_y's). It is decompiler noise, not a different field -- all four calls in all
//     three functions query around player_id's (the TICKING player's) ai_home_tile_x/y, never the
//     victim's home tile. Translate it as the plain field read, like the other three functions do.
//
// THE VICTIM-SELECTION GATE ITSELF DIFFERS BETWEEN drain_reserve_attack AND ITS TWO SIBLINGS.
// attack_nearest_defended / attack_random_target require THREE conditions (alive, hostile, has a
// standing building at buildings[p][0]) before even considering the score test, and use the
// one-variable latch above. drain_reserve_attack has NO buildings[p][0] check and NO latch: it is a
// plain "keep advancing while NOT(alive && hostile) OR the four ai_score_* fields are all zero"
// scan over a single index, verified against 0x004e9d11-0x004e9d8d. Do not add the missing buildings
// check to drain_reserve_attack, and do not add the latch/relatch dance to it -- that asymmetry is
// the original's, not a translation gap.
//
// THE HOME-PLAYER-PLUS-ONE ALIASING (2nd find_nearest_building_of_types / group_collect_buildings_of_types
// call in all three) reads `player_data[victim + 1].ai_housing_candidate_*` rather than
// `player_data[victim].*` -- the same one-slot-forward aliasing ai_state.h documents on the field
// itself (`ai_housing_candidate_heli`'s comment). For victim == MAX_PLAYERS-1 this reads past the
// live player_data array, exactly like the documented `players[p+1]` overrun in ai_store -- both
// `ai_view::players` and `ai_store::players` are raw pointers for exactly this reason. Preserve the
// overrun; do not clamp it.
//
// attack_random_target's victim pick is UNIFORMLY RANDOM (llm_rand_below_ai) over whatever the four
// group_collect_buildings_of_types calls appended to the SHARED _G_LLM_STRAT_AI_BUILDING_CANDIDATE_
// SCRATCH_LIST/_COUNT globals (reset to 0 by this function itself, immediately before the four
// calls -- it is the one direct write this file makes outside of engage_target's resolved_target_*
// fields). attack_nearest_defended / drain_reserve_attack instead each call
// group_find_nearest_building_of_types up to four times, taking the first non-zero result.
//
// DECLARED NEEDS (not yet in ai_state.h/mh_calls.gen.h's ai_calls binding -- see the translator's
// structured report): `ai_calls::group_find_nearest_building_of_types` and
// `ai_calls::group_collect_buildings_of_types` (both already exist as free functions in
// mh_calls.gen.h at 0x004e98a7 / 0x004e99be, just not yet as ai_calls members), plus `ai_view`/
// `ai_store` members backing `_G_LLM_STRAT_AI_BUILDING_CANDIDATE_SCRATCH_LIST` (int32_t[256]) and
// `_G_LLM_STRAT_AI_BUILDING_CANDIDATE_SCRATCH_COUNT` (int32_t) -- no region exists for either yet
// (mh_addrs.gen.h/mh_regions.gen.h), only an EOL adjacency comment naming the LIST symbol.
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

namespace detail {

// Shared by the three scan-target-and-preempt arms (NOT engage_target, which is a two-outcome
// re-validation with no scan). Mirrors the group_hold_report / group_task_report precedent of one
// report type serving several co-located functions in the same translation unit.
struct group_task_attack_report {
    bool    victim_found    = false; // the hostile-victim scan located a player
    int32_t victim_player   = -1;    // that player's index, meaningful only if victim_found
    int32_t drained_units   = 0;     // drain_reserve_attack only: units moved out of ai_groups[3]
    int32_t candidate_count = 0;     // attack_random_target only: scratch entries after the 4 collects
    int32_t target_building = 0;     // attack_nearest_defended/drain_reserve_attack only: the found building index (0 = none)
    int32_t turret_hits     = 0;     // engage tasks enqueued during the 21x21 turret-coverage scan
    bool    preempted       = false; // the engage-target preempt was issued (success path)
    bool    recalled        = false; // the recall_home enqueue + dequeue fallback ran instead
};

// llm_strat_ai_group_task_attack_nearest_defended @0x004ea0d2. Task code 0x15.
group_task_attack_report group_task_attack_nearest_defended(const ai_view &v, const ai_store &own,
                                                            const ai_calls &gc, int32_t player_id,
                                                            int32_t group_index);
// llm_strat_ai_group_task_engage_target @0x004ea072. Task code 0x16. TWO PARAMETERS ONLY -- see the
// header above.
void group_task_engage_target(const ai_view &v, const ai_store &own, const ai_calls &gc,
                              int32_t player_id, int32_t group_index);
// llm_strat_ai_group_task_drain_reserve_attack @0x004e9c98. Task code 0x17.
group_task_attack_report group_task_drain_reserve_attack(const ai_view &v, const ai_store &own,
                                                         const ai_calls &gc, uint32_t player_id,
                                                         int32_t group_index);
// llm_strat_ai_group_task_attack_random_target @0x004e9a73. Task code 0x18.
group_task_attack_report group_task_attack_random_target(const ai_view &v, const ai_store &own,
                                                         const ai_calls &gc, int32_t player_id,
                                                         int32_t group_index);

} // namespace detail

void group_task_attack_nearest_defended(int32_t player_id, int32_t group_index);
void group_task_engage_target(int32_t player_id, int32_t group_index);
void group_task_drain_reserve_attack(uint32_t player_id, int32_t group_index);
void group_task_attack_random_target(int32_t player_id, int32_t group_index);

} // namespace mh::ai
