//
// sim/libtrans/sim_lt_rng_draws.cpp -- see sim_lt_rng_draws.h. Translated from the DISASSEMBLY
// (tmp/decomp_lib_trans/llm_rand_below_00499f49.asm, tmp/decomp_lib_trans/llm_rand_below_fx_00499f84.asm),
// not from the Ghidra .c drafts.
//
#include "sim/libtrans/sim_lt_rng_draws.h"

#include <intrin.h> // _ReturnAddress -- C-prime's draw-site tag

#include "ai/ai_state.h"      // ai_say / trace_budget -- the shared trace sink, not AI state
#include "sim/rng_trace.h"    // C-prime: the draw-sequence trace
#include "sim/sim_rng_next.h" // mh::sim::detail::rng_next -- the verified, already-ours PRNG step

namespace mh::sim {

namespace detail {

// llm_rand_below @0x00499f49. Pure binding: channel=0, lo=0, hi=upper_bound. See the header banner
// for the push-order derivation (0x00499f67-0x00499f6a).
int32_t rand_below(sim_store &own, int32_t upper_bound) {
    return rng_next(own, /*channel=*/0, /*lo=*/0, /*hi=*/upper_bound);
}

// llm_rand_below_fx @0x00499f84. Same binding, channel=1 (0x00499fa2-0x00499fa5). The committed
// prototype is int32_t(uint32_t) since the 2026-09-02 re-dump (header banner); rng_next
// takes/returns int32_t, and the cast on the bound is bit-pattern-preserving (two's complement),
// so it does not change which value is produced for any input, including one whose top bit is set.
int32_t rand_below_fx(sim_store &own, uint32_t upper_bound) {
    return rng_next(own, /*channel=*/1, /*lo=*/0, /*hi=*/static_cast<int32_t>(upper_bound));
}

} // namespace detail

// ---- the public wrappers --------------------------------------------------------------------------

int32_t rand_below(int32_t upper_bound) {
    mh::sim::rng_trace_set_site(_ReturnAddress()); // C-prime: the game-logic frame above this wrapper
    sim_state st = state();
    return detail::rand_below(st.own, upper_bound);
}

int32_t rand_below_fx(uint32_t upper_bound) {
    mh::sim::rng_trace_set_site(_ReturnAddress()); // C-prime
    sim_state st = state();
    return detail::rand_below_fx(st.own, upper_bound);
}


} // namespace mh::sim
