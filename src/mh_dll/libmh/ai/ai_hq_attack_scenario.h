//
// ai/ai_hq_attack_scenario.h -- the scripted HQ-attack scenario trigger (RI-AI batch E).
//
// One function: a ONE-SHOT scripted-scenario kickoff. It turns the strategic AI on globally, spawns
// a dedicated soldier near player 0's landing zone as the scripted HQ-attacker, forces players 0 and
// 1 into a mutual war relation, commits that soldier to attacking the enemy HQ, and latches a
// "done" flag so the scenario cannot re-fire.
//
// NOT SHADOW-ARMED, DELIBERATELY -- see ai_hq_attack_scenario.cpp for why. Every side effect here is
// irreversible (a spawned unit, a diplomacy flip) in a way the shadow arm's restore-between-arms
// design cannot undo, so running both the original and this translation back to back would double
// every one of them. There is no detail::install_shadow_* for this unit.
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

// The logic over an EXPLICIT state, so `net_selftest.exe aitest` can drive it over heap buffers with
// no game and no rig. The wrapper below is this applied to state()/live_calls(); the split costs one
// inlined call.
namespace detail {

// llm_strat_ai_start_hq_attack_scenario @0x004ba7e8.
void start_hq_attack_scenario(const ai_view &v, const ai_store &own, const ai_calls &gc);

} // namespace detail

void start_hq_attack_scenario();

} // namespace mh::ai
