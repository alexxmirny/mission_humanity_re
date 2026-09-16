#pragma once
#include <cstdint>

#include "sim/sim_state.h"
#include "sim/sim_unit_type_predicates.h" // UNIT_TYPE_UNDEFINED/A_HELI/A_WALKER/A_PLANE/A_HELI_MOTHER

namespace mh::sim {

// The five outward calls both functions make between them, indirected for offline testability (same
// reason as every other multi-callee TU in this closure).
struct prod_cargo_calls {
    // llm_strat_prod_unload_cargo_unit @0x0048ea27. ALREADY TRANSLATED elsewhere in this closure
    // (sim_prod_unload_cargo_unit.cpp/.h) -- called here as an ORIGINAL/by-address callee per the
    // translator brief's Law 4 ("minimise live seams"), matching sim_bldg_anim_trigger.h's identical
    // precedent for a sibling that is already its own translation unit. Signature matches
    // addr/mh_calls.gen.h's committed prototype exactly. Return value is discarded by the caller
    // (see the header banner's dead-accumulator note).
    uint32_t (*unload_cargo_unit)(uint16_t player, int32_t building_index, uint32_t cargo_index);

    // llm_bldg_transfer_notify_noop @0x0048f2f8. Committed prototype void(void) -- called with no
    // arguments (the EAX/EDX loads visible before each .asm call site are dead).
    void (*transfer_notify_noop)();

    // game_SpendResource @0x00497f94. EAX=player, EDX=resource_id, EBX=amount.
    void (*spend_resource)(int32_t player, int32_t resource_id, int32_t amount);

    // llm_strat_population_remove @0x00491486. EAX=player, EDX=count.
    void (*population_remove)(uint32_t player, int32_t count);

    // llm_strat_unit_housing_count_add @0x0049779b. EAX=player, EDX=unit_proto_id.
    void (*unit_housing_count_add)(int32_t player, int32_t unit_proto_id);
};

const prod_cargo_calls &live_prod_cargo_calls();

namespace detail {

// llm_strat_prod_unload_cargo_manifest @0x0048e2c0. See the header banner for the full derivation;
// the .cpp carries the per-line address citation. Read-only over sim state (every write happens
// inside the ORIGINAL callee, not in this function's own body), so no `sim_store &` parameter.
int32_t prod_unload_cargo_manifest(const sim_view &v, const prod_cargo_calls &c, uint16_t player,
                                   int32_t building_index);

// llm_strat_prod_try_start_unit @0x00492843. See the header banner for the full derivation. Takes
// `sim_store &own` for the header-row increment at the end (its only direct write).
int32_t prod_try_start_unit(const sim_view &v, sim_store &own, const prod_cargo_calls &c,
                            uint16_t player, int32_t unit_proto_id);

} // namespace detail

// ---- the public surface, in original-behaviour terms -----------------------------------------
// Parameter types match the committed prototypes in addr/mh_calls.gen.h exactly (the drift gate
// enforces this on export/shadow installation): int32_t(uint32_t player, int32_t).
int32_t prod_unload_cargo_manifest(uint32_t player, int32_t building_index);
int32_t prod_try_start_unit(uint32_t player, int32_t unit_proto_id);

namespace detail {
} // namespace detail

} // namespace mh::sim
