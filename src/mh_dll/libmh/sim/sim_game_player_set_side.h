#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The one external callee both functions reach, ORIGINAL and already committed in
// addr/mh_calls.gen.h (mh::call::llm_map_fog_of_war_recompute, __watcall, no args) -- indirected for
// offline testability, matching every other sibling `_calls` struct in this subsystem, and per the
// batch context's explicit instruction to route it this way even though it is itself in-flight this
// same slice, in sim_fog_of_war.cpp/another TU.
struct game_player_set_side_calls {
    // llm_map_fog_of_war_recompute @0x00428b11.
    void (*fog_of_war_recompute)();
};

const game_player_set_side_calls &live_game_player_set_side_calls();

namespace detail {

// llm_game_player_set_human @0x0049e759. ORs the player's bit into
// _G_LLM_GAME_HUMAN_PLAYER_MASK, mirrors the whole byte into is_human, then recomputes fog of war.
void game_player_set_human(sim_store &own, const game_player_set_side_calls &c, uint8_t player);

// llm_game_player_set_ai @0x0049e79c. Mirror of game_player_set_human above: ANDs the player's bit
// out of _G_LLM_GAME_HUMAN_PLAYER_MASK, mirrors the whole byte into is_human, then recomputes fog of
// war.
void game_player_set_ai(sim_store &own, const game_player_set_side_calls &c, uint8_t player);

} // namespace detail

// ---- the public surface, in original-behaviour terms -----------------------------------------
// Parameter type matches the COMMITTED prototype (addr/mh_calls.gen.h / addr/mh_export.gen.h) --
// see the header banner above on why it is uint8_t, not a wider int.

void game_player_set_human(uint8_t player);
void game_player_set_ai(uint8_t player);

namespace detail {
} // namespace detail

} // namespace mh::sim
