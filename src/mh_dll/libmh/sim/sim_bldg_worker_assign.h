#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The three external callees this closure reaches, indirected for offline testability (same reason as
// every other sibling `_calls` struct in this subsystem). All three stay original this slice --
// add_workers is a same-batch sibling (see the header banner above on why it is still called through
// this indirection rather than a direct C++ call), remove_workers/notify_state_change are external.
struct worker_assign_calls {
    // llm_strat_bldg_add_workers @0x00491916. EAX=player, EDX=building_id, EBX=count (the CLAMPED
    // request); returns the ACTUAL number of workers moved.
    int32_t (*add_workers)(uint16_t player, uint32_t building_id, int32_t count);
    // llm_strat_bldg_remove_workers @0x004917c5. EAX=player, EDX=building_id, EBX=count (the
    // UNCLAMPED request -- unassign_workers has no analogous cap); returns the ACTUAL amount removed.
    uint32_t (*remove_workers)(uint16_t player, uint32_t building_id, uint32_t count);
    // llm_strat_bldg_notify_state_change @0x00470c5c. EAX=player, EDX=building_id. Called
    // unconditionally by both functions, after the population bookkeeping.
    void (*notify_state_change)(uint16_t player, uint32_t building_id);
};

const worker_assign_calls &live_worker_assign_calls();

namespace detail {

// llm_strat_bldg_assign_workers @0x00491b78. See the header banner above for the derivation. Returns
// the actual number of workers moved (add_workers' own return value).
int32_t assign_workers(const sim_view &v, sim_store &own, const worker_assign_calls &gc, uint32_t player,
                       uint32_t building_id, int32_t count);

// llm_strat_bldg_unassign_workers @0x00491c08. Mirror image of assign_workers above, minus the clamp
// -- and so, unlike assign_workers, it consults no read-only state at all (no `sim_view` parameter).
// Returns the actual number of workers removed (remove_workers' own return value).
int32_t unassign_workers(sim_store &own, const worker_assign_calls &gc, uint16_t player,
                         uint32_t building_index, uint32_t count);

} // namespace detail

// ---- the public surface, in original-behaviour terms -----------------------------------------
// Parameter types match the committed prototypes (addr/mh_export.gen.h's sig_llm_strat_bldg_
// assign_workers / sig_llm_strat_bldg_unassign_workers) and addr/mh_calls.gen.h's own out-call
// wrappers.

int32_t assign_workers(uint32_t player, uint32_t building_id, int32_t count);
int32_t unassign_workers(uint16_t player, uint32_t building_index, uint32_t count);

namespace detail {
} // namespace detail

} // namespace mh::sim
