#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_strat_rng_seed_channel @0x004b4ce6. Writes _G_LLM_STRAT_RNG_STATE[ch] = value & 0xffff
// through `own`. No callees (verified against the .asm -- body is load, mask, store, return; the
// leading PUSH EBP/MOV EBP,ESP/PUSH EAX/PUSH EBX frame setup is ordinary prologue/callee-save, not
// a call to assert_stack_capacity or anything else). void return, matching the original.
void rng_seed_channel(sim_store &own, int32_t ch, uint32_t value);

} // namespace detail

// Live wrapper: the logic applied to state().own.
void rng_seed_channel(int32_t ch, uint32_t value);

} // namespace mh::sim
