#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The four external callees this closure reaches. Indirected for the same reason as every other sim/
// TU's `_calls` struct: a direct mh::call:: inside a detail:: body reaches into the live game image,
// which makes the body untestable by net_selftest.exe simtest. Signatures copied verbatim from
// addr/mh_calls.gen.h.
struct landing_spot_calls {
    // llm_rand_below @0x00499f49 -- claim_landing_spot's random probe start index.
    int32_t (*rand_below)(int32_t upper_bound);
    // llm_strat_set_landing_site @0x00454fe3 -- commits the claimed spot's (x,y) into the player's
    // per-planet landing_x/landing_y (writes state INSIDE this callee, not visible to this closure);
    // return value (uint8_t) is unused by the original caller and discarded here too.
    uint8_t (*set_landing_site)(uint32_t player, uint32_t planet, uint32_t x, uint32_t y,
                                uint32_t spot_index);
    // llm_strat_count_landing_spots @0x00454d8d -- 0 iff the map's landing-spot table is exhausted.
    int32_t (*count_landing_spots)();
    // llm_strat_spawn_invasion_force @0x004dd446.
    int32_t (*spawn_invasion_force)(uint32_t player, int32_t is_alien_race, int32_t home_tile_x,
                                    int32_t home_tile_y, int32_t invasion_points);
};

const landing_spot_calls &live_landing_spot_calls();

namespace detail {

// llm_strat_claim_landing_spot @0x00454eb2. See the header banner for the full derivation.
int32_t claim_landing_spot(const sim_view &v, sim_store &own, const landing_spot_calls &c,
                           int32_t player, int32_t planet);

// llm_strat_spawn_enemy_landing @0x004998ae. Calls claim_landing_spot() directly (in-TU C++ call,
// not through `c`/mh::call::) per the batch context.
int32_t spawn_enemy_landing(const sim_view &v, sim_store &own, const landing_spot_calls &c);

} // namespace detail

// Public wrappers. Signatures match the committed EXPORT prototypes (mh_export.gen.h's
// sig_llm_strat_claim_landing_spot / sig_llm_strat_spawn_enemy_landing) exactly.
int32_t claim_landing_spot(uint32_t player, uint32_t planet);
int32_t spawn_enemy_landing();

namespace detail {
} // namespace detail

} // namespace mh::sim
