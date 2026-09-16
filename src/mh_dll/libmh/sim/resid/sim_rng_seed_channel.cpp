//
// sim/resid/sim_rng_seed_channel.cpp -- see sim_rng_seed_channel.h. Translated from the
// DISASSEMBLY (tmp/decomp_sim_resid/llm_strat_rng_seed_channel_004b4ce6.asm), the Ghidra .c being
// a draft.
//
#include "sim/resid/sim_rng_seed_channel.h"

namespace mh::sim {

namespace detail {

// ---- llm_strat_rng_seed_channel @0x004b4ce6 --------------------------------------------------
void rng_seed_channel(sim_store &own, int32_t ch, uint32_t value) {
    // 0x004b4ceb: EBX = ch (param 1, [EBP+0x8])
    // 0x004b4cee-0x004b4cf1: EAX = value (param 2, [EBP+0xc]) & 0xffff -- PRESERVE-BUG, see header:
    // the high 16 bits of `value` are discarded unconditionally.
    // 0x004b4cf6: _G_LLM_STRAT_RNG_STATE[ch] = EAX -- PRESERVE-BUG, see header: `ch` is used as a
    // bare scaled index with no bounds check anywhere in the body.
    own.rng_state_at(static_cast<uint32_t>(ch)) = value & 0xffffu;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void rng_seed_channel(int32_t ch, uint32_t value) {
    sim_state st = state();
    detail::rng_seed_channel(st.own, ch, value);
}

} // namespace mh::sim
