#pragma once
#include <cstdint>

#include "tact/tact_state.h"

namespace mh::tact {

struct weapon_calc_scatter_calls {
    int32_t (*rand_fn)();      // llm_rand @0x004da98b
    double (*sqrt_fn)(double); // llm_sqrt @0x004da9c0
};

const weapon_calc_scatter_calls &live_weapon_calc_scatter_calls();

namespace detail {

// llm_tact_weapon_calc_scatter @0x00430c77.
void weapon_calc_scatter(const tact_view &v, const weapon_calc_scatter_calls &c, int32_t building_id,
                         int32_t x, int32_t y, int32_t tx, int32_t ty, int32_t *out_dx,
                         int32_t *out_dy, int32_t weapon_slot);

} // namespace detail

void weapon_calc_scatter(int32_t building_id, int32_t x, int32_t y, int32_t tx, int32_t ty,
                         int32_t *out_dx, int32_t *out_dy, int32_t weapon_slot);

} // namespace mh::tact
