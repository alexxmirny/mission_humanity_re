#pragma once
#include <cstdint>

#include "sim/sim_event_codes.h"   // EVENT_INFO_REFRESH
#include "sim/sim_order_enqueue.h" // UNIT_STATE_STOP_TO_DEFAULT
#include "sim/sim_state.h"
#include "sim/sim_unit_type_predicates.h" // UNIT_TYPE_A_HELI_MOTHER / UNIT_TYPE_H_HELI_MOTHER

namespace mh::sim {

// UNIT_STATE_MOVE_WALKER (0xf) -- only ever declared as a per-TU local copy elsewhere in this
// codebase (sim_order_dispatch.cpp), not in a shared header; this TU declares its own per that
// established per-TU-constant convention.
inline constexpr uint16_t UNIT_STATE_MOVE_WALKER = 0x0f;

// ---- the outward calls -----------------------------------------------------------------------
//
// Indirected for the same reason as every other sim/ TU: a direct mh::call:: inside a detail:: body
// reaches into the live game image, making the body untestable by net_selftest.exe simtest. All nine
// are ORIGINAL functions outside this batch. Signatures copied verbatim from addr/mh_calls.gen.h.
struct unit_state_misc2_calls {
    void (*unit_set_state)(uint16_t new_state);                                     // @0x004866c9
    int32_t (*tiles_adjacent)(int32_t x1, int32_t y1, int32_t x2, int32_t y2);      // @0x00498934
    void (*unit_set_state_order)(uint16_t new_order, uint16_t new_state);           // @0x00486657
    int32_t (*path_make_single_step)(uint32_t player, int32_t unit_index);          // @0x0049561f
    void (*unit_unlink_tile)(uint32_t player, uint16_t unit_index);                 // @0x00486f41
    void (*fow_remove_sight)(uint32_t player, int32_t x, int32_t y, uint8_t sight); // @0x00496868
    void (*population_remove)(uint32_t player, int32_t count);                      // @0x00491486
    void (*unit_teardown)(uint32_t player, uint16_t unit_index);                    // @0x00487ba5
    uint32_t (*set_event)(uint32_t type);                                           // @0x00413a52
};

const unit_state_misc2_calls &live_unit_state_misc2_calls();

namespace detail {

// llm_strat_unit_state_enter_class_move @0x00482f2e. See the header derivation above.
void unit_state_enter_class_move(const sim_view &v, sim_store &own, const unit_state_misc2_calls &c);

// llm_strat_unit_state_step_adjacent @0x00485bab. See the header derivation above (in particular
// the CORRECTION note on the draft's control-flow structuring).
void unit_state_step_adjacent(const sim_view &v, sim_store &own, const unit_state_misc2_calls &c);

// llm_strat_unit_state_production_ready @0x00486106. See the header derivation above.
void unit_state_production_ready(const sim_view &v, sim_store &own, const unit_state_misc2_calls &c);

} // namespace detail

// Live wrappers: the logic applied to state() and live_unit_state_misc2_calls(). Match the committed
// prototypes (sig_llm_strat_unit_state_* in addr/mh_export.gen.h) exactly -- all three are zero-arg
// void(void).
void unit_state_enter_class_move();
void unit_state_step_adjacent();
void unit_state_production_ready();

namespace detail {
} // namespace detail

} // namespace mh::sim
