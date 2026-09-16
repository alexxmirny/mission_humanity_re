//
// ai/ai_build_plan_push.h -- append one entry to a player's precomputed build-order plan (RI-AI).
//
// One function: the append side of player_data::ai_build_plan[] / ai_build_plan_len_and_flag. The
// consume side (ai_build_plan_cursor / the >>31-masked length compare) lives in
// llm_strat_ai_plan_construction (ai_construction_plan.cpp) and is not touched here.
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

// The logic over an EXPLICIT state, so `net_selftest.exe aitest` can drive it over heap buffers
// with no game and no rig. The wrapper below is this applied to state(); the split costs one
// inlined call.
namespace detail {

// llm_strat_ai_build_plan_push @0x004dc781.
//
// No-op when ai_build_id == -1 (the sentinel). Otherwise appends ai_build_id at
// player_data[player].ai_build_plan[len] (len read out of the low 31 bits of
// ai_build_plan_len_and_flag -- bit 0x80000000 of that same dword is an UNRELATED ring/spiral-scan
// flag the original never masks off before indexing, because multiplying the raw dword by 4 for the
// byte offset shifts that bit out of the 32-bit result anyway; this translation masks explicitly
// instead of relying on that overflow, per the same idiom llm_strat_ai_plan_construction's own
// consumer-side comparison already uses) and increments the WHOLE dword (count and flag together,
// matching the original's `INC dword ptr`, not just the low 31 bits) by one.
//
// No cap check against the array's 32-entry extent, matching the original -- see the field comment
// on ai_build_plan[] in mh_structs.gen.h ("<=6 entries seen in practice").
void build_plan_push(const ai_store &own, int32_t player, int32_t ai_build_id);

} // namespace detail

void build_plan_push(int32_t player, int32_t ai_build_id);

} // namespace mh::ai
