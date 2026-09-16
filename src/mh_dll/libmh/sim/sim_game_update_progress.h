#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The three outward calls this closure reaches (see the header banner's CALLEES section). Indirected
// for the same testability reason as every other sim/ TU's `_calls` struct.
struct game_update_progress_calls {
    int32_t (*count_landing_spots)();                        // llm_strat_count_landing_spots @0x00454d8d
    void (*finalize_acquire)(uint16_t player, uint16_t inv); // llm_progress_finalize_acquire @0x004404cc
    void (*propagate_unlocks)(uint16_t player);              // llm_progress_propagate_unlocks @0x00440cb5
};

const game_update_progress_calls &live_game_update_progress_calls();

namespace detail {

// game_UpdateProgress @0x004402b0. See the header banner above for the full per-branch derivation.
void game_update_progress(const sim_view &v, sim_store &own, const game_update_progress_calls &c,
                          uint16_t plr, uint16_t inv);

} // namespace detail

// Live wrapper: the logic applied to state() and live_game_update_progress_calls(). Matches the
// committed export prototype (sig_game_UpdateProgress) exactly.
void game_update_progress(uint16_t plr, uint16_t inv);

namespace detail {
} // namespace detail

} // namespace mh::sim
