//
// crt/crt_rand.h -- the vendored `llm_rand` (LIB-CRT, the VENDOR-EQUIV class).
//
// WHAT THIS REPLACES. `llm_rand` @0x004da98b, 6 sites -- five calls-struct members in tact plus one
// R9 DIRECT inline site in tact/tact_unit_owner_tick.cpp (`try_wander`), which is named separately in
// LIB-CRT's clause because a vendor pass that only rewrites binder rows would leave it and still read
// as complete.
//
// ---- WHICH STREAM THIS IS, AND WHY THAT MATTERS --------------------------------------------------
//
// NOT the sim RNG. The deterministic simulation's random source is already ours (the LT1A batch); the
// tracker records this trap explicitly because the names invite the confusion. `llm_rand` is the
// COSMETIC TACTICAL stream: ambient sound selection, spawn jitter, teleport sparkle, animation phase,
// weapon scatter, idle wander. Its six sites are all in `tact`.
//
// ---- THE GENERATOR IS THE ANSI C LCG, EXACTLY ----------------------------------------------------
//
//     seed = seed * 0x41C64E6D + 0x3039      @0x004da995-0x004da9a1
//     return (seed >> 16) & 0x7FFF           @0x004da9a3-0x004da9a8   (a LOGICAL shift, SHR)
//
// so the result is in [0, 0x7FFF] and the multiply/add wrap in 32 bits. There is nothing
// vendor-specific in the arithmetic -- the whole equivalence question is WHERE THE SEED LIVES and
// what it starts at.
//
// ---- THE SEED: the one genuine difference from the original ---------------------------------------
//
// The original keeps it in the CRT's per-thread block, at thread_data+0x0c (FUN_004da981 @0x004da981
// is `llm_crt_get_thread_data` + 0xc). A standalone libmh has no such block, so it lives here.
//
// The original also has a NULL-state branch (@0x004da993 `TEST EAX,EAX / JZ`): if the thread-data
// pointer is null it returns 0 without advancing anything. That cannot happen to a file-scope
// variable, so the branch is unreachable here -- noted rather than reproduced, because reproducing an
// impossible predicate is how a dead `if` becomes a live bug later.
//
// THE INITIAL SEED IS 1, AND THAT IS MEASURED RATHER THAN ASSUMED FROM ANSI (2026-09-08). The block
// is initialised exactly once, at load, by __InitThreadData @0x004f19a7 -- instruction 0x004f19ce is
// `MOV dword ptr [EBX+0xc],0x1`. All 34 callers of the thread-data getter were enumerated and NONE
// writes +0x0c except llm_rand's own update; the binary contains no `srand` at all, and
// __InitMultipleThread (the only other path that would re-init a block) has zero callers. So the
// game's cosmetic stream is deterministic from process start.
//
// DO NOT GENERALISE THAT TO "the game's randomness is deterministic". The STRATEGIC game RNG
// (llm_strat_rng_seed_*, @0x00499dc8) IS wall-clock seeded through time()/localtime(). It is a
// different generator on a different channel, it is already ours, and it is not this file's.
//
// `llm_srand` exists so the host can pin the stream anyway -- a headless libmh replaying a recorded
// tactical session needs to, and nothing in the sim may read it.
//
// Not thread-safe, by the same argument crt_sprintf.h's `unsupported_seen` and crt_string.h's
// `strtok_saved` make: libmh's sim is single-threaded. These three cells are the complete list of
// mutable state this directory owns.
//
// The offline oracle is `net_selftest crttest` (mh_nettest/crt_vendor_selftest.cpp), which sweeps the
// sequence against the original assembly over >= 10000 draws from equal seeds, per LIB-CRT's
// done_when.
//
#pragma once

#include <cstdint>

namespace mh::crt {

// ANSI C's initial seed, and Watcom's: the value the per-thread block carries before anything calls
// srand.
inline uint32_t rand_seed = 1u;

inline void llm_srand(uint32_t seed) {
    rand_seed = seed;
}

// ---- llm_rand @0x004da98b ------------------------------------------------------------------------
inline int32_t llm_rand() {
    rand_seed = rand_seed * 0x41C64E6Du + 0x3039u;            // @0x004da995 IMUL / @0x004da99b ADD
    return static_cast<int32_t>((rand_seed >> 16) & 0x7FFFu); // @0x004da9a5 SHR / @0x004da9a8 AND
}

} // namespace mh::crt
