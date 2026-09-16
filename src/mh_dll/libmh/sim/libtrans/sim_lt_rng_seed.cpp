//
// sim/libtrans/sim_lt_rng_seed.cpp -- see sim_lt_rng_seed.h. Translated from the DISASSEMBLY
// (tmp/decomp_lib_trans/llm_strat_rng_seed_ch0_00499fbf.asm,
// tmp/decomp_lib_trans/llm_strat_rng_seed_ch1_00499ff2.asm), not from the Ghidra .c drafts beside
// them.
//
#include "sim/libtrans/sim_lt_rng_seed.h"

#include "ai/ai_state.h"                    // ai_say / trace_budget -- the shared trace sink, not AI state
#include "sim/resid/sim_rng_seed_channel.h" // detail::rng_seed_channel -- the callee both wrappers delegate to

namespace mh::sim {

namespace detail {

void rng_seed_ch0(sim_store &own, uint32_t value) {
    // 0x00499fd7/0x00499fda: EAX is spilled to a stack slot and reloaded -- a plain compiler
    // round-trip, not a transformation; modelled as a direct pass-through of `value`.
    // 0x00499fdd-0x00499fde: PUSH value, PUSH 0 (channel 0) -- __cdecl argument order is (ch, value).
    // No mask, no clamp, no assert here: both preserve-bugs (the missing bounds check on `ch` and the
    // 16-bit seed mask) live in rng_seed_channel, and are its business, not this wrapper's.
    rng_seed_channel(own, 0, value);
}

void rng_seed_ch1(sim_store &own, uint32_t value) {
    // 0x0049a00a/0x0049a00d: same spill-and-reload round-trip as ch0.
    // 0x0049a010-0x0049a011: PUSH value, PUSH 1 (channel 1) -- the ONLY difference from ch0 is this
    // constant. Re-checked against the .asm immediately before writing this line.
    rng_seed_channel(own, 1, value);
}

} // namespace detail

// ---- the public wrappers --------------------------------------------------------------------------

void rng_seed_ch0(uint32_t value) {
    sim_state st = state();
    detail::rng_seed_ch0(st.own, value);
}

void rng_seed_ch1(uint32_t value) {
    sim_state st = state();
    detail::rng_seed_ch1(st.own, value);
}


} // namespace mh::sim
