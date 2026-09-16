//
// lockstep/lt_reload_snapshot_resync.h -- llm_game_reload_snapshot_resync_clocks @0x00425bcd
// (lib_trans batch D / d6, RI-LOCKSTEP). Reload/resync-path clock re-baseliner: not per-tick, fires
// only on the reload/snapshot-resync path (3 callers), reachable only in a multiplayer run
// (context_D.md §7c: SP run never calls it).
//
// ---- WHAT IT DOES, in the asm's own order ----------------------------------------------------------
//
// 1. SAVE three globals: SND_ENABLED (dword), GAME_CLOCK (8 B, both dwords), SIM_ACTIVE (dword).
// 2. INSTALL a temporary state: SND_ENABLED = 0, GAME_CLOCK = the `now` parameter (a raw 64-bit
//    bit-copy -- 0x00425c0f-0x00425c1a moves it as two dwords, no x87 instruction touches it, so a
//    plain `double` assignment reproduces it exactly), SIM_ACTIVE = 0xffffffff (i.e. int32_t -1).
// 3. CALL three EFFECTFUL emitters, IN THIS ORDER, WITH THE INSTALLED STATE STILL ACTIVE:
//    llm_strat_race_alert_sound_emit, llm_strat_group_order_ack_voice, llm_strat_race_alert_text_emit
//    (0x00425c29/0x2e/0x33). This is the mechanism, not an optimisation to skip: with SND_ENABLED
//    forced to 0 each emitter re-bases its own internal next-emit timestamp against the temporarily
//    installed GAME_CLOCK instead of actually emitting. DO NOT SUPPRESS THESE CALLS AND DO NOT
//    special-case the SND_ENABLED dance -- see the shadow-binding note below for why. Two of the
//    three also draw llm_rand_below_fx internally, so the count and order of calls is
//    RNG-stream-relevant; preserve both exactly.
// 4. RESTORE the three globals, IN THIS EXACT ORDER (0x00425c38-0x00425c53): GAME_CLOCK low dword,
//    GAME_CLOCK high dword, SND_ENABLED, SIM_ACTIVE. The ordering is part of the spec, not
//    incidental -- it is the only place in this function two adjacent stores could be transposed
//    without changing any SINGLE variable's final value, so it is easy to "simplify" by accident.
// 5. RESEED slot [0] of EVERY ONE of the 8 players, UNCONDITIONALLY -- no alive gate, no live-record
//    gate. `units[p][0].activity_clock` and `buildings[p][0].last_tick_time` both get
//    `GAME_CLOCK + RELOAD_TICK_BACKDATE` (-60.0); `buildings[p][0].cycle_progress` gets
//    `GAME_CLOCK + RELOAD_CYCLE_BACKDATE` (-20.0). This makes the first post-reload tick immediately
//    due. THE LOOP RE-READS THE RESTORED GAME_CLOCK FRESH ON EVERY ONE OF ITS THREE ADDS
//    (0x00425c6f / 0x00425c88 / 0x00425ca1 are three independent FLD/FADD/FSTP triplets, not one
//    shared load) -- so the backdates are relative to the ORIGINAL clock restored in step 4, never
//    to the `now` parameter. Do not hoist a shared local out of this loop.
//
// This is the sole behavioural difference from the sibling sim-side sweep
// `mh::sim::clock_resync_units_and_buildings` (sim/resid/sim_clock_resync.{h,cpp},
// @0x00499a3a): that function walks every LIVE record of every ALIVE player and contains ZERO x87
// instructions (pure bit-copies); this one touches only slot [0], touches it unconditionally, and
// does real FADD arithmetic for the backdate. Do not borrow that function's guards, and do not import
// this function's arithmetic into that one.
//
// ---- WHY THIS TU BINDS ITS OWN RAW POINTERS RATHER THAN EXTENDING engine_state --------------------
//
// turn_engine.h's `engine_state` has no `units`, `buildings`, `sim_active` or `snd_enabled` members
// (context_D.md §4b) and this file must not add them (conductor-owned edit). So, exactly like this
// directory's own resync.h/.cpp (`resync_state`, bound directly to `mh::addr::` constants rather than
// through `engine_state`), this TU declares its own `reload_state` and binds it directly. See
// `declared_needs` in the translation report for the one open question this leaves: `units`/
// `buildings` are owned by `mh::sim`, and the ST1 precedent (engine_state's `order_pending_count`/
// `order_staging_count` are deliberately rebound from `mh::orders`' own accessors rather than
// resolved independently, specifically so a later island move cannot silently break a consumer
// nobody remembered) argues this TU should eventually do the same. Not decided here.
//
// ---- SHADOW COVERAGE (context_D.md §7b) ------------------------------------------------------------
//
// GAME_CLOCK, `units` and `buildings` all carry MF_MEASURED and are compared. SIM_ACTIVE and
// SND_ENABLED are MF_VIEW only -- but both are saved on entry and restored to that exact value before
// this function returns, so a shadow arm's net effect on either is nil; the narrowing is benign here
// (unlike some other batch-D rows, per that section -- this was checked, not assumed).
//
// ---- Translated from the DISASSEMBLY
// (tmp/decomp_lib_trans/llm_game_reload_snapshot_resync_clocks_00425bcd.asm); the exported `.c` is a
// draft whose plate was corrected 2026-09-02 (it used to mislabel the three emitters as "sub-load
// calls restoring the saved clock") -- read the corrected plate, but translate from the assembly.
//
#pragma once
#include <cstdint>

#include "addr/mh_structs.gen.h"
#include "state/roster_caps.h" // SB-BIND T2: derived per-player row capacities

namespace mh::lockstep {

// Everything this function reads or writes, as typed pointers. Bound to the live game by
// live_reload_state(); a test can bind plain locals instead.
struct reload_state {
    // ---- the three save/install/restore globals, in the asm's own order ---------------------------
    int32_t *snd_enabled; // _G_LLM_SND_ENABLED         (0x00ae2ab0, 4 B) -- save -> 0 -> restore
    double  *game_clock;  // _G_LLM_STRAT_GAME_CLOCK    (0x005d0198, 8 B) -- save -> `now` -> restore;
                          // then re-read fresh by every one of the loop's three backdate adds
    int32_t *sim_active;  // _G_LLM_STRAT_SIM_ACTIVE    (0x005d01d4, 4 B) -- save -> -1 -> restore

    // ---- the two write targets, slot [0] only, all 8 players, unconditionally ---------------------
    mh::game::mh_map_object_unit     *units;     // RID_UNITS     (0x00dd8c48, stride 0xe9  * 100)
    mh::game::mh_map_object_building *buildings; // RID_BUILDINGS (0x00c3d2a0, stride 0x111 * 100)

    // ---- the two backdate constants, read out of the image rather than hardcoded -------------------
    const double *reload_tick_backdate;  // _G_LLM_STRAT_RELOAD_TICK_BACKDATE  (0x005001b6 = -60.0)
    const double *reload_cycle_backdate; // _G_LLM_STRAT_RELOAD_CYCLE_BACKDATE (0x005001be = -20.0)

    // SB-BIND T2: the per-player ROW CAPACITIES, derived from the sizes the host bound. LAST on
    // purpose -- this struct is aggregate-initialised positionally by both the production binder and
    // the selftest fixture, so a member inserted above silently re-pairs every initializer after it.
    // Stock default, so a fixture that does not name it is unchanged.
    mh::state::roster_caps caps{mh::state::STOCK_ROSTER_CAPS};
};

// The three re-basing calls. All three are classified `effectful` (context_D.md §2) and ALL THREE
// run for real in both the production entry and the shadow arm -- see the .cpp for why no `inert_*`
// variant exists here, unlike this directory's resync.cpp.
struct reload_calls {
    void (*race_alert_sound_emit)(); // llm_strat_race_alert_sound_emit @0x00425a6e
    void (*group_order_ack_voice)(); // llm_strat_group_order_ack_voice @0x004259b2
    void (*race_alert_text_emit)();  // llm_strat_race_alert_text_emit  @0x00425b2a
};

reload_state        live_reload_state();
const reload_calls &live_reload_calls();

namespace detail {

// Pure over its state and calls -- exercised without a game by libtranstest. `now` is the caller's
// wall/session clock, passed in exactly as the original's single stack parameter.
void reload_snapshot_resync_clocks(const reload_state &st, const reload_calls &calls, double now);

} // namespace detail

// Production entry point: detail:: over live_reload_state() / live_reload_calls(). Matches the
// original's committed `void __watcall llm_game_reload_snapshot_resync_clocks(double now)`. Its
// signature matches the committed one exactly, so it needs no separate adapter to be bound.
void reload_snapshot_resync_clocks(double now);

namespace detail {


} // namespace detail

} // namespace mh::lockstep
