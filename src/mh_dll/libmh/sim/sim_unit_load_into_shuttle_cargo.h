#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The 50 conceptual manifest entries and their 14-byte stride -- see the header banner's derivation.
// NOT shared cross-TU (per the "each TU re-derives its own copy" convention sim_order_enqueue.cpp's
// fine_to_tile() and siblings establish); no other translated function reads cargo_manifest_raw yet.
inline constexpr int32_t CARGO_MANIFEST_SLOTS = 50;  // 0x32 -- loop visits indices 49..0 inclusive
inline constexpr int32_t CARGO_ENTRY_STRIDE   = 0xe; // 14 bytes/entry

// ---- the outward calls ---------------------------------------------------------------------------
//
// Indirected for the same reason as every other sim/ TU: a direct mh::call:: inside a detail:: body
// reaches into the live game image, which makes the body untestable by net_selftest.exe simtest.
struct unit_load_into_shuttle_cargo_calls {
    int32_t (*unit_state_is_boarding)(int32_t state); // llm_unit_state_is_boarding @0x004967ce
    void (*storage_remove_docked_unit)(uint16_t player, int32_t unit_index,
                                       int32_t storage_slot);    // llm_strat_storage_remove_docked_unit @0x00489dc4
    void (*unit_teardown)(uint32_t player, uint16_t unit_index); // llm_strat_unit_teardown @0x00487ba5
};

const unit_load_into_shuttle_cargo_calls &live_unit_load_into_shuttle_cargo_calls();

namespace detail {

// llm_strat_unit_load_into_shuttle_cargo @0x0048e7c5. See the header banner above for the full
// derivation; the .cpp carries the per-line address citation. Returns 0 on no-op (manifest full, or
// the unit isn't in a boarding state), else the unit's cfg `human` flag (0 if it carries soldiers).
int32_t unit_load_into_shuttle_cargo(const sim_view &v, sim_store &own,
                                     const unit_load_into_shuttle_cargo_calls &c, uint16_t player,
                                     int32_t building_idx, uint16_t unit_idx);

} // namespace detail

// Live wrapper: the logic applied to state() and live_unit_load_into_shuttle_cargo_calls(). Matches
// the committed prototype (sig_llm_strat_unit_load_into_shuttle_cargo) exactly.
int32_t unit_load_into_shuttle_cargo(uint16_t player, int32_t building_idx, uint16_t unit_idx);

namespace detail {
} // namespace detail

} // namespace mh::sim
