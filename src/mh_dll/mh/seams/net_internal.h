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
#define SHIP_LOOKAHEAD_MS "100"              // [net] lockstep_step_ms  -- horizon lookahead / input latency
#define SHIP_SIM_STEP_MS  "20"               // [net] sim_step_ms       -- strategic sim sub-step (50 Hz)
inline constexpr int SHIP_HEARTBEAT_MS = 50; // [net] horizon_heartbeat_ms -- off-frame advertisement
inline constexpr int SHIP_ADAPTIVE     = 1;  // [net] lockstep_adaptive    -- auto-tune the lookahead
// [net] rx_spin -- drain RX off-frame while stalled at the horizon, instead of idling until the next
// frame. Shipped ON since 2026-07-26: the P5 sweep measured it as a consistent 2-4 point cut in
// wall-clock deficit at EVERY lookahead (200 ms RTT), with a large drop in de-sync icon count and no
// downside in any run -- every combination stayed ALL PAIRS IDENTICAL. It was 0 only because it was
// added as an experiment and never re-defaulted.
inline constexpr int SHIP_RX_SPIN     = 1; // [net] rx_spin              -- off-frame RX drain while stalled
inline constexpr int SHIP_QPC_CLOCK   = 1; // [net] qpc_clock            -- ~1 ms game-clock quantum
inline constexpr int SHIP_HIRES_CLOCK = 1; // [net] hires_clock          -- timeBeginPeriod(1)
inline constexpr int SHIP_EAGER_ADV   = 1; // [net] eager_advertise      -- advertise from present
inline constexpr int SHIP_LOG_LEVEL   = 1; // [net] lockstep_log/frametime_log + [trace] temporal

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
inline char          g_log[MAX_PATH];      // mh_net.log path (per-run folder)
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

// [net] lockstep_log=1 -> mh_lockstep.log + the `;` DIAG lines. Written by net_lockstep.cpp's
// config (lockstep_install_core); read as the DIAG gate across net_seams (lobby dispatch, recv
// seams, finalize), the net_seams sbm logger, and the net_diag loggers.
inline bool g_ls_log = false;

// Read a game double (ms). Shared sampler for every timing log/DIAG line across the seam TUs.
inline long ms_of(uintptr_t a) {
    double d;
    memcpy(&d, (const void *)a, sizeof(double));
    return (long)(d * 1000.0);
}

// ---- cross-TU functions defined in net_seams.cpp (core transport/lobby glue) -----------------
void seam_log(const char *s); // append one line to mh_net.log (arm banners + DIAG lines)
// N1: the client's lobby slot -- the transport/host-assigned id if known (>=1), else 1 (the 2-player
// default). In declared-id mode this equals the configured player_id, so the 2-player lobby is unchanged.
extern "C" int MH_Net_LocalPlayerId(void); // net_transport: this client's own/host-assigned id
inline int     mp_client_slot() {
    int lp = MH_Net_LocalPlayerId();
    return (lp >= 1 && lp <= 7) ? lp : 1;
}

// ---- cross-TU functions defined in net_diag.cpp (perf-trace + tracer instrumentation) --------
void            temporal_capture(int id);            // record one temporal event (main-thread, mode-3 gated)
void            temporal_flush();                    // drain captured events to mh_temporal.log (from on_present)
void            install_trace_hooks();               // [trace] funcs=VA -> run-before hooks -> mh_trace.log
int             temporal_configure();                // read [trace] temporal; if set, init QPF + tev path. Returns the flag.
extern "C" void MH_Seam_TraceDump(const char *when); // dump tracer call counts
// lockstep-diagnostic loggers (pure log; moved from net_seams in Phase 4 stage 3):
void install_presence_lost_logger(); // presence_lost entry logger (g_ls_log-gated; from MH_Seam_Init)
void install_savegame_err_logger();  // savegame_io_error dlg CALLER logger (g_ls_log-gated; from MH_Seam_Init)
void install_exit_witness();         // D15: run-before witness on utils_abort + llm_fatal_cleanup.
                                     // UNGATED -- every exit this binary takes on its own is silent
                                     // (_exit / ExitProcess: no WER, no game log), which is why four
                                     // hosts could die at once leaving nothing to read.
void gm_logger_configure();          // read [net] log_gamemode + build the mh_gamemode.log path (from MH_Seam_Init)
void gm_logger_lazy_arm();           // arm the DR0 write-bp via a helper thread (from lazy_start, off loader-lock)

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

// ---- cross-TU surface of net_discovery.cpp (synth session / browser / S2-S4 discovery+join) --
void mp_host_advertise_session();               // S2: build + broadcast the host's SESSION_INFO (~1 Hz)
bool mp_read_typed_join_ip(char *out, int cap); // U1c: the in-game typed join IP (client), if complete
// recv-thread control-frame handlers (net_seams' MH_Net_Arm registers them on the transport -- they
// are net steps, so `[net] enable=0` skips them along with the four transport installs; F3B):
void           on_session_info_recv(int sender, const unsigned char *buf, int len); // S3: store the host record
void           on_join_recv(int sender, const unsigned char *buf, int len);         // S4: host admits a JOIN
void           on_start_recv(int sender, const unsigned char *buf, int len);        // U2: host's FLAG_START
extern "C" int MH_Seam_TakeStartSlots(unsigned char *out, int cap);                 // U28: host's authoritative slots
void           on_leave_recv(int sender);                                           // U12: client left the lobby
// dead-stub detour bodies (net_seams' install_mp_bootstrap installs them; PROLOGUE-guarded there):
void discover_poll_detour();  // client discovery poll -> synth/browser record
void host_advertise_detour(); // host session-create/advertise -> mark role + ok
void ret_zero_detour();       // connect-prep / disconnect / map-send no-op (return 0)
void join_connect_detour();   // client join click -> send JOIN control frame, return ok
void map_recv_step_detour();  // client map-recv step -> set the done flag (map pre-loaded)
