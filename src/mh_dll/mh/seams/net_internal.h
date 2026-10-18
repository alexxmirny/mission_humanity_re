//
// net_internal.h -- shared spine for the MP-seam translation units. The MP netcode grew as one
// large file (net_seams.cpp); the 2026-07 refactor (Phase 4) peels
// cohesive pieces into sibling TUs (net_diag.cpp = perf-trace + function-entry instrumentation;
// net_discovery.cpp = session discovery/browser/join). This header holds only what CROSSES a TU
// boundary: state that MH_Seam_Init (net_seams.cpp) writes and a sibling reads, plus the cross-TU
// function declarations.
//
// Shared globals are C++17 `inline` variables at global scope -- one definition across every
// including TU, no extern/def bookkeeping -- matching net_seams.cpp's unqualified use. State PRIVATE
// to one TU stays `static`/anonymous-namespace there; the linker flags a misclassification.
// (Only the net_seams-family TUs include this header, so these names never meet launch/harness/
// mp_menu's own internal-linkage PROLOGUE/g_log.)
//
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <cstring> // memcpy (ms_of)

#include "hook/detour.h"            // mh::hook::WATCOM_PROLOGUE + install_* (used across the seam TUs)
#include "include/mh_seam_export.h" // MH_SeamAddrs (g_a below)
#include "include/mh_run_context.h" // SES1: MH_RunDir/MH_ProcessDir + mh_run_path (the path tick below)
#include "include/mh_session_dir.h" // SES1: MH_SessionRecord + the SESSION_BEGIN/END/json builders
#include "addr/mh_addrs.gen.h"      // mh::addr::_G_LLM_LOBBY_SLOTS -- mp_lobby_array_index below. Every
                                    // including TU here also includes this on its own before or after
                                    // net_internal.h; #pragma once makes the repeat free, and this
                                    // header must not depend on that ordering to compile standalone.

// Watcom frame prologue (55 89 e5 68) -- every hookable mh.exe entry opens with it; the install
// guards compare *target against this before arming (wrong build / already-hooked = safe no-op).
inline constexpr uint32_t PROLOGUE = mh::hook::WATCOM_PROLOGUE;

// ---- SHIPPING DEFAULTS (2026-07-25 ship build) ------------------------------------------------
// Every one of these used to be "off / leave the game's value", with the good values living only in
// tools/mp_run.py's generated ini -- so the test rig ran a tuned build and a real player ran an
// untuned one. They are the DLL's defaults now; mh_net.ini only overrides. Values are the ones the
// 2026-07-22..25 sweeps + determinism gates ran on, except the two the user set for ship:
//   * lookahead 100 ms (was 30 on the LAN sweep) -- internet play needs the margin; the adaptive
//     controller shrinks it toward the ~20 ms floor when the link turns out to be fast.
//   * sim sub-step 20 ms / 50 Hz (was 10 ms / 100 Hz) -- half the sim work for motion that is still
//     far finer than the stock 100 ms, and still an exact multiple of the 10 ms clock quantum.
// sim_step CHANGES THE SIM (both peers must agree); the lookahead does not (committed = min over
// peers), which is what lets the controller be purely local. Strings because the knobs are atof'd.
#define SHIP_LOOKAHEAD_MS "100"                     // [net] lockstep_step_ms  -- horizon lookahead / input latency
#define SHIP_SIM_STEP_MS  "20"                      // [net] sim_step_ms       -- strategic sim sub-step (50 Hz)
inline constexpr int SHIP_HITCH_MS          = 100;  // [net] hitch_ms -- frame-hitch log + mid-frame stack sample; 0 = off (single-player and multiplayer)
inline constexpr int SHIP_FRAME_WATCHDOG_MS = 1000; // mp:P17 [net] frame_watchdog_ms -- 0 = off; MP matches only
inline constexpr int SHIP_HEARTBEAT_MS      = 50;   // [net] horizon_heartbeat_ms -- off-frame advertisement
inline constexpr int SHIP_ADAPTIVE          = 1;    // [net] lockstep_adaptive    -- auto-tune the lookahead
// [net] rx_spin -- drain RX off-frame while stalled at the horizon, instead of idling until the next
// frame. Shipped ON since 2026-07-26: the P5 sweep measured it as a consistent 2-4 point cut in
// wall-clock deficit at EVERY lookahead (200 ms RTT), with a large drop in de-sync icon count and no
// downside in any run -- every combination stayed ALL PAIRS IDENTICAL. It was 0 only because it was
// added as an experiment and never re-defaulted.
inline constexpr int SHIP_RX_SPIN     = 1; // [net] rx_spin              -- off-frame RX drain while stalled
inline constexpr int SHIP_QPC_CLOCK   = 1; // [net] qpc_clock            -- ~1 ms game-clock quantum
inline constexpr int SHIP_HIRES_CLOCK = 1; // [net] hires_clock          -- timeBeginPeriod(1)
inline constexpr int SHIP_EAGER_ADV   = 1; // [net] eager_advertise      -- advertise from present
inline constexpr int SHIP_LOG_LEVEL   = 1; // [net] lockstep_log/frametime_log -- diet-reduced (mp:SES5), not off
// mp:SES5 decision (5) -- temporal split off SHIP_LOG_LEVEL: the per-EVENT trace is a profiling
// instrument (no player-report question has ever needed it), unlike lockstep_log/frametime_log
// which stay on by default just written far less densely. Rig/lane inis set [trace] temporal=1
// explicitly (tools/ui_test.py NET_BLOCK, tools/mp_run.py TRACE_TEMPORAL) so UI-REC/determinism/
// pacing work is unaffected; only a SHIPPED install with no ini override goes quiet.
inline constexpr int SHIP_TEMPORAL_LEVEL = 0; // [trace] temporal
// mp:GS2. [net] data_timeout_ms -- drop a peer whose lockstep horizon has not moved (no DATA, keepalive
// or not) in this many ms; <= 0 disables. 15000 (15 s) is deliberately BELOW the low end of the field's
// observed 22-35 s freezes (2026-09-20), so it actually resolves them, and ABOVE the transport link
// watchdog's own 10 s keepalive interval, so an ordinary keepalive-cadence hiccup never trips it on its
// own. What does NOT starve the DATA path, so does not cost a false drop: a map download happens in the
// LOBBY (SESSION_MODE != 3, this watchdog does not run yet); the injected in-match "big map" hotkey is
// unconditionally refused whenever the session is a live lockstep match (it would skip the sim tick and
// stall the peer that pressed it, so it is gated off there by construction) -- retail has no in-match MP
// pause at all. What CAN legitimately starve it: the game's own WM_ACTIVATE handler pauses THIS peer's
// local game clock on Windows focus loss, so a long alt-tab reads identically to a freeze from the OTHER
// peer's point of view -- this is not special-cased (a backgrounded peer being eventually dropped is
// ordinary MP behaviour, same as any other online game); 15 s is meant to clear a quick glance-away, not
// an extended one. Manual saving mid-lockstep-match was not checked before choosing this default -- if
// retail allows it and it blocks the sim thread near or past 15 s, that is an open follow-up.
inline constexpr int SHIP_DATA_TIMEOUT_MS = 15000;
// mp:U20b. [net] stall_icon_delay_ms -- the de-sync corner icon (llm_net_lockstep_wait_player_overlay_show)
// shows only once the sim has been parked at the lockstep horizon for this long; 0 = stock (the second
// parked frame). Measured on the 2026-10-14 field reports (three matches, 335 icon episodes, wall time
// from SYNC_WAIT_ACTIVE 0->1 to 1->0 in mh_lockstep.log): p50 0-31 ms, p90 32-125 ms, max 703 ms -- every
// icon the players saw was a sub-second at-horizon wait. 1000 ms sits above all of them and well below
// the kick modal (SYNC_RETRY_COUNTDOWN < 0x38, ~4 s parked), which this does not touch. Display-only and
// LOCAL: each peer may run its own value.
inline constexpr int SHIP_STALL_ICON_DELAY_MS = 1000;

// ---- WHICH BODIES RUN: NOT HERE ANY MORE (fork F2E, 2026-09-13) --------------------------------
//
// This block used to hold 23 per-domain promotion constants and the rebind gate's ship default --
// one shipping default per migrated domain, each threaded into its installer as `default_on`, each
// overridable by a `[promote]` ini key. That was the migration era's answer to "is this domain
// translated AND trusted yet", and it was the right shape while the answer differed per domain.
//
// It is now ONE question with one answer: `mh::config::ours_run()` (mh/config/config.h), from
// `[config] mode`. The constants are deleted, the 57 ini reads with them, and a surviving
// `[promote]`/`[rebind]` section REFUSES the run by name rather than quietly overriding a selector
// that is now the only control. The one per-domain distinction that survived the collapse is the
// save closure, because it is genuinely different -- a translated body we deliberately do not
// install -- and it lives beside the selector as `mh::config::kSaveClosureOwned`.
//
// WHERE THE EVIDENCE WENT. Each deleted constant carried its domain's acceptance record (the
// asymmetric 2-peer runs, the golden replays, the red arms that proved each oracle could fail).
// That is history, not configuration: it is in this file's git history at the F2E parent commit, in
// the per-domain migration ledgers under tools/data/*_migration.json (frozen at F2E as static
// inputs), and in the session reports. It was not carried forward here because a shipping-defaults
// block that documents defaults it no longer has is the stale-prose failure this fork is removing.

// X-TOMB (the endgame plan D-E2). ON at ship, but it arms ONLY the airtight claim: the dead
// REMAINDER (entry+8..end) of every PROMOTED body, whose entry is provably JMP'd to ours so the
// remainder cannot execute. With the promotions right this changes nothing; with one wrong it turns
// a silent mis-execution into a named, terminating hit. `[tombstone] enable=0` is the rollback.
//
// LEDGER-DEAD bodies are NOT armed at ship (`[tombstone] arm_dead=1` opts in). MEASURED 2026-09-01:
// a ledger's `dead` row is "unreached within THAT domain's measured closure", not "never executes"
// -- llm_strat_ai_build_target_list is `dead` in the AI ledger yet runs in a single-player game
// against the AI opponent. Arming the dead set is a deliberate AUDIT, run when you want those claims
// tested (and to fire), never the shipping configuration. `force_arm=<name>` is the negative arm.
inline constexpr int SHIP_TOMBSTONE = 1; // [tombstone] enable -- trap the dead remainder of promoted bodies

// Per-EVENT temporal-trace event ids (mh_net.ini [trace] temporal=1). Shared because events are
// emitted from several TUs (the present/time_tick detours in net_seams, sim_step in harness via
// MH_Temporal_Event) and recorded by net_diag.cpp.
enum { TEV_FRAME    = 0,
       TEV_PUMP     = 1,
       TEV_COMMIT   = 2,
       TEV_TIMETICK = 3,
       TEV_SIMTICK  = 4,
       TEV_SIMSTEP  = 5,
       TEV_SENDEXT  = 6,
       TEV_PRESENT  = 7 };

// ---- shared state: MH_Seam_Init (net_seams.cpp) writes; net_diag.cpp reads --------------------
inline char          g_ini[MAX_PATH];      // mh_net.ini path (next to the exe)
inline char          g_log[MAX_PATH];      // mh_net.log path (per-SESSION folder -- seam_paths_tick)
inline DWORD         g_main_tid = 0;       // main/frame thread id, captured in MH_Seam_Init
inline LARGE_INTEGER g_qpc_freq = {0};     // QPC frequency (temporal trace)
inline bool          g_temporal = false;   // [trace] temporal=1 armed
inline char          g_tev_path[MAX_PATH]; // mh_temporal.log path
inline int           g_trace_n = 0;        // function-entry tracer: # armed hooks (net_diag owns; net_seams' lobby reads)

// The mh.exe lobby RX/role addresses (overridable for tests via MH_Seam_SetAddrs). Defined in
// net_seams.cpp (initializer = the mh::addr defaults); read across the whole seam family.
extern MH_SeamAddrs g_a;

// S4 join gate: >=1 explicit JOIN naming our lobby-id has been admitted. Written on the recv
// thread (on_join_recv, net_discovery.cpp); read by net_seams' lobby dispatch (host work gate).
inline volatile LONG g_host_join_seen = 0;

// U40 RELINK LATCH: this peer's link belonged to a match that has ENDED, so the transport may be
// dialled again. Set by mp_session_close (net_discovery.cpp) at the MATCH-end reasons only, on a
// manual CLIENT; read by on_discover_poll (may I kick a connect?) and CONSUMED by lazy_start
// (net_seams.cpp), which is the one place allowed to re-enter MH_Net_InitEx on a started transport.
//
// A LATCH RATHER THAN A DIRECT TEARDOWN, because the exit seams run on the GAME thread: closing
// sockets and joining their threads at the game-over dialog would put a hitch (and a thread-join) on
// the frame that draws the player's result. The re-dial instead happens where a re-dial belongs --
// the discovery browser, main thread, on the connect thread we already spawn there -- so the boundary
// is the TRIGGER and the browser is the ACTOR. It is also why "no reconnect storms mid-game" needs no
// extra guard: on_discover_poll exists only on the browser screen.
inline volatile LONG g_net_relink = 0;

// [net] lockstep_log=1 -> mh_lockstep.log + the `;` DIAG lines. Written by net_lockstep.cpp's
// config (lockstep_install_core); read as the DIAG gate across net_seams (lobby dispatch, recv
// seams, finalize), the net_seams sbm logger, and the net_diag loggers.
inline bool g_ls_log = false;

// mp:X2h -- the savegame_io_error dlg CALLER logger (net_diag.cpp on_savegame_err) only ever WROTE
// its caller to the log line; nothing kept the value. The lobby-tick stall watch (net_seams.cpp)
// wants it as one of the stall line's identifying fields ("which modal is this"), so it is latched
// here on every dlg entry the logger's trampoline sees. Same install gate as the line itself
// (install_savegame_err_logger runs only under [net] lockstep_log=1, on by default in the shipped
// ini) -- with the trampoline never armed this simply stays 0, which the stall line prints as "no
// dlg observed", a true statement rather than a stale one.
inline volatile LONG g_last_dlg_caller = 0;

// ---- SES1: the per-session directory, as the seam TUs see it ---------------------------------
//
// One generation token per cached path. Every seam log writer calls its tick FIRST, so the line it
// is about to append lands in whichever directory is current -- the process ("menu") one before a
// lobby, the session one during a match. The cost is an integer compare; the alternative (composing
// the path at arm time, as this stack did until SES1) writes three matches into one folder.
//
// The DERIVED paths (mh_lockstep / mh_frametime / mh_temporal / mh_trace) each own
// their generation in the TU that writes them, and the ones holding an OPEN HANDLE act on
// mh_run_path's return value: a rebuilt path means close the handle and let the next write reopen.
inline unsigned long g_log_gen = 0;

inline void seam_paths_tick() { mh_run_path(g_log, MAX_PATH, "%smh_net.log", &g_log_gen); }

// Open a session directory for `match_id_hex` and announce it, or close the open one with `reason`.
// Defined in net_discovery.cpp (it owns the match_id); called from the lobby/lockstep exit seams in
// net_seams.cpp and net_lockstep.cpp. Both are safe to call when nothing is open.
//
// SES8: `mode` is the directory's last field -- "host" / "client" for a lobby, the single-player
// word from mh_session_solo_mode otherwise. `map` overrides the map token (nullptr = the loaded
// map's name, current_map_data.map_name). Both are final at the open: nothing renames the folder.
void mp_session_open(const unsigned char *match_id, int slot, const char *mode, const char *map = nullptr);
void mp_session_close(const char *reason);
// SES8: once per present (net_lockstep.cpp on_present) -- opens the single-player session.
void mp_session_solo_tick();
// RL15: once per present -- derive the presence state and publish presence.json if it changed.
void mp_presence_tick();

// mp:U62 (HM-M4) -- the planned handover of the transport hub (net_lockstep.cpp). mp_hub_leave is the common
// exit of every seam where a hub's PLAYER leaves a match (the ESC-menu quit, the defeat/stats screen, the
// process exiting); it is a no-op for a client and for a hub with fewer than two other humans playing.
// mp_leave_for_exit is the WM_DESTROY flavour: a hub mid-match first runs the clean quit (park + pinned
// self-removal), then hands over. Both are main-thread calls that may block ~1 round trip.
void mp_hub_leave(const char *why);
void mp_leave_for_exit();

// Read a game double (ms). Shared sampler for every timing log/DIAG line across the seam TUs.
inline long ms_of(uintptr_t a) {
    double d;
    memcpy(&d, (const void *)a, sizeof(double));
    return (long)(d * 1000.0);
}

// ---- cross-TU functions defined in net_seams.cpp (core transport/lobby glue) -----------------
void seam_log(const char *s); // append one line to mh_net.log (arm banners + DIAG lines)
// mp:GS1 (a): discard every datagram the game queue holds at the player's JOIN click -- all of it
// predates the JOIN, so none of it is the lobby being joined (the previous lobby's retail 0x0e was).
void mp_drain_pre_join_queue();
// mp:X2h -- diagnostic: log once when on_lobby_dispatch has gone quiet >= 1000 ms with a live manual
// link outside a match (a modal stopped the drain), and once when it resumes. Called from
// net_lockstep.cpp's on_present -- the per-frame path proven to keep running under a retail modal.
void mp_lobby_stall_watch();
// mp:P16 -- host: publish the per-slot SRTT table (net_seams.cpp), callable in-match; self rate-limited.
void mp_ping_publish_tick();
// N1: the client's lobby slot -- the transport/host-assigned id if known (>=1), else 1 (the 2-player
// default). In declared-id mode this equals the configured player_id, so the 2-player lobby is unchanged.
//
// THIS IS THE WIRE/CONNECTION IDENTITY, NOT necessarily our ARRAY POSITION in
// _G_LLM_LOBBY_SLOTS/Players[] -- see mp_lobby_array_index below for that one, and mp:GS1(b) for why
// the two are not interchangeable. _G_LLM_NET_LOCAL_PLAYER_INDEX (compared against wire side_ids
// elsewhere in this file) wants exactly this value; retail's real `PlayerSide`
// (mh::addr::PlayerSide) wants the array index instead.
extern "C" int MH_Net_LocalPlayerId(void); // net_transport: this client's own/host-assigned id
inline int     mp_client_slot() {
    int lp = MH_Net_LocalPlayerId();
    return (lp >= 1 && lp <= 7) ? lp : 1;
}

// mp:GS1(b) (2026-09-21): our own ARRAY POSITION in _G_LLM_LOBBY_SLOTS/Players[] -- what retail's
// session_begin_multi (`PlayerSide = _G_LLM_NET_LOCAL_PLAYER_SLOT`, VA 0x0045438c-0x004543a9) and the
// build_players_from_slots pipeline (`Players[PlayerSide].relation[k] = 2`, `.race_or_faction`, the D18
// diagonal write) actually want when they consume `mh::addr::PlayerSide`. mp_client_slot() above
// answers a DIFFERENT question (our wire/connection id), and every per-frame writer of retail
// PlayerSide in this seam family used to feed it that wire id directly, on the unstated assumption
// that a peer's wire id always equals its lobby SLOT index -- true for a slot's first occupant, false
// the instant the host's slot allocator (slot_find_or_alloc) REUSES a vacated slot for a fresh,
// higher-numbered connection (a process-exit + fresh re-join is exactly this). Two independent
// symptoms were traced to it: the lockstep committed-horizon freeze at the 10000 ms boot sentinel
// (turn_engine.cpp's participates()/commit_horizon self-exclusion, `*player_side != i`, failing to
// exclude our own real row) and a stray D18-style relation stamp landing in the WRONG (empty) Players[]
// row when build_players_from_slots_finish ran with the stale wire-id value still in PlayerSide.
//
// Resolve by SCANNING the lobby slots for the one whose player_id field (+0x01) matches our own wire
// id -- the same side_id -> array-index translation player_by_side_id does for every OTHER peer's
// messages, just applied to ourselves before Players[] exists to look it up in. The slots are kept in
// sync with the host's allocation every lobby frame, so once populated the scan is authoritative; the
// wire-id guess survives only as the pre-population fallback (right after our own JOIN, before the
// host's first per-frame slot broadcast has arrived). Every per-frame writer of retail `PlayerSide`
// (mh::addr::PlayerSide) in this TU family must use this, not mp_client_slot() -- writers of
// _G_LLM_NET_LOCAL_PLAYER_INDEX / _G_LLM_LOBBY_LOCAL_SLOT_INDEX keep using mp_client_slot(), unchanged.
inline int mp_lobby_array_index(bool is_host) {
    if (is_host) return 0;
    const int            lp          = mp_client_slot();
    const unsigned char *slots       = (const unsigned char *)mh::addr::_G_LLM_LOBBY_SLOTS; // stride 0x39
    constexpr int        SLOT_STRIDE = 0x39, SLOT_STATUS_OFF = 0x0b, SLOT_PID_OFF = 0x01;
    for (int i = 0; i < 8; ++i) {
        const unsigned char *s = slots + (size_t)i * SLOT_STRIDE;
        if (s[SLOT_STATUS_OFF] != 0 && *(const int *)(s + SLOT_PID_OFF) == lp) return i;
    }
    return lp; // slots not populated with our id yet -- pre-sync fallback, corrected within a frame or two
}

// ---- cross-TU functions defined in net_diag.cpp (perf-trace + tracer instrumentation) --------
void            temporal_capture(int id);               // record one temporal event (main-thread, mode-3 gated)
void            temporal_flush();                       // drain captured events to mh_temporal.log (from on_present)
void            install_trace_hooks();                  // [trace] funcs=VA -> run-before hooks -> mh_trace.log
int             temporal_configure();                   // read [trace] temporal; if set, init QPF + tev path. Returns the flag.
extern "C" void MH_Seam_TraceDump(const char *when);    // dump tracer call counts
extern "C" void MH_TeamRel_SetLobbyMode(int team_mode); // net_lockstep: mp:U52 -- the lobby MODE byte at session entry
extern "C" int  MH_TeamRel_Enabled(void);               // net_lockstep: mp:U52 -- [net] team_relations_fix
// lockstep-diagnostic loggers (pure log; moved from net_seams in Phase 4 stage 3):
void install_presence_lost_logger(); // presence_lost entry logger (g_ls_log-gated; from MH_Seam_Init)
void install_savegame_err_logger();  // savegame_io_error dlg CALLER logger (g_ls_log-gated; from MH_Seam_Init)
void install_exit_witness();         // D15: run-before witness on utils_abort + llm_fatal_cleanup.
                                     // UNGATED -- every exit this binary takes on its own is silent
                                     // (_exit / ExitProcess: no WER, no game log), which is why four
                                     // hosts could die at once leaving nothing to read.
// mp:P17 -- main-thread frame watchdog + focus tap (net_diag.cpp). beat: main thread, once per present.
// start: from lazy_start. [net] frame_watchdog_ms / focus_log / frame_watchdog_inject_ms.
void frame_watchdog_beat();
// [net] hitch_ms stack sampler (net_diag.cpp). Main thread calls; the watchdog thread samples the main thread MID-FRAME.
void hitch_sample_tt(long long qpc);               // a strategic frame's time_tick ran at `qpc` (marks "in game")
void hitch_sample_beat(long long qpc);             // a present completed at `qpc` (a new frame begins)
void hitch_sample_emit(long long frame_start_qpc); // print the sample(s) taken during the frame that just ended; call right after its [hitch] line
void hitch_inject_poll(long long qpc);             // [net] hitch_inject_ms test arm: one Sleep on the main thread, ~3 s into a game
void frame_watchdog_start();

// ---- cross-TU surface of reimpl_probe.cpp (P0-EXPORT proof) ----------------------------------
void reimpl_probe_install(); // shadow-check then replace w_strlen with a C++ body (from MH_Seam_Init)

// ---- F2B / D1 R1: the lockstep promotion OUTCOME, recorded on the SEAM side -------------------
//
// net_lockstep.cpp's rebind sites used to call mh::lockstep::{promotion_active,time_tick_requested,
// sim_tick_requested}() directly. A guarded call is still a link-level reference, and a link-level
// reference is what would put libmh.dll on mh_net.dll's import list -- which is the claim D1 makes
// and tools/check_net_lockstep_refs.py measures.
//
// THESE ARE NOT SELECTOR READS AND MUST NOT BECOME ONE. install_promotion can refuse (a partial
// install, an unknown `lockstep_seams=` token, a per-key `[promote]` override that survives until
// F2E), so what the rebind sites need is what the install DID, not what the configuration asked
// for. reimpl_probe.cpp owns that call site, so it records the answer the instant install_promotion
// returns: MH_Seam_Init runs reimpl_probe_install() before lockstep_install_core(), and nothing
// writes the closure's own flags in between.
//
// ALL-FALSE IS THE CORRECT ANSWER FOR A PROCESS THAT NEVER INSTALLS -- net_selftest links these TUs
// without running MH_Seam_Init, and the three predicates answered false there too.
struct seam_lockstep_promotion {
    bool active;    // install_promotion installed at least one seam
    bool time_tick; // ... and time_tick was selected; it arrives by REBIND (C4), never by install
    bool sim_tick;  // ... and sim_tick was selected AND `[promote] sim_tick` set (C6)
};
inline seam_lockstep_promotion g_lockstep_promoted = {false, false, false};
// ---- cross-TU surface of net_lockstep.cpp (mode-3 pacing/perf seams) -------------------------
// Called by MH_Core_Arm (net_seams.cpp) in this order -- arm-log line order is the refactor gate,
// and tools/check_arm_order.py is what enforces it since fork F3A:
void lockstep_install_core();             // [net] pacing knobs + overlay de-fang + hires/qpc clock +
                                          //   timing-log path + the time_tick hook. Sets g_ls_log.
void lockstep_install_present_gameover(); // present hook (frametime/eager/temporal) + game-over detour
void lockstep_transport_started();        // start the horizon-heartbeat thread (from lazy_start, off loader-lock)
// PT-SET1: the turret runtime setters the gameplay apply hooks (seams/gameplay_hooks.cpp) call. Each int-returning setter
// answers 1 = in force, 0 = refused (the fix is asked for but its splice is MISMATCHED: logged, gate stays 0 = retail).
int  mh_set_turret_reload_realtime(int on);
void mh_set_turret_fire_rate_mult(double m);
int  mh_set_turret_clip_fix(int on);
int  mh_set_turret_fire_poll_fix(int on);
int  mh_set_mother_free_build(int on);       // mods:A1
int  mh_set_mother_free_build_ai(int on);    // mods:A1
bool mh_mother_free_covers(uint32_t player); // mods:A1: the rule opens the primary-mother gate for this player
// mods:M1P: the movement-prototype setters (gameplay apply hooks). The int ones answer 1 = in force, 0 = refused (splice MISMATCHED/DISPLACED).
int  mh_set_move_centroid_wrap_fix(int on);
int  mh_set_move_detour_slot_guard(int on);
int  mh_set_move_attack_heading_fix(int on); // mods:M1X (move_proto_blocked.cpp)
int  mh_set_move_flood_queue_guard(int on);
void mh_set_route_cost_mode(int mode);
void mh_set_route_cost_base(int v);
void mh_set_route_cost_ring(int which, int v); // which = 4 / 8 / 16
void mh_set_route_gap_wave_full(int n);        // the LAST route-cost row: logs the whole set
void mh_set_route_corridor_mode(int mode);     // mods:M1C: 0 retail / 1 corridor+adjacent / 2 all regions / 3 measure only
// mods:M1X lane B (seams/move_proto_blocked.cpp): the blocked-unit protocol splice + its setters.
void install_move_proto_blocked();
void mh_set_move_block_wait_ms(int ms);
void mh_set_move_keep_sidestep(int on);
void mh_set_move_repath_cooldown_ms(int ms);
// mods:M1N (seams/move_proto_nudge.cpp): nudge an idle blocker / claim a turning leader's tile / head-on swap by stepping.
void install_move_proto_nudge();             // called from install_move_proto_blocked
int  mh_set_move_nudge_idle(int on);         // 1 = in force, 0 = refused
int  mh_set_move_claim_leader(int on);       // 1 = in force, 0 = refused
int  mh_set_move_swap_headon(int on);        // 1 = in force, 0 = refused
int  mh_set_move_route_through_idle(int on); // mods:M1N 3e-5: 1 = in force, 0 = refused (needs move_nudge_idle=1 at plan time)
void install_move_proto_arrival();           // mods:M1X arrival (seams/move_proto_arrival.cpp), from install_move_proto
int  mh_set_move_distinct_goals(int on);     // mods:M1X 2a
int  mh_set_move_near_enough(int r);         // mods:M1X 2b: R tiles, 0 = retail
int  mh_set_move_merge_goal_fix(int on);     // mods:M1X: 1 = in force (or carried by the libmh twin), 0 = refused
void install_move_proto_speed();             // mods:M1X lane C (move_proto_speed.cpp), called from install_move_proto
int  mh_set_move_group_speed(int on);        // mods:M1X: 1 = in force, 0 = refused
void install_combat_proto();                 // mods:M1C (seams/combat_proto.cpp), called from install_move_proto
int  mh_set_move_squad_merge_fix(int on);    // mods:M1C: 1 = in force (or carried by the libmh twin), 0 = refused
int  mh_set_attack_range_footprint(int on);  // mods:M1C: 1 = in force, 0 = refused
int  mh_set_projectile_facing_fix(int on);   // mods:M1C: 1 = in force (or carried by the libmh twin), 0 = refused

// ---- cross-TU surface of net_discovery.cpp (synth session / browser / S2-S4 discovery+join) --
void mp_host_advertise_session(); // S2: build + broadcast the host's SESSION_INFO (~1 Hz)
// mp:X2: the client's map-download COMPLETION report -- a re-sent JOIN carrying the hash it now
// holds. Called from the lobby tick when maps::client_take_rejoin() says a download just landed;
// it lives in net_discovery because that TU owns the stored host record and the player name.
void mp_join_resend_map_report();
bool mp_read_typed_join_ip(char *out, int cap); // U1c: the in-game typed join IP (client), if complete
// mp:R2, the RELAY SESSION SOURCE. `[net] relay` makes the transport dial a relay instead of the
// host directly, and the relay then carries a DIRECTORY of the lobbies registered on it -- which
// is what a browsing client lists when it has no host to connect to yet. Two questions cross the
// TU boundary into lazy_start (net_seams.cpp), because the room a relayed client dials is a LOBBY
// choice and lazy_start is the one place that builds MH_NetConfig:
bool     mp_relay_configured(); // `[net] relay` is set -- this peer reaches its games through one
uint32_t mp_relay_dial_room();  // the room the client should dial: a listed lobby's, or 0 for none
// mp:R7a -- the PER-DIAL relay-vs-direct decision, so `[net] relay` set no longer forces every
// connection through the relay. lazy_start hands the answer to the module in MH_NetConfig.relay_addr
// (empty = a direct dial). The relay address itself (already trimmed of any trailing `; comment`) is
// read here rather than in the module, which stops reading `[net] relay` for the dial.
bool mp_relay_addr(char *out, int cap); // the trimmed `[net] relay` value; false + out[0]='\0' if unset
bool mp_dial_is_relayed();              // the kick site's latched decision for the current manual client dial (mp:R7a)
// recv-thread control-frame handlers (net_seams' MH_Net_Arm registers them on the transport -- they
// are net steps, so `[net] enable=0` skips them along with the four transport installs; F3B):
void           on_session_info_recv(int sender, const unsigned char *buf, int len); // S3: store the host record
void           on_join_recv(int sender, const unsigned char *buf, int len);         // S4: host admits a JOIN
void           on_start_recv(int sender, const unsigned char *buf, int len);        // U2: host's FLAG_START
extern "C" int MH_Seam_TakeStartSlots(unsigned char *out, int cap);                 // U28: host's authoritative slots
extern "C" int MH_Seam_TakeStartGameplay(unsigned char *out, int cap);              // PT-SET1: host's gameplay trailer
void           on_leave_recv(int sender);                                           // U12: client left the lobby
// dead-stub detour bodies (net_seams' install_mp_bootstrap installs them; PROLOGUE-guarded there):
void discover_poll_detour();  // client discovery poll -> synth/browser record
void host_advertise_detour(); // host session-create/advertise -> mark role + ok
void ret_zero_detour();       // connect-prep / map-send no-op (return 0)
void net_disconnect_detour(); // disconnect no-op + R7: arm the first browser's directory probe
void join_connect_detour();   // client join click -> send JOIN control frame, return ok
void map_recv_step_detour();  // client map-recv step -> set the done flag (map pre-loaded)
