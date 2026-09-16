#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The three outward calls this closure reaches, indirected for offline testability -- same reason
// sim_bldg_construct_finalize.h / sim_bldg_reset_construction_anim.h indirect their own committed-
// original callees. Member function-pointer types match each callee's OWN committed prototype in
// addr/mh_calls.gen.h exactly.
struct bldg_instant_construct_calls {
    // llm_bldg_footprint_is_clear @0x0049396b -- already-translated sibling (SIM1B), called at its
    // ORIGINAL address per house rule (not via its new C++ name).
    int32_t (*footprint_is_clear)(int32_t x, int32_t y, int32_t building_type, uint32_t viewer);
    // game_HandleProgress @0x0043ff0c.
    void (*handle_progress)(uint16_t player, uint16_t inv);
    // llm_bldg_construct_finalize @0x00462e66 -- already-translated sibling (SIM1B), called at its
    // ORIGINAL address per house rule (not via its new C++ name).
    int32_t (*construct_finalize)(uint32_t param_1, int32_t y_b, uint16_t player, char param_4,
                                  uint32_t x_b, uint32_t building_id);
};

const bldg_instant_construct_calls &live_bldg_instant_construct_calls();

namespace detail {

// llm_bldg_queue_construction @0x00462d89. See the header banner above for the full derivation; the
// .cpp carries the per-branch address citation. Writes `buildings[player][result].cycle_progress`
// only -- hence `sim_store &own`, not merely a const view.
int32_t bldg_instant_construct(const sim_view &v, sim_store &own,
                               const bldg_instant_construct_calls &c, int32_t player,
                               int32_t building_type, int32_t x, int32_t y);

} // namespace detail

// Public wrapper. Parameter types match the committed prototype (sig_llm_bldg_queue_construction /
// addr/mh_calls.gen.h's own inline forwarder) exactly: int(int player, int building_type, int x,
// int y).
int32_t bldg_instant_construct(int32_t player, int32_t building_type, int32_t x, int32_t y);

namespace detail {
} // namespace detail

} // namespace mh::sim
