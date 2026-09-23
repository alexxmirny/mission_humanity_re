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
// MH_FontGuard_OnPresent (gfx_font_guard.cpp) -- mp:F2 [fonts] probe_text. The comment is ABOVE the
// include, not beside it: this path is one character longer than the block's longest, and a trailing
// comment here would re-align every other line in it.
#include "include/mh_fontguard_export.h"
#include "seams/ui_net_indicator.h"    // mp:L1 MH_NetIndicator_OnPresent -- the player-visible indicator
#include "include/mh_uidrive_export.h" // MH_UIDrive_OnPresent (ui_drive.cpp) -- UI automation Phase 2
#include "include/mh_harness_export.h" // MH_Harness_RebindSimTick -- C6 sim_tick promotion by rebind
#include "include/mh_module_bind.h"    // MH_Libmh_OnPresent -- F4D's spine-crossing report
#include "config/config.h"             // F2A: the D11 selector behind the frame pair's promotion default
#include "config/ini_read.h"           // TL-HARN4: read_ini_string -- strips a trailing `;comment`
#include "addr/mh_addrs.gen.h"         // generated EN VAs (tools/gen_dll_addrs.py)
#include "addr/mh_calls.gen.h"         // mh::call::llm_net_lockstep_count_active_players (mp:P9 resync_count_init)
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
// mp:T3. Reached by relative path rather than through an include directory because it is a
// satellite module's header and mh.dll is not that module -- the same shape as
// hostapi_io_bind.cpp's "../../libmh/include/libmh.h". It is header-only so that the three
// projects that need the arithmetic (mh, mh_net_udp, mh_nettest) share ONE definition, and so
// the offline suite that proves it (net_selftest.exe udpstatstest) proves the code mh.dll runs.
#include "../../mh_net_udp/udp_stats.h" // RFC 6298 / 3393 / 7680 + the lookahead decision

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
extern "C" void MH_MP_DrainRelayNotice(void); // net_discovery -- mp:R4a: arm the queued relay-level notice (main thread)

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

// mp:P9W (2026-09-22): the ONLY call to llm_wait_screen_frame (0x0043ee38, the GAME_MODE==8 frame),
// inside llm_frame_dispatch's case 8 -- ReVA find-cross-references on llm_wait_screen_frame shows
// exactly this one incoming reference. Raw VA, deliberately NOT added to the generated address
// manifest -- this is a CALL SITE inside frame_dispatch, not a region/global, same precedent as
// lobby_ping.cpp's ADDR_SLOT_WIDGET_PTRS (a manifest entry would also mean a Ghidra-side edit, out
// of scope for a byte-patch splice like this one). Verified against /eng/mh.exe (read-memory).
constexpr uintptr_t ADDR_WAIT_SCREEN_CALL_SITE = 0x004a0423u;
const uint8_t       WAIT_SCREEN_CALL_EXPECT[5] = {0xE8, 0x10, 0xEA, 0xF9, 0xFF}; // call 0x0043ee38 (llm_wait_screen_frame)

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
int  g_desync_icon_gate      = 0; // [net] desync_icon_gate -- MP U20; reimpl-only, see reimpl_fixes
int  g_gone_peer_frame_guard = 1; // [net] gone_peer_frame_guard -- MP U19e; reimpl-only, DEFAULT ON, see reimpl_fixes
int  g_defang_xui            = 0; // extend_ui_enter wait+mode8 pair (the DOMINANT ~2s mode-8 freeze): 0=live 1=NOP
int  g_resync_trigger_reset =
    0; // 1 = zero RESYNC_TRIGGER_COUNT on horizon recovery (spurious-resync ROOT fix, option b; see install_resync_trigger_reset)
int g_resync_trigger_gate =
    0; // 1 = gate both RESYNC_TRIGGER_COUNT increments on SYNC_RETRY_COUNTDOWN<0x38 (option c; count only genuine silence)
int g_resync_count_init =
    1; // 1 (default) = recompute ACTIVE_PLAYER_COUNT at match start so the resync threshold is players*100, not 0 (mp:P9 root fix; see resync_count_init_tick)
int g_resync_order_horizon =
    1;               // 1 (default) = clamp the CTL_RESYNC_BEGIN synthetic order's exec_time to LOCKSTEP_HORIZON (MP D14; see install_resync_order_horizon)
int g_eager_adv = 0; // 1 = advertise+commit the post-step horizon in the present hook (perf-decouple diagnostic: proved committed is never the binding constraint; determinism-safe, no rate effect)
int g_resync_receiver_deadline =
    1; // 1 (default) = mp:P9W: a non-leader forces itself out of the mode-8 wait screen after max(2002,
       // data_timeout_ms) ms with no RESUME, then removes the (presumed-dead) leader -- see
       // resync_receiver_deadline_hook / install_resync_receiver_deadline

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
// in-order.
//
// DEFAULT ON SINCE U19 (2026-09-18), and the two sentences the old default rested on were both wrong.
// It said B2 (graceful_drop) "already catches a clean quit via the socket-close", so (a) was redundant.
// It does not: a player who quits to the MAIN MENU leaves the process running with its socket open, and
// the U40 relink that would close it is consumed by the discovery browser, which a player who walks away
// never opens. Measured on the rig with the knob OFF: the survivor sat in the SYNCHRONIZING spinner for
// 56.7 s (974 overlay icon calls) before the retail silence timeout ended its match. With the knob ON the
// same walk ended the survivor's match 187 ms after the quitter's broadcast, at the SAME game clock on
// both peers (3259 ms) -- the in-order dispatch drop the design always promised. The second wrong
// sentence was "possible 2-player game-over flash": the quitter's self-removal DOES reach on_gameover
// below quorum, and the outcome dialog is suppressed there already (`selfremove_dlg_suppressed`), so the
// quitter's captured frame is the plain main menu. Set [net] graceful_leave=0 to go back to the timeout.
int   g_graceful_leave = 1;
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
// mp:U19b -- the quitter's HALF of the parked precondition. [net] graceful_leave_park (default ON):
// before the self-removal, freeze OUR advertised horizon at H_d and wait (inside the quit callback,
// bounded by graceful_leave_park_ms) until every survivor has parked on it, so all of them apply the
// roster flip at the same sim clock -- the D5 rule, seen from the departing peer. See on_quit_to_menu.
int           g_leave_park      = 1;
int           g_leave_park_ms   = 1500;       // safety valve: send the removal anyway after this long
volatile LONG g_leave_frozen    = 0;          // 1 = no more horizon adverts from this peer (heartbeat + eager)
double        g_leave_frozen_hd = 0.0;        // mp:U19h -- the H_d that freeze advertised; the value every
                                              // survivor must hold when the removal record arrives
bool             g_leave_in_progress = false; // the freeze..teardown window of one quit (on_time_tick's guard)
double           g_hz_max_seen       = 0.0;   // max HORIZON sampled this match (adaptive shrink can lower it)
CRITICAL_SECTION g_hb_cs;                     // serialises the heartbeat's write+send against the freeze
bool             g_hb_cs_ready = false;
// U19d: GetTickCount() at the last REAL fast-drop broadcast just below (0 = never yet this process).
// Declared here, ahead of on_time_tick, because that is where it is SET; on_gameover_post (further
// down, by the block comment at OUTCOME_NETWORK_ERROR) is where it is READ.
DWORD g_last_fastdrop_tick = 0;

// Optional per-frame lockstep timing log (mh_net.ini [net] lockstep_log=1 -> mh_lockstep.log). One
// line per strategic frame while in mode-3, so freezes show up as large wall-time gaps between rows
// and we can see WHETHER the sim is starved by the peer horizon, the pump cadence, or rx delivery.
// (The g_ls_log gate itself lives in net_internal.h -- net_seams + net_diag read it for DIAG gating.)
HANDLE        g_ls_h = INVALID_HANDLE_VALUE;
char          g_ls_path[MAX_PATH];
unsigned long g_ls_gen = 0; // SES1: the run-directory generation g_ls_path was composed for

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
bool          g_ft_log = false;
HANDLE        g_ft_h   = INVALID_HANDLE_VALUE;
char          g_ft_path[MAX_PATH];
unsigned long g_ft_gen = 0; // SES1: the run-directory generation g_ft_path was composed for
// g_qpc_freq -> net_internal.h (shared: frametime log here + net_diag.cpp's temporal trace)
void *g_ft_tramp = nullptr;

// mp:SES5 decision (4) -- this log used to be ONE ROW PER PRESENT (measured 23 MB in a 43-minute
// match). Diet: log a present's row only when its interval exceeds 2x the PREVIOUS 1-second
// window's median (a hitch worth seeing), plus one aggregate line per second (min/avg/p95/max over
// EVERY present in that window, hitches included) so the pacing shape survives even when no single
// frame trips the outlier test. The header + per-present row FORMAT are unchanged (backward
// compatible with any reader that only knows the 2-column "qpc_us game_mode" shape); the aggregate
// line is its own new, `#`-prefixed format (see log_formats.json `frametime.summary`), which an
// unaware reader already skips as a comment, same as the header.
long long g_ft_prev_us = -1; // previous present's qpc_us (-1 = no previous present yet)
// A headless solo lane has been measured at ~1876 fps, well above any small fixed array -- so the
// array is a SAMPLE for the percentile estimate only, capped, while `g_ft_win_true_n`/
// `g_ft_win_sum_ms` are UNCAPPED and drive `n`/`avg_ms` exactly regardless of how many presents
// this window actually saw (a capped `n` would have understated `avg_ms` by dividing the TRUE sum
// by a truncated count).
double              g_ft_win_ms[512];
int                 g_ft_win_n          = 0; // count of samples actually stored (<= array size)
int                 g_ft_win_true_n     = 0; // TRUE present count this window (uncapped)
double              g_ft_win_sum_ms     = 0.0;
long long           g_ft_win_start_us   = -1;      // qpc_us this window opened at
double              g_ft_last_median_ms = 0.0;     // previous COMPLETED window's median -- this window's outlier threshold
constexpr long long FT_WINDOW_US        = 1000000; // 1 Hz

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
    MH_MP_DrainRelayNotice();      // mp:R4a: a relay-level notice the UDP module queued -> arm it (main thread)
    mh::ui::browser_notice_tick(); // U23: keep the involuntary-exit notice on the browser status line
    mh::ui::slide_geom_watch();    // U37 diag ([net] slide_diag): log any change to the menu frame geometry
    MH_FontGuard_OnPresent();      // F2: [fonts] probe_text through the game's own font path, before overlay+capture
    MH_NetIndicator_OnPresent();   // mp:L1: the player-visible net indicator, through the GAME font path
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
    if (g_eager_adv && *(const uint8_t *)ADDR_SESSION_MODE == 3 && !g_leave_frozen && // U19b: frozen = silent
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
    // SES1: per-SESSION, handle swapped on a boundary so the header lands in each file (see ls_log_tick).
    if (mh_run_path(g_ft_path, MAX_PATH, "%smh_frametime.log", &g_ft_gen) && g_ft_h != INVALID_HANDLE_VALUE) {
        CloseHandle(g_ft_h);
        g_ft_h = INVALID_HANDLE_VALUE;
    }
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
        // SES5: a new file is a fresh window/outlier baseline -- don't carry the file that just
        // closed's present timing forward into this one.
        g_ft_prev_us        = -1;
        g_ft_win_n          = 0;
        g_ft_win_true_n     = 0;
        g_ft_win_sum_ms     = 0.0;
        g_ft_win_start_us   = -1;
        g_ft_last_median_ms = 0.0;
    }
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    // microseconds since an arbitrary origin (analyzer only uses deltas)
    long long us = g_qpc_freq.QuadPart ? (t.QuadPart * 1000000LL) / g_qpc_freq.QuadPart : t.QuadPart;
    DWORD     w;
    if (g_ft_prev_us < 0) {
        // First present of this file: no interval yet to judge or window. Log it unconditionally --
        // it anchors find_lobby_clip_qpc's qpc_us/wall-clock join (mp_analyze.py), which needs row 0.
        char line[64];
        int  n = wsprintfA(line, "%I64d %d\n", us, (int)*(const uint8_t *)ADDR_GAME_MODE);
        WriteFile(g_ft_h, line, n, &w, nullptr);
        g_ft_prev_us      = us;
        g_ft_win_start_us = us;
        return;
    }
    double interval_ms = (double)(us - g_ft_prev_us) / 1000.0;
    g_ft_prev_us       = us;
    if (interval_ms >= 0.0 && interval_ms < 60000.0) { // guard a clock reset/QPC glitch, same bound read_frametimes uses
        if (g_ft_win_n < (int)(sizeof(g_ft_win_ms) / sizeof(g_ft_win_ms[0]))) {
            g_ft_win_ms[g_ft_win_n++] = interval_ms;
        }
        ++g_ft_win_true_n;
        g_ft_win_sum_ms += interval_ms;
        // Outlier row: this present's own interval is far more than the previous window's typical
        // frame, i.e. a hitch worth naming on its own. Appended as a THIRD column (interval_us) so
        // the hitch's duration survives even though its neighbours are no longer logged -- an old
        // 2-column-only reader still parses qpc_us/game_mode fine and just ignores the extra field.
        if (interval_ms > 2.0 * g_ft_last_median_ms) {
            char line[80];
            int  n = wsprintfA(line, "%I64d %d %I64d\n", us, (int)*(const uint8_t *)ADDR_GAME_MODE,
                               (long long)(interval_ms * 1000.0 + 0.5));
            WriteFile(g_ft_h, line, n, &w, nullptr);
        }
    }
    if (us - g_ft_win_start_us >= FT_WINDOW_US && g_ft_win_true_n > 0) {
        // 1 Hz aggregate: `n`/`avg_ms` are exact over the TRUE (uncapped) present count this window
        // -- a headless solo lane has been measured at ~1876 fps, well past the fixed sample array
        // below, and dividing the true sum by a truncated count would have understated avg_ms.
        // min/p95/max come from the array, a SAMPLE (the first up-to-512 presents this window) --
        // exact when true_n fits, a reasonable estimate otherwise.
        double sorted[sizeof(g_ft_win_ms) / sizeof(g_ft_win_ms[0])];
        memcpy(sorted, g_ft_win_ms, sizeof(double) * (size_t)g_ft_win_n);
        for (int i = 1; i < g_ft_win_n; ++i) { // small sample (<=512) -- insertion sort is plenty
            double v = sorted[i];
            int    j = i - 1;
            while (j >= 0 && sorted[j] > v) {
                sorted[j + 1] = sorted[j];
                --j;
            }
            sorted[j + 1] = v;
        }
        double min_ms = sorted[0], max_ms = sorted[g_ft_win_n - 1];
        double avg_ms = g_ft_win_sum_ms / g_ft_win_true_n;
        double median = sorted[g_ft_win_n / 2];
        int    p95_i  = (int)(0.95 * (g_ft_win_n - 1));
        double p95_ms = sorted[p95_i];
        // wsprintfA has no float conversion -- round to whole milliseconds (the per-present outlier
        // row above still carries the exact interval_us for anything needing sub-ms precision).
        char agg[128];
        int  an = wsprintfA(agg, "# 1s qpc_us=%I64d n=%d min_ms=%d avg_ms=%d p95_ms=%d max_ms=%d\n",
                            us, g_ft_win_true_n, (int)(min_ms + 0.5), (int)(avg_ms + 0.5),
                            (int)(p95_ms + 0.5), (int)(max_ms + 0.5));
        WriteFile(g_ft_h, agg, an, &w, nullptr);
        g_ft_last_median_ms = median;
        g_ft_win_n          = 0;
        g_ft_win_true_n     = 0;
        g_ft_win_sum_ms     = 0.0;
        g_ft_win_start_us   = us;
    }
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

// mp:SES5 decision (2) -- ls_log_tick used to write one row per strategic frame (measured 215 MB /
// 495 rows/s in a 43-minute match). Write on CHANGE of any non-counter column instead (the values
// that carry the story: sess/game/flags/syncwait/countdn/pcount/p54bc/step), or every 500 ms
// regardless -- so a genuine freeze (rx frozen, since_rx climbing, nothing else changing) still
// produces a row every 500 ms (2 rows/s) rather than going silent. wall_ms/clock_ms/tx_pkts/rx_pkts/
// since_rx_ms etc. are COUNTERS that move every frame by construction and are deliberately excluded
// from the change test -- gating on them would defeat the diet entirely.
DWORD           g_ls_last_write_t      = 0;
bool            g_ls_have_last         = false;
int             g_ls_last_sess         = -1;
int             g_ls_last_game         = -1;
unsigned        g_ls_last_flags        = 0;
int             g_ls_last_syncwait     = -1;
int             g_ls_last_countdn      = -1;
int             g_ls_last_pcount       = -1;
int             g_ls_last_p54bc        = -1;
long            g_ls_last_step_ms      = -1;
constexpr DWORD LS_ROW_MAX_INTERVAL_MS = 500;

// ==== mp:T3 -- ARRIVAL LATENESS, AND THE COLUMNS THAT CARRY IT ===================================
//
// WHY THIS HALF IS HERE AND NOT IN THE TRANSPORT. SRTT/RTTVAR/IPDV/loss are properties of the LINK
// and the transport measures them (udp_stats.h, fed from mh_net_udp's channel B). Arrival lateness
// is not a link property at all: it is "did peer P's horizon cover the sub-step the sim wanted,
// and by how much" -- a question about the LOCKSTEP PROTOCOL's own state, answerable only where the
// peer-horizon array and the sim clock are both in view, which is this file. A transport that
// measured it would be measuring a game it does not know it is carrying.
//
// THE SIGN, and it is the project's agreed one for this quantity: POSITIVE = ms of margin before
// the deadline, NEGATIVE = ms the sim sat blocked waiting. Two sample kinds, and the asymmetry is
// deliberate:
//
//   a POSITIVE sample, once per sim-clock advance per live peer: (peer_horizon - (clock + sim_step))
//     -- the margin the peer's last advertisement actually left. Taken only when the clock moved, so
//     the sample rate is the SIM's, not the frame rate's: a 200 fps menu-grade frame loop would
//     otherwise flood the window with copies of one advertisement and make a spiky link look calm.
//
//   a NEGATIVE sample, once per blocked EPISODE, attributed to the peer that bound COMMITTED: the
//     wall ms between the sim first being unable to fund a sub-step and the peer's EXTEND landing.
//     Attribution uses COMMITTED (the value the sim actually clamps to) rather than a per-peer
//     comparison, because COMMITTED = min(local, peers) is the condition that really blocks, and the
//     argmin peer is the one that really held it.
//
// A block that outlasts LATE_STALL_SPLIT_MS emits an interim sample and re-arms, so a peer that
// stalls for ten seconds reaches the controller during the stall instead of only on recovery -- the
// old starved-fraction proxy's one genuine advantage, kept.
//
// The percentile reduction is refreshed on a timer, not per frame: one sort of <=256 ints every
// LATE_SNAP_MS is free, and per frame it would be the most expensive thing in the log path.
//
// WHICH SLOTS ARE PEERS -- measured, not assumed, and this cost a rig run to learn. The first build
// read a slot as live if its horizon was non-zero and not our own. On a 2-player match that admitted
// SLOTS 2..7 as well, because retail initialises every PEER_HORIZON entry to the stock lookahead of
// 10.0 GAME-SECONDS (_G_LLM_STRAT_LOCKSTEP_STEP_SIZE's retail default) and leaves an
// unused one there forever. For the first ten seconds of a match that is a huge POSITIVE margin and
// looks merely odd; past ten seconds it goes negative without bound, becomes the smallest horizon in
// the array, and is therefore always the "binding peer". Measured on the 2026-09-18 clean-LAN UDP
// run: at game clock 27.7 s the controller was reading `peer=2 tail95 -1439 ms` off an empty slot and
// had saddled the lookahead at the 400 ms ceiling on a sub-millisecond LAN.
//
// The test that actually distinguishes them is MOVEMENT. An unused slot never changes; a real peer's
// horizon advances every time it advertises. So a slot becomes live when its value first changes,
// and stops being live after LATE_LIVE_MS of no change -- which also retires a peer that has died or
// left, instead of letting its frozen horizon pin the lookahead at the ceiling for the rest of the
// match. Deliberately NOT MH_Net_ActivePeerIds: that answers in TRANSPORT ids, and a client's single
// conn to the host carries player_id -1 (the declared-id convention, mh_net_export.h), so the client
// -- the peer that most needs this -- would see an empty set.
constexpr int   LS_LATE_PEERS       = 8;    // the retail PEER_HORIZON array's width
constexpr DWORD LATE_SNAP_MS        = 250;  // how often the published percentiles are recomputed
constexpr DWORD LATE_STALL_SPLIT_MS = 2000; // a block longer than this reports in, then re-arms
constexpr DWORD LATE_LIVE_MS        = 5000; // a horizon unchanged this long is not a peer advertising

mh::netstats::LatenessWindow g_late_w[LS_LATE_PEERS];
DWORD                        g_late_blocked[LS_LATE_PEERS]   = {0};
double                       g_late_last_h[LS_LATE_PEERS]    = {0.0};
DWORD                        g_late_last_move[LS_LATE_PEERS] = {0};
long                         g_late_prev_clk                 = -1;
DWORD                        g_late_snap_t                   = 0;
// The published reduction -- read by ls_log_tick's columns, the net.late overlay provider and the
// adaptive controller. `g_late_have` is false until some peer has AD_LATE_MIN_SAMPLES samples, and
// every reader renders that as `n/a` rather than as a zero.
bool g_late_have   = false;
int  g_late_p50    = 0;
int  g_late_tail95 = 0;
int  g_late_tail99 = 0;
int  g_late_n      = 0;
int  g_late_peer   = -1;

// mp:GS2 -- game-level peer-data timeout. [net] data_timeout_ms; <= 0 disables. See data_timeout_tick
// (below lateness_tick) for why g_late_last_move[] -- already maintained above for the lookahead
// controller -- is the right signal: it is per-PEER (unlike MH_NetStats.last_rx_tick, one scalar for
// the whole transport, mh_net_export.h:150), it needs no lockstep promotion (lateness_tick reads
// ADDR_PEER_HORIZON directly, so this runs in CONFIGURATION (1) too -- exactly where
// gone_peer_frame_guard is INERT, mp:GS1's scope), and it only moves when the peer's SIM actually
// advances its horizon -- a keepalive-only link (the field's 22-35 s since_rx freezes, GS1/RM1)
// leaves it frozen while the transport stays "up".
int  g_data_timeout_ms            = SHIP_DATA_TIMEOUT_MS;
bool g_gs2_dropped[LS_LATE_PEERS] = {false}; // latched per slot per match -- never re-fire on an already-dropped side

// mp:T3c -- UNUSED HORIZON, the second surplus signal. Same window machinery, one sample per
// strategic frame: local_h - COMMITTED, floored at zero. It is 0 for whichever peer's own horizon is
// the binding one and positive for the peer that is being clamped by the other side, which is
// exactly the case the arrival-lateness tail is blind to (udp_stats.h's AD_SLACK_MULT note has the
// measurement). The MEDIAN is published rather than a tail because the quantity sawtooths by
// construction -- COMMITTED steps up each time the peer's EXTEND lands and then sits while our own
// horizon crawls -- so its minimum is near zero every window and says nothing about the surplus.
mh::netstats::LatenessWindow g_slack_w;
int                          g_slack_p50 = 0;

// mp:T3c -- the join warm-up's clock. Armed by adaptive_tick the first time a peer's measurement
// exists at all, cleared by the match reset just above (a new match is a new join). It lives here
// rather than with the other g_ad_* state because THIS is the function that knows a match restarted.
DWORD g_ad_warm_t0 = 0;

// A latency column is either a number or the literal token `n/a`. It is never a zero standing in for
// "unmeasured": the TCP module cannot measure a round trip at all, and a 0 ms SRTT in a log would be
// read by every later reader as a perfect link. (dead-ends: the same shape as G198's silent nan.)
void lat_int_col(char *b, bool have, long v) {
    if (have) wsprintfA(b, "%ld", v);
    else lstrcpyA(b, "n/a");
}

void lat_us_col(char *b, bool have, int us) {
    if (have) wsprintfA(b, "%ld", (long)((us + 500) / 1000));
    else lstrcpyA(b, "n/a");
}

// A DECISION CONSUMES THE SAMPLES IT WAS MADE ON, and this is not an optimisation -- it is the
// difference between a controller and a ratchet. Measured 2026-09-18 on the rig, at 80 ms RTT with
// the shipping 100 ms lookahead: a joining client is genuinely starved for its first half-second
// (the host has not advertised yet), so the first window reads tail95 -31 ms and the controller
// GROWS, correctly. The growth worked -- the very next window recorded zero starved time. But the
// window is a rolling ring of the last 256 samples, so the join burst was still 20% of it, still
// dragging the 5th percentile below zero, and the fast path re-read those same dead samples every
// 500 ms: 100 -> 141 -> 182 -> 227 -> 284 -> 355 -> 400 ms in two and a half seconds, five of the six
// steps answering a problem that had already been fixed. Then, because shrinking is 4% per window by
// design, it took ninety-five seconds to give the overshoot back -- on a LAN.
//
// Clearing after each decision makes every tail a statement about the regime SINCE the last move,
// which is the only thing a control loop can act on. The two sample-count gates
// (mh::netstats::AD_LATE_MIN_SAMPLES to decide at all, AD_FAST_MIN_SAMPLES to decide EARLY) are what
// stop the emptied window from producing a confident number off three samples.
//
// Only while the controller is running: with `lockstep_adaptive=0` nothing consumes the samples, and
// the log columns are then a rolling 256-sample view, which is the more useful thing for a pinned
// run being measured rather than tuned.
void lateness_snapshot();

void lateness_consume() {
    for (int i = 0; i < LS_LATE_PEERS; ++i) g_late_w[i].reset();
    g_slack_w.reset(); // mp:T3c: the unused-horizon window is consumed with the rest, same reason
    lateness_snapshot();
}

void lateness_snapshot() {
    // The BINDING peer is the one whose pessimistic tail is worst; that is the peer the lookahead
    // has to cover, and covering the average peer instead is how a two-client game ends up tuned for
    // the good link and stalling on the bad one.
    int best_peer = -1, best_t95 = 0, best_p50 = 0, best_t99 = 0, best_n = 0;
    for (int i = 0; i < LS_LATE_PEERS; ++i) {
        if (g_late_w[i].count() < mh::netstats::AD_LATE_MIN_SAMPLES) continue;
        int p50 = 0, t95 = 0, t99 = 0;
        if (!g_late_w[i].percentiles(p50, t95, t99)) continue;
        if (best_peer < 0 || t95 < best_t95) {
            best_peer = i;
            best_p50  = p50;
            best_t95  = t95;
            best_t99  = t99;
            best_n    = g_late_w[i].count();
        }
    }
    g_late_have   = (best_peer >= 0);
    g_late_peer   = best_peer;
    g_late_p50    = best_p50;
    g_late_tail95 = best_t95;
    g_late_tail99 = best_t99;
    g_late_n      = best_n;
    // mp:T3c. Same minimum weight as a lateness tail: a median off three frames is not a median.
    int sp50 = 0, s95 = 0, s99 = 0;
    g_slack_p50 = (g_slack_w.count() >= mh::netstats::AD_LATE_MIN_SAMPLES &&
                   g_slack_w.percentiles(sp50, s95, s99))
                      ? sp50
                      : 0;
}

// mp:L1 -- THE ADDITIVE GETTER, and it is additive on purpose: the adaptive controller and its
// signal belong to mp:T3/T3c, so the player-visible indicator (seams/ui_net_indicator.cpp) reads
// this state through an accessor rather than growing a second copy of the argmin inside another TU.
// It computes nothing and writes nothing: `g_late_blocked[i]` is already exactly "the tick at which
// the sim became blocked on peer i, 0 = not blocked", maintained by lateness_tick below, and only
// one slot can be non-zero at a time (the block is charged to the peer that bound COMMITTED).
// Returns that peer's PEER_HORIZON / strategic-player slot and how long the block has lasted, or
// -1 when the sim is not blocked at all.
extern "C" int MH_Lockstep_StallBindingPeer(unsigned long *blocked_ms) {
    for (int i = 0; i < LS_LATE_PEERS; ++i) {
        if (g_late_blocked[i] == 0) continue;
        if (blocked_ms) *blocked_ms = (unsigned long)(GetTickCount() - g_late_blocked[i]);
        return i;
    }
    if (blocked_ms) *blocked_ms = 0;
    return -1;
}

// Main thread, from on_time_tick, BEFORE adaptive_tick reads the snapshot. Runs whether or not the
// adaptive controller is on, because the log columns and the overlay want the measurement either
// way -- a pinned-lookahead run that shows a healthy tail is evidence about the pin.
void lateness_tick() {
    if (*(const uint8_t *)ADDR_SESSION_MODE != 3) return;
    if (!MH_Net_IsStarted() || MH_Net_PeerCount() <= 0) return;
    double clk, com, sim;
    memcpy(&clk, (const void *)ADDR_GAME_CLOCK, sizeof(double));
    memcpy(&com, (const void *)ADDR_COMMITTED(), sizeof(double));
    memcpy(&sim, (const void *)ADDR_SIM_STEP_INT(), sizeof(double));
    if (sim <= 0.0) return;
    const DWORD  now      = GetTickCount();
    const double required = clk + sim;
    const long   clk_ms   = (long)(clk * 1000.0 + 0.5);
    // A clock that went BACKWARDS is a new match (or a resync), not a frame: everything measured
    // about the previous one is about a different link state and a different peer set.
    if (g_late_prev_clk >= 0 && clk_ms < g_late_prev_clk) {
        for (int i = 0; i < LS_LATE_PEERS; ++i) {
            g_late_w[i].reset();
            g_late_blocked[i]   = 0;
            g_late_last_h[i]    = 0.0;
            g_late_last_move[i] = 0;
            g_gs2_dropped[i]    = false; // mp:GS2 -- a new match is a new peer set, not a residue of the last one
        }
        g_late_have = false;
        g_late_peer = -1;
        g_late_n    = 0;
        g_slack_w.reset();
        g_slack_p50  = 0;
        g_ad_warm_t0 = 0; // mp:T3c: a new match is a new join, so the warm-up runs again
    }
    const bool advanced = (g_late_prev_clk >= 0 && clk_ms != g_late_prev_clk);
    g_late_prev_clk     = clk_ms;

    const int me = MH_Net_LocalPlayerId();
    double    h[LS_LATE_PEERS];
    bool      live[LS_LATE_PEERS];
    int       bind = -1;
    for (int i = 0; i < LS_LATE_PEERS; ++i) {
        memcpy(&h[i], (const void *)(ADDR_PEER_HORIZON() + (unsigned)i * 8u), sizeof(double));
        if (h[i] != g_late_last_h[i]) {
            g_late_last_h[i]    = h[i];
            g_late_last_move[i] = now ? now : 1;
        }
        // See the LATE_LIVE_MS note above: MOVEMENT is what tells a peer from an empty slot holding
        // retail's 10-second sentinel, and it retires a departed peer for free.
        live[i] = (i != me) && (h[i] > 0.0) && g_late_last_move[i] != 0 &&
                  (now - g_late_last_move[i]) <= LATE_LIVE_MS;
        if (live[i] && (bind < 0 || h[i] < h[bind])) bind = i;
        // mp:SES6 -- push this slot's CURRENT horizon down to the transport, whether or not it is
        // "live" by the LATE_LIVE_MS test above: that test exists to pick the adaptive controller's
        // binding peer, and a horizon that stopped moving 5+ seconds ago (exactly the suspended-sim
        // shape this item exists to surface) is the case the counters line needs to keep printing a
        // FROZEN value for, not the case to stop reporting. `i != me` alone is the right gate here --
        // `h[i] > 0.0` would also suppress the honest "the peer's own horizon fell to zero or below"
        // reading, which is itself informative. Same value the adaptive controller already computed
        // this tick, so this adds no new read of ADDR_PEER_HORIZON.
        if (i != me) MH_Net_SetPeerHorizon(i, (int)(h[i] * 1000.0));
    }

    if (com < required) { // horizon cannot fund the next sub-step: we are blocked, on `bind`
        if (bind >= 0) {
            if (g_late_blocked[bind] == 0) g_late_blocked[bind] = now ? now : 1;
            else if (now - g_late_blocked[bind] >= LATE_STALL_SPLIT_MS) {
                g_late_w[bind].push(-(int)(now - g_late_blocked[bind]));
                g_late_blocked[bind] = now ? now : 1;
            }
        }
    } else {
        for (int i = 0; i < LS_LATE_PEERS; ++i) {
            if (!live[i]) {
                g_late_blocked[i] = 0;
                continue;
            }
            if (g_late_blocked[i] != 0) { // the episode ended: charge it, and skip this frame's margin
                g_late_w[i].push(-(int)(now - g_late_blocked[i]));
                g_late_blocked[i] = 0;
            } else if (advanced) {
                // Game milliseconds. At the shipping 100% game speed that is wall milliseconds; at
                // any other speed it is still the right unit, because the deadline it is measured
                // against is a game-clock deadline too.
                g_late_w[i].push((int)((h[i] - required) * 1000.0));
            }
        }
    }
    // mp:T3c -- one unused-horizon sample per advancing frame, on the same cadence and in the same
    // game milliseconds as the lateness samples above. `advanced` gates it for the same reason: a
    // frame the sim did not move on re-measures the same instant and would weight it twice.
    if (advanced) {
        double lh;
        memcpy(&lh, (const void *)ADDR_LOCAL_HORIZON, sizeof(double));
        if (lh > 0.0 && com > 0.0 && bind >= 0) {
            const int slack = (int)((lh - com) * 1000.0);
            g_slack_w.push(slack > 0 ? slack : 0);
        }
    }
    if (now - g_late_snap_t >= LATE_SNAP_MS) {
        g_late_snap_t = now;
        lateness_snapshot();
    }
}

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
    // SES1: per-SESSION. The path is re-resolved here rather than at arm time, and a rebuild closes
    // the open handle so the header is rewritten into the new file -- a session's mh_lockstep.log is
    // then self-describing, which is what tools/mp_analyze.py's column parse needs.
    if (mh_run_path(g_ls_path, MAX_PATH, "%smh_lockstep.log", &g_ls_gen) &&
        g_ls_h != INVALID_HANDLE_VALUE) {
        CloseHandle(g_ls_h);
        g_ls_h = INVALID_HANDLE_VALUE;
    }
    if (g_ls_h == INVALID_HANDLE_VALUE) {
        g_ls_h = CreateFileA(g_ls_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (g_ls_h == INVALID_HANDLE_VALUE) {
            g_ls_log = false;
            return;
        }
        // mp:T3 APPENDED 13 COLUMNS AFTER icon_shown, and the append is the contract: this file's
        // two readers are header-driven (mp_pacing_report.read_lockstep) and
        // positional-with-an-optional-tail (mp_analyze.parse_lockstep), so a column added at the END
        // is read by both and an OLDER log stays readable by today's tools. Never insert in the
        // middle.
        //   srtt/rttvar/ipdv/loss  the TRANSPORT's per-peer measurement (MH_NetStats::lat[]), by
        //                          TRANSPORT peer slot -- which is not necessarily the lockstep
        //                          horizon slot peer0_ms/peer1_ms use, and on a 2-player game there
        //                          is exactly one peer so the question does not arise. `n/a` means
        //                          the bound transport does not measure its link (the TCP module has
        //                          no channel B), which is a different claim from 0.
        //   late_*                 the LOCKSTEP's arrival-lateness reduction in L0's sign: POSITIVE
        //                          = ms of margin before the deadline. tail95/tail99 are the 5th/1st
        //                          percentiles -- the PESSIMISTIC tail, which is the LOW end under
        //                          that sign (udp_stats.h's LatenessWindow explains the naming).
        //   late_peer              the horizon slot whose tail bound the decision; -1 = none yet.
        const char *hdr = "# wall_ms clock_ms total_ms local_h_ms committed_ms peer0_ms peer1_ms "
                          "step_ms stall pcount tx_pkts rx_pkts since_rx_ms "
                          "sess game flags grace_ms syncwait countdn p54bc sync_ms sim_burst "
                          "icon_calls icon_shown " // P4: CUMULATIVE -- diff two rows for a rate
                          "srtt0_ms srtt1_ms rttvar0_ms rttvar1_ms ipdv0_ms ipdv1_ms "
                          "loss0_pm loss1_pm "
                          "late_p50_ms late_tail95_ms late_tail99_ms late_n late_peer\n";
        DWORD w;
        WriteFile(g_ls_h, hdr, lstrlenA(hdr), &w, nullptr);
        g_ls_prev_clock_ms = -1;    // fresh file -> first row's burst is a baseline (0)
        g_ls_have_last     = false; // SES5: a new file starts a fresh change-detection baseline too
    }
    // mp:SES5 decision (2) -- the row-gate: write only on a change of one of the 8 non-counter
    // columns below, or every LS_ROW_MAX_INTERVAL_MS regardless (so a frozen match with nothing
    // changing still produces a row every 500 ms -- 2 rows/s -- and the freeze shape (rx frozen,
    // since_rx climbing) stays visible). wall_ms/clock_ms/tx_pkts/rx_pkts/since_rx_ms/sim_burst/
    // icon_* are COUNTERS, deliberately excluded from the change test.
    int      cur_game     = (int)*(const uint8_t *)ADDR_GAME_MODE;
    unsigned cur_flags    = (unsigned)*(const uint8_t *)ADDR_STATUS_FLAGS;
    int      cur_syncwait = *(const int *)ADDR_SYNC_WAIT();
    int      cur_countdn  = *(const int *)ADDR_SYNC_COUNTDN;
    int      cur_pcount   = *(const int *)ADDR_PLAYER_COUNT;
    int      cur_p54bc    = *(const int *)ADDR_PLAYERCT_54BC;
    long     cur_step_ms  = ms_of(ADDR_STEP_SIZE);
    DWORD    gate_now     = GetTickCount();
    bool     changed      = !g_ls_have_last || sess != g_ls_last_sess || cur_game != g_ls_last_game ||
                   cur_flags != g_ls_last_flags || cur_syncwait != g_ls_last_syncwait ||
                   cur_countdn != g_ls_last_countdn || cur_pcount != g_ls_last_pcount ||
                   cur_p54bc != g_ls_last_p54bc || cur_step_ms != g_ls_last_step_ms;
    bool due = !g_ls_have_last || (gate_now - g_ls_last_write_t) >= LS_ROW_MAX_INTERVAL_MS;
    if (!changed && !due) return; // this frame's row is redundant with the last written one
    g_ls_have_last     = true;
    g_ls_last_write_t  = gate_now;
    g_ls_last_sess     = sess;
    g_ls_last_game     = cur_game;
    g_ls_last_flags    = cur_flags;
    g_ls_last_syncwait = cur_syncwait;
    g_ls_last_countdn  = cur_countdn;
    g_ls_last_pcount   = cur_pcount;
    g_ls_last_p54bc    = cur_p54bc;
    g_ls_last_step_ms  = cur_step_ms;
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
    // mp:T3 -- the eight transport columns are rendered BEFORE the row, so an unmeasurable one can
    // be the literal `n/a` rather than a zero that reads as a perfect link.
    char srtt[2][16], rttvar[2][16], ipdv[2][16], loss[2][16];
    for (int pi = 0; pi < 2; ++pi) {
        const bool slot = (s.lat_supported != 0) && pi < s.lat_count;
        const bool meas = slot && s.lat[pi].samples > 0;
        lat_us_col(srtt[pi], meas, slot ? s.lat[pi].srtt_us : 0);
        lat_us_col(rttvar[pi], meas, slot ? s.lat[pi].rttvar_us : 0);
        lat_us_col(ipdv[pi], meas, slot ? s.lat[pi].ipdv_us : 0);
        lat_int_col(loss[pi], slot && s.lat[pi].loss_pm >= 0, slot ? s.lat[pi].loss_pm : 0);
    }
    char late50[16], late95[16], late99[16];
    lat_int_col(late50, g_late_have, g_late_p50);
    lat_int_col(late95, g_late_have, g_late_tail95);
    lat_int_col(late99, g_late_have, g_late_tail99);
    char  line[560];
    int   n = wsprintfA(line, "%lu %ld %ld %ld %ld %ld %ld %ld %d %d %ld %ld %lu "
                                "%d %d 0x%02x %ld %d %d %d %ld %ld %ld %ld "
                                "%s %s %s %s %s %s %s %s %s %s %s %d %d\n",
                        now, clock_ms, ms_of(ADDR_TOTAL_TIME), ms_of(ADDR_LOCAL_HORIZON), ms_of(ADDR_COMMITTED()),
                        ms_of(ADDR_PEER_HORIZON() + 0 * 8), ms_of(ADDR_PEER_HORIZON() + 1 * 8), cur_step_ms,
                        *(const int *)ADDR_STALL_COUNT, cur_pcount,
                        s.tx_pkts, s.rx_pkts, s.last_rx_tick ? (unsigned)(now - s.last_rx_tick) : 0u,
                        sess, cur_game, cur_flags,
                        ms_of(ADDR_GRACE_TIMER()), cur_syncwait, cur_countdn,
                        cur_p54bc, ms_of(ADDR_SYNC_ACCUM()), sim_burst,
                        g_icon_calls, g_icon_shown,
                        srtt[0], srtt[1], rttvar[0], rttvar[1], ipdv[0], ipdv[1], loss[0], loss[1],
                        late50, late95, late99, g_late_n, g_late_peer);
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

// mp:T3 -- three latency readouts, and there are three rather than one for the reason the Age of
// Empires netcode write-up gives: players tolerate a high STEADY delay far better than a lower
// delay that varies, so a link can have a low, steady ping and still feel bad. A single blended
// number would hide exactly the thing that matters. `net.srtt` is the recognisable ping (with its RFC 6298
// deviation term beside it, which is the "is it steady" half); `net.loss` and `net.late` are the
// two that a ping cannot say anything about. All three print `n/a` rather than 0 when nothing
// measured them -- over the TCP module the first two always do, because it has no channel B.
void ovp_srtt(char *b, int cap) {
    (void)cap;
    MH_NetStats s;
    MH_Net_GetStats(&s);
    const bool meas = s.lat_supported && s.lat_count > 0 && s.lat[0].samples > 0;
    if (!meas) {
        lstrcpyA(b, "n/a");
        return;
    }
    wsprintfA(b, "%ld +/-%ld", (long)((s.lat[0].srtt_us + 500) / 1000),
              (long)((s.lat[0].rttvar_us + 500) / 1000));
}

void ovp_loss(char *b, int cap) {
    (void)cap;
    MH_NetStats s;
    MH_Net_GetStats(&s);
    char c0[16];
    lat_int_col(c0, s.lat_supported && s.lat_count > 0 && s.lat[0].loss_pm >= 0,
                (s.lat_supported && s.lat_count > 0) ? s.lat[0].loss_pm : 0);
    wsprintfA(b, "%s pm", c0); // per mille, so a 0.3% loss reads as `3 pm` rather than rounding to 0%
}

// The arrival-lateness tail the adaptive controller is steering on, and the peer it named.
void ovp_late(char *b, int cap) {
    (void)cap;
    if (!g_late_have) {
        lstrcpyA(b, "n/a");
        return;
    }
    wsprintfA(b, "%d/%d/%d p%d n%d", g_late_p50, g_late_tail95, g_late_tail99, g_late_peer, g_late_n);
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
    MH_Overlay_RegisterProvider("net.srtt", ovp_srtt);           // mp:T3 peer0 SRTT +/- RTTVAR, ms
    MH_Overlay_RegisterProvider("net.loss", ovp_loss);           // mp:T3 peer0 loss, per mille
    MH_Overlay_RegisterProvider("net.late", ovp_late);           // mp:T3 lateness p50/tail95/tail99
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

// ==== Adaptive lookahead controller (P1, re-signalled at mp:T3) =================================
// Auto-tunes the lookahead (== input latency) to the lowest value this link sustains at ~1.0x. It
// reuses the STOCK grow/shrink scaffold's shape but NOT its signal: retail scales STEP_SIZE by FPS
// and a stall-count-vs-player-count heuristic, which ratchets to multi-second lag after an alt-tab
// FPS crash -- that is why we pin it off.
//
// SIGNAL, since mp:T3: the BINDING PEER'S ARRIVAL-LATENESS TAIL (lateness_tick, above) -- the
// 95th-percentile-worst margin, in ms, between a peer's advertised horizon and the deadline the sim
// needed it by. What it replaced was a LOCAL PROXY: the fraction of a 2 s window during which
// COMMITTED could not fund the next sub-step. The proxy was not wrong, it was blunt in three ways
// that each cost a measurable amount:
//   * it was a BOOLEAN per frame, so it could say the horizon ran out but never by HOW MUCH -- the
//     controller then had to guess its step size (mp:P1 fix (d) is the scar: a flat +25% took four
//     windows to climb the 100 ms a 200 ms link was short by, starving the player for all eight
//     seconds). The tail is in milliseconds, so the growth step is simply the missing margin.
//   * it was PEER-BLIND. COMMITTED is a minimum, so "we were starved" names no peer; in a 3-player
//     game the lookahead was tuned by whoever happened to bind, with no record of who. The tail is
//     per peer, and the decision names the peer it came from (`late_peer`).
//   * it could only fire AFTER the horizon had already run out. Lateness is measured while the
//     margin is still positive, so the controller can grow before the first stall rather than in
//     response to it.
// The starved fraction is still accumulated and still printed on every move, as a diagnostic that
// keeps new runs comparable with every earlier mp:P1/mp:P5 pacing measurement, all of which are
// expressed in it.
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
// between the two thresholds is the hysteresis band. Both thresholds, and the decision itself, live
// in mh::netstats (udp_stats.h) so that the asymmetry is an offline assertion rather than a claim
// about code only a two-VM rig can reach.
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
constexpr DWORD AD_WINDOW_MS = 2000; // decide at most once per window...
constexpr DWORD AD_FAST_MS   = 500;  // ...except to GROW, which may decide this early
// ...and only off a tail with some weight behind it. mh::netstats::AD_LATE_MIN_SAMPLES (8) is the
// floor for a tail to exist at all; the SHORTCUT needs more, because the moment it would otherwise
// fire hardest is the join transient -- the first half-second in lockstep, when the peer has not
// advertised yet and every sample is legitimately terrible. Measured 2026-09-18 on a clean LAN: the
// client grew 100 -> 400 ms in 2.5 s off a first window of 8 samples, then spent 95 s giving it back.
// At ~50 samples/s (one per sim sub-step) 32 samples is ~0.6 s, so the 200 ms clause still has three
// times the headroom it needs inside its 2 s budget.
constexpr int AD_FAST_MIN_SAMPLES = 32;
// mp:T3c -- and the sample gate above was still not enough on its own, because the transient is a
// WALL-CLOCK event, not a sample-count one: 32 samples arrive in ~0.6 s at 50 sub-steps a second,
// and the clean-LAN measurement below had the client still genuinely starved at 0.5 s. So the
// controller additionally declines to decide anything for this long after the peer measurement
// first exists, and throws each window away while it waits, so the first real decision is made
// entirely on samples taken after the join settled. 1.5 s is three times the measured transient and
// still comfortably inside the 2 s budget T3's 200 ms growth clause allows, which is the other
// constraint it has to fit between -- and it is only ever paid once per match.
constexpr DWORD  AD_WARMUP_MS      = 1500;
constexpr DWORD  AD_MAX_SAMPLE_MS  = 250; // clamp one frame's contribution (an alt-tab is not starvation)
constexpr double AD_SIM_FLOOR_MULT = 3.0; // never shrink below this many sim sub-steps of horizon
int              g_adaptive        = 0;   // [net] lockstep_adaptive
double           g_ls_min          = 0.030;
double           g_ls_max          = 0.400; // U35 2026-09-02: 200 saddled on real internet links (the icon storm); LAN settles ~100 and never nears it
double           g_step_eps_ms     = 0.01;  // shared with the fixed-pin path (anti-alias nudge)
DWORD            g_ad_t0           = 0;
int              g_ad_frames = 0, g_ad_starved = 0;
// The starved-TIME accounting mp:P1 fix (a) built. It is NO LONGER THE ERROR TERM -- mp:T3 replaced
// that with the binding peer's arrival-lateness tail (lateness_tick above) -- but it is kept and
// still printed on every controller move, for two reasons. It is the quantity every earlier P1/P5
// mp:P1/mp:P5 pacing measurement is expressed in, so a new run stays comparable with the old ones;
// and when the two disagree (a window that reads 0% starved while the tail says the margin is gone)
// that disagreement is the interesting thing, and it is only visible if both are on the line.
DWORD g_ad_last_ms = 0, g_ad_time_ms = 0, g_ad_starved_ms = 0;
// P1 fix (c), 2026-07-26: shrink only after SUSTAINED cleanliness. Carried across into mp:T3's
// controller unchanged (mh::netstats::AD_LATE_SHRINK_AFTER), because the reason still holds: "the
// margin is comfortable" is the SUCCESS condition, and treating one comfortable window as proof of
// surplus walks straight off the value that produced it. Measured then: a 184<->200 oscillation
// spending a starved window on every cycle (13.5% deficit against 5.8% for a fixed 200).
int g_ad_clean = 0; // consecutive windows above the shrink threshold

// Keep the value OFF the 10 ms clock grid: an exactly-on-grid lookahead phase-locks with the
// centisecond clock and forfeits an extra quantum most steps (the MP latency notes CORRECTION 3).
double off_grid_ms(double ms) {
    double tenths = ms / 10.0;
    double frac   = tenths - (double)(long)tenths;
    if (frac < 0.001 || frac > 0.999) ms += g_step_eps_ms;
    return ms;
}

// mp:P10 -- the link's own one-way delay, for the binding peer, as a CONSERVATIVE (high) estimate.
//
// The unused-horizon signal (`g_slack_p50`) is local_h - COMMITTED, and COMMITTED is computed
// against the peer's LAST ARRIVED advertisement, which is one one-way delay old. So slack overstates
// the horizon we are actually wasting by exactly the flight time, and on any link with real latency
// BOTH peers read a positive slack at the same instant -- the 2026-09-20 field logs open with
// `slack 100 ms` on both sides of a 205 ms link while both were genuinely starved. udp_stats.h's
// P10 note carries the argument; this function is the measurement it needs.
//
// (SRTT + 4*RTTVAR)/2 is RFC 6298's RTO bound halved, i.e. deliberately biased HIGH: an over-estimate
// only returns some of the latency win (the decision degrades smoothly toward the pre-P10 one),
// while an under-estimate costs throughput (offline arm: half the true delay -> 0.76x realtime).
//
// -1 means "not measured", and the gate then stands down: the TCP module reports lat_supported = 0
// and every decision stays bit-for-bit pre-P10.
//
// THE PEER LOOKUP IS BY player_id, NOT BY INDEX (dead-ends G261). `lat[]` is in the TRANSPORT's
// peer-slot order and `g_late_peer` is a lockstep horizon slot; the two coincide only because
// lateness_tick indexes horizon slots in the same id space MH_Net_LocalPlayerId() lives in. So match
// the id, and accept the single-entry fallback only when there is exactly one peer and the transport
// has not learnt its id -- which is the 2-player case the note on lat_count already calls out.
double binding_peer_owd_ms() {
    if (g_late_peer < 0) return -1.0;
    MH_NetStats s;
    MH_Net_GetStats(&s);
    if (!s.lat_supported || s.lat_count <= 0) return -1.0;
    const MH_NetPeerLatency *hit = 0;
    for (int i = 0; i < s.lat_count && i < MH_NET_MAX_PEERS; ++i) {
        if (s.lat[i].player_id == g_late_peer) {
            hit = &s.lat[i];
            break;
        }
    }
    if (!hit && s.lat_count == 1 && s.lat[0].player_id < 0) hit = &s.lat[0];
    if (!hit || hit->samples <= 0) return -1.0;
    const double srtt_ms   = (double)hit->srtt_us / 1000.0;
    const double rttvar_ms = (double)hit->rttvar_us / 1000.0;
    const double owd       = (srtt_ms + 4.0 * rttvar_ms) * 0.5;
    return (owd > 0.0) ? owd : -1.0;
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
    DWORD now = GetTickCount();
    // The starved-time diagnostic (see the g_ad_last_ms note): accumulated exactly as before, read
    // by nothing but the log line below.
    const bool starved = (com < clk + sim);
    ++g_ad_frames;
    if (starved) ++g_ad_starved;
    if (g_ad_last_ms) {
        DWORD dt = now - g_ad_last_ms;
        if (dt > AD_MAX_SAMPLE_MS) dt = AD_MAX_SAMPLE_MS;
        g_ad_time_ms += dt;
        if (starved) g_ad_starved_ms += dt;
    }
    g_ad_last_ms = now;
    if (g_ad_t0 == 0) g_ad_t0 = now;

    const double sim_ms = sim * 1000.0;
    // RAISE FAST, LOWER SLOWLY -- the asymmetry the AoE finding and mp:P1 both arrived at, now with
    // teeth on the fast half: a window that is ALREADY under the grow threshold does not have to be
    // waited out, because nothing it can still measure will change the verdict, and every extra
    // millisecond spent waiting is a millisecond the player is stalling. Shrinking keeps the full
    // window (and then AD_LATE_SHRINK_AFTER of them), because giving latency back is never urgent.
    const bool urgent = g_late_have && g_late_n >= AD_FAST_MIN_SAMPLES &&
                        (double)g_late_tail95 < mh::netstats::AD_LATE_GROW_MULT * sim_ms;
    const DWORD due = urgent ? AD_FAST_MS : AD_WINDOW_MS;
    if ((now - g_ad_t0) < due) return;

    // mp:T3c -- the join warm-up. The clock starts the moment ANY peer measurement exists (not at
    // lockstep entry: before a peer has advertised there is nothing to be early about), and until it
    // runs out every window below is judged LA_WARMUP and its samples are dropped by the
    // lateness_consume() at the end. `g_ad_warm_t0` is cleared by the match reset in lateness_tick.
    if (g_ad_warm_t0 == 0 && g_late_have) g_ad_warm_t0 = now ? now : 1;
    const bool warm = (g_ad_warm_t0 != 0) && (now - g_ad_warm_t0) >= AD_WARMUP_MS;

    double cur = g_lockstep_step;
    if (cur <= 0.0) memcpy(&cur, (const void *)ADDR_STEP_SIZE, sizeof(double));
    double floor_s = g_ls_min;
    if (AD_SIM_FLOOR_MULT * sim > floor_s) floor_s = AD_SIM_FLOOR_MULT * sim; // sim-relative floor

    // THE WHOLE JUDGEMENT IS mh::netstats::lookahead_decide, and it is there rather than here so it
    // can be asserted without a rig: net_selftest.exe udpstatstest drives it through the exact three
    // claims this item's done_when asks of the rig (climbs within one window at 200 ms, settles on
    // the floor on a clean link, cannot shrink before AD_LATE_SHRINK_AFTER clean windows).
    //
    // DETERMINISM. This moves g_lockstep_step, which on_time_tick pins into STEP_SIZE -- a peer's
    // own ADVERTISED horizon. COMMITTED is min(local, peers) and the sim clamps to COMMITTED, so a
    // peer's lookahead gates only its OWN willingness to run ahead; the step sequence is unchanged
    // however the two peers' values differ. Nothing here reads or writes sim state, and the new
    // error term is measured from PEER_HORIZON, which is an input to the sim's clamp and not an
    // output of it. Proven, not argued: the determinism gate over UDP with the controller active.
    mh::netstats::LookaheadIn in;
    in.cur_ms                          = cur * 1000.0;
    in.sim_ms                          = sim_ms;
    in.floor_ms                        = floor_s * 1000.0;
    in.ceil_ms                         = g_ls_max * 1000.0;
    in.have                            = g_late_have;
    in.tail95_ms                       = (double)g_late_tail95;
    in.clean_in                        = g_ad_clean;
    in.warm                            = warm;                  // mp:T3c
    in.slack_ms                        = (double)g_slack_p50;   // mp:T3c
    in.link_owd_ms                     = binding_peer_owd_ms(); // mp:P10
    const mh::netstats::LookaheadOut d = mh::netstats::lookahead_decide(in);
    g_ad_clean                         = d.clean_out;

    const double want_ms = off_grid_ms(d.want_ms);
    const double want    = want_ms / 1000.0;
    if (want != cur) {
        g_lockstep_step = want; // on_time_tick pins it into STEP_SIZE from here on
        if (g_ls_log) {
            const char *verdict = (d.verdict == mh::netstats::LA_GROW)     ? "grow"
                                  : (d.verdict == mh::netstats::LA_SHRINK) ? "shrink"
                                  : (d.verdict == mh::netstats::LA_HOLD)   ? "hold"
                                  : (d.verdict == mh::netstats::LA_WARMUP) ? "warmup"
                                                                           : "nosamples";
            char        b[256];
            // mp:P10 -- `owd` is the conservative one-way-delay estimate the slack is corrected by
            // (-1 = the transport cannot measure its link, so the grow gate stood down). Without it
            // on the line a `hold` on a starved window is unreadable after the fact: the raw slack
            // alone does not say whether the gate fired.
            wsprintfA(b,
                      "; [adaptive] t=%lu lookahead %ld -> %ld ms %s (late p50 %d tail95 %d tail99 "
                      "%d ms, n=%d, peer=%d, slack %d ms, owd %d ms; starved %ld/%ld ms diag)\n",
                      now, (long)(cur * 1000.0), (long)want_ms, verdict, g_late_p50, g_late_tail95,
                      g_late_tail99, g_late_n, g_late_peer, g_slack_p50, (int)in.link_owd_ms,
                      (long)g_ad_starved_ms, (long)g_ad_time_ms);
            seam_log(b);
        }
    }
    g_ad_t0         = now;
    g_ad_frames     = 0;
    g_ad_starved    = 0;
    g_ad_time_ms    = 0;
    g_ad_starved_ms = 0;
    lateness_consume(); // the next decision is made on samples taken after this one -- see there
}

// mp:P9 -- the resync-trigger WATCH: the two numbers the 2026-09-20 field logs could not answer.
// RESYNC_TRIGGER_COUNT (0x00e58791) is the leader's cumulative stall-nag counter (two increment sites,
// SENT @0x49d8cb and RECEIVED @0x49c508; the `resync_trigger_gate` carriers below); when it
// passes ACTIVE_PLAYER_COUNT*100 the leader force-fires the mode-8 resync barrier (~2 s frozen on
// every peer) and zeroes it. The field's crawl is that barrier firing every ~2 s in configuration
// (1), where the gate is not carried -- but the rig would not reproduce it, and nobody could say how
// fast the counter climbed or whether SYNC_RETRY_COUNTDOWN (the gate's own predicate, < 0x38 =
// genuine silence) was ever low. This reads all three every frame and writes:
//   `; [resync] trigger_count=N threshold=T countdown=C (+D in 10s)`   every 10 s WHILE N MOVES
//   `; [resync] force_resync FIRED #k: count N -> 0 (threshold T, countdown C)`   on every reset
// A reset is inferred from the counter DROPPING (force_resync is the only writer that lowers it);
// the line is on every peer, and only the leader's count ever moves. Reads game memory only: no
// patch, no libmh, so configuration (1) carries it -- which is the whole point.
// All three are MOVABLE regions (relocate_state=1), so they are resolved through live_base per read.
//
// WATCH v2 (mp:P9 2/2, 2026-09-22) -- THE COUNTER-DROP INFERENCE CANNOT SEE THE FIELD'S STORM.
// force_resync (0x0049d9d6) zeroes RESYNC_TRIGGER_COUNT at ENTRY, before its RESYNC_IN_PROGRESS
// one-shot test, and the threshold it fires over is ACTIVE_PLAYER_COUNT*100 where
// ACTIVE_PLAYER_COUNT (0x005d54c4) is written ONLY by llm_net_lockstep_count_active_players --
// called from the removal paths and the countdown-expiry branch, never at session begin -- so in
// every match until the first removal the threshold is 0, the FIRST stall-nag fires, and the count
// goes 0 -> 1 -> 0 inside ONE keepalive/dispatch call: a per-frame sampler reads a constant 0 and
// the `FIRED` line above never fires (the 2026-09-22 rig reproduction: 0 lines, 0 increments, and
// the barrier storm it was looking for had latched on the first nag -- see the rig trap below).
// So the barrier is now watched DIRECTLY, on the flag force_resync sets:
//   `; [resync] barrier #k BEGIN (count was N, threshold T, countdown C) flags=0xFF`   RESYNC_IN_PROGRESS 0 -> 1
//   `; [resync] barrier #k END after M ms flags=0xFF`                                   RESYNC_IN_PROGRESS 1 -> 0
// `flags` is _G_LLM_NET_LOCKSTEP_STATUS_FLAGS (0x005d55b4) -- bit 0x40 rides with the barrier
// (set when the resync-begin order enters the wait screen, i.e. AFTER the frame this BEGIN sample
// is taken on, and cleared with the flag by llm_wait_screen_frame -- the GAME_MODE==8 frame,
// leader, after the 2002 ms deadline: `STATUS_FLAGS & 0xbf` -- or dispatch inner 0xe on the
// receivers; so a healthy run reads 0x00 on BOTH lines and mh_lockstep.log's flags column carries
// the 0x40 in between). The END line can be MISSING for the rest of a match: if the mode-8 store at 0x004c85ed is NOP'd
// (`[net] defang_overlay=1` -> defang_xui, the UI rig's default until this change; the SHIPPED ini
// says 0) the wait-screen frame never runs, nothing clears the flag, and every later force_resync
// entry is the silent one-shot no-op. One BEGIN with no END and flags stuck at 0x40 is that
// signature, not a quiet link. The FIRED-from-counter-drop line stays as a secondary witness.
inline uintptr_t rt_count_base() { return mh::state::live_base(mh::state::RID_NET_LOCKSTEP_RESYNC_TRIGGER_COUNT); }     // u32 (unaligned)
inline uintptr_t rt_countdown_base() { return mh::state::live_base(mh::state::RID_NET_LOCKSTEP_SYNC_RETRY_COUNTDOWN); } // int, reload 0x3c
inline uintptr_t rt_players_base() { return mh::state::live_base(mh::state::RID_NET_ACTIVE_PLAYER_COUNT); }             // int
inline uintptr_t rt_in_progress_base() { return mh::state::live_base(mh::state::RID_NET_RESYNC_IN_PROGRESS); }          // int (4 bytes; ==1 while a barrier runs)
uint32_t         g_rt_last_count = 0;
uint32_t         g_rt_10s_base   = 0;
DWORD            g_rt_next_tick  = 0;
unsigned         g_rt_fires      = 0;
unsigned         g_rt_barriers   = 0;     // BEGIN edges seen this match
DWORD            g_rt_barrier_t0 = 0;     // GetTickCount at the last BEGIN edge
bool             g_rt_in_barrier = false; // last sampled RESYNC_IN_PROGRESS != 0
bool             g_rt_armed      = false;
bool             g_rt_said_armed = false; // the per-match baseline line below was written

void resync_trigger_watch_tick() {
    if (*(const uint8_t *)ADDR_SESSION_MODE != 3) {
        g_rt_armed = g_rt_said_armed = false;
        return;
    }
    uint32_t count;
    memcpy(&count, (const void *)rt_count_base(), sizeof(count)); // the byte address is odd
    const int      countdown   = *(const int *)rt_countdown_base();
    const uint32_t threshold   = (uint32_t)(*(const int *)rt_players_base()) * 100u;
    const bool     in_progress = *(const int *)rt_in_progress_base() != 0;
    const unsigned flags       = (unsigned)*(const uint8_t *)ADDR_STATUS_FLAGS;
    const DWORD    now         = GetTickCount();
    if (!g_rt_armed) { // first frame of a match: baseline, no line
        g_rt_armed      = true;
        g_rt_last_count = g_rt_10s_base = count;
        g_rt_next_tick                  = now + 10000;
        g_rt_barriers                   = 0;
        g_rt_in_barrier                 = in_progress; // a match never starts inside a barrier; sampled anyway
        return;
    }
    if (in_progress != g_rt_in_barrier) {
        g_rt_in_barrier = in_progress;
        char b[160];
        if (in_progress) {
            g_rt_barrier_t0 = now;
            wsprintfA(b, "; [resync] barrier #%u BEGIN (count was %u, threshold %u, countdown %d) flags=0x%02x\n",
                      ++g_rt_barriers, (unsigned)g_rt_last_count, (unsigned)threshold, countdown, flags);
        } else {
            wsprintfA(b, "; [resync] barrier #%u END after %lu ms flags=0x%02x\n", g_rt_barriers,
                      (unsigned long)(now - g_rt_barrier_t0), flags);
        }
        seam_log(b);
    }
    if (count < g_rt_last_count) {
        char b[160];
        wsprintfA(b, "; [resync] force_resync FIRED #%u: count %u -> %u (threshold %u, countdown %d)\n",
                  ++g_rt_fires, (unsigned)g_rt_last_count, (unsigned)count, (unsigned)threshold, countdown);
        seam_log(b);
        g_rt_10s_base = count;
    }
    g_rt_last_count = count;
    if ((long)(now - g_rt_next_tick) >= 0) {
        // mp:P9 (2/2): ONE baseline line per match at the first 10 s tick, whether or not the counter
        // moved. The two lines below are silent while the count sits still -- which is exactly what
        // the first 15-minute reproduction (2026-09-22, configuration (1), gate OFF, 100 ms shim)
        // produced: ~5700 stall-nags sent by the host (SYNC_RETRY_COUNTDOWN 60->59->60 on every
        // episode) and NOT ONE increment, so the log carried nothing at all and could not say
        // whether the threshold (ACTIVE_PLAYER_COUNT*100) was 200 or 0. A field log where nothing
        // fires must still name the threshold; this is the line that does.
        if (!g_rt_said_armed) {
            g_rt_said_armed = true;
            char b[160];
            wsprintfA(b, "; [resync] armed: trigger_count=%u threshold=%u (ACTIVE_PLAYER_COUNT=%u) countdown=%d\n",
                      (unsigned)count, (unsigned)threshold, (unsigned)(threshold / 100u), countdown);
            seam_log(b);
        }
        if (count != g_rt_10s_base) {
            char b[160];
            wsprintfA(b, "; [resync] trigger_count=%u threshold=%u countdown=%d (+%u in 10s)\n", (unsigned)count,
                      (unsigned)threshold, countdown, (unsigned)(count - g_rt_10s_base));
            seam_log(b);
        }
        g_rt_10s_base  = count;
        g_rt_next_tick = now + 10000;
    }
}

// mp:P9 ROOT FIX (2026-09-22) -- `[net] resync_count_init` (default ON): recompute ACTIVE_PLAYER_COUNT
// once at match start, through the game's own llm_net_lockstep_count_active_players (0x0049e3ea).
//
// WHY. That function is the ONLY writer of _G_LLM_NET_ACTIVE_PLAYER_COUNT (0x005d54c4) in the image
// (mp:P9 RE, EN v409: whole-image dword scan, 7 hits = the xref list), and retail never calls it at
// session begin -- its callers are time_tick's countdown-expiry branch (0x0043f214), the two
// player_remove paths and dispatch inner cases 7/8/9 (the removal frames); session_globals_reset
// zeroes 0x005d54c0 and 0x005d54c8 and SKIPS this one. So every match starts with the DGROUP initial
// 0 and the leader's force-resync threshold ACTIVE*100 is 0: both INC sites (SENT @0x49d8cb, RECEIVED
// @0x49c508) do INC -> CMP count > 0 -> force_resync, i.e. the FIRST stall-nag of every episode fires
// the 2 s mode-8 barrier -- the field's ~2 s storm (109 resync-state/resume pairs in the joiner's
// 2026-09-20 log). With the count at 2 the threshold is 200 and the trigger means what its author
// meant: 200 cumulative nags. No byte patch: this is a call into an original function from our
// seam, like ui_bldg_cancel_task's order call, so configuration (1) carries it.
//
// WHERE. The first on_time_tick of a SESSION_MODE==3 match. llm_strat_session_begin_multi
// (0x0045435f) sets SESSION_MODE=3 and THEN runs llm_strat_player_profile_init over every slot with
// controller_flags != 0 (that is what sets the ALIVE|HUMAN bits count_active_players counts) inside
// the SAME call, so the next time_tick sees the slots final; and the first keepalive (the SENT INC)
// needs SYNC_WAIT_ELAPSED > 1.0 s of parked time inside time_tick's body, which runs AFTER this
// pre-hook -- so the count is 2 before either INC site can compare against it. The lobby-entry
// seam (launch.cpp mp_lobby_entry_tick) runs too EARLY: it writes NET_LOCAL_PLAYER_SLOT every lobby
// frame, before profile_init has set any status_flags.
//
// DETERMINISM. count_active_players writes exactly one global (decompiled: zero it, count the
// slots with status_flags & ALIVE(2) & HUMAN(4), store, return). Its FIVE readers (Ghidra xrefs to
// 0x005d54c4, re-read for this change): (a) dispatch 0x0049c50e and (b) keepalive 0x0049d8d1 -- the
// resync trigger threshold, evaluated only on the leader (both sites are behind
// is_local_leader_peer(-1)) and deciding only WHEN the leader emits its already-replicated resync
// broadcast; (c) time_tick 0x0043f2fb and (d) dispatch 0x0049c539 -- the adaptive-pacing gate
// `stalls >= ACTIVE*5` that grows _G_LLM_STRAT_LOCKSTEP_STEP_SIZE (0x005d55bc) / restamps
// ADAPT_NEXT_TIME (0x005d55c4): per-peer lookahead pacing, which this DLL already pins per frame from
// g_lockstep_step (on_time_tick below) and which is not in tools/data/hash_manifest.json (checked:
// none of 0x005d54c4 / 0x005d55bc / 0x005d55c4 / 0x00e58795 lies in a hashed region); (e) the
// read-back at 0x0049e44d inside count_active_players itself. None feeds a hashed region or an
// order, so initialising it on both peers changes no replicated state -- the same argument the
// resync_trigger_gate carriers rest on.
bool g_rci_done = false; // this match's init already ran

void resync_count_init_tick() {
    if (*(const uint8_t *)ADDR_SESSION_MODE != 3) {
        g_rci_done = false;
        return;
    }
    if (g_rci_done) return;
    g_rci_done = true;
    if (!g_resync_count_init) return; // knob off: the stock threshold-0 start (the reproduction arm)
    const int before = *(const int *)rt_players_base();
    const int after  = mh::call::llm_net_lockstep_count_active_players();
    char      b[160];
    wsprintfA(b, "; [resync] count_init: ACTIVE_PLAYER_COUNT %d -> %d (threshold %u)\n", before, after,
              (unsigned)after * 100u);
    seam_log(b);
}

// mp:P9W (2026-09-22) -- A RECEIVER'S OWN DEADLINE ON THE MODE-8 WAIT SCREEN.
//
// llm_wait_screen_frame (0x0043ee38, the GAME_MODE==8 frame) gives the LEADER a fixed ~2002 ms
// deadline (RESYNC_DEADLINE_MS) after which it exits the barrier unconditionally: send RESUME
// (0xe), restore GAME_MODE, clear RESYNC_IN_PROGRESS + STATUS_FLAGS&0x40, resync the clock (see the
// function's own plate). A non-leader has NO such deadline -- its only exit is dispatch inner case
// 0xe (the leader's RESUME) actually arriving. And time_tick -- the ONLY place any of this DLL's
// own timeouts run (data_timeout_tick/GS2, graceful_drop) -- does not run at all while GAME_MODE==8
// (llm_wait_screen_frame calls nothing but dispatch), so none of those existing mechanisms can ever
// fire during a barrier either. If the leader dies (crash, quit, the link going dark) mid-barrier,
// every receiver is wedged in the wait screen forever: found by mp:P9 (2/2)'s storm-repro run when
// the blackhole shim landed inside a barrier (tmp/p9b/storm_run). Retail's own bug -- the leader
// has a deadline, the receiver none.
//
// FIX: give the receiver the SAME KIND of deadline, tracked entirely in this DLL (no dependency on
// RESYNC_DEADLINE_MS, a leader-computed global a non-leader never writes and may hold a
// stale/garbage value). install_resync_receiver_deadline splices llm_frame_dispatch's ONE call site
// to llm_wait_screen_frame (ADDR_WAIT_SCREEN_CALL_SITE); the thunk calls the ORIGINAL function
// FIRST -- preserving the leader's own exit and every peer's dispatch/RX-drain unchanged -- then,
// on a peer that is NOT the leader and is STILL inside a barrier after max(2002, data_timeout_ms)
// ms of this DLL's own GetTickCount() clock, runs the leader's exact exit sequence minus the RESUME
// broadcast (we cannot ask a presumably-dead leader to do anything, and broadcasting our OWN RESUME
// would be impersonating a leader we are not): llm_net_mp_leave_reset_game_mode,
// llm_net_lockstep_sync_busywait, clear RESYNC_IN_PROGRESS + STATUS_FLAGS&0x40,
// llm_strat_time_resync_and_tick -- and THEN removes the presumed-dead leader through the same
// "normal removal path" GS2/graceful_drop already use: llm_net_player_remove(leader's side_id),
// found by scanning _G_LLM_STRAT_PLAYERS[] for the first ALIVE|HUMAN slot -- the exact scan
// llm_net_lockstep_is_local_leader_peer itself runs to elect the leader, so "the leader" here means
// the same peer that function would call leader.
//
// SAFE FOR THE SCENARIO THIS EXISTS FOR (the 2-peer storm row's shape). llm_net_player_remove's own
// plate warns it is NOT scheduled onto the ordered stream and can lose peer ordering if called
// without every peer parked at the same horizon -- the same hazard graceful_drop above guards
// against with its own TOTAL>=COMMITTED latch (see g_pending_dead in on_time_tick) -- but that
// hazard is about MULTIPLE SURVIVING peers disagreeing on when the drop happens. With the leader
// dead and the barrier already freezing every remaining peer's sim identically (mode 8 runs no
// sim_tick at all -- nobody's clock is moving to disagree about), the lone survivor removing it
// once its own deadline expires has no OTHER live peer to diverge from. A 3+-peer extension would
// need the same latch shape graceful_drop uses; out of scope for this fix.
//
// [net] resync_receiver_deadline, default ON.
constexpr int STATUS_ALIVE = 0x2;
constexpr int STATUS_HUMAN = 0x4;

bool  g_recv_in_barrier = false; // our own last-sampled "am I in a barrier", tracked at this hook's own cadence
DWORD g_recv_barrier_t0 = 0;     // GetTickCount() when we first observed it this episode

// The leader's side_id -- the first _G_LLM_STRAT_PLAYERS[] slot with ALIVE|HUMAN (stride 0x740,
// status_flags at +0, side_id at +0x73c -- docs/structs.md), the same scan
// llm_net_lockstep_is_local_leader_peer runs internally (0x0049e653) -- or -1 if no such slot
// exists.
int recv_deadline_leader_side_id() {
    const uint8_t *players = (const uint8_t *)mh::addr::_G_LLM_STRAT_PLAYERS;
    for (int i = 0; i < 8; ++i) {
        const uint32_t flags = *(const uint32_t *)(players + (size_t)i * 0x740u);
        if ((flags & STATUS_ALIVE) && (flags & STATUS_HUMAN)) {
            return *(const int32_t *)(players + (size_t)i * 0x740u + 0x73cu);
        }
    }
    return -1;
}

void resync_receiver_deadline_hook() {
    mh::call::llm_wait_screen_frame(); // the original body -- leader exit + every peer's dispatch, unchanged
    if (!g_resync_receiver_deadline) return;
    if (*(const int *)rt_in_progress_base() == 0) { // RESUME already landed (or we were never in one) -- reset
        g_recv_in_barrier = false;
        return;
    }
    if (mh::call::llm_net_lockstep_is_local_leader_peer(-1) != 0) return; // the leader has its own deadline above
    const DWORD now = GetTickCount();
    if (!g_recv_in_barrier) {
        g_recv_in_barrier = true;
        g_recv_barrier_t0 = now;
        return;
    }
    const DWORD deadline_ms = (DWORD)(g_data_timeout_ms > 2002 ? g_data_timeout_ms : 2002);
    if ((DWORD)(now - g_recv_barrier_t0) < deadline_ms) return;
    const int dead = recv_deadline_leader_side_id();
    char      b[176];
    wsprintfA(b, "; [resync] receiver_deadline: %lu ms in barrier with no RESUME -- leaving the wait screen and removing side_id=%d\n",
              (unsigned long)(now - g_recv_barrier_t0), dead);
    seam_log(b);
    mh::call::llm_net_mp_leave_reset_game_mode();
    mh::call::llm_net_lockstep_sync_busywait();
    *(int *)rt_in_progress_base() = 0;
    *(uint8_t *)ADDR_STATUS_FLAGS &= 0xbf;
    mh::call::llm_strat_time_resync_and_tick();
    g_recv_in_barrier = false;
    if (dead >= 0) mh::hook::call_watcall1(mh::addr::llm_net_player_remove, (void *)(intptr_t)dead);
}

// Register discipline: llm_wait_screen_frame takes no args and returns nothing, so (like
// present_detour above) the thunk need not marshal anything -- pushad/pushfd around the call is
// enough to leave the caller's registers exactly as it left them.
__declspec(naked) void wait_screen_deadline_thunk() {
    __asm {
        pushad
        pushfd
        call resync_receiver_deadline_hook
        popfd
        popad
        ret // back into llm_frame_dispatch's case 8, past the spliced CALL llm_wait_screen_frame
    }
}

void install_resync_receiver_deadline() {
    if (mh::hook::promoted_owner_of(ADDR_WAIT_SCREEN_CALL_SITE)) {
        seam_log("; resync_receiver_deadline splice DISPLACED: llm_frame_dispatch is promoted this run\n");
        return;
    }
    uint8_t repl[5];
    repl[0]     = 0xE8;
    int32_t rel = (int32_t)((uintptr_t)&wait_screen_deadline_thunk - (ADDR_WAIT_SCREEN_CALL_SITE + 5));
    memcpy(repl + 1, &rel, sizeof(rel));
    const bool ok = patch_bytes_guarded(ADDR_WAIT_SCREEN_CALL_SITE, WAIT_SCREEN_CALL_EXPECT, repl, 5);
    if (!ok)
        seam_log("; resync_receiver_deadline NOT armed (unexpected bytes at the wait-screen call site)\n");
    else if (g_resync_receiver_deadline)
        seam_log("; resync_receiver_deadline armed: a non-leader leaves the mode-8 wait screen after "
                 "max(2002, data_timeout_ms) ms with no RESUME\n");
    else
        seam_log("; resync_receiver_deadline spliced but INERT ([net] resync_receiver_deadline=0)\n");
}

// mp:GS2 -- game-level peer-data timeout. Called from on_time_tick right after lateness_tick(), so
// g_late_last_h[]/g_late_last_move[] are this frame's fresh values (same ones the lookahead's own
// live/dead call used). A frozen peer's SIM stops moving its horizon forward while its TRANSPORT can
// keep answering keepalives (mp:GS1/RM1's field freezes: since_rx 22-35 s, link never drops) -- this
// runs the SAME direct removal U17(b)'s transport-death fast-drop uses (llm_net_player_remove by
// side_id), so it works whether or not lockstep is promoted: CONFIGURATION (1) is exactly the mode
// gone_peer_frame_guard cannot help, because that guard lives in the reimpl dispatch_packet libmh
// never runs there, while this reads ADDR_PEER_HORIZON directly and calls a real game function.

// mp:U19b -- a slot the lockstep dispatch has ALREADY removed (a clean quit's subtype-8 record clears
// PLAYER_HUMAN on arrival, 0x0049cb44) must not be removed a second time by a wall-clock watchdog. The
// quitter's process stays in the main menu with its transport UP, so to GS2 its slot keeps reading as a
// connected, data-silent peer, and to the transport-death catch it reads as a dead one the moment it
// finally closes the socket (browser re-dial, exit). A second llm_net_player_remove on a gone slot is
// not idempotent for the survivors: another red "Disconnected" notice, LOCKSTEP_PLAYER_COUNT
// decremented again, time_resync_and_tick re-run -- and at an UNPARKED position, because the frozen
// horizon that parked everybody the first time no longer participates in the min.
// status_flags: bit 0x02 ALIVE, 0x04 HUMAN (turn_engine.h), byte 0 of the 0x740 player profile.
inline bool slot_is_active_human(int idx) {
    if (idx < 0 || idx >= 8) return false;
    const uint8_t f = *(const uint8_t *)(mh::addr::_G_LLM_STRAT_PLAYERS + (unsigned)idx * 0x740u);
    return (f & 0x02) != 0 && (f & 0x04) != 0;
}

void data_timeout_tick() {
    if (g_data_timeout_ms <= 0) return;                         // [net] data_timeout_ms <= 0 -- feature off
    if (*(const uint8_t *)ADDR_SESSION_MODE != 3) return;       // only a live lockstep match has peers to time out
    if (!MH_Net_IsStarted() || MH_Net_PeerCount() <= 0) return; // solo: nothing to watch
    // BOUND THE SCAN TO REAL SLOTS ONLY -- measured trap, first two rig runs of `gs2_data_timeout`
    // (2026-09-21): every UNUSED PEER_HORIZON slot in a 2-player match got "dropped" too.
    // `net_seams.cpp`'s own PLAYERDUMP comment already names the cause -- an empty slot holds
    // "retail's 10-second sentinel", a nonzero value indistinguishable in SHAPE from a real peer's
    // very first sample (both simply differ from g_late_last_h[]'s zero-initialised default), so
    // g_late_last_move[] latches a phantom slot's "first observation" exactly like a real one's --
    // and a value that then never changes again ages past T identically to a genuinely frozen real
    // peer. The FIRST fix tried `current_map_player_count` (lobby_widgets.cpp's A_MAP_PCOUNT) and
    // measured WRONG on the client: that field is finalised by the HOST's build_players and reads
    // correctly there, but on a CLIENT it still held the bare map file's declared capacity (8, not
    // the lobby's real 2) -- an asymmetry between roles this seam cannot afford. `MH_Net_PeerCount()`
    // (the TRANSPORT's own connected-peer count) is symmetric by construction -- every other gate in
    // this file already reads it identically on both roles -- so `1 + MH_Net_PeerCount()` is the
    // match's real size for as long as side ids stay contiguous from 0, which they are for any match
    // nobody has left yet (this seam's whole subject). A peer who later leaves narrows PeerCount
    // again, which only ever SHRINKS the scan -- never re-admits a slot this watchdog already dropped.
    const int   n   = 1 + MH_Net_PeerCount() < LS_LATE_PEERS ? 1 + MH_Net_PeerCount() : LS_LATE_PEERS;
    const DWORD now = GetTickCount();
    const int   me  = MH_Net_LocalPlayerId();
    for (int i = 0; i < n; ++i) {
        if (i == me || g_gs2_dropped[i]) continue;
        // U19b: a slot the lockstep dispatch already removed (a clean quit) is not a data-silent
        // peer, it is a gone one. Re-read every frame rather than latched: the flags are the
        // roster's own truth, and a latch taken one frame too early would disarm this watchdog for
        // a peer whose seat had simply not been flagged yet.
        if (!slot_is_active_human(i)) continue;
        // A slot with h<=0 has never advertised at all yet (the join window GS1 already gates on) --
        // nothing to time out until it has really been seen alive once.
        if (g_late_last_h[i] <= 0.0 || g_late_last_move[i] == 0) continue;
        const DWORD since = now - g_late_last_move[i];
        if (since < (DWORD)g_data_timeout_ms) continue;
        g_gs2_dropped[i] = true; // latch FIRST -- the removal call below must never re-enter this slot
        mh::hook::call_watcall1(mh::addr::llm_net_player_remove, (void *)(intptr_t)i);
        notify_player_dropped(i);
        char b[176];
        wsprintfA(b, "; GS2: peer %d data-silent for %lu ms > %d -> dropped\n", i, (unsigned long)since,
                  g_data_timeout_ms);
        seam_log(b);
        // SES1: same "a kick that leaves nobody" guard as U17(b) -- a below-quorum drop closes our own
        // session record even though the peer that just left never sends its own teardown seam.
        if (MH_Net_PeerCount() <= 0) mp_session_close("timeout");
    }
}

void on_time_tick() {
    // mp:U19b -- the quit's freeze ends with the quit. A time_tick outside the freeze..teardown window
    // (g_leave_in_progress) is a LATER match: every frozen quit runs its whole wait + removal + retail
    // teardown inside ONE UI callback, so no tick of the match being left can land here after it.
    // (A tick INSIDE the window is possible -- a survivor dropping during the wait routes through
    // time_resync_and_tick -- which is what the guard is for.)
    if (g_leave_frozen && !g_leave_in_progress) {
        InterlockedExchange(&g_leave_frozen, 0);
        g_hz_max_seen = 0.0;
    }
    if (*(const uint8_t *)ADDR_SESSION_MODE == 3) {
        // U19b: the largest horizon this peer has advertised so far. Every advert is the HORIZON
        // global's value at send time and every writer bumps it before sending, so a per-frame sample
        // only misses a bump that the adaptive controller then LOWERED within the same frame. A new
        // match starts below the old maximum (the clock restarts) -- reset on the clock going
        // backwards, the same signal lateness_tick uses.
        double h, clk;
        memcpy(&h, (const void *)ADDR_LOCAL_HORIZON, sizeof(double));
        memcpy(&clk, (const void *)ADDR_GAME_CLOCK, sizeof(double));
        // A horizon more than 2 s ahead of the clock is a restarted clock, not a shrink: the
        // lookahead is capped at [net] lockstep_max_ms (SHIPPED 0.4 s) and the paged-list keepalive
        // bumps by one scaled step at a time. THE 2 s IS NOT INDEPENDENT OF THAT CAP -- it is the
        // cap with headroom, so anyone raising lockstep_max_ms has to come back here: at the
        // shipped 400 ms the margin is 5x, at a 1 s ceiling it would be 2x, and at anything above
        // 2 s a legitimate horizon reads as a restarted clock and this resets every window.
        // mp:P11 measured the ceiling as adequate through RTT 600 (where it does clamp, 4-5 windows
        // per peer, and the pair still converges), so no raise is shipped -- but the coupling is
        // written down here rather than rediscovered.
        if (clk + 2.0 < g_hz_max_seen) g_hz_max_seen = 0.0;
        if (h > g_hz_max_seen) g_hz_max_seen = h;
    }
    lateness_tick();             // mp:T3 -- sample first: the controller below reads the snapshot it publishes
    data_timeout_tick();         // mp:GS2 -- act on this frame's fresh g_late_last_move[] before anything else touches it
    resync_count_init_tick();    // mp:P9 root fix -- ACTIVE_PLAYER_COUNT=players at match start (before the watch samples it)
    resync_trigger_watch_tick(); // mp:P9 -- the resync-trigger counter + the barrier edges, readable off one peer's log
    adaptive_tick();             // may move g_lockstep_step; the pin below applies it the same frame
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
            if (dead >= 0) {
                const int pidx =
                    mh::hook::call_watcall1(mh::addr::llm_strat_player_by_side_id, (void *)(intptr_t)dead);
                if (pidx != -1 && slot_is_active_human(pidx)) {
                    g_pending_dead      = dead;
                    g_pending_dead_wait = 0;
                } else if (pidx != -1 && g_ls_log) {
                    // U19b: the socket of a peer the dispatch already removed (a clean quit, sitting
                    // in its main menu) finally closed -- nothing left to remove, see
                    // slot_is_active_human.
                    // Deliberately NOT the `; U17 fast-drop: transport-dead peer` needle: every
                    // quit checker reads that line as "the B2 catch ended this match".
                    char b[128];
                    wsprintfA(b, "; U19b: transport of already-removed peer side=%d closed -- no second removal\n",
                              dead);
                    seam_log(b);
                }
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
                notify_player_dropped(dead);           // host-side "player dropped" HUD notice
                g_last_fastdrop_tick = GetTickCount(); // U19d: a REAL transport death -- see on_gameover_post
                if (g_ls_log) {
                    char b[192];
                    wsprintfA(b,
                              "; U17 fast-drop: transport-dead peer side=%d -> broadcast removal"
                              " (%s after %d frames)\n",
                              dead, parked ? "PARKED" : "TIMEOUT-UNPARKED", g_pending_dead_wait);
                    seam_log(b);
                }
                // SES1: a kick that leaves NOBODY is the end of this match for us -- there is no peer
                // left to be in lockstep with, and the seams that normally close a session (gameover,
                // either leave) will not fire, because the player is still sitting in a game that has
                // quietly become single. Guarded on the peer count rather than on the kick itself: in
                // a 3-peer game losing one is an incident, not an ending.
                if (MH_Net_PeerCount() <= 0) mp_session_close("timeout");
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

// U19d: on a 2-player graceful quit the SURVIVOR's end-of-match dialog reads "Connection to server
// lost" instead of naming the departure -- RIG-MEASURED 2026-09-18 (host_rematch, no net_extra pin):
// exactly ONE call to this entry point, log line `; on_gameover ENTER sess=3 outcome=0` (the naked
// thunk could not recover `outcome` before this fix, hence the always-0), dumped widget text
// literally "Connection to server lost", no `; U17 fast-drop` anywhere in the host's log -- i.e. a
// perfectly clean quit, not a transport failure, producing the wrong text on its FIRST and ONLY
// entry. (An earlier theory here guessed a REDUNDANT second call and a debounce fix; the rig proved
// that wrong -- there was only ever one call -- and the debounce is gone.)
//
// RE'D via the promoted C++ this build actually runs (libmh/lockstep/rx_dispatch.cpp -- wire
// promotion is armed BY DEFAULT under `[config] mode=brokered`, so the retail assembly this
// comment used to cite is not what executes): `detail::dispatch_packet`'s outer loop
// (rx_dispatch.cpp:469-474) reads a leading tag byte per message and, for any tag it does not
// recognise, calls `handle_garbled` -- which unconditionally fires
// `calls.outcome_dialog(OUTCOME_NETWORK_ERROR)` (=7, rx_dispatch.cpp:85/283), the ONLY caller of
// outcome code 7 anywhere in the closure, mapping (llm_ui_outcome_dialog's MP switch, retail
// 0x004c6c4f) to `G_TEXT_PTRS[0x30d]` = "connection_to_server_lost" (text-id table row 781). The
// CORRECT below-quorum path (`handle_peer_drop` -> `last_peer_teardown` -> `presence_lost`) maps to
// `G_TEXT_PTRS[0x2dc]` = "you_are_the_last_player" (row 732) instead. Exactly how a clean quit's own
// bytes end up read as an unrecognised tag was not pinned down further (a framing/cursor question
// inside a migration-owned TU, rx_dispatch.cpp, outside this file's write set) -- but the SYMPTOM,
// the DISCRIMINATOR below, and the FIX are all measured directly, which is enough to correct what
// the player sees without touching that file.
//
// FIX: this entry point (gameover_outcome_dialog == llm_ui_outcome_dialog @0x004c6c4f) is exclusively
// ours already (gameover_detour), so rather than hook the wire layer -- where every candidate
// (llm_net_lockstep_send_presence_lost, llm_net_lockstep_dispatch) is ALREADY claimed by the wire/
// turn-engine promotion closures, armed by the same default -- this is now a WRAP detour: it recovers
// the real `outcome` byte (Watcom `__watcall`'s single param arrives in AL), runs on_gameover_pre
// BEFORE the original body (unchanged: session downgrade + mp_session_close, same ordering as
// before), CALLS (not jmp's to) the stolen-prologue trampoline so the original dialog-setup body
// still runs and returns here, then runs on_gameover_post: if the outcome the ORIGINAL body just
// rendered was OUTCOME_NETWORK_ERROR(7) AND no REAL transport failure was observed recently
// (g_last_fastdrop_tick, set at the U17 fast-drop broadcast in on_time_tick -- a genuine transport
// death), it overwrites the ONE widget field retail's own switch would have set differently for a
// below-quorum drop: `_G_LLM_UI_OUTCOME_DLG_MESSAGE_WIDGET.label` (+0x38, 0x00650b17) ->
// `G_TEXT_PTRS[0x2dc]`. That widget is shared -- per its own addr-header plate -- between the initial
// modal and the transitional REPORT screen the "Ok" button slides through on the way to the HUD, so
// one poke fixes both. The NOTE/"continue game" text (G_TEXT_PTRS[0x2dd]) is identical for outcome
// 6/7/8 in retail's own switch, so it needs no correction.
//
// RESIDUE, stated rather than hidden: the discriminator is "no fast-drop in the last
// g_fastdrop_recent_ms", which is exactly right for every case the rig has measured (a graceful
// quit's B2 line never appears; U17 fast-drop always precedes a real transport death) but is not a
// proof for a hypothetical byte-corrupted-yet-still-connected link, which nothing in the suite
// exercises today -- link_death (tools/test_ui.py) is a LOBBY-phase blackhole test and never reaches
// this entry point at all, so it cannot regress from this change either way.
constexpr uint8_t OUTCOME_NETWORK_ERROR = 7;    // mirrors libmh/lockstep/rx_dispatch.cpp's own constant
DWORD             g_fastdrop_recent_ms  = 2000; // generous vs. the one-frame gap fast-drop -> its own dialog
uint8_t           g_go_outcome          = 0;    // the real `outcome` byte, captured by gameover_detour from
                                                // AL before pushad; read by both on_gameover_pre/_post

// Run-before the game-over/outcome dialog: leave lockstep so the LOSER shows its result immediately
// (see the block comment at g_sync_gameover). SESSION_MODE is a dword; 3 = lockstep, 2 = MP-local.
// The downgrade is idempotent, which is why the shape below can run it unconditionally: SESSION is
// 3 or it is not. Ordering is unchanged from before U19d: this still runs BEFORE the original body.
void on_gameover_pre() {
    const uint32_t outcome = g_go_outcome;
    if (g_ls_log) {
        char b[160];
        wsprintfA(b, "; on_gameover ENTER sess=%d outcome=%u gclk=%ld (downgrade=%d)\n",
                  (int)*(const uint8_t *)ADDR_SESSION_MODE, outcome, ms_of(ADDR_GAME_CLOCK),
                  (int)(*(volatile uint32_t *)ADDR_SESSION_MODE == 3));
        seam_log(b);
    }
    if (*(volatile uint32_t *)ADDR_SESSION_MODE == 3)
        *(volatile uint32_t *)ADDR_SESSION_MODE = 2; // SESSION_MP_LOCKSTEP -> SESSION_MP_LOCAL
    // SES1: leaving lockstep because the match ENDED is the session's natural close, and it is the
    // one close that happens on BOTH peers at (near) the same step -- which is what makes two
    // directories with the same match_id comparable at their last row.
    mp_session_close("gameover");
}
// U19d -- see the block comment above on_gameover_pre. Runs AFTER the original dialog-setup body
// (gameover_detour is now a WRAP, not a tail jmp), so this corrects what the original just wrote
// rather than trying to influence it.
void on_gameover_post() {
    if (g_go_outcome != OUTCOME_NETWORK_ERROR) return;
    const DWORD now                  = GetTickCount();
    const bool  real_transport_death = g_last_fastdrop_tick != 0 &&
                                      (now - g_last_fastdrop_tick) < g_fastdrop_recent_ms;
    if (real_transport_death) return; // a genuine link death -- leave "Connection to server lost" alone
    void **const label_slot = (void **)(mh::addr::_G_LLM_UI_OUTCOME_DLG_MESSAGE_WIDGET + 0x38);
    void **const text_ptrs  = (void **)mh::addr::cfg_G_TEXT_PTRS;
    *label_slot             = text_ptrs[0x2dc]; // "you_are_the_last_player" -- text-id table row 732
    if (g_ls_log)
        seam_log("; U19d: outcome-dialog said 'Connection to server lost' with no real transport "
                 "failure -- corrected to 'you are the last player'\n");
}
// The pre-U33 naked shape, restored by F1C as the no-gate fallback and the only host since fork
// F2F. U19d turned it from a tail jmp into a WRAP (`call [g_go_tramp]`, not `jmp`): the stolen
// prologue + jmp-back still runs the ORIGINAL dialog-setup body exactly as before, but now RETURNS
// here (the original's own `ret` pops the return address this `call` pushed) so on_gameover_post can
// read/correct what it just rendered. EAX (the original's `return 1`) survives the trailing
// pushad/pushfd .. popfd/popad pair unperturbed, so the caller sees the same return value as before.
__declspec(naked) void gameover_detour() {
    __asm {
        mov  byte ptr [g_go_outcome], al // Watcom __watcall: the byte `outcome` param arrives in AL
        pushad
        pushfd
        call on_gameover_pre
        popfd
        popad
        call dword ptr [g_go_tramp] // WRAP: stolen prologue + jmp to llm_ui_outcome_dialog+8, returns HERE
        pushad
        pushfd
        call on_gameover_post
        popfd
        popad
        ret // EAX (the original's `return 1`) survived both pushad/popad pairs untouched
    }
}

// mp:U19b -- THE QUITTER'S HALF OF THE PARKED PRECONDITION (the D5 rule, seen from the departing peer).
//
// WHAT WAS WRONG. llm_net_player_remove(side) puts _G_LLM_NET_PEER_HORIZON[pidx] on the wire as the
// removal record's horizon (0x0049dd3a), and the receiver (0x0049cb6d: FLD record / FCOMP
// PEER_HORIZON[pidx] / JZ) reads a mismatch as "our views of that peer diverged": it clears every
// other ALIVE&&HUMAN slot and calls llm_strat_player_presence_lost(i, 1) on each (0x0049cb7f..cc02)
// -- the session is over for everyone. For a peer removing ITSELF that slot is the LOCAL one, which
// nothing ever writes (a peer's own horizon lives in _G_LLM_STRAT_LOCKSTEP_HORIZON; commit_horizon
// skips the local slot for exactly that reason): it holds llm_strat_player_param_defaults_init's 10.0
// sentinel for the whole match, while every survivor holds the quitter's real last advert (~3.3 s at
// the U19b drop). So EVERY clean quit took the divergence arm. In a 2-peer match that arm is a no-op
// (the only other human is the quitter, already flagged gone) and the CTL_PLAYER_LEFT that follows
// ended the match anyway, which is why U19 never saw it; with a real third peer it ended BOTH
// survivors' matches the millisecond the record landed (lane F's finding B, 2026-09-22).
//
// WHAT THIS DOES.
//  (1) FREEZE. H_d = max(largest horizon we ever advertised, current HORIZON, clock + lookahead).
//      Advertise it once more, so every survivor's PEER_HORIZON[us] reads exactly H_d, and stop every
//      further advert from this peer (heartbeat + eager, under g_hb_cs so no advert is mid-flight).
//      From here on H_d is the survivors' barrier: their TOTAL clamps to it, their sim runs out of
//      quanta at P = the last sim-step grid point <= H_d (sim_tick's `while (clock + step <= TOTAL)`),
//      and P is the SAME on every peer because every peer's clock walks the same grid -- so it is
//      emulated here from our own clock.
//  (2) WAIT, inside this callback, draining our RX through the retail dispatcher, until every other
//      ALIVE&&HUMAN slot has parked on P. A parked survivor advertises P + its own lookahead, and the
//      lookahead is per-peer under the adaptive controller, so "advert == P + L" is not decidable
//      from here; what IS observable is that a survivor still walking toward P re-advertises a MOVING
//      value every heartbeat (50 ms) while a parked one advertises a CONSTANT. Parked <=> advert >= P
//      and unchanged for PARK_STABLE_MS. A survivor stalled below P by a lagging third one also reads
//      constant -- but that third one is still moving, and when it parks the stalled one moves once
//      more, so the predicate over ALL survivors holds only once all of them sit at P.
//  (3) STAMP. PEER_HORIZON[our slot] = H_d, so the record llm_net_player_remove builds next carries
//      the value every survivor holds -> the receiver takes the AGREE arm (0x0049cc0c):
//      count_active_players() > 1 -> commit_horizon (we leave the min), a peer lighter, the match goes
//      on -- applied at the same sim clock on every survivor, which is what keeps them hash-identical
//      from the drop step on.
// BOUNDED: [net] graceful_leave_park_ms (default 1500) is the safety valve; past it the record goes
// out anyway -- still with the honest H_d, so the AGREE arm is still taken and only the parking is
// unproven -- and the log line says so. [net] graceful_leave_park=0 skips the wait, keeps the stamp.
constexpr DWORD PARK_STABLE_MS = 130; // > 2 heartbeat periods: a walking survivor re-advertises within one

void graceful_leave_park(int side) {
    g_leave_in_progress = true;
    const int me        = mh::hook::call_watcall1(mh::addr::llm_strat_player_by_side_id, (void *)(intptr_t)side);
    double    clk, look, sub, hcur;
    memcpy(&clk, (const void *)ADDR_GAME_CLOCK, sizeof(double));
    memcpy(&sub, (const void *)ADDR_SIM_STEP_INT(), sizeof(double));
    memcpy(&hcur, (const void *)ADDR_LOCAL_HORIZON, sizeof(double));
    if (g_lockstep_step > 0.0) look = g_lockstep_step;
    else memcpy(&look, (const void *)ADDR_STEP_SIZE, sizeof(double));
    double hd = clk + look;
    if (hcur > hd) hd = hcur;
    if (g_hz_max_seen > hd) hd = g_hz_max_seen;

    // (1) freeze -- the final advert and the stop flag are one atomic step against the heartbeat.
    if (g_hb_cs_ready) EnterCriticalSection(&g_hb_cs);
    memcpy((void *)ADDR_LOCAL_HORIZON, &hd, sizeof(double));
    {
        unsigned char pkt[9];
        pkt[0] = 2; // lockstep packet type 2 = EXTEND (horizon advert), the heartbeat's own shape
        memcpy(pkt + 1, &hd, sizeof(double));
        MH_Net_Send(MH_NET_BROADCAST, pkt, 9);
    }
    g_leave_frozen_hd = hd; // mp:U19h: inside the CS with the flag -- the gate below reads both
    InterlockedExchange(&g_leave_frozen, 1);
    if (g_hb_cs_ready) LeaveCriticalSection(&g_hb_cs);

    // P: where the survivors' sim runs out of quanta. Same grid, same arithmetic (`step + clock`, the
    // order sim_tick adds in), bounded because hd >= clk and sub > 0.
    double P = clk;
    if (sub > 0.0)
        for (int n = 0; n < 100000 && P + sub <= hd; ++n) P = sub + P;

    // (2) wait for every survivor to park on P.
    const DWORD t0 = GetTickCount();
    double      last[8];
    DWORD       since[8];
    int         survivors = 0;
    for (int i = 0; i < 8; ++i) {
        memcpy(&last[i], (const void *)(ADDR_PEER_HORIZON() + (unsigned)i * 8u), sizeof(double));
        since[i] = t0;
        if (i != me && slot_is_active_human(i)) ++survivors;
    }
    const char *verdict = g_leave_park ? "VALVE-UNPARKED" : "UNWAITED";
    DWORD       waited  = 0;
    while (g_leave_park) {
        mh::call::llm_net_lockstep_dispatch(); // drain RX: PEER_HORIZON[] + commit, exactly the pump's call
        const DWORD now = GetTickCount();
        waited          = now - t0;
        if (*(const uint8_t *)ADDR_SESSION_MODE != 3) {
            verdict = "SESSION-ENDED"; // something in that RX ended the match for us first
            break;
        }
        survivors = 0;
        bool all  = true;
        for (int i = 0; i < 8; ++i) {
            if (i == me || !slot_is_active_human(i)) continue;
            ++survivors;
            double h;
            memcpy(&h, (const void *)(ADDR_PEER_HORIZON() + (unsigned)i * 8u), sizeof(double));
            if (h != last[i]) {
                last[i]  = h;
                since[i] = now;
            }
            if (h < P || now - since[i] < PARK_STABLE_MS) all = false;
        }
        if (survivors == 0) {
            verdict = "NO-SURVIVORS";
            break;
        }
        if (all) {
            verdict = "PARKED";
            break;
        }
        if (waited > (DWORD)g_leave_park_ms) break; // VALVE-UNPARKED
        Sleep(2);
    }

    // (3) mp:U19h -- REPAIR, RE-ADVERTISE, then stamp. The freeze in (1) silences the two advert
    // paths THIS file owns (the heartbeat and the eager advert), and that was assumed to be all of
    // them. It is not: llm_net_lockstep_dispatch -- which the wait loop above calls in a tight loop
    // for the whole park -- reaches libmh's MSG_KEEPALIVE arm, and on the FIFTH consecutive stall nag
    // that arm fires an emergency horizon bump (rx_dispatch.cpp, `stall_nag_count == 5`):
    //     HORIZON = LOCKSTEP_STEP_SIZE * EMERGENCY_STEP_MUL + GAME_CLOCK
    // with EMERGENCY_STEP_MUL = 2.0. Our clock is frozen, so that is exactly `clk + 2*step` = H_d +
    // one lookahead -- it then advertises it and commits. During the park EVERY survivor keepalive
    // names us (we are the peer they are all stalled on), so the nag counter climbs fast and whether
    // the fifth one lands inside the ~200-250 ms window is a coin flip. That is the whole of mp:U19h:
    // when it lands, every survivor's PEER_HORIZON[us] reads H_d+step while the removal record still
    // carries H_d, the receiver's FCOMP disagrees (rx_dispatch.cpp's handle_peer_drop), and the
    // DISAGREE arm runs eliminate_other_humans -> presence_lost(mode=1) -> outcome 8 -- the survivors'
    // match ends. MEASURED: the quitter's own mh_temporal.log shows LOCAL_HORIZON and COMMITTED both
    // stepping 3290 -> 3320 across the park gap with no frame in between.
    //
    // Three things close it, and none of them is a longer wait -- a longer park is MORE keepalives,
    // so waiting harder makes this strictly likelier:
    //   (a) MH_Seam_GameSend drops any type-2 advert while frozen, so no arm anywhere in the closure
    //       -- named or not yet written -- can put a different horizon on the wire after the freeze.
    //   (b) HORIZON is put back to H_d here, because (a) stops the SEND but the bump has already
    //       written the global, and a drifted HORIZON would re-enter the barrier arithmetic.
    //   (c) the frozen advert is REPEATED immediately before the record goes out. This is the part
    //       that cannot race: MSG_HORIZON is a straight overwrite on the receiver (rx_dispatch.cpp's
    //       MSG_HORIZON case memcpy's it, no max()), llm_net_player_remove's record follows on the
    //       SAME ordered per-connection stream with nothing in between that can send, so the last
    //       thing every survivor applies before the compare is the value the compare will read.
    // (b) also gives us the assertion this run needed: if HORIZON moved, SAY SO, with both values.
    {
        double now_h;
        memcpy(&now_h, (const void *)ADDR_LOCAL_HORIZON, sizeof(double));
        if (now_h != hd) {
            if (g_ls_log) {
                char w[192];
                wsprintfA(w,
                          "; [u19h] HORIZON MOVED during the park: frozen=%ld ms now=%ld ms -- an advert "
                          "path inside the dispatch drain is not frozen; repairing to the frozen value\n",
                          (long)(hd * 1000.0 + 0.5), (long)(now_h * 1000.0 + 0.5));
                seam_log(w);
            }
            memcpy((void *)ADDR_LOCAL_HORIZON, &hd, sizeof(double));
        }
        unsigned char pkt[9];
        pkt[0] = 2; // EXTEND -- the seam's own send, so the type-2 gate in MH_Seam_GameSend (which
                    // only sees the GAME's sends, through llm_net_transport_send) does not touch it
        memcpy(pkt + 1, &hd, sizeof(double));
        MH_Net_Send(MH_NET_BROADCAST, pkt, 9);
    }
    if (me >= 0 && me < 8) memcpy((void *)(ADDR_PEER_HORIZON() + (unsigned)me * 8u), &hd, sizeof(double));
    if (g_ls_log) {
        char b[224];
        wsprintfA(b,
                  "; U19b graceful-leave: survivors %s after %lu ms -- H_d=%ld ms P=%ld ms lookahead=%ld ms"
                  " survivors=%d\n",
                  verdict, (unsigned long)waited, (long)(hd * 1000.0 + 0.5), (long)(P * 1000.0 + 0.5),
                  (long)(look * 1000.0 + 0.5), survivors);
        seam_log(b);
        // mp:U19h DIAGNOSTIC (2026-09-23). H_d alone does not say whether the park's predicate was
        // satisfied by a survivor that had genuinely parked ON H_d or by one that was merely not
        // advertising -- a frozen render reads as a constant advert too, and "constant and >= P"
        // cannot tell the two apart. Dumping the WHOLE slot array we are about to stamp ourselves
        // into, in the same units, puts both halves of the comparison the RECEIVER is about to make
        // (rx_dispatch.cpp's `; [u19h] drop:` line) on one readable line here.
        char row[256];
        int  n = wsprintfA(row, "; [u19h] park exit me=%d stamped=%ld ms adverts:", me,
                           (long)(hd * 1000.0 + 0.5));
        for (int i = 0; i < 8; ++i) {
            double h;
            memcpy(&h, (const void *)(ADDR_PEER_HORIZON() + (unsigned)i * 8u), sizeof(double));
            if (i == me || slot_is_active_human(i))
                n += wsprintfA(row + n, " s%d=%ld%s", i, (long)(h * 1000.0 + 0.5), i == me ? "(me)" : "");
        }
        wsprintfA(row + n, "\n");
        seam_log(row);
    }
}

// U17 (a) run-before llm_game_return_to_main_menu_cb: if we are quitting a RUNNING lockstep game,
// broadcast our own CLEAN removal (subtype 8) with our side_id. llm_net_player_remove flushes the wire
// frame via llm_net_transport_send BEFORE it mutates local state, so it reaches survivors while the
// transport is still up -- which it did NOT until U19 held U40's relink latch across the call; see
// the block comment at that hold. They apply it in dispatch order (determinism-safe). The
// retail teardown body then runs and returns us to the menu. No leader-gate: the quitter authoritatively
// self-removes (exactly one sender). SESSION_MODE byte: 3 = SESSION_MP_LOCKSTEP.
void on_quit_to_menu() {
    // SES1: FIRST, and outside both gates below. Quit-to-menu ends the session whether or not the
    // graceful-leave broadcast is armed and whether or not we were still in mode 3 -- a player who
    // ESCs out of a lobby has left the match just as surely as one who quits a running game, and a
    // session left open here would swallow the next match's menu lines.
    mp_session_close("quit");
    if (!g_graceful_leave) return;
    if (*(const uint8_t *)ADDR_SESSION_MODE != 3) return; // not in a running lockstep game
    int side = *(const int32_t *)mh::addr::_G_LLM_NET_LOCAL_PLAYER_INDEX;
    // U19 -- HOLD U40's RELINK LATCH ACROSS THE BROADCAST, or there is no broadcast.
    //
    // mp_session_close() above ends in client_relink_arm() on a manual client, and
    // llm_net_player_remove reaches the wire through MH_Seam_GameSend, whose FIRST statement is
    // lazy_start() -- the one site allowed to re-enter MH_Net_InitEx on a started transport, and the
    // site that CONSUMES that latch. So the send meant to announce our departure instead tore the
    // link down and handed the frame to a socket that was still handshaking.
    //
    // MEASURED on the rig 2026-09-18, before this guard, with graceful_leave=1: the quitter logged
    // `U40 relink: re-initialising the transport`, then `net: conn 0 closed` (+1 ms), and only then
    // `U17 graceful-leave: broadcast self-removal side=1` (+195 ms); the host's log carries no
    // GameRecv of a removal at all and dropped us through `U17 fast-drop: transport-dead peer`
    // instead. The knob's whole mechanism was a no-op with a side effect -- the departure LOOKED
    // fast only because the accidental socket close woke the B2 catch.
    //
    // Clearing the latch for the length of the call and restoring it after keeps both halves: the
    // removal frame rides the LIVE link and survivors apply it in dispatch order (determinism-safe,
    // through the retail receiver), and the browser still re-dials when the player goes looking for
    // another game. Ordering the broadcast BEFORE mp_session_close would also work and is the
    // smaller diff, but it hands the session-end reason to whichever seam the self-removal trips on
    // the way (a below-quorum self-removal reaches on_gameover), and SES1's reason vocabulary is
    // worth more than three lines.
    const LONG relink_held = InterlockedExchange(&g_net_relink, 0);
    graceful_leave_park(side);                                                        // U19b: freeze our horizon, wait for the survivors to park on it, stamp the record
    mh::hook::call_watcall1(mh::addr::llm_net_player_remove, (void *)(intptr_t)side); // EAX = side_id
    g_leave_in_progress = false;
    if (relink_held) InterlockedExchange(&g_net_relink, relink_held);
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
        // U19b: a peer that has frozen its horizon for a clean quit advertises nothing more -- the
        // write+send below is one step under g_hb_cs, so the freeze can never split them.
        if (g_hb_cs_ready) EnterCriticalSection(&g_hb_cs);
        if (InterlockedCompareExchange(&g_leave_frozen, 0, 0)) {
            if (g_hb_cs_ready) LeaveCriticalSection(&g_hb_cs);
            continue;
        }
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
        if (g_hb_cs_ready) LeaveCriticalSection(&g_hb_cs);
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

// mp:P9 (2026-09-22) -- `resync_trigger_gate`'s BYTE-PATCH CARRIERS, RE-INSTATED. C8-e retired both
// (2026-07-29) on the strength of the promoted body carrying the fix, and every rig run since has
// been configuration (2), where it does. The players' drop-in (`-net.zip`, ruling Q10) is
// configuration (1): no libmh.dll, nothing promoted, and `refuse_uncarried_fix` wrote "THIS RUN
// DOES NOT HAVE THAT FIX" into all 15 field processes of 2026-09-20 while the mode-8 barrier fired
// every ~2 s (the mp:P8 measurement, 2026-09-22). So the fix comes back as a patch for exactly the
// runs the body cannot reach: unpromoted owner -> the splice; promoted owner -> DISPLACED, the body
// carries it (C1's interlock; the same shape `resync_trigger_reset` above has kept all along).
// Mechanism unchanged from the 2026-07 patch: both leader-only `inc dword [RESYNC_TRIGGER_COUNT]`
// sites (SENT @0x49d8cb in llm_net_send_lockstep_keepalive, RECEIVED @0x49c508 in
// llm_net_lockstep_dispatch) become CALL rel32 + NOP into a thunk that replays the INC only while
// SYNC_RETRY_COUNTDOWN < 0x38 -- genuine sustained silence, sync_overlay_show's own predicate -- so
// routine at-horizon nags stop ratcheting the counter toward ACTIVE_PLAYERS*100. Leader-local
// scratch; determinism-safe. The 2026-07 audit tallies are gone with fix_audit (F3D); the
// [resync] watch above is the instrument now.
constexpr uintptr_t ADDR_RESYNC_INC_SENT = 0x0049d8cb;
constexpr uintptr_t ADDR_RESYNC_INC_RECV = 0x0049c508;
const uint8_t       RESYNC_INC_EXPECT[6] = {0xFF, 0x05, 0x91, 0x87, 0xE5, 0x00}; // inc dword ptr [0x00e58791]
// clang-format off
__declspec(naked) void resync_trigger_gate_thunk() {
    __asm {
        push eax
        mov  eax, 0x00e58789        // &SYNC_RETRY_COUNTDOWN (0x3c at reload, drains on real non-advance)
        cmp  dword ptr [eax], 0x38
        jge  skip                   // routine at-horizon wait -> do NOT count
        mov  eax, 0x00e58791        // &RESYNC_TRIGGER_COUNT
        inc  dword ptr [eax]        // genuine sustained silence -> the displaced INC
    skip:
        pop  eax
        ret
    }
}
// clang-format on
void install_resync_trigger_gate() {
    const uintptr_t sites[2]  = {ADDR_RESYNC_INC_SENT, ADDR_RESYNC_INC_RECV};
    const char     *owners[2] = {"llm_net_send_lockstep_keepalive", "llm_net_lockstep_dispatch"};
    int             armed = 0, displaced = 0;
    for (int k = 0; k < 2; k++) {
        if (mh::hook::promoted_owner_of(sites[k])) { // our promoted body carries it -- C1
            ++displaced;
            continue;
        }
        uint8_t repl[6];
        repl[0]     = 0xE8;
        int32_t rel = (int32_t)((uintptr_t)&resync_trigger_gate_thunk - (sites[k] + 5));
        memcpy(repl + 1, &rel, sizeof(rel));
        repl[5] = 0x90;
        if (patch_bytes_guarded(sites[k], RESYNC_INC_EXPECT, repl, 6)) ++armed;
        else refuse_uncarried_fix("resync_trigger_gate", sites[k], owners[k]); // neither carrier: say so
    }
    char b[220];
    // clang-format off
    wsprintfA(b, "; resync-trigger gate: %d/2 INC sites patched (gated on SYNC_RETRY_COUNTDOWN<0x38), %d displaced by promotion, %d MISMATCHED\n", armed, displaced, 2 - armed - displaced);
    // clang-format on
    seam_log(b);
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

// ---- SES1: the numbers the SESSION_BEGIN / SESSION_END records report ---------------------------
//
// It lives in THIS TU because this TU owns them: the game's cumulative stall counter, the two icon
// counters the overlay thunk feeds, and the two pacing pins (lookahead + sim sub-step) that the ini
// set and on_time_tick writes into the game every frame. net_discovery.cpp, which owns the session
// record, can see none of that -- and a second reader of these addresses would be a second answer.
//
// Every argument is optional: the OPEN only wants the two step periods (nothing has run yet, so the
// counters would all read zero), and the CLOSE only wants the counters. Passing null for the half you
// do not want is cheaper than two functions that would drift apart.
//
// The step periods are reported in MILLISECONDS and prefer OUR pinned value over the game global:
// g_lockstep_step is a DLL double with no torn-read window, while STEP_SIZE (0x005d55bc) is only
// 4-aligned -- the same preference horizon_heartbeat_thread makes, for the same reason.
extern "C" void MH_Seam_SessionPacing(long *clock_ms, long *stall, long *icon_calls, long *icon_shown,
                                      int *step_ms, int *sim_step_ms) {
    if (clock_ms) *clock_ms = ms_of(ADDR_GAME_CLOCK);
    if (stall) *stall = (long)*(const int *)ADDR_STALL_COUNT;
    if (icon_calls) *icon_calls = g_icon_calls;
    if (icon_shown) *icon_shown = g_icon_shown;
    if (step_ms) {
        double s = g_lockstep_step;
        if (s <= 0.0) memcpy(&s, (const void *)ADDR_STEP_SIZE, sizeof(double));
        *step_ms = (int)(s * 1000.0 + 0.5);
    }
    if (sim_step_ms) {
        double s = g_sim_step;
        if (s <= 0.0) memcpy(&s, (const void *)ADDR_SIM_STEP_INT(), sizeof(double));
        *sim_step_ms = (int)(s * 1000.0 + 0.5);
    }
}

// ---- mp:U19h: the leave freeze, published to net_seams.cpp ---------------------------------------
//
// net_seams.cpp owns MH_Seam_GameSend, the ONE place every outbound in-game frame the GAME sends
// passes through -- libmh's send_lockstep_extend reaches it via llm_net_transport_send ->
// game_send_detour. Gating there rather than at each advert site is deliberate: the advert paths this
// file knows about are already frozen, and mp:U19h was caused by one it did NOT know about. A gate at
// the wire covers the arms nobody has enumerated, including ones not yet written.
//
// Returns 1 while this peer has frozen its horizon for a clean quit; `out` (optional) receives H_d.
extern "C" int MH_Seam_LeaveFrozenHorizon(double *out) {
    if (!InterlockedCompareExchange(&g_leave_frozen, 0, 0)) return 0;
    if (out) *out = g_leave_frozen_hd;
    return 1;
}

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
    // TL-HARN4: this reads as a STRING (not GetPrivateProfileIntA) only for the atof fractional
    // parse below -- atof already stops at the first non-numeric byte, so a trailing `;comment`
    // was harmless either way; routed through the shared helper anyway for a clean buffer + one
    // less GetPrivateProfileStringA call site to audit.
    mh::config::read_ini_string("net", "lockstep_step_ms", "", step_buf, sizeof(step_buf), g_ini);
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
    // TL-HARN4 (see step_buf's note above -- atof-parsed, so this was already safe either way).
    mh::config::read_ini_string("net", "lockstep_step_eps_ms", "0.01", eps_buf, sizeof(eps_buf), g_ini);
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
    // TL-HARN4 (see step_buf's note above -- atof-parsed, so this was already safe either way).
    mh::config::read_ini_string("net", "sim_step_ms", SHIP_SIM_STEP_MS, sim_buf, sizeof(sim_buf), g_ini);
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
    g_graceful_leave = GetPrivateProfileIntA("net", "graceful_leave", 1, g_ini); // U17 (a) clean-quit self-removal; default ON since U19 (B2 does NOT catch a quit-to-menu -- see g_graceful_leave)
    g_graceful_drop  = GetPrivateProfileIntA("net", "graceful_drop", 1, g_ini);  // U17 (b) fast hard-drop on transport-death; default ON
    // mp:U19b: the quitter freezes its horizon and waits for the survivors to park on it before the
    // self-removal (see graceful_leave_park); the CS serialises that freeze against the heartbeat.
    g_leave_park    = GetPrivateProfileIntA("net", "graceful_leave_park", 1, g_ini);
    g_leave_park_ms = GetPrivateProfileIntA("net", "graceful_leave_park_ms", 1500, g_ini);
    if (!g_hb_cs_ready) {
        InitializeCriticalSection(&g_hb_cs); // DllMain-safe (kernel32 only, no loader re-entry)
        g_hb_cs_ready = true;
    }
    // mp:GS2: game-level peer-data timeout -- drop a peer whose horizon has not moved in this many ms
    // (data-silent, not merely link-silent). <= 0 disables. Default justified beside SHIP_DATA_TIMEOUT_MS.
    g_data_timeout_ms = GetPrivateProfileIntA("net", "data_timeout_ms", SHIP_DATA_TIMEOUT_MS, g_ini);
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
    g_defang_xui               = GetPrivateProfileIntA("net", "defang_xui", g_defang, g_ini);        // freeze fix (=defang_overlay)
    g_resync_wait_fix          = GetPrivateProfileIntA("net", "resync_wait_fix", 1, g_ini);          // defang dep #2 REAL fix (kills the resync busy-wait garbage-spin hang); default ON
    g_resync_trigger_reset     = GetPrivateProfileIntA("net", "resync_trigger_reset", 0, g_ini);     // spurious-resync ROOT fix (option b); default OFF pending validation
    g_resync_trigger_gate      = GetPrivateProfileIntA("net", "resync_trigger_gate", 1, g_ini);      // spurious-resync ROOT fix (option c); DEFAULT ON -- validated 2026-07-25 (0.98x/0 resyncs, det-clean, drop path preserved). Supersedes defang_xui as the freeze fix.
    g_resync_count_init        = GetPrivateProfileIntA("net", "resync_count_init", 1, g_ini);        // mp:P9 ROOT fix: ACTIVE_PLAYER_COUNT recomputed at match start so the threshold is players*100, not 0; DEFAULT ON (see resync_count_init_tick)
    g_resync_receiver_deadline = GetPrivateProfileIntA("net", "resync_receiver_deadline", 1, g_ini); // mp:P9W: a non-leader forces itself out of a leaderless barrier after max(2002, data_timeout_ms) ms; DEFAULT ON (see resync_receiver_deadline_hook)
    // MP U20. DEFAULT OFF pending the measurement it exists to be judged by: the icon storm's
    // proximate cause is the adaptive controller saddling its ceiling and probing down (2026-08-29),
    // and suppressing an icon that a correctly-sized lookahead already stops firing would be hiding a
    // signal for nothing. Reimpl-ONLY -- there is no byte patch, so an unpromoted run cannot carry
    // it; the arming line below says so rather than letting a run believe it is gated.
    g_desync_icon_gate     = GetPrivateProfileIntA("net", "desync_icon_gate", 0, g_ini);
    g_resync_order_horizon = GetPrivateProfileIntA("net", "resync_order_horizon", 1, g_ini); // MP D14: schedule the resync-begin synthetic order at the horizon like every other replicated order; DEFAULT ON
    // MP U19e. DEFAULT ON: without it the leader's re-broadcast of a peer drop overwrites the very
    // datagram it is dispatching (one shared _G_LLM_NET_SEND_BUF for TX and RX), and the parse walks
    // into the payload and raises outcome 7 on a clean quit. Reimpl-ONLY, like desync_icon_gate.
    g_gone_peer_frame_guard = GetPrivateProfileIntA("net", "gone_peer_frame_guard", 1, g_ini);
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
        // U19e. Same footing as the line above -- reimpl-only, no byte-patch carrier -- but its ini
        // default is 1, so an UNPROMOTED run silently has no guard. The arming line below says so.
        fx.gone_peer_frame_guard = g_gone_peer_frame_guard != 0;
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
        // U19e: this one is ON by default, so the informative case is the INERT one -- a run with the
        // wire/turn-engine promotion off has no guard at all (there is no byte patch to fall back on)
        // and its leader will still trample the datagram it re-broadcasts a drop for.
        if (g_gone_peer_frame_guard && !g_lockstep_promoted.active)
            seam_log("; gone_peer_frame_guard=1 but INERT -- the lockstep promotion is OFF this run, "
                     "so dispatch_packet is retail's and a frame from an already-dropped peer still "
                     "overwrites itself (MP U19e)\n");
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
    // mp:P9 (2026-09-22): the two refuse_uncarried_fix calls that stood here since C8-e became the
    // installer again -- it refuses per site itself, for a site that is neither patched nor promoted.
    if (g_resync_trigger_gate) install_resync_trigger_gate();
    if (g_resync_order_horizon) install_resync_order_horizon();
    // mp:P9W: spliced UNCONDITIONALLY (like install_overlay_patches above) -- the thunk always runs
    // the original llm_wait_screen_frame first and self-gates its own extra work on
    // g_resync_receiver_deadline, so an operator flipping the knob off/on needs no re-arm.
    install_resync_receiver_deadline();

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
    if (ls_log || sp_log) {                                              // the path itself is composed per write (SES1: per session) in ls_log_tick
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
        // (SES1: g_ft_path is composed per write in the present hook, so ft_log only sets the gate.)
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

    // Quit-to-main-menu: hook llm_game_return_to_main_menu_cb entry (55 89 E5 68 prologue).
    //
    // INSTALLED UNCONDITIONALLY SINCE U40, AND THE GATE THAT USED TO BE HERE WAS A REAL DEFECT. The
    // site arrived with U17 (a), whose graceful-leave broadcast is opt-in ([net] graceful_leave,
    // default OFF), so the INSTALL inherited that gate. SES1 then hung the session close on the same
    // body -- and in the shipping configuration the body was never reached, so `mp_session_close
    // ("quit")` had never once run: a player who ESCs out of a match to the main menu left the
    // session directory OPEN with no SESSION_END, and every line of the next match landed in the
    // finished one's folder. Measured 2026-09-17 on the rig: a peer that quit through the ESC menu
    // wrote no SESSION_END at all. U40 hangs its own boundary work on the same close, which is how
    // this surfaced. on_quit_to_menu gates the graceful-leave HALF internally, so the knob keeps its
    // meaning exactly; what stops being optional is noticing that the match ended.
    //
    // THE NAMED NEXT VICTIM (U30's census). This site is un-collided only because nothing targets
    // llm_game_return_to_main_menu_cb yet -- and it IS promotable
    // (mh::exp::addr_llm_game_return_to_main_menu_cb exists with no MH_EXPORT_REPLACE bound), so
    // the day something claims it this would have printed the same wrong-build message the
    // gameover site printed for its whole life. It is under the general guard now, by name,
    // BEFORE the collision rather than after it.
    //
    // THE ARM-LOG LINE IS PRINTED FOR THE ABNORMAL CASE ONLY, and the reason is unchanged even
    // though U19 inverted which case that is: tools/data/arm_order/*.json record the healthy arm
    // sequence of five configurations, so a line present in a healthy run is a baseline row, and a
    // new row can only be added together with five real boots to re-record them. While
    // graceful_leave defaulted OFF the healthy run was the silent one and the SUCCESS line was the
    // gated one; since U19 made ON the default it is the other way round, so the line now names a
    // run someone has DISABLED the clean leave on -- which is exactly the configuration a reader of
    // a slow-departure report needs to see. The FAILURE line stays unconditional for the same
    // reason it always was: no baseline contains a refused install.
    {
        const bool armed = install_trampoline(
            mh::addr::llm_game_return_to_main_menu_cb, (void *)quit_to_menu_detour, &g_quit_tramp, 8,
            mh::hook::entry_claim::exclusive, "the quit-to-menu detour (session close + [net] graceful_leave)");
        if (!armed) seam_log("; quit-to-menu NOT armed -- see the [interlock] line for the reason\n");
        else if (!g_graceful_leave)
            seam_log("; U17 graceful-leave DISABLED by config ([net] graceful_leave=0): a quit-to-menu "
                     "will leave survivors waiting out the retail silence timeout\n");
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
