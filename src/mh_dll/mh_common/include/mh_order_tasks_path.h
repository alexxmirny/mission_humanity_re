#pragma once
#include <cstdint>

namespace mh::order_tasks {

// The caller supplies the fresh writer's solver buffer (at least capacity + 1 bytes)
// and encoded route (at least capacity two-byte waypoints). This proves encoding
// completeness, not writer provenance, assignment identity, readiness or arrival.
inline bool complete_direction_trace(const uint8_t *solver, const uint8_t *encoded, uint32_t capacity) {
    if (!solver || !encoded || !capacity || capacity > 300 || solver[0] >= 24) return false;
    for (uint32_t i = 0; i < capacity; ++i) {
        const uint32_t expected = solver[i + 1];
        const uint32_t actual   = encoded[i * 2];
        if (expected == 255) return actual == 255;
        if (expected >= 24 || actual != expected) return false;
    }
    return false;
}

} // namespace mh::order_tasks
