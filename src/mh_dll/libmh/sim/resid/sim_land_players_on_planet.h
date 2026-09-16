#pragma once
#include <cstdint>

#include "sim/resid/sim_planet_map_session_init.h" // intra-slice DIRECT call (sim_resid rule 2)
#include "sim/sim_state.h"

namespace mh::sim {

// The frontier callees this closure reaches, indirected like every sim/ TU so detail:: stays
// testable under simtest. llm_strat_planet_map_session_init is NOT here -- it is one of the ten
// sim_resid siblings and is called directly as detail::planet_map_session_init(v, own,
// live_planet_map_session_init_calls()) (its own header already carries a `_calls` parameter, so
// per sim_resid rule 2 this TU does not re-bind it as a member here).
struct land_players_on_planet_calls {
    int32_t (*claim_landing_spot)(uint32_t player, uint32_t planet); // llm_strat_claim_landing_spot @0x00454eb2
    uint32_t (*unit_create)(uint32_t x, uint32_t y, uint16_t unit, uint16_t player,
                            uint8_t is_ship); // llm_strat_unit_create @0x00463860
    void (*spawn_ai_base)(int32_t player, int32_t is_alien, int32_t x,
                          int32_t y);             // llm_strat_spawn_ai_base @0x004dcd0e
    void (*landing_spots_reroll_out_of_bounds)(); // llm_strat_landing_spots_reroll_out_of_bounds @0x00454de5
    void (*init_human_player_data)(uint32_t player_idx,
                                   int32_t  is_alien_race); // llm_strat_init_human_player_data @0x004dd91d
    void (*cam_set_col)(int32_t col);                      // llm_map_cam_set_col @0x0044af33
    void (*cam_set_row)(int32_t row);                      // llm_map_cam_set_row @0x0044af6d
    void (*cam_mark_viewport_dirty)();                     // llm_map_cam_mark_viewport_dirty @0x004a5a6b
    int32_t (*get_starting_unit)(uint32_t race);           // game_GetStartingUnit @0x0045eded
    // utils_sprintf__vssii @0x004cfb9c -- the AI-base-layout DMP filename (HUMAN arm only).
    int32_t (*land_dmp_sprintf)(void *dst, const char *format, const char *a0, const char *a1,
                                int32_t a2, int32_t a3);
    // w_sprintf__vii @0x004d0320 -- the "[x,y]" coordinate debug text (tail, unconditional).
    int32_t (*coord_msg_sprintf)(void *dst, const wchar_t *format, int32_t a0, int32_t a1);
};

const land_players_on_planet_calls &live_land_players_on_planet_calls();

namespace detail {

// llm_game_land_players_on_planet @0x0045534e. Reads the player roster / cfg planets / system table
// through `v`, mutates the player roster, the control-group slot, the two outer-planet latches, the
// no-start-unit flag, the UI base-marker coord, and the DMP-path scratch through `own`, and reaches
// every callee (all frontier except the direct planet_map_session_init call) through `c`. void
// return, matching the original.
// ---- the offline-oracle seam (SIM-RESID batch B, 2026-08-31) ----------------------------------
// The trailing `c_*` parameters exist so an OFFLINE ORACLE can substitute a SIBLING's calls table.
// Each is defaulted to the same `live_*_calls()` this body used to name inline, so every production
// caller is unchanged and the default IS the previous behaviour. Why it cannot stay inline:
// `net_selftest.exe` loads no game image, and a `live_*` table holds `mh::call::` naked thunks
// against absolute game VAs -- so a `detail::` body that binds one itself FAULTS before its first
// assertion rather than failing, and the parent's own mock cannot intercept it because the sibling
// is not reached through `c`. TRANSITIVE, not just direct: this closure is three deep, and an
// oracle for the top must be able to mock the bottom. Gated by tools/lint_detail_calls.py.
void land_players_on_planet(
    const sim_view &v, sim_store &own, const land_players_on_planet_calls &c,
    int32_t                              planet_index,
    const planet_map_session_init_calls &c_pmsi = live_planet_map_session_init_calls());

} // namespace detail

// Live wrapper: the logic applied to state() and live_land_players_on_planet_calls(). Parameter type
// matches the committed export/call shape (sig_llm_game_land_players_on_planet in
// addr/mh_export.gen.h): void(uint32_t planet_index).
void land_players_on_planet(uint32_t planet_index);

// ---- THE LANDING OBSERVER (C10, 2026-09-05) -------------------------------------------------------
//
// WHY A SEAM AND NOT A DETOUR ON THE ENTRY, which is what the harness used to rely on. The all-AI
// soak converts the host's player slot HUMAN->AI at landing, and it did that with an
// install_trampoline on llm_game_land_players_on_planet's own entry bytes. That works only while an
// ORIGINAL caller reaches this function. Once llm_strat_session_begin_multi is promoted, OUR body
// calls `detail::land_players_on_planet(...)` DIRECTLY -- the correct shape for a translated closure
// (sim_resid rule 2, intra-slice) -- and a direct C++ call cannot land on a trampoline sitting at the
// callee's game VA. The conversion silently stopped happening, slot 0 stayed HUMAN, and the whole-run
// A/B then compared an 8-way-with-one-human world against an all-AI one and reported a divergence
// that had nothing to do with any body's arithmetic.
//
// So the hook moves onto something the BODY calls, which is route-independent by construction: entry
// seam, harness rebind, or a sibling's direct intra-slice call all reach it.
//
// LAYERING: a setter, for the same reason set_dispatch_observer is one -- a reimplementation TU must
// not include a seams header, so the seam pushes its function in. Pass nullptr to clear.
void set_land_players_observer(void (*fn)());

// What is registered, or nullptr. Exposed so a test can assert the WIRING rather than the pointer's
// existence (the D17 lesson: a pure-function test cannot see whether anything calls it).
void (*land_players_observer())();

// Call the registered observer, if any. `detail::land_players_on_planet`'s FIRST statement, split out
// as its own function exactly as fire_dispatch_observer is: the body needs live game state and cannot
// be driven offline, while this can. IDEMPOTENCE IS THE CALLER'S JOB and the current observer has it
// -- harness.cpp's on_land_players() is one-shot behind g_allai_ran -- which is what makes it safe for
// both the detour AND the body to reach it in a run where an original caller still exists.
void fire_land_players_observer();

} // namespace mh::sim
