#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The two outward calls this function makes. Indirected for the same reason as every other sim/ TU
// (sim_prod_shuttle_fuel_apply.h's own `prod_shuttle_fuel_apply_calls` is the precedent for the
// single-callee shape; this function needs two): a direct mh::call:: inside detail:: would reach into
// the live game image and make the body untestable by net_selftest.exe simtest / the offline fixture.
struct prod_shuttle_fuel_check_calls {
    void (*resource_add)(int32_t player, int32_t resource_id, int32_t amount);   // llm_resource_add @0x00497f4a
    void (*spend_resource)(int32_t player, int32_t resource_id, int32_t amount); // game_SpendResource @0x00497f94
};

const prod_shuttle_fuel_check_calls &live_prod_shuttle_fuel_check_calls();

namespace detail {

// llm_prod_shuttle_fuel_check @0x004934b6. See the header banner above for the full derivation; the
// .cpp carries the per-branch address citation. Takes the mutable store (not just the view) because
// pass 2 draws down `resources_reserved[player][shuttle_slot][id]` on the slot record in place.
// Returns 0 (same-planet no-op, or fully charged) or a nonzero `resource_id + 0x89` / bare `0x89`
// shortage-reason code (see PASS 1 above).
int32_t prod_shuttle_fuel_check(const sim_view &v, sim_store &own, const prod_shuttle_fuel_check_calls &c,
                                uint16_t player, int32_t building_index, int32_t dest_planet);

} // namespace detail

// Live wrapper: the logic applied to state() and live_prod_shuttle_fuel_check_calls(). Matches the
// committed prototype (sig_llm_prod_shuttle_fuel_check) exactly.
int32_t prod_shuttle_fuel_check(uint16_t player, int32_t building_index, int32_t dest_planet);

namespace detail {
} // namespace detail

} // namespace mh::sim
