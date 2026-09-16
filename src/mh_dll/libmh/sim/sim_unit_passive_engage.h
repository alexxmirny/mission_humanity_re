//
// sim/sim_unit_passive_engage.h -- the human-controlled ("ai_enabled==0") sibling of the AI's
// active-unit combat tick (RI-SIM / SIM1A).
//
// One function: llm_strat_unit_passive_engage_tick @0x004ee50e (0x243 bytes).
//
// COUNTERPART OF llm_strat_ai_active_unit_tick (mh::ai::active_unit_tick, ai/ai_active_unit_tick.h,
// RI-AI batch E). Runs once per player per _G_LLM_STRAT_AI_MOVE_PERIOD tick, for EVERY player slot --
// unlike its AI sibling, which only fires for ai_enabled players -- so it is the reason units under
// direct human control still auto-acquire and auto-fire at nearby enemies. Shares the exact same
// entry four-instruction diagonal-relation write as the AI sibling (see the .cpp for the derivation),
// but is otherwise a completely different body: a flat per-unit walk (no AI groups, no scoring pass,
// no swap-removal) that tries, per living unit, a graduated sequence of target-seek strategies via the
// ORIGINAL llm_strat_ai_engage_* helpers, stopping at the first one that commits an attack.
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// ---- the outward calls -----------------------------------------------------------------------
//
// Indirected for the same reason as sim_bldg_finish_order.h's table: a direct mh::call:: inside a
// detail:: body reaches into the live game image, which makes the body untestable by
// net_selftest.exe simtest. All nine are ORIGINAL functions outside this batch (translator-brief
// hazard note on this function: "none of them are in this batch or need reimplementing here") -- no
// stub variant is provided because nothing in this closure is ever reimplemented C++ that would need
// to be excluded from a shadow comparison; live_unit_passive_engage_calls() is the only binder.
struct unit_passive_engage_calls {
    uint32_t (*unit_is_order_pending)(uint32_t player, uint32_t unit_id);             // llm_strat_ai_unit_is_order_pending
    int32_t (*unit_is_idle_or_patrolling)(int32_t player, int32_t unit_index);        // llm_strat_unit_is_idle_or_patrolling
    int32_t (*scan_targets_for_engage)(int32_t player, int32_t target_unit_id);       // llm_strat_ai_scan_targets_for_engage
    int32_t (*target_ref_is_alive)(uint32_t target_ref_packed, int32_t target_index); // llm_strat_ai_target_ref_is_alive
    int32_t (*engage_select_and_commit)(int32_t player, int32_t unit_index,
                                        int32_t use_alt_commit); // llm_strat_ai_engage_select_and_commit
    int32_t (*scan_target_list_for_engage_candidates)(int32_t  player,
                                                      uint16_t ai_group_mask); // llm_strat_ai_scan_target_list_for_engage_candidates
    int32_t (*unit_scan_engage_candidates_in_range)(int32_t player, int32_t unit_id,
                                                    uint32_t target_mask); // llm_strat_ai_unit_scan_engage_candidates_in_range
    int32_t (*engage_filter_and_commit_target)(uint32_t attacker_ref,
                                               uint32_t context);          // llm_strat_ai_engage_filter_and_commit_target
    void (*unit_issue_default_order)(uint32_t player, int32_t unit_index); // llm_strat_unit_issue_default_order
};

const unit_passive_engage_calls &live_unit_passive_engage_calls();

namespace detail {

// llm_strat_unit_passive_engage_tick @0x004ee50e.
//
// ENTRY: player_data[player].ai_player_relation[player] = FOREIGN_BLDG_CHANGE_FLAG ? -1 : 1 -- see
// ai_active_unit_tick.cpp's derivation of the same diagonal-index instructions (the compiler folds
// the self-index into the row-stride multiply); translated independently here per brief rule 4.
//
// ROSTER WALK: count-driven, same idiom as target_list_scan_visible_enemies (ai_scan_visible.cpp) --
// `remaining` seeds from units[player][0]'s raw unit_above bytes (little-endian reassembly),
// `unit_id` starts at 1 and increments every iteration, `remaining` decrements only on an occupied
// slot (unit_proto_id != 0). A slot with proto_id == 0, OR an occupied slot with energy <= 0.0, does
// NOTHING for that iteration -- not even the shared tail below; both are OUTSIDE its guard
// (0x004ee586/0x004ee598, both JZ/JNC straight to the loop increment).
//
// PER-UNIT (proto_id != 0 && !(energy <= 0.0) -- an ORDERED test, so a NaN energy does NOT skip;
// see the .cpp for the x87 FCOMP/JNC derivation, same idiom as sim_bldg_alive.cpp's guard):
//   NOT order-pending: llm_strat_unit_is_idle_or_patrolling -- if idle/patrolling, a graduated
//     target-seek sequence, each step gated on the previous one's engage_select_and_commit failing
//     (use_alt_commit=1 every time): (1) if an existing passive-engage target is recorded, re-scan
//     it, drop it if no longer alive, and try to commit -- COMMITTING HERE JUMPS DIRECTLY TO THE
//     SHARED TAIL, skipping every later step; (2) scan around the unit itself; (3) scan the unit's
//     AI-group target list (ai_group_index); (4) scan a fixed 0xe0 range, result discarded either
//     way. Not idle/patrolling: no-op, falls straight through to the shared tail.
//   order-pending: if engagement_flags&1 (has an active commitment) AND target_ref&0x40 (a building
//     target), re-scan targets and, if any were found, issue the unit's default order. Always falls
//     through to the shared tail afterward, whether or not that inner check ran.
//
// SHARED TAIL (LAB_004ee6d4), reached three ways -- a goto from the passive-engage-target commit
// above, and two plain fallthroughs (the "not idle" no-op, and the end of the order-pending branch):
// if target2_ref == 0 (no secondary target already locked), scan targets, scan the AI-group target
// list, scan a fixed 0xa0 range, then filter-and-commit via the packed-ref path (attacker_ref=player,
// context=unit_id).
//
// _G_LLM_STRAT_AI_ENGAGE_CANDIDATE_SCRATCH_COUNT is a pure transient (translator-brief rule 12): it
// is reset to 0 immediately before every scan_targets_for_engage / scan_target_list_for_engage_
// candidates / unit_scan_engage_candidates_in_range call in this function, and carries nothing
// between calls -- see the .cpp for the six raw-disassembly reset sites (four store an immediate 0,
// two store a register the preceding TEST already proved zero; both are the identical observable
// reset).
//
// EXIT (the only one, the remaining==0 loop-exit): player_data[player].ai_target_list_count = 0.
void unit_passive_engage_tick(const sim_view &v, sim_store &own, const unit_passive_engage_calls &c,
                              int32_t player);

} // namespace detail

// Live wrapper: the logic applied to state() and live_unit_passive_engage_calls(). Matches the
// original's committed __watcall(EAX) shape (sig_llm_strat_unit_passive_engage_tick).
void unit_passive_engage_tick(int32_t player);

namespace detail {
} // namespace detail

} // namespace mh::sim
