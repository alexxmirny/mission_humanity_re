#pragma once
#include <cstdint>

#include "sim/sim_event_codes.h" // SESSION_MP_LOCKSTEP, EVENT_INFO_REFRESH
#include "sim/sim_state.h"

namespace mh::sim {

// unit::state == 4, this function's own literal (0x0048777d's EBX operand, matching
// sim_unit_teardown.cpp's identically-valued but SEPARATELY-declared UNIT_TEARDOWN_STATE_CORPSE_FOW_
// DECAY -- see the DECLARED NEED above on why this file does not reuse that name).
inline constexpr int16_t UNIT_TEARDOWN_MAPPED_STATE_CORPSE_FOW_DECAY = 4;

// The slot-[0] live-unit accumulator step for THIS function's own FADD site, 0x00501482
// (conductor-confirmed via ReVA read-memory: IEEE-754 double bytes for exactly -1.0).
// SAME VALUE as sim_unit_teardown.cpp's UNIT_TEARDOWN_LIVE_ACCUM_STEP (DAT_0050148a) but a DIFFERENT,
// separately compiler-emitted anonymous double literal at a DIFFERENT address -- declared as its own
// constant rather than aliased/reused, per the batch context's explicit instruction.
inline constexpr double UNIT_TEARDOWN_MAPPED_ENERGY_DELTA = -1.0; // DAT_00501482

// ---- the outward calls -----------------------------------------------------------------------
//
// Indirected for the same reason as every other sim/ TU: a direct mh::call:: inside a detail:: body
// reaches into the live game image, making the body untestable by net_selftest.exe simtest. All seven
// are ORIGINAL functions outside this batch -- none need reimplementing here; live_unit_teardown_
// mapped_calls() is the only binder. Signatures copied verbatim from addr/mh_calls.gen.h (checked
// against this function's own register-order use in the .cpp).
struct unit_teardown_mapped_calls {
    void (*path_free_slot)(uint16_t player, int32_t unit_index);                     // llm_strat_path_free_slot
    void (*fow_remove_sight)(uint32_t player, int32_t x, int32_t y, uint8_t radius); // llm_strat_fow_remove_sight
    void (*unit_ctrlgroup_leave)(uint32_t unit_index);                               // llm_strat_unit_ctrlgroup_leave
    uint32_t (*set_event)(uint32_t type);                                            // game_SetEvent
    uint32_t (*player_presence_lost)(uint32_t player, uint32_t mode);                // llm_strat_player_presence_lost
    void (*unit_set_state_of)(int32_t player, int32_t unit_index, int16_t state);    // llm_strat_unit_set_state_of
    void (*ai_notify_object_removed)(uint32_t flags, uint32_t object_index,
                                     int32_t hard_remove); // llm_strat_ai_notify_object_removed
};

const unit_teardown_mapped_calls &live_unit_teardown_mapped_calls();

namespace detail {

// llm_strat_unit_teardown_mapped @0x0048753e. See the header derivation above for the full 11-step
// shape.
void unit_teardown_mapped(const sim_view &v, sim_store &own, const unit_teardown_mapped_calls &c,
                          uint32_t player, uint32_t unit_index);

} // namespace detail

// Live wrapper: the logic applied to state() and live_unit_teardown_mapped_calls(). Matches the
// original's committed __watcall(EAX,EDX) shape (sig_llm_strat_unit_teardown_mapped).
void unit_teardown_mapped(uint32_t player, uint32_t unit_index);

namespace detail {
} // namespace detail

} // namespace mh::sim
