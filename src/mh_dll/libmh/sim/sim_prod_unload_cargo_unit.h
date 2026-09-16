#pragma once
#include <cstdint>

#include "sim/sim_event_codes.h" // EVENT_INFO_REFRESH -- shared across sim/ TUs
#include "sim/sim_state.h"

namespace mh::sim {

// game::e::event members 7 and 14 (the strategic-sim notes' resolved 25-member table) -- see the
// header's DECLARED-NEED note above on why these are local, file-prefixed constants rather than a
// shared enum. Member 6 (INFO_REFRESH) is NOT re-declared here; it is reused from sim_event_codes.h.
inline constexpr uint32_t PROD_UNLOAD_CARGO_UNIT_BUILD_PROJECTS_REFRESH = 7u;
inline constexpr uint32_t PROD_UNLOAD_CARGO_UNIT_MAP_OBJECTS_REFRESH    = 14u;

// The four outward calls, indirected for offline testability (same reason as every other multi-callee
// TU in this batch: a direct mh::call:: inside a detail:: body reaches into the live game image, which
// makes the body untestable by net_selftest.exe simtest).
struct prod_unload_cargo_unit_calls {
    // llm_strat_unit_spawn_docked @0x0046434c. addr/mh_calls.gen.h: EAX=unit_proto_id,
    // EDX=player, EBX=probe_slot (here: the building's `sub_id`). Returns the new unit's index, or 0
    // on failure.
    int32_t (*unit_spawn_docked)(uint16_t unit_proto_id, uint16_t player, uint32_t probe_slot);

    // llm_strat_ai_notify_unit_lifecycle @0x004dbb38. EAX=player, EDX=unit_type, EBX=unit_id,
    // ECX=kind (4 = "unit appeared").
    void (*ai_notify_unit_lifecycle)(uint16_t player, uint16_t unit_type, uint32_t unit_id,
                                     uint32_t kind);

    // llm_strat_unit_ctrlgroup_add_member @0x00449401. EAX=unit_id, EDX=&group.count (address
    // escape), EBX=group_index.
    void (*unit_ctrlgroup_add_member)(int32_t unit_id, int32_t *group_count_ptr, int32_t group_index);

    // game_SetEvent @0x00413a52. Return value discarded by the original (called up to three times).
    uint32_t (*set_event)(uint32_t type);
};

const prod_unload_cargo_unit_calls &live_prod_unload_cargo_unit_calls();

namespace detail {

// llm_strat_prod_unload_cargo_unit @0x0048ea27. See the header banner above for the full derivation;
// the .cpp carries the per-line address citation. Returns 0 on success, 1 if the spawn failed.
uint32_t prod_unload_cargo_unit(const sim_view &v, sim_store &own,
                                const prod_unload_cargo_unit_calls &c, uint16_t player,
                                int32_t building_index, uint32_t cargo_index);

} // namespace detail

// ---- the public surface, in original-behaviour terms -----------------------------------------
// Parameter types match the committed prototype in addr/mh_calls.gen.h exactly (the drift gate
// enforces this on export/shadow installation): uint32_t(uint16_t player, int32_t building_index,
// uint32_t cargo_index).
uint32_t prod_unload_cargo_unit(uint16_t player, int32_t building_index, uint32_t cargo_index);

namespace detail {
} // namespace detail

} // namespace mh::sim
