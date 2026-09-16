#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// ---- (1) llm_strat_weapon_pixel_distance_ratio's outward calls -----------------------------------
struct weapon_pixel_distance_ratio_calls {
    // llm_strat_pixel_delta_wrapped @0x004944f6 -- TWO INT32 OUT-POINTERS (not doubles; see the
    // header banner's hazard #4 note).
    void (*pixel_delta_wrapped)(int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t *out_dx,
                                int32_t *out_dy);
    double (*sqrt_fn)(double x); // llm_sqrt @0x004da9c0
};

const weapon_pixel_distance_ratio_calls &live_weapon_pixel_distance_ratio_calls();

// ---- (2) llm_strat_unit_estimate_weapon_damage's outward calls -----------------------------------
struct unit_estimate_weapon_damage_calls {
    double (*scale_pct)(double value, int32_t pct); // llm_math_scale_pct @0x0044b3b3
};

const unit_estimate_weapon_damage_calls &live_unit_estimate_weapon_damage_calls();

namespace detail {

// llm_strat_weapon_pixel_distance_ratio @0x00448cc3. See the header banner for the full derivation.
double weapon_pixel_distance_ratio(const sim_view &v, int32_t weapon_id, int32_t x1, int32_t y1,
                                   int32_t x2, int32_t y2, const weapon_pixel_distance_ratio_calls &c);

// llm_strat_unit_estimate_weapon_damage @0x004d32aa. See the header banner for the full derivation.
// Parameter names per the committed-prototype mapping documented above (player/unit_index are the
// ATTACKER; target_ref/target_index identify the TARGET).
uint32_t unit_estimate_weapon_damage(const sim_view &v, int32_t player, int32_t unit_index,
                                     uint32_t target_ref, int32_t target_index,
                                     const unit_estimate_weapon_damage_calls &c);

} // namespace detail

// Live wrappers: the logic applied to state().read and the live_*_calls() above. Match the committed
// prototypes (sig_llm_strat_weapon_pixel_distance_ratio / sig_llm_strat_unit_estimate_weapon_damage)
// exactly.
double   weapon_pixel_distance_ratio(int32_t weapon_id, int32_t x1, int32_t y1, int32_t x2, int32_t y2);
uint32_t unit_estimate_weapon_damage(int32_t player, int32_t unit_index, uint32_t target_ref,
                                     int32_t target_index);

namespace detail {
} // namespace detail

} // namespace mh::sim
