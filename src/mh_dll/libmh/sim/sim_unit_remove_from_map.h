#pragma once
#include <cstdint>

#include "sim/sim_event_codes.h" // SESSION_MP_LOCKSTEP, EVENT_INFO_REFRESH -- shared with sim_unit_on_destroyed.h
#include "sim/sim_state.h"

namespace mh::sim {

// ---- the outward calls -------------------------------------------------------------------------
//
// Indirected for the same reason as sim_unit_ctrl_group.h's table: a direct mh::call:: inside a
// detail:: body reaches into the live game image, which makes the body untestable by
// net_selftest.exe simtest. All four are ORIGINAL functions outside this batch; game_SetEvent's return
// value (a bool-ish status) is discarded at every call site in the assembly, so the pointer keeps the
// real `uint32_t` return type (matching mh_calls.gen.h's own signature) rather than forcing a `void`
// adapter -- callers simply don't use the result.
struct unit_remove_from_map_calls {
    void (*population_remove)(uint32_t player, int32_t count); // llm_strat_population_remove
    void (*fow_remove_sight)(uint32_t player, int32_t x, int32_t y,
                             uint8_t radius);     // llm_strat_fow_remove_sight
    void (*ctrlgroup_leave)(uint32_t unit_index); // llm_strat_unit_ctrlgroup_leave
    uint32_t (*set_event)(uint32_t type);         // game_SetEvent
};

const unit_remove_from_map_calls &live_unit_remove_from_map_calls();

namespace detail {

// llm_strat_unit_remove_from_map @0x00487252. See the header derivation above for the two-way split,
// the soldier-unlink loop, the population-remove branch, and the shared tail. No return value.
void unit_remove_from_map(const sim_view &v, sim_store &own, const unit_remove_from_map_calls &c,
                          uint16_t player, uint32_t unit_idx);

} // namespace detail

// Live wrapper: the logic applied to state() and live_unit_remove_from_map_calls(). Matches the
// original's committed __watcall(EAX,EDX) shape (sig_llm_strat_unit_remove_from_map).
void unit_remove_from_map(uint16_t player, uint32_t unit_idx);

namespace detail {
} // namespace detail

} // namespace mh::sim
