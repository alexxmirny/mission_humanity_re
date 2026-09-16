#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// ---- this function's own literal operand with no backing Ghidra enum (rule 17a fallback) -------------
inline constexpr int32_t TEXT_ID_PLANET_LABEL = 0x78; // G_TEXT_PTRS index, the "Planet" label

// The two external callees this closure reaches, indirected for offline testability like every other
// sim/ TU's `_calls` struct. Signatures copied verbatim from addr/mh_calls.gen.h.
struct add_planet_to_available_calls {
    int32_t (*w_sprintf__vss)(void *dst, const wchar_t *format, const wchar_t *a0, const wchar_t *a1);
    uint32_t (*print_text_message)(void *text); // game_ui_PrintTextMessage
};

const add_planet_to_available_calls &live_add_planet_to_available_calls();

namespace detail {

// game_AddPlanetToAvailable @0x00440208. See the header notes above for the full derivation. `own`
// (not const sim_view alone) because the body writes both `sim_store::text_scratch()` (the shared
// presentation-scratch buffer) and `sim_store::planet_time_at()` (the one real state write).
void game_add_planet_to_available(const sim_view &v, sim_store &own,
                                  const add_planet_to_available_calls &c, uint16_t player,
                                  int32_t planet_id);

} // namespace detail

// Live wrapper: the logic applied to state() and live_add_planet_to_available_calls(). Matches the
// committed prototype (sig_game_AddPlanetToAvailable) exactly.
void game_add_planet_to_available(uint16_t player, int32_t planet_id);

namespace detail {
} // namespace detail

} // namespace mh::sim
