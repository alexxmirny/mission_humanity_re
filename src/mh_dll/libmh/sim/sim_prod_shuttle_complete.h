#pragma once
#include <cstdint>

#include "sim/sim_state.h"
#include "sim/sim_unit_type_predicates.h" // UNIT_TYPE_A_HELI_MOTHER/_H_HELI_MOTHER (this header)
                                          // + UNIT_TYPE_A_HELI_CARGO/_H_HELI_CARGO (transitively via
                                          // sim_order_enqueue.h) -- existing Ghidra enum members
                                          // (rule 17a), not re-derived locally.

namespace mh::sim {

// ---- this function's own literal operands with no backing Ghidra enum (rule 17a fallback) -----------
inline constexpr int32_t THROTTLE_INCREMENT_CAP        = 5;                          // _G_LLM_STRAT_PROD_COMPLETE_THROTTLE bump stops here
inline constexpr int32_t THROTTLE_RELEASE_THRESHOLD    = 10;                         // exact value gating the quiet-release short-circuit
inline constexpr int16_t STATUS_ARRIVED_READY_TO_SPAWN = static_cast<int16_t>(0xc9); // per the field's own comment

inline constexpr int32_t TEXT_ID_PRODUCTION_COMPLETE = 0x1d; // G_TEXT_PTRS index, base message text

inline constexpr int32_t SND_VOLUME                  = 100;
inline constexpr int32_t SND_PORT_ARRIVAL_BASE       = 0xe;  // cargo-heli arrival voice line base
inline constexpr int32_t SND_MOTHERSHIP_ARRIVAL_BASE = 0xb;  // mothership/other arrival voice line base
inline constexpr int32_t RACE2_SOUND_OFFSET          = 0x12; // added to the base when player_race==2

inline constexpr int32_t CAM_PAN_TARGET_UNSET = -1; // written to COL ONLY when a locator call fails

// ---- the outward calls ---------------------------------------------------------------------------
//
// Indirected for the same reason as every other sim/ TU: a direct mh::call:: inside a detail:: body
// reaches into the live game image, which makes the body untestable by net_selftest.exe simtest.
struct production_complete_calls {
    void *(*w_str_copy)(void *src, void *dst);          // utils_w_str_copy
    void *(*concat)(void *dst, void *src);              // utils_concat
    void (*snd_play)(int32_t sound_id, int32_t volume); // llm_snd_play
    uint32_t (*print_text_message)(void *text);         // game_ui_PrintTextMessage
    uint32_t (*locate_active_port)(uint32_t player, int32_t *out_col, int32_t *out_row,
                                   uint32_t *out_port_slot);
    uint32_t (*find_mothership_position)(int32_t player, uint32_t *out_x, uint32_t *out_y);
    void (*prod_shuttle_slot_release)(int32_t player, int32_t slot);
    void (*prod_deliver_arrivals)();
};

const production_complete_calls &live_production_complete_calls();

namespace detail {

// llm_strat_production_complete @0x0048d886. See the header banner above for the full derivation; the
// .cpp carries the per-branch address citation. Matches the original's void(player,slot,elapsed_time)
// signature.
void prod_shuttle_complete(const sim_view &v, sim_store &own, const production_complete_calls &c,
                           uint32_t player, uint32_t slot, double elapsed_time);

} // namespace detail

// Live wrapper: the logic applied to state() and live_production_complete_calls(). Matches the
// committed prototype (sig_llm_strat_production_complete) exactly.
void prod_shuttle_complete(uint32_t player, uint32_t slot, double elapsed_time);

namespace detail {
} // namespace detail

} // namespace mh::sim
