#pragma once
#include <cstdint>

#include "sim/sim_order_enqueue.h" // UNIT_STATE_STOP_TO_DEFAULT, UNIT_STATE_PARKED, BUILDING_TYPE_A_GARAGE/
                                   // _A_SHUTTLE/_H_GARAGE -- all already pinned there, reused rather than
                                   // re-declared (would be a redefinition otherwise).
#include "sim/sim_state.h"

namespace mh::sim {

// The state-family literals this TU transitions through -- see the header banner's state-family note
// for the full 12-value set sim_dock_slot_is_busy.h independently established. No backing Ghidra enum;
// own local names per this cluster's established convention. UNIT_STATE_STOP_TO_DEFAULT/UNIT_STATE_
// PARKED are reused from sim_order_enqueue.h (already shared there).
inline constexpr uint16_t ENTER_ARRIVAL_CHECK_STATE_GROUP_MARSHAL       = 0x0a;
inline constexpr uint16_t ENTER_ARRIVAL_CHECK_STATE_ENTER_STORAGE_BEGIN = 0x24;
inline constexpr uint16_t ENTER_STORAGE_BEGIN_STATE_ENTER_WAIT          = 0x25;
inline constexpr uint16_t ENTER_WAIT_STATE_ENTER_STORAGE_BEGIN          = 0x24;

// CONDUCTOR RESOLUTION (2026-08-21): the two boot-constant doubles this TU needed (DAT_00501400,
// DAT_00501408) are now read-memory-confirmed, named, typed and bound as sim_view members --
// v.enter_wait_activity_backoff_seconds (_G_LLM_STRAT_UNIT_ENTER_WAIT_ACTIVITY_BACKOFF, 0.05) and
// v.enter_walk_in_soldier_transport_mult (_G_LLM_STRAT_UNIT_ENTER_WALK_SOLDIER_STEP_SCALE, 0.5) --
// see sim_state.h/.cpp. The detail:: bodies read them through the view; no local placeholders remain.

// ---- the outward calls -----------------------------------------------------------------------
//
// ONE shared calls struct for all four functions in this TU (each detail:: function uses only the
// subset it needs) -- matches sim_unit_state_flight.h's precedent of one shared struct/binder per TU.
// Signatures copied verbatim from addr/mh_calls.gen.h.
struct unit_state_enter_calls {
    void (*unit_set_state)(uint16_t new_state); // llm_strat_unit_set_state @0x004866c9
    int32_t (*storage_can_enter)(uint16_t player, uint32_t unit_index,
                                 uint32_t storage_slot);                                  // @0x0048a808
    void (*storage_board_unit)(uint16_t player, uint32_t unit_idx, uint32_t storage_idx); // @0x0048b6ba
    void (*unit_queue_advance)(uint32_t player, uint32_t unit_index);                     // @0x004dbf27
    int32_t (*dir_from_to)(int32_t x1, int32_t y1, int32_t x2, int32_t y2);               // @0x0049482b
    int32_t (*unit_walk_step_allowed)(uint32_t player, int32_t unit_index);               // @0x0048c79a
    void (*unit_soldiers_set_heading)(uint16_t player, int32_t unit_index,
                                      uint8_t sprite_frame);                          // @0x00489ab6
    double (*dir_step_factor)(int32_t dir);                                           // @0x00449b28
    void (*target_release_ref)(uint32_t player_idx, int32_t unit_idx, uint32_t mode); // @0x004dac44
    void (*path_free_slot)(uint16_t player, int32_t unit_index);                      // @0x004969e8
};

const unit_state_enter_calls &live_unit_state_enter_calls();

namespace detail {

// llm_strat_unit_state_enter_arrival_check @0x0047fe68. See the header banner. Read-only over `v`
// (writes nothing tracked -- see the NOT SHADOWABLE note), so no sim_store parameter.
void unit_state_enter_arrival_check(const sim_view &v, const unit_state_enter_calls &c);

// llm_strat_unit_state_enter_storage_begin @0x0047ff50. See the header banner.
void unit_state_enter_storage_begin(const sim_view &v, sim_store &own, const unit_state_enter_calls &c);

// llm_strat_unit_state_enter_wait @0x00480218. See the header banner.
void unit_state_enter_wait(const sim_view &v, sim_store &own, const unit_state_enter_calls &c);

// llm_strat_unit_state_enter_walk_in @0x004803bf. See the header banner.
void unit_state_enter_walk_in(const sim_view &v, sim_store &own, const unit_state_enter_calls &c);

} // namespace detail

// Live wrappers: the logic applied to state() and live_unit_state_enter_calls(). Match the committed
// void(void) prototypes exactly (no parameters -- ambient cur_player/cur_index/cur_unit).
void unit_state_enter_arrival_check();
void unit_state_enter_storage_begin();
void unit_state_enter_wait();
void unit_state_enter_walk_in();

namespace detail {
} // namespace detail

} // namespace mh::sim
