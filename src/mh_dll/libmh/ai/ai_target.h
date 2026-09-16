//
// ai/ai_target.h -- the AI's tracked-enemy-target roster, writer and one reader (RI-AI / AI1A,
// batch A layer 1).
//
// player_data::ai_target_list is a flat, capped-at-64 array of mh_llm_strat_ai_target_entry (see
// ai_state.h / addr/mh_structs.gen.h). Two functions over it, unrelated to each other -- neither
// calls the other, they only share the roster:
//
//   target_list_add          -- WRITER @0x004d6d67. Appends one tracked target: skips self-owned
//                                targets, dedupes on (position, aggressor_ref), and refuses once the
//                                list is at its cap of 64 -- an EQUALITY test in the original
//                                (`CMP count,0x40 / JZ return`), not a `>=` range check.
//   scan_targets_for_engage  -- READER @0x004ed130. Re-walks the list for one specific target_id
//                                and re-offers every hostile-owned match to the SAME engage-scratch
//                                producer ai_engage.h's three functions consume
//                                (llm_strat_ai_engage_candidate_add).
//   target_list_remove       -- REMOVER @0x004d6e6f (RI-AI / AI1C, batch C layer 1). Swap-remove of
//                                every entry matching the roster's AGGRESSOR half. Its only caller
//                                is llm_strat_ai_notify_object_removed, which calls it once per
//                                active player when any map object stops existing.
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

// The logic over an EXPLICIT state, so net_selftest.exe aitest can drive it over heap buffers with
// no game and no rig. The wrappers below are these applied to state(); the split costs one inlined
// call.
namespace detail {

// llm_strat_ai_scan_targets_for_engage @0x004ed130. Walks player_idx's ai_target_list and offers,
// as an engage candidate, every entry that (a) has any of victim_ref 0xa0 set, (b) whose target_id
// equals this function's target_id argument, and (c) whose owner is a player player_idx is HOSTILE
// to -- the same sign test (`ai_player_relation <= -1`) ai_engage.h's producer uses. Returns how
// many entries were offered.
int32_t scan_targets_for_engage(const ai_view &v, const ai_calls &gc, int32_t player_idx,
                                int32_t target_id);

// llm_strat_ai_target_list_add @0x004d6d67. Appends one entry to player's ai_target_list: victim_index,
// victim_ref (the kind/class bitmask -- fills the entry's `victim_ref` field verbatim), an AI-group
// index looked up only when the player's AI is enabled (else 0), then aggressor_index and
// aggressor_ref. Three early returns, none of which write anything: the target is self-owned
// (aggressor_ref's low nibble == player), an existing entry already matches on (aggressor_index,
// aggressor_ref), or the list is already at its cap of 64. Parameter names match the Ghidra prototype
// and the struct field vocabulary as of finding 2026-08-03-1549-1 (previously target_kind/target_id/
// aggressor_unit_index/position, which lagged the 2026-08-03 struct-field rename).
void target_list_add(const ai_store &own, const ai_calls &gc, int32_t player, int32_t victim_ref,
                     int32_t victim_index, uint32_t aggressor_ref, int32_t aggressor_index);

// llm_strat_ai_target_list_remove @0x004d6e6f. Purges player's ai_target_list of every entry whose
// AGGRESSOR pair matches -- entry.aggressor_index == aggressor_index AND entry.aggressor_ref ==
// aggressor_ref. Three properties of the original that a "sensible" rewrite would lose, all of them
// observable in the compared region and so all of them reproduced verbatim:
//
//   (1) IT MATCHES THE ATTACKER HALF, NOT THE VICTIM HALF. An entry recording that the removed
//       object was itself attacked (victim_index / victim_ref, +0x00 / +0x04) is left in place.
//   (2) IT DOES NOT STOP AT THE FIRST MATCH -- the loop runs to the (live, re-read) count.
//   (3) THE SCAN INDEX ADVANCES OVER THE SWAPPED-IN ENTRY. The INC is on the match path too, so the
//       record moved down from the tail is never tested; with two matching entries the tail one
//       SURVIVES this call. That is the original's behaviour, not a bug to fix.
//
// Returns how many entries it removed. Nothing in the game reads that -- the original returns void
// -- but a shadow arm's counters and the offline tests both need it, and a returned int costs one
// register in a function that already writes the roster it counts.
int32_t target_list_remove(const ai_store &own, int32_t player, uint32_t aggressor_ref,
                           int32_t aggressor_index);

} // namespace detail

int32_t scan_targets_for_engage(int32_t player_idx, int32_t target_id);
void    target_list_add(int32_t player, int32_t victim_ref, int32_t victim_index, uint32_t aggressor_ref,
                        int32_t aggressor_index);
void    target_list_remove(int32_t player, uint32_t aggressor_ref, int32_t aggressor_index);

} // namespace mh::ai
