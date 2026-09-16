//
// sim/resid/sim_planet_session_begin.cpp -- see sim_planet_session_begin.h. Translated from the
// DISASSEMBLY (tmp/decomp_sim_resid/llm_strat_planet_session_begin_004541c3.asm), the Ghidra `.c`
// being a draft.
//
#include "sim/resid/sim_planet_session_begin.h"

#include "addr/mh_calls.gen.h"                            // typed callables for the effectful originals we still call OUT to
#include "sim/libtrans/sim_lt_progress_unlock_fixpoint.h" // the rebound propagate_unlocks
#include "sim/sim_event_codes.h"                          // SESSION_SP

// Intra-slice siblings (G21): these three edges are DIRECT C++ calls, never mh::call::, per the
// sim_resid batch context's rule 2. session_state_reset and land_players_on_planet are written by
// other agents in this same fan-out and may not exist yet at translation time -- the include and
// the call are written as if they do; a missing sibling is a build error for the conductor to fix.
#include "sim/resid/sim_land_players_on_planet.h"
#include "sim/resid/sim_session_clear_presence_flag.h"
#include "sim/resid/sim_session_state_reset.h"
#include "state/boot_snapshot.h" // LIB-BOOT: the import-ordering latch
#include "state/host_api.h"
#include "state/host_events.h"
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const planet_session_begin_calls &live_planet_session_begin_calls() {
    static const planet_session_begin_calls c = {
        MH_LIBMH_BIND(llm_strat_rng_seed_ch0),
        MH_LIBMH_BIND(llm_strat_rng_seed_ch1),
        mh::state::evt::msg_queue_clear_all,
        mh::host().map_ReadMap_pre,
        MH_LIBMH_BIND(llm_game_player_set_human),
        mh::host().wide_to_local_bytes,
        MH_LIBMH_BIND(llm_strat_player_profile_init),
        MH_LIBMH_BIND(llm_diplomacy_init_skirmish),
        MH_LIBMH_BIND(llm_game_reload_snapshot_resync_clocks),
        &mh::sim::progress_propagate_unlocks, // REBOUND 2026-09-02: translated (LT1B), ours binds directly
        MH_LIBMH_BIND(llm_strat_time_resync_and_tick),
        MH_LIBMH_BIND(llm_snd_ambient_reseed_planet_event_times),
        mh::state::evt::inv_viewport,
    };
    return c;
}

namespace {

// llm_strat_player_profile_init's controller_flags immediates (0x00454282 / 0x004542c0). Not a
// Ghidra enum -- plain per-slot roster-entry kind constants local to this call site.
constexpr uint32_t PROFILE_CONTROLLER_MOTHER  = 7;
constexpr uint32_t PROFILE_CONTROLLER_SHUTTLE = 0xb;

// G_TEXT_PTRS indices for the two arrival-name strings (0x00454266 / 0x004542a4).
constexpr int32_t TEXT_MOTHER_ARRIVED  = 0xa9;
constexpr int32_t TEXT_SHUTTLE_ARRIVED = 0xaa;

// Players 1 and 2 each get a starting shuttle (0x0045428e-0x004542cf: EBP-0x18 runs 1, 2).
constexpr int32_t FIRST_SHUTTLE_PLAYER = 1;
constexpr int32_t LAST_SHUTTLE_PLAYER  = 2; // inclusive

} // namespace

namespace detail {

// ---- llm_strat_planet_session_begin @0x004541c3 --------------------------------------------------
void planet_session_begin(const sim_view &v, sim_store &own,
                          const planet_session_begin_calls &c, int32_t race,
                          int32_t                              reset_flag,
                          const session_state_reset_calls     &c_ssr,
                          const new_game_init_calls           &c_ngi,
                          const land_players_on_planet_calls  &c_lpop,
                          const planet_map_session_init_calls &c_pmsi) {
    // 0x004541e0-0x004541ef: reseed BOTH rng channels from wall-clock seconds. The original makes
    // two INDEPENDENT draws here (the second llm_strat_rng_seed_wallclock_seconds call is not the
    // first value reused); libmh now reads ONE pushed session parameter instead, so both channels
    // get the same number. That is a DECLARED divergence and it is the fix, not a shortcut: the two
    // draws differ only across a second boundary, while a pulled wall-clock read is what lets two
    // multiplayer peers seed from two different clocks. See libmh_set_session_seed's banner in
    // libmh/include/libmh.h (LIFT-TABLE S5, 2026-09-09).
    //
    // LIB-BOOT trap (a): a session has begun, so a post-cfg snapshot import from here on would
    // clobber live state -- `Planets` slot 0x1f and G_TEXT_PTRS are rewritten per session by
    // llm_strat_scenario_planet_clone, which also READS Planets[1].icon_index it does not
    // overwrite. libmh_import_snapshot refuses once this latch is set.
    mh::state::boot::note_session_begun();

    const uint32_t seed = static_cast<uint32_t>(mh::state::session_seed());
    c.rng_seed_ch0(seed);
    c.rng_seed_ch1(seed);

    // 0x004541f4-0x004541f7: intra-slice edge -- forwards `reset_flag` unchanged (no other read of
    // it anywhere in this function).
    mh::sim::detail::session_state_reset(v, own, c_ssr,
                                         static_cast<uint32_t>(reset_flag), c_ngi);

    // 0x004541fc: `CMP dword ptr [EBP-0x20],0x2` -- compares `race` to 2 and NOTHING consumes the
    // resulting flags (no Jcc follows before they are clobbered). Dead Watcom-codegen leftover;
    // omitted, no observable effect (see uncertainties[]).

    // 0x00454203/0x00454208: record the race, force the session into single-player.
    own.player_race_mut() = race;       // _G_LLM_STRAT_PLAYER_RACE -- DECLARED NEED (no prior writer)
    own.session_mode()    = SESSION_SP; // _G_LLM_GAME_SESSION_MODE = 1

    c.ui_message_queue_clear_all();
    c.map_read_map_pre(static_cast<uint32_t>(*v.planet_index));

    // 0x00454221/0x0045422a: SP defaults -- local player is side 0, in local-player-slot 1.
    own.player_side_mut()       = 0; // PlayerSide -- DECLARED NEED (no prior writer)
    own.local_player_slot_mut() = 1; // _G_LLM_STRAT_LOCAL_PLAYER_SLOT -- DECLARED NEED (no prior writer)

    // 0x00454233-0x0045425d: PLAYER_PROFILE_RACE_SELECTOR. race==0 or race==1 selects
    // (mother_race, shuttle_race) = (1, 2); any other race (>= 2) swaps them to (2, 1). This is the
    // controller-race value llm_strat_player_profile_init's own `race` parameter takes -- it is NOT
    // necessarily the same axis as the top-level `race`/_G_LLM_STRAT_PLAYER_RACE just stored above
    // (see uncertainties[] -- its exact meaning is not derivable from this function alone).
    int32_t mother_race, shuttle_race;
    if (race == 0 || race == 1) {
        mother_race  = 1;
        shuttle_race = 2;
    } else {
        mother_race  = 2;
        shuttle_race = 1;
    }

    // 0x0045425d-0x00454289: player 0's starting MOTHER.
    c.player_set_human(0);
    char *mother_name = reinterpret_cast<char *>(
        c.get_ascii_ver(const_cast<wchar_t *>(v.text_ptrs[TEXT_MOTHER_ARRIVED])));
    c.player_profile_init(0, PROFILE_CONTROLLER_MOTHER, static_cast<uint32_t>(mother_race),
                          *v.game_clock, 0, mother_name, -1);

    // 0x0045428e-0x004542cf: players 1 and 2 each get a starting SHUTTLE. getAsciiVer is
    // re-evaluated every iteration (not hoisted), matching the assembly's per-iteration call.
    for (int32_t player = FIRST_SHUTTLE_PLAYER; player <= LAST_SHUTTLE_PLAYER; ++player) {
        char *shuttle_name = reinterpret_cast<char *>(
            c.get_ascii_ver(const_cast<wchar_t *>(v.text_ptrs[TEXT_SHUTTLE_ARRIVED])));
        c.player_profile_init(static_cast<uint32_t>(player), PROFILE_CONTROLLER_SHUTTLE,
                              static_cast<uint32_t>(shuttle_race), *v.game_clock, 1, shuttle_name,
                              -1);
    }

    // 0x004542cf-0x00454301: skirmish diplomacy init, then the SIM_ACTIVE 0->1 bracket around
    // snapshot resync / progress propagation / the (dead) empty stub, which we no longer call /
    // a second message-queue clear.
    c.diplomacy_init_skirmish();
    own.sim_active_mut() = 0; // _G_LLM_STRAT_SIM_ACTIVE -- DECLARED NEED (no prior writer)
    c.reload_snapshot_resync_clocks(0.0);
    c.progress_propagate_unlocks(static_cast<uint16_t>(own.player_side_mut()));
    // (empty llm_strat_session_begin_empty_stub @0x00454966 here in the original -- not reproduced)
    c.ui_message_queue_clear_all();
    own.sim_active_mut() = 1;

    // 0x0045430c-0x00454316: intra-slice edges -- land the roster, then clear system presence.
    mh::sim::detail::land_players_on_planet(v, own, c_lpop,
                                            static_cast<uint32_t>(*v.planet_index), c_pmsi);
    mh::sim::detail::session_clear_system_presence_flag(v, own);
    // Frontier: llm_strat_time_resync_and_tick is itself a sim_resid seed, but it belongs to a batch
    // this unit does not own, so it stays an mh::call:: frontier original here.
    c.time_resync_and_tick();

    // 0x0045431b-0x00454331: re-seed the ambient sound clock for the planet from LAST_GAME_TIME
    // (read, not written, by this function), then mark the camera viewport dirty.
    c.snd_ambient_reseed_planet_event_times(*v.planet_index, own.last_game_time());
    c.map_cam_mark_viewport_dirty();

    // 0x00454336-0x0045433d: re-seed RNG channel 0 from the persisted single-byte seed (read-only
    // here; own.rng_seed_byte() is the only accessor, no separate const-view binding exists).
    c.rng_seed_ch0(static_cast<uint32_t>(own.rng_seed_byte()));

    // 0x00454342-0x00454350: clear the two per-session flags for the fresh SP game.
    own.show_unit_flags()               = 0; // _G_LLM_STRAT_SHOW_UNIT_FLAGS
    own.mp_ally_victory_rule_flag_mut() = 0; // _G_LLM_STRAT_MP_ALLY_VICTORY_RULE_FLAG -- DECLARED NEED
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

void planet_session_begin(int32_t race, int32_t reset_flag) {
    sim_state st = state();
    detail::planet_session_begin(st.read, st.own, live_planet_session_begin_calls(), race,
                                 reset_flag);
}

} // namespace mh::sim
