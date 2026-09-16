#pragma once

namespace mh::sim {

namespace detail {

// llm_bldg_transfer_notify_noop @0x0048f2f8. Empty body -- see the header banner. No sim_view, no
// sim_store, no callees: there is nothing for this function to read, write, or call.
void transfer_notify_noop();

} // namespace detail

// Live wrapper, matching the original's __watcall (no-arg, void-return) shape exactly.
void transfer_notify_noop();

namespace detail {
} // namespace detail

} // namespace mh::sim
