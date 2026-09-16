//
// sim/sim_game_speed_adjust.h -- llm_game_speed_increase / llm_game_speed_decrease, the mirror pair
// that steps one player's per-player game-speed factor up or down against a shared cap/floor (RI-SIM
// / SIM1F).
//
// Two functions, same TU, same shape: llm_game_speed_increase @0x004976cd (0x67 bytes) and
// llm_game_speed_decrease @0x00497734 (0x67 bytes), both llm_strat_order_queue_dispatch order-code
// table arms (order_code 0xe8 / 0xe9). Each reads+writes _G_LLM_GAME_SPEED_PLAYER_FACTOR[player]
// (own.game_speed_player_factor_at(player)) gated against the shared boot-constant cap/floor
// (v.game_speed_factor_max/_min), then -- only on the branch taken -- calls
// llm_game_speed_recompute() and llm_ui_print_game_speed() through the _calls struct below.
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// Indirected for the same reason as every other module here: a direct mh::call:: inside a detail::
// body reaches into the live game image, which makes the body untestable by net_selftest.exe simtest.
// Both members are EFFECTFUL originals (the sim closure's seam table) -- routed through here like
// every other outward call, not suppressed.
struct game_speed_adjust_calls {
    void (*game_speed_recompute)(); // llm_game_speed_recompute   @0x00497623
    void (*ui_print_game_speed)();  // llm_ui_print_game_speed    @0x0044b35e
};

const game_speed_adjust_calls &live_game_speed_adjust_calls();

namespace detail {

// llm_game_speed_increase @0x004976cd. If the player's factor is below the shared cap, multiplies it
// up by the shared step and notifies (recompute + UI refresh); otherwise a no-op.
void game_speed_increase(const sim_view &v, sim_store &own, const game_speed_adjust_calls &c,
                         uint32_t player);

// llm_game_speed_decrease @0x00497734. Mirror: if the player's factor is above the shared floor,
// divides it down by the shared step and notifies; otherwise a no-op.
void game_speed_decrease(const sim_view &v, sim_store &own, const game_speed_adjust_calls &c,
                         uint32_t player);

} // namespace detail

// Live wrappers: the logic applied to state() and live_game_speed_adjust_calls(). Match the
// originals' committed __watcall(EAX) shape.
void game_speed_increase(uint32_t player);
void game_speed_decrease(uint32_t player);


} // namespace mh::sim
