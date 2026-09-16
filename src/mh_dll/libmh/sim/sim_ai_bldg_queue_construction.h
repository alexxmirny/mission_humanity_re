#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// entry.resource_reserved has 5 slots; legitimate cfg resource ids are 1..4 (id 0 == UNDEFINED,
// the loop-terminating sentinel). See the header banner above for why this bound exists at all --
// the ORIGINAL store is unbounded.
inline constexpr int32_t BLDG_QUEUE_RESOURCE_RESERVED_COUNT = 5;

// The one outward call the THUNK makes -- llm_strat_bldg_queue_construction itself, at its ORIGINAL
// address. See the header banner on why this is a one-member struct rather than a direct
// `mh::call::` inside `detail::` (sim_bldg_defense_cost.h's precedent).
struct bldg_queue_construction_thunk_calls {
    int32_t (*queue_construction)(int32_t player, int32_t building_type, int16_t x,
                                  uint16_t y); // llm_strat_bldg_queue_construction @0x004e2550
};

const bldg_queue_construction_thunk_calls &live_bldg_queue_construction_thunk_calls();

namespace detail {

// llm_strat_bldg_queue_construction @0x004e2550. See the header banner for the full derivation;
// the .cpp carries the per-write address citation. Returns 0 (appended) or 1 (queue full).
int32_t bldg_queue_construction(const sim_view &v, sim_store &own, int32_t player,
                                int32_t building_type, int16_t x, uint16_t y);

// llm_strat_bldg_queue_construction_thunk @0x004e4e36. A pure forwarder -- see the header banner.
void bldg_queue_construction_thunk(const bldg_queue_construction_thunk_calls &c, int32_t player,
                                   int32_t building_type, int16_t x, uint16_t y);

} // namespace detail

// Public wrappers. Signatures match the committed prototypes in addr/mh_calls.gen.h exactly.
int32_t bldg_queue_construction(int32_t player, int32_t building_type, int16_t x, uint16_t y);
void    bldg_queue_construction_thunk(int32_t player, int32_t building_type, int16_t x, uint16_t y);

namespace detail {
} // namespace detail

} // namespace mh::sim
