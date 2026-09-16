#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The outward calls, indirected (like every sim/ TU) so detail:: stays testable under simtest. All
// four are Law-4 frontier originals per the batch context, not intra-slice siblings.
struct advisor_tick_calls {
    void *(*w_str_copy)(void *src, void *dst);     // utils_w_str_copy @0x004d02d2
    uint32_t (*ui_print_text_message)(void *text); // game_ui_PrintTextMessage @0x00496508
    int32_t (*bldg_uses_workers)(uint32_t player,
                                 int32_t  building_index); // llm_strat_bldg_uses_workers @0x004988b0
    void (*bldg_get_coords)(uint16_t player, int32_t building_index, int32_t *out_col,
                            int32_t *out_row); // llm_strat_bldg_get_coords @0x00449b8a
};

const advisor_tick_calls &live_advisor_tick_calls();

namespace detail {

// llm_strat_advisor_tick @0x0049b5b0. Reads the roster/storage/cfg inputs through `v`, mutates
// CUR_PLAYER/CUR_INDEX/CUR_BUILDING (the ambient per-building walk pointers)/CAM_PAN_TARGET_COL/ROW/
// FLOATING_MSG_QUEUE_ACTIVE/ADVISOR_PHASE/ADVISOR_NEXT_TIME through `own`, and reaches the two
// effectful UI originals plus the two read-only helper originals through `c`.
void advisor_tick(const sim_view &v, sim_store &own, const advisor_tick_calls &c, double now);

} // namespace detail

// Live wrapper: the logic applied to state() and live_advisor_tick_calls().
void advisor_tick(double now);

} // namespace mh::sim
