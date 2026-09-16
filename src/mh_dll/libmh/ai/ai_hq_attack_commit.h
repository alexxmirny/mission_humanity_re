#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

// The logic over an EXPLICIT state, so `net_selftest.exe aitest` can drive it over heap buffers with
// no game and no rig. The wrapper below is this applied to state()/live_calls(); the split costs one
// inlined call.
namespace detail {

// llm_strat_ai_unit_commit_attack_on_enemy_hq @0x004ba6fc.
void unit_commit_attack_on_enemy_hq(const ai_view &v, const ai_store &own, const ai_calls &gc);

} // namespace detail

void unit_commit_attack_on_enemy_hq();

} // namespace mh::ai
