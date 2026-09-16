#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// ---- this function's own literal operands with no backing Ghidra ENUM TYPE (rule 17a fallback,
// same posture as libmh/ai/ai_construction_plan.cpp's INVENTION_TYPE_BUILDING / libmh/ai/ai_train_plan.h's
// INVENTION_TYPE_UNIT -- NOT reused from either per the "no new shared helpers" rule; this TU defines
// its own copy). Domain: cfg_enum_E_INVETION_TYPE (Ghidra's own spelling, docs/structs.md).
inline constexpr uint8_t INVENTION_TYPE_BUILDING = 1;
inline constexpr uint8_t INVENTION_TYPE_UNIT     = 2;
inline constexpr uint8_t INVENTION_TYPE_PROJECT  = 3;
inline constexpr uint8_t INVENTION_TYPE_PLANET   = 4;
inline constexpr uint8_t INVENTION_TYPE_SYSTEM   = 5;
inline constexpr uint8_t INVENTION_TYPE_UPGRADE  = 6;

// ---- the outward calls (the five non-already-translated siblings; llm_game_notify_system_available
// is reached through its own public wrapper instead, see the header banner above) --------------------
struct handle_progress_calls {
    void (*add_to_available_buildings_with_check)(uint16_t player, int32_t building_index);
    void (*progress_notify_unit_available)(uint16_t player, uint16_t unit_index);
    void (*add_project_to_available_with_check)(uint16_t player, uint32_t project_index);
    void (*add_planet_to_available)(uint16_t player, int32_t planet_id);
    void (*handle_upgrade)(uint32_t player, int32_t upgrade_id);
    void (*update_progress)(uint16_t player, uint16_t inv);
};

const handle_progress_calls &live_handle_progress_calls();

namespace detail {

// game_HandleProgress @0x0043ff0c. See the header banner above for the full derivation; the .cpp
// carries the per-branch address citation. Matches the original's void(player,inv) signature.
void game_handle_progress(const sim_view &v, sim_store &own, const handle_progress_calls &c,
                          uint16_t player, uint16_t inv);

} // namespace detail

// Live wrapper: the logic applied to state() and live_handle_progress_calls(). Matches the committed
// prototype (sig_game_HandleProgress) exactly.
void game_handle_progress(uint16_t player, uint16_t inv);

namespace detail {
} // namespace detail

} // namespace mh::sim
