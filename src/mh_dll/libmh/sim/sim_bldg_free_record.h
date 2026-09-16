#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_strat_bldg_free_record @0x0047ba6c. See the header banner above for the full derivation; the
// .cpp carries the per-write address citation. No `sim_view` parameter: the body never reads sim
// state (the one read in the original, G_PLANET_INDEX, feeds only the dead-code block above).
void bldg_free_record(sim_store &own, uint32_t player, int32_t building_index);

} // namespace detail

// Live wrapper: the logic applied to state().own. Matches the committed prototype
// (sig_llm_strat_bldg_free_record) exactly.
void bldg_free_record(uint32_t player, int32_t building_index);

namespace detail {
} // namespace detail

} // namespace mh::sim
