// mh_mapgen -- deterministic RNGs. Header-only, integer only, identical on every platform.
//   SplitMix64  (Vigna, public domain)           -- seeding / stream splitting
//   Pcg32       (O'Neill, pcg32 XSH RR 64/32)    -- the generator's working RNG
// No std:: distributions anywhere: their output is implementation-defined and would break the
// "same seed -> same map on Win32 and x64" contract.
#pragma once
#include <cstdint>

namespace mh_mapgen {

struct SplitMix64 {
    std::uint64_t state;
    explicit SplitMix64(std::uint64_t seed = 0) : state(seed) {}
    std::uint64_t next() {
        state += 0x9E3779B97F4A7C15ull;
        std::uint64_t z = state;
        z               = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z               = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
};

struct Pcg32 {
    std::uint64_t state = 0, inc = 1;

    // Reference pcg32_srandom_r(initstate, initseq).
    void seed(std::uint64_t initstate, std::uint64_t initseq) {
        state = 0;
        inc   = (initseq << 1) | 1u;
        next_u32();
        state += initstate;
        next_u32();
    }
    std::uint32_t next_u32() {
        std::uint64_t old = state;
        state             = old * 6364136223846793005ull + inc;
        std::uint32_t xs  = std::uint32_t(((old >> 18) ^ old) >> 27);
        std::uint32_t rot = std::uint32_t(old >> 59);
        return (xs >> rot) | (xs << ((32u - rot) & 31u));
    }
    // Unbiased value in [0, bound) (reference pcg32_boundedrand_r). bound 0 returns 0.
    std::uint32_t bounded(std::uint32_t bound) {
        if (bound == 0) return 0;
        std::uint32_t threshold = (0u - bound) % bound;
        for (;;) {
            std::uint32_t r = next_u32();
            if (r >= threshold) return r % bound;
        }
    }
};

// The generator's per-attempt stream: both PCG words come from SplitMix64(seed + attempt*golden).
inline Pcg32 make_rng(std::uint64_t seed, std::uint32_t attempt) {
    SplitMix64    sm(seed + std::uint64_t(attempt) * 0x9E3779B97F4A7C15ull);
    std::uint64_t a = sm.next();
    std::uint64_t b = sm.next();
    Pcg32         r;
    r.seed(a, b);
    return r;
}

} // namespace mh_mapgen
