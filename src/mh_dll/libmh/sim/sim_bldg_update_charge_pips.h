#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// ---- the outward calls -----------------------------------------------------------------------
//
// Indirected for the same reason as every other module here: a direct mh::call:: inside a detail::
// body reaches into the live game image, which makes the body untestable by net_selftest.exe simtest.
//
// utils_math_trunc is deliberately NOT a member here -- see the header banner's "OUTWARD CALLS" note.
struct bldg_update_charge_pips_calls {
    int32_t (*rand_below)(int32_t upper_bound); // llm_rand_below @0x00499f49
};

const bldg_update_charge_pips_calls &live_bldg_update_charge_pips_calls();

namespace detail {

// llm_strat_bldg_update_charge_pips @0x00478eb4. See the header banner above for the full derivation;
// the .cpp carries the per-branch address citation. Takes the mutable store (not just the view) because
// pip_active_count/pip_level/pip_frame/pip_timer are written in place on the building instance
// (writes_shared=["buildings"] per sim_migration.json; no other region written).
void bldg_update_charge_pips(const sim_view &v, sim_store &own, const bldg_update_charge_pips_calls &c,
                             uint16_t player, uint32_t building_id);

} // namespace detail

// Live wrapper: the logic applied to state() and live_bldg_update_charge_pips_calls(). Matches the
// committed prototype (`void __watcall(ushort player, uint building_id)`) exactly.
void bldg_update_charge_pips(uint16_t player, uint32_t building_id);

namespace detail {
} // namespace detail

} // namespace mh::sim
