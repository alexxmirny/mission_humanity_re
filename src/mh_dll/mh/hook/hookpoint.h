//
// hook/hookpoint.h -- D5: the NAMED HOOK POINTS, and the only way in for a module that is not mh.dll.
//
// ---- WHY THIS EXISTS ----------------------------------------------------------------------------
//
// Ruling D5 (the fork plan): "Only mh.dll uses `install_trampoline`. Every other module registers
// through a concrete hook API (named hook points: sim step/tick, present, order enqueue/dispatch,
// wallclock, whole-body pin slots). The entry-claim registry stays mh.dll-private."
//
// Measured before the item opened: 77 raw-primitive call sites, ALL under mh/seams plus one in
// mh/save. Fourteen of them are the DETERMINISM HARNESS's, which at F4 becomes mh_harness.dll -- a
// separate binary that would then be reaching across a DLL boundary to write E9 bytes into the game,
// carrying its own copy of every target VA, every stolen-byte count, every entry-claim decision and
// every arm-guard constant. That is the coupling this file removes, and it removes it by moving the
// DATA rather than by wrapping the call: a caller now names a POINT, and the point's row carries the
// target, the steal width, the claim and the guard.
//
// ---- WHAT A POINT IS ----------------------------------------------------------------------------
//
// One row per named point (points.cpp's TABLE), carrying:
//
//   id      -- the short greppable token ("sim_step"). What a report, a test or a grep names.
//   who     -- the human string the interlock's refusal summary and the SIM1-P clause 6 yield line
//              print. UNCHANGED FROM THE CALL SITE IT CAME FROM, deliberately: `check_arm_order`
//              gates the arm log's TEXT, so a re-plumbing that reworded a refusal would read as a
//              behaviour change. The two names are separate fields for exactly that reason -- the
//              id is ours to choose, the who is a fixed output string.
//   target  -- the game VA (0 for a registration-only point, which hooks no entry).
//   stolen  -- prologue bytes an observer steals (8 everywhere today).
//   claim   -- entry_claim::exclusive or ::rebind, the C4/C6 decision, made ONCE per point here
//              instead of at each site.
//   expect  -- the byte guard install_* applies (WATCOM_PROLOGUE, or 0 when `entry8` is stronger).
//   entry8  -- the exact eight entry bytes the export header was generated against, for the points
//              whose target does NOT open with a Watcom frame (llm_rand, the strat seed, the wndproc
//              tap, land_players). 0 when there is none.
//
// ---- WHAT IT IS NOT -----------------------------------------------------------------------------
//
// It is NOT a second interlock. Every arm here goes through the SAME install_jmp /
// install_trampoline / detour_refusal that every mh.dll seam uses, so the promoted->owned->prologue
// refusal order, the refusal registry and the one enumerated summary line are unchanged and
// untouched. The entry-claim table stays mh.dll-private: nothing here exposes g_owned, its ordering
// or its refusal vocabulary -- a caller gets a bool and the interlock does the explaining, by name,
// in the log it already writes.
//
// It is also not a place to put NEW behaviour. Every function below was one call at one site before
// this file existed, and the arm log is byte-identical across the move.
//
#pragma once
#include "hook/detour.h" // entry_claim, refuse_reason, WATCOM_PROLOGUE -- the primitives underneath

#include <cstdint>

namespace mh::hook {

// The named points. Grouped by SHAPE, because the shape decides which arm_* a caller may use and a
// mismatched pair is a programming error rather than a runtime condition (points.cpp refuses it).
enum class point {
    // ---- OBSERVE: a run-before-and-continue trampoline over the original's entry ----------------
    sim_tick,           // llm_strat_sim_tick        -- the determinism harness's frame-pump hook (C6)
    sim_step,           // llm_strat_sim_step        -- ditto, the per-step trajectory hash (C6)
    console,            // llm_debug_console_dispatch-- SHIFT+ENTER console
    order_dispatch,     // llm_strat_order_queue_dispatch -- the [test] order_mode=1 recorder
    land_players,       // llm_game_land_players_on_planet -- the ALLAI landing conversion (C10)
    tact_frame,         // llm_tact_frame            -- the tactical cadence (tact_hash_step/synth)
    tact_order_enqueue, // the tactical journal's order-enqueue recorder
    tact_group_order,   // the tactical journal's group-order recorder
    ui_input_update,    // llm_strat_input_update    -- the SPCAMP-FLAKE entry counter
    ui_storage_panel,   // llm_strat_ui_storage_bldg_panel -- ditto

    // ---- REPLACE: a whole-body pin slot; the original never runs --------------------------------
    // The fourth pin, llm_time_get_ticks_ms, is NOT here: it already arms through the generated
    // EXPORT funnel (MH_EXPORT_REPLACE / mh_export_install_*), whose per-function entry-byte guard is
    // strictly stronger than anything a table row can carry. That is the precedent this group
    // follows, not an exception to it -- a pin whose target HAS a generated export row should use it.
    pin_wallclock,         // GetCurrentTime                        -- [harness] pin_wallclock
    pin_rand,              // llm_rand                              -- [harness] pin_rand
    pin_strat_seed,        // llm_strat_rng_seed_wallclock_seconds  -- [harness] pin_strat_seed
    pin_input_wndproc_tap, // llm_input_wndproc_tap -- the replay's single-input-producer rule

    // ---- NEUTER: a guarded byte write over the entry, no detour and no thunk --------------------
    order_enqueue, // llm_strat_order_enqueue -> `xor eax,eax; ret` (replay_suppress_enqueue)

    // ---- REGISTER-ONLY: no entry of its own; the point's OWNER fires the callback ---------------
    // These are the four `set_*_observer` APIs the reimplemented bodies grew, plus R7's two
    // instrument handoffs. A body reached by a DIRECT intra-slice C++ call lands on no entry hook, so
    // an observer inside it is the only route that survives promotion (C10/D18); this is where a
    // module asks for one WITHOUT naming the domain that owns it.
    sim_step_pre,              // mh::sim::set_sim_step_pre_hook      -- D21's sampler onto our root
    dispatch_observer,         // mh::sim::set_dispatch_observer      -- D18's recorder
    land_players_observer,     // mh::sim::set_land_players_observer  -- C10's landing seam
    session_begin_multi,       // mh::sim::set_session_begin_multi_observer -- C10, the session seam
    time_resync_prelude,       // mh::sim::set_time_resync_instrument_hooks -- R7 / SIM-SAVE-DIV
    lt_frame_pace_time_tick,   // R7 / LT1F: the pacing chain hook the frame pair calls
    lt_frame_harness_sim_tick, // R7 / LT1F: the harness chain hook the frame pair calls

    count_
};

// The row's own fields, for a report or a test. `point_target` is 0 for a register-only point.
const char *point_id(point p);
const char *point_who(point p);
uintptr_t   point_target(point p);

// Do the target's first eight bytes still match the constant this point was generated against?
//
// Only meaningful for a point whose row carries an `entry8` -- the ones whose target does not open
// with a Watcom frame, where the generic prologue guard would refuse a perfectly good hook. Returns
// TRUE for a point with no entry8 (there is nothing to disagree with), so a caller may ask
// unconditionally. Exists as its own question because the call sites log a DIFFERENT sentence for a
// wrong build than for a refused install, and both sentences are gated by check_arm_order.
bool entry_bytes_match(point p);

// Would arming this point be refused? The primitives' own adjudication (detour_refusal), WITHOUT the
// write, asked with the row's claim and guard. For a caller that must decide ALL-OR-NOTHING across
// several points before touching any of them -- the tactical journal's two order seams, where a
// half-installed recorder records half a game.
bool available(point p);

// ---- the three arming shapes ---------------------------------------------------------------------
//
// Each returns exactly what the primitive under it returns, and each has already LOGGED and FILED any
// refusal through the interlock. A false here means "not armed, and the [interlock] summary names
// why" -- the caller's job is to say what is missing from the run, not to explain the refusal.

// OBSERVE. `detour` is the caller's naked thunk, `tramp_out` the slot it jumps back through.
bool arm_observer(point p, void *detour, void **tramp_out);

// REPLACE. `body` is the caller's whole replacement.
bool arm_replacement(point p, const void *body);

// NEUTER. Writes the row's replacement bytes over the entry if the guard bytes are there; returns
// false, having written nothing, if they are not.
//
// DELIBERATELY CLAIMS NOTHING AND FILES NOTHING. The site this moved from (MH_Harness_LateArm's
// enqueue neuter) hand-rolled its own VirtualProtect and never touched the claim registry, and it
// runs AFTER the SIM1-P clause 6 yield derivation has already walked that registry. Making it claim
// would change what that derivation reports -- i.e. the arm log -- which is a behaviour change wearing
// a re-plumbing's clothes. Left as measured; the claim question is F4's, with the harness's own DLL.
bool arm_neuter(point p);

// ---- registration ---------------------------------------------------------------------------------

// Register `fn` at a REGISTER-ONLY point. Returns false if the point is not register-only, or if the
// owning domain refuses the registration (mh::sim::set_sim_step_pre_hook is the one that can: its
// slot takes a single subscriber, and a second would silently replace the first).
//
// This is the D5 boundary in its purest form: the caller names a point, and the forwarding into the
// domain that owns the slot happens HERE, in mh.dll, which is the module allowed to know both sides.
// Before this, four modules each named `mh::sim::` directly -- three of them from net seam code,
// where check_net_lockstep_refs counted them as config-(1) coupling residue (R7).
bool register_callback(point p, void (*fn)());

// What is registered at `p`, or nullptr. Exposed so a test can assert the WIRING rather than the
// pointer's existence -- the same reason mh::sim::dispatch_observer() is exposed (the D17 lesson).
void (*callback_of(point p))();

// LT1F (R7): arm the frame-pair promotion with whatever is registered at the two lt_frame_* points.
// Returns install_promotion_lt_frame's own result, unchanged -- the routing decision, the refusal and
// its log line all still belong to libmh/sim/libtrans/sim_lt_frame.cpp. What moved is only WHO names it:
// net code now hands over two hooks by name and asks mh.dll to install, instead of calling a sim
// installer across the fork boundary.
int arm_frame_promotion(int default_on, int spine_promoted);

} // namespace mh::hook
