#pragma once
#include <cstdint>

namespace mh::sim {

namespace detail {

// llm_player_teardown_hook_stub @0x0049800c. Empty body -- see the header banner. No sim_view, no
// sim_store, no callees: there is nothing for this function to read, write, or call. The parameter is
// accepted (matching the original's committed EAX-storage int param) but genuinely unused.
int32_t player_teardown_hook_stub(int32_t player_index);

} // namespace detail

// Live wrapper, matching the original's __watcall(EAX) int-in/int-out shape exactly.
int32_t player_teardown_hook_stub(int32_t player_index);

namespace detail {
} // namespace detail

} // namespace mh::sim
