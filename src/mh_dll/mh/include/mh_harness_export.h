#pragma once
//
// Determinism replay harness (MP-restoration go/no-go -- Step 0).
//
// The harness is a diagnostic layer injected into mh.exe alongside (or instead of) the mod DLL.
// It hooks the deterministic strategic sim step (llm_strat_sim_step @0x0043f512) and, each step,
// hashes the live sim-state arrays + RNG + clock and appends the hash to a log. Two runs whose
// per-step hash streams are byte-identical are replay-deterministic; comparing an Intel vs an AMD
// log localises any x87 floating-point divergence to the exact step (and, via the per-region
// breakdown, to the exact array). See the replay-harness notes.
//
// FORK F4E: EVERY DECLARATION BELOW IS NOW A DLL BOUNDARY. The harness ships as mh_harness.dll and
// mh.dll resolves these thirteen symbols with GetProcAddress, defining each as a forwarding shim over
// the bound table (mh/seams/harness_bind.cpp). Not one call site changed -- that is the point of the
// shim -- but two things are worth knowing when editing this header:
//
//   * The CONTRACT is mh/include/mh_harness_module.h, which carries the same thirteen names WITH the
//     value each answers when the instrument is absent. Adding a declaration here is not enough:
//     tools/gen_harness_contract.py re-derives the set from objects and REFUSES if the two disagree.
//   * MH_Harness_ReportRelocation is the one exception and is NOT a contract row -- it moved to
//     mh/seams/core_arm.cpp with the relocating bind it reports (see its own note below).
//
// Activated from DllMain (DLL_PROCESS_ATTACH) via MH_Harness_Init when mh_net.ini beside the exe
// carries `[harness] enable=1`; a no-op otherwise, so the same DLL is safe to ship without the
// harness firing. THE ARM IS AN EXPLICIT KEY since fork F2G (ruling Q6) -- it used to be the mere
// existence of a separate mh_harness.ini, which is a configuration written in the filesystem and
// which silently decided one gate's real step budget. A leftover copy of that
// file beside the exe is now REFUSED by mh::config rather than ignored.
//
#ifdef __cplusplus
extern "C" {
#endif

// Install the sim-step + debug-console trampolines and load the `[harness]` config out of
// mh_net.ini. Safe to call unconditionally: without `[harness] enable=1` beside the exe it returns
// 0 without patching anything. Returns 1 when the harness armed. Call once, from DllMain, before exe
// code runs -- and when it returns 1, DllMain should SKIP the pool relocation (the harness runs on
// a minimal import-only exe so the sim stays as close to vanilla as possible).
//
// F3B: THE FIRST STATEMENT IS THE GATE NOW. The unconditional preamble that used to open this
// function -- paths, the state/host-api/inbound/event binds, the rebind arm -- is mh.dll core, not
// the instrument, and is MH_Core_Arm_Early() in mh_core_arm_export.h. DllMain calls that
// immediately before this, so the boot order did not move; only the ownership did.
int MH_Harness_Init(void);

// C6: point the harness's sim_tick detour at a REPLACEMENT body instead of the original.
//
// llm_strat_sim_tick can only have one entry owner, and the harness must be it -- if the promotion seam
// won that race the HARNESS would be the one that refused, which voids a determinism run silently
// instead of failing it. So the instrument keeps the entry and this moves only its fall-through: the
// detour still runs on_sim_tick (the fixed-timestep pin, the temporal event) and then jumps to `ours`
// rather than to the stolen-prologue trampoline.
//
// `ours` must be an entry thunk with llm_strat_sim_tick's own calling convention -- it is jumped to,
// not called, so it returns straight to sim_tick's caller. Pass nullptr to leave the original in place.
// Returns 1 if the rebind took, 0 if the harness is not armed in this process (no `[harness]
// enable=1`), in which case nobody owns the entry and an ordinary entry install is the correct route.
int MH_Harness_RebindSimTick(void *ours);

// LT1F (2026-09-02): the C6 chain hook for the translated frame spine. Runs on_sim_tick (the
// fixed-timestep pin, TEV_SIMTICK) exactly as the sim_tick entry detour would -- and ONLY when
// that detour armed this run (no `[harness] enable=1` -> no-op), so a direct-calling frame body stays
// byte-equivalent to the entry path in every configuration. Handed to mh::sim's frame unit as a
// pointer by net_lockstep.cpp's install (a reimpl TU includes no seams header).
void MH_Harness_OnSimTick(void);

// LIB-TRANS-P (2026-09-02): nonzero iff this run's harness config will arm the deterministic
// wall-clock pin over time_GetCurrentTime's entry -- the lib_trans promotion of that entry yields
// when set (the pin arms after MH_Seam_Init's promotions, so without the yield the promotion wins
// the entry and every harnessed run loses its pinned clock; measured on the first SP-oracle run).
int MH_Harness_WantsWallclockPin(void);

// RI-SIM / SIM1F (C6, one instrument over): the same for llm_strat_sim_STEP -- the strategic sim domain
// root. The determinism harness owns sim_step's entry too (its per-step golden-hash + the SIM-CUT
// exactly-once probe live in the detour), so a reimpl promotion cannot install over it; this points the
// detour's fall-through at `ours` (the generated __watcall(void) entry thunk for mh::sim::sim_step)
// instead of the stolen-prologue trampoline, AFTER on_sim_step has hashed the pre-body state. `ours` is
// jumped to, so it returns straight to sim_step's caller. Returns 1 if the rebind took, 0 if no harness
// armed this run (no `[harness] enable=1`). Called from reimpl_probe when the selector says ours runs.
int MH_Harness_RebindSimStep(void *ours);

// (MH_Harness_RebindTactFrame / MH_Harness_RebindTactEnqueue stood here until fork F5H, 2026-09-14.
// F2E demoted tact permanently and deleted their only callers; F4E measured them callerless and
// parked the delete to F5's sweep. Neither was ever a contract row or a .def entry.)

// D18: the same rebind for llm_strat_order_queue_dispatch. The order RECORDER detours that entry
// whenever [test] order_mode=1, which made recording and [promote] sim_dispatch mutually exclusive --
// whichever armed second lost, and it was the promotion, refused with a wrong-build message. With
// this, the detour keeps the entry (so recording still runs first) and only its fall-through moves.
// Returns 1 if the rebind took, 0 if no recording detour is installed this run, in which case the
// ordinary export install is the correct route. Called from install_promotion_dispatch.
int MH_Harness_RebindOrderDispatch(void *ours);

// C10: the same rebind for llm_game_land_players_on_planet. The all-AI soak's landing-conversion
// detour arms in MH_Harness_Init, which precedes every promotion, so that detour ALWAYS owns this
// entry and sim_resid's own install was ALWAYS refused -- "30/31 seams installed -- PARTIAL, treat
// this run as invalid" on every all-AI soak since SIM-RESID-P. With this the detour keeps the entry
// (so the conversion still runs first) and only its fall-through moves to ours. Returns 1 if the
// rebind took, 0 if no landing detour is armed this run, in which case the ordinary export install is
// the correct route. Called from install_promotion_resid.
//
// PAIRED WITH A SEAM, and neither half is sufficient alone: this fixes the ENTRY contest, while
// mh::sim::set_land_players_observer covers the edge no entry hook can see -- our own promoted
// session_begin_multi calling detail::land_players_on_planet directly.
int MH_Harness_RebindLandPlayers(void *ours);

// D18: everything the harness must do AFTER the promotions exist. MH_Harness_Init runs BEFORE
// MH_Seam_Init, so anything decided at harness-init time predates every promotion -- which is how
// `[harness] replay_suppress_enqueue` came to invalidate the whole `[promote] orders` container.
// Call at the END of MH_Seam_Init. No-op when no harness is armed, or when this run wants nothing
// from it.
void MH_Harness_LateArm(void);

// SB-HOSTFREE: write the relocating state bind's evidence. The bind happens in MH_Core_Arm_Early,
// which has no logger; this is called from the arm-time report block beside [statebind], which
// does. Silent unless `[harness] relocate_state` asked for a relocation -- a "0 relocated" line on
// every ordinary run would train a reader to skim past the one that matters.
//
// F4E: THIS ONE IS MH.DLL'S, not the instrument's, and it is NOT a row of the module contract. It
// reports the RELOCATING STATE BIND, which has to run inside MH_Core_Arm_Early (before anything can
// resolve a region), so the arena, the bind and this report all live in mh/seams/core_arm.cpp. The
// name stays because it describes whose CONFIGURATION it reports -- `[harness] relocate_state` -- and
// because renaming it would be churn for a sentence. The F4 surface pass predicted thirteen crossing
// symbols; it was twelve at F4E, and this is the one that stopped crossing by moving to the side
// that owns its data (R9). It is thirteen again today for an unrelated reason (StepFence, above).
void MH_Harness_ReportRelocation(void);

// F5J: THE SIM-STEP FENCE -- the honest surface behind the UI script grammar's `simstep <N>`.
//
// WHY UI_DRIVE CANNOT JUST READ A COUNTER. An in-game capture has to be taken at a KNOWN sim step or
// the frame carries whatever sprite phase the sim happened to be on. `gameclock <MS>` and a bare
// step-count predicate both flip at a deterministic step and are then read on the next PRESENT -- and
// llm_strat_sim_tick's catch-up loop runs 0..N steps per frame, so the step count at that present is
// the BURST SIZE, not the target. On a solo/host peer the burst is pinned by pin_wallclock; on a
// lockstep CLIENT it is set by the wire (the horizon arrives in chunks), which no capture-timing pin
// can reach. That is the measured F4H result: pinned, pause_mp_gate's host capture came within a
// pixel and its client gave two frames 3.5% apart.
//
// So the predicate FIRES ON THE STEP instead of after it. Arming a fence at N makes the harness write
// TOTAL_GAME_TIME = GAME_CLOCK from inside on_sim_step at step N -- which ends that frame's catch-up
// loop with step N (mode 3 tests `clock + interval <= total`, the SP branch tests `0 < total - clock`,
// and both are false) -- and re-applies the same write at every subsequent sim_tick entry, so the sim
// is HELD at N and every present from there renders the identical state. It is the same clock pin
// `fixed_step` and the clock-track replay already use, one value lower.
//
// DETERMINISM-SAFE, and the reason is that a fence changes WHEN steps run, never WHICH: the sim stays
// a pure function of (start, seed, tick-ordered orders). A held peer simply stops advancing, exactly
// as a horizon-starved one does, and keeps advertising its (frozen) horizon so its partner can still
// reach N. It is a TEST hook -- only a `[uitest]` script arms one -- and it is not released
// automatically, because the scenarios that use it capture and `end`.
//
// `target > 0` arms (idempotently; re-arming at a different target clears the hold), `target <= 0`
// releases. RETURNS the current sim step, or -1 when no harness armed in this process -- which is
// also the absent value, so an uninstrumented lane and a disarmed one answer alike and ui_drive
// refuses the script by name instead of timing out on a predicate nothing can satisfy.
int MH_Harness_StepFence(int target);

// UI-REC: the present-cadence tick for the input journal's mode-agnostic arm ([harness]
// ui_journal_rec / ui_journal). Call once per present, from net_lockstep::on_present.
//
// WHY THE PRESENT AND NOT A GAME HOOK. The tactical input journal is indexed on llm_tact_frame,
// which only ticks in mode 6 -- so it cannot record or replay the sequence that STARTS a game
// (main menu -> new game -> in-game). The present hook is the only per-frame callback that runs in
// every mode, which is why the capture, overlay and UI-drive harnesses already ride it. This also
// carries the pinned wall clock across the menu, where neither in-game cadence advances it.
//
// Cheap when idle (one branch) and inert without an armed harness, so it is called unconditionally.
void MH_Harness_OnPresent(void);

// UI-REC: nonzero iff this run armed a UI journal and therefore REQUIRES the present hook. The hook
// is otherwise installed only when [net] frametime_log / eager_advertise / [trace] temporal asks for
// it; all three ship at 1, so without this the recorder works by coincidence and fails silently the
// first time a fragment turns one off. Read by lockstep_install_present_gameover.
int MH_Harness_WantsPresentTick(void);

// THE MOVIE TICK, which is a SECOND per-frame anchor and not a duplicate of the present one.
// llm_ui_menu_async_tick drives the intro/briefing screens through its own DirectDraw surface calls
// and never reaches llm_gfx_present_flip, so for the whole of those screens MH_Harness_OnPresent does
// not run -- measured: a per-present hook fired at present 1 and 362 of a run whose movie is minutes
// long. Anything the harness needs to do WHILE a movie is up has to hang off this instead.
// video.cpp owns the trampoline (it already had one there for the no_window keeper) and asks
// MH_Harness_WantsMovieTick() whether to arm it when its own reason does not apply.
void MH_Harness_OnMovieTick(void);
int  MH_Harness_WantsMovieTick(void);

#ifdef __cplusplus
}
#endif
