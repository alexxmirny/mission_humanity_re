#pragma once
#include <cstdint>

namespace mh::sim {

namespace detail {

// llm_strat_facing24_to_delta @0x0049610f. See the header banner above for the derivation; the .cpp
// carries the per-branch address citation. Pure: no sim_view/sim_store parameter, matching
// sim_facing24_from_points.h's precedent for a function that reads no game state.
void facing24_to_delta(uint32_t facing24, int32_t *out_dx, int32_t *out_dy);

} // namespace detail

// Public wrapper. Signature matches the committed export/call/shadow shape
// (sig_llm_strat_facing24_to_delta: void(__watcall*)(uint32_t, int32_t*, int32_t*)) exactly -- the
// two out-params carry the committed `int32_t*` pointee (TACT1-P C6, 2026-09-04), matching
// sim_unit_predict_coords.h's identical out-param convention for the SAME callee (that function's
// `facing24_to_delta` member of `unit_predict_coords_after_delay_calls` binds this exact original).
void facing24_to_delta(uint32_t facing24, int32_t *out_dx, int32_t *out_dy);

namespace detail {
} // namespace detail

} // namespace mh::sim
