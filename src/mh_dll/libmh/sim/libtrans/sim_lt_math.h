#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_math_manhattan_dist @0x00489164. Pure: no sim_view/sim_store parameter at all (matches
// sim_unit_facing24_delta.h's precedent for a function that reads no game state) -- see the header
// banner for the subtrahend-order and abs-idiom derivation.
int32_t manhattan_dist(int32_t x0, int32_t y0, int32_t x1, int32_t y1);

// llm_math_scale_pct @0x0044b3b3. Reads sim_view::math_percent_divisor only (read-only, no write) --
// see the header banner for the x87 operation-order derivation.
double scale_pct(const sim_view &v, double value, int32_t pct);

} // namespace detail

// Live wrappers: the logic applied to state().read (scale_pct only -- manhattan_dist touches no state
// at all). Match each original's committed __watcall prototype exactly (addr/mh_calls.gen.h /
// addr/mh_export.gen.h).
int32_t manhattan_dist(int32_t x0, int32_t y0, int32_t x1, int32_t y1);
double  scale_pct(double value, int32_t pct);

namespace detail {
} // namespace detail

} // namespace mh::sim
