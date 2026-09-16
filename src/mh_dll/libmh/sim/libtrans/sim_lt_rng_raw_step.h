#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_rand_prng_tick_slot @0x004b4cc0, ADOPTED (see the header banner -- not a callee thunk). The
// state half of the rotate-mix LCG and NOTHING else: state[slot] = ROR16(state[slot] + 0x9248, 3);
// returns the NEW value, zero-extended (0..0xffff). No lo/hi scaling -- each caller below supplies its
// own. `slot` is used exactly as the original's raw (unchecked) index arithmetic; no bounds check
// here either.
uint32_t rng_tick_slot(sim_store &own, int32_t slot);

// llm_rand_below_ai @0x004d4063. Ticks channel 2 (hardcoded, never a parameter) then scales
// `(state' * range) >> 16` -- see the header banner for why this is NOT
// detail::rng_next(own, 2, 0, range).
int32_t rand_below_ai(sim_store &own, uint32_t range);

// llm_rand_state_advance @0x004b4d3b. Ticks the CALLER-CHOSEN slot `rng_index` (unlike its sibling
// above, which always ticks channel 2) then normalizes the NEW state to a double via
// v.rng_norm_divisor. __cdecl in the original; rng_index arrives on the stack there.
double rand_state_advance(const sim_view &v, sim_store &own, int32_t rng_index);

} // namespace detail

// ---- the public wrappers, matching each original's committed prototype exactly -------------------

// int32_t __watcall llm_rand_below_ai(uint32_t range) -- addr/mh_calls.gen.h:1914.
int32_t rand_below_ai(uint32_t range);

// double __cdecl llm_rand_state_advance(int32_t rng_index) -- addr/mh_calls.gen.h:1595.
double rand_state_advance(int32_t rng_index);

namespace detail {
} // namespace detail

} // namespace mh::sim
