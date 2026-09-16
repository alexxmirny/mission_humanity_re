#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// ---- the outward call llm_strat_unit_notify_ui makes -----------------------------------------
//
// Indirected for the same reason as sim_unit_mount_pos.h's mount_pos_calls / sim_unit_on_destroyed.h's
// unit_on_destroyed_calls: a direct mh::call:: inside a detail:: body reaches into the live game
// image, which makes the body untestable by net_selftest.exe simtest. game_SetEvent is itself a
// not-yet-translated sim-migration-set function (layer 11, a different batch) -- per the translator
// brief's callee rule it stays original rather than being reimplemented here. Signature copied
// verbatim from addr/mh_calls.gen.h.
struct notify_ui_calls {
    uint32_t (*set_event)(uint32_t type);
};

const notify_ui_calls &live_notify_ui_calls();

namespace detail {

// llm_strat_unit_notify_ui @0x00488a22. See the header SHAPE / HAZARD notes above. `own` is taken
// (not const sim_view alone) because the local-player arm writes general.change_flag through the
// DECLARED-NEED accessor above; the click-select id is READ (not written) through the same store,
// matching the existing precedent cited above.
void notify_ui(const sim_view &v, sim_store &own, const notify_ui_calls &c, uint32_t side,
               uint32_t unit_index);

// llm_strat_unit_notify_status @0x004dae0e. See the header SHAPE notes above. No outward calls, so
// no `calls` struct -- pure state machine over `v`/`own`.
void notify_status(const sim_view &v, sim_store &own, uint32_t player, int32_t unit_index,
                   uint32_t status_code);

} // namespace detail

// Live wrappers: the logic applied to state() and (for notify_ui) live_notify_ui_calls(). Match the
// originals' committed __watcall(AX,EDX) / __watcall(AX,EDX,EBX) shapes
// (sig_llm_strat_unit_notify_ui / sig_llm_strat_unit_notify_status).
void notify_ui(uint32_t side, uint32_t unit_index);
void notify_status(uint32_t player, int32_t unit_index, uint32_t status_code);

namespace detail {
} // namespace detail

} // namespace mh::sim
