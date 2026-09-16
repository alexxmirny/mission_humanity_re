#pragma once
#include "addr/mh_export.gen.h" // MH_EXPORT_REPLACE / the entry-thunk shapes
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_strat_group_scratch_add_unit_and_normalize_heading @0x0048d283. See the header derivation
// above for the full shape.
void group_scratch_add_unit_and_normalize_heading(const sim_view &v, sim_store &own, int32_t player,
                                                  int32_t unit_index, int32_t *io_count);

} // namespace detail

// Live wrapper: the logic applied to state(). Matches the committed prototype
// (sig_llm_strat_group_scratch_add_unit_and_normalize_heading) exactly -- io_count carries the
// committed `int32_t *` pointee (TACT1-P C6, 2026-09-04) to match the mh::call:: / mh::exp::
// function-pointer type bit-for-bit.
void group_scratch_add_unit_and_normalize_heading(int32_t player, int32_t unit_index, int32_t *io_count);

namespace detail {
} // namespace detail

} // namespace mh::sim
