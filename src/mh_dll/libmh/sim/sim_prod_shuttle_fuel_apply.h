#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The one outward call this function makes. Indirected for the same reason as every other sim/ TU
// (sim_bldg_grant_type_resources.h's own `grant_type_resources_calls` is the precedent for this exact
// one-member shape): a direct mh::call:: inside detail:: would reach into the live game image and make
// the body untestable by net_selftest.exe simtest / the offline fixture.
struct prod_shuttle_fuel_apply_calls {
    void (*resource_add)(int32_t player, int32_t resource_id, int32_t amount); // llm_resource_add @0x00497f4a
};

const prod_shuttle_fuel_apply_calls &live_prod_shuttle_fuel_apply_calls();

namespace detail {

// llm_prod_shuttle_fuel_apply @0x00493705. See the header banner above for the full derivation
// (including the two DECLARED NEEDs on `fuel`'s offset and size). Always returns 0.
int32_t prod_shuttle_fuel_apply(const sim_view &v, const prod_shuttle_fuel_apply_calls &c,
                                uint16_t player, int32_t building_index, int32_t dest_planet);

} // namespace detail

// Live wrapper: the logic applied to state().read and live_prod_shuttle_fuel_apply_calls(). Matches
// the committed prototype (mh_calls.gen.h) exactly.
int32_t prod_shuttle_fuel_apply(uint16_t player, int32_t building_index, int32_t dest_planet);

namespace detail {
} // namespace detail

} // namespace mh::sim
