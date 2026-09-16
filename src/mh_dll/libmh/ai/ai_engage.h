//
// ai/ai_engage.h -- the AI engage-candidate pipeline, reimplemented (RI-AI / AI0 pilot, batch A).
//
// Three of AI0's four pilot functions. They share one shared TRANSIENT, the engage-candidate
// scratch list: a producer fills it (llm_strat_ai_engage_candidate_add and its nine siblings, all
// still original), then one of these consumers filters it down and commits an attack order. The
// list carries nothing between AI ticks -- llm_strat_ai_unit_passive_engage_tick resets it at four
// points and claims it for its own pass -- which is why it is a `transient` and not part of any
// persistent model.
//
// WHAT THESE THREE ARE FOR, in the order the pipeline runs them:
//   scan_target_list_for_engage_candidates -- PRODUCER: walks the player's tracked-target list and
//                                             offers each hostile, group-matching target.
//   engage_select_and_commit               -- CONSUMER (unit attacker): filter, sort, turret-first,
//                                             pick, commit via llm_strat_ai_commit_attack_order.
//   engage_filter_and_commit_target        -- CONSUMER (packed-ref attacker): the same filter and
//                                             pick, but commits by ENQUEUEING a unit order.
//
// THE TWO CONSUMERS ARE NEAR-TWINS AND ARE DELIBERATELY NOT FACTORED TOGETHER. They differ in four
// places (the turret partition, the commit path, the register the ground/AA predicates live in, and
// the shape of the attacker argument), and three of those four are behavioural. A shared helper
// would have to be parameterised on all of them, which buys nothing and would make a future
// divergence between the two originals invisible.
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

// The logic over an EXPLICIT state, so `net_selftest.exe aitest` can drive it over heap buffers
// with no game and no rig. The wrappers below are these applied to state(); the split costs one
// inlined call.
namespace detail {

// llm_strat_ai_scan_target_list_for_engage_candidates @0x004ed2ad.
// Offers every entry of player's ai_target_list that is (a) flagged with any of 0xa0 in victim_ref,
// (b) a member of an AI group selected by ai_group_mask, and (c) owned by a player this one is
// HOSTILE to. Returns how many were offered.
int32_t scan_target_list_for_engage_candidates(const ai_view &v, const ai_calls &gc, int32_t player,
                                               uint16_t ai_group_mask);

// llm_strat_ai_engage_select_and_commit @0x004edcca. Returns 1 iff an attack order was committed.
bool engage_select_and_commit(const ai_view &v, const ai_store &own, const ai_calls &gc,
                              int32_t player, int32_t unit_index, int32_t use_alt_commit);

// llm_strat_ai_engage_filter_and_commit_target @0x004ede8b. Returns 1 iff an order was enqueued.
int32_t engage_filter_and_commit_target(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                        uint32_t attacker_ref, int32_t context);

} // namespace detail

int32_t scan_target_list_for_engage_candidates(int32_t player, uint16_t ai_group_mask);
bool    engage_select_and_commit(int32_t player, int32_t unit_index, int32_t use_alt_commit);
int32_t engage_filter_and_commit_target(uint32_t attacker_ref, int32_t context);

} // namespace mh::ai
