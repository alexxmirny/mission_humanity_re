#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

// llm_map_region_flood_fill @0x00422450. See the header banner for the full derivation (sentinel,
// depth compares, queue discipline, neighbour wrap, return value). No callees -- no `_calls` struct.
int32_t region_flood_fill(const sim_view &v, sim_store &own, uint32_t x, uint32_t y, llm_map_region *block);

} // namespace detail

// The public wrapper: state() and no calls table (there are none). Signature matches
// addr/mh_export.gen.h's committed `sig_llm_map_region_flood_fill` exactly -- this IS the promotion
// seam (batch doc rule 2).
int32_t region_flood_fill(uint32_t x, uint32_t y, llm_map_region *block);

} // namespace mh::sim
