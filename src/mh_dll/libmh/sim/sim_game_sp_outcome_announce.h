#pragma once
#include <cstdint>

#include "sim/sim_event_codes.h" // SESSION_SP
#include "sim/sim_state.h"

namespace mh::lockstep {
struct overlay_hoist_ops; // LIB-ABI stage E -- lockstep/overlay_hoist.h
}

namespace mh::sim {

// ---- this function's own literal operand with no backing Ghidra enum (rule 17a fallback) --------
inline constexpr int32_t TEXT_ID_SP_OUTCOME = 0x7e; // llm_ui_print_queue_text_id arg

inline constexpr int32_t SND_SP_OUTCOME_ID     = 0xa6; // llm_snd_play sound id, fixed (no race/player variance)
inline constexpr int32_t SND_SP_OUTCOME_VOLUME = 100;  // llm_snd_play volume

inline constexpr uint8_t OUTCOME_DIALOG_KIND = 4; // llm_ui_outcome_dialog arg

// ---- the outward calls ---------------------------------------------------------------------------
//
// Indirected for the same reason as every other sim/ TU: a direct mh::call:: inside a detail:: body
// reaches into the live game image, which makes the body untestable by net_selftest.exe simtest.
struct game_sp_outcome_announce_calls {
    void (*ui_print_queue_text_id)(int32_t text_id);    // llm_ui_print_queue_text_id
    void (*snd_play)(int32_t sound_id, int32_t volume); // llm_snd_play
    int32_t (*ui_outcome_dialog)(uint8_t outcome);      // llm_ui_outcome_dialog, return discarded here
    // LIB-ABI stage E hoist ops (lockstep/overlay_hoist.h) -- tail member, null-skipped by
    // suites, which is what keeps the R3b panel hoist out of the offline arm where there is
    // no game process and therefore no panel state to keep consistent.
    const mh::lockstep::overlay_hoist_ops *hoist;
};

const game_sp_outcome_announce_calls &live_game_sp_outcome_announce_calls();

namespace detail {

// llm_game_sp_outcome_announce @0x0049803b. See the header banner above for the full derivation.
// No parameters, no state writes; `own` is unused (present only for shape parity with every other
// detail:: body in this module).
void game_sp_outcome_announce(const sim_view &v, sim_store &own,
                              const game_sp_outcome_announce_calls &c);

} // namespace detail

// Live wrapper: the logic applied to state() and live_game_sp_outcome_announce_calls(). Matches the
// committed prototype (sig_llm_game_sp_outcome_announce) exactly.
void game_sp_outcome_announce();

namespace detail {
} // namespace detail

} // namespace mh::sim
