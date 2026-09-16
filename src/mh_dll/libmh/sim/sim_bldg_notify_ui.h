#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The one external callee this closure reaches (game_SetEvent @0x00413a52), indirected for offline
// testability like every other sim/ TU's `_calls` struct. Signature copied verbatim from
// addr/mh_calls.gen.h.
struct bldg_notify_ui_calls {
    uint32_t (*set_event)(uint32_t type); // game_SetEvent @0x00413a52
};

const bldg_notify_ui_calls &live_bldg_notify_ui_calls();

namespace detail {

// llm_strat_bldg_notify_ui @0x00470bdd. See the header SHAPE notes above. `own` (not const sim_view
// alone) because branch (1) writes general.change_flag/change_flag2 through the sim_store
// accessors, and both branches read _G_LLM_CLICK_SELECT_TARGET_ID through the existing mutable-only
// accessor `sim_store::click_select_target_id()` (a pure comparison, never written here -- same
// established precedent sim_unit_notify.cpp / sim_unit_on_destroyed.cpp already use for that same
// global, since sim_view carries no const sibling for it).
void bldg_notify_ui(const sim_view &v, sim_store &own, const bldg_notify_ui_calls &c, uint16_t player,
                    uint32_t b_index);

} // namespace detail

// Live wrapper: the logic applied to state() and live_bldg_notify_ui_calls(). Matches the committed
// prototype (sig_llm_strat_bldg_notify_ui) exactly.
void bldg_notify_ui(uint16_t player, uint32_t b_index);

namespace detail {
} // namespace detail

} // namespace mh::sim
