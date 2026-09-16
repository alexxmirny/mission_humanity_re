#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// ---- this function's own literal operands with no backing Ghidra enum (rule 17a) ---------------------
inline constexpr uint16_t UNIT_STATE_DIE_EXPLODE        = 0x02; // early-out + the branch this fires
inline constexpr uint16_t UNIT_STATE_CORPSE_FOW_DECAY   = 0x04; // early-out, second value
inline constexpr uint16_t UNIT_STATE_DEPLOY_TO_BUILDING = 0x17; // gates the footprint-unmap chain

// llm_strat_ai_notify_object_removed's packed `flags` argument: (cur_player | 0x80). The 0x80 bit's own
// meaning is not established anywhere this translation could check (no sibling caller of this exact
// callee found in the tree yet) -- named only to keep the literal out of the call site per rule 17a;
// see uncertainties[].
inline constexpr uint32_t OBJECT_REMOVED_FLAG_UNIT = 0x80u;

// ---- the outward calls ---------------------------------------------------------------------------
struct unit_apply_damage_calls {
    void (*unit_set_state)(uint16_t new_state);
    void (*ai_notify_object_removed)(uint32_t flags, uint32_t object_index, int32_t hard_remove);
    void (*unit_state_die_explode)();
    void (*unit_soldier_remove_last)(uint32_t player, int32_t unit_index);
    void (*bldg_footprint_set_passable)(int32_t tile_x, int32_t tile_y, int32_t building_idx);
    void (*unit_update_damage_smoke)(uint32_t player, int32_t unit_idx);
    void (*unit_notify_ui)(uint32_t side, uint32_t unit_index);
};

const unit_apply_damage_calls &live_unit_apply_damage_calls();

namespace detail {

// llm_strat_unit_apply_damage @0x0047e380. See the header banner above for the full derivation; the
// .cpp carries the per-line address citation. No parameters -- operates on `v`/`own`'s ambient
// cur_unit/cur_player/cur_index alone, matching the original's void(void) signature.
void unit_apply_damage(const sim_view &v, sim_store &own, const unit_apply_damage_calls &c);

} // namespace detail

// Live wrapper: the logic applied to state() and live_unit_apply_damage_calls(). Matches the committed
// prototype (sig_llm_strat_unit_apply_damage) exactly.
void unit_apply_damage();

namespace detail {
} // namespace detail

} // namespace mh::sim
