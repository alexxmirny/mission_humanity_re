#pragma once
#include <cstdint>

#include "sim/sim_order_enqueue.h" // UNIT_STATE_PARKED/_EXIT_WAIT/_STOP_TO_DEFAULT,
                                   // BUILDING_TYPE_A_SHUTTLE/_H_SHUTTLE, BUILT_FLAGS_OPERATIONAL
#include "sim/sim_state.h"
#include "sim/sim_unit_type_predicates.h" // UNIT_TYPE_A_GROUND

namespace mh::sim {

// EXIT_STORAGE_BEGIN (0x20) -- see the header banner above (same value as
// sim_unit_force_disembark.cpp's own local UNIT_STATE_EXIT_STORAGE_BEGIN copy; this TU declares its
// own per the established per-TU-constant convention rather than including that unrelated file).
inline constexpr uint16_t UNIT_STATE_EXIT_STORAGE_BEGIN = 0x20;

// ---- the outward calls -----------------------------------------------------------------------
//
// Indirected for the same reason as every other sim/ TU: a direct mh::call:: inside a detail:: body
// reaches into the live game image, making the body untestable by net_selftest.exe simtest. All
// eleven are ORIGINAL functions outside this batch. Signatures copied verbatim from
// addr/mh_calls.gen.h.
struct unit_state_exit_calls {
    int32_t (*storage_can_exit)(int32_t player, uint32_t unit_index, int32_t storage_slot); // @0x0048a548
    void (*storage_place_exit_ground)(uint16_t player, int32_t unit_index,
                                      int32_t storage_slot); // @0x00489f54
    int32_t (*path_find_free_slot)(int32_t player);          // @0x00495f1b
    void (*storage_exit_air)(uint16_t player, int32_t unit_index, int32_t storage_slot,
                             uint32_t free_slot);                                       // @0x0048b01d
    void (*unit_set_state)(uint16_t new_state);                                         // @0x004866c9
    void (*race_alert_text_emit)();                                                     // @0x00425b2a
    int32_t (*storage_type_accepts_unit)(uint32_t building_index, uint16_t unit_index); // @0x00497b91
    double (*dir_step_factor)(int32_t dir);                                             // @0x00449b28
    void (*map_fow_UpdateFoWPlus)(uint32_t player, uint32_t x, uint32_t y,
                                  uint8_t sight); // @0x0049681a
    void (*ai_group_member_count_adjust)(uint32_t player, uint32_t unit_index, uint32_t group_or_type,
                                         uint32_t mode);                  // @0x004db499
    void (*unit_set_state_order)(uint16_t new_order, uint16_t new_state); // @0x00486657
};

const unit_state_exit_calls &live_unit_state_exit_calls();

namespace detail {

// llm_strat_unit_state_exit_storage_begin @0x0047efc6. See the header derivation above.
void unit_state_exit_storage_begin(const sim_view &v, sim_store &own, const unit_state_exit_calls &c);

// llm_strat_unit_state_exit_wait @0x0047f270. See the header derivation above.
void unit_state_exit_wait(const sim_view &v, sim_store &own, const unit_state_exit_calls &c);

// llm_strat_unit_state_exit_cancel @0x0047f351. See the header derivation above.
void unit_state_exit_cancel(const sim_view &v, sim_store &own, const unit_state_exit_calls &c);

// llm_strat_unit_state_exit_walk_out @0x0047f3a1. See the header derivation above (in particular the
// ==2/else CORRECTION note).
void unit_state_exit_walk_out(const sim_view &v, sim_store &own, const unit_state_exit_calls &c);

} // namespace detail

// Live wrappers: the logic applied to state() and live_unit_state_exit_calls(). Match the committed
// prototypes (sig_llm_strat_unit_state_exit_* in addr/mh_export.gen.h) exactly -- all four are
// zero-arg void(void).
void unit_state_exit_storage_begin();
void unit_state_exit_wait();
void unit_state_exit_cancel();
void unit_state_exit_walk_out();

namespace detail {
} // namespace detail

} // namespace mh::sim
