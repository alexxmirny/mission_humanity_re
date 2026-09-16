#pragma once
#include <cstdint>

#include "tact/tact_state.h"

namespace mh::tact {

// llm_tact_unit_roster_slot (Ghidra-named per mh_addrs.gen.h, not yet emitted into
// mh_structs.gen.h -- see DECLARED NEED N7). Stated here only as byte-layout constants for this TU's
// own manual indexing, not as a shared struct type.
inline constexpr int32_t ROSTER_SLOT_BYTES       = 0x100; // sizeof(llm_tact_unit_roster_slot)
inline constexpr int32_t ROSTER_IDS_BYTE_OFFSET  = 4;     // offsetof(llm_tact_unit_roster_slot, ids)
inline constexpr int32_t UNASSIGNED_ROSTER_BYTES = 0x100; // sizeof(_G_LLM_TACT_UNASSIGNED_UNIT_ROSTER)
inline constexpr int32_t GROUP_ROSTER_BYTES      = 0x800; // sizeof(_G_LLM_TACT_GROUP_UNIT_ROSTER) == 8 * ROSTER_SLOT_BYTES

// The one outward call, indirected for offline testability (same shape as every other multi-callee
// TU in this module).
struct squad_roster_refresh_calls {
    void *(*fill_data)(void *ptr, uint32_t size, uint8_t value); // utils_fill_data @0x004d1780
};

const squad_roster_refresh_calls &live_squad_roster_refresh_calls();

namespace detail {

// llm_tact_squad_roster_refresh @0x004356bd. See the header banner for the full derivation. Takes
// both `v` (the unit roster's read side, plus the new MULTI_PANEL_VISIBLE_ROWS bound -- DECLARED NEED
// N1) and `own` (every write this function makes -- DECLARED NEEDS N2-N5).
void squad_roster_refresh(const tact_view &v, tact_store &own, const squad_roster_refresh_calls &c);

} // namespace detail

void squad_roster_refresh();

// Declared here per the module convention; DEFINED in tact_squad_roster_refresh.cpp, CALLED from
// install_shadow() by the conductor (not this TU).
namespace detail {
}

} // namespace mh::tact
