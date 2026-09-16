#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The one outward call `unit_queue_advance_search` makes -- llm_strat_order_issue_0xf_adjacent_by_
// offset, an ORIGINAL function outside this batch (a separate TU's future translation target).
// Indirected the same way every other sim/ TU with an original callee is, so `detail::` stays
// drivable over an injected/recording call table (net_selftest.exe simtest) rather than reaching
// into the loaded game image.
struct unit_queue_advance_search_calls {
    void (*order_issue_0xf_adjacent_by_offset)(uint32_t player, int32_t unit_index, int32_t dx,
                                               int32_t dy); // llm_strat_order_issue_0xf_adjacent_by_offset @0x0046a19d
};

const unit_queue_advance_search_calls &live_unit_queue_advance_search_calls();

namespace detail {

// llm_strat_unit_queue_advance_search @0x004d7f3a. See the header banner above for the full DFS
// shape. Returns the final frontier count (0 = no free tile reachable; >=1 = chain length).
int32_t unit_queue_advance_search(const sim_view &v, sim_store &own, uint32_t player,
                                  uint32_t unit_index, const unit_queue_advance_search_calls &c);

// llm_strat_unit_queue_advance @0x004dbf27. A pure JMP-tail forwarder to this TU's own
// unit_queue_advance_search -- see the header banner on why this is a direct detail:: call rather
// than an mh::call:: indirection.
void unit_queue_advance(const sim_view &v, sim_store &own, uint32_t player, uint32_t unit_index,
                        const unit_queue_advance_search_calls &c);

} // namespace detail

// Public wrappers. Signatures match the committed prototypes in addr/mh_calls.gen.h /
// addr/mh_export.gen.h exactly (sig_llm_strat_unit_queue_advance_search,
// sig_llm_strat_unit_queue_advance).
int32_t unit_queue_advance_search(uint32_t player, uint32_t unit_index);
void    unit_queue_advance(uint32_t player, uint32_t unit_index);

namespace detail {
} // namespace detail

} // namespace mh::sim
