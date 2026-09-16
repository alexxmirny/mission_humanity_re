#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The outward calls, indirected (like every sim/ TU) so detail:: stays testable under simtest. Both
// are frontier originals (neither is a sim_resid sibling), reached through mh::call:: in
// live_invasion_due_check_calls(), never called directly.
struct invasion_due_check_calls {
    int32_t (*spawn_enemy_landing)();                                 // llm_strat_spawn_enemy_landing @0x004998ae
    uint32_t (*player_presence_lost)(uint32_t player, uint32_t mode); // llm_strat_player_presence_lost @0x00498089
};

const invasion_due_check_calls &live_invasion_due_check_calls();

namespace detail {

// llm_strat_invasion_due_check @0x0049949d. Reads G_PLANET_INDEX / game_clock / local_player_slot
// through `v`, reads+writes the current planet's invasion timer through `own`
// (planet_invasion_time_at, mutable-only region), and reaches both callees (both frontier) through
// `c`. Returns the original's int 0/1 fired flag -- see THE RETURN VALUE in the banner above.
int32_t invasion_due_check(const sim_view &v, sim_store &own, const invasion_due_check_calls &c);

} // namespace detail

// Live wrapper: the logic applied to state() and live_invasion_due_check_calls().
int32_t invasion_due_check();

} // namespace mh::sim
