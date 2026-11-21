#pragma once
#include "mh_order_tasks.h"

namespace mh::order_tasks::air {
inline constexpr uint32_t LEG_STEPS     = 299;
inline constexpr uint32_t CIRCUIT_STEPS = CAPACITY * LEG_STEPS;
inline constexpr uint32_t CHUNK_STEPS   = 64;

struct pose {
    tile     position;
    uint32_t heading;
};
struct path {
    uint32_t count;
    uint8_t  headings[LEG_STEPS];
};
struct circuit {
    pose     start;
    uint32_t count;
    uint8_t  headings[CIRCUIT_STEPS];
};

// Native movement states, not 24 equally spaced angles. Transient states 8..23
// must finish their turn before another turn may begin.
bool legal(uint32_t previous, uint32_t next) noexcept;
pose stepped(dimensions, pose, uint32_t next) noexcept;

// Exact tile AND heading. On failure only count is defined (and is zero).
// The complete bounded search uses shared fixed scratch: calls must be serialized,
// with no reentry. There are no callbacks or allocations inside this module.
bool plan(dimensions, pose from, pose to, bool require_motion, path &) noexcept;
bool build_circuit(dimensions, const tile *points, uint32_t count, circuit &) noexcept;
bool orbit(dimensions, pose from, circuit &) noexcept;

// These consume a valid generated circuit. at() returns the pose immediately
// before headings[next_index % count], including the actual previous heading.
// Invalid/empty input to at() returns start; join_index() returns UINT32_MAX.
pose at(dimensions, const circuit &, uint32_t next_index) noexcept;
// Closest toroidal Chebyshev distance, then smallest native bearing change,
// then lowest phase index. Only steady previous headings 0..7 are candidates.
uint32_t join_index(dimensions, pose from, const circuit &) noexcept;
} // namespace mh::order_tasks::air
