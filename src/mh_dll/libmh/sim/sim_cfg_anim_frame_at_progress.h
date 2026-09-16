#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The one external callee this closure reaches, indirected for offline testability (same reason as
// every other module here: a direct mh::call:: inside a detail:: body reaches into the live game
// image, which makes the body untestable by net_selftest.exe simtest).
struct cfg_anim_frame_at_progress_calls {
    // cfg_GetAnimTime @0x004616fd. PURE function of its one argument (no state touched) -- already
    // committed in addr/mh_calls.gen.h.
    double (*get_anim_time)(int32_t start_frame);
};

const cfg_anim_frame_at_progress_calls &live_cfg_anim_frame_at_progress_calls();

namespace detail {

// llm_cfg_anim_frame_at_progress @0x0046190d. See the header banner above for the full derivation;
// the .cpp carries the per-line address citation. Matches the original's
// int32_t(int32_t, double) -> int32_t signature exactly.
int32_t cfg_anim_frame_at_progress(const sim_view &v, const cfg_anim_frame_at_progress_calls &c,
                                   int32_t start_frame, double progress_fraction);

} // namespace detail

// ---- the public surface, in original-behaviour terms -----------------------------------------
// Parameter/return types match the committed prototype exactly (the drift gate enforces this on
// export/shadow installation): int32_t(int32_t start_frame, double progress_fraction).
int32_t cfg_anim_frame_at_progress(int32_t start_frame, double progress_fraction);

namespace detail {
} // namespace detail

} // namespace mh::sim
