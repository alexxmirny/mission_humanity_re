#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The five already-migrated (committed-original) callees this closure reaches, indirected for
// offline testability -- same reason as sim_bldg_scrap_stored_units.h's `scrap_stored_units_calls`.
// All five are REAL calls in production; this function does not reimplement any of their bodies, it
// only sequences them per-arm, so they are dispatched through mh::call:: rather than any co-migrated
// mh::sim:: body (translator brief rule 3 / the task instructions' explicit rule for this function).
struct unit_bldg_energy_refill_full_calls {
    // Building arm, in call order. llm_strat_bldg_update_charge_pips @0x00478eb4,
    // llm_strat_refresh_building @0x004705de, llm_strat_bldg_notify_ui @0x00470bdd.
    void (*bldg_update_charge_pips)(uint16_t player, uint32_t building_id);
    void (*refresh_building)(uint16_t p_id, int32_t b_id);
    void (*bldg_notify_ui)(uint16_t player, uint32_t b_index);

    // Unit arm, in call order. llm_strat_unit_update_damage_smoke @0x004870d8,
    // llm_strat_unit_notify_ui @0x00488a22.
    void (*unit_update_damage_smoke)(uint32_t player, int32_t unit_idx);
    void (*unit_notify_ui)(uint32_t side, uint32_t unit_index);
};

const unit_bldg_energy_refill_full_calls &live_unit_bldg_energy_refill_full_calls();

namespace detail {

// llm_unit_bldg_energy_refill_full @0x0049a8c2. See the header banner above for the full
// derivation; the .cpp carries the per-branch address citation.
void unit_bldg_energy_refill_full(const sim_view &v, sim_store &own,
                                  const unit_bldg_energy_refill_full_calls &gc,
                                  uint32_t player_and_flags, uint32_t target_index);

} // namespace detail

// ---- the public surface, in original-behaviour terms -----------------------------------------
// Parameter types match the committed prototype (sig_llm_unit_bldg_energy_refill_full) exactly:
// void(uint32_t player_and_flags, uint32_t target_index).
void unit_bldg_energy_refill_full(uint32_t player_and_flags, uint32_t target_index);

namespace detail {
} // namespace detail

} // namespace mh::sim
