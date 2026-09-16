#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The one external callee this closure reaches, indirected for offline testability (same reason as
// every other sibling `_calls` struct in this subsystem). See the header banner above for the
// name-mismatch finding: this is addr/mh_calls.gen.h's CURRENT name for 0x0046318d, not the stale
// name the exported .asm's inline comment carries.
struct prod_shuttle_slot_bind_default_calls {
    // llm_strat_prod_shuttle_slot_release @0x0046318d. EAX=player (16-bit value, widened), EDX=slot.
    // Void return; called unconditionally right after a free slot is found, before any of this
    // function's own field writes.
    void (*slot_release)(int32_t player, int32_t slot);
};

const prod_shuttle_slot_bind_default_calls &live_prod_shuttle_slot_bind_default_calls();

namespace detail {

// llm_prod_shuttle_slot_bind_default @0x0048dea7. See the header banner above for the full
// derivation; the .cpp carries the per-branch address citation. Returns 0 if the building already
// holds a bound slot, the newly-bound slot index (1-9) on success, or -1 if all nine slots are
// occupied.
int32_t prod_shuttle_slot_bind_default(const sim_view &v, sim_store &own,
                                       const prod_shuttle_slot_bind_default_calls &c, uint32_t player,
                                       int32_t building_index);

} // namespace detail

// Public wrapper. Parameter types match the committed prototype
// (sig_llm_prod_shuttle_slot_bind_default) exactly: int32_t(uint32_t player, int32_t building_index).
int32_t prod_shuttle_slot_bind_default(uint32_t player, int32_t building_index);

namespace detail {
} // namespace detail

} // namespace mh::sim
