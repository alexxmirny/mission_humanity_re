#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The three outward calls, all ALREADY-MIGRATED SIBLINGS in the shuttle-unload SCC ring -- called
// through the ORIGINAL binary function per the ring's "ONE RULE" (never a sibling's mh::sim::
// wrapper). Indirected for offline testability, same reasoning as every other multi-callee TU in this
// batch: a direct `mh::call::` inside `detail::` reaches into the live game image, which makes the
// body untestable by net_selftest.exe simtest.
struct prod_shuttle_bay_unload_all_calls {
    // llm_prod_shuttle_unload_resource @0x0048e4fb. EAX=player, EDX=building_index, EBX=resource_id,
    // ECX=cap (here always -1, "unload everything"). Returns a uint8_t status, discarded by this
    // caller.
    uint8_t (*unload_resource)(uint16_t player, int32_t building_index, uint16_t resource_id,
                               int32_t cap);

    // llm_prod_shuttle_unload_passengers @0x0048e6f2. EAX=player, EDX=building_index, EBX=cap (here
    // always -1). Returns int32_t, discarded by this caller.
    int32_t (*unload_passengers)(uint16_t player, int32_t building_index, int32_t cap);

    // llm_strat_prod_unload_cargo_unit @0x0048ea27 (this ring's sibling, sim_prod_unload_cargo_unit.h).
    // EAX=player, EDX=building_index, EBX=cargo_index. Returns 0 success / 1 failure, summed into the
    // DEAD accumulator (see the banner above) -- discarded here too.
    uint32_t (*unload_cargo_unit)(uint16_t player, int32_t building_index, uint32_t cargo_index);
};

const prod_shuttle_bay_unload_all_calls &live_prod_shuttle_bay_unload_all_calls();

namespace detail {

// llm_prod_shuttle_bay_unload_all @0x0048f7a6. See the header banner above for the full derivation;
// the .cpp carries the per-line address citation. No shared state written directly -- const view only.
void prod_shuttle_bay_unload_all(const sim_view &v, const prod_shuttle_bay_unload_all_calls &c,
                                 uint16_t player, int32_t building_index);

} // namespace detail

// ---- the public surface, in original-behaviour terms -----------------------------------------
// Parameter types match the committed prototype in addr/mh_calls.gen.h exactly (the drift gate
// enforces this on export/shadow installation): void(uint16_t player, int32_t building_index).
void prod_shuttle_bay_unload_all(uint16_t player, int32_t building_index);

namespace detail {
} // namespace detail

} // namespace mh::sim
