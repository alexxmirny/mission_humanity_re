#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// llm_strat_unit_state (docs/structs.md's `state`/`order` field domain, applied but with no
// generated C++ enum header) -- PARKED = 0x1f. Same value multiple other sim/ TUs already name
// locally (sim_order_enqueue.h's UNIT_STATE_PARKED, sim_unit_update_rotation.cpp's STATE_PARKED,
// sim_unit_force_disembark.cpp's UNIT_STATE_PARKED) -- see the header's DECLARED NEED note above.
inline constexpr uint16_t MAP_UNIT_ADD_STATE_PARKED = 0x1fu;

// game::e::event member 14 (the strategic-sim notes' resolved 25-member table), same value + same
// derivation sim_unit_init_record.h's own MAP_OBJECTS_REFRESH names -- duplicated locally rather than
// included cross-TU, per the "write only your own new files" / "this TU gets its own state" rule.
inline constexpr uint32_t MAP_UNIT_ADD_MAP_OBJECTS_REFRESH = 14u;

// ---- the outward calls -----------------------------------------------------------------------
//
// Indirected for the same reason as every other multi-callee TU in this batch: a direct mh::call::
// inside a detail:: body reaches into the live game image, which makes the body untestable by
// net_selftest.exe simtest. Both callees are ORIGINAL functions outside this batch.
struct map_unit_add_calls {
    void *(*fill_data)(void *ptr, uint32_t size, uint8_t default_);
    uint32_t (*set_event)(uint32_t type);
};

const map_unit_add_calls &live_map_unit_add_calls();

namespace detail {

// map_unit_Add @0x00461e9e. See the header hazards above for the full derivation. `player` arrives
// as a full register but the ORIGINAL reads it back via a 16-bit (word) load at every address
// computation in the body (unlike `unit`/`unit_proto`, which are read back full-width) -- reproduced
// by truncating to `uint16_t` once, at the top, matching sim_unit_init_record.cpp's identical note
// for its own `player` parameter.
void unit_add(const sim_view &v, sim_store &own, const map_unit_add_calls &c, uint32_t unit,
              int32_t unit_proto, uint16_t player);

} // namespace detail

// Live wrapper: the logic applied to state(). Matches the original's committed
// `__mh_watcall_ebx_volatile` shape (sig_map_unit_Add).
void unit_add(uint32_t unit, int32_t unit_proto, uint16_t player);

namespace detail {
} // namespace detail

} // namespace mh::sim
