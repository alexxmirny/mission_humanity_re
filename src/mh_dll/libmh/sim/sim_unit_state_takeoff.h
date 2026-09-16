#pragma once
#include <cstdint>

#include "sim/sim_order_enqueue.h" // BUILDING_TYPE_A_HELIPAD/_H_HELIPAD, UNIT_TYPE_A_HELI/_H_HELI/
                                   // _A_HELI_CARGO/_H_HELI_CARGO
#include "sim/sim_state.h"

namespace mh::sim {

// llm_strat_unit_state_takeoff_landing's OWN dispatch key (unit.state, +0x6) -- this single handler
// is bound at BOTH slots 0x15 and 0x16 in _G_LLM_STRAT_UNIT_STATE_FUNCS and re-reads its own state to
// tell which invoked it (see the header derivation above). No committed llm_strat_unit_state ENUM
// members exist for these two, nor for the 0x2a/0x2b dock-taxi states below (grepped, absent);
// declared locally as this TU's own constants, same per-TU-constant convention
// sim_unit_state_exit.h's UNIT_STATE_EXIT_STORAGE_BEGIN uses. The 0x15/0x16 VALUES are independently
// corroborated by two sibling TUs' own local constants of the SAME value (not shared here because
// each is itself a private per-TU copy): sim_order_dispatch.cpp's anonymous-namespace
// UNIT_STATE_TAKEOFF=0x15/UNIT_STATE_LANDING=0x16, and sim_unit_state_taxi_dock.h's
// TAXI_DOCK_STATE_TAKEOFF=0x15 ("takeoff_taxi's terminal state" -- i.e. that sibling function's OWN
// climb-taxi phase transitions the unit INTO this state, which is what this file's handler then
// drives). Neither sibling names 0x2a/0x2b either.
//
// WARNING: 0x15/0x16 COINCIDENTALLY equal sim_unit_type_predicates.h's UNIT_TYPE_A_HELI_SHUTTLE/
// UNIT_TYPE_H_HELI_SHUTTLE -- a completely DIFFERENT domain (cfg_unit.type, not unit.state). Do not
// conflate; this file never includes sim_unit_type_predicates.h for exactly that reason.
inline constexpr uint16_t UNIT_STATE_TAKEOFF     = 0x15;
inline constexpr uint16_t UNIT_STATE_LANDING     = 0x16;
inline constexpr uint16_t UNIT_STATE_DOCK_TAXI_A = 0x2a; // set on any non-H_HELIPAD dock building.
inline constexpr uint16_t UNIT_STATE_DOCK_TAXI_H = 0x2b; // set when the dock building is H_HELIPAD.

// ---- the outward calls -----------------------------------------------------------------------
//
// Indirected for the same reason as every other sim/ TU: a direct mh::call:: inside a detail:: body
// reaches into the live game image, making the body untestable by net_selftest.exe simtest. All nine
// are ORIGINAL functions outside this batch. Signatures copied verbatim from addr/mh_calls.gen.h.
struct unit_state_takeoff_calls {
    void (*unit_set_state)(uint16_t new_state);                          // @0x004866c9
    void (*unit_unlink_tile)(uint32_t unit_player, uint16_t unit_index); // @0x00486f41
    void (*storage_dock_list_append)(uint16_t player, uint16_t unit_idx,
                                     uint16_t storage_slot); // @0x0048b5a7
    void (*ai_group_member_count_adjust)(uint32_t player, uint32_t unit_index, uint32_t group_or_type,
                                         uint32_t mode);                             // @0x004db499
    uint32_t (*game_SetEvent)(uint32_t type);                                        // @0x00413a52
    void (*fow_remove_sight)(uint32_t player, int32_t x, int32_t y, uint8_t radius); // @0x00496868
    void (*map_unit_PutOnMap)(uint16_t player, uint16_t b_id, uint8_t x, uint8_t y); // @0x00486c6a
    void (*map_fow_UpdateFoWPlus)(uint32_t player, uint32_t x, uint32_t y,
                                  uint8_t sight); // @0x0049681a
    void (*storage_remove_docked_unit)(uint16_t player, int32_t unit_index,
                                       int32_t storage_slot); // @0x00489dc4
};

const unit_state_takeoff_calls &live_unit_state_takeoff_calls();

namespace detail {

// llm_strat_unit_state_takeoff_landing @0x004813f1. AMBIENT (reads CUR_PLAYER/CUR_INDEX/CUR_UNIT).
// See the header derivation above.
void unit_state_takeoff_landing(const sim_view &v, sim_store &own, const unit_state_takeoff_calls &c);

// llm_strat_unit_takeoff_finalize @0x0048b162. EXPLICIT (player, unit_index) -- no CUR_* reads.
// See the header derivation above.
void unit_takeoff_finalize(const sim_view &v, sim_store &own, const unit_state_takeoff_calls &c,
                           uint16_t player, int32_t unit_index);

} // namespace detail

// Live wrappers: the logic applied to state() and live_unit_state_takeoff_calls(). Match the
// committed prototypes (sig_llm_strat_unit_state_takeoff_landing / sig_llm_strat_unit_takeoff_finalize
// in addr/mh_export.gen.h) exactly -- the first is zero-arg void(void), the second void(uint32_t,
// uint32_t).
void unit_state_takeoff_landing();
void unit_takeoff_finalize(uint32_t player, uint32_t unit_index);

namespace detail {
} // namespace detail

} // namespace mh::sim
