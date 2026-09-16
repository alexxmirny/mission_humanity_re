#pragma once
#include <cstdint>

#include "sim/sim_game_set_event.h" // game_e_event / game_set_event_calls / detail::game_set_event -- threaded sibling (rule 3c)
#include "sim/sim_state.h"

namespace mh::sim {

// NOTE: this function's own INVENTION_TYPE_* / BUILD_*_REFRESH literal operands (rule 17a fallback,
// no backing Ghidra ENUM TYPE) are defined in the .cpp's anonymous namespace, not here -- see the
// header banner's "THE cfg_enum_E_INVETION_TYPE DOMAIN" section for why (ODR-safety, not semantics).

// The two RemoveFromAvailable* outward calls, indirected so detail:: stays testable under
// net_selftest.exe libtranstest (a direct `mh::call::` inside detail:: reaches into the live game
// image and faults offline).
struct lt_progress_finalize_calls {
    void (*remove_from_available_buildings)(uint32_t player, int32_t b_i); // game_RemoveFromAvailableBuildings @0x004141e8
    void (*remove_from_available_projects)(uint32_t player, uint32_t p_i); // game_RemoveFromAvailableProjects @0x0041428e
};

const lt_progress_finalize_calls &live_lt_progress_finalize_calls();

namespace detail {

// llm_progress_finalize_acquire @0x004404cc. See the header banner above for the full per-branch
// derivation. `c_set_event` is threaded per rule 3c so an offline oracle can substitute a mock
// game_set_event_calls table without this body binding `live_game_set_event_calls()` itself; every
// production caller is unchanged because the default IS the previous behaviour.
//
// SELF-RECURSIVE: the SYSTEM arm calls this same function again, with the same `v`/`own`/`c`/
// `c_set_event` and the same `player`, for a new `pid`. See the header banner's arming note -- this
// is exactly why the ORIGINAL site is not shadow-armed; the C++ recursion itself is ordinary.
void progress_finalize_acquire(const sim_view &v, sim_store &own, const lt_progress_finalize_calls &c,
                               uint16_t player, uint16_t pid,
                               const game_set_event_calls &c_set_event = live_game_set_event_calls());

} // namespace detail

// Live wrapper: the logic applied to state() and live_lt_progress_finalize_calls(). Matches the
// already-committed callable `mh::call::llm_progress_finalize_acquire` / the committed export
// prototype `sig_llm_progress_finalize_acquire` exactly (`void(uint16_t player, uint16_t pid)`).
void progress_finalize_acquire(uint16_t player, uint16_t pid);


} // namespace mh::sim
