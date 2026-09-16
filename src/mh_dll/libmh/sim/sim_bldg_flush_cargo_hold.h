#pragma once
#include "addr/mh_export.gen.h" // MH_EXPORT_REPLACE / the entry-thunk shapes
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// game::e::event member 7 (the strategic-sim notes' resolved 25-member table) -- see the header's
// DECLARED-NEED note above on why this is a local, file-prefixed constant rather than a shared enum.
// Same numeric value as sim_prod_unload_cargo_unit.h's PROD_UNLOAD_CARGO_UNIT_BUILD_PROJECTS_REFRESH,
// deliberately NOT reused across TUs (write-only-your-own-files).
inline constexpr uint32_t BLDG_FLUSH_CARGO_HOLD_BUILD_PROJECTS_REFRESH = 7u;

// The three outward calls, indirected for offline testability (same reason as every other
// multi-callee TU in this batch: a direct mh::call:: inside a detail:: body reaches into the live
// game image, which makes the body untestable by net_selftest.exe simtest). bay_unload_all and
// prod_shuttle_slot_release are both SCC SIBLINGS -- see the SCC RULE note above.
struct bldg_flush_cargo_hold_calls {
    // llm_prod_shuttle_bay_unload_all @0x0048f7a6. EAX=player, EDX=building_index.
    void (*bay_unload_all)(uint16_t player, int32_t building_index);

    // llm_strat_prod_shuttle_slot_release @0x0046318d. EAX=player, EDX=slot.
    void (*prod_shuttle_slot_release)(int32_t player, int32_t slot);

    // game_SetEvent @0x00413a52. Return value discarded by the original.
    uint32_t (*set_event)(uint32_t type);
};

const bldg_flush_cargo_hold_calls &live_bldg_flush_cargo_hold_calls();

namespace detail {

// llm_strat_bldg_flush_cargo_hold @0x0048e046. See the header banner above for the full derivation;
// the .cpp carries the per-line address citation. Void return.
void bldg_flush_cargo_hold(const sim_view &v, sim_store &own, const bldg_flush_cargo_hold_calls &c,
                           uint32_t player, int32_t building_index);

} // namespace detail

// ---- the public surface, in original-behaviour terms -----------------------------------------
// Parameter types match the committed prototype in addr/mh_calls.gen.h exactly (the drift gate
// enforces this on export/shadow installation): void(uint32_t player, int32_t building_index).
void bldg_flush_cargo_hold(uint32_t player, int32_t building_index);

namespace detail {
} // namespace detail

} // namespace mh::sim
