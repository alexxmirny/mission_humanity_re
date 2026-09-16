//
// sim/resid/sim_session_clear_presence_flag.cpp -- see sim_session_clear_presence_flag.h. Translated
// from the DISASSEMBLY
// (tmp/decomp_sim_resid/llm_game_session_clear_system_presence_flag_004987ae.asm), the Ghidra `.c`
// being a draft (and, here, a wrong one -- see the header banner's `.asteriods` vs `.enemy` note).
//
#include "sim/resid/sim_session_clear_presence_flag.h"

namespace mh::sim {

namespace {

// player_profile.status_flags bits (E_STRAT_PLAYER_STATUS) -- b1 ALIVE. Same spelling as
// sim/sim_player_presence_lost.cpp's own local copy; libmh/sim/ keeps its own copy rather than
// depending on ai/ai_state.h.
inline constexpr uint32_t STATUS_ALIVE = 0x2u;

// Ghidra enum /Manual/game/E_PLANET_STATUS: UNKNOWN=0 (same citation tact_state.h's
// planet_status_at() comment uses for the identical region).
inline constexpr int32_t PLANET_STATUS_UNKNOWN = 0;

// The single, hardcoded player index this body processes. The outer loop's bounds
// (`for (i=2; i<3; ++i)`, @0x004987c6/@0x004987cd) are literal, so it is not really a loop -- it
// always runs exactly one iteration, for player slot 2.
inline constexpr int32_t TRACKED_PLAYER = 2;

// Planets[] walk range: slots 1..0x1f (slot 0 unused), same PLANET_COUNT posture
// sim/sim_player_presence_lost.cpp already uses for the identical span.
inline constexpr int32_t PLANET_COUNT = 0x20;

} // namespace

namespace detail {

// ---- llm_game_session_clear_system_presence_flag @0x004987ae --------------------------------------
void session_clear_system_presence_flag(const sim_view &v, sim_store &own) {
    const int32_t planet_index = *v.planet_index;

    // 0x004987e0-0x004987ed: gate. `CMP EAX,dword ptr [EDX + 0xbe71c1]` with EDX =
    // G_PLANET_INDEX * 0x427. Planets' base is 0x00be6da0 (mh_regions.gen.h RID_PLANETS), so the
    // operand is +0x421, which mh_structs.gen.h's offsetof static_asserts name `enemy` (asteriods
    // is +0x419, turn_speed +0x41d). The signed JLE @0x004987f3 skips the whole body when
    // 2 <= enemy, i.e. the body runs only when `enemy` is 0 or 1. The function's DB plate said
    // `.asteriods` until 2026-08-31 and this translation followed it; corrected at EN v378
    // (finding 2026-08-31-1157-1) -- Ghidra's own decompile of the line already read `.enemy`.
    if (static_cast<int32_t>(v.cfg_planets[planet_index].enemy) >= TRACKED_PLAYER) return;

    bool still_absent = true; // [EBP-0x20]: survives every planet below -> the write @0x0049889a fires.

    for (int32_t j = 1; j < PLANET_COUNT; ++j) {
        // 0x0049881a-0x0049882d: only planets in the current system count.
        if (v.cfg_planets[j].system_index != *v.current_system) continue;

        if (v.planet_status[j] == PLANET_STATUS_UNKNOWN && j != planet_index) {
            // 0x00498835-0x0049884b: an unvisited planet elsewhere in the system -- still something
            // left to discover, so the player cannot be marked gone yet.
            still_absent = false;
        } else if (v.profiles[TRACKED_PLAYER].buildings_alive[j] > 0 ||
                   v.profiles[TRACKED_PLAYER].units_alive[j] > 0) {
            // 0x00498863 / 0x0049887b: the tracked player still holds a building or unit on planet j.
            still_absent = false;
        }
    }

    if (still_absent) {
        // 0x0049889a: AND byte ptr [...],0xfd -- clear STATUS_ALIVE on the tracked player's profile.
        own.profile_at(TRACKED_PLAYER).status_flags &= ~STATUS_ALIVE;
    }
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void session_clear_system_presence_flag() {
    sim_state st = state();
    detail::session_clear_system_presence_flag(st.read, st.own);
}

} // namespace mh::sim
