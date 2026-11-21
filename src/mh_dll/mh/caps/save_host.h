// Hosted fixed-window/overflow composition for promoted planet save/load bodies.
#pragma once
#include <cstdint>

namespace mh::caps::savehost {
// Gather canonical windows and prepare path encoding before any fixed-window write.
int32_t prepare();
// Scatter loaded windows, validate/apply overflow, finalize orders and rebuild path state.
// The handle is opaque Watcom FILE state, never an MSVC FILE pointer.
int32_t load_extensions(int32_t handle);
// Restore temporary path encoding on every save exit, including write failure.
void restore();
} // namespace mh::caps::savehost
