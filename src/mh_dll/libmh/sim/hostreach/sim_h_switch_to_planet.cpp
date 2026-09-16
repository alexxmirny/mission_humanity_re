//
// sim/hostreach/sim_h_switch_to_planet.cpp -- see sim_h_switch_to_planet.h. Translated from the
// DISASSEMBLY (tmp/decomp_sim/SwitchToPlanet_0044ce13.asm), the Ghidra .c being a draft that disagrees
// with the asm on control flow (see the header banner).
//
#include "sim/hostreach/sim_h_switch_to_planet.h"

#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder (MH_LIBMH_BIND)
#include "state/host_api.h"     // mh::host() -- the generated host-callback table
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const switch_to_planet_calls &live_switch_to_planet_calls() {
    static const switch_to_planet_calls c = {
        // CONDUCTOR RESOLUTION (SIM1-H, 2026-09-10). The writer correctly refused to decide the
        // cross-domain routing unilaterally, and an earlier draft of this block kept all six on
        // `mh::call::` with a reasoned argument. That argument was WRONG about the save pair and
        // moot about the rest, and gen_libmh_calls / gen_va_census said so by name: a `mh::call::`
        // token is a source-level dependency on original machine code at a fixed VA, which is
        // exactly what a standalone libmh cannot have. Every one of these has a better home, and
        // all six are equivalent AT SHIP to what the .asm does. Three routes, one per class:
        //
        // (1) HOST-CALLBACK -> `mh::host()`, the generated table. The adjudication ledger
        //     (tools/data/libmh_call_ledger.json) already classes both of these, and the tool
        //     refuses the alternative in those words: "module code must route through mh::host(),
        //     or the no-op-host arm cannot see the call".
        //       llm_time_get_ticks_ms  -- `host-callback:time`, PACING ONLY (nothing it returns may
        //                                 reach hashed sim state; here it only drives the
        //                                 fast-forward loop's clock advance).
        //       map_ReadMap_pre        -- `host-callback:map-io`, DEFER-BLOB, shape unchanged.
        mh::host().ticks_ms,
        //
        // (2) REBINDABLE ROWS -> MH_LIBMH_BIND. The macro lives in addr/, which the outward-call
        //     census does not scan, so these stop being VA sites without changing a single byte of
        //     behaviour. WHY THE SAVE PAIR IS SAFE, since the earlier draft's whole objection was
        //     that binding them would run OUR walker where SHIP_PROMOTE_SAVE=0 says the original
        //     runs: the binder FOLLOWS THE PROMOTE KEY. save_live.cpp's own site says so at the one
        //     place that already does this -- "With `[promote] save` on this is our own body; with
        //     it off it is the original" (run_ours_savegame, the MH_LIBMH_BIND(map_SavePlanetToDisk)
        //     call). So at ship these two reach the original planet-file walkers, exactly as the
        //     instruction at 0x0044cece does, and the frozen save format is untouched.
        //     llm_strat_time_tick / llm_strat_sim_tick are promoted at ship
        //     (SHIP_PROMOTE_LOCKSTEP = 1), so both routes were already the same code.
        MH_LIBMH_BIND(llm_strat_time_tick),
        MH_LIBMH_BIND(llm_strat_sim_tick),
        MH_LIBMH_BIND(map_SavePlanetToDisk),
        MH_LIBMH_BIND(map_LoadPlanetFromDisk),
        mh::host().map_ReadMap_pre,
        // llm_strat_sim_clock_advance carries an MH_EXPORT_REPLACE in libmh/lockstep/turn_engine.cpp,
        // so gen_libmh_rebind's `translated()` accepts it and this binder site is what makes the row
        // exist -- the macro appeared on the next regeneration, as predicted.
        MH_LIBMH_BIND(llm_strat_sim_clock_advance),
    };
    return c;
}

namespace detail {

// ---- SwitchToPlanet @0x0044ce13 --------------------------------------------------------------------
int32_t switch_to_planet(const sim_view &v, sim_store &own, const switch_to_planet_calls &c,
                         int32_t planet_index, const land_players_on_planet_calls &c_land,
                         const planet_map_session_init_calls &c_pmsi,
                         const diplomacy_set_relation_calls  &c_set_relation) {
    // 0x0044ce35: unconditional at entry, before anything else runs.
    own.sim_active_mut() = 0;

    // 0x0044ce3f-0x0044ce5e: if the OUTGOING planet still has a queued production for the local
    // player, fast-forward the sim (game speed forced to 10x) until the queue drains.
    if (v.profiles[*v.player_side].prod_queue_slot[*v.planet_index] != 0) {
        // 0x0044ce60-0x0044ce7a / 0x0044ceb4-0x0044cebf: save/force/restore game_speed. sim_state.h
        // exposes game_speed only as one `double &own.game_speed()` (no split-dword accessor) -- the
        // asm's two-dword MOV/MOV save+restore collapses to a whole-double save/restore here,
        // bit-identical (an 8-byte raw copy of a double's bytes IS the double's value).
        const double saved_game_speed = own.game_speed();
        own.game_speed()              = 10.0; // bit pattern 0x40240000_00000000
        do {
            // 0x0044ce84-0x0044ce8e: re-read PlayerSide / G_PLANET_INDEX live on every pass (in the
            // while condition below), never cached across these three calls.
            c.time_get_ticks_ms();
            c.time_tick();
            c.sim_tick();
        } while (v.profiles[*v.player_side].prod_queue_slot[*v.planet_index] != 0);
        own.game_speed() = saved_game_speed;
    }

    // 0x0044cec4: always runs next, whether or not the loop above ran. Still the OLD planet --
    // G_PLANET_INDEX has not been overwritten yet.
    c.save_planet_to_disk(static_cast<uint32_t>(*v.planet_index), 1);

    // DEAD BY CONSTRUCTION (0x0044cee9-0x0044cf42, see header banner): [EBP-0x2c] is stored 0 at
    // 0x0044cedc and re-compared to 0 at 0x0044cee3 with no write between -> the guard is
    // unconditionally true, so the G_PLANET_STATUS[planet]=0 / progress[].acquired check and the CALL
    // llm_strat_revoke_invention at 0x0044cf3e never execute. Not reproduced.

    // 0x0044cf43-0x0044cf46: G_PLANET_INDEX = planet_index (the parameter).
    own.planet_index_mut() = planet_index;

    // 0x0044cf4b-0x0044cf5a: G_PLANET_STATUS[G_PLANET_INDEX] == UNKNOWN (0; Ghidra enum
    // /Manual/game/E_PLANET_STATUS, no committed C++ binding -- declared_needs).
    if (v.planet_status[*v.planet_index] == 0 /* E_PLANET_STATUS::UNKNOWN */) {
        // 0x0044cf8c-0x0044cfab: clamp the game clock DOWN to the planet's own saved time if that is
        // earlier (JBE = game_clock <= planet_time skips the clamp; not-taken here means
        // game_clock > planet_time, so the clamp fires).
        if (own.planet_time_at(*v.planet_index) < own.game_clock_mut()) {
            own.game_clock_mut() = own.planet_time_at(*v.planet_index);
        }
        c.read_map_pre(static_cast<uint32_t>(*v.planet_index));
        // llm_game_land_players_on_planet: ROUTING CORRECTION #1 -- see header banner. Threaded via
        // c_land (rule 3c) rather than MH_LIBMH_BIND.
        mh::sim::detail::land_players_on_planet(v, own, c_land, *v.planet_index, c_pmsi);
        // llm_game_session_clear_system_presence_flag: ROUTING CORRECTION #2 -- see header banner. Its
        // sim/resid sibling takes exactly (v, own), no struct to thread; called directly.
        mh::sim::detail::session_clear_system_presence_flag(v, own);
        // llm_diplomacy_restore_relations: THIS batch's own sibling (rule 3a); its own trailing calls
        // param defaults, so no threading needed here.
        mh::sim::detail::diplomacy_restore_relations(v, own, c_set_relation);
        // 0x0044cfcf: CMP G_PLANET_INDEX,5 -- flags-only, nothing reads the result. Omitted.
    } else {
        // 0x0044cf5c-0x0044cf76: known planet -> load from disk. Failure returns -1 immediately,
        // skipping the ambient-reseed / sim-clock-advance tail below entirely.
        if (c.load_planet_from_disk(*v.planet_index, 1) == 0) {
            return -1;
        }
        // 0x0044cf7b: CMP G_PLANET_INDEX,5 -- also flags-only (unconditional JMP right after it).
    }

    // DEAD BY CONSTRUCTION (0x0044cfe0-0x0044d08b, see header banner): [EBP-0x1c] is stored 0 exactly
    // once, at function entry, and never written on any path above -> the guard at 0x0044cfd6 always
    // jumps past the 100x100 Building[].unit_quant[] reset loop and its CALL llm_rand_below_fx. Not
    // reproduced.

    // 0x0044d090-0x0044d0a6: ambient reseed for the (now-current) planet, LAST_GAME_TIME as a double
    // (own.last_game_time(), read-only use here). ROUTING CORRECTION #3 -- see header banner:
    // llm_snd_ambient_reseed_planet_event_times is an already-translated sim-closure sibling
    // (sim/libtrans/sim_lt_ambient.h), called directly rather than through `c`/mh::call::.
    mh::sim::detail::ambient_reseed_planet_event_times(own, *v.planet_index, own.last_game_time());

    // INERT (0x0044d0ae-0x0044d0d7, see header banner): both arms of the JNC branch reconverge at the
    // identical instruction 0x0044d0d8 with no memory write on either side; the x87 stack stays
    // balanced throughout. Omitted.

    return c.sim_clock_advance();
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

int32_t switch_to_planet(int32_t planet_index) {
    sim_state st = state();
    return detail::switch_to_planet(st.read, st.own, live_switch_to_planet_calls(), planet_index);
}

} // namespace mh::sim
