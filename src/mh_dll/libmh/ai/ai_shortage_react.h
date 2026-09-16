//
// ai/ai_shortage_react.h -- the resource-shortage build-queue reaction (RI-AI / AI1B, batch B).
//
// The CALLER (llm_strat_ai_plan_construction) decides WHETHER to call this at all, off the shortage
// flags the AI notes references; this function itself never reads a shortage flag. What it does is try
// to queue ONE construction, chosen from a small fixed candidate set, to relieve whichever shortage
// the caller already detected.
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

// The logic over an EXPLICIT state, so `net_selftest.exe aitest` can drive it over heap buffers
// with no game and no rig. The wrapper below is this applied to state(); the split costs one
// inlined call.
namespace detail {

// llm_strat_ai_react_resource_shortage @0x004e571d.
//
// 1. EARLY EXIT (0x004e5741-0x004e5760): if a build of building type (is_alien_race == 0 ? 0x15 :
//    1) is already pending in the AI build queue, return immediately -- neither of the two chances
//    below runs. This 1-vs-0x15 pair is NOT one of the four race pairs ai_state.h names
//    (turret/relay/mine/mother); see the translator's declared_needs.
// 2. THE FOUR-SLOT FALLBACK CHAIN (0x004e5766-0x004e585a) is a FALL-THROUGH chain sharing one queue
//    site, not four independent ifs: each of player_data::ai_resource_shortage_candidates[0..3] is
//    tried in order against its paired ai_score_cat_0x1i, and the first slot for which
//    (already_queued(cand) + score == 0) AND (ai_building_type_available[cand] == 1) is queued via
//    llm_strat_bldg_queue_construction(player, cand, x=-1, y=0) -- at most ONE of the four is ever
//    queued. Slot 4's branch ENCODING is inverted in the assembly relative to slots 1-3 (JNZ-to-skip
//    rather than JZ-to-queue) but tests the identical predicate; this translation applies the same
//    `if (eligible) queue` shape uniformly rather than mirroring the encoding difference.
// 3. THE TAIL (0x004e586a-0x004e58c7) is a SECOND, independent chance, gated on five conditions
//    (ai_score_cat_0x10 != 0, ai_score_cat_0x20 != 0, !already_queued(build_candidate_cat_0x30),
//    ai_score_cat_0x30 == 0, ai_building_type_available[build_candidate_cat_0x30] == 1) against
//    player_data::ai_build_candidate_cat_0x30. It runs UNCONDITIONALLY after step 2 -- whether or
//    not step 2 queued anything -- because the queue call in step 2 falls straight through into this
//    block in the assembly; only the step-1 early exit can skip it.
//
// SLOT 1 AND THE TAIL CAN NEVER BOTH FIRE, and that is a property of the original rather than of
// any particular state: slot 1 is eligible only when `already_queued(cand[0]) + ai_score_cat_0x10
// == 0` (ADD @0x004e5789, JNZ @0x004e578f) and the tail's FIRST gate is `ai_score_cat_0x10 != 0`
// (CMP @0x004e5880, JZ @0x004e5887). The same counter, opposite senses, about 250 bytes apart.
// Slots 2-4 carry no such implication, so they are the only route by which a run observes the
// fall-through from the chain's queue call into the tail. Found by `aitest ->
// test_react_resource_shortage`, whose first draft asserted two constructions where only one is
// reachable -- worth knowing before reading a rig log, because "the tail never ran" on a save whose
// AI keeps firing slot 1 is the expected shape, not a missing branch.
//
// A candidate slot holds -1 when unset (ai_state.h). When it does, this function still indexes
// player_data::ai_building_type_available[-1] exactly as the original does (EBX + EAX*1 + offset
// with EAX = 0xffffffff) -- a one-byte out-of-bounds read the original relies on reading something
// other than 1, not a bound this translation adds or removes.
void react_resource_shortage(const ai_view &v, const ai_calls &gc, int32_t player);

} // namespace detail

void react_resource_shortage(int32_t player);

} // namespace mh::ai
