#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The two already-migrated SIM1A callees this closure reaches, indirected for offline testability
// (same reason as sim_bldg_placement_preview.h's `placement_preview_calls` / sim_order_enqueue.h's
// `calls`). Both are REAL calls in production -- this function does not reimplement either body,
// it only sequences the two original calls per docked unit, so they are dispatched through
// mh::call:: rather than mh::sim::'s own (also-translated) bodies for those functions.
struct scrap_stored_units_calls {
    // llm_strat_unit_refund_build_cost_by_health @0x0048d706. unit_index passed FULL WIDTH
    // (int32_t), matching the committed prototype.
    void (*refund_build_cost_by_health)(int32_t player, int32_t unit_index);

    // llm_strat_unit_teardown @0x00487ba5. unit_index passed TRUNCATED to 16 bits, matching the
    // committed uint16_t prototype.
    void (*unit_teardown)(uint32_t player, uint16_t unit_index);
};

const scrap_stored_units_calls &live_scrap_stored_units_calls();

namespace detail {

// llm_bldg_scrap_stored_units @0x0048d7d5. See the header banner above for the full derivation;
// the .cpp carries the per-line address citation.
void bldg_scrap_stored_units(const sim_view &v, const scrap_stored_units_calls &gc, uint32_t player,
                             int32_t building_index);

} // namespace detail

// ---- the public surface, in original-behaviour terms -----------------------------------------
// Parameter types match the committed prototype in addr/mh_calls.gen.h exactly (the drift gate
// enforces this on export/shadow installation): void(uint32_t player, int32_t building_index).
void bldg_scrap_stored_units(uint32_t player, int32_t building_index);

namespace detail {
} // namespace detail

} // namespace mh::sim
