//
// net_lockstep.cpp -- MP LOCKSTEP pacing/perf seams, split out of net_seams.cpp (2026-07 refactor,
// Phase 4 stage 3). Everything here rides the live mode-3 lockstep sim:
// the time_tick run-before hook (lookahead/sim-step/game-speed pins + the off-frame RX drain + the
// per-frame timing log), the present-flip hook (frame-time log + eager horizon advertisement + the
// temporal-trace drain), the SYNCHRONIZING-overlay de-fang byte patches, the hi-res clock fixes
// (timeBeginPeriod + the GetTickCount-IAT->QPC redirect), the fixed-cadence horizon-heartbeat
// thread, and the game-over leave-lockstep detour. All of it is pacing / advertisement / display /
// log only -- the per-block comments carry the determinism argument for each piece.
//
// INSTALLS live here too (lockstep_install_core / lockstep_install_present_gameover), but they are
// CALLED from net_seams' MH_Seam_Init in the exact pre-split order, so the arm-log line sequence --
// part of the refactor gate -- is unchanged. lockstep_transport_started() is the third entry,
// called from net_seams' lazy_start (off loader-lock) to start the heartbeat thread.
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h> // atof (ini fractional-ms parse)

#include "include/mh_net_export.h"
#include "state/region_runtime.h"      // SB-HOSTFREE: live_base/ptr -- a movable region is read
                                       // where it IS, not where the binary put it
#include "include/mh_net_key.h"        // MH_Key_Load -- mint mh_key.txt on the first frame (see ensure_key_once)
#include "include/mh_capture_export.h" // MH_Capture_OnPresent (gfx_capture.cpp) -- UI frame capture
#include "include/mh_overlay_export.h" // MH_Overlay_OnPresent (gfx_overlay.cpp) -- debug overlay (drawn first)
#include "include/mh_uidrive_export.h" // MH_UIDrive_OnPresent (ui_drive.cpp) -- UI automation Phase 2
#include "include/mh_harness_export.h" // MH_Harness_RebindSimTick -- C6 sim_tick promotion by rebind
#include "include/mh_module_bind.h"    // MH_Libmh_OnPresent -- F4D's spine-crossing report
#include "config/config.h"             // F2A: the D11 selector behind the frame pair's promotion default
#include "addr/mh_addrs.gen.h"         // generated EN VAs (tools/gen_dll_addrs.py)
#include "net_internal.h"              // shared spine: PROLOGUE, TEV_*, g_ini/g_log, g_ls_log, ms_of, net_diag decls
#include "hook/detour.h"               // install_trampoline (shared toolkit)
#include "hook/hookpoint.h"            // D5/R7: the named hook points -- the frame-pair + prelude handoffs
#include "hook/watcall.h"              // call_watcall1 (bridge into __watcall llm_net_player_remove) -- U17
#include "hook/patch.h"                // patch_bytes_guarded
#include "hook/promoted.h"             // promoted_owner_of -- suppressed is not the same as MISMATCHED
#include "fix/resync_clamp.h"          // MP D14 clamp (R4: shared, closure-neutral; unit-tested in lockstest)
#include "lockstep/turn_engine.h"      // set_icon_counters / reimpl_fixes / the two promotion entry thunks
#include "addr/mh_export.gen.h"        // entry_llm_strat_time_tick (the C4 direct-install fallback)
#include "hook/export.h"               // install_export_ok
#include "ui/lobby_ui.h"               // D4: the present hook drives two UI-module repaints

// R7 RESOLVED (fork F3C): the two mh::sim installs that used to be forward-declared and called from
// here are GONE. LT1F's frame-pair promotion and SIM-SAVE-DIV's time_resync prelude are now reached
// through the named hook points (hook/hookpoint.h) -- net code registers its two chain hooks by NAME
// and asks mh.dll to install, instead of naming a sim installer across the fork boundary. The
// routing decision, the refusal and the log line still belong to mh/sim; only the caller moved.
// check_net_lockstep_refs counted these as config-(1) coupling residue and no longer sees them.

#pragma comment(lib, "user32.lib") // wsprintfA
#pragma comment(lib, "winmm.lib")  // timeBeginPeriod (hires_clock)
// WIN32_LEAN_AND_MEAN drops <mmsystem.h>, so declare the one multimedia-timer call we use.
extern "C" __declspec(dllimport) unsigned int __stdcall timeBeginPeriod(unsigned int uPeriod);

using mh::hook::install_trampoline;
using mh::hook::patch_bytes_guarded;

namespace {

// mh.exe fixed VAs (generated EN header; image base 0x00400000, no ASLR).
constexpr uintptr_t ADDR_TIME_TICK = mh::addr::llm_strat_time_tick;             // per-frame; adapts STEP_SIZE
constexpr uintptr_t ADDR_STEP_SIZE = mh::addr::_G_LLM_STRAT_LOCKSTEP_STEP_SIZE; // double: lookahead, game-sec
constexpr uintptr_t ADDR_PRESENT   = mh::addr::llm_gfx_present_flip;            // the DirectDraw blit/flip = 1/frame
constexpr uintptr_t ADDR_GAME_MODE = mh::addr::_G_LLM_GAME_MODE;                // byte; 2=strategic, 3=sync overlay, 6=tactical

// lockstep timing-log source globals (all doubles unless noted)
constexpr uintptr_t ADDR_SESSION_MODE = mh::addr::_G_LLM_GAME_SESSION_MODE; // byte; 3 = lockstep
constexpr uintptr_t ADDR_GAME_CLOCK   = mh::addr::_G_LLM_STRAT_GAME_CLOCK;
// SB-HOSTFREE: a FUNCTION, not a constant -- this region is MOVABLE and a relocating host
// leaves 0xCD at the stock address.
inline uintptr_t ADDR_SIM_STEP_INT() { // double; ~0.1 game-s per sim step
    return mh::state::live_base(mh::state::RID_STRAT_SIM_STEP_INTERVAL);
}
constexpr uintptr_t ADDR_TOTAL_TIME    = mh::addr::TOTAL_GAME_TIME;               // sim clock, clamped to committed
constexpr uintptr_t ADDR_LOCAL_HORIZON = mh::addr::_G_LLM_STRAT_LOCKSTEP_HORIZON; // our requested
// SB-HOSTFREE: a FUNCTION, not a `constexpr uintptr_t`. This region is MOVABLE -- a
// relocating host puts it in its own arena and fills the .bss it left with 0xCD -- so a
// constant baked at compile time reads poison. tools/check_movable_addresses.py is the gate.
inline uintptr_t ADDR_COMMITTED() { // min over peers
    return mh::state::live_base(mh::state::RID_STRAT_LOCKSTEP_COMMITTED_HORIZON);
}
inline uintptr_t ADDR_PEER_HORIZON() { // [8]
    return mh::state::live_base(mh::state::RID_NET_PEER_HORIZON);
}
constexpr uintptr_t ADDR_COMMIT_HORIZON_FN = mh::addr::llm_net_lockstep_commit_horizon;   // void(void), main-thread; recomputes COMMITTED = min(local_h, active network-human peers)
constexpr uintptr_t ADDR_LOCKSTEP_PUMP     = mh::addr::llm_net_lockstep_pump;             // void(void); per-frame net pump (order flush + send_extend + RX drain via dispatch). Called in the rx_spin busy-wait.
constexpr uintptr_t ADDR_GAME_SPEED        = mh::addr::game_speed;                        // double; time_tick's dt multiplier
constexpr uintptr_t ADDR_GAMEOVER_DLG      = mh::addr::gameover_outcome_dialog;           // the game-over/outcome dialog (code 4/5/6/7/8)
constexpr uintptr_t ADDR_STALL_COUNT       = mh::addr::_G_LLM_STRAT_LOCKSTEP_STALL_COUNT; // int
constexpr uintptr_t ADDR_PLAYER_COUNT      = mh::addr::_G_LLM_NET_LOCKSTEP_PLAYER_COUNT;  // int
// Phase 2c thread-1 (client-drop) diagnosis globals -- the grace-timeout removal path in time_tick.
constexpr uintptr_t ADDR_STATUS_FLAGS = mh::addr::_G_LLM_NET_LOCKSTEP_STATUS_FLAGS; // byte; 0x80 = awaiting-drop
// SB-HOSTFREE: a FUNCTION, not a constant -- this region is MOVABLE and a relocating host
// leaves 0xCD at the stock address.
inline uintptr_t ADDR_GRACE_TIMER() { // peer-timeout accumulator (game-sec; removes at >5.0)
    return mh::state::live_base(mh::state::RID_NET_LOCKSTEP_PEER_TIMEOUT_ELAPSED);
}
// SB-HOSTFREE: a FUNCTION, not a constant -- this region is MOVABLE and a relocating host
// leaves 0xCD at the stock address.
inline uintptr_t ADDR_SYNC_ACCUM() { // sync-wait accumulator (game-sec)
    return mh::state::live_base(mh::state::RID_NET_LOCKSTEP_SYNC_WAIT_ELAPSED);
}
// SB-HOSTFREE: a FUNCTION, not a constant -- this region is MOVABLE and a relocating host
// leaves 0xCD at the stock address.
inline uintptr_t ADDR_SYNC_WAIT() { // int
    return mh::state::live_base(mh::state::RID_NET_SYNC_WAIT_ACTIVE);
}
constexpr uintptr_t ADDR_SYNC_COUNTDN  = mh::addr::_G_LLM_NET_LOCKSTEP_SYNC_RETRY_COUNTDOWN; // int; starts 0x3c
constexpr uintptr_t ADDR_PLAYERCT_54BC = mh::addr::mode3_trigger_player_count;               // decremented on removal

// SYNCHRONIZING-overlay de-fang (perf-decouple step 2). In llm_strat_time_tick's horizon-stall path,
// two calls flip _G_LLM_GAME_MODE to 3 (the modal SYNCHRONIZING overlay) whenever the sim can't make a
// full step -- which at a tight lookahead is EVERY step, so each 100 ms of clock costs a ~2 s input-
// locked freeze (the overlay dismiss is gated by a ~1 s accumulator, not by the peer horizon that the
// heartbeat already keeps fresh). Suppressing these two calls keeps the frame on the strategic path
// (GAME_MODE stays 2), so a horizon-wait costs ~1 frame of latency instead of the modal. Display/pacing
// only -- the sim still only steps when it CAN (committed = min(peers)), so determinism is untouched.
constexpr uintptr_t ADDR_OVL_WAIT_CALL = mh::addr::ovl_wait_call_site; // call llm_net_lockstep_wait_player_overlay_show
// EN expected bytes: these are `E8 rel32` calls whose SITE is in llm_strat_time_tick (delta-0 band)
// but whose TARGET (the overlay funcs) sits +0x504 above the RU VA, so the rel32 is EN-specific.
// Verified against /eng/mh.exe (the EN port notes).
const uint8_t OVL_WAIT_EXPECT[5] = {0xE8, 0xBF, 0x8C, 0x08, 0x00}; // call 0x004c7dc0

// The DOMINANT in-game freeze (2026-07-11, GAME_MODE logger): a lockstep-extend SYSTEM order dispatched
// in llm_strat_order_queue_dispatch calls llm_net_lockstep_extend_ui_enter (0x004c80b9), which saves the
// mode, shows the wait overlay, and sets _G_LLM_GAME_MODE=8 -> the strategic frame is skipped ~2 s until
// the peer's completion dismisses it (FUN_004c80fa restores mode from the SAVE). We keep the save and
// suppress only the overlay call + the mode-8 write, so the mode stays 2 (strategic frame keeps running,
// input responsive) and FUN_004c80fa still restores correctly. The sim is clamped to committed either way
// -> determinism holds; the mode-8 modal was only a visual/input-lock. This is the real fix (the two
// calls above are the smaller mode-3 path). Patch sites are INSIDE extend_ui_enter (single caller).
constexpr uintptr_t ADDR_OVL_XUI_WAIT  = mh::addr::ovl_xui_wait_site;  // call wait_player_overlay_show (inside extend_ui_enter)
constexpr uintptr_t ADDR_OVL_XUI_MODE8 = mh::addr::ovl_xui_mode8_site; // mov byte [_G_LLM_GAME_MODE], 8
// XUI pair: site AND target both shifted +0x504 on EN -> rel32 unchanged; the MODE8 mov targets
// absolute data (RU/EN-identical) -> both expect strings are build-invariant.
const uint8_t OVL_XUI_WAIT_EXPECT[5]  = {0xE8, 0xD3, 0xF7, 0xFF, 0xFF};             // call wait_player_overlay_show
const uint8_t OVL_XUI_MODE8_EXPECT[7] = {0xC6, 0x05, 0xD0, 0x02, 0x52, 0x00, 0x08}; // mov byte[0x5202d0],8
const uint8_t OVL_XUI_WAIT_PATCH[5]   = {0x90, 0x90, 0x90, 0x90, 0x90};             // NOP the overlay call
const uint8_t OVL_XUI_MODE8_PATCH[7]  = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90}; // NOP the mode=8 write

// Building-dialog flicker (GAME_MODE=3 overload) -- ISOLATED FIX (2026-07-12). The building info/status
// dialog (FUN_004c70d5, the deconstruct/status popup) OPENS by setting GAME_MODE=3 with an EMPTY widget
// list (widget_list=0) + its own draw ptr, and is CLOSED by llm_net_lockstep_overlay_dismiss (0x004c7818)
// resetting GAME_MODE=2 (which stops the mode-3 overlay dispatch). llm_strat_time_tick also calls that same
// dismiss on every SYNCHRONIZING-overlay recovery -- but with the overlay SHOW already de-fanged above, those
// dismiss calls have no overlay to tear down; their only remaining effect while a building dialog is up is to
// spuriously close it -> the 3<->2 flicker.
// FIX: symmetric to the show-suppression, NOP the three `call overlay_dismiss` sites INSIDE time_tick. The
// peer-timeout accumulator reset (DOUBLE_00e587a1 = -1.0) and the SYNC_WAIT/countdown resets are SEPARATE
// stores that run BEFORE these calls, so they are PRESERVED -- unlike the reverted SYNC_WAIT_ACTIVE-gate
// neuter, which skipped the whole block and broke the reset (false peer-timeout/disconnect, worse at 1.5x).
// The dialog's own user-close (a different dismiss caller) is unaffected. Gated with defang (the
// show-suppression this completes). Determinism-safe: display/teardown only, the sim still steps only when
// committed. (Old approach: NOP the SYNC_WAIT_ACTIVE=1 write at 0x0043f0ad -- reverted; do NOT reintroduce.)

// NOTE (2026-07-16): a "preserve_overshoot" experiment NOPed the TOTAL=committed clamp at 0x0043f012 to try
// to recover the ~0.74-0.91x sim-rate loss. It was REVERTED -- that clamp is LOAD-BEARING: llm_strat_sim_tick's
// catch-up loop bounds on TOTAL_GAME_TIME, and the clamp is what caps TOTAL to the committed horizon. Removing
// it free-runs the sim at wall speed (measured 1.000x) and the peers desync (213 steps, game_clock diverged).
// So the per-step frame-quantization loss is INTRINSIC to lockstep, not a wasteful discard.

// Lockstep-lookahead pin (mh_net.ini [net] lockstep_step_ms). The game defaults
// _G_LLM_STRAT_LOCKSTEP_STEP_SIZE to 10.0 GAME-seconds and adaptively scales it by FPS/stalls in
// llm_strat_time_tick -- an FPS heuristic, NOT a network one -- which produces multi-second input lag
// (and ratchets up after an alt-tab FPS crash). When >0 we pin STEP_SIZE to this value every frame
// (run-before hook on time_tick), overriding the adaptive scaling. 0 = leave the game's default.
double g_lockstep_step = 0.0;   // pinned lookahead in game-seconds (0 = off)
bool   g_rx_spin       = false; // off-frame RX drain (adaptive lookahead (b) Step 2): during a
                                // horizon stall, busy-poll the net pump + commit until COMMITTED rises,
                                // instead of yielding a whole ~frame for the next per-frame pump. Breaks the
                                // ~2-frame confirmation floor. Main-thread only (no race); determinism-safe.
double g_sim_step = 0.0;        // pinned strategic sim SUB-STEP interval in game-seconds (0 = off, leave game
                                // default 0.1s = 10Hz). INDEPENDENT of the lockstep lookahead: smaller => the
                                // committed window is consumed in finer sim steps (llm_strat_sim_tick's catch-up
                                // loop), so unit position updates finely within the SAME network horizon =>
                                // smoother movement without extra traffic. Fixed-timestep => still deterministic;
                                // it DOES change the sim outcome (AI/timers tick more often), so both peers must
                                // set the SAME value. Movement-smoothness experiment.
double g_game_speed = 0.0;      // pinned game_speed multiplier (0 = off / leave game default). Experiment:
                                // >1 runs the deterministic sim faster in wall-time (both peers set it) to
                                // test whether more-frequent position updates smooth unit movement. Note the
                                // real rate is still capped by the lockstep round-trip at a tight lookahead.
void *g_tt_tramp = nullptr;

// --- Horizon heartbeat (perf-decouple) --------------------------------
// The game advertises its lockstep horizon only from the render/frame path: llm_strat_time_tick (and
// the pump/dispatch/input paths) set _G_LLM_STRAT_LOCKSTEP_HORIZON = GAME_CLOCK + STEP_SIZE and call
// llm_net_send_lockstep_extend, which emits a 9-byte packet [type=2][double horizon]. So a RENDER
// hitch stops horizon advertisement and STARVES the peer's sim -> the 2 s SYNCHRONIZING overlay, and
// at a small lookahead a self-reinforcing freeze-storm (measured ~40 freezes/100 s on the VM peer).
// This thread re-emits that EXACT EXTEND packet on a fixed REAL-TIME cadence, independent of rendering,
// so a peer's render stall no longer stalls the other peer. Determinism-preserving: it advertises
// PACING only -- it never advances GAME_CLOCK/TOTAL_GAME_TIME or steps the sim, so the sim stays a pure
// function of (start, seed, tick-ordered orders); committed = min(peers) is unchanged in VALUE, just
// confirmed more regularly. Both peers run it. Gated by mh_net.ini [net] horizon_heartbeat_ms (0=off).
volatile LONG g_hb_run       = 0;
HANDLE        g_hb_thread    = nullptr;
int           g_hb_ms        = 0; // heartbeat period in real ms (0 = off)
DWORD         g_last_ls_tick = 0; // GetTickCount() when SESSION_MODE was last 3 (for the endgame grace)
// Endgame-sync grace: the win/loss resolver (llm_strat_player_presence_lost) leaves lockstep
// (SESSION_MODE 3->2) the instant the HOST reaches the deciding sim step, stopping advertisement -- so a
// peer one lookahead behind can never reach that step to resolve its own game-over (it just sees "host
// left"). Keep advertising the frozen final horizon for this window after leaving 3 so the trailing peer
// catches up and ends deterministically. Determinism-safe (advertisement only).
constexpr DWORD HB_ENDGAME_GRACE_MS = 3000;
int             g_defang            = 0; // 1 = suppress the in-game SYNCHRONIZING overlay (perf-decouple step 2)
int             g_overlay_gate      = 0; // 1 = redirect the wait-overlay call through the SYNC_RETRY_COUNTDOWN gate (root-cause fix; owns the wait-overlay site instead of the defang NOP)
int             g_resync_wait_fix   = 1; // 1 (default) = neuter sync_delay_stub's uninit return -> kills the resync busy-wait garbage-spin hang (defang dep #2, the REAL root cause; net_resync_wait_fix)
// Fine-grained per-group overlay-patch knobs (default from defang_overlay/overlay_gate, each overridable
// via [net] defang_tt_wait / defang_tt_sync / defang_xui / defang_dismiss). For isolating WHICH patch kills
// the freeze while keeping the "Player not responding" kick modal (tt_sync) live. See install_overlay_patches.

// ---- P4: make the "de-sync icon storm" a number -------------------------------------------------
// After the first long internet game the player reported "a lot of de-sync icons, felt as micro-
// freezes" -- and there was no way to check, so no pacing experiment could be judged. These count the
// wait-overlay call site: g_icon_calls every time the game WANTS the icon, g_icon_shown every time one
// is actually drawn (they differ only when the gate is armed, which is exactly the gate's value made
// visible). Written by the naked thunk, so plain longs at a fixed address, not statics-in-a-function.
// Diagnostic only: a counter in our DLL is invisible to the sim, so this cannot perturb determinism.
long g_icon_calls = 0, g_icon_shown = 0;
long g_icon_gate  = 0; // mirrors defang_tt_wait==2 for the thunk (asm cannot read a C++ bool cheaply)
int  g_icon_count = 1; // [net] icon_count -- install the counting thunk even when nothing is gated

// U20 (2026-08-30): THE SAME TWO COUNTERS, FED FROM THE PROMOTED BODY. The thunk above can only
// count while llm_strat_time_tick runs the ORIGINAL, and promotion has been the ship default since
// SHIP_PROMOTE_LOCKSTEP -- so on every shipped build these columns read 0 and that zero meant
// "nobody counted", not "no icons". These two are handed to mh::lockstep::set_icon_counters() so the
// reimplemented body feeds the identical longs: same columns, same readers, no third column.
// EXACTLY ONE of the two writers is ever live over the site (C1's interlock guarantees it), so they
// cannot double-count.
void icon_note_wanted() { ++g_icon_calls; }
void icon_note_shown() { ++g_icon_shown; }
int  g_desync_icon_gate = 0; // [net] desync_icon_gate -- MP U20; reimpl-only, see reimpl_fixes
int  g_defang_xui       = 0; // extend_ui_enter wait+mode8 pair (the DOMINANT ~2s mode-8 freeze): 0=live 1=NOP
int  g_resync_trigger_reset =
    0; // 1 = zero RESYNC_TRIGGER_COUNT on horizon recovery (spurious-resync ROOT fix, option b; see install_resync_trigger_reset)
int g_resync_trigger_gate =
    0; // 1 = gate both RESYNC_TRIGGER_COUNT increments on SYNC_RETRY_COUNTDOWN<0x38 (option c; count only genuine silence)
int g_resync_order_horizon =
    1;               // 1 (default) = clamp the CTL_RESYNC_BEGIN synthetic order's exec_time to LOCKSTEP_HORIZON (MP D14; see install_resync_order_horizon)
int g_eager_adv = 0; // 1 = advertise+commit the post-step horizon in the present hook (perf-decouple diagnostic: proved committed is never the binding constraint; determinism-safe, no rate effect)

// Game-over leave-lockstep. Detours the common outcome dialog (0x004c6c4f) to downgrade SESSION_MODE
// 3->2 on entry, so a peer showing its result is never still in lockstep waiting for someone who left.
// The game is already decided for this peer, so this is determinism-safe.
//
// U30, 2026-08-29 -- THE PREMISE THIS WAS WRITTEN ON IS WRONG, and the correction is worth keeping
// because it changes what the detour is FOR. It used to say: "the retail resolver downgrades ONLY on
// the WINNER's branch; the LOSER stays in SESSION_MP_LOCKSTEP behind its 'You lost' dialog and
// FREEZES". Read llm_strat_player_presence_lost (0x00498089) again: its `if (SESSION == 3) SESSION =
// 2` sits AFTER the winner/loser if-else closes, so BOTH arms run it -- the loser's arm
// (PlayerSide == player: outcome code 4, send_presence_lost) falls straight into it. MEASURED on both
// peers of a conquest run: the loser and the winner each log `on_gameover ENTER sess=2 (downgrade=0)`,
// i.e. retail had already left lockstep before the dialog opened.
//
// THE DETOUR IS THEREFORE INERT IN THIS BUILD, and that is a reachability result, not a guess.
// llm_ui_outcome_dialog (0x004c6c4f) has exactly FOUR callers:
//   * llm_strat_unit_on_destroyed and llm_game_sp_outcome_announce -- both gated on SESSION_SP;
//   * llm_strat_player_presence_lost -- downgrades first, on both arms, as above;
//   * llm_net_lockstep_dispatch's GARBLED-STREAM arm (0x0049d22d -> outcome code 7 at 0x0049d30f),
//     which eliminates the other humans, runs the teardown hook and opens the dialog WITHOUT touching
//     SESSION_MODE. That is the one caller that could reach it at SESSION==3 -- and it is UNREACHABLE
//     HERE, because MH_Seam_GameRecv (net_seams.cpp) drops any datagram whose type byte is outside
//     1..5 before the game sees it. That filter has been in since 2026-07-11, for its own reason (a
//     leaked LOBBY packet was tearing the session down at ~step 31).
// MEASURED 2026-08-29 rather than argued: [harness] garble_at=120 on the host sent one outer-tag-6
// frame; the client logged `GameRecv DROP non-lockstep sender=0 len=1 type=0x06`, never entered
// on_gameover, and the run stayed determinism-clean to step 300.
// KEEP IT ANYWAY -- it costs one entry claim and it is the guard if the recv filter is ever widened or
// a promoted dispatch changes -- but do not re-tell the freeze story, and know that arming it changed
// no behaviour. (The trade this used to be weighed against -- the effects gate that wanted the same
// entry -- is gone: fork F2F deleted the gates, so the entry is uncontested.)
int g_sync_gameover = 0; // 1 = install the game-over leave-lockstep detour
// U17 (a) clean in-game leave: when THIS peer quits a running lockstep game (ESC->Quit->Yes ->
// llm_game_return_to_main_menu_cb), broadcast our own removal BEFORE the teardown so survivors drop us
// in-order. DEFAULT OFF (2026-07-25): B2 (graceful_drop) already catches a clean quit via the socket-close,
// so (a) is redundant for the survivor side; its quitter-side self-removal isn't end-to-end rig-tested yet
// (possible 2-player game-over flash). Opt-in via [net] graceful_leave=1.
int   g_graceful_leave = 0;
void *g_quit_tramp     = nullptr;
// The game-over fix's trampoline. Introduced by F1C as the no-gate fallback; the only host since
// fork F2F dropped the effects gates.
void *g_go_tramp = nullptr;
// U17 (b) fast HARD-drop: when a client's transport connection dies (kill/disconnect), the host's recv
// thread latches its id (MH_Net_TakeDeadPeer); on the next frame on_time_tick (main thread) broadcasts an
// immediate in-order removal instead of waiting out the ~60-count silence timeout. Host-only by construction
// (a client's host-conn latches -1). Default ON.
int g_graceful_drop = 1;
// D5: the transport-dead peer latched but NOT yet removed -- held until the sim clock has
// reached the committed horizon (retail's parked precondition). -1 = nothing pending.
int g_pending_dead      = -1;
int g_pending_dead_wait = 0;
int g_pending_dead_max  = 600; // frames (~10 s at 60 fps) before the safety valve fires anyway

// Optional per-frame lockstep timing log (mh_net.ini [net] lockstep_log=1 -> mh_lockstep.log). One
// line per strategic frame while in mode-3, so freezes show up as large wall-time gaps between rows
// and we can see WHETHER the sim is starved by the peer horizon, the pump cadence, or rx delivery.
// (The g_ls_log gate itself lives in net_internal.h -- net_seams + net_diag read it for DIAG gating.)
HANDLE g_ls_h = INVALID_HANDLE_VALUE;
char   g_ls_path[MAX_PATH];

// SP clock cross-check (mh_net.ini [net] sp_clock_log=1). Bypasses the mode-3
// gate below so the SAME per-frame row (wall_ms/total_ms/clock_ms) is emitted in a SINGLE-PLAYER --load
// run -- no lockstep, no TOTAL=committed clamp -- to isolate whether the ~3% "sub-wall game clock" the
// MP free-climb measured is lockstep-independent. Log-only, so determinism-irrelevant. In SP the lockstep
// columns (committed/peers/stall/tx/rx) are just idle/zero; only wall_ms + total_ms + clock_ms matter.
bool g_ls_log_sp = false;

// Optional per-PRESENT frame-time log (mh_net.ini [net] frametime_log=1 -> mh_frametime.log). Phase 3:
// llm_gfx_present_flip is the actual DirectDraw blit/flip, so present-to-present deltas here are what the
// player FEELS -- distinct from the sim/net cadence the lockstep log samples. One line per presented
// frame: high-res QPC microseconds + the game mode (2=strategic, 3=sync overlay, 6=tactical) so the
// analyzer can isolate strategic-gameplay frame times and catch the ~2s overlay hitches.
bool   g_ft_log = false;
HANDLE g_ft_h   = INVALID_HANDLE_VALUE;
char   g_ft_path[MAX_PATH];
// g_qpc_freq -> net_internal.h (shared: frametime log here + net_diag.cpp's temporal trace)
void *g_ft_tramp = nullptr;

// The per-EVENT temporal trace ([trace] temporal=1 -> mh_temporal.log) lives in net_diag.cpp; the
// present/time_tick detours here call temporal_capture()/temporal_flush() (declared in net_internal.h).

// Mint mh_key.txt on the first rendered frame, if it does not exist yet.
//
// The transport loads the key when it starts, which is the first time the player hosts or joins --
// but a host needs to SEND the key to its players BEFORE that, and "the file appears only after you
// have already created a game" is a confusing first-run experience. Doing it here instead makes the
// file exist from the first launch, as the docs promise.
//
// Why not in MH_Seam_Init: that runs in DllMain, and minting a key needs the system CSPRNG, which we
// reach via LoadLibrary("advapi32") -- a LoadLibrary under the loader lock is a documented deadlock.
// on_present is the first place that is both off the loader lock and guaranteed to run.
void ensure_key_once() {
    static bool done = false;
    if (done) return;
    done = true;
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    char *slash = exe;
    for (char *p = exe; *p; ++p)
        if (*p == '\\' || *p == '/') slash = p;
    slash[1] = '\0';
    unsigned char key[MH_KEY_LEN];
    char          hex[MH_KEY_HEX_LEN + 1] = {0};
    int           generated               = 0;
    // F4B: MH_Key_Load is one of the 23 symbols that now live in mh_net.dll, reached through
    // module_bind.cpp's forwarding shim. With no module it answers MH_KEY_OPEN with nothing
    // generated, so this function goes silent -- which is the right answer: a process with no
    // transport has no session to key, and printing first-run key instructions for a multiplayer
    // that cannot start would be advice about a thing that is not there. No guard is needed here
    // because the surface already carries it (mh_net_module.h).
    int st = MH_Key_Load(exe, key, hex, &generated);
    if (st == MH_KEY_SECURE && generated) {
        char b[220];
        wsprintfA(b, "; mh_key.txt generated -- send this file (or the line below) to the players joining you:\n;   %s\n", hex);
        seam_log(b);
    } else if (st == MH_KEY_INVALID) {
        seam_log("; mh_key.txt is unreadable/corrupt -- multiplayer will REFUSE to start. Fix or delete it.\n");
    }
}

void on_present() {
    // THE MODULE BIND USED TO HAVE A SECOND ARM HERE (F4A's MH_ModuleBind_OnPresent, mechanism B:
    // off the loader lock, on the first rendered frame). F4B deleted it with the spike it armed --
    // mh_net.dll must be bound before MH_Core_Arm_Early, so the DllMain arm is the only one with a
    // subject, and a second arm point nothing calls is the dead-knob shape F4A ruling (a) removes.
    // The mechanism stays RECORDED in docs/dll-split.md as the documented fallback, and this is
    // still where it would go: "the first place that is both off the loader lock and guaranteed to
    // run", which is also why ensure_key_once sits here (minting a key needs LoadLibrary advapi32).
    MH_Libmh_OnPresent();          // F4D's standing arm: report the spine-boundary crossing counters
    ensure_key_once();             // first frame: make sure the host has a key it can share
    mh::ui::browser_notice_tick(); // U23: keep the involuntary-exit notice on the browser status line
    mh::ui::slide_geom_watch();    // U37 diag ([net] slide_diag): log any change to the menu frame geometry
    MH_Overlay_OnPresent();        // debug overlay: paint BEFORE capture reads, so captured frames include it
    MH_Capture_OnPresent();        // UI capture harness: grab the composed frame if a trigger fired (cheap when idle)
    MH_UIDrive_OnPresent();        // UI automation harness (Phase 2): hotkeys + auto-click state predicate (cheap when idle)
    MH_Harness_OnPresent();        // UI-REC: the mode-agnostic input journal's cadence + the menu's pinned-clock advance

    temporal_capture(TEV_PRESENT); // frame-end event (post time_tick+sim_tick: post-clamp clock/total)
    temporal_flush();              // incremental drain, post-present -> off the sim path
    // Eager horizon advertisement (perf-decouple). present runs AFTER sim_tick
    // in the strategic frame (llm_strat_frame: time_tick -> sim_tick -> render_present), so GAME_CLOCK here
    // already reflects THIS frame's sim_step(s). Retail re-advertises that advanced clock only on the NEXT
    // frame's time_tick (which computes HORIZON *before* sim_tick moves the clock), and time_tick hard-clamps
    // TOTAL=committed with the stalled wall-time DISCARDED -- so at a 1-step lookahead every step forfeits ~1
    // frame (~25 ms) -> 0.74x on a ~0 ms link (root cause measured, cross-validated 2026-07-16).
    // Advertising + committing the post-step horizon right here closes that gap: OUR committed rises this
    // frame and the peer learns the new horizon a frame sooner, so the sim becomes wall-bound (~1.0x) with NO
    // lookahead change (no input-latency cost). Determinism-safe: advertisement/commit PACING only -- the
    // sim_step sequence stays bounded by committed=min(peers) and wall time; COMMITTED's VALUE is unchanged,
    // just reached sooner. commit_horizon runs main-thread here, as do all its retail callers. Gated by
    // [net] eager_advertise (0=off). MUST re-pass the per-step region-hash oracle (the MP perf notes S4).
    if (g_eager_adv && *(const uint8_t *)ADDR_SESSION_MODE == 3 &&
        MH_Net_IsStarted() && MH_Net_PeerCount() > 0) {
        double clock;
        memcpy(&clock, (const void *)ADDR_GAME_CLOCK, sizeof(double));
        double step;
        if (g_lockstep_step > 0.0) step = g_lockstep_step;
        else memcpy(&step, (const void *)ADDR_STEP_SIZE, sizeof(double));
        double horizon = clock + step;
        memcpy((void *)ADDR_LOCAL_HORIZON, &horizon, sizeof(double)); // keep OUR requested horizon fresh
        unsigned char pkt[9];
        pkt[0] = 2;
        memcpy(pkt + 1, &horizon, sizeof(double)); // type 2 = EXTEND
        MH_Net_Send(MH_NET_BROADCAST, pkt, 9);     // tell peers now (g_conn_cs-locked)
        ((void (*)())ADDR_COMMIT_HORIZON_FN)();    // raise OUR committed THIS frame
    }
    if (!g_ft_log) return;
    if (g_ft_h == INVALID_HANDLE_VALUE) {
        g_ft_h = CreateFileA(g_ft_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (g_ft_h == INVALID_HANDLE_VALUE) {
            g_ft_log = false;
            return;
        }
        QueryPerformanceFrequency(&g_qpc_freq);
        char h[96];
        // D22: no qpc_freq field here -- this column is ALREADY converted to microseconds below
        // (t.QuadPart * 1e6 / g_qpc_freq), so the raw tick rate is not a divisor for it and a
        // reader who printed it and divided qpc_us by it would get a timeline ~10x too short.
        // The column name states the actual unit; that is the whole contract.
        int   hn = wsprintfA(h, "# qpc_us game_mode\n");
        DWORD w;
        WriteFile(g_ft_h, h, hn, &w, nullptr);
    }
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    // microseconds since an arbitrary origin (analyzer only uses deltas)
    long long us = g_qpc_freq.QuadPart ? (t.QuadPart * 1000000LL) / g_qpc_freq.QuadPart : t.QuadPart;
    char      line[64];
    int       n = wsprintfA(line, "%I64d %d\n", us, (int)*(const uint8_t *)ADDR_GAME_MODE);
    DWORD     w;
    WriteFile(g_ft_h, line, n, &w, nullptr);
}

__declspec(naked) void present_detour() {
    __asm {
        pushad
        pushfd
        call on_present
        popfd
        popad
        jmp  dword ptr [g_ft_tramp] // stolen 8-byte prologue + jmp back to present+8
    }
}

// Phase 2c thread-1: keep logging ACROSS the mode-3 exit. The old `mode!=3 -> return` gate hid the
// exact frame the client leaves lockstep (its log went tiny). Latch on first mode-3, then keep logging
// every strategic time_tick through the drop for a bounded post-window so we catch the transition +
// the grace timer climbing to 5.0 + the 0x80 awaiting-drop flag + the DAT_005d54bc decrement.
bool          g_ls_seen3   = false;
int           g_ls_post3   = 0;    // strategic frames logged after leaving mode 3
constexpr int LS_POST3_MAX = 1200; // ~20s@60fps cap so a legit mode-2 tail can't grow the log forever
// Sim-tick BURST tracking (movement-discontinuity visibility). GAME_CLOCK advances
// exactly SIM_STEP_INTERVAL per sim_step (llm_strat_sim_tick's catch-up while-loop), so the ClockMs delta
// between consecutive logged frames / interval = how many sim_steps that frame advanced. 1 = smooth,
// 0 = starved/waiting peer horizon, N>1 = a CATCH-UP BURST (units jump N microsteps at once -> the
// visible stutter). No extra hook -- pure delta of a value ls_log already reads.
long g_ls_prev_clock_ms = -1;

void ls_log_tick() {
    if (!g_ls_log && !g_ls_log_sp) return;
    int sess = (int)*(const uint8_t *)ADDR_SESSION_MODE;
    if (!g_ls_log_sp) { // normal MP behavior: gate on having entered lockstep
        if (sess == 3) {
            g_ls_seen3 = true;
            g_ls_post3 = 0;
        } else {
            if (!g_ls_seen3) return;                // never entered lockstep yet -> nothing to trace
            if (g_ls_post3 >= LS_POST3_MAX) return; // captured the transition + tail; stop
            ++g_ls_post3;
        }
    }
    // sp_clock_log: log every strategic frame unconditionally (mode-2 SP has no lockstep to gate on).
    if (g_ls_h == INVALID_HANDLE_VALUE) {
        g_ls_h = CreateFileA(g_ls_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (g_ls_h == INVALID_HANDLE_VALUE) {
            g_ls_log = false;
            return;
        }
        const char *hdr = "# wall_ms clock_ms total_ms local_h_ms committed_ms peer0_ms peer1_ms "
                          "step_ms stall pcount tx_pkts rx_pkts since_rx_ms "
                          "sess game flags grace_ms syncwait countdn p54bc sync_ms sim_burst "
                          "icon_calls icon_shown\n"; // P4: CUMULATIVE -- diff two rows for a rate
        DWORD       w;
        WriteFile(g_ls_h, hdr, lstrlenA(hdr), &w, nullptr);
        g_ls_prev_clock_ms = -1; // fresh file -> first row's burst is a baseline (0)
    }
    // Sim-step burst this frame = round(ClockMs delta / SIM_STEP_INTERVAL). See g_ls_prev_clock_ms note.
    long clock_ms    = ms_of(ADDR_GAME_CLOCK);
    long interval_ms = ms_of(ADDR_SIM_STEP_INT());
    if (interval_ms < 1) interval_ms = 100;
    long sim_burst = (g_ls_prev_clock_ms < 0) ? 0
                                              : (clock_ms - g_ls_prev_clock_ms + interval_ms / 2) / interval_ms;
    if (sim_burst < 0) sim_burst = 0; // GAME_CLOCK reset (new match / resync) -> not a burst
    g_ls_prev_clock_ms = clock_ms;
    MH_NetStats s;
    MH_Net_GetStats(&s);
    DWORD now = GetTickCount();
    char  line[340];
    int   n = wsprintfA(line, "%lu %ld %ld %ld %ld %ld %ld %ld %d %d %ld %ld %lu "
                                "%d %d 0x%02x %ld %d %d %d %ld %ld %ld %ld\n",
                        now, clock_ms, ms_of(ADDR_TOTAL_TIME), ms_of(ADDR_LOCAL_HORIZON), ms_of(ADDR_COMMITTED()),
                        ms_of(ADDR_PEER_HORIZON() + 0 * 8), ms_of(ADDR_PEER_HORIZON() + 1 * 8), ms_of(ADDR_STEP_SIZE),
                        *(const int *)ADDR_STALL_COUNT, *(const int *)ADDR_PLAYER_COUNT,
                        s.tx_pkts, s.rx_pkts, s.last_rx_tick ? (unsigned)(now - s.last_rx_tick) : 0u,
                        sess, (int)*(const uint8_t *)ADDR_GAME_MODE, (unsigned)*(const uint8_t *)ADDR_STATUS_FLAGS,
                        ms_of(ADDR_GRACE_TIMER()), *(const int *)ADDR_SYNC_WAIT(), *(const int *)ADDR_SYNC_COUNTDN,
                        *(const int *)ADDR_PLAYERCT_54BC, ms_of(ADDR_SYNC_ACCUM()), sim_burst,
                        g_icon_calls, g_icon_shown);
    DWORD w;
    WriteFile(g_ls_h, line, n, &w, nullptr);
}

// R5 (fork F3D, ruling Q3): the `[net] fix_audit` SAMPLER IS DELETED -- the knob, its two counters,
// fix_audit_tick(), and with them this TU's last three references into the closure's gate-audit
// tallies (mh::lockstep::dispatch_gate_audit / emit_gate_audit / gate_audit).
//
// It went because it had already said, in its own comment, that it could no longer do its job. The
// sampler existed for C3's CROSS-IMPLEMENTATION equivalence run: the byte patch and our reimplemented
// body were two carriers of one fix, and the proof was a PAIR of columns -- `gate patch sent_ev == 0`
// (the patch displaced, never evaluated) alongside `gate ours sent_ev > 0` (our body evaluating it
// instead). C8-e retired the byte patch, which deleted the left half of every pair; what was left
// could only ever show OUR predicate running, and that half alone is not the equivalence test. The
// comment that shipped with it said exactly this ("this line can no longer re-prove the migration").
// A diagnostic that states it cannot make its own argument is a maintenance cost with no return, and
// it was the only thing in net code naming those three closure symbols.
//
// WHAT IS NOT LOST. The migration's evidence is the 2026-07-29 rig run that printed both halves
// (2026-07-29) plus lockstest's `test_w5_sent_gate_audit_tally`, which drives the SENT-side
// tally directly and in both gate states -- an offline oracle, not a sampled log line. The closure
// keeps dispatch_gate_audit()/emit_gate_audit(): they are still incremented by rx_dispatch/tx_emit,
// still read by lockstep_state.cpp's state view, and still asserted by that test. What went is the
// NET-SIDE READER, which is the half D1 is about.
//
// ---- debug-overlay providers (P1) --------------------------------------------------------------
// The `net.*` family. These live HERE rather than in gfx_overlay.cpp because this TU already owns the
// lockstep addresses and the transport stats -- the overlay just holds a name->fn table, so a family is
// a registration, not an edit there. Same values the `ls_log_tick` columns carry, but readable WHILE the
// frame that produced them is on screen, which is the whole point for a stall or a freeze.
//
// All read-only, no allocation; they run inside the present hook every frame.

void ovp_player(char *b, int cap) {
    (void)cap;
    wsprintfA(b, "%d", MH_Net_LocalPlayerId());
}

void ovp_peers(char *b, int cap) {
    (void)cap;
    wsprintfA(b, "%d %s", MH_Net_PeerCount(), MH_Net_IsStarted() ? "up" : "down");
}

void ovp_tx_rx(char *b, int cap) {
    (void)cap;
    MH_NetStats s;
    MH_Net_GetStats(&s);
    wsprintfA(b, "%ld/%ld", s.tx_pkts, s.rx_pkts);
}

// Silence since the last received packet. This is the number that tells a freeze (rx stopped) apart
// from a stall (rx fine, horizon not advancing) -- the two look identical on screen otherwise.
void ovp_since_rx(char *b, int cap) {
    (void)cap;
    MH_NetStats s;
    MH_Net_GetStats(&s);
    DWORD now = GetTickCount();
    wsprintfA(b, "%lu", s.last_rx_tick ? (unsigned long)(now - s.last_rx_tick) : 0ul);
}

void ovp_clock(char *b, int cap) {
    (void)cap;
    wsprintfA(b, "%ld", ms_of(ADDR_GAME_CLOCK));
}

void ovp_total(char *b, int cap) {
    (void)cap;
    wsprintfA(b, "%ld", ms_of(ADDR_TOTAL_TIME));
}

void ovp_horizon(char *b, int cap) {
    (void)cap;
    wsprintfA(b, "%ld", ms_of(ADDR_LOCAL_HORIZON));
}

void ovp_committed(char *b, int cap) {
    (void)cap;
    wsprintfA(b, "%ld", ms_of(ADDR_COMMITTED()));
}

// Headroom: how far the committed horizon is AHEAD of the sim clock. <= 0 means the sim is
// horizon-starved (waiting on a peer) -- the direct readout of the lockstep-latency condition,
// and the fastest way to see which peer is holding the match up.
void ovp_lag(char *b, int cap) {
    (void)cap;
    wsprintfA(b, "%ld", ms_of(ADDR_COMMITTED()) - ms_of(ADDR_GAME_CLOCK));
}

void ovp_peer_ms(char *b, int cap) {
    (void)cap;
    wsprintfA(b, "%ld/%ld", ms_of(ADDR_PEER_HORIZON() + 0 * 8), ms_of(ADDR_PEER_HORIZON() + 1 * 8));
}

void ovp_step_ms(char *b, int cap) {
    (void)cap;
    wsprintfA(b, "%ld", ms_of(ADDR_STEP_SIZE));
}

void ovp_stall(char *b, int cap) {
    (void)cap;
    wsprintfA(b, "%d", *(const int *)ADDR_STALL_COUNT);
}

void ovp_pcount(char *b, int cap) {
    (void)cap;
    wsprintfA(b, "%d", *(const int *)ADDR_PLAYER_COUNT);
}

void ovp_flags(char *b, int cap) {
    (void)cap;
    wsprintfA(b, "0x%02x", (unsigned)*(const uint8_t *)ADDR_STATUS_FLAGS); // 0x80 = awaiting-drop
}

// The peer-timeout / resync-overlay triple: grace accumulator, sync-wait active, retry countdown.
// This is the set that was read out of log files while chasing the U17 drop and the resync freeze.
void ovp_sync(char *b, int cap) {
    (void)cap;
    wsprintfA(b, "g%ld w%d c%d", ms_of(ADDR_GRACE_TIMER()), *(const int *)ADDR_SYNC_WAIT(),
              *(const int *)ADDR_SYNC_COUNTDN);
}

void register_overlay_providers() {
    MH_Overlay_RegisterProvider("net.player", ovp_player);       // our own player id
    MH_Overlay_RegisterProvider("net.peers", ovp_peers);         // connected peers + transport up/down
    MH_Overlay_RegisterProvider("net.tx_rx", ovp_tx_rx);         // packets sent/received
    MH_Overlay_RegisterProvider("net.since_rx", ovp_since_rx);   // ms since the last packet arrived
    MH_Overlay_RegisterProvider("net.clock", ovp_clock);         // GAME_CLOCK ms
    MH_Overlay_RegisterProvider("net.total", ovp_total);         // TOTAL_GAME_TIME ms (clamped to committed)
    MH_Overlay_RegisterProvider("net.horizon", ovp_horizon);     // our requested horizon
    MH_Overlay_RegisterProvider("net.committed", ovp_committed); // min over peers
    MH_Overlay_RegisterProvider("net.lag", ovp_lag);             // committed - clock; <=0 = starved
    MH_Overlay_RegisterProvider("net.peer_ms", ovp_peer_ms);     // peer0/peer1 horizons
    MH_Overlay_RegisterProvider("net.step_ms", ovp_step_ms);     // lockstep lookahead
    MH_Overlay_RegisterProvider("net.stall", ovp_stall);         // stall count
    MH_Overlay_RegisterProvider("net.pcount", ovp_pcount);       // lockstep player count
    MH_Overlay_RegisterProvider("net.flags", ovp_flags);         // lockstep status flags
    MH_Overlay_RegisterProvider("net.sync", ovp_sync);           // grace / sync-wait / retry countdown
}

// (b) Step 2 -- off-frame RX drain. Runs from on_time_tick (main-thread), i.e. AFTER the frame's own pump
// and BEFORE time_tick clamps TOTAL to committed. If the sim is horizon-starved (committed doesn't yet
// allow the next sim step), busy-poll the net pump (order-flush is idempotent once staged==0; send_extend
// fires at most once until GAME_CLOCK advances; dispatch drains RX) + recompute committed, until committed
// rises enough for a step or we time out -- instead of returning to the frame loop and eating a whole
// ~frame for the next pump. This raises committed THIS frame so sim_tick advances now. Single-threaded, so
// no race (committed/peer-horizon are written only main-thread; the heartbeat is TX-only). Determinism-safe:
// committed only GATES how far the sim advances (= min over peers); draining RX sooner lowers latency
// without changing the sim step sequence. Bounded by RX_SPIN_TIMEOUT_MS (a dropped/slow peer falls through
// to the retail stall + the 5 s grace-timeout removal).
constexpr DWORD RX_SPIN_TIMEOUT_MS = 40; // ~2-3 frames; cap the per-frame busy-wait
void            rx_spin_until_horizon() {
    double step;
    memcpy(&step, (const void *)ADDR_SIM_STEP_INT(), sizeof(double));
    if (step <= 0.0) return;
    if (!MH_Net_IsStarted() || MH_Net_PeerCount() <= 0) return; // solo: nothing to wait for
    DWORD t0 = GetTickCount();
    for (;;) {
        double clk, com;
        memcpy(&clk, (const void *)ADDR_GAME_CLOCK, sizeof(double));
        memcpy(&com, (const void *)ADDR_COMMITTED(), sizeof(double));
        if (com >= clk + step) break;           // committed already allows the next sim step
        ((void (*)())ADDR_LOCKSTEP_PUMP)();     // order-flush + send_extend + RX drain (dispatch)
        ((void (*)())ADDR_COMMIT_HORIZON_FN)(); // recompute committed = min(local, peers)
        if ((GetTickCount() - t0) > RX_SPIN_TIMEOUT_MS) break;
        SwitchToThread(); // yield -- don't hard-spin a core
    }
}

// U17: floating "player dropped" HUD notice on the peer that runs the DIRECT removal (our B2 host trigger).
// llm_net_player_remove itself doesn't print; the retail time_tick detector prints right after it, and the
// dispatch case-'\b' RECEIVER already prints on every peer that RECEIVES the frame -- so only the direct
// caller is otherwise silent (the freeze-free host looked like nothing happened). Matches retail: format
// cfg::G_TEXT_PTRS[0xa6] ("Disconnected") + the player name into G_TEXT_TMP, print red.
void notify_player_dropped(int side) {
    int idx = mh::hook::call_watcall1(mh::addr::llm_strat_player_by_side_id, (void *)(intptr_t)side);
    if (idx < 0 || idx >= 8) return;
    const char    *name = (const char *)(mh::addr::_G_LLM_STRAT_PLAYERS + (unsigned)idx * 0x740u + 1812u);
    const wchar_t *txt  = ((const wchar_t *const *)mh::addr::cfg_G_TEXT_PTRS)[0xa6];
    wchar_t        nw[40];
    MultiByteToWideChar(CP_ACP, 0, name, -1, nw, 40);
    wsprintfW((wchar_t *)mh::addr::G_TEXT_TMP, L"%s (%s)", txt ? txt : L"", nw);
    mh::hook::call_watcall1(mh::addr::llm_ui_print_floating_msg_red, (void *)mh::addr::G_TEXT_TMP);
    if (g_ls_log) {
        char a[128], b[176];
        WideCharToMultiByte(CP_ACP, 0, (const wchar_t *)mh::addr::G_TEXT_TMP, -1, a, sizeof(a), nullptr, nullptr);
        wsprintfA(b, "; U17 notify: printed floating msg '%s'\n", a);
        seam_log(b);
    }
}

// ==== Adaptive lookahead controller (P1) ========================================================
// Auto-tunes the lookahead (== input latency) to the lowest value this link sustains at ~1.0x. It
// reuses the STOCK grow/shrink scaffold's shape but NOT its signal: retail scales STEP_SIZE by FPS
// and a stall-count-vs-player-count heuristic, which ratchets to multi-second lag after an alt-tab
// FPS crash -- that is why we pin it off. This drives off MEASURED HORIZON STARVATION instead.
//
// Signal: per frame in live lockstep, "starved" = COMMITTED cannot fund even one more sim sub-step
// (com < clk + sim_step) -- the same clamped condition rx_spin waits on. Over a 2 s window, the
// starved FRACTION is the controller's error term: it is high exactly when the horizon window is
// shorter than the confirmation round-trip, which is the thing the lookahead has to cover.
//
// Why a purely LOCAL controller is safe: COMMITTED = min(local_horizon, peer_horizons), and the sim
// clamps to COMMITTED. So a peer's lookahead only ever gates ITS OWN willingness to run ahead -- the
// effective horizon stays symmetric however the two values differ, and the sim's step sequence is
// unchanged. No agreement protocol, no cross-peer messages, no determinism exposure (contrast
// sim_step_ms, which DOES change the sim and therefore must match). Verified by the per-peer
// independence test in done_when.
//
// Asymmetric by design: grow fast (a stall storm is felt immediately), shrink slowly (latency is a
// comfort win, and oscillating around the floor is worse than sitting slightly above it). The gap
// between the two thresholds is the hysteresis band.
//
// THE FLOOR IS RELATIVE TO sim_step, not absolute (learned the hard way, 2026-07-25). The first
// version floored at a flat 30 ms, taken from the LAN sweep -- but that sweep ran at sim_step=10, so
// 30 ms was really "3 sub-steps of horizon". With the shipping sim_step=20 the same 30 ms floor let
// the controller walk down to ~39 ms = under 2 sub-steps, and the CLIENT FROZE mid-match (~step
// 1165, reproduced twice; clean at a fixed lookahead and clean at sim_step=10, which is what
// isolated it). A peer needs a couple of sub-steps of buffer to run concurrently at all -- below
// that it is permanently at the horizon and one hiccup wedges it. So the effective floor is
// max(configured floor, AD_SIM_FLOOR_MULT x sim_step), which reproduces the measured 30 ms sweet
// spot at sim_step=10 and gives 60 ms at sim_step=20.
constexpr DWORD  AD_WINDOW_MS      = 2000; // decide at most once per window
constexpr int    AD_MIN_FRAMES     = 30;   // ...and only on enough samples to mean anything
constexpr DWORD  AD_MAX_SAMPLE_MS  = 250;  // clamp one frame's contribution (an alt-tab is not starvation)
constexpr double AD_GROW           = 1.25; // starving -> +25% (the FLOOR on a growth step)
constexpr double AD_GROW_MAX       = 2.00; // ...and the ceiling, so one bad window cannot double twice
constexpr double AD_SHRINK         = 0.96; // sustained-clean -> -4%
constexpr int    AD_SHRINK_AFTER   = 3;    // ...and only after this many CONSECUTIVE clean windows
constexpr double AD_STARVE_HI      = 0.06; // >6% of frames starved -> grow
constexpr double AD_STARVE_LO      = 0.01; // <1% -> shrink (between the two: hold)
constexpr double AD_SIM_FLOOR_MULT = 3.0;  // never shrink below this many sim sub-steps of horizon
int              g_adaptive        = 0;    // [net] lockstep_adaptive
double           g_ls_min          = 0.030;
double           g_ls_max          = 0.400; // U35 2026-09-02: 200 saddled on real internet links (the icon storm); LAN settles ~100 and never nears it
double           g_step_eps_ms     = 0.01;  // shared with the fixed-pin path (anti-alias nudge)
DWORD            g_ad_t0           = 0;
int              g_ad_frames = 0, g_ad_starved = 0;
// P1 fix (a), 2026-07-26: starvation is accumulated as TIME, not as a frame count. The old fraction
// starved_frames/frames aliases against the starvation waveform -- it is a transient that opens after
// a sim step and closes when the peer's EXTEND lands, so a fast peer samples inside it repeatedly
// while a slow peer can step straight over it. Measured on ONE link: the dev box read 8-39 starved
// frames per window and grew, while the 60 fps VM read 0-1 and shrank to the floor. Weighting each
// sample by the time it stands for removes the frame-rate dependence.
DWORD g_ad_last_ms = 0, g_ad_time_ms = 0, g_ad_starved_ms = 0;
// P1 fix (c), 2026-07-26: shrink only after SUSTAINED cleanliness. With (a) alone the controller
// climbed correctly to 200 ms and then immediately gave it back -- "0% starved" is the SUCCESS
// condition, and treating one clean window as proof of surplus walks it straight off the value that
// produced the success. Measured: a 184<->200 oscillation spending a starved window on every cycle
// (13.5% deficit against 5.8% for a fixed 200). Growth answers something the player feels now;
// shrinking only buys back input latency, so it can afford to wait for evidence.
int g_ad_clean = 0; // consecutive windows below AD_STARVE_LO

// Keep the value OFF the 10 ms clock grid: an exactly-on-grid lookahead phase-locks with the
// centisecond clock and forfeits an extra quantum most steps (the MP latency notes CORRECTION 3).
double off_grid_ms(double ms) {
    double tenths = ms / 10.0;
    double frac   = tenths - (double)(long)tenths;
    if (frac < 0.001 || frac > 0.999) ms += g_step_eps_ms;
    return ms;
}

void adaptive_tick() {
    if (!g_adaptive) return;
    if (*(const uint8_t *)ADDR_SESSION_MODE != 3) return;       // live lockstep only
    if (!MH_Net_IsStarted() || MH_Net_PeerCount() <= 0) return; // solo: nothing to tune against
    double clk, com, sim;
    memcpy(&clk, (const void *)ADDR_GAME_CLOCK, sizeof(double));
    memcpy(&com, (const void *)ADDR_COMMITTED(), sizeof(double));
    memcpy(&sim, (const void *)ADDR_SIM_STEP_INT(), sizeof(double));
    if (sim <= 0.0) return;
    DWORD now     = GetTickCount();
    bool  starved = (com < clk + sim); // horizon can't fund the next sub-step
    ++g_ad_frames;
    if (starved) ++g_ad_starved; // kept for the log line: frames stay the readable unit
    if (g_ad_last_ms) {          // (a) weight by TIME, so the error term does not depend on frame rate
        DWORD dt = now - g_ad_last_ms;
        if (dt > AD_MAX_SAMPLE_MS) dt = AD_MAX_SAMPLE_MS;
        g_ad_time_ms += dt;
        if (starved) g_ad_starved_ms += dt;
    }
    g_ad_last_ms = now;

    if (g_ad_t0 == 0) g_ad_t0 = now;
    if ((now - g_ad_t0) < AD_WINDOW_MS) return;

    if (g_ad_frames >= AD_MIN_FRAMES && g_ad_time_ms > 0) {
        double frac = (double)g_ad_starved_ms / (double)g_ad_time_ms;
        double cur  = g_lockstep_step;
        if (cur <= 0.0) memcpy(&cur, (const void *)ADDR_STEP_SIZE, sizeof(double));
        // P1 finding (b) -- "committed = min over peers, so the lower peer binds and the higher one
        // sees starvation it cannot fix" -- is REAL but is NOT actionable this way, and the attempt is
        // recorded because it is an easy idea to have twice. Gating growth on "am I the binding peer?"
        // (local_horizon <= committed) suppressed growth ENTIRELY: measured 48.5% deficit with zero
        // controller moves, worse than doing nothing. The reason is that COMMITTED is the minimum over
        // the OTHER peers' horizons as last received, so on a 200 ms link it is ~200 ms stale by
        // construction -- a peer's own live horizon is almost always above it, and the test reads
        // "someone else binds" on every peer at once, exactly on the links that need to grow.
        // Kept as a LOGGED diagnostic (bind=), not a control input. The asymmetry (b) describes is
        // believed to be downstream of (a) anyway: once both peers measure starvation in TIME they
        // agree about the link, so they climb together instead of one running away -- which is what
        // the numbers below have to confirm.
        double local_h = 0.0;
        memcpy(&local_h, (const void *)ADDR_LOCAL_HORIZON, sizeof(double));
        bool   we_bind = (local_h <= com + sim * 0.5);
        double want    = cur;
        if (frac > AD_STARVE_HI) {
            // P1 fix (d), 2026-07-26: grow PROPORTIONALLY to how starved we are, not by a flat +25%.
            // A fixed step ignores the size of the error, so the climb from the shipping 100 ms to the
            // 200 ms a 200 ms link needs took four 2 s windows -- and the peer is heavily starved for
            // all eight seconds of it (measured 54/37/40/21%). On a short match that ramp IS the whole
            // deficit. Scaling by the measured starved fraction reaches the same place in about half
            // the windows, while a lightly-starved link still gets the gentle old step because AD_GROW
            // is the floor. Capped so a single pathological window cannot overshoot the band wildly.
            double g = 1.0 + frac;
            if (g < AD_GROW) g = AD_GROW;
            if (g > AD_GROW_MAX) g = AD_GROW_MAX;
            want       = cur * g;
            g_ad_clean = 0; // any starvation resets the patience counter
        } else if (frac < AD_STARVE_LO) {
            if (++g_ad_clean >= AD_SHRINK_AFTER) want = cur * AD_SHRINK;
        } else {
            g_ad_clean = 0; // in the hysteresis band: hold, and accrue no credit toward shrinking
        }
        double floor_s = g_ls_min;
        if (AD_SIM_FLOOR_MULT * sim > floor_s) floor_s = AD_SIM_FLOOR_MULT * sim; // sim-relative floor
        if (want < floor_s) want = floor_s;
        if (want > g_ls_max) want = g_ls_max;
        double want_ms = off_grid_ms(want * 1000.0);
        want           = want_ms / 1000.0;
        if (want != cur) {
            g_lockstep_step = want; // on_time_tick pins it into STEP_SIZE from here on
            if (g_ls_log) {
                char b[160];
                wsprintfA(b, "; adaptive: lookahead %ld -> %ld ms (starved %ld/%ld ms = %d%%, %d/%d frames, bind=%d)\n",
                          (long)(cur * 1000.0), (long)want_ms, (long)g_ad_starved_ms,
                          (long)g_ad_time_ms, (int)(frac * 100.0), g_ad_starved, g_ad_frames,
                          we_bind ? 1 : 0);
                seam_log(b);
            }
        }
    }
    g_ad_t0         = now;
    g_ad_frames     = 0;
    g_ad_starved    = 0;
    g_ad_time_ms    = 0;
    g_ad_starved_ms = 0;
}

void on_time_tick() {
    adaptive_tick(); // may move g_lockstep_step; the pin below applies it the same frame
    if (g_lockstep_step > 0.0) memcpy((void *)ADDR_STEP_SIZE, &g_lockstep_step, sizeof(double));
    if (g_game_speed > 0.0) memcpy((void *)ADDR_GAME_SPEED, &g_game_speed, sizeof(double)); // pin before time_tick reads it
    if (g_sim_step > 0.0) memcpy((void *)ADDR_SIM_STEP_INT(), &g_sim_step, sizeof(double)); // pin sub-step granularity (both time_tick's arm-gate + sim_tick's loop read it)
    if (g_rx_spin && *(const uint8_t *)ADDR_SESSION_MODE == 3) rx_spin_until_horizon();     // (b) Step 2: drain RX off-frame during a stall
    if (g_graceful_drop && *(const uint8_t *)ADDR_SESSION_MODE == 3) {
        // U17 (b) fast hard-drop: a client's transport died -> broadcast removal. Guards: dead>=0
        // excludes a client's host-conn (-1) so this is host-only (client-death only); the roster
        // check skips a side already removed by another path.
        //
        // D5 FIX (2026-07-27) -- WAIT FOR THE PARKED PRECONDITION BEFORE FIRING.
        // The old comment here claimed this was the "same fn + ordered stream as the retail late
        // trigger". Same fn, same subtype, same receiver -- but the ordering NEVER came from the fn or
        // the stream. llm_net_player_remove is applied local-immediately and on arrival at the
        // receiver; retail is safe only because BOTH its triggers fire from inside llm_strat_time_tick's
        // PARKED branch, i.e. once a silent peer's frozen PEER_HORIZON has clamped every survivor's
        // TOTAL_GAME_TIME to the same value H_d. Firing on transport-death skipped that, so the host
        // applied the flip at T_a < H_d while receivers applied it at T_a + latency.
        // MEASURED before this fix: gap = -101 ms and -70 ms at ship pacing (parked at the rig's 30 ms
        // pin, which is why the rig defaults hid it), and a 3-peer kill run desynced the survivor for
        // 1-3 steps in `strat_players`. See D4/D5/D9.
        // So: LATCH the dead peer here, and release it only once TOTAL >= COMMITTED. That restores the
        // retail barrier exactly while still skipping the ~60-count silence wait U17(b) existed to avoid.
        if (g_pending_dead < 0) {
            int dead = MH_Net_TakeDeadPeer();
            if (dead >= 0 &&
                mh::hook::call_watcall1(mh::addr::llm_strat_player_by_side_id, (void *)(intptr_t)dead) != -1) {
                g_pending_dead      = dead;
                g_pending_dead_wait = 0;
            }
        }
        if (g_pending_dead >= 0) {
            const double total     = *(const double *)ADDR_TOTAL_TIME;
            const double committed = *(const double *)ADDR_COMMITTED();
            // SAFETY VALVE: if the parked state never arrives the peer must still be removed, or a
            // wrong model here would hang the session instead of desyncing it. Fire anyway after
            // g_pending_dead_max frames and say so loudly -- a silent fallback would hide the fact
            // that the barrier assumption failed.
            const bool parked  = (total >= committed);
            const bool timeout = (++g_pending_dead_wait > g_pending_dead_max);
            if (parked || timeout) {
                const int dead = g_pending_dead;
                g_pending_dead = -1;
                mh::hook::call_watcall1(mh::addr::llm_net_player_remove, (void *)(intptr_t)dead);
                notify_player_dropped(dead); // host-side "player dropped" HUD notice
                if (g_ls_log) {
                    char b[192];
                    wsprintfA(b,
                              "; U17 fast-drop: transport-dead peer side=%d -> broadcast removal"
                              " (%s after %d frames)\n",
                              dead, parked ? "PARKED" : "TIMEOUT-UNPARKED", g_pending_dead_wait);
                    seam_log(b);
                }
            }
        }
    }
    temporal_capture(TEV_TIMETICK); // time_tick ENTRY (pre this frame's accumulate+clamp)
    ls_log_tick();
}
// C4 REBIND TARGET. Null = fall through to the original (the stolen-prologue trampoline), which is the
// shipped default and byte-for-byte today's behaviour. Non-null = fall through to mh::lockstep's
// promoted body instead. ONE OWNER PER ENTRY: this detour owns ADDR_TIME_TICK and keeps owning it under
// promotion; only its destination changes, so on_time_tick's pacing / adaptive / rx_spin / graceful-drop
// work runs ahead of OUR body exactly as it runs ahead of the original.
void *g_tt_promoted = nullptr;

__declspec(naked) void time_tick_detour() {
    __asm {
        pushad
        pushfd
        call on_time_tick
        popfd
        popad
                            // The CMP clobbers EFLAGS after the POPFD restored them. Safe here and only here: this is a
                            // FUNCTION ENTRY, and neither llm_strat_time_tick's Watcom prologue nor our entry thunk reads
                            // incoming flags -- an entry takes its arguments in registers and on the stack, never in the
                            // status word. Do not copy this pattern to a mid-function splice, where it would not hold.
        cmp  dword ptr [g_tt_promoted], 0
        jne  promoted
        jmp  dword ptr [g_tt_tramp] // stolen 8-byte prologue + jmp back to time_tick+8
    promoted:
        jmp  dword ptr [g_tt_promoted] // the generated entry thunk -> mh::lockstep::time_tick
    }
}

// LT1F (2026-09-02): the C4 chain hook for OUR frame spine. mh::sim's translated llm_strat_frame
// pair calls our time_tick body DIRECTLY (the routing decision -- sim_lt_frame.h), which skips the
// entry detour above, so the frame bodies chain this instead: on_time_tick, ARMED-GATED, so a run
// whose config never armed the pacing detour behaves identically through either path. TU-local
// (on_time_tick is anonymous-namespace); handed to the frame unit as a pointer at install time.
void lt_frame_pace_time_tick() {
    if (g_tt_tramp) on_time_tick();
}

// LT1F spine interlock inputs: did the two spine promotions ACTUALLY land this run? Set by the
// two installers below on every success route (rebind or direct). The frame promotion refuses
// without both -- see install_promotion_lt_frame's banner.
bool g_tt_promoted_ok       = false;
bool g_sim_tick_promoted_ok = false;

// Run-before the game-over/outcome dialog: leave lockstep so the LOSER shows its result immediately
// (see the block comment at g_sync_gameover). SESSION_MODE is a dword; 3 = lockstep, 2 = MP-local.
// The downgrade is idempotent, which is why the shape below can run it unconditionally: SESSION is
// 3 or it is not. The two-argument signature is U33's participant shape, kept because the naked
// thunk pushes the same two words (both zero -- see gameover_detour).
void on_gameover(uint32_t outcome, uint32_t) {
    if (g_ls_log) {
        char b[160];
        wsprintfA(b, "; on_gameover ENTER sess=%d outcome=%u gclk=%ld (downgrade=%d)\n",
                  (int)*(const uint8_t *)ADDR_SESSION_MODE, outcome, ms_of(ADDR_GAME_CLOCK),
                  (int)(*(volatile uint32_t *)ADDR_SESSION_MODE == 3));
        seam_log(b);
    }
    if (*(volatile uint32_t *)ADDR_SESSION_MODE == 3)
        *(volatile uint32_t *)ADDR_SESSION_MODE = 2; // SESSION_MP_LOCKSTEP -> SESSION_MP_LOCAL
}
// The pre-U33 naked shape, restored by F1C as the no-gate fallback and the only host since fork
// F2F. The outcome argument is not recoverable here, so on_gameover logs it as 0.
__declspec(naked) void gameover_detour() {
    __asm {
        pushad
        pushfd
        push 0 // a1 (unused)
        push 0 // outcome: not recoverable in the naked shape
        call on_gameover
        add  esp, 8
        popfd
        popad
        jmp  dword ptr [g_go_tramp] // stolen prologue + back to llm_ui_outcome_dialog+8
    }
}

// U17 (a) run-before llm_game_return_to_main_menu_cb: if we are quitting a RUNNING lockstep game,
// broadcast our own CLEAN removal (subtype 8) with our side_id. llm_net_player_remove flushes the wire
// frame via llm_net_transport_send BEFORE it mutates local state, so it reaches survivors while the
// transport is still up; they apply it in dispatch order (determinism-safe) and resume immediately. The
// retail teardown body then runs and returns us to the menu. No leader-gate: the quitter authoritatively
// self-removes (exactly one sender). SESSION_MODE byte: 3 = SESSION_MP_LOCKSTEP.
void on_quit_to_menu() {
    if (!g_graceful_leave) return;
    if (*(const uint8_t *)ADDR_SESSION_MODE != 3) return; // not in a running lockstep game
    int side = *(const int32_t *)mh::addr::_G_LLM_NET_LOCAL_PLAYER_INDEX;
    mh::hook::call_watcall1(mh::addr::llm_net_player_remove, (void *)(intptr_t)side); // EAX = side_id
    if (g_ls_log) {
        char b[96];
        wsprintfA(b, "; U17 graceful-leave: broadcast self-removal side=%d before quit-to-menu\n", side);
        seam_log(b);
    }
}
__declspec(naked) void quit_to_menu_detour() {
    __asm {
        pushad
        pushfd
        call on_quit_to_menu
        popfd
        popad
        jmp  dword ptr [g_quit_tramp] // stolen 8-byte prologue + jmp back to llm_game_return_to_main_menu_cb+8
    }
}

// Fixed-cadence horizon advertiser (see the block comment at g_hb_run). Sleeps until the live mode-3
// lockstep sim is running with a peer, then broadcasts the game's own EXTEND packet every g_hb_ms.
DWORD WINAPI horizon_heartbeat_thread(LPVOID) {
    for (;;) {
        Sleep(g_hb_ms > 0 ? g_hb_ms : 50);
        if (!InterlockedCompareExchange(&g_hb_run, 1, 1)) break; // signalled to stop
        {
            uint8_t sess = *(const uint8_t *)ADDR_SESSION_MODE;
            DWORD   now  = GetTickCount();
            if (sess == 3) g_last_ls_tick = now; // live lockstep sim
            else if (g_last_ls_tick == 0 || now - g_last_ls_tick > HB_ENDGAME_GRACE_MS)
                continue; // not lockstep + past the endgame grace
        }
        if (!MH_Net_IsStarted() || MH_Net_PeerCount() <= 0) continue; // nobody to advertise to
        // horizon = GAME_CLOCK + STEP_SIZE, exactly as the game's advertise path computes it.
        // GAME_CLOCK (0x005d0198) is 8-byte aligned -> atomic on x86. STEP_SIZE (0x005d55bc) is only
        // 4-aligned, so prefer our pinned copy g_lockstep_step (a DLL global, no torn read) when
        // lockstep_step_ms is set -- which the MP builds always do; fall back to the game global.
        double clock;
        memcpy(&clock, (const void *)ADDR_GAME_CLOCK, sizeof(double));
        double step;
        if (g_lockstep_step > 0.0) step = g_lockstep_step;
        else memcpy(&step, (const void *)ADDR_STEP_SIZE, sizeof(double));
        double horizon = clock + step;
        // Keep OUR requested horizon fresh too, so a stalled local render doesn't cap our own COMMITTED
        // = min(HORIZON, peers) (recomputed on the recv thread) -- this decouples our sim as well, not
        // just the peer's. HORIZON (0x005d5594) is 4-aligned; a torn read by the main thread is
        // astronomically rare and only delays local timing by one commit cycle (self-correcting on the
        // next beat) -- it never changes sim CONTENT (determinism holds).
        memcpy((void *)ADDR_LOCAL_HORIZON, &horizon, sizeof(double));
        unsigned char pkt[9];
        pkt[0] = 2; // lockstep packet type 2 = EXTEND (horizon advert)
        memcpy(pkt + 1, &horizon, sizeof(double));
        MH_Net_Send(MH_NET_BROADCAST, pkt, 9); // MH_Net_Send is g_conn_cs-locked (thread-safe)
    }
    return 0;
}

// Overlay-fire ROOT-CAUSE fix (2026-07-24 investigation) -- the alternative to the wait-overlay NOP in the
// defang. WHY the overlay fires spuriously: the "waiting for player" modal (wait_player_overlay_show
// @0x004c7dc0) is called UNGATED on the 2nd consecutive at-horizon frame in llm_strat_time_tick
// (call site = ADDR_OVL_WAIT_CALL). But healthy 1-RTT lockstep sits AT the committed horizon almost every
// frame: time_tick clamps TOTAL down to COMMITTED on each overshoot, and COMMITTED only advances when a peer
// EXTEND packet lands (~100 ms net cadence) while frames render ~16 ms -> ~5/6 frames are at-horizon -> the
// modal flickers mode 2<->3 at net cadence, and defang-off that flicker collapses lockstep in ~2 s. Its
// SIBLING sync_overlay_show is already gated on SYNC_RETRY_COUNTDOWN<0x38 (genuine multi-second silence);
// the wait-overlay call is the un-gated one. FIX: redirect the wait-overlay CALL through this thunk, which
// applies the SAME real-timeout gate, so the modal shows ONLY on genuine silence. Strictly REDUCES when
// mode-3 is entered (never on a healthy frame) and leaves the real peer-timeout/drop path (downstream of the
// genuine countdown) untouched -> determinism-safe, and it fixes the trigger at the source instead of
// suppressing the effect (candidate for retiring the wait-overlay half of the defang).
// P4 folded in: the thunk now also COUNTS, and is installed even when no gating is wanted (g_icon_gate
// == 0 -> pure pass-through + a counter). That is why the counter is here rather than in a second
// patch: one interception point, and "wanted" vs "drawn" fall out of the same place.
// Register discipline: EAX carries the arg and is untouched; EDX is saved around the countdown read;
// only EFLAGS are clobbered, which the original gate thunk already did across the same call.
__declspec(naked) void wait_overlay_gate_thunk() {
    __asm {
        inc  dword ptr [g_icon_calls] // the game WANTED the icon this frame
        cmp  dword ptr [g_icon_gate], 0
        je   forward // no gate -> pass everything through (counting only)
        push edx
        mov  edx, 0x00e58789 // &_G_LLM_NET_LOCKSTEP_SYNC_RETRY_COUNTDOWN (starts 0x3c, drains on real non-advance)
        cmp  dword ptr [edx], 0x38
        pop  edx // restore (pop leaves EFLAGS from cmp intact)
        jge  suppressed // countdown >= 0x38 => routine at-horizon wait -> skip
    forward:
        inc  dword ptr [g_icon_shown] // ...and one was actually drawn
        push 0x004c7dc0 // tail-jump to the real wait_player_overlay_show (EAX = player_idx arg preserved)
        ret
    suppressed:
        ret // return past the call site
    }
}
void install_overlay_gate() {
    // Its target is inside llm_strat_time_tick, promotable since C4 -- so "displaced" is a THIRD outcome
    // here and must not be reported as a byte MISMATCH. Without this the log said "unexpected bytes at
    // wait-overlay call site" on every promoted run, which reads as "the DLL is running against the
    // wrong mh.exe build" -- a much scarier and entirely false diagnosis. Same guard, same reason, as
    // install_resync_trigger_reset.
    //
    // WHAT IT USED TO COST, AND NO LONGER DOES (U20, 2026-08-30). This branch used to end
    // "...the P4 icon counters are UNAVAILABLE in this run, not zero", because the thunk WAS the only
    // writer of icon_calls/icon_shown -- so a promoted run, i.e. every shipped run, had no icon
    // measurement and the columns read a zero that meant "nobody counted". That sentence is now
    // FALSE, and leaving it would be worse than the original absence: the next reader would take a
    // real measurement for one. Our promoted body feeds the same two longs through
    // mh::lockstep::set_icon_counters (see install_overlay_patches), so the columns are LIVE here --
    // and better, since they count per sim step rather than per displaced call site.
    if (mh::hook::promoted_owner_of(ADDR_OVL_WAIT_CALL)) {
        seam_log("; overlay gate DISPLACED: time_tick is promoted, so the wait-overlay call site never "
                 "executes -- but the icon counters (icon_calls/icon_shown) ARE LIVE, fed by our body "
                 "via set_icon_counters. A zero in those columns means no icons, not no measurement.\n");
        return;
    }
    uint8_t repl[5];
    repl[0]     = 0xE8; // CALL rel32 -> our gate thunk (replaces the CALL to wait_player_overlay_show)
    int32_t rel = (int32_t)((uintptr_t)&wait_overlay_gate_thunk - (ADDR_OVL_WAIT_CALL + 5));
    memcpy(repl + 1, &rel, sizeof(rel));
    bool ok = patch_bytes_guarded(ADDR_OVL_WAIT_CALL, OVL_WAIT_EXPECT, repl, 5);
    if (!ok)
        seam_log("; overlay gate NOT armed (unexpected bytes at wait-overlay call site)\n");
    else if (g_icon_gate)
        seam_log("; overlay gate armed: wait-overlay call -> SYNC_RETRY_COUNTDOWN<0x38 gate (root-cause fix) + P4 counters\n");
    else
        seam_log("; P4 icon counters armed: wait-overlay call COUNTED, not gated (pass-through)\n");
}

// Resync-wait hang fix (defang dep #2 -- the REAL root cause, live-dump-confirmed 2026-07-24). The go-live
// collapse is NOT a message deadlock (the earlier waitframe_pump theory was a red herring, reverted): a
// frozen-client full-memory dump showed the main thread BUSY-SPINNING in llm_net_lockstep_sync_busywait
// (@0x0049e6f5), a hard `do{}while(GetTickCount < now+delay)` whose `delay` comes from
// llm_net_lockstep_sync_delay_stub (EN @0x0049c02c) -- a dead/cut-MP stub that RETURNS UNINITIALIZED STACK
// (`mov eax,[ebp-0x28]`, never written). The garbage delay is racy: small -> exits fast (survives), huge
// (~6.1M ms in the 2026-07-10 dump) -> spins ~forever (the ~2/3 go-live hang). It is reached via the resync
// path (dispatch processes the type-4/0xd resync record -> sync_busywait); defang-off amplifies it (overlay
// freezes -> stalls -> the leader-stall resync trigger fires). This is the SAME known hang as mh_hang.dmp
// (2026-07-10); the static fix src/patcher/net_resync_wait_fix_EN.mh.patch.json exists but was NEVER applied
// to the deployed EN exes (all read 8B 45 D8). FIX: patch the stub's return `mov eax,[ebp-0x28]` (8B 45 D8 @
// 0x0049c044) -> `xor eax,eax; nop` (31 C0 90) so it returns 0 -> the busy-wait deadline is now+0 -> instant.
// Safe: the stub never returned a meaningful value (the sync-wait was always broken; retail never reached
// it), the resync's REAL recovery work (rebaseline timestamp, re-advertise, broadcast, time_resync_and_tick)
// still runs -- only the broken garbage-length spin is neutralized; the '<0' error gate in
// session_begin_multi (0x0045450e) sees a non-error 0. Default ON (correctness fix); knob for A/B.
constexpr uintptr_t ADDR_RESYNC_STUB_RET  = 0x0049c044;         // llm_net_lockstep_sync_delay_stub: the `mov eax,[ebp-0x28]` return
const uint8_t       RESYNC_STUB_EXPECT[3] = {0x8B, 0x45, 0xD8}; // mov eax,[ebp-0x28]  (returns uninitialized stack)
const uint8_t       RESYNC_STUB_PATCH[3]  = {0x31, 0xC0, 0x90}; // xor eax,eax; nop    (return 0 -> instant wait)
void                install_resync_wait_fix() {
    // DISPLACED is a THIRD outcome, and it must not be reported as a byte mismatch. Since C8-c
    // promotes llm_net_lockstep_sync_delay_stub, patch_bytes_guarded returns false in a promoted run
    // because the C1 interlock SUPPRESSED the write -- not because the bytes were wrong. Without this
    // branch the log would read "NOT armed (unexpected bytes -- already patched?)", which is false in
    // both of its clauses and points a reader at a byte-level investigation that has nothing to find.
    // Same shape and same reason as install_resync_trigger_reset below.
    //
    // WHAT CARRIES THE FIX THEN: our body returns 0 UNCONDITIONALLY (resync.h argues why reproducing
    // the original's uninitialised-stack read is not an option -- it is the hang). So under promotion
    // this knob has no off switch: `[net] resync_wait_fix=0` does NOT restore stock behaviour, and the
    // line below says so rather than letting an A/B run silently compare a config against itself.
    // The rollback arm for that experiment is `[promote] lockstep=0`, which is the diagnostic
    // configuration the seam-class taxonomy §4c R2 reserves for exactly this.
    if (mh::hook::promoted_owner_of(ADDR_RESYNC_STUB_RET)) {
        seam_log("; resync-wait fix DISPLACED: sync_delay_stub is promoted, and our body carries the fix "
                                               "UNCONDITIONALLY (returns 0). [net] resync_wait_fix=0 cannot switch it off in this run -- "
                                               "use [promote] lockstep=0 to get the original back.\n");
        return;
    }
    bool ok = patch_bytes_guarded(ADDR_RESYNC_STUB_RET, RESYNC_STUB_EXPECT, RESYNC_STUB_PATCH, 3);
    seam_log(ok ? "; resync-wait fix armed: sync_delay_stub -> return 0 (kills the busy-wait garbage-spin hang)\n"
                               : "; resync-wait fix NOT armed (unexpected bytes at sync_delay_stub return -- already patched?)\n");
}

// Spurious-resync ROOT fix (option b, 2026-07-24 delegated dig). `RESYNC_TRIGGER_COUNT` (0x00e58791) is a
// cumulative leader-local stall-nag counter: ++ per type-3 ACK sent/received, reset ONLY in force_resync +
// at match start -- but NOT on horizon recovery, unlike its per-episode siblings (SYNC_WAIT_ACTIVE,
// SYNC_RETRY_COUNTDOWN, SYNC_WAIT_ELAPSED, PEER_TIMEOUT_ELAPSED, all reset in time_tick's recovery branch
// @0x43f2a5). At a tight lookahead "at-horizon" is the STEADY state, so stall-nags stream as normal play and
// the cumulative counter ratchets to ACTIVE_PLAYERS*100 -> force_resync force-fires a SPURIOUS mode-8 resync
// (~2 s freeze) ~every 2.6 s. FIX: also zero the counter in the recovery branch, so it counts CONSECUTIVE
// (not cumulative) stalls -- a genuinely stuck peer never reaches recovery, so its count still climbs and a
// REAL resync still fires. Determinism-safe: the counter is leader-local scratch gating only WHEN the leader
// emits its (already-synchronized) resync broadcast -- it feeds no per-peer sim computation. This is the
// root-cause complement to defang_xui (which suppresses the mode-8 modal); with this on, resyncs become rare
// so the modal only shows on genuine desync. Mechanism: call-splice the first recovery store
// `mov [SYNC_WAIT_ACTIVE],0` (@0x43f2a5, 10 bytes) with CALL rel32 + 5 NOP -> a thunk that REPLAYS that store
// AND zeroes RESYNC_TRIGGER_COUNT, then returns (both absolute-mem stores, no rel-target recompute).
constexpr uintptr_t    ADDR_RESYNC_RECOVERY_SPLICE = 0x0043f2a5;
const uint8_t          RESYNC_RECOVERY_EXPECT[10]  = {0xC7, 0x05, 0xA9, 0x87, 0xE5, 0x00, 0x00, 0x00, 0x00, 0x00};
__declspec(naked) void resync_trigger_reset_thunk() {
    __asm {
        push eax // preserve (EAX is reloaded fresh at 0x43f2fb, but be safe); MOV/PUSH/POP leave flags intact
        mov  eax, 0x00e587a9
        mov  dword ptr [eax], 0 // displaced store: SYNC_WAIT_ACTIVE = 0 (what we spliced out)
        mov  eax, 0x00e58791
        mov  dword ptr [eax], 0 // NEW: RESYNC_TRIGGER_COUNT = 0 on recovery
        pop  eax
        ret // return to 0x43f2af (the next recovery store)
    }
}
// ==================================================================================================
// C8 (ii): REFUSE LOUDLY when a knob's only carrier is gone.
//
// The user's call, 2026-07-29. C8 retires the byte patches whose behaviour our reimplemented bodies
// now carry -- but `[promote]` stays DEFAULT-OFF, because the one-flag rollback is what every
// promotion run leans on. That combination has a trap in it: once the byte patch is deleted, a knob
// like `[net] resync_trigger_gate=1` reaches ONLY our body, so in an UNPROMOTED run it would set a
// flag that nothing reads and silently do NOTHING.
//
// A knob that quietly stops working is the exact failure family this project keeps rediscovering --
// the `--steps` knob that never reached the DLL, the shadowed ini section, the seam subset that
// selected nothing. So the deletion comes with this: if the knob is SET and the function that would
// carry it is NOT promoted, say so by name, loudly, in the run's own log. Option (i) (flip promotion
// default-on) and option (iii) (keep the patch) were both considered and rejected; see C8's ledger
// entry for why (ii) is the reversible one.
//
// The test is `promoted_owner_of(site)`: it answers non-null exactly when the address lies inside a
// body that IS promoted this run -- which is precisely "our code is live here, so the flag will be
// read". It is the same question C1's interlock asks before writing a patch byte, asked from the
// other side.
void refuse_uncarried_fix(const char *knob, uintptr_t site, const char *owner) {
    if (mh::hook::promoted_owner_of(site)) return; // our promoted body carries it -- nothing to say
    char b[300];
    wsprintfA(b,
              "; [net] UNCARRIED FIX: %s is set, but its byte-patch carrier at %08X was RETIRED (C8) "
              "and its owner %s is NOT promoted this run -- so THIS RUN DOES NOT HAVE THAT FIX. "
              "Enable the matching [promote] key, or accept stock behaviour knowingly.\n",
              knob, (unsigned int)site, owner);
    seam_log(b);
}

void install_resync_trigger_reset() {
    uint8_t repl[10];
    repl[0]     = 0xE8; // CALL rel32 -> thunk
    int32_t rel = (int32_t)((uintptr_t)&resync_trigger_reset_thunk - (ADDR_RESYNC_RECOVERY_SPLICE + 5));
    memcpy(repl + 1, &rel, sizeof(rel));
    for (int k = 5; k < 10; k++) repl[k] = 0x90; // NOP-pad the remainder of the displaced 10-byte store
    // Its target is inside llm_strat_time_tick, promotable since C4 -- so "displaced" is a third
    // outcome here and must not be reported as a byte MISMATCH.
    if (mh::hook::promoted_owner_of(ADDR_RESYNC_RECOVERY_SPLICE)) {
        seam_log("; resync-trigger reset DISPLACED: time_tick is promoted, so this fix must come from "
                 "our body (see the [interlock] line naming its carrier)\n");
        return;
    }
    bool ok = patch_bytes_guarded(ADDR_RESYNC_RECOVERY_SPLICE, RESYNC_RECOVERY_EXPECT, repl, 10);
    seam_log(ok ? "; resync-trigger reset armed: RESYNC_TRIGGER_COUNT zeroed on recovery (spurious-resync root fix)\n"
                : "; resync-trigger reset NOT armed (unexpected bytes at recovery-branch splice @0x43f2a5)\n");
}

// Spurious-resync fix, part c (increment GATE) -- complement to the recovery-reset above. Both leader-only
// RESYNC_TRIGGER_COUNT increment sites -- SENT nag @0x49d8cb (send_lockstep_ack) and RECEIVED nag @0x49c508
// (dispatch case '\x02') -- `inc dword[0xe58791]` unconditionally. The recovery-reset (b) only clears the
// LEADER's OWN-recovery accumulation; the RECEIVED-nag path keeps climbing from the peer's independent
// stall-nags between recoveries (measured: b alone only halves the spurious resyncs). FIX c: gate BOTH
// increments on SYNC_RETRY_COUNTDOWN(0xe58789) < 0x38 -- the SAME "genuine multi-second silence" predicate
// sync_overlay_show uses -- so routine at-horizon nags (countdown near its 0x3c reset) DON'T count; only a
// real sustained stall (countdown drained past 0x38) does. Determinism-safe (leader-local scratch, gates only
// the leader's synchronized resync-broadcast decision). Mechanism: call-splice each 6-byte INC with CALL rel32
// + NOP -> a shared thunk that conditionally re-does the INC. b+c together should drive spurious resyncs ~0.
// C8-e (2026-07-30): THE BYTE PATCH IS RETIRED. Its two naked thunks, the shared expected-bytes
// array, the CALL rel32 + NOP splice and install_resync_trigger_gate() were HERE and are gone --
// archived, with the restore procedure, in the retired-patch log.
//
// THIS IS A RETIREMENT, NOT A SCOPE DECISION, and the difference is visible in what survives:
// `[net] resync_trigger_gate` IS STILL A KNOB and still means exactly what it meant. Only its
// CARRIAGE moved -- our own bodies evaluate the predicate now (rx_dispatch.cpp for the RECV site,
// tx_emit.cpp for the SENT one, both migrated in C3/W5 and rig-proven). What the run owes an ini
// that sets the knob is therefore a `refuse_uncarried_fix` line when our bodies are NOT live --
// see lockstep_install_core -- which is the opposite of the three defangs above, where the
// capability itself was dropped and there is nothing left to carry.
//
// THE PATCH-SIDE HALF OF THE fix_audit COMPARISON WENT WITH IT. `gate patch sent_ev/ok` and
// `recv_ev/ok` were incremented by those thunks; with no thunk they could only ever read 0, and
// a column that can only read one value is not evidence, it is a gate that cannot go red. The
// four counters and their four columns are removed rather than left reading 0 forever.

// MP D14 -- "the resync-begin synthetic order lands on a DIFFERENT STEP on each peer". A STOCK bug,
// found 2026-07-28 while attributing L1-P's asymmetric-promotion red and measured at 3-in-4 runs with
// BOTH peers running the stock turn engine, i.e. it is not the reimplementation's. The two fixes above
// make the resync RARER (the trigger gate is on by default and cut it to ~1 per 3000-step run); this
// one makes the resync that does fire land on the same step everywhere.
//
// MECHANISM, and note it is one step further upstream than "each peer invents the time": the exec_time
// is REPLICATED. llm_net_lockstep_force_resync (0x0049d9d6) calls llm_net_lockstep_broadcast_resync_state
// (0x0049d8ef) with the LITERAL double 2.0 -- `PUSH 0x40000000 / PUSH 0x0` @0x0049da0b. That callee uses
// the value TWICE: it copies it into the CTL_RESYNC_BEGIN wire message (control tag 13, @0x0049d938) and
// it builds the 0x44 synthetic order record -- exec_time at +0x00, kind 0xf0 at +0x0a, 13 at +0x0c and
// +0x0e -- which it hands to llm_strat_order_pending_enqueue (0x00466790) DIRECTLY @0x0049d9c5. The
// receiving peer's dispatch does the same with the double it took off the wire.
//
// So BOTH the local enqueue and the remote one bypass llm_strat_order_schedule (0x00466348) -- and that
// is the entire defect, because order_schedule is precisely the function that reconciles a replicated
// order's exec_time with the lockstep horizon (`exec_time < HORIZON` -> clamp the record UP to HORIZON,
// @0x0046637e) BEFORE it both enqueues and broadcasts. 2.0 is permanently in the PAST, so release_due
// fires the order the instant each peer's own dispatch drains it -- a LOCAL, arrival-timing decision --
// and the peers do not agree. MEASURED over 8 archived runs (4 per arm, decoded by tools/mp_order_diff.py):
// 6 skewed by 1-2 steps and 2 agreed, and the skew predicts the run's verdict in 8 of 8. The blast radius
// is SMALL and exact -- the order sits in order_queue for ONE step, so precisely 2 steps of the recorded
// order stream differ, after which the peers reconverge.
//
// FIX = apply exactly order_schedule's clamp, at the one choke point both paths pass through: the
// exec_time argument of broadcast_resync_state, clamped to max(exec_time, LOCKSTEP_HORIZON).
//
// WHY CLAMPING TO OUR OWN HORIZON IS SAFE is not a new argument -- it is the SAME clamp against the SAME
// global that every ordinary replicated order in the game already receives. If clamping to the local
// horizon could land an order behind a peer's clock, every order in the game would desync; they do not.
//
// MIXED-VERSION NOTE: the fix is entirely LEADER-side, and the clamped value is what goes on the wire, so
// a peer running an unpatched DLL enqueues the same corrected time. Only the leader's build matters.
//
// Mechanism = an 8-byte prologue trampoline rather than a byte splice, because the clamp is a double
// compare and the caller has only 7 bytes to splice (the two PUSHes) -- not enough for two `PUSH m32`.
// PROLOGUE-guarded like every other trampoline here, so a wrong build is a logged no-op.
constexpr uintptr_t ADDR_BROADCAST_RESYNC_STATE = 0x0049d8ef;
void               *g_brs_tramp                 = nullptr;

// `RET 0x8` with the double pushed by the caller == __stdcall(double). mh_calls.gen.h reaches the same
// function through its stack-args-callee-cleans wrapper, so the two agree.
void __stdcall broadcast_resync_state_detour(double exec_time) {
    const double horizon = *(const double *)mh::addr::_G_LLM_STRAT_LOCKSTEP_HORIZON;
    // The clamp itself is a pure function so lockstest can drive its edge cases without a rig -- see
    // the note at its definition for why the rig cannot reach them. R4 (fork F3D): it lives in
    // fix/resync_clamp.h, NOT in the closure. This detour is the carrier that runs when the ORIGINAL
    // body is live, so naming `mh::lockstep::detail::` here made a pure double comparison into a
    // config-(1) reference from net code into the reimplemented closure -- the one residue no guard
    // could remove, because the original configuration genuinely runs this arithmetic. The function
    // moved; the arithmetic did not.
    ((void(__stdcall *)(double))g_brs_tramp)(mh::fix::resync_order_exec_time(exec_time, horizon));
}

void install_resync_order_horizon() {
    // U30 retired the `*(uint32_t *)ADDR == PROLOGUE &&` that used to open this `if`. It is the site
    // D17 patched by hand -- and the hand patch is gone too (below), because the primitive now owns
    // the whole adjudication and this file's OTHER four install sites, 450 lines down, never got the
    // bespoke treatment. Fixing one site at a time is what let sync_gameover ship dead.
    if (install_trampoline(ADDR_BROADCAST_RESYNC_STATE, (void *)broadcast_resync_state_detour,
                           &g_brs_tramp, 8, mh::hook::entry_claim::exclusive,
                           "the resync-order horizon detour (MP D14)"))
        seam_log("; resync-order horizon armed: CTL_RESYNC_BEGIN exec_time clamped to LOCKSTEP_HORIZON (MP D14)\n");
    //
    // SINCE D17 THE PROMOTED CASE IS NO LONGER A LOSS, so it must not read like one. Our body carries
    // the same clamp behind the same [net] key (reimpl_fixes::resync_order_horizon), so this says
    // CARRIED -- the resync-wait fix's phrasing, for the same situation. THIS BRANCH IS THE ONE THING
    // U30 KEPT here, and it is kept for a reason the generic path cannot cover: the primitive can say
    // that a promotion displaced the detour, but only this site knows the fix survived the
    // displacement. What went is the DIAGNOSIS -- the primitive has already made it, by name, and
    // filed it in the summary; this only adds the good news.
    else if (mh::hook::promoted_owner_of(ADDR_BROADCAST_RESYNC_STATE)) {
        char b[288];
        wsprintfA(b,
                  "; resync-order horizon DISPLACED: broadcast_resync_state is promoted, and our body "
                  "CARRIES the clamp behind the same [net] resync_order_horizon key (=%d this run). MP "
                  "D14 is live here; use [promote] lockstep=0 for the original.\n",
                  // Read back the FIX STRUCT, not the ini global it was built from. The two can only
                  // disagree if set_fixes ran before the ini did, and that is exactly the class of
                  // wiring failure D17 was -- a knob that reads right while nothing consumes it. This
                  // way the arming line is evidence about the thing that actually gates the clamp.
                  (int)mh::lockstep::fixes().resync_order_horizon);
        seam_log(b);
    } else
        seam_log("; resync-order horizon NOT armed -- see the [interlock] line above for which of the "
                 "three reasons (owned entry / wrong bytes)\n");
}

// Suppress the in-game SYNCHRONIZING overlay (see the OVL_* block up top). Both call sites are guarded;
// either mismatch leaves the exe untouched and logs (ship-safe). Idempotent enough for a one-shot init.
void install_overlay_patches() {
    // C8-e (2026-07-30): the three `defang_*` knobs aimed inside llm_strat_time_tick -- tt_wait,
    // tt_sync and dismiss -- ARE DELETED. That is a SCOPE DECISION (C3b dropped: resync_trigger_gate
    // supersedes them as the root-cause fix and all three were default-off), NOT a retirement, and
    // The seam-class taxonomy 4c is explicit that the two must not borrow each other's reasoning.
    // A retirement moves a fix's carriage and owes the run a `refuse_uncarried_fix` line; a scope
    // decision removes a capability, and what it owes is a REFUSAL naming the knob as gone. Both are
    // in lockstep_install_core.
    //
    // WHAT SURVIVES AT 0x43f0fc, and it is the reason "delete defang_tt_wait" was not "delete the
    // patch": the wait-overlay call site is ALSO owned by the P4 de-sync-icon counter
    // ([net] icon_count, DEFAULT ON), which is an unrelated diagnostic that merely shares the address.
    // install_overlay_gate() used to fire on `tt_wait == 2 || (tt_wait == 0 && icon_count)`; with the
    // defang gone, `icon_count` is the whole condition and the thunk is a pure pass-through counter.
    //   xui  0=live 1=NOP -- extend_ui_enter wait+mode8 (the DOMINANT ~2s mode-8 freeze). KEPT: 4c
    //                       says it fails R1 and R2 (its target is a callee our body still calls
    //                       through), so it is out of scope for retirement no matter how far C8 goes.
    bool xw = true, xm = true;
    // Permanently 0 now: gating was `tt_wait == 2` and there is no tt_wait. The naked thunk still
    // READS this and still carries its gate branch -- deleting hand-written asm is a separate risk
    // and does not belong in the same change as a knob retirement, so the branch is left dead.
    g_icon_gate = 0;
    if (g_icon_count) install_overlay_gate(); // the thunk owns the wait-overlay call site
    // U20: and the OTHER carrier of the same two counters -- our promoted body, which is what
    // actually runs on the ship default. Wired under the same [net] icon_count knob so "count the
    // de-sync icon" means one thing whichever implementation is live, and unwired (null sinks) when
    // the knob is off so the two carriers stay switchable together.
    //
    // R3 (fork F1A ruling, landed F3D): BEHIND THE SELECTOR, for the same reason as R2's block above
    // and with the same non-effect. This registers two net-side counter sinks INTO the closure; under
    // `[config] mode=original` the body that would call them is not promoted, so the registration was
    // a write nobody read -- while still being a config-(1) link-level reference into mh/lockstep.
    // The byte-patch carrier of the SAME two counters is the `install_overlay_gate()` line right
    // above, and that one is deliberately NOT behind the selector: the thunk is what counts the icon
    // when our body is not live, which is precisely the original configuration.
    if (mh::config::ours_run())
        mh::lockstep::set_icon_counters(g_icon_count ? icon_note_wanted : nullptr,
                                        g_icon_count ? icon_note_shown : nullptr);
    if (g_defang_xui) {
        xw = patch_bytes_guarded(ADDR_OVL_XUI_WAIT, OVL_XUI_WAIT_EXPECT, OVL_XUI_WAIT_PATCH, 5);
        xm = patch_bytes_guarded(ADDR_OVL_XUI_MODE8, OVL_XUI_MODE8_EXPECT, OVL_XUI_MODE8_PATCH, 7);
    }
    // Three-way, not two-way (C1). A site inside a PROMOTED body is refused by the interlock, and
    // reporting that as "MISMATCH" would be a wrong-build message emitted by a perfectly good build.
    auto st = [](uintptr_t site, bool ok) -> const char * {
        if (mh::hook::promoted_owner_of(site)) return "displaced";
        return ok ? "ok" : "MISMATCH";
    };
    char b[420];
    wsprintfA(b, "; overlay patches (xui=%d icon_count=%d): xui_call=%s xui_mov=%s\n", g_defang_xui,
              g_icon_count, st(ADDR_OVL_XUI_WAIT, xw), st(ADDR_OVL_XUI_MODE8, xm));
    seam_log(b);
}

// Hi-res game clock via QPC (perf-decouple). The strategic sim clock reads
// GetTickCount (FUN_004cfec4/FUN_004cff80 -> INT_00e654e4, /100 in time::GetCurrentTime), whose ~15.6 ms
// resolution makes TOTAL_GAME_TIME advance in 16 ms quanta; the lockstep TOTAL=committed clamp then discards
// ~half a quantum (~8 ms) of un-simulatable overshoot per 100 ms step -> ~8% sim-rate loss at step=100
// (fps-independent; timeBeginPeriod(1) can't fix it -- GetTickCount is immutable on modern Windows). Both
// clock functions call GetTickCount INDIRECTLY through one IAT slot (0x01070414, `call dword ptr [0x1070414]`,
// .idata writable), so overwriting that slot redirects every GetTickCount call to a QPC-derived millisecond
// count (~1 ms resolution). TOTAL then climbs smoothly, the per-step discard shrinks ~16x, and the sim should
// approach 1.0x at step=100 -- no input-latency cost, no sim change (wall-clock resolution only, so
// determinism-safe: the deterministic sim_step sequence is untouched). Gated by mh_net.ini [net] qpc_clock.
constexpr uintptr_t ADDR_IAT_GETTICKCOUNT = mh::addr::IAT_GETTICKCOUNT; // game IAT slot for KERNEL32!GetTickCount
LARGE_INTEGER       g_qpc_clock_freq      = {0};
DWORD __stdcall my_gettickcount_qpc() { // GetTickCount contract: DWORD ms, monotonic; game uses deltas
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (DWORD)((t.QuadPart * 1000) / g_qpc_clock_freq.QuadPart);
}
void install_qpc_clock() {
    QueryPerformanceFrequency(&g_qpc_clock_freq);
    if (g_qpc_clock_freq.QuadPart == 0) {
        seam_log("; qpc_clock: QPF=0, skipped\n");
        return;
    }
    void **slot = (void **)ADDR_IAT_GETTICKCOUNT;
    DWORD  old;
    if (VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old)) {
        *slot = (void *)&my_gettickcount_qpc;
        VirtualProtect(slot, sizeof(void *), old, &old);
        FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void *));
        seam_log("; qpc_clock: GetTickCount IAT slot 0x01070414 -> QPC ms (~1 ms game-clock quantum)\n");
    } else {
        seam_log("; qpc_clock: VirtualProtect failed\n");
    }
}

} // namespace

// ==== install entries (called from net_seams' MH_Seam_Init, in this order) ========================
// Config knobs + overlay de-fang + hires/qpc clock + the timing-log path + the time_tick hook.
// Verbatim the pre-split MH_Seam_Init block (minus hold_start, which stays a net_seams concern).
// Defined below, after the detour it rebinds -- called from the end of lockstep_install_core (C4).
void install_time_tick_promotion();
void install_sim_tick_promotion();

void lockstep_install_core() {
    // DECLARE the time_tick promotion before anything patches bytes (C1 x C4). The rebind itself has to
    // happen at the END of this function, because it needs the pacing trampoline that this function
    // installs last -- but the four byte patches aimed INSIDE time_tick's body (defang_tt_wait,
    // defang_tt_sync, defang_dismiss, resync_trigger_reset) are written well before that. Registering
    // the promotion only at rebind time would therefore let all four land in a body that is about to be
    // JMP'd away: the C1 bug, reintroduced by C4's own mechanism. Measured, not theorised -- the first
    // promoted run after the rebind landed reported "1 registered fix(es) displaced" when the true
    // answer was 5. So the intent is recorded here and honoured by patch_bytes_guarded from this line
    // on; if the rebind below then fails, it says so emphatically, because that combination (fixes
    // suppressed AND the original still running) is the one state worse than either alone.
    if (g_lockstep_promoted.time_tick) mh::hook::note_promoted(ADDR_TIME_TICK);
    register_overlay_providers(); // debug overlay's net.* family (no-op unless [debug] is configured)
    // Optional time_tick hook: pins the lockstep lookahead (lockstep_step_ms>0) and/or writes the
    // per-frame timing log (lockstep_log=1). Install once if either is requested.
    // lockstep_step_ms is parsed as a STRING + atof so fractional ms is expressible (the 100+ε phase
    // sweep): "100.25" -> 0.10025 game-seconds. g_lockstep_step is already a double in game-seconds,
    // and on_time_tick pins it verbatim, so nothing downstream needs to change. (Was GetPrivateProfileIntA,
    // integer-only -- see the sub-ms step sweep.)
    // SHIP DEFAULTS (2026-07-25). Read with an EMPTY sentinel default so "absent" is distinguishable
    // from "explicitly set": an absent key takes the shipping value AND leaves the adaptive controller
    // free to move it, while any explicit lockstep_step_ms is an operator PIN (adaptive off unless
    // lockstep_adaptive=1 is also explicit). That is what keeps every existing rig -- mp_run.py and
    // test_ui.py both write explicit values -- behaving exactly as before this change.
    char step_buf[32];
    GetPrivateProfileStringA("net", "lockstep_step_ms", "", step_buf, sizeof(step_buf), g_ini);
    const bool step_explicit = (step_buf[0] != '\0');
    if (!step_explicit) lstrcpyA(step_buf, SHIP_LOOKAHEAD_MS);
    double step_ms = atof(step_buf);
    // Auto-nudge the lookahead OFF the 10 ms clock grid by a small epsilon (default 0.01 ms) so a round
    // config value (10/20/100) doesn't phase-lock with the centisecond clock -- the aliasing tax
    // (CORRECTION 3, the MP latency notes): exactly-on-grid step targets eat an extra 10 ms quantum most
    // steps (rate 0.92), any off-grid value recovers it (~0.96). So `lockstep_step_ms=100` -> 100.01 ms
    // in-game. Determinism-safe (both peers add the identical epsilon). NOTE this is LOOKAHEAD-only: the sim
    // sub-step (sim_step_ms) wants the OPPOSITE -- exactly ON-grid (10 ms = one clean sub-step per clock
    // tick) -- so no epsilon there. Set lockstep_step_eps_ms=0 to pin exact grid values (the ep-sweep does).
    char eps_buf[32];
    GetPrivateProfileStringA("net", "lockstep_step_eps_ms", "0.01", eps_buf, sizeof(eps_buf), g_ini);
    double step_eps_ms = atof(eps_buf);
    int    ls_log      = GetPrivateProfileIntA("net", "lockstep_log", SHIP_LOG_LEVEL, g_ini);
    if (step_ms > 0.0) g_lockstep_step = (step_ms + step_eps_ms) / 1000.0;
    // Adaptive lookahead (P1). ON by default, but only when lockstep_step_ms was NOT given: an
    // explicit value is an operator pin (every rig ini sets one, so every existing gate keeps its
    // fixed lookahead). lockstep_adaptive=1 in the ini overrides that and adapts from the pin as the
    // starting point.
    g_step_eps_ms = step_eps_ms;
    g_adaptive    = GetPrivateProfileIntA("net", "lockstep_adaptive", step_explicit ? 0 : SHIP_ADAPTIVE, g_ini);
    // NOTE the effective floor is max(this, 3 x sim_step) -- see AD_SIM_FLOOR_MULT. Lowering this
    // knob alone will NOT take the lookahead under 3 sub-steps; that guard is what a frozen client
    // cost us.
    g_ls_min = GetPrivateProfileIntA("net", "lockstep_min_ms", 30, g_ini) / 1000.0;
    g_ls_max = GetPrivateProfileIntA("net", "lockstep_max_ms", 400, g_ini) / 1000.0;
    if (g_ls_min < 0.010) g_ls_min = 0.010; // below ~1 clock quantum the sim cannot make progress
    if (g_ls_max < g_ls_min) g_ls_max = g_ls_min;
    // Strategic sim sub-step interval (movement-smoothness experiment). Independent of the lockstep
    // lookahead above: smaller => the committed window is consumed in finer sim steps => smoother unit
    // motion, SAME network horizon. 0 = leave the game default (0.1s / 10Hz). Fractional ms allowed.
    char sim_buf[32];
    GetPrivateProfileStringA("net", "sim_step_ms", SHIP_SIM_STEP_MS, sim_buf, sizeof(sim_buf), g_ini);
    double sim_step_ms = atof(sim_buf);
    if (sim_step_ms > 0.0) g_sim_step = sim_step_ms / 1000.0;
    // Off-frame RX drain (adaptive lookahead (b) Step 2). Runs in on_time_tick; needs the
    // time_tick hook (armed below). Both peers can set it independently. Off by default.
    g_rx_spin = GetPrivateProfileIntA("net", "rx_spin", SHIP_RX_SPIN, g_ini) != 0;
    // Experimental game-speed pin (percent; 100 = normal, 200 = 2x). Both peers must set the SAME value
    // (deterministic). Pinned every frame via the time_tick hook below (installed if this is set).
    int gs_pct = GetPrivateProfileIntA("net", "game_speed_pct", 0, g_ini);
    if (gs_pct > 0) g_game_speed = (double)gs_pct / 100.0;
    // ANNOUNCE THE EFFECTIVE SPEED, ALWAYS -- including when the knob was absent (AI0, 2026-08-01).
    // Two things make this worth a line rather than a comment. (1) game_speed_pct changes how much
    // GAME TIME a step carries, so per-step goldens recorded at different speeds are simply not
    // comparable -- and nothing downstream could previously TELL, because a run log did not say what
    // speed it ran at. `test_ui.py --sp-determinism` now reads this line out of each arm and refuses
    // a cross-speed comparison. (2) An unconditional line means the ABSENCE of it is itself the
    // signal, which is how the rig_fixed_step_loop section mismatch above was caught.
    {
        // g_game_speed defaults to 0.0 meaning "do not pin at all", so the EFFECTIVE percentage when
        // the knob is absent is the game's own 100 -- print that, not the sentinel.
        char b[128];
        wsprintfA(b, "; game_speed: %d%% (%s)\n", gs_pct > 0 ? gs_pct : 100,
                  gs_pct > 0 ? "pinned via [net] game_speed_pct" : "game_speed_pct absent -> unpinned");
        seam_log(b);
    }
    g_sync_gameover  = GetPrivateProfileIntA("net", "sync_gameover", 1, g_ini);  // default ON (endgame fix)
    g_graceful_leave = GetPrivateProfileIntA("net", "graceful_leave", 0, g_ini); // U17 (a) clean-quit self-removal; default OFF (opt-in; B2 covers quit via socket-close)
    g_graceful_drop  = GetPrivateProfileIntA("net", "graceful_drop", 1, g_ini);  // U17 (b) fast hard-drop on transport-death; default ON
    // Horizon heartbeat cadence (perf-decouple): >0 arms a thread that advertises the lockstep horizon
    // on real time, independent of render frames. Read here (DllMain, no thread); the thread starts in
    // lazy_start (off loader-lock). A good value is roughly lockstep_step_ms/4 .. /6 (e.g. 50 for 300).
    g_hb_ms = GetPrivateProfileIntA("net", "horizon_heartbeat_ms", SHIP_HEARTBEAT_MS, g_ini);

    // Overlay de-fang (perf-decouple step 2): byte-patch out the in-game SYNCHRONIZING modal so a
    // horizon-wait costs ~1 frame not ~2 s. Byte-patch (VirtualProtect) -> loader-lock safe, like the
    // seam installs. Pairs with the heartbeat to make a tight lookahead actually playable.
    g_defang       = GetPrivateProfileIntA("net", "defang_overlay", 0, g_ini);
    g_overlay_gate = GetPrivateProfileIntA("net", "overlay_gate", 0, g_ini); // gate the wait-overlay call (tt_wait=2)
    g_icon_count   = GetPrivateProfileIntA("net", "icon_count", 1, g_ini);   // P4: count de-sync icon shows (default ON, diagnostic-only)
    // REPURPOSED 2026-07-24 (proven in-game): defang_overlay=1 now means the FREEZE FIX ONLY -- NOP the
    // extend_ui_enter mode-8 path (`xui`), the DOMINANT freeze (a routine lockstep-extend order fires
    // ~3x/sec and pins the game clock ~2 s each -> 0.13x sim rate; determinism-clean to NOP). The mode-3
    // overlays stay LIVE by default so the de-sync corner ICON (tt_wait) and the "Player not responding"
    // kick MODAL (tt_sync) still show on genuine stalls (display-only, don't pin the clock, det-safe). Each
    // group is individually overridable; set defang_tt_sync=1 to restore the old suppress-the-modal behavior.
    g_defang_xui           = GetPrivateProfileIntA("net", "defang_xui", g_defang, g_ini);    // freeze fix (=defang_overlay)
    g_resync_wait_fix      = GetPrivateProfileIntA("net", "resync_wait_fix", 1, g_ini);      // defang dep #2 REAL fix (kills the resync busy-wait garbage-spin hang); default ON
    g_resync_trigger_reset = GetPrivateProfileIntA("net", "resync_trigger_reset", 0, g_ini); // spurious-resync ROOT fix (option b); default OFF pending validation
    g_resync_trigger_gate  = GetPrivateProfileIntA("net", "resync_trigger_gate", 1, g_ini);  // spurious-resync ROOT fix (option c); DEFAULT ON -- validated 2026-07-25 (0.98x/0 resyncs, det-clean, drop path preserved). Supersedes defang_xui as the freeze fix.
    // MP U20. DEFAULT OFF pending the measurement it exists to be judged by: the icon storm's
    // proximate cause is the adaptive controller saddling its ceiling and probing down (2026-08-29),
    // and suppressing an icon that a correctly-sized lookahead already stops firing would be hiding a
    // signal for nothing. Reimpl-ONLY -- there is no byte patch, so an unpromoted run cannot carry
    // it; the arming line below says so rather than letting a run believe it is gated.
    g_desync_icon_gate     = GetPrivateProfileIntA("net", "desync_icon_gate", 0, g_ini);
    g_resync_order_horizon = GetPrivateProfileIntA("net", "resync_order_horizon", 1, g_ini); // MP D14: schedule the resync-begin synthetic order at the horizon like every other replicated order; DEFAULT ON
    // C8-e: the three retired `defang_*` knobs. A SCOPE DECISION, not a retirement -- the capability
    // is gone, so there is no carrier to point at and refuse_uncarried_fix would be the wrong message
    // (it says "the fix moved and you are not running the thing that carries it"; here there is no
    // fix any more). What an ini that still sets one is owed is a refusal naming it as GONE, for the
    // same reason `[promote] wire` refuses rather than being ignored: a stale fragment must not read
    // as a deliberate configuration that quietly did nothing. Archived in the retired-patch log.
    //
    // F3D ADDED A FOURTH ROW AND MADE THE REASON PER-KNOB, because `fix_audit` retires for a
    // different reason than the three defangs and "delete it" without the why is the useless half of
    // the message. THIS IS A NOTICE, NOT A REFUSAL, and that is a decision rather than an omission:
    // mh::config's `refuse` machinery TERMINATES the process, and it is scoped to configuration
    // VOCABULARY -- a surviving `[promote]` section or an unknown `[config] mode` is a statement
    // about WHICH BODIES RUN, and a run whose author believes something false about that is the
    // failure F2E existed to remove. `fix_audit=60` makes no such statement: the run executes
    // identically without it, and the operator's symptom is a log missing its `[fix_audit]` rows.
    // That symptom is cured by a line in the same log, which is what this is, and it costs one table
    // row instead of a new refusal class. (The three defang rows' emitted text is byte-identical
    // across this change -- check_arm_order gates it -- and all four lines are absent from every lane
    // because no lane sets any of these keys.)
    struct retired_knob {
        const char *name;
        const char *why;
    };
    static const char *const kDefangWhy =
        "C8-e -- the three time_tick defangs were dropped as a scope decision; resync_trigger_gate "
        "supersedes them as the root-cause fix";
    static const retired_knob RETIRED_KNOBS[] = {
        {"defang_tt_wait", kDefangWhy},
        {"defang_tt_sync", kDefangWhy},
        {"defang_dismiss", kDefangWhy},
        {"fix_audit",
         "fork F3D / ruling Q3 -- the C3 equivalence sampler is DELETED. Its proof was a PAIR of "
         "columns and C8-e retired the byte patch that filled the left one, so the line could no "
         "longer re-prove the migration; lockstest's test_w5_sent_gate_audit_tally is the oracle now"},
    };
    for (const retired_knob &gone : RETIRED_KNOBS) {
        if (GetPrivateProfileIntA("net", gone.name, -1, g_ini) != -1) {
            char rb[512];
            wsprintfA(rb,
                      "; [net] RETIRED KNOB: %s no longer exists (%s). The key is IGNORED. Delete "
                      "it.\n",
                      gone.name, gone.why);
            seam_log(rb);
        }
    }
    install_overlay_patches(); // self-gates: each group applied per its knob (incl. the gate for tt_wait==2)
    if (g_resync_wait_fix) install_resync_wait_fix();
    if (g_resync_trigger_reset) install_resync_trigger_reset();
    // C8 (ii): refuse_uncarried_fix is DEFINED above but deliberately NOT CALLED yet. It gets wired
    // per patch, in C8.3, AS EACH `if (knob) install_x();` LINE IS REPLACED BY IT -- which is the
    // only place it can be correct. Wiring it here alongside a still-present install_x() would make
    // every ordinary run announce that a patch had been retired when it had not: the predicate is
    // "our body is not live here", which is TRUE in any unpromoted run, and unpromoted-with-the-knob-
    // on is the DEFAULT (resync_trigger_gate defaults to 1). The first attempt did exactly that.
    // C3: the SAME knob now drives both carriers, which is the point -- whichever implementation is
    // live, `[net] resync_trigger_gate` means one thing. The byte patch below is what carries it while
    // llm_net_lockstep_dispatch runs the original; this hands the identical setting to our body for
    // when it does not, and C1's interlock guarantees the two can never both be live over one site.
    //
    // R2 (fork F1A ruling, landed F3D): THE WHOLE BLOCK IS BEHIND THE SELECTOR. Everything in it is a
    // handoff INTO the reimplemented closure -- read `fixes()`, overwrite five fields, `set_fixes()`,
    // and report what our body will therefore do -- and under `[config] mode=original` there is no
    // closure to configure: nothing of ours is promoted, so the struct it writes is read by nobody.
    // That made this the largest config-(1) residue in check_net_lockstep_refs' enumeration (four of
    // the nine symbols: fixes / set_fixes / reimpl_fixes / SYNC_OVERLAY_AFTER), and it was residue for
    // no behavioural reason at all -- the call ran, wrote a struct, and the original bodies ignored it.
    //
    // IT CHANGES NOTHING UNDER `brokered`, which is the only configuration that reaches the body these
    // fields steer. Under `original` the two log lines inside cannot fire at a stock ini either
    // (`desync_icon_gate` and `rig_fixed_step_loop` both default 0), so the arm log is unchanged in
    // both modes -- and if an original-mode run DID set `desync_icon_gate=1`, the line it used to
    // print said "INERT -- this fix exists ONLY in our body and time_tick is NOT promoted this run",
    // which is exactly the thing the selector now settles one level up.
    if (mh::config::ours_run()) {
        mh::lockstep::reimpl_fixes fx = mh::lockstep::fixes();
        fx.resync_trigger_gate        = g_resync_trigger_gate != 0;
        fx.resync_trigger_reset       = g_resync_trigger_reset != 0;
        // RIG-ONLY. Default 0; it forces sim_tick's mode-3 fixed-step loop in single-player so the SP
        // oracle integrates like MP. It only takes effect on a run that also promotes sim_tick
        // ([promote] sim_tick=1) -- the original body still branches on the mode -- so the arming line
        // below reports BOTH, because "knob set" and "knob effective" differ here and a run that
        // confused them would silently measure the stock shape.
        //
        // IT LIVES IN [net], NOT [harness], AND THAT IS STILL NOT A STYLE CHOICE -- though the
        // reason narrowed at F2G. It was: `g_ini` here is mh_net.ini while the [harness] section
        // lived in a SEPARATE FILE (mh_harness.ini), so the first version's ("harness", ..., g_ini)
        // read was always 0 -- GetPrivateProfileInt cannot distinguish "absent section" from "set to
        // the default" -- and the knob was inert while the run reported success. The arming line
        // below is what caught it, by NOT printing. F2G merged the files, so that read would now
        // resolve; the key stays in [net] because this is a NET knob and because [harness] keys are
        // inert unless `[harness] enable=1` armed the instrument, which this one must not require.
        fx.rig_fixed_step_loop = GetPrivateProfileIntA("net", "rig_fixed_step_loop", 0, g_ini) != 0;
        // D17: the same knob the trampoline used, so it means ONE thing whichever implementation is
        // live -- which is the third property reimpl_fixes is documented to have. Note the ini
        // DEFAULT here is 1, unlike its `= false` initialiser in the struct: the initialiser is the
        // faithful-stock value a pure caller or a lockstest fixture gets, while the shipped run has
        // had this fix on since D14, and restoring that is the point of carrying it.
        fx.resync_order_horizon = g_resync_order_horizon != 0;
        // U20. Unlike every other field here, this knob has NO byte-patch carrier -- `defang_tt_wait`
        // was its nearest ancestor and C8-e dropped it, leaving wait_overlay_gate_thunk as a pure
        // counter with `g_icon_gate` hardcoded 0. So an UNPROMOTED run with this set has no gate at
        // all, and that has to be said out loud rather than discovered from an unchanged icon rate.
        fx.desync_icon_gate = g_desync_icon_gate != 0;
        mh::lockstep::set_fixes(fx);
        if (g_desync_icon_gate) {
            const bool carried = g_lockstep_promoted.time_tick;
            char       b[300];
            wsprintfA(b,
                      "; desync_icon_gate=1 (MP U20: the de-sync icon shows only below "
                      "SYNC_RETRY_COUNTDOWN %d): %s\n",
                      (int)mh::lockstep::SYNC_OVERLAY_AFTER,
                      carried ? "ARMED -- time_tick is promoted and our body carries it"
                              : "INERT -- this fix exists ONLY in our body and time_tick is NOT "
                                "promoted this run. There is no byte patch to fall back on. Enable "
                                "[promote] lockstep, or accept the stock icon knowingly.");
            seam_log(b);
        }
        if (fx.rig_fixed_step_loop) {
            char b[160];
            wsprintfA(b, "; rig_fixed_step_loop=1 (SP integrates at SIM_STEP_INTERVAL); sim_tick promoted=%d\n",
                      (int)g_lockstep_promoted.sim_tick);
            seam_log(b);
        }
    }
    // C8-e: THIS IS WHERE refuse_uncarried_fix FINALLY GETS CALLED, and only now is it correct.
    // C8.1 defined it and deliberately left it uncalled, with a comment recording the first attempt's
    // mistake: putting it next to a STILL-PRESENT installer and calling it "silent today by
    // construction". That was false -- its predicate is "our body is NOT live here", which is true in
    // any unpromoted run, and unpromoted-with-the-knob-on was the default. With the installer gone,
    // that same sentence is no longer a false alarm: it is the truth. An unpromoted run with
    // `[net] resync_trigger_gate=1` really does NOT have the fix at either site any more.
    //
    // PER SITE, not per knob. One knob covers two addresses in two different owners, and W4.5's
    // defect was exactly a single carrier claiming both -- true for the recv site, false for the sent
    // one -- while lint reported PASS. Two calls, two owners named.
    if (g_resync_trigger_gate) {
        refuse_uncarried_fix("resync_trigger_gate", 0x0049c508, "llm_net_lockstep_dispatch");
        refuse_uncarried_fix("resync_trigger_gate", 0x0049d8cb, "llm_net_send_lockstep_keepalive");
    }
    if (g_resync_order_horizon) install_resync_order_horizon();

    // Hi-res game clock (perf-decouple). The strategic sim clock (time::GetCurrentTime
    // = INT_00e654e4/100, written from GetTickCount in FUN_004cfec4/FUN_004cff80) advances in ~15.6 ms
    // GetTickCount quanta. At a tight lockstep lookahead the TOTAL=committed clamp discards ~half a quantum
    // (~8 ms) of un-simulatable overshoot every 100 ms step -> ~8% sim-rate loss (fps-INDEPENDENT; confirmed by
    // a vsync-off run at 891 fps showing no change). timeBeginPeriod(1) lowers the global system-timer period so
    // GetTickCount updates at ~1 ms -> the quantum (and thus the per-step discard) shrinks ~16x -> the sim
    // approaches 1.0x at step=100, with NO input-latency cost and NO sim change (wall-clock resolution only ->
    // determinism-safe). Gated by mh_net.ini [net] hires_clock. (Process exit restores the period.)
    if (GetPrivateProfileIntA("net", "hires_clock", SHIP_HIRES_CLOCK, g_ini)) {
        timeBeginPeriod(1);
        seam_log("; hires_clock: timeBeginPeriod(1) armed (finer Sleep/scheduler; does NOT affect GetTickCount)\n");
    }
    // Hi-res game clock via QPC (the real timer-quantum fix; see install_qpc_clock). Redirects the game's
    // GetTickCount (IAT slot 0x01070414) to a QPC-derived ms count so TOTAL climbs in ~1 ms steps.
    if (GetPrivateProfileIntA("net", "qpc_clock", SHIP_QPC_CLOCK, g_ini)) install_qpc_clock();

    int sp_log = GetPrivateProfileIntA("net", "sp_clock_log", 0, g_ini); // SP clock cross-check (log every frame)
    if (ls_log || sp_log) {                                              // build mh_lockstep.log path next to the exe
        lstrcpynA(g_ls_path, g_log, MAX_PATH);                           // g_log = "...\mh_net.log"
        char *slash = g_ls_path;
        for (char *p = g_ls_path; *p; ++p)
            if (*p == '\\' || *p == '/') slash = p;
        lstrcpyA(slash + 1, "mh_lockstep.log");
        g_ls_log    = ls_log != 0;
        g_ls_log_sp = sp_log != 0;
    }
    if (g_lockstep_step > 0.0 || g_ls_log || g_ls_log_sp || g_game_speed > 0.0 || g_sim_step > 0.0 ||
        g_rx_spin || g_graceful_drop || g_adaptive) { // graceful_drop + adaptive need on_time_tick every frame
        // entry_claim::rebind (C9): time_tick IS promoted in the default closure, and this detour
        // holding its entry is C4's protocol -- the promotion rebinds our fall-through rather than
        // writing the entry itself. An exclusive claim would be refused here. The Watcom-prologue
        // expectation is still the default one this site used to check itself (U30).
        if (install_trampoline(ADDR_TIME_TICK, (void *)time_tick_detour, &g_tt_tramp, 8,
                               mh::hook::entry_claim::rebind, "the pacing/adaptive/rx_spin time_tick detour")) {
            // Claim the entry (C4). From here a direct entry patch on llm_strat_time_tick -- an
            // install_export, a second trampoline -- is refused BY NAME instead of losing a silent race
            // against whichever writer got there first.
            mh::hook::note_entry_owner(ADDR_TIME_TICK,
                                       "time_tick_detour (pacing / adaptive / rx_spin / graceful_drop)");
            char b[280];
            wsprintfA(b, "; time_tick hook armed: lockstep_step=%s(+%s eps) ms, adaptive=%d [%ld..%ld ms], sim_step=%s ms, rx_spin=%d, lockstep_log=%d, sp_clock_log=%d, game_speed=%d%%\n",
                      step_buf, eps_buf, g_adaptive, (long)(g_ls_min * 1000.0), (long)(g_ls_max * 1000.0),
                      sim_buf, (int)g_rx_spin, ls_log, sp_log, gs_pct);
            seam_log(b);
        } else {
            g_adaptive      = 0;
            g_lockstep_step = 0.0;
            g_ls_log        = false;
            g_ls_log_sp     = false;
            g_sim_step      = 0.0;
            seam_log("; time_tick hook NOT armed -- see the [interlock] line for the reason\n");
        }
    }
    install_time_tick_promotion();
    install_sim_tick_promotion();

    // LT1F (2026-09-02): the frame pair, LAST -- it may only promote over a promoted spine (its
    // bodies call OUR pump/time_tick/sim_tick directly; sim_lt_frame.h carries the routing
    // decision + interlock rationale). spine_promoted is computed from what ACTUALLY installed
    // above, not from the ini request; the two chain hooks reach the unit as pointers because
    // on_time_tick/on_sim_tick are TU-local to their instruments.
    mh::hook::register_callback(mh::hook::point::lt_frame_pace_time_tick, &lt_frame_pace_time_tick);
    mh::hook::register_callback(mh::hook::point::lt_frame_harness_sim_tick, &MH_Harness_OnSimTick);
    mh::hook::arm_frame_promotion(
        mh::config::ours_run(),
        (g_lockstep_promoted.active && g_tt_promoted_ok && g_sim_tick_promoted_ok) ? 1 : 0);
    // The same prelude, handed to the OTHER body that reaches our time_tick without the entry --
    // llm_strat_time_resync_and_tick (SIM-SAVE-DIV). UNCONDITIONAL on purpose, unlike the frame
    // installer above: the hook is already armed-gated on g_tt_tramp, and the receiving side gates
    // again on its own rebind row, so handing it over costs nothing when either is off. Tying it to
    // the frame promotion instead would leave the load path unpinned in every config that promotes
    // the resid domain without the frame pair -- which is exactly the config the divergence was
    // found in.
    mh::hook::register_callback(mh::hook::point::time_resync_prelude, &lt_frame_pace_time_tick);
}

// C4: promote llm_strat_time_tick by REBINDING the pacing detour's fall-through, or -- if no detour
// armed, so the entry is unclaimed -- by the ordinary entry patch. Called at the END of
// lockstep_install_core, after the trampoline has had its chance, because the rebind needs a detour to
// rebind. mh::lockstep::install_promotion runs EARLIER (reimpl_probe_install) and only records the
// request; that ordering is why the two halves are split rather than done in one place.
//
// Three outcomes, all of them logged, because "promotion silently did not happen" is the failure this
// project keeps rediscovering -- an asymmetric run whose asymmetry evaporated PASSES.
void install_time_tick_promotion() {
    if (!g_lockstep_promoted.time_tick) return;
    void *ours = mh::lockstep::time_tick_entry_thunk();

    if (g_tt_tramp) {
        g_tt_promoted    = ours;
        g_tt_promoted_ok = true;
        seam_log("; [promote] time_tick REBOUND: the pacing detour now falls through to OURS "
                 "(on_time_tick still runs first -- adaptive/rx_spin/graceful_drop are unaffected)\n");
        return;
    }
    // No detour armed this run (no pacing knob set), so nobody owns the entry and a direct install is
    // the correct path -- same seam, different route, and the route is reported either way.
    if (mh::hook::install_export_ok(ADDR_TIME_TICK, ours, "llm_strat_time_tick",
                                    mh::exp::entry_llm_strat_time_tick)) {
        g_tt_promoted_ok = true;
        seam_log("; [promote] time_tick INSTALLED directly (no pacing detour armed this run)\n");
    } else
        seam_log("; [promote] time_tick NOT promoted -- neither a detour to rebind nor an installable "
                 "entry. TREAT THIS RUN AS INVALID: the promotion was DECLARED at the top of "
                 "lockstep_install_core, so the byte patches inside time_tick's body were suppressed, "
                 "and the ORIGINAL body is what will run -- fixes lost with no implementation carrying "
                 "them. Fix the arming failure; do not interpret this run.\n");
}

// C6: promote llm_strat_sim_tick by REBINDING the DETERMINISM HARNESS's detour, or -- if the harness
// did not arm this run, so the entry is unclaimed -- by the ordinary entry patch. The instrument and the
// subject both want this entry, and the asymmetry is what forces the rebind: if the export seam won the
// race it would be the HARNESS that refused, which VOIDS a determinism run silently (no hashes) instead
// of failing it. So the harness keeps the entry and only its fall-through moves.
//
// OPT-IN ONLY ([promote] sim_tick=1). Not in the default closure -- every L1-P promotion result was
// measured with sim_tick original, and quietly changing what `lockstep=1` installs would make all of it
// describe a configuration nobody ran.
//
// Three outcomes, all logged, for the same reason as time_tick's: "promotion silently did not happen"
// is the failure this project keeps rediscovering.
void install_sim_tick_promotion() {
    if (!g_lockstep_promoted.sim_tick) return;
    void *ours = mh::lockstep::sim_tick_entry_thunk();

    if (MH_Harness_RebindSimTick(ours)) {
        g_sim_tick_promoted_ok = true;
        seam_log("; [promote] sim_tick REBOUND: the determinism harness detour now falls through to "
                 "OURS (on_sim_tick still runs first -- the fixed-timestep pin and the per-step hash "
                 "are unaffected)\n");
        return;
    }
    // No harness this run (no `[harness] enable=1`), so nobody owns the entry and a direct install is the
    // correct path -- same seam, different route, and the route is reported either way.
    if (mh::hook::install_export_ok(mh::exp::addr_llm_strat_sim_tick, ours, "llm_strat_sim_tick",
                                    mh::exp::entry_llm_strat_sim_tick)) {
        g_sim_tick_promoted_ok = true;
        seam_log("; [promote] sim_tick INSTALLED directly (no determinism harness armed this run)\n");
    } else
        seam_log("; [promote] sim_tick NOT promoted -- neither a harness detour to rebind nor an "
                 "installable entry. TREAT THIS RUN AS INVALID: the request was made, so a reader will "
                 "assume our body ran, and the ORIGINAL is what will execute.\n");
}

// Present hook (llm_gfx_present_flip, post-sim_tick): drives the Phase-3 frame-time log (frametime_log)
// and/or the eager horizon advertisement (eager_advertise, perf-decouple) and/or the per-EVENT temporal
// trace drain ([trace] temporal=1, net_diag.cpp). Install the shared hook once if any is requested.
// Then the game-over leave-lockstep detour (gated by sync_gameover, read in lockstep_install_core).
void lockstep_install_present_gameover() {
    int ft_log    = GetPrivateProfileIntA("net", "frametime_log", SHIP_LOG_LEVEL, g_ini);
    int eager_adv = GetPrivateProfileIntA("net", "eager_advertise", SHIP_EAGER_ADV, g_ini);
    // Per-EVENT temporal trace ([trace] temporal=1). Needs the present hook too (TEV_PRESENT + the flush);
    // net_diag owns the recorder -- temporal_configure() reads the flag + inits QPF and the log path.
    int temporal = temporal_configure();
    // UI-REC: the harness's own claim on this hook. Its input journal is indexed on presents and its
    // menu clock advance happens here, so a run with `[harness] ui_journal_rec/ui_journal` set needs
    // the hook whatever the three keys above say. Without this the recorder rides SHIP_EAGER_ADV=1
    // and reports ARMED while journalling nothing the moment a fragment sets eager_advertise=0.
    int ui_journal = MH_Harness_WantsPresentTick();
    if (ft_log || eager_adv || temporal || ui_journal) {
        if (ft_log) {
            lstrcpynA(g_ft_path, g_log, MAX_PATH); // derive path next to mh_net.log (run folder)
            char *slash = g_ft_path;
            for (char *p = g_ft_path; *p; ++p)
                if (*p == '\\' || *p == '/') slash = p;
            lstrcpyA(slash + 1, "mh_frametime.log");
        }
        // ONE ENTRY, ONE OWNER (hook/promoted.h C4). This site used to have TWO hosts: a pre-hook
        // registered with the effects gate that owned llm_gfx_present_flip's entry (U33), and this
        // own detour as the fallback for `[effects] arm=0`. Fork F2F deleted the deferred-effect
        // machinery (ruling D8), so the gate is gone and nothing else claims the entry -- the
        // fallback became the only host, which is what it was designed to be able to do.
        //
        // The fallback existing at all is the reason this drop was a deletion rather than a
        // regression: frame capture, the UI automation driver and the overlay ALL piggyback
        // on_present, so a host that vanished with the seam would have taken the whole harness
        // with it (dead-ends G99).
        const bool armed =
            install_trampoline(ADDR_PRESENT, (void *)present_detour, &g_ft_tramp, 8,
                               mh::hook::entry_claim::exclusive,
                               "the present hook (frametime / eager-advertise / temporal trace)");
        if (armed) {
            if (ft_log) g_ft_log = true;
            if (eager_adv) g_eager_adv = 1;
            if (temporal) g_temporal = true;
            char b[240];
            // The ui_journal clause is APPENDED rather than added as a fourth field, so a run that
            // does not use it emits the byte-identical line it emitted before -- this line's
            // sequence is diffed against a baseline in the refactor gate.
            wsprintfA(b,
                      "; present hook armed (own detour): frametime_log=%d eager_advertise=%d "
                      "temporal=%d%s\n",
                      ft_log, eager_adv, temporal, ui_journal ? " ui_journal=1" : "");
            seam_log(b);
        } else {
            seam_log("; present hook NOT armed -- see the [interlock] line for the reason\n");
        }
    }

    // Endgame-sync: leave lockstep at the game-over dialog, as a run-before detour over
    // llm_ui_outcome_dialog's entry.
    //
    // THE HISTORY IS THE ARGUMENT FOR THE SHAPE, and it is why the fallback below outlived the
    // mechanism that displaced it. This site printed `; game-over leave-lockstep NOT armed
    // (unexpected prologue)` on both peers of every shipped run, 61 ms after the effects seam
    // armed, and it was never a prologue problem: the effects gate over llm_ui_outcome_dialog had
    // taken the entry first. U30 handed the entry over; U33 reversed that into gate PARTICIPATION
    // so the gate could keep it; F1C (2026-09-12) proved participation alone was dead whenever the
    // gate was disarmed and restored this own-detour host beside it. Fork F2F (ruling D8) then
    // deleted the gates altogether, so the own detour is the only host again -- the pre-U33 shape,
    // reached deliberately this time. The outcome argument is invisible to the naked thunk (logged
    // as 0); nothing else about the fix changes.
    if (g_sync_gameover) {
        const bool go_armed = install_trampoline(
            ADDR_GAMEOVER_DLG, (void *)gameover_detour, &g_go_tramp, 8,
            mh::hook::entry_claim::exclusive,
            "the game-over leave-lockstep fix ([net] sync_gameover)");
        if (go_armed)
            seam_log("; game-over leave-lockstep armed (own detour): loser -> SESSION "
                     "3->2 at the outcome dialog\n");
        else
            seam_log("; game-over leave-lockstep NOT armed -- see the [interlock] line for "
                     "the reason\n");
    }

    // U17 (a) clean-quit self-removal: hook llm_game_return_to_main_menu_cb entry (55 89 E5 68 prologue).
    if (g_graceful_leave) {
        // THE NAMED NEXT VICTIM (U30's census). This site is un-collided only because nothing targets
        // llm_game_return_to_main_menu_cb yet -- and it IS promotable
        // (mh::exp::addr_llm_game_return_to_main_menu_cb exists with no MH_EXPORT_REPLACE bound), so
        // the day something claims it this would have printed the same wrong-build message the
        // gameover site printed for its whole life. It is under the general guard now, by name,
        // BEFORE the collision rather than after it.
        if (install_trampoline(mh::addr::llm_game_return_to_main_menu_cb, (void *)quit_to_menu_detour,
                               &g_quit_tramp, 8, mh::hook::entry_claim::exclusive,
                               "the U17 graceful-leave detour ([net] graceful_leave)"))
            seam_log("; U17 graceful-leave armed (ESC->Quit broadcasts self-removal before teardown)\n");
        else
            seam_log("; U17 graceful-leave NOT armed -- see the [interlock] line for the reason\n");
    }
}

// Start the horizon heartbeat once the transport is up (called from net_seams' lazy_start = off
// loader-lock, like the transport threads). Gated by [net] horizon_heartbeat_ms; the thread itself
// no-ops until mode 3.
void lockstep_transport_started() {
    if (g_hb_ms > 0 && !g_hb_thread) {
        InterlockedExchange(&g_hb_run, 1);
        g_hb_thread = CreateThread(nullptr, 0, horizon_heartbeat_thread, nullptr, 0, nullptr);
        char b[128];
        wsprintfA(b, "; horizon heartbeat started: advertise every %d ms (perf-decouple)\n", g_hb_ms);
        seam_log(b);
    }
}
