//
// sim/resid/sim_session_begin_multi.cpp -- see sim_session_begin_multi.h. Translated from the
// DISASSEMBLY (tmp/decomp_sim_resid/llm_strat_session_begin_multi_0045435f.asm), the exported .c
// being a draft.
//
#include "sim/resid/sim_session_begin_multi.h"

#include "addr/mh_calls.gen.h"                            // typed callables for the frontier originals we still call OUT to
#include "sim/libtrans/sim_lt_progress_unlock_fixpoint.h" // the rebound propagate_unlocks
#include "state/boot_snapshot.h"                          // LIB-BOOT: the import-ordering latch
#include "state/host_api.h"
#include "state/host_events.h"
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const session_begin_multi_calls &live_session_begin_multi_calls() {
    static const session_begin_multi_calls c = {
        MH_LIBMH_BIND(llm_strat_scenario_planet_clone),
        MH_LIBMH_BIND(llm_strat_rng_seed_ch0),
        MH_LIBMH_BIND(llm_game_player_set_human),
        mh::state::evt::msg_queue_clear_all,
        mh::host().map_ReadMap_pre,
        MH_LIBMH_BIND(llm_net_lockstep_peer_timing_reset),
        MH_LIBMH_BIND(llm_strat_player_profile_init),
        MH_LIBMH_BIND(llm_diplomacy_init_multiplayer),
        MH_LIBMH_BIND(llm_game_reload_snapshot_resync_clocks),
        &mh::sim::progress_propagate_unlocks, // REBOUND 2026-09-02: translated (LT1B), ours binds directly
        MH_LIBMH_BIND(game_UpdatePlanetProgress),
        MH_LIBMH_BIND(llm_game_speed_recompute),
        MH_LIBMH_BIND(llm_net_lockstep_sync_delay_stub),
        MH_LIBMH_BIND(llm_menu_force_return_to_main),
    };
    return c;
}

namespace {

// SESSION_MP_LOCAL (mode 2) is not in sim_event_codes.h (which carries only SESSION_SP /
// _MP_LOCKSTEP) -- same local-definition precedent sim_player_presence_lost.cpp already uses for
// this exact constant.
constexpr int32_t SESSION_MP_LOCAL    = 2;
constexpr int32_t SESSION_MP_LOCKSTEP = 3;

// 0x004543ae: the lobby host-count floor the session-mode gate compares against (unsigned JBE, so
// host_count in {0,1} takes the LOCAL branch, host_count >= 2 takes LOCKSTEP).
constexpr int32_t LOBBY_SOLO_HOST_COUNT_MAX = 1;

// 0x004543cd-0x004543eb: the four literal cells that stand up the MP "virtual home planet" slot.
// Compile-time-constant addresses in the assembly (no index register feeds any of them) -- these are
// four fixed pokes, not a loop over planet/system id 0x1f.
constexpr int32_t HOME_PLANET_INDEX = 0x1f;
constexpr int32_t HOME_SYSTEM_INDEX = 0;
// INT-TABLE INDEX 4, not 1. `MOV dword ptr [0x00be1a40],0x1f` @0x004543cd, and System's bound base
// is 0x00be1a30, so the target is base + 0x10 = int32 index 4 -- the SAME cell
// sim_land_players_on_planet.cpp reads back through its own SYSTEM_FIELD_INDEX_4, and that file
// already warns that the Ghidra draft's field NAME for it ("planets[1]") cannot be trusted because
// the separately-typed cfg_final_struct_System is anchored 8 bytes late. This constant was 1 for
// exactly that reason -- the name said "planets[1]" and the slot number was read off the name
// rather than off the address. Index 1 is 0x00be1a34, the system's define_index/name field, which
// three other functions read as a G_TEXT_PTRS index. Found by reimpl-verify, 2026-08-31.
constexpr int32_t HOME_SYSTEM_FIELD_INDEX = 4;
// `System` is a raw int table on both halves of the state interface (see
// sim_view::system_define_index_base); the record stride is 0x8c bytes. Same name and same value as
// sim_land_players_on_planet.cpp and sim_player_presence_lost.cpp use for the identical region.
constexpr int32_t SYSTEM_STRIDE_INTS = 0x8c / 4;

// 0x00454458-0x00454452: the profile-init loop bound (Players[0..7]).
constexpr int32_t PLAYER_SLOT_COUNT = 8;

} // namespace

namespace detail {

// ---- llm_strat_session_begin_multi @0x0045435f -------------------------------------------------
int32_t session_begin_multi(const sim_view &v, sim_store &own,
                            const session_begin_multi_calls &c, void *cfg_blob,
                            const session_state_reset_calls     &c_ssr,
                            const new_game_init_calls           &c_ngi,
                            const land_players_on_planet_calls  &c_lpop,
                            const planet_map_session_init_calls &c_pmsi) {
    // C10: the session-entry observer, BEFORE any of the body. Order is load-bearing, not tidiness --
    // it carries the manual-menu MP host-count fix-up, and the body below is what reads that value to
    // choose SESSION_MODE 2 (solo) vs 3 (lockstep). Null in every run that does not ask for it,
    // including every offline oracle. See the header for why a detour cannot serve here.
    fire_session_begin_multi_observer();

    // LIB-BOOT trap (a): from here on, a post-cfg snapshot import would CLOBBER live session state.
    // `Planets` is one region with two writers -- cfg_ConstructPlanets fills slots 1..N once at boot
    // and llm_strat_scenario_planet_clone (called a few lines below) rewrites the reserved slot
    // 0x1f every session, READING Planets[1].icon_index it does not overwrite. G_TEXT_PTRS has the
    // same split. So the latch is set BEFORE the body, and libmh_import_snapshot refuses afterwards.
    mh::state::boot::note_session_begun();

    // 0x0045437a-0x0045437f: INTRA-SLICE -- llm_strat_session_state_reset(2), a direct C++ call
    // per sim_resid rule 2, never mh::call::.
    detail::session_state_reset(v, own, c_ssr, 2, c_ngi);

    // 0x00454384-0x00454387: pass the caller's blob straight through to the frontier scenario
    // cloner (opaque everywhere in this tree; see declared_needs #12 for why cfg_blob has no type).
    c.scenario_planet_clone(cfg_blob);

    // 0x0045438c-0x004543a9: PlayerSide = _G_LLM_NET_LOCAL_PLAYER_SLOT (the injected MP stack's OWN
    // local-slot int -- declared_needs #2, NOT sim_view::local_player_slot, a different address).
    // _G_LLM_STRAT_PLAYER_RACE = Players[PlayerSide].race_or_faction, the `Players` region
    // (declared_needs #1), NOT `profiles`/_G_LLM_STRAT_PLAYERS.
    own.player_side_mut() = static_cast<int16_t>(*v.net_local_player_slot);
    // ZERO-extend: 0x00454398 is `MOVZX EAX, word ptr [PlayerSide]`, and sim_view binds PlayerSide
    // as int16_t, so a bare widen SIGN-extends. The other two reads of it in this same function
    // (0x00454427 and 0x004544d1, below) already cast through an unsigned type; this one did not.
    // Unreachable in practice -- the lobby bounds the slot to 0..7 -- but Law 2 matches the
    // instruction, not the range that happens to hold. Found by reimpl-verify, 2026-08-31.
    const int32_t me      = static_cast<int32_t>(static_cast<uint16_t>(*v.player_side));
    own.player_race_mut() = static_cast<int32_t>(v.player_desc_slots[me].race_or_faction);

    // 0x004543ae-0x004543cd: session mode by lobby host count.
    if (*v.net_lobby_scan_host_count <= LOBBY_SOLO_HOST_COUNT_MAX) {
        own.session_mode() = SESSION_MP_LOCAL;
    } else {
        own.session_mode() = SESSION_MP_LOCKSTEP;
    }

    // 0x004543cd-0x004543eb: the four literal home-planet-slot pokes (declared_needs #6, #7, #10,
    // #11) -- NOT computed from any loop variable in the original.
    own.system_define_index_at(HOME_SYSTEM_INDEX * SYSTEM_STRIDE_INTS + HOME_SYSTEM_FIELD_INDEX) =
        HOME_PLANET_INDEX;
    own.cfg_planet_at(HOME_PLANET_INDEX).system_index = HOME_SYSTEM_INDEX;
    own.planet_index_mut()                            = HOME_PLANET_INDEX;
    own.current_system_mut()                          = HOME_SYSTEM_INDEX;

    // 0x004543f5-0x00454414: cfg_blob+0x14 (opaque blob, declared_needs #12 -- read directly off
    // the raw parameter, not through v/own), then the per-session flag/byte set.
    own.rng_seed_byte()                 = static_cast<const uint8_t *>(cfg_blob)[0x14];
    own.show_unit_flags()               = 1;
    own.mp_ally_victory_rule_flag_mut() = 0;
    own.chat_target_mask()              = 0;

    // 0x0045441b-0x00454422: re-read the seed byte just stored (matches the original's own
    // MOVZX-from-memory reload rather than reusing a cached local).
    c.rng_seed_ch0(static_cast<uint32_t>(own.rng_seed_byte()));

    // 0x00454427-0x00454447: human flag, first of two message-queue clears (EFFECTFUL WALL),
    // ReadMap, lockstep peer-timing reset.
    c.player_set_human(static_cast<uint8_t>(*v.player_side));
    c.ui_message_queue_clear_all();
    c.map_read_map_pre(static_cast<uint32_t>(*v.planet_index));
    c.net_lockstep_peer_timing_reset();

    // 0x00454447-0x004544b7: for every player slot whose Players[i].controller_flags is nonzero,
    // initialise its profile. `Players` (declared_needs #1) is the lobby-config table, a DIFFERENT
    // array from `profiles`/_G_LLM_STRAT_PLAYERS.
    for (int32_t i = 0; i < PLAYER_SLOT_COUNT; ++i) {
        if (v.player_desc_slots[i].controller_flags != 0) {
            c.player_profile_init(static_cast<uint32_t>(i),
                                  static_cast<uint32_t>(v.player_desc_slots[i].controller_flags),
                                  static_cast<uint32_t>(v.player_desc_slots[i].race_or_faction),
                                  *v.game_clock, static_cast<uint32_t>(v.player_desc_slots[i].color_or_team),
                                  const_cast<char *>(v.player_desc_slots[i].name),
                                  v.player_desc_slots[i].scenario_side_id);
        }
    }

    // 0x004544b9-0x004544be: diplomacy init, then mark the sim inactive during the reload/unlock
    // sequence below (declared_needs #8 -- SAME accessor sim_planet_transition_finalize.h needs).
    c.diplomacy_init_multiplayer();
    own.sim_active_mut() = 0;

    // 0x004544ca-0x004544d8: resync clocks to 0.0, propagate unlocks for the local player.
    c.reload_snapshot_resync_clocks(0.0);
    c.progress_propagate_unlocks(static_cast<uint16_t>(*v.player_side));

    // 0x004544dd-0x004544eb: skip the planet-progress refresh while the tutorial owns the screen.
    if (*v.tutorial_step == 0) {
        c.update_planet_progress();
    }

    // 0x004544eb-0x00454504: second message-queue clear (EFFECTFUL WALL, NOT a duplicate to
    // dedup -- the original calls it twice), mark the sim active again, land the players
    // (INTRA-SLICE, direct call), recompute game speed.
    c.ui_message_queue_clear_all();
    own.sim_active_mut() = 1;
    detail::land_players_on_planet(v, own, c_lpop, *v.planet_index, c_pmsi);
    c.game_speed_recompute();

    // 0x00454509-0x00454513: both STUBS in the original. The first, llm_lobby_map_recv_step_stub, is
    // EMPTY and is not reproduced (SIMABI-HOOKS); the second returns the value branched on below.
    const int32_t ret = c.net_lockstep_sync_delay_stub();

    // 0x00454516-0x0045454f: on a negative result while the tutorial is not running, tear down and
    // bail to the main menu with -1; otherwise arm the next lockstep adaption time and return the
    // (possibly still negative, if the tutorial IS running) raw result.
    if (ret < 0 && *v.tutorial_step == 0) {
        // (empty llm_teardown_hook_stub @0x0049bc44 here in the original -- not reproduced)
        c.menu_force_return_to_main();
        return -1;
    }

    // 0x00454525-0x00454531: declared_needs #13 for lockstep_session_adapt_delay's binding.
    own.lockstep_adapt_next_time() = *v.game_clock + *v.lockstep_session_adapt_delay;
    return ret;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

// ---- the session-entry observer (C10) -- see the header for why this is not a detour ------------
namespace {
void (*g_sbm_observer)() = nullptr;
} // namespace

void set_session_begin_multi_observer(void (*fn)()) { g_sbm_observer = fn; }
void (*session_begin_multi_observer())() { return g_sbm_observer; }
void fire_session_begin_multi_observer() {
    if (g_sbm_observer) g_sbm_observer();
}

int32_t session_begin_multi(void *cfg_blob) {
    sim_state st = state();
    return detail::session_begin_multi(st.read, st.own, live_session_begin_multi_calls(), cfg_blob);
}

} // namespace mh::sim
