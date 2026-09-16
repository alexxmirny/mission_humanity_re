#pragma once
#include <cstdint>

#include "sim/sim_state.h"
#include "sim/sim_unit_type_predicates.h" // UNIT_TYPE_A_HELI_MOTHER/_H_HELI_MOTHER/_A_HELI_SHUTTLE/_H_HELI_SHUTTLE

namespace mh::sim {

// unit::order == 0x31 ("ASCEND_TO_ORBIT" per sim_order_dispatch.cpp's / sim_storage_launch_parked_
// to_orbit.cpp's own independent pins of this same value) -- climb_vertical's cost-multiplier gate.
inline constexpr uint16_t UNIT_STATE_ASCEND_TO_ORBIT = 0x31;

// unit::state literal 0xcd set by ascend_to_orbit once elevation reaches its cfg cap+300. See the
// DECLARED NEED above: Ghidra's own draft names this token PRODUCTION_READY, not independently
// enum-verified here.
inline constexpr uint16_t UNIT_STATE_PRODUCTION_READY = 0xcd;

// G_TEXT_PTRS[0x1c] (28 decimal) -- ascend_to_orbit's fixed base message text id, read off
// 0x58447c - G_TEXT_PTRS's base 0x58440c. See the SIM-CUT UI message derivation above.
inline constexpr int32_t TEXT_ID_ASCEND_TO_ORBIT_BASE = 0x1c;

// DAT_00501438 / DAT_0050143e, read-memory-confirmed by the conductor (see the batch context) as the
// wide separators " (" and ")" ascend_to_orbit concatenates around the destination planet name.
// Supplied as this translation's own literals (no view-layer binding exists for either DAT_ address).
inline constexpr wchar_t ASCEND_TO_ORBIT_MSG_SEP_OPEN[]  = L" (";
inline constexpr wchar_t ASCEND_TO_ORBIT_MSG_SEP_CLOSE[] = L")";

// ---- the outward calls -----------------------------------------------------------------------
//
// ONE shared calls struct for all three functions in this TU (each detail:: function uses only the
// subset it needs) -- matches sim_unit_type_predicates.h's precedent of one shared struct/binder per
// TU rather than one per function, scaled here to a struct that is non-empty. Signatures copied
// verbatim from addr/mh_calls.gen.h.
struct unit_state_flight_calls {
    void (*unit_set_state)(uint16_t new_state);                      // llm_strat_unit_set_state @0x004866c9
    void (*prod_unbind_planet)(int32_t player, int32_t planet_slot); // @0x0048ff73
    void *(*w_str_copy)(void *src, void *dst);                       // utils_w_str_copy @0x004d02d2
    void *(*concat)(void *dst, void *src);                           // utils_concat @0x004d02ea
    uint32_t (*print_text_message)(void *text);                      // game_ui_PrintTextMessage @0x00496508
};

const unit_state_flight_calls &live_unit_state_flight_calls();

namespace detail {

// llm_strat_unit_state_climb_vertical @0x00481960. See the file banner for the full derivation.
void unit_state_climb_vertical(const sim_view &v, sim_store &own, const unit_state_flight_calls &c);

// llm_strat_unit_state_descend_cruise @0x00481f7c. See the file banner for the full derivation.
void unit_state_descend_cruise(const sim_view &v, sim_store &own, const unit_state_flight_calls &c);

// llm_strat_unit_state_ascend_to_orbit @0x0048214d. See the file banner for the full derivation.
void unit_state_ascend_to_orbit(const sim_view &v, sim_store &own, const unit_state_flight_calls &c);

} // namespace detail

// Live wrappers: the logic applied to state() and live_unit_state_flight_calls(). Match the
// committed void(void) prototypes exactly (no parameters -- ambient cur_player/cur_index/cur_unit).
void unit_state_climb_vertical();
void unit_state_descend_cruise();
void unit_state_ascend_to_orbit();

namespace detail {
} // namespace detail

} // namespace mh::sim
