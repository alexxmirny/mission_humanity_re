#pragma once

namespace mh::sim {

namespace detail {

// llm_bldg_load_resource_tail_noop @0x0049f84a. Literal empty body -- see the header banner above
// for the full derivation of why the original's counting loop has zero observable effect. No
// sim_view, no sim_store, no callees.
void bldg_load_resource_tail_noop();

} // namespace detail

// Live wrapper, matching the original's __watcall (no-arg, void-return) shape exactly.
void bldg_load_resource_tail_noop();

namespace detail {
} // namespace detail

} // namespace mh::sim
