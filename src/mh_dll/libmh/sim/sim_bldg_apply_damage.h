#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// ---- this function's own literal operand with no backing Ghidra enum (rule 17a fallback) -------------
// state@0xd's DESTROYED member -- see the header banner above for why this duplicates, rather than
// reuses, sim_state.h's sibling BLDG_STATE_* constants (declared_need: hoist it there).
inline constexpr uint16_t BLDG_STATE_DESTROYED = 2; // llm_strat_bldg_state::DESTROYED

// ---- the outward calls ---------------------------------------------------------------------------
//
// Indirected for the same reason as every other sim/ TU: a direct mh::call:: inside a detail:: body
// reaches into the live game image, which makes the body untestable by net_selftest.exe simtest.
struct bldg_apply_damage_calls {
    void (*ai_notify_object_removed)(uint32_t flags, uint32_t object_index, int32_t hard_remove);
    void (*bldg_state_destroyed)();
    void (*bldg_update_charge_pips)(uint16_t player, uint32_t building_id);
    void (*refresh_building)(uint16_t p_id, int32_t b_id);
    void (*bldg_notify_ui)(uint16_t player, uint32_t b_index);
};

const bldg_apply_damage_calls &live_bldg_apply_damage_calls();

namespace detail {

// llm_strat_bldg_apply_damage @0x004710ba. See the header banner above for the full derivation; the
// .cpp carries the per-line address citation. No parameters -- operates on `v`/`own`'s ambient
// cur_building/cur_player/cur_index alone, matching the original's void(void) signature.
void bldg_apply_damage(const sim_view &v, sim_store &own, const bldg_apply_damage_calls &c);

} // namespace detail

// Live wrapper: the logic applied to state() and live_bldg_apply_damage_calls(). Matches the committed
// prototype (sig_llm_strat_bldg_apply_damage) exactly.
void bldg_apply_damage();

namespace detail {
} // namespace detail

} // namespace mh::sim
