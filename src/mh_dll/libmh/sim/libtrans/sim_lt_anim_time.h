#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// cfg_GetAnimTime @0x004616fd. See the header banner above for the full derivation; the .cpp carries
// the per-line address citation. Matches the original's double(int32_t) -> double signature exactly.
// `index_` is `cfg_t_frame_index`, Ghidra's semantic typedef of int32_t for this domain -- it has no
// materialised C++ typedef anywhere in this codebase (same established convention
// sim_cfg_anim_frame_at_progress.h documents), so it is `int32_t` here too.
double cfg_get_anim_time(const sim_view &v, int32_t index_);

} // namespace detail

// ---- the public surface, in original-behaviour terms -----------------------------------------
// Parameter/return types match the committed prototype exactly (addr/mh_calls.gen.h:774):
// double(int32_t index_).
double cfg_get_anim_time(int32_t index_);

namespace detail {
} // namespace detail

} // namespace mh::sim
