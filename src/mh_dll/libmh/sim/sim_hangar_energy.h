#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// ---- llm_strat_hangar_recharge_pulse's callees --------------------------------------------------------
struct hangar_recharge_pulse_calls {
    int32_t (*unit_try_pay_action_cost)(uint32_t player, int32_t unit_idx);
    void (*unit_update_damage_smoke)(uint32_t player, int32_t unit_idx);
};

const hangar_recharge_pulse_calls &live_hangar_recharge_pulse_calls();

namespace detail {

// llm_strat_hangar_any_unit_needs_energy @0x0047b0db. Pure query -- no calls struct needed (nothing to
// indirect). Returns 1 if any docked unit in buildings[player][index]'s hangar has energy < its type's
// max (ordered) OR an unordered compare against it; 0 otherwise (including an empty/absent hangar).
int32_t hangar_any_unit_needs_energy(const sim_view &v, int32_t player, int32_t index);

// llm_strat_hangar_recharge_pulse @0x0047b1be. See the header banner above for the full per-step
// derivation; the .cpp carries the per-branch address citation.
void hangar_recharge_pulse(const sim_view &v, sim_store &own, uint32_t player, int32_t index,
                           const hangar_recharge_pulse_calls &c);

} // namespace detail

// Live wrappers: the logic applied to state() (and, for recharge_pulse, live_hangar_recharge_pulse_calls()).
// Match the committed prototypes (sig_llm_strat_hangar_any_unit_needs_energy /
// sig_llm_strat_hangar_recharge_pulse) exactly.
int32_t hangar_any_unit_needs_energy(int32_t player, int32_t index);
void    hangar_recharge_pulse(uint32_t player, int32_t index);

namespace detail {
} // namespace detail

} // namespace mh::sim
