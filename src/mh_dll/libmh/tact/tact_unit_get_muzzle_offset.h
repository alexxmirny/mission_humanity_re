#pragma once
#include <cstdint>

#include "tact/tact_state.h"

namespace mh::tact {

namespace detail {

// llm_tact_unit_get_muzzle_offset @0x00432105.
void unit_get_muzzle_offset(const tact_view &v, int32_t unit_id, int32_t *out_x, int32_t *out_y,
                            uint32_t weapon_slot);

} // namespace detail

void unit_get_muzzle_offset(int32_t unit_id, int32_t *out_x, int32_t *out_y, uint32_t weapon_slot);

} // namespace mh::tact
