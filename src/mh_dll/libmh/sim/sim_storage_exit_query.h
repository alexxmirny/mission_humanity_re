#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

namespace detail {

uint32_t storage_exit_tile_is_clear(const sim_view &v, uint16_t player, int32_t storage_slot);

} // namespace detail

uint32_t storage_exit_tile_is_clear(uint16_t player, int32_t storage_slot);

} // namespace mh::sim
