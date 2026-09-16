#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The two outward calls this function makes, indirected via mh::call:: for testability -- same shape
// as every other sim/ TU. See the header banner above for why this is NOT a direct call to either
// sibling's public mh::sim:: wrapper.
struct prod_shuttle_depart_calls {
    int32_t (*unit_load_into_shuttle_cargo)(uint16_t player, int32_t building_idx,
                                            uint16_t unit_idx); // llm_strat_unit_load_into_shuttle_cargo @0x0048e7c5
    int32_t (*prod_bldg_depart_finalize)(uint16_t player, int32_t building_index,
                                         int32_t dest_planet); // llm_prod_bldg_depart_finalize @0x0048ec51
};

const prod_shuttle_depart_calls &live_prod_shuttle_depart_calls();

namespace detail {

// llm_prod_shuttle_depart @0x0048e16a. See the header banner above for the full derivation; the .cpp
// carries the per-branch address citation. Always returns 1.
int32_t prod_shuttle_depart(const sim_view &v, sim_store &own, const prod_shuttle_depart_calls &c,
                            uint16_t player, int32_t building_index, int32_t dest_planet);

} // namespace detail

// Live wrapper: the logic applied to state(). Matches the committed prototype
// (sig_llm_prod_shuttle_depart) exactly.
int32_t prod_shuttle_depart(uint16_t player, int32_t building_index, int32_t dest_planet);

namespace detail {
} // namespace detail

} // namespace mh::sim
