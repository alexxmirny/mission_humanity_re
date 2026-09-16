//
// state/rebind_shims.cpp -- see rebind_shims.h for why these four exist and why they are hand-written.
//
#include "state/rebind_shims.h"

#include "orders/order_queue.h"
#include "sim/sim_resource_spend.h"
#include "sim/sim_spawn_invasion_force.h"
#include "sim/sim_unit_calc_range_approach_point.h"

namespace mh::rebind::shim {

// The committed shape says uint32_t; the wrapper takes int32_t. Same 32 bits, and every real player
// id is 0..7 -- the narrowing cannot lose a value that exists.
void bldg_record_resource_expenditure_stats(uint32_t player_id, int32_t building_id) {
    ::mh::sim::bldg_record_resource_expenditure_stats(static_cast<int32_t>(player_id), building_id);
}

// `mh::orders::order` IS `mh::game::mh_llm_strat_order` (order_queue.h:37) -- the only difference is
// the const the wrapper adds, so this is a qualification conversion, not a reinterpretation.
void order_integrity_check(::mh::game::mh_llm_strat_order *order, char *tag) {
    ::mh::orders::integrity_check(order, tag);
}

// The wrapper returns uint32_t (it forwards llm_strat_ai_invasion_spawn_reinforcements' result); the
// committed shape says int32_t. Same 32 bits.
int32_t spawn_invasion_force(uint32_t player, int32_t is_alien_race, int32_t home_tile_x,
                             int32_t home_tile_y, int32_t invasion_points) {
    return static_cast<int32_t>(::mh::sim::spawn_invasion_force(
        player, is_alien_race, home_tile_x, home_tile_y, invasion_points));
}

// The committed shape says uint32_t * out-params (TACT1-P C6, 2026-09-04), and the public wrapper
// now carries that same type, so this shim forwards straight through -- the reinterpret it used to
// need lives inside that wrapper, at the one boundary where the verified detail:: body's int32_t *
// is reached.
int32_t unit_calc_range_approach_point(uint32_t *io_target_x, uint32_t *io_target_y) {
    return ::mh::sim::unit_calc_range_approach_point(io_target_x, io_target_y);
}

} // namespace mh::rebind::shim
