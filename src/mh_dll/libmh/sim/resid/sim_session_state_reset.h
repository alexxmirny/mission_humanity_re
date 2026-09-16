#pragma once
#include <cstdint>

#include "sim/resid/sim_new_game_init.h" // intra-slice DIRECT call (sim_resid rule 2)
#include "sim/sim_state.h"

namespace mh::sim {

// The outward calls, indirected (like every sim/ TU) so detail:: stays testable under simtest. All
// four are frontier originals (none of them are among the ten sim_resid sibling translations).
struct session_state_reset_calls {
    double (*time_get_current_time)();  // time_GetCurrentTime @0x00427616
    void (*cam_jump_queue_clear)();     // llm_cam_jump_queue_clear @0x0044dd11
    void (*invasion_alert_reset_all)(); // llm_strat_invasion_alert_reset_all @0x0049b447
    void (*prod_reset_system)();        // llm_strat_prod_reset_system @0x0048ffe9
};

const session_state_reset_calls &live_session_state_reset_calls();

namespace detail {

// llm_strat_session_state_reset @0x00453ee4. Reads the System[] table through v; rewrites the
// session-clock quintet, the per-planet tables, the per-session flag/byte set, and (soft path) the
// player-profile roster's mother_established through own; reaches the four frontier originals
// through c; calls the intra-slice sibling new_game_init (sim/resid/sim_new_game_init.h) DIRECTLY on
// the full-reset path. void return, matching the original.
// ---- the offline-oracle seam (SIM-RESID batch B, 2026-08-31) ----------------------------------
// The trailing `c_*` parameters exist so an OFFLINE ORACLE can substitute a SIBLING's calls table.
// Each is defaulted to the same `live_*_calls()` this body used to name inline, so every production
// caller is unchanged and the default IS the previous behaviour. Why it cannot stay inline:
// `net_selftest.exe` loads no game image, and a `live_*` table holds `mh::call::` naked thunks
// against absolute game VAs -- so a `detail::` body that binds one itself FAULTS before its first
// assertion rather than failing, and the parent's own mock cannot intercept it because the sibling
// is not reached through `c`. TRANSITIVE, not just direct: this closure is three deep, and an
// oracle for the top must be able to mock the bottom. Gated by tools/lint_detail_calls.py.
void session_state_reset(const sim_view &v, sim_store &own, const session_state_reset_calls &c,
                         int32_t                    reset_flag,
                         const new_game_init_calls &c_ngi = live_new_game_init_calls());

} // namespace detail

// Live wrapper: the logic applied to state() and live_session_state_reset_calls(). Matches the
// original's committed void __watcall llm_strat_session_state_reset(int reset_flag) signature.
void session_state_reset(int32_t reset_flag);

} // namespace mh::sim
