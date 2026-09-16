#pragma once
#include <cstdint>

#include "sim/sim_event_codes.h" // SESSION_SP
#include "sim/sim_state.h"

namespace mh::sim {

// The event code. The original's parameter is Ghidra's `game_e_event` enum, but there is no generated
// C++ enum for it in this tree (the no-`::` flatten convention leaves it as a plain 4-byte code at
// call sites -- see sim_unit_on_destroyed.h's note); a uint32_t matches the original's storage exactly
// and is what every sibling game_SetEvent citation passes.
using game_e_event = uint32_t;

// Indirected for the same reason as every other sim/ TU: a direct mh::call:: inside a detail:: body
// reaches into the live game image, which makes the body untestable by net_selftest.exe simtest.
struct game_set_event_calls {
    int32_t (*planet_select_screen_open)(int32_t open_arg); // llm_ui_planet_select_screen_open @0x004cb3f2
    void (*snd_play)(int32_t sound_id, int32_t volume);     // llm_snd_play @0x00425233
};

const game_set_event_calls &live_game_set_event_calls();

namespace detail {

// game_SetEvent @0x00413a52. Reads the panel-state globals through `own` (it is both reader and
// writer of them, within one call), the session/tutorial/roster inputs through `v`, and the two
// effectful callees through `c`. Returns the original's undefined4 result (mostly ignored by callers;
// the defer path returns ring positions, the applied path a fallback-table-derived value).
uint32_t game_set_event(const sim_view &v, sim_store &own, const game_set_event_calls &c,
                        game_e_event type);

} // namespace detail

// Live wrapper: the logic applied to state() and live_game_set_event_calls().
uint32_t game_set_event(game_e_event type);

namespace detail {
} // namespace detail

} // namespace mh::sim
