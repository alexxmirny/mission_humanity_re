#pragma once
#include <cstdint>

#include "sim/sim_order_enqueue.h" // BUILDING_TYPE_A_TURRET (0x05) / BUILDING_TYPE_H_TURRET (0x19)
#include "sim/sim_state.h"

namespace mh::sim {

// ---- this file's own literal operands, sourced from the Ghidra .c drafts' resolved
// llm_strat_bldg_state enum member names (rule 17a: existing enum, not yet a real C++ type -- see
// the declared_needs note above). NOT re-derived/invented -- transcribed from tmp/decomp_sim's
// llm_strat_bldg_state_default_reset_004711c3.c / llm_strat_bldg_state_idle_activate_00472415.c.
inline constexpr uint16_t BLDG_STATE_IDLE_ACTIVATE = 1;    // Ghidra: IDLE_ACTIVATE
inline constexpr uint16_t BLDG_STATE_TURRET_SCAN   = 0x7a; // Ghidra: TURRET_SCAN
inline constexpr uint16_t BLDG_STATE_IDLE_NOOP_88  = 0x88; // Ghidra: IDLE_NOOP_88 (see header note)

// The one external callee this closure reaches (default_reset and idle_activate only), indirected
// for offline testability -- same reason as every other sim/ TU: a direct mh::call:: inside a
// detail:: body reaches into the live game image, which makes the body untestable by
// net_selftest.exe simtest.
struct bldg_state_reset_idle_calls {
    void (*bldg_notify_ui)(uint16_t player, uint32_t b_index);
};

const bldg_state_reset_idle_calls &live_bldg_state_reset_idle_calls();

namespace detail {

// llm_strat_bldg_state_default_reset @0x004711c3. See the header derivation above.
void bldg_state_default_reset(const sim_view &v, sim_store &own, const bldg_state_reset_idle_calls &c);

// llm_strat_bldg_state_idle_noop @0x00471203. No `calls` struct -- makes no outward call besides the
// inert stack probe, same "no _calls struct" shape as sim_unit_state_budget_noop.cpp's pair.
void bldg_state_idle_noop(sim_store &own);

// llm_strat_bldg_state_idle_activate @0x00472415. See the header derivation above.
void bldg_state_idle_activate(const sim_view &v, sim_store &own, const bldg_state_reset_idle_calls &c);

} // namespace detail

// Live wrappers: the logic applied to state() and live_bldg_state_reset_idle_calls(). Match the
// originals' committed void(void) prototypes exactly.
void bldg_state_default_reset();
void bldg_state_idle_noop();
void bldg_state_idle_activate();

namespace detail {
} // namespace detail

} // namespace mh::sim
