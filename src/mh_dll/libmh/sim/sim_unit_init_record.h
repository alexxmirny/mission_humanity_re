#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// game::e::event member 14 (the strategic-sim notes' resolved 25-member table) -- see the header's
// DECLARED NEED above. Named locally, not in sim_event_codes.h, per the "write only your own new
// files" rule; sim_event_codes.h already carries EVENT_INFO_REFRESH (member 6) for its own two
// callers and is a natural future home for this one too.
inline constexpr uint32_t MAP_OBJECTS_REFRESH = 14;

// ---- the outward calls -----------------------------------------------------------------------
//
// Indirected for the same reason as every other multi-callee TU in this batch (sim_unit_on_destroyed.h
// et al.): a direct mh::call:: inside a detail:: body reaches into the live game image, which makes
// the body untestable by net_selftest.exe simtest AND is invisible to the translation lint's
// deterministic callee check (both look for `c.<member>`, not `mh::call::<fn>`). Originally written
// as direct calls per an earlier draft of this file's own brief citing sim_unit_create_soldier.cpp's
// style; refactored to this shape before landing, matching sim_unit_spawn_on_tile.cpp's own precedent
// for the identical fix. All three callees are ORIGINAL functions outside this batch.
struct init_record_calls {
    void *(*fill_data)(void *ptr, uint32_t size, uint8_t default_);
    void (*cursor_lookup_offset_pair)(int32_t table_col, int32_t table_row, char *out_a, char *out_b);
    uint32_t (*set_event)(uint32_t type);
};

const init_record_calls &live_init_record_calls();

namespace detail {

// llm_strat_unit_init_record @0x004619b0. See the header hazards above for the full derivation.
// `player` arrives as a full 32-bit register but the ORIGINAL reads it back via a 16-bit (word) load
// at EVERY address computation in the body (unlike `unit_idx`/`unit_proto_id`, which are read back
// full-width) -- reproduced by truncating to `uint16_t` once, at the top, and indexing with that.
void init_record(const sim_view &v, sim_store &own, const init_record_calls &c, int32_t unit_idx,
                 uint32_t unit_proto_id, uint32_t player);

} // namespace detail

// Live wrapper: the logic applied to state(). Matches the original's committed
// `__mh_watcall_ebx_volatile` shape (sig_llm_strat_unit_init_record).
void unit_init_record(int32_t unit_idx, uint32_t unit_proto_id, uint32_t player);

namespace detail {
} // namespace detail

} // namespace mh::sim
