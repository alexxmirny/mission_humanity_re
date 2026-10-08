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
#include "mh_ini_gate.h" // RL2: the ship gate every ini read goes through
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
#include "seams/gone_peer_guard.h"     // mp:U19i gone_peer_frame_guard's byte-patch carrier
#include "include/mh_uidrive_export.h" // MH_UIDrive_OnPresent (ui_drive.cpp) -- UI automation Phase 2
#include "include/mh_harness_export.h" // MH_Harness_RebindSimTick -- C6 sim_tick promotion by rebind
#include "include/mh_log_sink.h"       // LOG1: the async log sink (frametime + lockstep logs)
#include "include/mh_module_bind.h"    // MH_Libmh_OnPresent -- F4D's spine-crossing report
#include "config/config.h"             // F2A: the D11 selector behind the frame pair's promotion default
#include "include/mh_config_dir.h"     // RL3: the config dir mh_key.txt lives in
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
#include "mh_spectate.h"               // mp:U54 -- the spectator roster rules, shared with the libmh twin
#include "addr/mh_export.gen.h"        // entry_llm_strat_time_tick (the C4 direct-install fallback)
#include "hook/export.h"               // install_export_ok
#include "ui/lobby_ui.h"               // D4: the present hook drives two UI-module repaints
// mp:T3. Reached by relative path rather than through an include directory because it is a
// satellite module's header and mh.dll is not that module -- the same shape as
// hostapi_io_bind.cpp's "../../libmh/include/libmh.h". It is header-only so that the three
// projects that need the arithmetic (mh, mh_net_udp, mh_nettest) share ONE definition, and so
// the offline suite that proves it (net_selftest.exe udpstatstest) proves the code mh.dll runs.
#include "../../mh_net_udp/udp_stats.h"       // RFC 6298 / 3393 / 7680 + the lookahead decision
#include "desync/world_sync_core.h"           // mp:X3c -- ff_* arithmetic for the world-resync catch-up
#include "../../mh_net_udp/lookahead_start.h" // mp:P14 -- the START lookahead, seeded from the lobby RTT
#include "../../mh_net_udp/relay_path.h"      // mp:P16 -- the relayed client->host->client delay (3+ peer star)
#include "ui/lobby_ping.h"                    // mp:P16 -- lobby_ping_published_srtt (the host's published SRTT table)
#include "ui/player_strings.h"                // mp:U62 -- the hub-change notice is a table row
#include "seams/adaptive_window.h"            // mp:P15 -- the first window's start (all peers live) + post-spin starved

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

// mp:U19i -- gone_peer_frame_guard's byte-patch carrier (see seams/gone_peer_guard.h for the why).
// Outside the anonymous namespace so net_selftest.exe's gpfgtest drives THIS thunk, not a copy.
namespace mh::gone_peer_guard {

uint8_t  *g_buf      = nullptr; // set at install: live_base(RID_NET_SEND_BUF)
uintptr_t g_kick     = KICK;
unsigned  g_fires    = 0;
uint32_t  g_last_len = 0;

namespace {
uint8_t  g_saved[CAPACITY];
uint32_t g_saved_n    = 0;
int32_t  g_saved_side = 0;

// cdecl helpers the naked thunk calls between PUSHAD/POPAD pairs, so they may clobber EAX/ECX/EDX.
// The save is the reimpl guard's own `n = min(len, CAPACITY)` (rx_dispatch.cpp dispatch_packet).
void __cdecl save(uint32_t len, int32_t side_id) {
    g_saved_n    = len < CAPACITY ? len : CAPACITY;
    g_saved_side = side_id;
    g_last_len   = len;
    if (g_buf) memcpy(g_saved, g_buf, g_saved_n);
}
void __cdecl restore() {
    if (g_buf) memcpy(g_buf, g_saved, g_saved_n);
    // Rare by construction -- only a frame from a peer ALREADY written off, received by the leader --
    // so it is logged, but capped: a gone peer that keeps sending must not flood the session log.
    if (++g_fires <= 4) {
        char b[200];
        wsprintfA(b, "; [net] gone-peer frame guard FIRED #%u: kick re-broadcast for side %d, datagram (%u bytes) restored\n",
                  g_fires, (int)g_saved_side, (unsigned)g_saved_n);
        seam_log(b);
    }
}
} // namespace

// The splice target. Entered exactly as the displaced `call llm_net_send_lockstep_kick` would have
// been: EAX = side_id, EBP = llm_net_lockstep_dispatch's frame, everything else live. The two helper
// calls are bracketed by PUSHAD/POPAD so the caller sees only what the real emitter itself did; the
// emitter is called with the untouched register file, and its return state is what we hand back.
// clang-format off
__declspec(naked) void thunk() {
    __asm {
        pushad
        push eax                      // side_id (for the log line only)
        push dword ptr [ebp - 0x38]   // len -- guarded: the 3 bytes after the call are `mov eax,[ebp-0x38]`
        call save
        add  esp, 8
        popad
        call dword ptr [g_kick]       // the displaced call, EAX = side_id
        pushad
        call restore
        popad
        ret
    }
}
// clang-format on

} // namespace mh::gone_peer_guard

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
// Fine-grained per-group overlay-patch knobs (default from defang_overlay, each overridable
// via [net] defang_tt_wait / defang_tt_sync / defang_xui / defang_dismiss). For isolating WHICH patch kills
// the freeze while keeping the "Player not responding" kick modal (tt_sync) live. See install_overlay_patches.

// ---- P4: make the "de-sync icon storm" a number -------------------------------------------------
// After the first long internet game the player reported "a lot of de-sync icons, felt as micro-
// freezes" -- and there was no way to check, so no pacing experiment could be judged. These count the
// wait-overlay call site: g_icon_calls every time the game WANTS the icon, g_icon_shown every time one
// is actually drawn (they differ only when the gate is armed, which is exactly the gate's value made
// visible). Written by the naked thunk, so plain longs at a fixed address, not statics-in-a-function.
// Diagnostic only: a counter in our DLL is invisible to the sim, so this cannot perturb determinism.
long      g_icon_calls = 0, g_icon_shown = 0;
uintptr_t g_wait_target = 0x004c7dc0; // where wait_overlay_gate_thunk forwards: retail wait_player_overlay_show, or odg::wait_thunk (mp:U44 retail carrier)
long      g_icon_gate   = 0;          // mirrors defang_tt_wait==2 for the thunk (asm cannot read a C++ bool cheaply)
int       g_icon_count  = 1;          // [net] icon_count -- install the counting thunk even when nothing is gated

// U20 (2026-08-30): THE SAME TWO COUNTERS, FED FROM THE PROMOTED BODY. The thunk above can only
// count while llm_strat_time_tick runs the ORIGINAL, and promotion has been the ship default since
// SHIP_PROMOTE_LOCKSTEP -- so on every shipped build these columns read 0 and that zero meant
// "nobody counted", not "no icons". These two are handed to mh::lockstep::set_icon_counters() so the
// reimplemented body feeds the identical longs: same columns, same readers, no third column.
// EXACTLY ONE of the two writers is ever live over the site (C1's interlock guarantees it), so they
// cannot double-count.
void icon_note_wanted() { ++g_icon_calls; }
void icon_note_shown() { ++g_icon_shown; }
int  g_overlay_dialog_guard  = 1; // [net] overlay_dialog_guard -- MP U44; twin + retail call-site splices, default ON
int  g_desync_icon_gate      = 0; // [net] desync_icon_gate -- MP U20; reimpl-only, see reimpl_fixes
int  g_gone_peer_frame_guard = 1; // [net] gone_peer_frame_guard -- MP U19e/U19i; body + byte patch, DEFAULT ON
int  g_undock_reentry_fix    = 1; // [net] undock_reentry_fix -- MP U49; body + byte patch, DEFAULT ON, sim-affecting
int  g_diplo_order_dedup_fix = 1; // [net] diplo_order_dedup_fix -- MP U45; body + byte patch, DEFAULT ON, sim-affecting
int  g_defang_xui            = 0; // extend_ui_enter wait+mode8 pair (the DOMINANT ~2s mode-8 freeze): 0=live 1=NOP
int  g_resync_trigger_gate =
    0; // 1 = gate both RESYNC_TRIGGER_COUNT increments on SYNC_RETRY_COUNTDOWN<0x38 (option c; count only genuine silence)
int g_resync_count_init =
    1;               // 1 (default) = recompute ACTIVE_PLAYER_COUNT at match start so the resync threshold is players*100, not 0 (mp:P9 root fix; see resync_count_init_tick)
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
// (a client's host-conn latches -1). Always on (the `graceful_drop` knob is retired).
// D5: the transport-dead peer latched but NOT yet removed -- held until the sim clock has
// reached the committed horizon (retail's parked precondition). -1 = nothing pending.
int g_pending_dead      = -1;
int g_pending_dead_wait = 0;
int g_pending_dead_max  = 600; // frames (~10 s at 60 fps) before the safety valve fires anyway
// mp:U19b -- the quitter's HALF of the parked precondition. graceful_leave_park (always on):
// before the self-removal, freeze OUR advertised horizon at H_d and wait (inside the quit callback,
// bounded by LEAVE_PARK_MS) until every survivor has parked on it, so all of them apply the
// roster flip at the same sim clock -- the D5 rule, seen from the departing peer. See on_quit_to_menu.
constexpr int LEAVE_PARK_MS     = 1500; // safety valve: send the removal anyway after this long
volatile LONG g_leave_frozen    = 0;    // 1 = no more horizon adverts from this peer (heartbeat + eager)
double        g_leave_frozen_hd = 0.0;  // mp:U19h -- the H_d that freeze advertised; the value every
                                        // survivor must hold when the removal record arrives
bool g_leave_in_progress = false;       // the freeze..teardown window of one quit (on_time_tick's guard)
// max HORIZON sampled (or put on the wire by the eager hook) this match. MAIN THREAD is its only
// writer. mp:D30: it is also the floor the monotone pin holds STEP_SIZE up to (see on_time_tick).
alignas(8) double g_hz_max_seen = 0.0;
// mp:D30 -- the same maximum for what the HEARTBEAT THREAD sent. A separate variable so each has one
// writer (the heartbeat, under g_hb_cs); readers take the max of the two.
alignas(8) double g_hz_hb_sent = 0.0;
// mp:X3c -- THE HORIZON MIRROR (world resync). While a diverged peer catches up after a world import its GAME_CLOCK
// is B steps behind the horizon it has already advertised. Every "is this floor stale?" rule (a sent maximum more
// than 2 s ahead of the clock is a restarted clock) would read that as a new match, reset the floor and let the
// peer advertise a horizon BELOW what it already put on the wire (G297), stalling the host. With the mirror on the
// stale rule never fires and the heartbeat / eager hook advertise the LARGEST horizon any other active human has
// advertised (mh::netstats::mirror_horizon), so the host's committed horizon keeps moving at live pace. Set by
// MH_Lockstep_WorldSyncBegin, cleared when the fast-forward ends.
volatile LONG g_ws_mirror = 0;
inline bool   hz_stale(double clk, double v) { return !g_ws_mirror && mh::netstats::horizon_max_is_stale(clk, v); }
double        ws_mirror_horizon(double floor_h); // defined after slot_is_active_human
double        ws_extend_floor(double h);
void          ws_mirror_upkeep();
// mp:D30 -- the lookahead actually pinned into STEP_SIZE this frame: max(g_lockstep_step,
// max_sent - clock), mh::netstats::monotone_step. Written by on_time_tick under g_hb_cs, read by the
// heartbeat under g_hb_cs and by the eager hook on the main thread. 0 = no pin this match yet.
alignas(8) double g_eff_step = 0.0;
long g_d30_holds             = 0; // frames on which the pin held STEP_SIZE above the target (diag)
// The monotone pin is always on (the `horizon_monotone` knob is retired). The pre-D30 behaviour --
// STEP_SIZE takes the controller's target as-is and the eager hook / heartbeat compute clock + target
// with no floor -- was only ever the control arm G295 demanded for a lockstep change.
bool             g_d30_holding = false;
CRITICAL_SECTION g_hb_cs; // serialises the heartbeat's write+send against the freeze
bool             g_hb_cs_ready = false;
// U19d: GetTickCount() at the last REAL fast-drop broadcast just below (0 = never yet this process).
// Declared here, ahead of on_time_tick, because that is where it is SET; on_gameover_post (further
// down, by the block comment at OUTCOME_NETWORK_ERROR) is where it is READ.
DWORD g_last_fastdrop_tick = 0;

// Optional per-frame lockstep timing log (mh_net.ini [net] lockstep_log=1 -> mh_lockstep.log). One
// line per strategic frame while in mode-3, so freezes show up as large wall-time gaps between rows
// and we can see WHETHER the sim is starved by the peer horizon, the pump cadence, or rx delivery.
// (The g_ls_log gate itself lives in net_internal.h -- net_seams + net_diag read it for DIAG gating.)
bool          g_ls_hdr_done = false; // LOG1: header is in the CURRENT file (was: an open handle)
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
bool          g_ft_log      = false;
bool          g_ft_hdr_done = false; // LOG1: header is in the CURRENT file (was: an open handle)
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
    unsigned char key[MH_KEY_LEN];
    char          hex[MH_KEY_HEX_LEN + 1] = {0};
    int           generated               = 0;
    // F4B: MH_Key_Load is one of the 23 symbols that now live in mh_net.dll, reached through
    // module_bind.cpp's forwarding shim. With no module it answers MH_KEY_OPEN with nothing
    // generated, so this function goes silent -- which is the right answer: a process with no
    // transport has no session to key, and printing first-run key instructions for a multiplayer
    // that cannot start would be advice about a thing that is not there. No guard is needed here
    // because the surface already carries it (mh_net_module.h).
    // RL3: the key lives in the CONFIG directory (portable: beside the exe; else user storage).
    int st = MH_Key_Load(mh::cfgdir::config_dir(), key, hex, &generated);
    if (st == MH_KEY_SECURE && generated) {
        char b[220];
        wsprintfA(b, "; mh_key.txt generated -- send this file (or the line below) to the players joining you:\n;   %s\n", hex);
        seam_log(b);
    } else if (st == MH_KEY_INVALID) {
        seam_log("; mh_key.txt is unreadable/corrupt -- multiplayer will REFUSE to start. Fix or delete it.\n");
    }
}

extern "C" void MH_MP_CaptureBootResidue(void); // launch.cpp -- mp:D45: one-shot boot snapshot of the residue regions
void            hub_leave_poll();               // mp:U62 -- defined with the other handover seams below
void            fo_match_tick();                // mp:U63 -- defined with the other handover seams below
int             g_spectate_mask     = 1;        // [net] spectate_mask (default 1): 0 = never export the spectator bit (the U71 negative control)
bool            g_spec_menu_reached = false;    // mp:U54: a spectator that chose Exit match is back at the idle main menu (the retail flag is not set on that route)
void            spectator_tick();               // mp:U54/U71 -- defined with the retail spectate carriers below

void on_present() {
    // THE MODULE BIND USED TO HAVE A SECOND ARM HERE (F4A's MH_ModuleBind_OnPresent, mechanism B:
    // off the loader lock, on the first rendered frame). F4B deleted it with the spike it armed --
    // mh_net.dll must be bound before MH_Core_Arm_Early, so the DllMain arm is the only one with a
    // subject, and a second arm point nothing calls is the dead-knob shape F4A ruling (a) removes.
    // The mechanism stays RECORDED in docs/dll-split.md as the documented fallback, and this is
    // still where it would go: "the first place that is both off the loader lock and guaranteed to
    // run", which is also why ensure_key_once sits here (minting a key needs LoadLibrary advapi32).
    frame_watchdog_beat();         // mp:P17: stamp the main-thread frame beat the freeze watchdog reads
    MH_MP_CaptureBootResidue();    // mp:D45: one-shot, first idle main-menu frame (cheap when done)
    MH_Libmh_OnPresent();          // F4D's standing arm: report the spine-boundary crossing counters
    ensure_key_once();             // first frame: make sure the host has a key it can share
    mp_session_solo_tick();        // SES8: a single-player match opens its own session directory
    mp_presence_tick();            // RL15: presence.json on a state change (file I/O on a worker thread)
    MH_LogPrune_Start();           // RL5: once -- prune old log folders off the game thread
    MH_MP_DrainRelayNotice();      // mp:R4a: a relay-level notice the UDP module queued -> arm it (main thread)
    mp_lobby_stall_watch();        // mp:X2h: has a modal stopped the lobby-tick drain? (see net_seams.cpp)
    spectator_tick();              // mp:U54/U71: the transport's spectator mask + a spectator that Exit-ed ends its session
    hub_leave_poll();              // mp:U62: a defeated hub that stayed has reached the main menu -> hand the hub over
    fo_match_tick();               // mp:U63: tell the transport whether a match is running (arms the crash failover)
    mh::ui::browser_notice_tick(); // U23: keep the involuntary-exit notice on the browser status line
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
        // mp:D30 -- under g_hb_cs, so this write+send and the heartbeat's are ordered: the second
        // one reads a clock >= the first one's and floors at what the first one wrote, so neither can
        // put a LOWER horizon on the wire after the other (G297).
        if (g_hb_cs_ready) EnterCriticalSection(&g_hb_cs);
        if (InterlockedCompareExchange(&g_leave_frozen, 0, 0)) {
            if (g_hb_cs_ready) LeaveCriticalSection(&g_hb_cs);
            goto eager_done;
        }
        double clock;
        memcpy(&clock, (const void *)ADDR_GAME_CLOCK, sizeof(double));
        double step;
        if (g_eff_step > 0.0) step = g_eff_step; // the monotone pin's value (on_time_tick)
        else if (g_lockstep_step > 0.0) step = g_lockstep_step;
        else memcpy(&step, (const void *)ADDR_STEP_SIZE, sizeof(double));
        double before;
        memcpy(&before, (const void *)ADDR_LOCAL_HORIZON, sizeof(double));
        // mp:D30 -- never below what is already on the wire: HORIZON itself (safe to read here: the
        // main thread writes it, and the heartbeat only under the lock we hold), and both sent maxima.
        double floor_h = before;
        if (g_hz_max_seen > floor_h) floor_h = g_hz_max_seen;
        if (!hz_stale(clock, g_hz_hb_sent) && g_hz_hb_sent > floor_h) floor_h = g_hz_hb_sent;
        double horizon = g_ws_mirror ? ws_mirror_horizon(floor_h)
                                     : mh::netstats::monotone_horizon(clock, step, floor_h);
        memcpy((void *)ADDR_LOCAL_HORIZON, &horizon, sizeof(double)); // keep OUR requested horizon fresh
        // mp:T4 -- send only when this write CHANGED the horizon. This block runs once per present,
        // and an uncapped frame rate (1100-3500 fps measured on the rig) otherwise puts one segment
        // per frame into the UDP transport's 1024-slot window, which filled at a 360 ms round trip
        // and dropped the link. The write above and the commit below still run every frame. Why the
        // key is "HORIZON before this write" and NOT "the last value this path sent" (that version
        // desynced): mh::netstats::advert_should_send (udp_stats.h); dead-ends G294.
        if (mh::netstats::advert_should_send(before, horizon)) {
            unsigned char pkt[9];
            pkt[0] = 2;
            memcpy(pkt + 1, &horizon, sizeof(double)); // type 2 = EXTEND
            MH_Net_Send(MH_NET_BROADCAST, pkt, 9);     // tell peers now (g_conn_cs-locked)
        }
        if (horizon > g_hz_max_seen && !hz_stale(clock, horizon)) g_hz_max_seen = horizon;
        if (g_hb_cs_ready) LeaveCriticalSection(&g_hb_cs);
        ((void (*)())ADDR_COMMIT_HORIZON_FN)(); // raise OUR committed THIS frame
    }
eager_done:
    if (!g_ft_log) return;
    // SES1: per-SESSION, handle swapped on a boundary so the header lands in each file (see ls_log_tick).
    // LOG1: no handle is held here any more -- rows are ENQUEUED to the async sink. "Handle swapped on
    // a boundary" became "header re-emitted on a boundary" (g_ft_hdr_done).
    if (mh_run_path(g_ft_path, MAX_PATH, "%smh_frametime.log", &g_ft_gen)) g_ft_hdr_done = false;
    if (!g_ft_hdr_done) {
        if (g_ft_path[0] == '\0') {
            g_ft_log = false;
            return;
        }
        g_ft_hdr_done = true;
        QueryPerformanceFrequency(&g_qpc_freq);
        char h[96];
        // D22: no qpc_freq field here -- this column is ALREADY converted to microseconds below
        // (t.QuadPart * 1e6 / g_qpc_freq), so the raw tick rate is not a divisor for it and a
        // reader who printed it and divided qpc_us by it would get a timeline ~10x too short.
        // The column name states the actual unit; that is the whole contract.
        int hn = wsprintfA(h, "# qpc_us game_mode\n");
        mh_logq_write(g_ft_path, h, hn);
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
    if (g_ft_prev_us < 0) {
        // First present of this file: no interval yet to judge or window. Log it unconditionally --
        // it anchors find_lobby_clip_qpc's qpc_us/wall-clock join (mp_analyze.py), which needs row 0.
        char line[64];
        int  n = wsprintfA(line, "%I64d %d\n", us, (int)*(const uint8_t *)ADDR_GAME_MODE);
        mh_logq_write(g_ft_path, line, n);
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
            mh_logq_write(g_ft_path, line, n);
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
        mh_logq_write(g_ft_path, agg, an);
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
DWORD                        g_late_blocked[LS_LATE_PEERS] = {0};
double                       g_late_last_h[LS_LATE_PEERS]  = {0.0};
// mp:P15 wave 7 -- a slot's first read in a match SEEDS g_late_last_h, it is not a move
// (mh::adwin::horizon_observe). Reset per match with the rest.
bool  g_late_seen[LS_LATE_PEERS]      = {false};
DWORD g_late_last_move[LS_LATE_PEERS] = {0};
long  g_late_prev_clk                 = -1;
DWORD g_late_snap_t                   = 0;
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
// ADDR_PEER_HORIZON directly, so this runs in CONFIGURATION (1) too -- where gone_peer_frame_guard
// was INERT until mp:U19i's byte patch, mp:GS1's scope), and it only moves when the peer's SIM actually
// advances its horizon -- a keepalive-only link (the field's 22-35 s since_rx freezes, GS1/RM1)
// leaves it frozen while the transport stays "up".
int   g_data_timeout_ms            = SHIP_DATA_TIMEOUT_MS;
DWORD g_gs2_credit_tick            = 0;       // mp:U68 -- GetTickCount() of the last GS2 drop: the survivors' silence restarts there (0 = none)
bool  g_gs2_dropped[LS_LATE_PEERS] = {false}; // latched per slot per match -- never re-fire on an already-dropped side

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
// mp:P12 -- whether the controller has made its first post-warm-up decision this match. Cleared with
// g_ad_warm_t0 (same reason: a new match is a new join); read by adaptive_tick as `first_warm`.
bool g_ad_decided = false;
// mp:P15 (wave 7) -- the ALL-PEERS-LIVE latch (seams/adaptive_window.h (1)). Stamped by lateness_tick
// the first frame every other ALIVE && HUMAN player's horizon has moved this match; cleared by the
// match reset below with g_ad_warm_t0. `g_ad_live_restart` tells adaptive_tick to restart its window
// on that frame, so the first decided window never holds a frame from before every peer was live.
constexpr DWORD ALL_LIVE_TIMEOUT_MS = 5000; // == LATE_LIVE_MS: a roster miscount cannot switch the controller off
DWORD           g_ad_all_live_t     = 0;
DWORD           g_ad_first_live_t   = 0;
bool            g_ad_live_restart   = false;
// mp:P15 (wave 7) -- the POST-rx_spin starved figure (adaptive_window.h (2)). adaptive_tick leaves this
// frame's clamped interval in g_ad_frame_dt; adaptive_post_spin_tick charges it after rx_spin.
mh::adwin::PostSpinWindow g_ad_post;
DWORD                     g_ad_frame_dt = 0;

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
// Only while the controller is running: with the controller off (a pinned lockstep_step_ms) nothing consumes the samples, and
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

// mp:P15 (wave 7) -- the ALL-PEERS-LIVE latch. `live[]` is lateness_tick's own per-slot test (the
// horizon MOVED within LATE_LIVE_MS -- an unused slot keeps retail's 10 s sentinel). Expected = every
// other ALIVE && HUMAN player (llm_net_lockstep_count_active_players, the count mp:P9's count_init
// already trusts at match start). Writes, once per match on EVERY peer:
//   `; [adaptive] all-peers-live t=<tick> peers=L/E (first live +X ms)`        (or `... TIMEOUT ...`)
// -- the line mp:P15's first-window clause is measured from. Called only until it latches.
void all_live_tick(const bool *live, DWORD now) {
    int n_live = 0;
    for (int i = 0; i < LS_LATE_PEERS; ++i)
        if (live[i]) ++n_live;
    if (n_live > 0 && g_ad_first_live_t == 0) g_ad_first_live_t = now ? now : 1;
    const int                expected = mh::adwin::live_peers_expected(mh::call::llm_net_lockstep_count_active_players());
    const mh::adwin::AllLive v =
        mh::adwin::all_live_latch(n_live, expected, g_ad_first_live_t != 0,
                                  g_ad_first_live_t ? (uint32_t)(now - g_ad_first_live_t) : 0u, ALL_LIVE_TIMEOUT_MS);
    if (v == mh::adwin::AL_NOT_YET) return;
    g_ad_all_live_t   = now ? now : 1;
    g_ad_live_restart = true;
    if (g_ls_log) {
        char b[192];
        wsprintfA(b, "; [adaptive] all-peers-live t=%lu peers=%d/%d (first live +%lu ms)%s\n", now, n_live, expected,
                  (unsigned long)(now - g_ad_first_live_t),
                  v == mh::adwin::AL_TIMEOUT ? " TIMEOUT -- roster count never reached, latched anyway" : "");
        seam_log(b);
    }
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
            g_late_seen[i]      = false; // mp:P15: the first read of the new match seeds again
            g_late_last_move[i] = 0;
            g_gs2_credit_tick   = 0;
            g_gs2_dropped[i]    = false; // mp:GS2 -- a new match is a new peer set, not a residue of the last one
        }
        g_late_have = false;
        g_late_peer = -1;
        g_late_n    = 0;
        g_slack_w.reset();
        g_slack_p50       = 0;
        g_ad_warm_t0      = 0;     // mp:T3c: a new match is a new join, so the warm-up runs again
        g_ad_decided      = false; // mp:P12: ...and its first decision is a first decision again
        g_ad_all_live_t   = 0;     // mp:P15: ...and "every peer is live" has to be observed again
        g_ad_first_live_t = 0;
        g_ad_live_restart = false;
    }
    const bool advanced = (g_late_prev_clk >= 0 && clk_ms != g_late_prev_clk);
    g_late_prev_clk     = clk_ms;

    const int me = MH_Net_LocalPlayerId();
    double    h[LS_LATE_PEERS];
    bool      live[LS_LATE_PEERS];
    int       bind = -1;
    for (int i = 0; i < LS_LATE_PEERS; ++i) {
        memcpy(&h[i], (const void *)(ADDR_PEER_HORIZON() + (unsigned)i * 8u), sizeof(double));
        if (mh::adwin::horizon_observe(g_late_seen[i], g_late_last_h[i], h[i])) g_late_last_move[i] = now ? now : 1;
        // See the LATE_LIVE_MS note above: MOVEMENT is what tells a peer from an empty slot holding
        // retail's 10-second sentinel, and it retires a departed peer for free.
        live[i] = mh::adwin::slot_live(i == me, h[i], g_late_last_move[i], now, LATE_LIVE_MS);
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
    if (g_ad_all_live_t == 0) all_live_tick(live, now); // mp:P15 -- once per match, then free

    if (com < required) { // horizon cannot fund the next sub-step: we are blocked, on `bind`
        if (bind >= 0) {  // open, or split at LATE_STALL_SPLIT_MS (adaptive_window.h (3))
            uint32_t since = (uint32_t)g_late_blocked[bind];
            int      s     = 0;
            if (mh::adwin::episode_blocked_top(since, now, LATE_STALL_SPLIT_MS, &s)) g_late_w[bind].push(s);
            g_late_blocked[bind] = since;
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
    // LOG1: ENQUEUED to the async sink; "a rebuild closes the handle" is now "a rebuild re-emits the header".
    if (mh_run_path(g_ls_path, MAX_PATH, "%smh_lockstep.log", &g_ls_gen)) g_ls_hdr_done = false;
    if (!g_ls_hdr_done) {
        if (g_ls_path[0] == '\0') {
            g_ls_log = false;
            return;
        }
        g_ls_hdr_done = true;
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
        mh_logq_puts(g_ls_path, hdr);
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
    char line[560];
    int  n = wsprintfA(line, "%lu %ld %ld %ld %ld %ld %ld %ld %d %d %ld %ld %lu "
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
    mh_logq_write(g_ls_path, line, n);
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
// Why a LOCAL controller can be safe, and the condition it needs: COMMITTED = min(local_horizon,
// peer_horizons), and the sim clamps to COMMITTED. So a peer's lookahead only gates ITS OWN
// willingness to run ahead, and the two peers need not agree on it -- no agreement protocol, no
// cross-peer messages (contrast sim_step_ms, which changes the sim and therefore must match).
// THAT IS TRUE FOR PACING ONLY. It is NOT true of a SHRINK applied as-is: every horizon writer
// computes GAME_CLOCK + STEP_SIZE, and orders are stamped from that horizon, so cutting the target
// straight into STEP_SIZE lowers the horizon below a value the peer already holds as COMMITTED. The
// next order then arrives after the peer stepped past its exec_time and runs a step late there -- a
// desync (mp:D30, dead-ends G297; it re-converged on the rig only because the synth order repeats).
// So the target set here is NOT pinned directly: on_time_tick's monotone_pin() holds STEP_SIZE at
// max(target, max_horizon_sent - clock) (mh::netstats::monotone_step), and a shrink takes effect as
// the clock catches up. With that pin the claim above holds.
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
// mp:P12 reuses this weight for the uncapped FIRST grow (udp_stats.h's note): one number for "a tail
// heavy enough to act on before the window is out", not two that can drift apart.
static_assert(AD_FAST_MIN_SAMPLES == mh::netstats::AD_FIRST_GROW_MIN_SAMPLES, "one sample gate");
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
int              g_adaptive        = 0;   // the adaptive lookahead controller (SHIP_ADAPTIVE unless lockstep_step_ms pins it)
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

// mp:P14 -- the START lookahead is seeded from the RTT the transport measured in the lobby
// (mh_net_udp/lookahead_start.h carries the formula, the sample floor and the evidence). Once per
// PROCESS, at the first live-lockstep frame of its first match: after that the value is the
// controller's own measurement of the link and is carried on purpose (mp:P11: a carried value that is
// too high is shrunk back, one too low is grown into, and re-deriving it costs a cold walk-up through
// starvation). `g_ad_seed_ok` is false when the operator PINNED the
// lookahead (an explicit lockstep_step_ms, adaptive or not): a pin is a starting point the operator
// chose, and every sweep that set one (P11's start-300 arms) must keep meaning what it meant.
bool g_ad_seed_ok   = false;
bool g_ad_seed_done = false;
// mp:P14 -- the first DECIDED window of each match always leaves a line (`; [adaptive] first-window`),
// even when its verdict does not move the value. That window is the one the opening stall lives in,
// and with a correct start the regular line -- written only on a CHANGE -- would stay silent exactly
// when the start was right, leaving the rig nothing to read the clause off.

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
//
// mp:P16 -- THE SINGLE-ENTRY FALLBACK IS THE HOST LINK, which is only the binding peer's path when the
// binding peer IS the host. On a star client bound by ANOTHER client the horizon flew own->host->other,
// so the host link alone under-corrects by the whole second leg (0.72x on the first 3-peer match). The
// arithmetic, and why it is what it is, is mh_net_udp/relay_path.h; this function only gathers its
// inputs: the transport's rows and the host's published per-client SRTT (lobby_ping_published_srtt).
// `[net] lockstep_relay_path=0` restores the pre-P16 lookup bit for bit (the negative arm).
int g_relay_path = 1; // [net] lockstep_relay_path -- mp:P16; read in MH_Lockstep_Init next to the adaptive controller setup

// mp:P16 -- the host's published SRTT table as the controller reads it. During a match the host
// republishes at ~1 Hz, so anything older than this is a host that stopped (or an old build that
// never did); the table then contributes nothing and the lookup is the pre-P16 one.
constexpr unsigned RELAY_PUB_MAX_AGE_MS = 10000;
// The SEED reads the table once, at the first live-lockstep frame -- the last summary is the lobby's,
// which can be tens of seconds old after a map load. A lobby RTT is still the right number to seed
// from (it is what the own-link seed already uses), so the age bound is generous.
constexpr unsigned RELAY_SEED_MAX_AGE_MS = 120000;

void relay_pub_table(double *pub, unsigned max_age_ms) {
    for (int i = 0; i < mh::netstats::RELAY_MAX_PEERS; ++i) {
        int ms = 0;
        pub[i] = mh::ui::lobby_ping_published_srtt(i, max_age_ms, &ms) ? (double)ms : -1.0;
    }
}

double binding_peer_owd_ms() {
    if (g_late_peer < 0) return -1.0;
    MH_NetStats s;
    MH_Net_GetStats(&s);
    if (!s.lat_supported || s.lat_count <= 0) return -1.0;
    mh::netstats::RelayLatRow rows[MH_NET_MAX_PEERS];
    int                       n = s.lat_count < MH_NET_MAX_PEERS ? s.lat_count : MH_NET_MAX_PEERS;
    for (int i = 0; i < n; ++i) {
        rows[i].player_id = s.lat[i].player_id;
        rows[i].samples   = s.lat[i].samples;
        rows[i].srtt_us   = s.lat[i].srtt_us;
        rows[i].rttvar_us = s.lat[i].rttvar_us;
    }
    double pub[mh::netstats::RELAY_MAX_PEERS];
    // Only a client with one row can use the table, and only a client bound by another client; do not
    // even touch it otherwise (keeps the host and the 2-player paths exactly as they were).
    // mp:U62: "bound by the hub" asks the transport who the hub IS (id 0 until a handover moves it).
    MH_NetHubStatus hub;
    MH_Net_HubStatus(&hub);
    const int  hub_id  = hub.hub_id >= 0 ? hub.hub_id : 0;
    const bool use_pub = g_relay_path && n == 1 && rows[0].player_id < 0 && g_late_peer != hub_id;
    if (use_pub) relay_pub_table(pub, RELAY_PUB_MAX_AGE_MS);
    return mh::netstats::binding_owd_ms(rows, n, g_late_peer, use_pub ? pub : 0, g_relay_path != 0, hub_id);
}

// mp:P14 -- apply the lobby-RTT seed. Runs from adaptive_tick's first live-lockstep frame, i.e. BEFORE
// this frame's monotone_pin and before any decision; a horizon already advertised at the old start
// is harmless both ways (the pin holds a lower target up to it, and a higher one is immediate).
void adaptive_seed_start() {
    g_ad_seed_done = true;
    if (!g_ad_seed_ok) return;
    MH_NetStats s;
    MH_Net_GetStats(&s);
    double srtt[MH_NET_MAX_PEERS];
    int    smp[MH_NET_MAX_PEERS];
    int    n = s.lat_supported ? s.lat_count : 0;
    if (n > MH_NET_MAX_PEERS) n = MH_NET_MAX_PEERS;
    for (int i = 0; i < n; ++i) {
        srtt[i] = (double)s.lat[i].srtt_us / 1000.0;
        smp[i]  = s.lat[i].samples;
    }
    // mp:P16 -- a star client's one row is the HOST link; the other clients are further, by their own
    // host legs. Adds the path RTTs (own + host->other) as extra candidates; lookahead_start takes the max.
    if (g_relay_path && n == 1 && s.lat[0].player_id < 0) {
        double pub[mh::netstats::RELAY_MAX_PEERS];
        relay_pub_table(pub, RELAY_SEED_MAX_AGE_MS);
        MH_NetHubStatus hub; // mp:U62: skip the HUB's published row (it is the own row), whichever id the hub has
        MH_Net_HubStatus(&hub);
        n = mh::netstats::seed_with_relay_paths(srtt, smp, n, MH_NET_MAX_PEERS, MH_Net_LocalPlayerId(), pub, true,
                                                hub.hub_id >= 0 ? hub.hub_id : 0);
    }
    // The controller's own effective floor (AD_SIM_FLOOR_MULT sub-steps). The configured sub-step is
    // preferred to the global: on_time_tick pins SIM_STEP_INT from g_sim_step only AFTER this call.
    double sim_s = g_sim_step;
    if (sim_s <= 0.0) memcpy(&sim_s, (const void *)ADDR_SIM_STEP_INT(), sizeof(double));
    double floor_s = g_ls_min;
    if (AD_SIM_FLOOR_MULT * sim_s > floor_s) floor_s = AD_SIM_FLOOR_MULT * sim_s;
    const double                          prev_ms = g_lockstep_step * 1000.0;
    const mh::netstats::LookaheadStartOut o =
        mh::netstats::lookahead_start(srtt, smp, n, s.lat_supported != 0, floor_s * 1000.0, g_ls_max * 1000.0,
                                      prev_ms);
    if (o.reason != mh::netstats::LS_START_NO_RTT) g_lockstep_step = off_grid_ms(o.start_ms) / 1000.0;
    if (g_ls_log) {
        char b[256];
        if (o.reason == mh::netstats::LS_START_NO_RTT)
            wsprintfA(b,
                      "; [adaptive] start %ld ms fallback -- no lobby RTT (%s, %d peer(s), need %d samples)\n",
                      (long)prev_ms, s.lat_supported ? "too few samples" : "transport cannot measure", n,
                      mh::netstats::AD_START_MIN_RTT_SAMPLES);
        else
            wsprintfA(b,
                      "; [adaptive] start %ld ms from rtt %ld ms (peer slot %d, %d samples, %s; floor %ld + "
                      "0.625 x srtt, ceiling %ld; was %ld)\n",
                      (long)(g_lockstep_step * 1000.0), (long)(o.rtt_ms + 0.5), o.peer, o.samples,
                      o.reason == mh::netstats::LS_START_SEEDED ? "seeded" : "partial", (long)(floor_s * 1000.0),
                      (long)(g_ls_max * 1000.0), (long)prev_ms);
        seam_log(b);
    }
}

void adaptive_tick() {
    // mp:P16 -- the HOST keeps the per-slot SRTT table live during the match (it was lobby-only): it is how
    // a star client learns the OTHER client's host leg. Before the adaptive gate: a host pinned to a
    // fixed lookahead still has adaptive clients to serve.
    // mp:U62: "the hub" is no longer "player 0" -- after a handover the hub is whichever survivor took over, and
    // it must keep publishing. A peer holding more than one connection IS a hub (a client holds exactly one).
    if (g_relay_path && *(const uint8_t *)ADDR_SESSION_MODE == 3 && MH_Net_IsStarted() && MH_Net_PeerCount() > 1)
        mp_ping_publish_tick();
    if (!g_adaptive) return;
    if (*(const uint8_t *)ADDR_SESSION_MODE != 3) return;       // live lockstep only
    if (!MH_Net_IsStarted() || MH_Net_PeerCount() <= 0) return; // solo: nothing to tune against
    if (!g_ad_seed_done) adaptive_seed_start();                 // mp:P14 -- once per process
    double clk, com, sim;
    memcpy(&clk, (const void *)ADDR_GAME_CLOCK, sizeof(double));
    memcpy(&com, (const void *)ADDR_COMMITTED(), sizeof(double));
    memcpy(&sim, (const void *)ADDR_SIM_STEP_INT(), sizeof(double));
    if (sim <= 0.0) return;
    DWORD now = GetTickCount();
    // mp:P15 (wave 7) -- the first window is measured from the moment ALL peers are live: restart the
    // window (and drop the lateness samples taken against a partial peer set) on the latch frame.
    if (g_ad_live_restart) {
        g_ad_live_restart = false;
        g_ad_t0           = now ? now : 1;
        g_ad_last_ms      = 0; // this frame opens the window; it has no interval inside it
        g_ad_frames = g_ad_starved = 0;
        g_ad_time_ms = g_ad_starved_ms = 0;
        g_ad_post.reset();
        lateness_consume();
    }
    // The starved-time diagnostic (see the g_ad_last_ms note): accumulated exactly as before, read
    // by nothing but the log line below. It is the PRE-rx_spin view; mp:P15's post-spin figure
    // (g_ad_post, charged by adaptive_post_spin_tick) is the one the first-window clause reads.
    const bool starved = mh::adwin::blocked(clk, com, sim);
    ++g_ad_frames;
    if (starved) ++g_ad_starved;
    g_ad_frame_dt = 0;
    if (g_ad_last_ms) {
        DWORD dt = now - g_ad_last_ms;
        if (dt > AD_MAX_SAMPLE_MS) dt = AD_MAX_SAMPLE_MS;
        g_ad_time_ms += dt;
        if (starved) g_ad_starved_ms += dt;
        g_ad_frame_dt = dt;
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
    // mp:P15 (wave 7): ...and not before every peer is live (adaptive_window.h (1)).
    if (g_ad_warm_t0 == 0 && mh::adwin::warm_anchor_ready(g_late_have, g_ad_all_live_t != 0))
        g_ad_warm_t0 = now ? now : 1;
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
    // DETERMINISM. This moves g_lockstep_step, the TARGET lookahead. It does not reach STEP_SIZE
    // directly: on_time_tick's monotone_pin() pins max(target, max_horizon_sent - clock), because a
    // shrink written straight into STEP_SIZE LOWERED the advertised horizon and desynced orders
    // (mp:D30, dead-ends G297 -- the earlier "the step sequence is unchanged however the two peers'
    // values differ" was true of pacing and false of orders). With the pin, a peer's lookahead gates
    // only its own willingness to run ahead. Nothing here reads or writes sim state; the error term is
    // measured from PEER_HORIZON, an input to the sim's clamp and not an output of it.
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
    in.samples                         = g_late_n;              // mp:P12
    in.first_warm                      = warm && !g_ad_decided; // mp:P12
    const mh::netstats::LookaheadOut d = mh::netstats::lookahead_decide(in);
    g_ad_clean                         = d.clean_out;
    // mp:P12 -- a window that judged SOMETHING (not warm-up, not an empty tail) spends the first
    // decision, whatever its verdict: a first window that holds leaves later grows capped.
    const bool first_decided = warm && d.verdict != mh::netstats::LA_NO_SAMPLES && !g_ad_decided;
    if (warm && d.verdict != mh::netstats::LA_NO_SAMPLES) g_ad_decided = true;
    if (first_decided && g_ls_log) {
        // mp:P14 -- the opening window, whatever it decided (see g_ad_seed_done's note). Deliberately
        // NOT the `lookahead A -> B ms` shape, so mp_pacing_report's move count does not read it.
        // mp:P15 (wave 7) -- `post-spin A/B ms` is the clause's figure (adaptive_window.h (2)); `window
        // from all-live +W ms` says where this window opened relative to the all-peers-live line, so a
        // reader can check it never opened before it.
        char b[320];
        wsprintfA(b,
                  "; [adaptive] first-window t=%lu at %ld ms -> %ld ms %s%s (tail95 %d ms, n=%d, peer=%d; "
                  "starved %ld/%ld ms diag; post-spin %lu/%lu ms; window from all-live +%lu ms)\n",
                  now, (long)(cur * 1000.0), (long)off_grid_ms(d.want_ms),
                  (d.verdict == mh::netstats::LA_GROW)     ? "grow"
                  : (d.verdict == mh::netstats::LA_SHRINK) ? "shrink"
                                                           : "hold",
                  d.first_full ? " uncapped" : "", g_late_tail95, g_late_n, g_late_peer, (long)g_ad_starved_ms,
                  (long)g_ad_time_ms, (unsigned long)g_ad_post.starved_ms, (unsigned long)g_ad_post.time_ms,
                  (unsigned long)(g_ad_t0 - g_ad_all_live_t));
        seam_log(b);
    }

    const double want_ms = off_grid_ms(d.want_ms);
    const double want    = want_ms / 1000.0;
    if (want != cur) {
        g_lockstep_step = want; // the TARGET: on_time_tick's monotone_pin() decides what reaches STEP_SIZE
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
    g_ad_post.reset();  // mp:P15: this frame's own post-spin interval then opens the next window
    lateness_consume(); // the next decision is made on samples taken after this one -- see there
}

// mp:P15 (wave 7) -- the POST-rx_spin half of the starved accounting (adaptive_window.h (2)). Called
// from on_time_tick right after rx_spin_until_horizon: charges the interval adaptive_tick measured for
// this frame to g_ad_post, as starved only if COMMITTED still cannot fund the next sub-step after the
// spin drained RX. On a window's deciding frame the interval lands in the NEXT window (the decision
// has already been taken) -- a one-frame shift, bounded by AD_MAX_SAMPLE_MS.
void adaptive_post_spin_tick() {
    if (!g_adaptive || g_ad_frame_dt == 0) return;
    double clk, com, sim;
    memcpy(&clk, (const void *)ADDR_GAME_CLOCK, sizeof(double));
    memcpy(&com, (const void *)ADDR_COMMITTED(), sizeof(double));
    memcpy(&sim, (const void *)ADDR_SIM_STEP_INT(), sizeof(double));
    g_ad_post.add(g_ad_frame_dt, mh::adwin::blocked(clk, com, sim));
    g_ad_frame_dt = 0;
}

// mp:P15 (user decision 2026-09-25) -- END the lateness blocked episode after rx_spin when the spin
// FUNDED the step (adaptive_window.h (3), dead-ends G314). Without it a peer phase-locked to its
// partner's EXTENDs never ended an episode at the top of a frame and fed the tail a synthetic -2000
// every LATE_STALL_SPLIT_MS. Runs whether or not the controller is on, like lateness_tick. The netind
// stall timer reads the same g_late_blocked, so it stops showing a phantom stall for that shape too.
void lateness_post_spin_tick() {
    if (!MH_Net_IsStarted() || MH_Net_PeerCount() <= 0) return;
    double clk, com, sim;
    memcpy(&clk, (const void *)ADDR_GAME_CLOCK, sizeof(double));
    memcpy(&com, (const void *)ADDR_COMMITTED(), sizeof(double));
    memcpy(&sim, (const void *)ADDR_SIM_STEP_INT(), sizeof(double));
    if (sim <= 0.0) return;
    const bool  funded = !mh::adwin::blocked(clk, com, sim);
    const DWORD now    = GetTickCount();
    for (int i = 0; i < LS_LATE_PEERS; ++i) {
        uint32_t since = (uint32_t)g_late_blocked[i];
        int      s     = 0;
        if (mh::adwin::episode_end_post_spin(since, funded, now, &s)) g_late_w[i].push(s);
        g_late_blocked[i] = since;
    }
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
        char b[192];
        if (in_progress) {
            g_rt_barrier_t0 = now;
            // mp:P9D: `count was` is the PREVIOUS frame's sample (g_rt_last_count) and `live` is the
            // counter read THIS frame. force_resync zeroes the count as its first act, so a real fire
            // reads `count was 199, live 0` -- the sampler straddled two keepalive increments, the
            // trigger did not fire low -- and says so on one line without P9C to explain it. `live`
            // sits AFTER `count was` so every parser that keys on `BEGIN (count was` still matches,
            // and the parsers take it as an optional group so archived logs still parse.
            wsprintfA(b,
                      "; [resync] barrier #%u BEGIN (count was %u, live %u, threshold %u, countdown %d) "
                      "flags=0x%02x\n",
                      ++g_rt_barriers, (unsigned)g_rt_last_count, (unsigned)count, (unsigned)threshold,
                      countdown, flags);
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
// Always on (the `resync_receiver_deadline` knob is retired).
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
    mh::call::llm_wait_screen_frame();              // the original body -- leader exit + every peer's dispatch, unchanged
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
    else
        seam_log("; resync_receiver_deadline armed: a non-leader leaves the mode-8 wait screen after "
                 "max(2002, data_timeout_ms) ms with no RESUME\n");
}

// mp:GS2 -- game-level peer-data timeout. Called from on_time_tick right after lateness_tick(), so
// g_late_last_h[]/g_late_last_move[] are this frame's fresh values (same ones the lookahead's own
// live/dead call used). A frozen peer's SIM stops moving its horizon forward while its TRANSPORT can
// keep answering keepalives (mp:GS1/RM1's field freezes: since_rx 22-35 s, link never drops) -- this
// runs the SAME direct removal U17(b)'s transport-death fast-drop uses (llm_net_player_remove by
// side_id), so it works whether or not lockstep is promoted: CONFIGURATION (1) is the mode
// gone_peer_frame_guard could not help before mp:U19i (the guard lived only in the reimpl
// dispatch_packet libmh never runs there), while this reads ADDR_PEER_HORIZON directly and calls a
// real game function.

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

// ==== mp:U54 -- SPECTATE AFTER DEFEAT: the shared predicates (the libmh twin and the retail byte-patch carriers read the
// SAME rules, mh_spectate.h) =======================================================================================
//
// A defeated human in a lockstep match with two or more OTHER humans still alive is not dropped: it stays in the session as a
// SPECTATOR (its HUMAN bit off, DEFEATED|GONE on -- the flip every survivor makes for it since U56 -- its session NOT
// downgraded, its own orders dropped). [net] spectate_after_defeat (default 1; needs player_left_pin_fix, resolved at ini read).
int          g_spectate_after_defeat = 1;
volatile int g_spec_pending          = 0; // retail carriers: set at the loser's elimination step, consumed by the call-splice below it
volatile int g_spec_res              = 0; // retail carrier of the spectator's match-end evaluation: spec_eval_retail's verdict
volatile int g_spec_msg_pending      = 0; // the "Defeated -- spectating" notice is owed (printed once gameplay resumes)

void spec_read_roster(mh::spectate::roster &r) {
    for (int i = 0; i < 8; ++i) {
        r.flags[i] = *(const uint32_t *)(mh::addr::_G_LLM_STRAT_PLAYERS + (unsigned)i * 0x740u);
        for (int j = 0; j < 8; ++j) r.relation[i][j] = *(const uint8_t *)(0x00e587f1u + (unsigned)i * 0x34u + (unsigned)j);
    }
    r.ally_rule = *mh::state::ptr<int32_t>(mh::state::RID_STRAT_MP_ALLY_VICTORY_RULE_FLAG) != 0;
}

// Is the LOCAL player a spectator right now? (pure roster state: every peer reads the same)
bool local_is_spectator() {
    if (!g_spectate_after_defeat) return false;
    const unsigned me = *(const uint16_t *)mh::addr::PlayerSide;
    if (me >= 8) return false;
    const uint32_t f = *(const uint32_t *)(mh::addr::_G_LLM_STRAT_PLAYERS + me * 0x740u);
    return mh::spectate::is_spectator(f, *(const uint8_t *)ADDR_SESSION_MODE == 3, true);
}

// mp:X3c -- the horizon the mirror advertises: never less than `floor_h` (what this peer already sent), the
// largest horizon any OTHER active human advertised, and the local HORIZON global. Our own PEER_HORIZON slot is
// ignored (G276: it holds our own stale value).
// mp:X3c-FIX (cause 2): the mirror floor for the GAME-SIDE horizon writers. During catch-up the client's clock
// is rewound by the backlog, so time_tick's advertise_horizon (clock + STEP_SIZE), an order stamp
// (max(clock + STEP, HORIZON)) and the 5th-nag bump (step * mul + clock) all land BEHIND what the host holds;
// MSG_HORIZON overwrites the host's peer horizon with no max, so the host's committed horizon fell (measured
// "DR TOTAL WRITE BACKWARDS 23660 -> 5300", host frozen ~18 s, 2 of 5 ended in ABORT reason=1). Floor at the
// choke every one of them passes (send_lockstep_extend): never advertise below what we sent / a peer holds /
// our own HORIZON. Deliberately NOT done by pinning STEP_SIZE up to the backlog: the nag bump multiplies
// STEP_SIZE and would advertise far ahead of live.
double ws_extend_floor(double h) {
    if (!g_ws_mirror) return h;
    double sent = g_hz_max_seen;
    if (g_hb_cs_ready) EnterCriticalSection(&g_hb_cs);
    if (g_hz_hb_sent > sent) sent = g_hz_hb_sent;
    if (g_hb_cs_ready) LeaveCriticalSection(&g_hb_cs);
    const double f = ws_mirror_horizon(sent);
    if (mh::netstats::mirror_extend_floor(h, f) == h) return h;
    memcpy((void *)ADDR_LOCAL_HORIZON, &f, sizeof(double)); // the writer stored the low value; keep HORIZON = what is on the wire
    if (f > g_hz_max_seen) g_hz_max_seen = f;
    static int s_logged = 0;
    if (s_logged < 8) {
        ++s_logged;
        char b[160];
        wsprintfA(b, "; [worldsync] horizon floor engaged: writer wanted %ld ms, mirror floor %ld ms\n", (long)(h * 1000.0), (long)(f * 1000.0));
        seam_log(b);
    }
    return f;
}

// One place for both the WorldSyncBegin and each FF tick: while mirroring, the stall-nag counter is zeroed (the
// 5th-nag emergency bump is a live-play recovery; during catch-up the stall is OURS -- the counter is KEEP_LOCAL,
// so the 5th nag landed inside the catch-up in 5 of 16 first resyncs) and HORIZON is raised to the mirror floor
// so an order stamped from it (max(clock + STEP, HORIZON)) is never below the advertised horizon.
void ws_mirror_upkeep() {
    const int32_t zero = 0;
    memcpy((void *)mh::state::live_base(mh::state::RID_NET_LOCKSTEP_STALL_NAG_COUNT), &zero, sizeof(zero));
    double local;
    memcpy(&local, (const void *)ADDR_LOCAL_HORIZON, sizeof(double));
    double sent = g_hz_max_seen;
    if (g_hz_hb_sent > sent) sent = g_hz_hb_sent;
    const double f = ws_mirror_horizon(sent);
    if (f > local) memcpy((void *)ADDR_LOCAL_HORIZON, &f, sizeof(double));
}

double ws_mirror_horizon(double floor_h) {
    double   peers[8];
    uint32_t mask = 0;
    for (int j = 0; j < 8; ++j) {
        memcpy(&peers[j], (const void *)(ADDR_PEER_HORIZON() + (unsigned)j * 8u), sizeof(double));
        if (slot_is_active_human(j)) mask |= 1u << j;
    }
    double local;
    memcpy(&local, (const void *)ADDR_LOCAL_HORIZON, sizeof(double));
    return mh::netstats::mirror_horizon(floor_h, peers, mask, 8, MH_Net_LocalPlayerId(), local);
}

// mp:U63 (HM-M5) -- THE CRASH FAILOVER SUSPENDS THE GAME'S OWN SILENCE TIMERS. While the transport replaces a hub that
// is gone (MH_NetHubStatus::failover_active: SUSPECT or ELECT), every survivor stalls by construction -- the star has no
// route between them -- so the timers that read a stall as "that peer is dead" would end the match in the middle of the
// recovery: GS2's data timeout (15 s), the U17 fast-drop's safety valve, and retail's resync countdown (U55 measured
// 58 s to outcome 8 on exactly this). The transport bounds the whole failover itself (`[net] failover_budget_ms`, 20 s),
// so a true partition still ends: when the budget runs out the timers simply resume, from the END of the suspension
// (g_fo_credit_tick), so no silence accrued during it is charged to anybody.
bool  g_fo_active      = false;
int   g_fo_phase       = 0;
DWORD g_fo_credit_tick = 0; // GetTickCount() when the last suspension ended (0 = never)

// mp:U64 (HM-M6) -- A SIDE THAT THE FAILOVER ENDED LEAVES THE MATCH AT ONCE. FO_MINORITY (this side holds no strict majority,
// plan Q1) and FO_FAILED (the budget ran out: nobody reachable) both refuse/lose every connection, but the game kept waiting
// for peers that will never answer and only ended through retail's resync countdown (U55: ~58 s, then outcome 8). The 4-peer
// double-loss arm measured a 2-of-4 minority sitting there for over a minute after its MINORITY verdict. So the game applies
// the verdict: every other human slot is removed locally (one per poll, re-checked against the live session), which drops
// the humans below retail's quorum and ends the match through the SAME route a transport death takes (presence_lost ->
// on_gameover outcome=8, "connection lost" / "you are the last player").
bool g_fo_end_logged = false;
void fo_end_side_poll(int phase) {
    if (phase != 4 /*FO_FAILED*/ && phase != 5 /*FO_MINORITY*/) {
        g_fo_end_logged = false;
        return;
    }
    if (*(const uint8_t *)ADDR_SESSION_MODE != 3) return;
    const int me = MH_Net_LocalPlayerId();
    for (int i = 0; i < 8; ++i) {
        if (i == me || !slot_is_active_human(i)) continue;
        if (!g_fo_end_logged) {
            g_fo_end_logged = true;
            char b[200];
            wsprintfA(b, "; U64: crash failover ended as %s -> this side leaves the match: removing the other human slots (connection lost)\n",
                      phase == 5 ? "MINORITY" : "FAILED");
            seam_log(b);
        }
        g_gs2_dropped[i] = true;
        mh::hook::call_watcall1(mh::addr::llm_net_player_remove, (void *)(intptr_t)i);
        notify_player_dropped(i);
        char b2[96];
        wsprintfA(b2, "; U64: removed human slot %d (failover verdict)\n", i);
        seam_log(b2);
        return; // one slot per poll: the first removal may already end the match
    }
}

void fo_poll() {
    static DWORD s_next = 0;
    const DWORD  now    = GetTickCount();
    if ((long)(now - s_next) >= 0) {
        s_next = now + 50;
        MH_NetHubStatus st;
        MH_Net_HubStatus(&st);
        const bool act = st.supported && st.failover_active != 0;
        if (act != g_fo_active) {
            char b[200];
            if (act) {
                wsprintfA(b, "; U63 failover ACTIVE (phase %d): GS2 / U17 safety valve / resync countdown suspended\n", st.failover);
            } else {
                g_fo_credit_tick = now;
                wsprintfA(b, "; U63 failover over (phase %d after %d ms, hub=%d): the silence timers resume from now\n",
                          st.failover, st.failover_ms, st.hub_id);
            }
            seam_log(b);
        }
        g_fo_active = act;
        g_fo_phase  = st.failover;
        fo_end_side_poll(st.supported ? st.failover : 0);
    }
    if (g_fo_active && *(const uint8_t *)ADDR_SESSION_MODE == 3) {
        int *cd = (int *)rt_countdown_base();
        if (*cd > 0 && *cd < 0x3c) *cd = 0x3c; // hold retail's SYNC_RETRY_COUNTDOWN at its reload: no tick toward the removal
    }
}

// mp:U64 -- the transport's answer RIGHT NOW. fo_poll() samples every 50 ms, and a failover can start, elect and remove the
// dead hub inside one such window (a 4-peer rig run: SUSPECT .420 -> hub elected .462 -> U17 fast-drop .464), so a decision
// that must not race the failover (the "kick that leaves nobody" session close) asks the transport itself.
bool fo_active_now() {
    MH_NetHubStatus st;
    MH_Net_HubStatus(&st);
    return g_fo_active || (st.supported && st.failover_active != 0);
}

void data_timeout_tick() {
    fo_poll();                                                  // mp:U63 -- BEFORE the early-outs: the state it keeps must follow the transport even when GS2 is off
    if (g_fo_active) return;                                    // mp:U63 -- the hub is being replaced: nobody is silent, the star is down
    if (g_data_timeout_ms <= 0) return;                         // [net] data_timeout_ms <= 0 -- feature off
    if (*(const uint8_t *)ADDR_SESSION_MODE != 3) return;       // only a live lockstep match has peers to time out
    if (!MH_Net_IsStarted() || MH_Net_PeerCount() <= 0) return; // solo: nothing to watch
    // SCAN BOUND (mp:U58): all LS_LATE_PEERS slots; the per-slot gates below decide which are real.
    // Two things keep an UNUSED slot from being read as a data-silent peer (mp:GS2's first rig run
    // "dropped" six players nobody seated -- an empty PEER_HORIZON slot holds retail's 10-second
    // sentinel, which looks like a peer's first sample): slot_is_active_human() (an unseated or AI slot
    // never has ALIVE|HUMAN) and g_late_last_move[] (a slot's first read only SEEDS the observer, so a
    // sentinel that never changes never arms -- mh::adwin::horizon_observe, dead-ends G255).
    // THE OLD BOUND `1 + MH_Net_PeerCount()` WAS WRONG for 3+ peers. PeerCount is the number of this
    // peer's own ACTIVE TRANSPORT CONNECTIONS, and the transport is a star: the host holds one per
    // client (N-1), a client holds exactly ONE (to the host; a relay leg is still one conn per peer).
    // So it was "symmetric" only for 2 peers (1 == 1). In a 3-peer match client 1 scanned slots 0..1
    // (the host only) and nobody scanned slot 2; and even on the host a departed client narrowed the
    // bound below a still-seated higher slot (ids are not contiguous once someone left).
    // Not a roster-derived bound either: current_map_player_count holds the map's capacity on a client.
    const DWORD now = GetTickCount();
    const int   me  = MH_Net_LocalPlayerId();
    // The silence of slot i in ms, or -1 when GS2 does not watch it (the per-slot gates).
    auto silence = [&](int i) -> long {
        if (i == me || g_gs2_dropped[i]) return -1;
        // U19b: a slot the lockstep dispatch already removed (a clean quit) is not a data-silent
        // peer, it is a gone one. Re-read every frame rather than latched: the flags are the
        // roster's own truth, and a latch taken one frame too early would disarm this watchdog for
        // a peer whose seat had simply not been flagged yet.
        if (!slot_is_active_human(i)) return -1;
        // A slot with h<=0 has never advertised at all yet (the join window GS1 already gates on) --
        // nothing to time out until it has really been seen alive once.
        if (g_late_last_h[i] <= 0.0 || g_late_last_move[i] == 0) return -1;
        // mp:U63: silence is counted from the END of a failover's suspension, never from before it
        DWORD base = g_late_last_move[i];
        if (g_fo_credit_tick != 0 && (long)(g_fo_credit_tick - base) > 0) base = g_fo_credit_tick;
        // mp:U68: a GS2 drop changes who the lockstep waits on (a parked hub un-parks only once the silent
        // client's removal reaches it), so every other slot's silence restarts at the drop.
        if (g_gs2_credit_tick != 0 && (long)(g_gs2_credit_tick - base) > 0) base = g_gs2_credit_tick;
        return (long)(now - base);
    };
    // mp:U68 -- A PARKED HUB IS NOT A DEAD HUB. In a star every client's lockstep waits on the hub's
    // horizon, and the hub's horizon waits on EVERY seated slot: when client 2 goes silent the hub parks,
    // its horizon stops, and every other client sees the hub cross the data timeout within a tick or two
    // of slot 2 (3 runs in 4 dropped both, det_arms --u58-gs2-3peer; and the fenced client dropped the hub
    // too). Slot 2's drop is what un-parks the hub, so while the transport says the hub is ALIVE (hub-loss
    // detection idle: it answers at the wire; a dead hub turns into the U63 failover, which suspends GS2
    // above) AND a third seated human exists (someone besides me and the hub the hub can be waiting on),
    // the hub gets a second window: it is dropped at twice the timeout, not at one. A 2-peer match has no
    // third party to wait on, so its hub is judged at the plain timeout as before. The drop of the silent
    // slot restarts every survivor's silence (g_gs2_credit_tick), so the un-parking has a full window.
    int hub_alive_id = -1;
    {
        MH_NetHubStatus st;
        MH_Net_HubStatus(&st);
        if (st.supported && st.enabled && st.role == 1 && st.failover == 0) hub_alive_id = st.hub_id;
    }
    for (int i = 0; i < LS_LATE_PEERS; ++i) {
        const long since_l = silence(i);
        if (since_l < 0) continue;
        const DWORD since = (DWORD)since_l;
        if (since < (DWORD)g_data_timeout_ms) continue;
        if (i == hub_alive_id && since < 2 * (DWORD)g_data_timeout_ms) {
            bool third = false;
            for (int j = 0; j < LS_LATE_PEERS && !third; ++j) third = j != me && j != i && !g_gs2_dropped[j] && slot_is_active_human(j);
            if (third) {
                static DWORD s_hold_log = 0;
                if ((long)(now - s_hold_log) >= 0) {
                    s_hold_log = now + 1000;
                    char hb[176];
                    wsprintfA(hb, "; GS2: hub %d silent %lu ms but its transport is alive and a third slot may be parking it -> held, not dropped (U68)\n",
                              i, (unsigned long)since);
                    seam_log(hb);
                }
                continue;
            }
        }
        g_gs2_credit_tick = now;
        g_gs2_dropped[i]  = true; // latch FIRST -- the removal call below must never re-enter this slot
        mh::hook::call_watcall1(mh::addr::llm_net_player_remove, (void *)(intptr_t)i);
        notify_player_dropped(i);
        char b[176];
        wsprintfA(b, "; GS2: peer %d data-silent for %lu ms > %d -> dropped\n", i, (unsigned long)since,
                  g_data_timeout_ms);
        seam_log(b);
        // SES1: same "a kick that leaves nobody" guard as U17(b) -- a below-quorum drop closes our own
        // session record even though the peer that just left never sends its own teardown seam.
        if (MH_Net_PeerCount() <= 0 && !fo_active_now()) mp_session_close("timeout"); // mp:U64: not while a failover is seating its roster
    }
}

// mp:D30 -- the largest horizon this peer is known to have put on the wire (main-thread view): what
// the main thread sampled/sent, and what the heartbeat sent. Each half has a single writer.
// HORIZON itself is part of it: every retail writer between two pins (time_tick's advertise, an
// order stamped at clock + STEP_SIZE, the keepalive) leaves its value there -- schedule raises HORIZON
// to any order past it -- and the heartbeat only writes it under g_hb_cs, so reading it under the
// lock on the main thread cannot tear.
double horizon_sent_max(double clk) {
    double m = hz_stale(clk, g_hz_max_seen) ? 0.0 : g_hz_max_seen;
    if (g_hb_cs_ready) EnterCriticalSection(&g_hb_cs);
    const double hb = g_hz_hb_sent;
    double       h;
    memcpy(&h, (const void *)ADDR_LOCAL_HORIZON, sizeof(double));
    if (g_hb_cs_ready) LeaveCriticalSection(&g_hb_cs);
    if (!hz_stale(clk, hb) && hb > m) m = hb;
    if (!hz_stale(clk, h) && h > m) m = h;
    return m;
}

// mp:D30 -- pin the lookahead every horizon writer reads. This is the ONE place STEP_SIZE is set
// from the controller's target, and every writer takes its lookahead from here: retail time_tick's
// advertise_horizon and the pump keepalive read STEP_SIZE (configuration (1) and the promoted build
// alike -- libmh's timekeeper/turn_engine read the same global), order_dispatch stamps
// max(GAME_CLOCK + STEP_SIZE, HORIZON), and the eager hook and the heartbeat read g_eff_step. So a
// controller SHRINK takes effect as the clock catches up to the horizon already sent, and the
// horizon never goes down (G297). The clock only moves forward inside a frame, so a writer that
// computes clock + STEP_SIZE later in this frame still lands >= the maximum pinned against here.
void monotone_pin() {
    double clk;
    memcpy(&clk, (const void *)ADDR_GAME_CLOCK, sizeof(double));
    const bool   ls  = *(const uint8_t *)ADDR_SESSION_MODE == 3;
    const double m   = ls ? horizon_sent_max(clk) : 0.0;
    const double eff = mh::netstats::monotone_step(g_lockstep_step, clk, m);
    if (g_hb_cs_ready) EnterCriticalSection(&g_hb_cs);
    g_eff_step = eff;
    if (g_hb_cs_ready) LeaveCriticalSection(&g_hb_cs);
    memcpy((void *)ADDR_STEP_SIZE, &eff, sizeof(double));
    const bool holding = eff > g_lockstep_step + 1e-6; // past rounding: (c + t) - c can exceed t by an ulp
    if (holding) ++g_d30_holds;
    if (holding != g_d30_holding) {
        g_d30_holding = holding;
        if (g_ls_log) {
            char b[192];
            if (holding)
                wsprintfA(b,
                          "; [monotone] HOLD target=%ld ms sent=%ld ms clock=%ld ms step=%ld ms -- the shrink "
                          "waits for the clock (mp:D30)\n",
                          (long)(g_lockstep_step * 1000.0), (long)(m * 1000.0), (long)(clk * 1000.0),
                          (long)(eff * 1000.0));
            else
                wsprintfA(b, "; [monotone] RELEASE target=%ld ms clock=%ld ms holds=%ld\n",
                          (long)(g_lockstep_step * 1000.0), (long)(clk * 1000.0), g_d30_holds);
            seam_log(b);
        }
    }
}

} // namespace

// mp:D30 -- the PER-STEP half of the pin. monotone_pin() at time_tick holds STEP_SIZE for the frame's
// starting clock; a writer that runs later in the frame, after the sim has advanced the clock, would
// otherwise add that frame-start value to a LATER clock and overshoot the held horizon -- and since
// the overshoot is itself sent, the next pin would have to hold it too, and the shrink would never
// land (the rig's synth workload stamps one order per step and pinned the lookahead at its peak).
// Re-pinning at every sim_step entry keeps `clock + STEP_SIZE` == max(clock + target, max sent) for
// whatever that step stamps. CORRECTNESS does not depend on this call -- a stale pin only ever errs
// HIGH -- only the shrink's progress does. Called from the ship path's desync sim_step hook
// (net_seams.cpp) and from the harness's own sim_step detour when it owns that entry.
extern "C" void MH_Lockstep_StepPin(void) {
    if (!g_tt_tramp || g_lockstep_step <= 0.0) return; // the pacing detour is not armed this run
    if (*(const uint8_t *)ADDR_SESSION_MODE != 3) return;
    monotone_pin();
}

// mp:X3b -- HOLD the horizon heartbeat still. horizon_heartbeat_thread rewrites HORIZON every
// g_hb_ms of REAL time from its own thread (g_hb_cs is taken around the write+send), so a reader on
// the main thread that wants ONE instant of it -- the world-snapshot capture and its SNAPCAP row, or
// the import and its SNAPIMP row -- cannot get one: a beat landing between the two reads moved the
// value under them (measured 2026-09-29: a beat at the same GetTickCount ms as the import turned
// ls_horizon 4003C294.. into 4003D70F.. between libmh_import_world and the SNAPIMP hash). on=1 takes
// g_hb_cs, on=0 releases it; a CRITICAL_SECTION is owned by a thread, so BOTH calls must come from
// the same thread. The holder must not call into the transport while holding (the recv thread can
// hold transport locks while waiting here): the harness holds across the capture/import and the
// in-memory hashes only. Recursive, so main-thread paths that take g_hb_cs themselves still work.
extern "C" void MH_Lockstep_HorizonHold(int on) {
    if (!g_hb_cs_ready) return;
    if (on) EnterCriticalSection(&g_hb_cs);
    else LeaveCriticalSection(&g_hb_cs);
}

// mp:X3c -- the world-resync CATCH-UP. After libmh_import_world_resync the peer's GAME_CLOCK is B steps behind
// the live TOTAL_GAME_TIME it kept (a keep-local region). The engine's own mode-3 loop would run the whole
// backlog inside ONE frame (no RX drain, no pump: the mirror would stop seeing the host advance), so the
// per-frame TOTAL is capped at clock + ff_steps sub-steps and the live target advances at wall rate:
//   g_ws_ff_live  the uncapped live target (== the value TOTAL would have had)
//   each frame    live += dt * speed (clamped to committed); TOTAL = min(live, clock + ff_steps * sub)
//   exit          when the backlog is within two caps: TOTAL = live, mirror off, state DONE
// The arithmetic is desync/world_sync_core.h ff_* (proven by `net_selftest wstest`).
enum { WS_FF_IDLE   = 0,
       WS_FF_ACTIVE = 1,
       WS_FF_DONE   = 2 };
volatile LONG g_ws_ff_state = WS_FF_IDLE;
int           g_ws_ff_steps = 20;
double        g_ws_ff_live  = 0.0;
LARGE_INTEGER g_ws_ff_prev  = {};
LARGE_INTEGER g_ws_ff_freq  = {};

static void ws_ff_read(double *clk, double *sub, double *speed, double *committed) {
    memcpy(clk, (const void *)ADDR_GAME_CLOCK, sizeof(double));
    memcpy(sub, (const void *)ADDR_SIM_STEP_INT(), sizeof(double));
    memcpy(speed, (const void *)ADDR_GAME_SPEED, sizeof(double));
    memcpy(committed, (const void *)ADDR_COMMITTED(), sizeof(double));
}

// Called from the world-sync import, mid sim step, right after the world was replaced. Returns the FF state.
extern "C" int MH_Lockstep_WorldSyncBegin(int ff_steps) {
    double clk, sub, speed, committed, total;
    ws_ff_read(&clk, &sub, &speed, &committed);
    memcpy(&total, (const void *)ADDR_TOTAL_TIME, sizeof(double)); // keep-local: the live target
    g_ws_ff_steps = ff_steps < 1 ? 1 : ff_steps;
    g_ws_ff_live  = total;
    QueryPerformanceFrequency(&g_ws_ff_freq);
    QueryPerformanceCounter(&g_ws_ff_prev);
    if (mh::desync::ws::ff_done(total, clk, g_ws_ff_steps, sub)) { // a backlog of <= two caps needs no help
        InterlockedExchange(&g_ws_ff_state, WS_FF_DONE);
        return WS_FF_DONE;
    }
    InterlockedExchange(&g_ws_mirror, 1);
    ws_mirror_upkeep(); // mp:X3c-FIX: zero the KEEP_LOCAL nag counter + floor HORIZON before the first frame
    const double cap = mh::desync::ws::ff_total(total, clk, g_ws_ff_steps, sub);
    memcpy((void *)ADDR_TOTAL_TIME, &cap, sizeof(double)); // the frame's loop re-reads TOTAL every iteration
    InterlockedExchange(&g_ws_ff_state, WS_FF_ACTIVE);
    return WS_FF_ACTIVE;
}
extern "C" int  MH_Lockstep_WorldSyncState(void) { return (int)g_ws_ff_state; }
extern "C" void MH_Lockstep_WorldSyncEnd(void) {
    InterlockedExchange(&g_ws_mirror, 0);
    InterlockedExchange(&g_ws_ff_state, WS_FF_IDLE);
}

namespace {
// Pre-hook of every time_tick (on_time_tick, also reached by lt_frame_pace_time_tick): the per-frame cap.
void ws_ff_tick() {
    if (g_ws_ff_state != WS_FF_ACTIVE) return;
    if (*(const uint8_t *)ADDR_SESSION_MODE != 3) { // the match ended under the catch-up: nothing to pace
        InterlockedExchange(&g_ws_mirror, 0);
        InterlockedExchange(&g_ws_ff_state, WS_FF_DONE);
        return;
    }
    double clk, sub, speed, committed;
    ws_ff_read(&clk, &sub, &speed, &committed);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const double dt = g_ws_ff_freq.QuadPart ? (double)(now.QuadPart - g_ws_ff_prev.QuadPart) / (double)g_ws_ff_freq.QuadPart : 0.0;
    g_ws_ff_prev    = now;
    ws_mirror_upkeep(); // mp:X3c-FIX: keep the nag counter at 0 and HORIZON floored every FF tick
    g_ws_ff_live = mh::desync::ws::ff_live_advance(g_ws_ff_live, dt, speed, committed);
    if (mh::desync::ws::ff_done(g_ws_ff_live, clk, g_ws_ff_steps, sub)) {
        memcpy((void *)ADDR_TOTAL_TIME, &g_ws_ff_live, sizeof(double)); // release: live pace from here
        InterlockedExchange(&g_ws_mirror, 0);
        InterlockedExchange(&g_ws_ff_state, WS_FF_DONE);
        return;
    }
    const double cap = mh::desync::ws::ff_total(g_ws_ff_live, clk, g_ws_ff_steps, sub);
    memcpy((void *)ADDR_TOTAL_TIME, &cap, sizeof(double));
}
} // namespace

namespace {

// ==== mp:U62 (HM-M4): THE GAME-SIDE SEAMS OF THE PLANNED HANDOVER ================================
//
// The transport does the handover (mh_net_udp: MH_Net_HubLeave / MH_Net_HubStatus, docs/mp-host-migration-
// plan.md section 5.4/6.5). What lives HERE is the question the transport cannot answer -- "has this hub's
// PLAYER just left a match that other humans are still playing?" -- and the one thing the survivors show.
//
// THE LEAVE SEAMS (every one ends in mp_hub_leave, which no-ops for a peer that is not a hub):
//   * quit from the ESC menu          on_quit_to_menu, AFTER U19b's park + pinned self-removal
//   * the defeat/stats screen         the host was defeated (on_gameover_pre armed g_hub_watching), keeps
//                                     relaying (plan Q2), and its player then reaches the main menu:
//                                     hub_leave_poll reads the game's own "in main menu" flag
//   * the process exiting on purpose  mp_leave_for_exit (WM_DESTROY, net_diag.cpp) and the harness's
//                                     graceful exit knob
//
// NO ROSTER WORK HERE, ON PURPOSE (G335): the leaving player is already out of every peer's sim roster at a
// PINNED step before the handover starts -- by U19b's park step for a quit, by the pinned elimination
// (presence_lost at the elimination step, U56) for a defeat. The handover moves the transport and nothing else.
constexpr uintptr_t ADDR_UI_IN_MAIN_MENU = 0x00603f70u; // _G_LLM_UI_IN_MAIN_MENU: 1 on entering the main menu, 0 on entering gameplay
constexpr int       HUB_LEAVE_MS         = 2500;        // the wait for every survivor's acknowledgement
bool                g_hub_watching       = false;       // a defeated hub that stays: leaving later must still hand over
long                g_hub_changes_seen   = 0;           // MH_NetHubStatus::changes already shown

bool net_hub_now() {
    MH_NetHubStatus st;
    MH_Net_HubStatus(&st);
    return st.supported && st.enabled && st.role == 0;
}

// Other humans still ALIVE in the sim -- the ones a handover keeps the match going for. An AI seat does not count.
int other_active_humans() {
    const int me = MH_Net_LocalPlayerId();
    int       n  = 0;
    for (int i = 0; i < 8; ++i)
        if (i != me && slot_is_active_human(i)) ++n;
    return n;
}

// on_gameover_pre: a hub that is DEFEATED while two or more other humans play on is not leaving (plan Q2: it
// keeps relaying). Remember that, so that the day its player does leave, the hub is handed over.
void hub_note_defeat() {
    // NOT gated on session mode == 3: a defeat is PINNED by U56's elimination step, which already took this peer's
    // session out of lockstep (measured: `on_gameover ENTER sess=2`), while the transport still relays. The hub
    // role + two other live humans is the test.
    if (!net_hub_now()) return;
    const int others = other_active_humans();
    g_hub_watching   = others >= 2;
    char b[176];
    wsprintfA(b, "; U62 hub-watch: this hub was defeated with %d other human(s) still playing -> %s (in_main_menu=%d)\n",
              others, g_hub_watching ? "it keeps relaying; it hands the hub over when its player leaves" : "no handover needed",
              *(const int *)ADDR_UI_IN_MAIN_MENU);
    seam_log(b);
}

// mp:U63 -- IS A MATCH RUNNING? The transport arms its hub-loss detection only inside one (a lobby whose host closes, or a
// peer back at the menu, is not a crash). In a match = the lockstep session (mode 3) -- and it stays "in" for a peer that
// was DEFEATED but still watches (session mode downgraded, the main menu not reached): that peer is still a seated
// member of the transport and may be the elected successor. Out = the main menu was reached.
void fo_match_tick() {
    static int  s_sent = -1;
    static bool s_in   = false;
    if (*(const uint8_t *)ADDR_SESSION_MODE == 3) s_in = true;
    else if (*(const int *)ADDR_UI_IN_MAIN_MENU != 0 || g_spec_menu_reached) s_in = false;
    if ((int)s_in == s_sent) return;
    s_sent = (int)s_in;
    MH_Net_SetInMatch(s_in ? 1 : 0);
    char b[96];
    wsprintfA(b, "; U63 in-match=%d (the transport's crash failover is %s)\n", (int)s_in, s_in ? "armed" : "disarmed");
    seam_log(b);
}

// A defeated hub's player has left once the game says it is back at the main menu (the flag the game sets on
// entering it and clears on entering gameplay). Per frame; two loads when nothing is armed.
void hub_leave_poll() {
    if (!g_hub_watching) return;
    if (*(const int *)ADDR_UI_IN_MAIN_MENU == 0 && !g_spec_menu_reached) return;
    mp_hub_leave("main-menu");
}

// The notice the survivors see: "<old> left; <new> is now hosting". A table row (ui/player_strings.def), so a
// language pack translates it. Names come from the player table, by side id (== transport id).
void hub_notice_tick() {
    static DWORD s_next = 0;
    const DWORD  now    = GetTickCount();
    if ((long)(now - s_next) < 0) return;
    s_next = now + 100;
    MH_NetHubStatus st;
    MH_Net_HubStatus(&st);
    if (!st.supported || st.changes <= g_hub_changes_seen) return;
    g_hub_changes_seen = st.changes;
    wchar_t   names[2][40];
    const int ids[2] = {st.old_hub, st.new_hub};
    for (int k = 0; k < 2; ++k) {
        names[k][0]   = 0;
        const int idx = ids[k] >= 0 ? mh::hook::call_watcall1(mh::addr::llm_strat_player_by_side_id, (void *)(intptr_t)ids[k]) : -1;
        if (idx >= 0 && idx < 8) {
            const char *nm = (const char *)(mh::addr::_G_LLM_STRAT_PLAYERS + (unsigned)idx * 0x740u + 1812u);
            MultiByteToWideChar(CP_ACP, 0, nm, -1, names[k], 40);
        }
        if (!names[k][0]) wsprintfW(names[k], L"#%d", ids[k]);
    }
    wsprintfW((wchar_t *)mh::addr::G_TEXT_TMP, mh::ui::tr(st.change_crash ? mh::ui::Str::HUB_LOST : mh::ui::Str::HUB_MIGRATED), names[0],
              names[1]);
    mh::hook::call_watcall1(mh::addr::llm_ui_print_floating_msg_red, (void *)mh::addr::G_TEXT_TMP);
    char b[200], a[160];
    WideCharToMultiByte(CP_ACP, 0, (const wchar_t *)mh::addr::G_TEXT_TMP, -1, a, sizeof(a), nullptr, nullptr);
    wsprintfA(b, "; U62 notice: hub %d -> %d (epoch %u): printed floating msg '%s'%s\n", st.old_hub, st.new_hub,
              st.change_epoch, a, st.change_crash ? " (crash failover, mp:U63)" : "");
    seam_log(b);
}

void on_time_tick() {
    hub_notice_tick(); // mp:U62: a hub change this peer went through -> the HUD line
    if (g_spec_msg_pending && local_is_spectator()) {
        // mp:U54: the defeat dialog was answered with Continue and the match is back on screen -- one fading line.
        g_spec_msg_pending = 0;
        wsprintfW((wchar_t *)mh::addr::G_TEXT_TMP, L"%s", mh::ui::tr(mh::ui::Str::SPECTATE_DEFEATED));
        mh::hook::call_watcall1(mh::addr::llm_ui_print_floating_msg_red, (void *)mh::addr::G_TEXT_TMP);
        seam_log("; U54 spectate: floating notice printed (Defeated -- spectating)\n");
    }
    ws_ff_tick(); // mp:X3c: world-resync catch-up pacing (idle unless a resync import armed it)
    // mp:U19b -- the quit's freeze ends with the quit. A time_tick outside the freeze..teardown window
    // (g_leave_in_progress) is a LATER match: every frozen quit runs its whole wait + removal + retail
    // teardown inside ONE UI callback, so no tick of the match being left can land here after it.
    // (A tick INSIDE the window is possible -- a survivor dropping during the wait routes through
    // time_resync_and_tick -- which is what the guard is for.)
    if (g_leave_frozen && !g_leave_in_progress) {
        InterlockedExchange(&g_leave_frozen, 0);
        g_hz_max_seen = 0.0;
        if (g_hb_cs_ready) EnterCriticalSection(&g_hb_cs);
        g_hz_hb_sent = 0.0; // mp:D30: a later match starts from its own clock
        if (g_hb_cs_ready) LeaveCriticalSection(&g_hb_cs);
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
        // mp:D30 -- the sent maxima now FLOOR every advert, so they must not outlive their match: a new
        // match's clock restarts, and a maximum left over from a short previous match would sit less
        // than HZ_RESTART_S ahead of it and hold the new match's lookahead up. Reset both on the clock
        // going backwards (the same signal lateness_tick uses), and on the 2 s rule as before.
        static double s_last_clk = 0.0;
        if (!g_ws_mirror && clk + 1e-6 < s_last_clk) { // mp:X3c: a mirrored catch-up is not a new match
            g_hz_max_seen = 0.0;
            if (g_hb_cs_ready) EnterCriticalSection(&g_hb_cs);
            g_hz_hb_sent = 0.0;
            if (g_hb_cs_ready) LeaveCriticalSection(&g_hb_cs);
        }
        s_last_clk = clk;
        if (hz_stale(clk, g_hz_max_seen)) g_hz_max_seen = 0.0;
        if (h > g_hz_max_seen && !hz_stale(clk, h)) g_hz_max_seen = h;
    }
    lateness_tick();                                                                        // mp:T3 -- sample first: the controller below reads the snapshot it publishes
    data_timeout_tick();                                                                    // mp:GS2 -- act on this frame's fresh g_late_last_move[] before anything else touches it
    resync_count_init_tick();                                                               // mp:P9 root fix -- ACTIVE_PLAYER_COUNT=players at match start (before the watch samples it)
    resync_trigger_watch_tick();                                                            // mp:P9 -- the resync-trigger counter + the barrier edges, readable off one peer's log
    adaptive_tick();                                                                        // may move g_lockstep_step; the pin below applies it the same frame
    if (g_lockstep_step > 0.0) monotone_pin();                                              // mp:D30: STEP_SIZE = max(target, max_sent - clock)
    if (g_game_speed > 0.0) memcpy((void *)ADDR_GAME_SPEED, &g_game_speed, sizeof(double)); // pin before time_tick reads it
    if (g_sim_step > 0.0) memcpy((void *)ADDR_SIM_STEP_INT(), &g_sim_step, sizeof(double)); // pin sub-step granularity (both time_tick's arm-gate + sim_tick's loop read it)
    if (g_rx_spin && *(const uint8_t *)ADDR_SESSION_MODE == 3) rx_spin_until_horizon();     // (b) Step 2: drain RX off-frame during a stall
    if (*(const uint8_t *)ADDR_SESSION_MODE == 3) lateness_post_spin_tick();                // mp:P15 -- a spin that funded the step ends the episode
    if (*(const uint8_t *)ADDR_SESSION_MODE == 3) adaptive_post_spin_tick();                // mp:P15 -- starved AFTER the spin
    if (*(const uint8_t *)ADDR_SESSION_MODE == 3) {
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
        // mp:U64 (HM-M6): a NEW hub does not remove the dead peer until its reconcile has finished. The removal is pinned to the
        // parked step, i.e. to the dead hub's final horizon AS THIS PEER KNOWS IT, and a live hub cut off by a partition has
        // frames (its last horizon adverts) that reached some survivors and not others; the reconcile is what makes every
        // survivor hold the same set of them. Removing first pinned the removal at a stale horizon on the hub (4-peer
        // partition arm: a transient strat_players mismatch at the removal step in 1 run of 3).
        bool rc_hold = false;
        if (g_pending_dead >= 0) {
            MH_NetHubStatus hs;
            MH_Net_HubStatus(&hs);
            rc_hold = hs.supported && hs.reconciling && !hs.reconcile_done && !hs.aborted;
        }
        if (g_pending_dead >= 0 && !rc_hold) {
            const double total     = *(const double *)ADDR_TOTAL_TIME;
            const double committed = *(const double *)ADDR_COMMITTED();
            // SAFETY VALVE: if the parked state never arrives the peer must still be removed, or a
            // wrong model here would hang the session instead of desyncing it. Fire anyway after
            // g_pending_dead_max frames and say so loudly -- a silent fallback would hide the fact
            // that the barrier assumption failed.
            const bool parked  = (total >= committed);
            const bool timeout = !g_fo_active && (++g_pending_dead_wait > g_pending_dead_max); // mp:U63: not mid-failover
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
                // mp:U64 (HM-M6): NOT while a crash failover is running. The NEW hub removes the dead old hub at the parked
                // step before its survivors have re-dialled, so it momentarily has no connection at all; "a kick that leaves
                // nobody" read that as the end of the match, closed the session (reason timeout) and U40 relinked the
                // transport -- destroying the hub the survivors were dialling (4-peer rig, 2026-10-03: every survivor saw
                // "player 2 did not answer" and elected ITSELF). The failover ends the match itself if nobody comes (MINORITY).
                if (MH_Net_PeerCount() <= 0 && !fo_active_now()) mp_session_close("timeout");
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
// (see the U30 block comment above g_graceful_leave). SESSION_MODE is a dword; 3 = lockstep, 2 = MP-local.
// The downgrade is idempotent, which is why the shape below can run it unconditionally: SESSION is
// 3 or it is not. Ordering is unchanged from before U19d: this still runs BEFORE the original body.
// mp:U54 -- the SPECTATOR'S DEFEAT DIALOG. Retail's outcome-4 (defeat) dialog of a lockstep match has ONE visible button: the
// four widgets of _G_LLM_UI_OUTCOME_DLG_WIDGET_LIST are [0] title "Game over", [1] message "You lost !", [2] "Ok" (opens the
// statistics screen, then the main menu = leaving the match) and [3] "Continue game" (llm_menu_finish_enter_gameplay, back to
// the running game), which llm_ui_outcome_dialog HIDES (flags |= 0xc0 at 0x004c6d44) except for outcomes 6/7/8. A spectator
// is given the outcome-6 shape (0x004c6eee: widget [2] flag bit 3 off and x = 50, widget [3] un-hidden) with its own labels,
// so the choice is CONTINUE SPECTATING (the retail Continue widget) or EXIT MATCH (the retail Ok path).
constexpr uintptr_t ADDR_ODLG_W2 = 0x00650c77; // "Ok" widget (+0x08 flags, +0x1c x, +0x38 label)
constexpr uintptr_t ADDR_ODLG_W3 = 0x00650cbb; // "Continue game" widget, hidden by retail for a defeat
void                spectator_dialog_fixup() {
    *(volatile uint8_t *)(ADDR_ODLG_W2 + 0x0a) &= 0xf7; // flags byte +0xa bit 3 off, as outcome 6's case does
    *(volatile int32_t *)(ADDR_ODLG_W2 + 0x1c) = 0x32;  // x = +50: Ok moves right, making room for [3]
    *(volatile uint8_t *)(ADDR_ODLG_W3 + 0x08) &= 0x3f; // un-hide + enable "Continue game"
    *(const wchar_t *volatile *)(ADDR_ODLG_W2 + 0x38) = mh::ui::tr(mh::ui::Str::SPECTATE_EXIT);
    *(const wchar_t *volatile *)(ADDR_ODLG_W3 + 0x38) = mh::ui::tr(mh::ui::Str::SPECTATE_CONTINUE);
    seam_log("; U54 spectate: defeat dialog widened -- [Continue spectating] [Exit match]\n");
}

void on_gameover_pre() {
    const uint32_t outcome = g_go_outcome;
    if (g_ls_log) {
        char b[160];
        wsprintfA(b, "; on_gameover ENTER sess=%d outcome=%u gclk=%ld (downgrade=%d)\n",
                  (int)*(const uint8_t *)ADDR_SESSION_MODE, outcome, ms_of(ADDR_GAME_CLOCK),
                  (int)(*(volatile uint32_t *)ADDR_SESSION_MODE == 3));
        seam_log(b);
    }
    hub_note_defeat(); // mp:U62: BEFORE the downgrade -- it reads the mode and the roster as the match left them
    // mp:U54: a SPECTATOR's defeat dialog (outcome 4 at its own elimination, session still lockstep) is NOT the end of the
    // match for it: the session stays lockstep, the transport session stays open. Continue game goes on spectating; Ok
    // leaves through the statistics screen to the main menu, where spectator_tick() ends the session (spec exit).
    if (local_is_spectator()) {
        g_spec_msg_pending = 1;
        if (g_ls_log) seam_log("; U54 spectate: defeat dialog opened by a SPECTATOR -- session stays lockstep (Continue = keep watching, Ok = leave)\n");
        return;
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
void spectator_dialog_fixup(); // mp:U54 -- below
void on_gameover_post() {
    if (g_go_outcome == 4 && local_is_spectator()) {
        spectator_dialog_fixup();
        return;
    }
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
// BOUNDED: LEAVE_PARK_MS (1500) is the safety valve; past it the record goes out anyway -- still
// with the honest H_d, so the AGREE arm is still taken and only the parking is unproven -- and the
// log line says so.
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
    if (!hz_stale(clk, g_hz_hb_sent) && g_hz_hb_sent > hd) hd = g_hz_hb_sent; // mp:D30

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
    const char *verdict = "VALVE-UNPARKED";
    DWORD       waited  = 0;
    for (;;) {
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
        if (waited > (DWORD)LEAVE_PARK_MS) break; // VALVE-UNPARKED
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
// mp:U62 -- the U17/U19b quit (freeze, park, pinned self-removal broadcast), factored out of on_quit_to_menu
// so the process-exit seam (mp_leave_for_exit) runs the SAME body. The two gates stay with the callers.
void graceful_quit_body() {
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
void on_quit_to_menu() {
    // SES1: FIRST, and outside both gates below. Quit-to-menu ends the session whether or not the
    // graceful-leave broadcast is armed and whether or not we were still in mode 3 -- a player who
    // ESCs out of a lobby has left the match just as surely as one who quits a running game, and a
    // session left open here would swallow the next match's menu lines.
    mp_session_close("quit");
    // mp:U54: a SPECTATOR is no barrier member (ALIVE and HUMAN both off), so there is nothing to park or remove: the
    // U19b body would put a removal record for a slot the survivors have already flipped on the wire.
    if (g_graceful_leave && *(const uint8_t *)ADDR_SESSION_MODE == 3 && !local_is_spectator()) // in a running lockstep game
        graceful_quit_body();
    // mp:U62: the player has left the match. A hub hands the transport to its successor NOW -- after the
    // pinned self-removal above has reached every survivor (they apply it in dispatch order), before the
    // retail teardown runs. No-op for a client, for a hub with fewer than two survivors, for hub_migration=0.
    mp_hub_leave("quit");
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
        if (g_eff_step > 0.0) step = g_eff_step; // mp:D30: the monotone pin's value, not the raw target
        else if (g_lockstep_step > 0.0) step = g_lockstep_step;
        else memcpy(&step, (const void *)ADDR_STEP_SIZE, sizeof(double));
        // mp:D30 -- never below what is already on the wire (G297). Both sent maxima, plus HORIZON as
        // the main thread holds it: a main-thread writer (retail time_tick, the pump) may have put a
        // later clock + STEP_SIZE there since our clock read. HORIZON is 4-aligned, so read it until
        // two reads agree rather than trust one read of a value that may be mid-write.
        if (hz_stale(clock, g_hz_hb_sent)) g_hz_hb_sent = 0.0;
        double floor_h = g_hz_max_seen;
        if (hz_stale(clock, floor_h)) floor_h = 0.0;
        if (g_hz_hb_sent > floor_h) floor_h = g_hz_hb_sent;
        {
            double a, b;
            int    tries = 0;
            do {
                memcpy(&a, (const void *)ADDR_LOCAL_HORIZON, sizeof(double));
                memcpy(&b, (const void *)ADDR_LOCAL_HORIZON, sizeof(double));
            } while (!(a == b) && ++tries < 8);
            if (a == b && a > floor_h) floor_h = a;
        }
        double horizon = g_ws_mirror ? ws_mirror_horizon(floor_h)
                                     : mh::netstats::monotone_horizon(clock, step, floor_h);
        if (horizon > g_hz_hb_sent) g_hz_hb_sent = horizon; // sole writer: this thread, under g_hb_cs
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
        push dword ptr [g_wait_target] // tail-jump to wait_player_overlay_show (0x004c7dc0, or mp:U44's carrier) -- EAX = player_idx preserved
        ret
    suppressed:
        ret // return past the call site
    }
}
void install_overlay_gate() {
    // Its target is inside llm_strat_time_tick, promotable since C4 -- so "displaced" is a THIRD outcome
    // here and must not be reported as a byte MISMATCH. Without this the log said "unexpected bytes at
    // wait-overlay call site" on every promoted run, which reads as "the DLL is running against the
    // wrong mh.exe build" -- a much scarier and entirely false diagnosis.
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
// session_begin_multi (0x0045450e) sees a non-error 0. Always on (the `resync_wait_fix` knob is retired).
constexpr uintptr_t ADDR_RESYNC_STUB_RET  = 0x0049c044;         // llm_net_lockstep_sync_delay_stub: the `mov eax,[ebp-0x28]` return
const uint8_t       RESYNC_STUB_EXPECT[3] = {0x8B, 0x45, 0xD8}; // mov eax,[ebp-0x28]  (returns uninitialized stack)
const uint8_t       RESYNC_STUB_PATCH[3]  = {0x31, 0xC0, 0x90}; // xor eax,eax; nop    (return 0 -> instant wait)
void                install_resync_wait_fix() {
    // DISPLACED is a THIRD outcome, and it must not be reported as a byte mismatch. Since C8-c
    // promotes llm_net_lockstep_sync_delay_stub, patch_bytes_guarded returns false in a promoted run
    // because the C1 interlock SUPPRESSED the write -- not because the bytes were wrong. Without this
    // branch the log would read "NOT armed (unexpected bytes -- already patched?)", which is false in
    // both of its clauses and points a reader at a byte-level investigation that has nothing to find.
    // Same shape and same reason as install_overlay_gate above.
    //
    // WHAT CARRIES THE FIX THEN: our body returns 0 UNCONDITIONALLY (resync.h argues why reproducing
    // the original's uninitialised-stack read is not an option -- it is the hang). The rollback arm
    // for an A/B experiment is `[promote] lockstep=0`, which is the diagnostic configuration the
    // seam-class taxonomy §4c R2 reserves for exactly this.
    if (mh::hook::promoted_owner_of(ADDR_RESYNC_STUB_RET)) {
        seam_log("; resync-wait fix DISPLACED: sync_delay_stub is promoted, and our body carries the fix "
                                               "UNCONDITIONALLY (returns 0). Use [promote] lockstep=0 to get the original back.\n");
        return;
    }
    bool ok = patch_bytes_guarded(ADDR_RESYNC_STUB_RET, RESYNC_STUB_EXPECT, RESYNC_STUB_PATCH, 3);
    seam_log(ok ? "; resync-wait fix armed: sync_delay_stub -> return 0 (kills the busy-wait garbage-spin hang)\n"
                               : "; resync-wait fix NOT armed (unexpected bytes at sync_delay_stub return -- already patched?)\n");
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

// mp:P9 (2026-09-22) -- `resync_trigger_gate`'s BYTE-PATCH CARRIERS, RE-INSTATED. C8-e retired both
// (2026-07-29) on the strength of the promoted body carrying the fix, and every rig run since has
// been configuration (2), where it does. The players' drop-in (`-net.zip`, ruling Q10) is
// configuration (1): no libmh.dll, nothing promoted, and `refuse_uncarried_fix` wrote "THIS RUN
// DOES NOT HAVE THAT FIX" into all 15 field processes of 2026-09-20 while the mode-8 barrier fired
// every ~2 s (the mp:P8 measurement, 2026-09-22). So the fix comes back as a patch for exactly the
// runs the body cannot reach: unpromoted owner -> the splice; promoted owner -> DISPLACED, the body
// carries it (C1's interlock).
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

// mp:U19i (2026-09-23) -- `gone_peer_frame_guard`'s BYTE-PATCH CARRIER, the P9 shape above applied
// to U19e's fix. The guard was reimpl-only (rx_dispatch.cpp dispatch_packet), so configuration (1)
// -- the -net.zip drop-in, no libmh.dll -- still let the leader's re-broadcast of a drop overwrite
// the datagram it was dispatching, and a clean 2-player quit still ended in outcome 7 with U19d's
// correction doing the cosmetic work. Same three outcomes as resync_trigger_gate: unpromoted owner
// -> the splice; promoted owner -> DISPLACED (our body carries it, C1); neither -> UNCARRIED FIX.
// The guard covers 8 bytes, not the 5 it rewrites: the thunk reads `len` from [ebp-0x38], and the
// `mov eax,[ebp-0x38]` right after the call is what proves that slot is `len` in this image.
constexpr uintptr_t ADDR_GPFG_KICK_CALL = 0x0049c330;                  // == mh::gone_peer_guard::SITE (static_assert below)
const uint8_t      *GPFG_EXPECT         = mh::gone_peer_guard::EXPECT; // call 0x49dc16; mov eax,[ebp-0x38]

static_assert(ADDR_GPFG_KICK_CALL == mh::gone_peer_guard::SITE, "one site, one spelling");
void install_gone_peer_frame_guard() {
    namespace gpg = mh::gone_peer_guard;
    const char *what;
    if (mh::hook::promoted_owner_of(ADDR_GPFG_KICK_CALL)) {
        what = "DISPLACED by promotion -- dispatch_packet's own guard carries it";
    } else {
        gpg::g_buf  = (uint8_t *)mh::state::live_base(mh::state::RID_NET_SEND_BUF);
        gpg::g_kick = gpg::KICK;
        uint8_t repl[8];
        memcpy(repl, GPFG_EXPECT, sizeof(repl));
        const int32_t rel = (int32_t)((uintptr_t)&gpg::thunk - (ADDR_GPFG_KICK_CALL + 5));
        memcpy(repl + 1, &rel, sizeof(rel));
        if (patch_bytes_guarded(ADDR_GPFG_KICK_CALL, GPFG_EXPECT, repl, 8)) {
            what = "PATCHED -- the kick re-broadcast @0049C330 now snapshots the datagram across the emit";
        } else {
            what = "MISMATCHED";
            refuse_uncarried_fix("gone_peer_frame_guard", ADDR_GPFG_KICK_CALL, "llm_net_lockstep_dispatch");
        }
    }
    char b[200];
    wsprintfA(b, "; gone-peer frame guard: %s (MP U19i)\n", what);
    seam_log(b);
}

// mp:U49 -- `undock_reentry_fix`'s BYTE-PATCH CARRIER (same shape as gone_peer_frame_guard above).
// llm_strat_order_queue_dispatch applies an undock (order_code 0x20) with no precondition check, so
// a second 0x20 applied in a LATER pass to a unit already walking out (0x21) re-enters
// exit_storage_begin, can_exit refuses the door's own holder, and the unit sits in EXIT_WAIT on a
// door only it can release (door_mutex_unit = it). Reachable only through the lockstep scheduling
// delay (a double-click lands the 2nd order a pass later); SP applies both in one pass.
//
// The splice is the 6 bytes at 0x00466cb6 (`CMP [EBP-0x28],0 / JNZ 0x00466cda`), whose ONLY
// predecessor is `JMP 0x00466cb6` @0x00466ca3 -- reached with order_code == 0x20 for both the
// boarding (cached is_boarding != 0) and the non-boarding unit. The stub keeps the non-boarding path
// (drop, as retail) and, for a boarding unit, reads the unit's CURRENT state: PARKED (0x1f) or
// EXIT_STORAGE_BEGIN (0x20, the state a same-pass first order just wrote) applies as retail; anything
// else (0x21 walking out, 0x22 waiting, ...) is dropped at 0x0046997f, the loop's `continue`.
// EAX/EDX are dead at all three exits (0x00466cbc, 0x00466ce1 and 0x0046997f reload them).
constexpr uintptr_t ADDR_UNDOCK_SITE = 0x00466cb6;
const uint8_t       UNDOCK_EXPECT[6] = {0x83, 0x7D, 0xD8, 0x00, 0x75, 0x1E};

// clang-format off
__declspec(naked) void undock_reentry_stub() {
    __asm {
        cmp   dword ptr [ebp - 0x28], 0       // cached is_boarding
        jnz   boarding
        mov   eax, 0x00466cbc                 // not boarding: retail's `state == 0x20 / 0x23 -> drop` test
        jmp   eax
    boarding:
        imul  edx, dword ptr [ebp - 0x34], 0x5b04
        imul  eax, dword ptr [ebp - 0x30], 0xe9
        add   eax, edx
        movzx eax, word ptr [eax + 0x00dd8c4e] // unit.state
        cmp   eax, 0x1f
        je    apply
        cmp   eax, 0x20
        je    apply
        mov   eax, 0x0046997f                 // walking out / waiting: drop the record
        jmp   eax
    apply:
        mov   eax, 0x00466ce1                 // retail apply block
        jmp   eax
    }
}
// clang-format on

void install_undock_reentry_fix() {
    const char *what;
    if (mh::hook::promoted_owner_of(ADDR_UNDOCK_SITE)) {
        what = "DISPLACED by promotion -- the promoted dispatcher's own guard carries it";
    } else {
        uint8_t       repl[6] = {0xE9, 0, 0, 0, 0, 0x90};
        const int32_t rel     = (int32_t)((uintptr_t)&undock_reentry_stub - (ADDR_UNDOCK_SITE + 5));
        memcpy(repl + 1, &rel, sizeof(rel));
        if (patch_bytes_guarded(ADDR_UNDOCK_SITE, UNDOCK_EXPECT, repl, 6)) {
            what = "PATCHED -- a 0x20 for a unit that is not PARKED is dropped at 00466CB6";
        } else {
            what = "MISMATCHED";
            refuse_uncarried_fix("undock_reentry_fix", ADDR_UNDOCK_SITE, "llm_strat_order_queue_dispatch");
        }
    }
    char b[200];
    wsprintfA(b, "; undock re-entry fix: %s (MP U49)\n", what);
    seam_log(b);
}

// ===== mp:U52 -- THE RETAIL CARRIERS ([net] team_relations_fix, [net] ally_damage_no_hostility) =========
//
// Configuration (1) (retail sim under mh.dll's net layer, `[config] mode=original`) has no libmh twins, so the three
// pieces of U52 are carried as byte patches, each displaced when its owner is promoted (the twin carries the rule):
//
//  (a) THE SEED. llm_strat_session_begin_multi runs `CALL llm_game_land_players_on_planet` @0x004544ff and then
//      `CALL llm_game_speed_recompute` @0x00454504 (5 bytes, E8 1A 31 04 00). Landing resets every player's AI
//      relation mirror, so the seed must come after it; the second call is spliced to a stub that runs the seed
//      (pairs through the retail llm_diplomacy_set_relation, which also fixes the AI mirror and the chat mask) and
//      tail-jumps to the original callee. Same rule as sim_session_begin_multi.cpp's apply_lobby_team_relations.
//  (b) THE RELATION LOCK. llm_strat_order_queue_dispatch case 0xf (0xf4) calls llm_diplomacy_set_relation @0x004698d2
//      (E8 DC 07 03 00); the stub returns without calling it while the ally-victory flag (Team mode) is set. The
//      call's return lands on `JMP 0x0046997f` (the loop's continue); EAX/EDX/EBX/ECX are all dead there.
//  (c) THE HOSTILITY GUARD. llm_strat_ai_bldg_register_visible_building @0x004db22f computes
//      EAX = victim*0x288fc + aggressor*4 and tests FOREIGN_BLDG_CHANGE_FLAG at 0x004db45c (7 bytes,
//      `CMP [0x00e58350],0`) before the unconditional / unset-only `ai_player_relation[aggressor] = -1`. The stub
//      first tests the victim's relation toward the aggressor (`CMP [EAX+0xe96388],0`): > 0 (ally) -> straight to
//      0x004db47c, past the stamp. Otherwise it repeats the displaced CMP and returns to the JNZ at 0x004db463.
//      EBX is dead from the ADD at 0x004db45a to the reload at 0x004db484; ECX (aggressor_ref) and ESI/EDX/EAX are
//      live and untouched.
//
// The lobby MODE byte arrives through MH_TeamRel_SetLobbyMode (the session-entry observer, net_seams.cpp), because
// configuration (1) has no reimpl_fixes to push it through.
int g_team_relations_fix       = 1; // [net] team_relations_fix -- MP U52; twin + byte patches, DEFAULT ON, sim-affecting
int g_ally_damage_no_hostility = 1; // [net] ally_damage_no_hostility -- MP U52; twin + byte patch, DEFAULT ON, sim-affecting
int g_u52_lobby_team_mode      = 0;


constexpr uintptr_t ADDR_TEAM_SEED_SITE = 0x00454504;
constexpr uintptr_t ADDR_TEAM_LOCK_SITE = 0x004698d2;
constexpr uintptr_t ADDR_ALLY_HOST_SITE = 0x004db45c;
const uint8_t       TEAM_SEED_EXPECT[5] = {0xE8, 0x1A, 0x31, 0x04, 0x00}; // call llm_game_speed_recompute
constexpr uintptr_t ADDR_VIS_AI_SITE    = 0x004698ff;                     // dispatch case 0x10 (0xf5), `CALL llm_game_player_set_ai`
constexpr uintptr_t ADDR_VIS_HUMAN_SITE = 0x00469909;                     // ... and `CALL llm_game_player_set_human`
const uint8_t       VIS_AI_EXPECT[5]    = {0xE8, 0x98, 0x4E, 0x03, 0x00};
const uint8_t       VIS_HUMAN_EXPECT[5] = {0xE8, 0x4B, 0x4E, 0x03, 0x00};
const uint8_t       TEAM_LOCK_EXPECT[5] = {0xE8, 0xDC, 0x07, 0x03, 0x00};             // call llm_diplomacy_set_relation
const uint8_t       ALLY_HOST_EXPECT[7] = {0x83, 0x3D, 0x50, 0x83, 0xE5, 0x00, 0x00}; // cmp [0x00e58350],0
int32_t            *g_u52_flag_ptr      = nullptr;                                    // _G_LLM_STRAT_MP_ALLY_VICTORY_RULE_FLAG, resolved through its region

void __cdecl team_seed_retail() {
    if (*mh::state::ptr<const int32_t>(mh::state::RID_GAME_TUTORIAL_STEP) != 0) return; // llm_game_start_tutorial too
    const uint8_t *pl = mh::state::ptr<const uint8_t>(mh::state::RID_PLAYERS);
    uint8_t        team[8];
    bool           en[8], any = false;
    for (int i = 0; i < 8; ++i) {
        en[i]           = pl[i * 0x34 + 6] != 0; // controller_flags
        const uint8_t t = pl[i * 0x34 + 7];      // the lobby TEAM byte (slot +0x0c)
        team[i]         = (t >= 1 && t <= 4) ? t : 0;
        if (en[i] && team[i] != 0) any = true;
    }
    char b[160];
    if (!any) return;
    int n = 0;
    for (int a = 0; a < 8; ++a)
        for (int c = 0; c < 8; ++c) {
            if (a == c || !en[a] || !en[c]) continue;
            mh::call::llm_diplomacy_set_relation(a, c, (team[a] != 0 && team[a] == team[c]) ? 1 : 2);
            ++n;
        }
    if (g_u52_lobby_team_mode) *mh::state::ptr<int32_t>(mh::state::RID_STRAT_MP_ALLY_VICTORY_RULE_FLAG) = 1;
    // VISION: this peer's own view state (not hashed; differs per peer by design), derived from the synced team
    // bytes. Teammates' bits -> PLAYER_CONTROL_MASK (I share with them) and the human/is_human view mask (they share
    // with me), then the fog is recomputed. Same rule as sim_session_begin_multi.cpp's apply_lobby_team_relations.
    const unsigned me    = *mh::state::ptr<const uint16_t>(mh::state::RID_PLAYERSIDE);
    unsigned       mates = 0;
    if (me < 8 && en[me] && team[me] != 0) {
        for (unsigned j = 0; j < 8; ++j)
            if (j != me && en[j] && team[j] == team[me]) mates |= 1u << j;
        *mh::state::ptr<uint8_t>(mh::state::RID_PLAYER_CONTROL_MASK) |= (uint8_t)mates;
        *mh::state::ptr<uint8_t>(mh::state::RID_GAME_HUMAN_PLAYER_MASK) |= (uint8_t)mates;
        *mh::state::ptr<uint32_t>(mh::state::RID_IS_HUMAN) = *mh::state::ptr<uint8_t>(mh::state::RID_GAME_HUMAN_PLAYER_MASK);
        mh::call::llm_map_fog_of_war_recompute();
    }
    wsprintfA(b, "; U52 (retail seed): %d relation pair(s) set from the lobby teams, mode=%s, vision mates=%02x\n", n,
              g_u52_lobby_team_mode ? "Team" : "FFA", mates);
    seam_log(b);
}

// clang-format off
__declspec(naked) void team_seed_stub() {
    __asm {
        pushad
        pushfd
        call team_seed_retail
        popfd
        popad
        mov  eax, 0x00497623     // llm_game_speed_recompute: the call this stub replaced
        jmp  eax
    }
}
__declspec(naked) void team_lock_stub() {
    __asm {
        mov  ecx, dword ptr [g_u52_flag_ptr]
        cmp  dword ptr [ecx], 0
        jne  locked
        mov  ecx, 0x0049a0b3     // llm_diplomacy_set_relation, args still in EAX/EDX/EBX
        jmp  ecx
    locked:
        ret                      // Team mode: the relation order is a no-op (returns to the case's JMP)
    }
}
__declspec(naked) void vis_ai_lock_stub() {
    __asm {
        mov  ecx, dword ptr [g_u52_flag_ptr]
        cmp  dword ptr [ecx], 0
        jne  locked
        mov  ecx, 0x0049e79c     // llm_game_player_set_ai, player still in EAX
        jmp  ecx
    locked:
        ret                      // Team mode: the 0xf5 vision grant is a no-op (returns to the case's JMP)
    }
}
__declspec(naked) void vis_human_lock_stub() {
    __asm {
        mov  ecx, dword ptr [g_u52_flag_ptr]
        cmp  dword ptr [ecx], 0
        jne  locked
        mov  ecx, 0x0049e759     // llm_game_player_set_human
        jmp  ecx
    locked:
        ret
    }
}
__declspec(naked) void ally_hostile_stub() {
    __asm {
        cmp  dword ptr [eax + 0x00e96388], 0   // the victim's relation toward the aggressor
        jg   ally
        cmp  dword ptr ds:[0x00e58350], 0      // the displaced instruction
        mov  ebx, 0x004db463                   // ... back to its JNZ
        jmp  ebx
    ally:
        mov  ebx, 0x004db47c                   // past the hostility stamp
        jmp  ebx
    }
}
// clang-format on

// The splice bytes: CALL/JMP rel32 to `stub`, NOP-padded to the site's length.
void u52_prep(uint8_t (&repl)[8], uintptr_t site, void *stub, bool is_call) {
    const int32_t rel = (int32_t)((uintptr_t)stub - (site + 5));
    repl[0]           = is_call ? (uint8_t)0xE8 : (uint8_t)0xE9;
    memcpy(repl + 1, &rel, sizeof(rel));
    repl[5] = repl[6] = repl[7] = 0x90;
}

// One site's outcome line + the refusal on a byte mismatch. (The patch_bytes_guarded call itself stays in each
// install_* body, written against its ADDR_* constant, because lint_dll_patches matches that literal.)
void u52_report(const char *what, uintptr_t site, int outcome, const char *knob, const char *owner) {
    static const char *const NAMES[] = {"DISPLACED by promotion -- the twin carries it", "PATCHED", "MISMATCHED"};
    if (outcome == 2) refuse_uncarried_fix(knob, site, owner);
    char b[200];
    wsprintfA(b, "; U52 retail carrier %s @%08X: %s\n", what, (unsigned)site, NAMES[outcome]);
    seam_log(b);
}

void install_team_relations_fix() {
    g_u52_flag_ptr = mh::state::ptr<int32_t>(mh::state::RID_STRAT_MP_ALLY_VICTORY_RULE_FLAG);
    uint8_t repl[8];
    int     outcome = 0;
    if (!mh::hook::promoted_owner_of(ADDR_TEAM_SEED_SITE)) {
        u52_prep(repl, ADDR_TEAM_SEED_SITE, (void *)&team_seed_stub, true);
        outcome = patch_bytes_guarded(ADDR_TEAM_SEED_SITE, TEAM_SEED_EXPECT, repl, 5) ? 1 : 2;
    }
    u52_report("seed", ADDR_TEAM_SEED_SITE, outcome, "team_relations_fix", "llm_strat_session_begin_multi");
    outcome = 0;
    if (!mh::hook::promoted_owner_of(ADDR_TEAM_LOCK_SITE)) {
        u52_prep(repl, ADDR_TEAM_LOCK_SITE, (void *)&team_lock_stub, true);
        outcome = patch_bytes_guarded(ADDR_TEAM_LOCK_SITE, TEAM_LOCK_EXPECT, repl, 5) ? 1 : 2;
    }
    u52_report("lock", ADDR_TEAM_LOCK_SITE, outcome, "team_relations_fix", "llm_strat_order_queue_dispatch");
    outcome = 0;
    if (!mh::hook::promoted_owner_of(ADDR_VIS_AI_SITE)) {
        u52_prep(repl, ADDR_VIS_AI_SITE, (void *)&vis_ai_lock_stub, true);
        outcome = patch_bytes_guarded(ADDR_VIS_AI_SITE, VIS_AI_EXPECT, repl, 5) ? 1 : 2;
    }
    u52_report("vision lock (ai)", ADDR_VIS_AI_SITE, outcome, "team_relations_fix", "llm_strat_order_queue_dispatch");
    outcome = 0;
    if (!mh::hook::promoted_owner_of(ADDR_VIS_HUMAN_SITE)) {
        u52_prep(repl, ADDR_VIS_HUMAN_SITE, (void *)&vis_human_lock_stub, true);
        outcome = patch_bytes_guarded(ADDR_VIS_HUMAN_SITE, VIS_HUMAN_EXPECT, repl, 5) ? 1 : 2;
    }
    u52_report("vision lock (human)", ADDR_VIS_HUMAN_SITE, outcome, "team_relations_fix", "llm_strat_order_queue_dispatch");
}

void install_ally_damage_no_hostility() {
    uint8_t repl[8];
    int     outcome = 0;
    if (!mh::hook::promoted_owner_of(ADDR_ALLY_HOST_SITE)) {
        u52_prep(repl, ADDR_ALLY_HOST_SITE, (void *)&ally_hostile_stub, false);
        outcome = patch_bytes_guarded(ADDR_ALLY_HOST_SITE, ALLY_HOST_EXPECT, repl, 7) ? 1 : 2;
    }
    u52_report("hostility guard", ADDR_ALLY_HOST_SITE, outcome, "ally_damage_no_hostility",
               "llm_strat_ai_bldg_register_visible_building");
}

// ===== mp:U66 -- THE RETAIL CARRIER OF U56 ([net] player_left_pin_fix) ================================
//
// Configuration (1) has no libmh twin, so U56's pinned loser-flag flip is three byte patches (each displaced when its
// owner is promoted -- the twin then carries the rule, so it is never applied twice):
//
//  (a) THE FLIP AT THE ELIMINATION STEP. llm_strat_player_presence_lost @0x00498089, MP path, mode==0: after the
//      natural-loss `AND ALIVE-off` and the `TEST HUMAN` at 0x0049816d, the 7-byte `MOV [EBP-0x34],1` at 0x00498176
//      (was_human = true; reached ONLY for a human that just lost its last presence) is jump-spliced to a stub that
//      repeats it and, when the player is not the local side, makes the flip the receipt used to make: HUMAN off,
//      DEFEATED|GONE on (rule of sim_player_presence_lost.cpp). EAX is dead (0x0049817d reloads it).
//  (b) THE RECEIPT. llm_net_lockstep_dispatch case CTL_PLAYER_LEFT @0x0049c60c: the case opens `IMUL EAX,[EBP-0x28],
//      0x740` (7 bytes, sender record offset) and flips the flags only when the sender is ALIVE. Its first instruction
//      is jump-spliced to a stub that repeats it and records the sender's status byte as it was BEFORE the flip.
//  (c) THE RESTORE. The case then calls llm_net_lockstep_count_active_players @0x0049c683 (E8 62 1D 00 00) and
//      branches on `> 1`. That call is spliced to a stub that runs the original, and -- when another human remains
//      (> 1) and the sender was ALIVE+HUMAN and not yet GONE -- puts the status byte back (rule of rx_dispatch.cpp). A
//      quit marks GONE in its DROP record first, so it is untouched; the last-peer teardown keeps the retail flip.
int g_player_left_pin_fix = 1; // [net] player_left_pin_fix -- MP U56/U66; twin + byte patches, DEFAULT ON, sim-affecting

constexpr uintptr_t ADDR_PIN_FLIP_SITE    = 0x00498176;
constexpr uintptr_t ADDR_PIN_CAPTURE_SITE = 0x0049c60c;
constexpr uintptr_t ADDR_PIN_RESTORE_SITE = 0x0049c683;
const uint8_t       PIN_FLIP_EXPECT[7]    = {0xC7, 0x45, 0xCC, 0x01, 0x00, 0x00, 0x00}; // mov [ebp-0x34],1
const uint8_t       PIN_CAPTURE_EXPECT[7] = {0x69, 0x45, 0xD8, 0x40, 0x07, 0x00, 0x00}; // imul eax,[ebp-0x28],0x740
const uint8_t       PIN_RESTORE_EXPECT[5] = {0xE8, 0x62, 0x1D, 0x00, 0x00};             // call count_active_players
volatile uint8_t    g_pin_before          = 0;                                          // the sender's status byte at receipt

void __cdecl pin_restore_retail(int sender) {
    uint8_t      *sf     = mh::state::ptr<uint8_t>(mh::state::RID_STRAT_PLAYERS) + (size_t)sender * 0x740;
    const uint8_t before = g_pin_before;
    const bool    pin    = (before & 0x02) != 0 && (before & 0x04) != 0 && (before & 0x08) == 0;
    if (pin) *sf = before;
    char b[100];
    wsprintfA(b, "; [rx] CTL_PLAYER_LEFT sender=%d sf=0x%x pin=%d (retail carrier)\n", sender, (unsigned)before, pin ? 1 : 0);
    seam_log(b);
}

// clang-format off
void __cdecl spec_decide_retail();            // mp:U54 -- below
void __cdecl pin_flip_other_retail(unsigned player); // mp:U54 -- below
__declspec(naked) void pin_flip_stub() {
    __asm {
        mov   dword ptr [ebp - 0x34], 1          // the displaced instruction
        movzx eax, word ptr ds:[0x00e58354]      // PlayerSide
        cmp   eax, dword ptr [ebp - 0x30]
        je    mine
        pushad
        push  dword ptr [ebp - 0x30]
        call  pin_flip_other_retail              // HUMAN off, DEFEATED on, GONE on (unless it SPECTATES, mp:U54)
        add   esp, 4
        popad
    pfin:
        push  0x0049817d
        ret
    mine:                                        // mp:U54: the LOCAL human lost its last presence -- does it spectate?
        pushad
        call  spec_decide_retail                 // sets g_spec_pending and makes the same flip when it does
        popad
        jmp   pfin
    }
}
__declspec(naked) void pin_capture_stub() {
    __asm {
        imul  eax, dword ptr [ebp - 0x28], 0x740 // the displaced instruction
        push  edx
        movzx edx, byte ptr [eax + 0x00cff060]
        mov   byte ptr [g_pin_before], dl
        pop   edx
        push  0x0049c613
        ret
    }
}
__declspec(naked) void pin_restore_stub() {
    __asm {
        mov   eax, 0x0049e3ea                    // llm_net_lockstep_count_active_players, the call this replaced
        call  eax
        cmp   eax, 1
        jle   done
        pushad
        push  dword ptr [ebp - 0x28]
        call  pin_restore_retail
        add   esp, 4
        popad
    done:
        ret
    }
}
// clang-format on

void install_player_left_pin_fix() {
    uint8_t repl[8];
    int     outcome = 0;
    if (!mh::hook::promoted_owner_of(ADDR_PIN_FLIP_SITE)) {
        u52_prep(repl, ADDR_PIN_FLIP_SITE, (void *)&pin_flip_stub, false);
        outcome = patch_bytes_guarded(ADDR_PIN_FLIP_SITE, PIN_FLIP_EXPECT, repl, 7) ? 1 : 2;
    }
    u52_report("pin flip", ADDR_PIN_FLIP_SITE, outcome, "player_left_pin_fix", "llm_strat_player_presence_lost");
    outcome = 0;
    if (!mh::hook::promoted_owner_of(ADDR_PIN_CAPTURE_SITE)) {
        u52_prep(repl, ADDR_PIN_CAPTURE_SITE, (void *)&pin_capture_stub, false);
        outcome = patch_bytes_guarded(ADDR_PIN_CAPTURE_SITE, PIN_CAPTURE_EXPECT, repl, 7) ? 1 : 2;
    }
    u52_report("pin capture", ADDR_PIN_CAPTURE_SITE, outcome, "player_left_pin_fix", "llm_net_lockstep_dispatch");
    outcome = 0;
    if (!mh::hook::promoted_owner_of(ADDR_PIN_RESTORE_SITE)) {
        u52_prep(repl, ADDR_PIN_RESTORE_SITE, (void *)&pin_restore_stub, true);
        outcome = patch_bytes_guarded(ADDR_PIN_RESTORE_SITE, PIN_RESTORE_EXPECT, repl, 5) ? 1 : 2;
    }
    u52_report("pin restore", ADDR_PIN_RESTORE_SITE, outcome, "player_left_pin_fix", "llm_net_lockstep_dispatch");
}

// ===== mp:U54 -- SPECTATE AFTER DEFEAT: THE RETAIL CARRIERS ([net] spectate_after_defeat) ============================
//
// Configuration (1) has no libmh twin (sim_player_presence_lost.cpp / order_queue.cpp carry these rules in configuration (2)),
// so the same rules are byte patches, each displaced when its owner is promoted (the twin then carries it). The rules are
// mh_spectate.h's, read from the live roster. Three sites + the extended U66 flip stub above:
//
//  S1  0x00498176 (pin_flip_stub, mode 0, a human just lost its last presence): when that human is the LOCAL player, decide
//      whether it spectates (>= 2 other humans alive, not decided by the team rule) and make the same flip every survivor
//      makes for it (HUMAN off, DEFEATED|GONE on); g_spec_pending remembers the decision for S2.
//  S2  0x00498197 (`CALL send_presence_lost; JMP 0x0049832c`, the local player's drop): a spectator sends no CTL_PLAYER_LEFT
//      and does not take the session 3->2 downgrade at 0x0049832c -- straight to the fanfare + outcome dialog (0x00498758).
//  S3  0x004982a8 (`CMP [SESSION],3`, after the roster loop of a player that is NOT the local one): for a spectator the
//      survivors' evaluation is the wrong question; it ends its own match when the survivors have decided theirs (spectate.h),
//      with the outcome of its own side (5 victory / 4 defeat), else it just returns (0x004987a5).
//  S0  0x0049814b (the MP path's first instruction): presence_lost is RE-ENTERED for a player that is already out (the
//      force-killed units' teardowns each call it). A spectator asked about ITSELF again returns at once -- the second
//      call would see was_human == false, take the local-player drop path and end the session it chose to keep.
//  S4  0x00466068 (`CALL stage_scheduled` in llm_strat_order_dispatch's replicated lane): an order whose owner is a spectator
//      is dropped BEFORE it is staged (return 0, `RET 8` for the double the caller pushed).
int __cdecl spec_eval_retail(unsigned player, unsigned mode);
int __cdecl spec_drop_retail(unsigned player);
int __cdecl spec_repeat_retail(unsigned player);
constexpr uintptr_t ADDR_SPEC_ENTRY_SITE = 0x0049814b;
constexpr uintptr_t ADDR_SPEC_SEND_SITE  = 0x00498197;
constexpr uintptr_t ADDR_SPEC_END_SITE   = 0x004982a8;
constexpr uintptr_t ADDR_SPEC_ORDER_SITE = 0x00466068;
const uint8_t       SPEC_ENTRY_EXPECT[7] = {0xC7, 0x45, 0xCC, 0x00, 0x00, 0x00, 0x00};                   // mov [ebp-0x34],0 (the MP path's first instruction)
const uint8_t       SPEC_SEND_EXPECT[10] = {0xE8, 0x8C, 0x61, 0x00, 0x00, 0xE9, 0x8B, 0x01, 0x00, 0x00}; // call send_presence_lost; jmp 0x0049832c
const uint8_t       SPEC_END_EXPECT[7]   = {0x83, 0x3D, 0x44, 0x83, 0xE5, 0x00, 0x03};                   // cmp [SESSION],3
const uint8_t       SPEC_ORDER_EXPECT[5] = {0xE8, 0xA4, 0x01, 0x00, 0x00};                               // call stage_scheduled

// U66's flip for a player other than the local side: HUMAN off, DEFEATED|GONE on -- except that a human who stays as a
// SPECTATOR is not GONE (mh_spectate.h). Every peer evaluates the same roster here, so every peer makes the same call.
void __cdecl pin_flip_other_retail(unsigned player) {
    if (player >= 8) return;
    uint32_t *sf   = (uint32_t *)(mh::addr::_G_LLM_STRAT_PLAYERS + player * 0x740u);
    bool      spec = false;
    if (g_spectate_after_defeat && *(const uint8_t *)ADDR_SESSION_MODE == 3) {
        mh::spectate::roster r;
        spec_read_roster(r);
        spec = mh::spectate::becomes_spectator(r, (int)player, true, true, true);
    }
    const uint8_t b = (uint8_t)mh::spectate::eliminated_flags(*sf, spec);
    *(uint8_t *)sf  = b; // byte 0 only, as the stub it replaces
}
void __cdecl spec_decide_retail() {
    g_spec_pending = 0;
    if (!g_spectate_after_defeat || *(const uint8_t *)ADDR_SESSION_MODE != 3) return;
    const unsigned me = *(const uint16_t *)mh::addr::PlayerSide;
    if (me >= 8) return;
    mh::spectate::roster r;
    spec_read_roster(r);
    if (!mh::spectate::becomes_spectator(r, (int)me, true, true, true)) return;
    g_spec_pending = 1;
    uint8_t *sf    = (uint8_t *)(mh::addr::_G_LLM_STRAT_PLAYERS + me * 0x740u);
    *sf            = (uint8_t)mh::spectate::eliminated_flags(*sf, true); // HUMAN off, DEFEATED on, GONE off -- the survivors' flip
    const auto vd  = mh::spectate::evaluate(r, (int)me);
    char       b[200];
    wsprintfA(b, "; U54 spectate: local human eliminated -> SPECTATOR (alive others=%d humans=%d) -- no CTL_PLAYER_LEFT, session stays lockstep (retail carrier)\n",
              vd.alive, vd.humans);
    seam_log(b);
}
// 0 = not a spectator (retail evaluation runs), 1 = a spectator, the match goes on, 2 = decided and its side won, 3 = decided, lost.
int __cdecl spec_eval_retail(unsigned player, unsigned mode) {
    if (!local_is_spectator()) return 0;
    const unsigned       me = *(const uint16_t *)mh::addr::PlayerSide;
    mh::spectate::roster r;
    spec_read_roster(r);
    player &= 0xffff;
    if (mode != 0 && player < 8) {
        // a FORCED removal (CTL_PLAYER_LEFT's receipt) races this peer's own natural flip of the leaver: the leaver is out either way
        r.flags[player] &= ~(mh::spectate::ST_ALIVE | mh::spectate::ST_HUMAN);
        r.flags[player] |= mh::spectate::ST_DEFEATED;
    }
    const auto vd = mh::spectate::evaluate(r, (int)me);
    if (!mh::spectate::decided(r, vd)) return 1;
    const bool won = mh::spectate::side_won(r, vd);
    char       b[200];
    wsprintfA(b, "; U54 spectate: the survivors decided the match (alive=%d humans=%d) -- the spectator's side %s (retail carrier)\n", vd.alive, vd.humans,
              won ? "WON" : "lost");
    seam_log(b);
    return won ? 2 : 3;
}
// 1 = the LOCAL player is already a spectator and is being asked about itself again: nothing to decide.
int __cdecl spec_repeat_retail(unsigned player) {
    return local_is_spectator() && (player & 0xffff) == *(const uint16_t *)mh::addr::PlayerSide;
}
int __cdecl spec_drop_retail(unsigned player) {
    player &= 0xf;
    if (player >= 8) return 0;
    const uint32_t f         = *(const uint32_t *)(mh::addr::_G_LLM_STRAT_PLAYERS + player * 0x740u);
    const bool     drop      = mh::spectate::is_spectator_flags(f);
    static int     s_dropped = 0;
    if (drop && ++s_dropped <= 8) {
        char b[128];
        wsprintfA(b, "; U54 spectate: dropped order owner=%u (#%d) (retail carrier)\n", player, s_dropped);
        seam_log(b);
    }
    return drop;
}

// clang-format off
__declspec(naked) void spec_entry_stub() {
    __asm {
        mov   dword ptr [ebp - 0x34], 0          // the displaced instruction
        pushad
        push  dword ptr [ebp - 0x30]             // player
        call  spec_repeat_retail
        add   esp, 4
        mov   dword ptr [g_spec_res], eax
        popad
        cmp   dword ptr [g_spec_res], 0
        jne   quiet
        push  0x00498152                         // back to `cmp [mode],0`
        ret
    quiet:
        push  0x004987a5                         // a spectator asked about itself again: return
        ret
    }
}
__declspec(naked) void spec_send_stub() {
    __asm {
        cmp   dword ptr [g_spec_pending], 0
        jne   spec
        mov   eax, 0x0049e328                    // llm_net_lockstep_send_presence_lost, the call this replaced
        call  eax
        push  0x0049832c                         // ... and the JMP behind it (session downgrade + overlay dismiss)
        ret
    spec:
        mov   dword ptr [g_spec_pending], 0
        push  0x00498758                         // a spectator: no CTL_PLAYER_LEFT, no downgrade -- fanfare + the outcome dialog
        ret
    }
}
__declspec(naked) void spec_end_stub() {
    __asm {
        pushad
        push  dword ptr [ebp - 0x2c]             // mode
        push  dword ptr [ebp - 0x30]             // player
        call  spec_eval_retail
        add   esp, 8
        mov   dword ptr [g_spec_res], eax
        popad
        cmp   dword ptr [g_spec_res], 0
        je    normal
        cmp   dword ptr [g_spec_res], 1
        je    undecided
        mov   byte ptr [ebp - 0x14], 4           // outcome: defeat
        cmp   dword ptr [g_spec_res], 2
        jne   decided
        mov   byte ptr [ebp - 0x14], 5           // outcome: victory (the spectator's side won)
        mov   eax, 0x7d
        mov   edx, 0x0049653d                    // llm_ui_print_queue_text_id(TEXT_VICTORY)
        call  edx
    decided:
        push  0x0049832c                         // the common downgrade + fanfare + dialog tail
        ret
    undecided:
        push  0x004987a5                         // return: the match is still being played
        ret
    normal:
        cmp   dword ptr ds:[0x00e58344], 3       // the displaced instruction
        push  0x004982af                         // ... back to its JNE
        ret
    }
}
__declspec(naked) void spec_order_stub() {
    __asm {
        pushad
        push  edx                                // owner (low word is the player index; spec_drop_retail masks it)
        call  spec_drop_retail
        add   esp, 4
        mov   dword ptr [g_spec_res], eax
        popad
        cmp   dword ptr [g_spec_res], 0
        jne   drop
        push  0x00466211                         // llm_strat_order_stage_scheduled: the call this replaced (return address intact)
        ret
    drop:
        xor   eax, eax
        ret   8                                  // dropped: 0 = not queued, and pop the double the caller pushed
    }
}
// clang-format on

void install_spectate() {
    uint8_t repl[8];
    int     outcome = 0;
    if (!mh::hook::promoted_owner_of(ADDR_SPEC_ENTRY_SITE)) {
        u52_prep(repl, ADDR_SPEC_ENTRY_SITE, (void *)&spec_entry_stub, false);
        outcome = patch_bytes_guarded(ADDR_SPEC_ENTRY_SITE, SPEC_ENTRY_EXPECT, repl, 7) ? 1 : 2;
    }
    u52_report("spectate entry", ADDR_SPEC_ENTRY_SITE, outcome, "spectate_after_defeat", "llm_strat_player_presence_lost");
    outcome = 0;
    if (!mh::hook::promoted_owner_of(ADDR_SPEC_SEND_SITE)) {
        uint8_t r10[10];
        u52_prep(repl, ADDR_SPEC_SEND_SITE, (void *)&spec_send_stub, false);
        memcpy(r10, repl, 5);
        memset(r10 + 5, 0x90, 5);
        outcome = patch_bytes_guarded(ADDR_SPEC_SEND_SITE, SPEC_SEND_EXPECT, r10, 10) ? 1 : 2;
    }
    u52_report("spectate send", ADDR_SPEC_SEND_SITE, outcome, "spectate_after_defeat", "llm_strat_player_presence_lost");
    outcome = 0;
    if (!mh::hook::promoted_owner_of(ADDR_SPEC_END_SITE)) {
        u52_prep(repl, ADDR_SPEC_END_SITE, (void *)&spec_end_stub, false);
        outcome = patch_bytes_guarded(ADDR_SPEC_END_SITE, SPEC_END_EXPECT, repl, 7) ? 1 : 2;
    }
    u52_report("spectate end", ADDR_SPEC_END_SITE, outcome, "spectate_after_defeat", "llm_strat_player_presence_lost");
    outcome = 0;
    if (!mh::hook::promoted_owner_of(ADDR_SPEC_ORDER_SITE)) {
        u52_prep(repl, ADDR_SPEC_ORDER_SITE, (void *)&spec_order_stub, true);
        outcome = patch_bytes_guarded(ADDR_SPEC_ORDER_SITE, SPEC_ORDER_EXPECT, repl, 5) ? 1 : 2;
    }
    u52_report("spectate order gate", ADDR_SPEC_ORDER_SITE, outcome, "spectate_after_defeat", "llm_strat_order_dispatch");
}


// ---- mp:U54 THE SPECTATOR'S WHOLE-MAP VIEW (render-only) ---------------------------------------------------------------
//
// A spectator sees the WHOLE MAP (user decision). The fog lives in the tile_objects plane -- per tile record, flags[1] bit 7
// "explored" / bit 6 "fogged" and byte 7 `visibility` (the mask of players that currently see it, which every object-draw
// test reads as `!= 0`) -- and that plane IS HASHED (hash region `tile_objects`, 524288 B), so it must not be written for good:
// the spectator's per-step hash has to stay the survivors'. Hence a DRAW-TIME override: llm_strat_render_view (retail
// @0x0044e5a4, reached both from the retail frame and, in configuration (2), through the libmh frame twin's host event) is
// wrapped; BEFORE it runs the two fog bytes of every tile are saved and set to "explored, not fogged, seen"; AFTER it they
// are put back exactly. Nothing between the two reads them for the sim -- the render pass is single-threaded with the sim
// step (the frame is time_tick -> sim_tick -> render) -- so the hash never sees the override. The first reveals self-check
// it: a checksum of the whole plane before the override and after the restore is logged, and must be equal.
void               *g_sv_tramp      = nullptr;
bool                g_sv_active     = false;
int                 g_sv_checks     = 0;
uint32_t            g_sv_sum_before = 0;
uint8_t             g_sv_vis[65536];
uint8_t             g_sv_flag[65536];
constexpr uintptr_t ADDR_RENDER_VIEW = 0x0044e5a4;

uint32_t sv_checksum(const uint8_t *base) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < 524288; ++i) h = (h ^ base[i]) * 16777619u;
    return h;
}
void spec_view_pre() {
    g_sv_active = false;
    if (!local_is_spectator()) return;
    uint8_t *base = mh::state::ptr<uint8_t>(mh::state::RID_TILE_OBJECTS);
    if (!base) return;
    if (g_sv_checks < 4) g_sv_sum_before = sv_checksum(base);
    for (int i = 0; i < 65536; ++i) {
        uint8_t *t   = base + (size_t)i * 8;
        g_sv_flag[i] = t[1];
        g_sv_vis[i]  = t[7];
        t[1]         = (uint8_t)((t[1] & 0x3f) | 0x80); // explored, not fogged
        t[7]         = 0xff;                            // seen
    }
    g_sv_active = true;
}
void spec_view_post() {
    if (!g_sv_active) return;
    g_sv_active   = false;
    uint8_t *base = mh::state::ptr<uint8_t>(mh::state::RID_TILE_OBJECTS);
    for (int i = 0; i < 65536; ++i) {
        uint8_t *t = base + (size_t)i * 8;
        t[1]       = g_sv_flag[i];
        t[7]       = g_sv_vis[i];
    }
    if (g_sv_checks < 4) {
        ++g_sv_checks;
        const uint32_t after = sv_checksum(base);
        char           b[160];
        wsprintfA(b, "; U54 view: whole-map reveal + restore #%d: tile_objects checksum before=%08x after=%08x -- %s\n", g_sv_checks,
                  (unsigned)g_sv_sum_before, (unsigned)after, after == g_sv_sum_before ? "RESTORED EXACTLY (the hashed plane is untouched)" : "MISMATCH");
        seam_log(b);
    }
}
// clang-format off
__declspec(naked) void spec_view_detour() {
    __asm {
        pushad
        pushfd
        call spec_view_pre
        popfd
        popad
        call dword ptr [g_sv_tramp]  // WRAP: the stolen prologue + the rest of llm_strat_render_view, returns HERE
        pushad
        pushfd
        call spec_view_post
        popfd
        popad
        ret
    }
}
// clang-format on
void install_spectate_view() {
    if (install_trampoline(ADDR_RENDER_VIEW, (void *)spec_view_detour, &g_sv_tramp, 8, mh::hook::entry_claim::exclusive,
                           "the spectator's whole-map view"))
        seam_log("; U54 view: llm_strat_render_view wrapped -- a spectator sees the whole map (a draw-time override of the two fog bytes per tile, restored after the draw)\n");
    else
        seam_log("; U54 view: NOT armed (render_view entry unavailable) -- see the [interlock] line\n");
}

// ---- the per-frame spectator seam (on_present) -----------------------------------------------------------------------
//  * keeps the transport's spectator mask in step with the roster (MH_Net_SetSpectator, mp:U71): a seat that is out of the
//    match (ALIVE and HUMAN off, DEFEATED|GONE on) is not a voter in the failover quorum;
//  * a spectator that Exit-ed to the main menu leaves the match: the session ends here (the sim is not stepping any more,
//    the survivors flipped its slot at the elimination step) -- the hub handover itself is hub_leave_poll's.
void spectator_tick() {
    static DWORD s_next = 0;
    static int   s_mask = 0;
    const DWORD  now    = GetTickCount();
    if ((long)(now - s_next) < 0) return;
    s_next   = now + 100;
    int mask = 0;
    if (g_spectate_after_defeat && g_spectate_mask) {
        for (int i = 0; i < 8; ++i) {
            const uint32_t f = *(const uint32_t *)(mh::addr::_G_LLM_STRAT_PLAYERS + (unsigned)i * 0x740u);
            if (mh::spectate::is_spectator_flags(f)) mask |= 1 << i;
        }
    }
    if (mask != s_mask) {
        for (int i = 0; i < 8; ++i)
            if (((mask ^ s_mask) >> i) & 1) MH_Net_SetSpectator(i, (mask >> i) & 1);
        char b[96];
        wsprintfA(b, "; U71 spectator mask %02x -> %02x (MH_Net_SetSpectator)\n", s_mask, mask);
        seam_log(b);
        s_mask = mask;
    }
    {
        // The two answers to the defeat dialog, as markers the checker reads: Continue closes the modal (game mode back to 2).
        static bool   s_dlg_seen = false;
        const bool    spec       = local_is_spectator();
        const uint8_t gm         = *(const uint8_t *)mh::addr::_G_LLM_GAME_MODE;
        if (spec && gm == 3) s_dlg_seen = true;
        else if (spec && gm == 2 && s_dlg_seen) {
            s_dlg_seen = false;
            seam_log("; U54 spectate: dialog answered CONTINUE (game mode back to 2) -- watching on\n");
        } else if (!spec) s_dlg_seen = false;
    }
    static bool s_was_spec = false; // latched: the statistics screen's teardown clears the roster before the menu is reached
    if (local_is_spectator() && !s_was_spec) {
        s_was_spec          = true;
        g_spec_menu_reached = false;
        seam_log("; U54 spectate: local seat latched as a spectator\n");
    }
    // The main menu reached after Exit match: the retail flag _G_LLM_UI_IN_MAIN_MENU is NOT set on this route (measured), so
    // read the idle menu itself -- GAME_MODE 3 + MENU_STATE 3 -- but only AFTER the statistics screen (MENU_STATE 10) was
    // seen, because the defeat dialog also runs under GAME_MODE 3 with a stale MENU_STATE 3.
    static bool s_stats_seen = false;
    {
        const uint8_t gm = *(const uint8_t *)mh::addr::_G_LLM_GAME_MODE;
        const uint8_t ms = *(const uint8_t *)mh::addr::_G_LLM_UI_MENU_STATE;
        if (!s_was_spec || gm != 3) s_stats_seen = false;
        else if (ms == 10) s_stats_seen = true;
    }
    const bool at_menu = *(const int *)ADDR_UI_IN_MAIN_MENU != 0 ||
                         (s_stats_seen && *(const uint8_t *)mh::addr::_G_LLM_UI_MENU_STATE == 3);
    if (s_was_spec && at_menu) {
        s_was_spec                              = false;
        s_stats_seen                            = false;
        g_spec_menu_reached                     = true;
        *(volatile uint32_t *)ADDR_SESSION_MODE = 2; // SESSION_MP_LOCKSTEP -> SESSION_MP_LOCAL, as the gameover seam does
        mp_session_close("gameover");
        g_spec_msg_pending = 0;
        seam_log("; U54 spectate: the spectator left the match (main menu reached) -- session closed\n");
    }
}


// mp:U45 -- `diplo_order_dedup_fix`'s BYTE-PATCH CARRIER (same shape as undock_reentry_fix above).
// llm_strat_order_release_due matches a due PENDING record against every QUEUE record with the same
// unit_index + owner_and_kind and (param0 or order_code differing); then the higher order_code wins at
// equal exec_time. One diplomacy Apply issues 0xf4 (relation) and 0xf5 (control mode) with unit 0, the
// issuer as owner, at one exec_time, so 0xf5 superseded 0xf4 on every peer. The twin
// (order_queue.cpp, g_admin_dedup_exempt) `continue`s when BOTH codes are 0xf4/0xf5.
//
// The splice is the 7 bytes at 0x0046667b (`MOV [EBP-0x34],0` -- the first instruction of the supersede
// test), whose ONLY predecessor is `JMP 0x0046667b` @0x00466674 (identity matched, params/codes
// differ). The stub reads the queue record's (index [EBP-0x30]) and the pending record's (index
// [EBP-0x24]) order_code; both in {0xf4,0xf5} -> jump to the loop's `continue` at 0x00466704 (matched
// stays as it was, exactly the twin), else run the displaced MOV and rejoin at 0x00466682. EAX/EDX are
// dead at both entries (0x00466682 reloads them); flags are dead (the next insn is IMUL).
constexpr uintptr_t ADDR_DIPLO_DEDUP_SITE = 0x0046667b;
const uint8_t       DIPLO_DEDUP_EXPECT[7] = {0xC7, 0x45, 0xCC, 0x00, 0x00, 0x00, 0x00};

// clang-format off
__declspec(naked) void diplo_dedup_stub() {
    __asm {
        imul  edx, dword ptr [ebp - 0x30], 0x44
        movzx edx, word ptr [edx + 0x00bb4ede]  // queue[q].order_code
        sub   edx, 0xf4
        cmp   edx, 1
        ja    normal                            // not 0xf4/0xf5
        imul  eax, dword ptr [ebp - 0x24], 0x44
        movzx eax, word ptr [eax + 0x00bb9e8e]  // pending[p].order_code
        sub   eax, 0xf4
        cmp   eax, 1
        ja    normal
        mov   eax, 0x00466704                   // both diplomacy admin orders: independent, never superseded
        jmp   eax
    normal:
        mov   dword ptr [ebp - 0x34], 0         // the displaced instruction
        mov   eax, 0x00466682
        jmp   eax
    }
}
// clang-format on

// mp:U44 -- `overlay_dialog_guard`'s RETAIL CARRIER (configuration (1) / `[config] mode=original`).
// The libmh twin (lockstep/overlay_hoist.cpp + host_event_sink.cpp) wraps every call of the stall overlay's
// callees with latch / hoist so that (1) lockstep only dismisses a screen it armed, (2) a kick modal that
// arms over an open building dialog stashes that dialog's UI state and gives it back on dismiss, and (3) the
// viewport is dirtied when a mode-3 screen is dismissed. Under the original bodies nothing runs that code,
// so the retail llm_net_lockstep_overlay_dismiss (0x004c7d1c) still forces mode 2 over a mode-3 dialog (and
// leaves _G_LLM_UI_ACTIVE_DIALOG pointing at it) and llm_net_lockstep_sync_overlay_show (0x004c7eea) hijacks
// the screen while the dialog's input route is still live.
//
// THE SPLICE IS PER CALL SITE, NOT PER ENTRY. The twin reaches the retail entries too (through the host
// callbacks), so an entry detour would run both ledgers. Each retail call site is rewritten
// `call X` -> `call odg::*_thunk` and is DISPLACED when promoted_owner_of(site) says libmh's body owns it:
//   dismiss x7: time_tick 0x0043f072/0x0043f286/0x0043f2e1, presence_lost 0x004982c3/0x0049833f,
//               sync_overlay_show's consume path 0x004c7fae (never executed under the twin -- its emit
//               only fires on a frame where the answer is -1), mp_leave 0x004c8620 (only when libmh does
//               not run at all: the twin's mp_leave hoist sits AFTER the host callback that contains it)
//   show    x1: time_tick 0x0043f18d
//   wait    x1: 0x0043f0fc, already owned by wait_overlay_gate_thunk (icon_count); its forward target
//               g_wait_target is pointed at odg::wait_thunk.
// The helpers below are the twin's overlay_hoist.cpp over raw VAs (mh::addr), same ledger, same order.
namespace odg {

struct ledger {
    bool    ours       = false; // the icon / kick modal is lockstep's own, armed and not yet dismissed
    bool    latch_ours = false;
    uint8_t latch_mode = 0;
    struct {
        bool     valid = false;
        uint32_t list = 0, dialog = 0, cb = 0, cb_a = 0, cb_b = 0, flags = 0;
        uint8_t  menu_state = 0;
    } stash;
};
ledger   g;
unsigned g_n_dismiss = 0, g_n_show = 0, g_n_wait = 0; // diagnostics

constexpr uintptr_t       ADDR_CB_A = mh::addr::menu_oneshot_cb; // _G_LLM_UI_MENU_ASYNC_CALLBACK_A
inline volatile uint8_t  &mode() { return *(volatile uint8_t *)mh::addr::_G_LLM_GAME_MODE; }
inline volatile uint32_t &u32(uintptr_t a) { return *(volatile uint32_t *)a; }
inline bool               slot_free() {
    const uint32_t list = u32(mh::addr::_G_LLM_UI_MENU_WIDGET_LIST);
    return list == 0 || list == mh::addr::_G_LLM_UI_WGT_LIST_GAMEPLAY_HUD;
}

// bit0 = GAME_MODE != 4 (the callee's guard), bit1 = the screen was mode 3 (viewport dirty after).
unsigned __cdecl pre_dismiss() {
    ++g_n_dismiss;
    g.latch_mode = mode();
    g.latch_ours = g.ours;
    return (mode() != 4 ? 1u : 0u) | (mode() == 3 ? 2u : 0u);
}

void __cdecl post_dismiss(unsigned flags) {
    if (flags & 2) mh::call::llm_map_cam_mark_viewport_dirty(); // twin: host_event_sink, right after the original
    if (!(flags & 1)) return;
    if (!g.latch_ours) { // not lockstep's screen: the original forced mode 2/3 over it -- put the mode back
        mode() = g.latch_mode;
        return;
    }
    const uint32_t list       = u32(mh::addr::_G_LLM_UI_MENU_WIDGET_LIST);
    const bool     free_after = list == 0 || list == mh::addr::_G_LLM_UI_WGT_LIST_GAMEPLAY_HUD ||
                            list == mh::addr::_G_LLM_UI_WGT_LIST_LOCKSTEP_SYNC;
    mode() = free_after ? 2 : 3;
    g.ours = false;
    if (g.stash.valid) { // give the player's dialog back exactly as the modal found it
        const auto &st                                      = g.stash;
        u32(mh::addr::_G_LLM_UI_MENU_WIDGET_LIST)           = st.list;
        u32(mh::addr::_G_LLM_UI_ACTIVE_DIALOG)              = st.dialog;
        u32(mh::addr::_G_LLM_UI_MENU_ASYNC_CALLBACK)        = st.cb;
        u32(ADDR_CB_A)                                      = st.cb_a;
        u32(mh::addr::_G_LLM_UI_MENU_ASYNC_CALLBACK_B)      = st.cb_b;
        u32(mh::addr::_G_LLM_DLG_STATE_FLAGS)               = st.flags;
        *(volatile uint8_t *)mh::addr::_G_LLM_UI_MENU_STATE = st.menu_state;
        mode()                                              = 3;
        g.stash.valid                                       = false;
    }
}

// Before sync_overlay_show: armed = slot free && mode != 4 (its own arming guard). A foreign screen is mode 3
// with a free list lockstep did not arm -- a dialog: stash its UI state, null ACTIVE_DIALOG.
void __cdecl pre_show() {
    ++g_n_show;
    const bool armed = slot_free() && mode() != 4;
    if (!armed) return;
    if (mode() == 3 && !g.ours && !g.stash.valid) {
        auto &st                               = g.stash;
        st.list                                = u32(mh::addr::_G_LLM_UI_MENU_WIDGET_LIST);
        st.dialog                              = u32(mh::addr::_G_LLM_UI_ACTIVE_DIALOG);
        st.cb                                  = u32(mh::addr::_G_LLM_UI_MENU_ASYNC_CALLBACK);
        st.cb_a                                = u32(ADDR_CB_A);
        st.cb_b                                = u32(mh::addr::_G_LLM_UI_MENU_ASYNC_CALLBACK_B);
        st.flags                               = u32(mh::addr::_G_LLM_DLG_STATE_FLAGS);
        st.menu_state                          = *(volatile uint8_t *)mh::addr::_G_LLM_UI_MENU_STATE;
        st.valid                               = true;
        u32(mh::addr::_G_LLM_UI_ACTIVE_DIALOG) = 0;
    }
    g.ours = true;
}

// Before wait_player_overlay_show: its arming guard is slot free && mode == 2.
unsigned __cdecl pre_wait() {
    ++g_n_wait;
    return (slot_free() && mode() == 2) ? 1u : 0u;
}
void __cdecl post_wait(unsigned armed) {
    if (armed) {
        mode() = 3;
        g.ours = true;
    }
    // The dialog's own frame function is still on the stack on the frame the modal armed and re-publishes
    // ACTIVE_DIALOG after we nulled it; this runs on every parked frame, so the stash stays authoritative.
    if (g.stash.valid) u32(mh::addr::_G_LLM_UI_ACTIVE_DIALOG) = 0;
}

// clang-format off
// The three splice targets. All preserve EBX/ESI/EDI (the C++ helpers do too) and return the callee's EAX.
__declspec(naked) void dismiss_thunk() {
    __asm {
        push  ebx
        push  esi
        push  edi
        call  pre_dismiss
        mov   esi, eax
        mov   eax, 0x004c7d1c        // retail llm_net_lockstep_overlay_dismiss
        call  eax
        mov   ebx, eax
        push  esi
        call  post_dismiss
        add   esp, 4
        mov   eax, ebx
        pop   edi
        pop   esi
        pop   ebx
        ret
    }
}
__declspec(naked) void show_thunk() {
    __asm {
        push  ebx
        push  esi
        push  edi
        call  pre_show
        mov   eax, 0x004c7eea        // retail llm_net_lockstep_sync_overlay_show
        call  eax
        pop   edi
        pop   esi
        pop   ebx
        ret
    }
}
__declspec(naked) void wait_thunk() { // EAX = player_idx (watcall)
    __asm {
        push  ebx
        push  esi
        push  edi
        mov   edi, eax               // pidx
        call  pre_wait
        mov   esi, eax               // armed
        mov   eax, edi
        mov   edx, 0x004c7dc0        // retail llm_net_lockstep_wait_player_overlay_show
        call  edx
        mov   ebx, eax
        push  esi
        call  post_wait
        add   esp, 4
        mov   eax, ebx
        pop   edi
        pop   esi
        pop   ebx
        ret
    }
}
// clang-format on

} // namespace odg

void install_diplo_order_dedup_fix() {
    const char *what;
    if (mh::hook::promoted_owner_of(ADDR_DIPLO_DEDUP_SITE)) {
        what = "DISPLACED by promotion -- the promoted release_due's own exemption carries it";
    } else {
        uint8_t       repl[7] = {0xE9, 0, 0, 0, 0, 0x90, 0x90};
        const int32_t rel     = (int32_t)((uintptr_t)&diplo_dedup_stub - (ADDR_DIPLO_DEDUP_SITE + 5));
        memcpy(repl + 1, &rel, sizeof(rel));
        if (patch_bytes_guarded(ADDR_DIPLO_DEDUP_SITE, DIPLO_DEDUP_EXPECT, repl, 7)) {
            what = "PATCHED -- 0xf4/0xf5 never supersede each other at 0046667B";
        } else {
            what = "MISMATCHED";
            refuse_uncarried_fix("diplo_order_dedup_fix", ADDR_DIPLO_DEDUP_SITE, "llm_strat_order_release_due");
        }
    }
    char b[200];
    wsprintfA(b, "; diplo order dedup fix: %s (MP U45)\n", what);
    seam_log(b);
}

struct odg_site {
    uintptr_t site;
    uint8_t   expect[5];
    void (*thunk)();
    const char *owner;
    bool        ours_skip; // leave it to the twin whenever libmh runs at all
};
const odg_site ODG_SITES[] = {
    {0x0043f072, {0xE8, 0xA5, 0x8C, 0x08, 0x00}, odg::dismiss_thunk, "llm_strat_time_tick", false},
    {0x0043f286, {0xE8, 0x91, 0x8A, 0x08, 0x00}, odg::dismiss_thunk, "llm_strat_time_tick", false},
    {0x0043f2e1, {0xE8, 0x36, 0x8A, 0x08, 0x00}, odg::dismiss_thunk, "llm_strat_time_tick", false},
    {0x004982c3, {0xE8, 0x54, 0xFA, 0x02, 0x00}, odg::dismiss_thunk, "llm_strat_player_presence_lost", false},
    {0x0049833f, {0xE8, 0xD8, 0xF9, 0x02, 0x00}, odg::dismiss_thunk, "llm_strat_player_presence_lost", false},
    {0x004c7fae, {0xE8, 0x69, 0xFD, 0xFF, 0xFF}, odg::dismiss_thunk, "llm_net_lockstep_sync_overlay_show", false},
    {0x004c8620, {0xE8, 0xF7, 0xF6, 0xFF, 0xFF}, odg::dismiss_thunk, "llm_net_mp_leave_reset_game_mode", true},
    {0x0043f18d, {0xE8, 0x58, 0x8D, 0x08, 0x00}, odg::show_thunk, "llm_strat_time_tick", false},
};
constexpr uintptr_t ADDR_ODG_DISMISS_TT1 = 0x0043f072, ADDR_ODG_DISMISS_TT2 = 0x0043f286,
                    ADDR_ODG_DISMISS_TT3 = 0x0043f2e1, ADDR_ODG_DISMISS_PL1 = 0x004982c3,
                    ADDR_ODG_DISMISS_PL2 = 0x0049833f, ADDR_ODG_DISMISS_SOS = 0x004c7fae,
                    ADDR_ODG_DISMISS_MPL = 0x004c8620, ADDR_ODG_SHOW_TT = 0x0043f18d;

void install_overlay_dialog_guard() {
    int patched = 0, displaced = 0, mismatched = 0;
    g_wait_target = (uintptr_t)&odg::wait_thunk; // the icon thunk's forward (site 0x0043f0fc), see install_overlay_gate
    for (const odg_site &s : ODG_SITES) {
        if (mh::hook::promoted_owner_of(s.site) || (s.ours_skip && MH_LibmhModule_IsBound() && mh::config::ours_run())) {
            ++displaced;
            continue;
        }
        uint8_t       repl[5] = {0xE8, 0, 0, 0, 0};
        const int32_t rel     = (int32_t)((uintptr_t)s.thunk - (s.site + 5));
        memcpy(repl + 1, &rel, sizeof(rel));
        bool ok = false;
        // one literal expression per site: lint_dll_patches keys the manifest on (file, expr)
        switch (s.site) {
            case ADDR_ODG_DISMISS_TT1: ok = patch_bytes_guarded(ADDR_ODG_DISMISS_TT1, s.expect, repl, 5); break;
            case ADDR_ODG_DISMISS_TT2: ok = patch_bytes_guarded(ADDR_ODG_DISMISS_TT2, s.expect, repl, 5); break;
            case ADDR_ODG_DISMISS_TT3: ok = patch_bytes_guarded(ADDR_ODG_DISMISS_TT3, s.expect, repl, 5); break;
            case ADDR_ODG_DISMISS_PL1: ok = patch_bytes_guarded(ADDR_ODG_DISMISS_PL1, s.expect, repl, 5); break;
            case ADDR_ODG_DISMISS_PL2: ok = patch_bytes_guarded(ADDR_ODG_DISMISS_PL2, s.expect, repl, 5); break;
            case ADDR_ODG_DISMISS_SOS: ok = patch_bytes_guarded(ADDR_ODG_DISMISS_SOS, s.expect, repl, 5); break;
            case ADDR_ODG_DISMISS_MPL: ok = patch_bytes_guarded(ADDR_ODG_DISMISS_MPL, s.expect, repl, 5); break;
            case ADDR_ODG_SHOW_TT: ok = patch_bytes_guarded(ADDR_ODG_SHOW_TT, s.expect, repl, 5); break;
        }
        if (ok) {
            ++patched;
        } else {
            ++mismatched;
            refuse_uncarried_fix("overlay_dialog_guard", s.site, s.owner);
        }
    }
    char b[220];
    wsprintfA(b, "; overlay dialog guard (retail carrier): %d site(s) PATCHED, %d DISPLACED (libmh owns them), %d MISMATCHED; wait site via the icon thunk (MP U44)\n",
              patched, displaced, mismatched);
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
    // bespoke treatment. Fixing one site at a time is what let the game-over detour ship dead.
    if (install_trampoline(ADDR_BROADCAST_RESYNC_STATE, (void *)broadcast_resync_state_detour,
                           &g_brs_tramp, 8, mh::hook::entry_claim::exclusive,
                           "the resync-order horizon detour (MP D14)"))
        seam_log("; resync-order horizon armed: CTL_RESYNC_BEGIN exec_time clamped to LOCKSTEP_HORIZON (MP D14)\n");
    //
    // SINCE D17 THE PROMOTED CASE IS NO LONGER A LOSS, so it must not read like one. Our body carries
    // the same clamp (unconditionally since the `resync_order_horizon` knob was retired), so this says
    // CARRIED -- the resync-wait fix's phrasing, for the same situation. THIS BRANCH IS THE ONE THING
    // U30 KEPT here, and it is kept for a reason the generic path cannot cover: the primitive can say
    // that a promotion displaced the detour, but only this site knows the fix survived the
    // displacement. What went is the DIAGNOSIS -- the primitive has already made it, by name, and
    // filed it in the summary; this only adds the good news.
    else if (mh::hook::promoted_owner_of(ADDR_BROADCAST_RESYNC_STATE)) {
        char b[288];
        lstrcpyA(b,
                 "; resync-order horizon DISPLACED: broadcast_resync_state is promoted, and our body "
                 "CARRIES the clamp unconditionally. MP D14 is live here; use [promote] lockstep=0 for the "
                 "original.\n");
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
    // mp:U44: the retail overlay_dialog_guard needs the stall icon's arming seen (the thunk's forward target), so the
    // pass-through thunk is installed for it too even with icon_count=0 (it only counts, which cannot perturb anything).
    if (g_icon_count || g_overlay_dialog_guard) install_overlay_gate(); // the thunk owns the wait-overlay call site
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
    if (mh::config::ours_run()) mh::lockstep::set_extend_floor(ws_extend_floor); // mp:X3c-FIX
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
extern "C" void MH_TeamRel_SetLobbyMode(int team_mode) { g_u52_lobby_team_mode = team_mode; }
extern "C" int  MH_TeamRel_Enabled(void) { return g_team_relations_fix; }

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

// ---- mp:U62: the leave seams' common exit (declared in net_internal.h) -----------------------------------
//
// Called when this peer's player has LEFT a match. A hub with two or more other humans still playing hands the
// transport to the elected successor (the module blocks until every survivor acknowledged, ~1 round trip, or
// HUB_LEAVE_MS); everyone else returns at once. `why` is only for the log.
void mp_hub_leave(const char *why) {
    if (!net_hub_now()) return;
    const bool mid_match = *(const uint8_t *)ADDR_SESSION_MODE == 3 || g_hub_watching;
    const int  others    = other_active_humans();
    char       b[200];
    if (!mid_match || others < 2) {
        wsprintfA(b, "; U62 hub-leave (%s): not handing over -- %s (%d other human(s) alive)\n", why,
                  mid_match ? "fewer than two other humans" : "no match running", others);
        seam_log(b);
        g_hub_watching = false;
        return;
    }
    const DWORD     t0 = GetTickCount();
    const int       r  = MH_Net_HubLeave(HUB_LEAVE_MS);
    MH_NetHubStatus st;
    MH_Net_HubStatus(&st);
    wsprintfA(b, "; U62 hub-leave (%s): %s after %lu ms (epoch %u, %d other human(s) alive, acks %d, re-issues %d)\n", why,
              r == MH_HUB_LEAVE_HANDED ? "HANDED OVER" : r == MH_HUB_LEAVE_TIMEOUT ? "TIMED OUT"
                                                                                   : "NOTHING TO HAND OVER",
              (unsigned long)(GetTickCount() - t0), st.epoch, others, st.acks_rx, st.retargets);
    seam_log(b);
    g_hub_watching = false;
}

// The process is exiting on purpose (WM_DESTROY: window closed / Alt-F4 / the game's own exit). A HUB mid-match
// first leaves the way a quit does -- U19b's park + pinned self-removal, so every survivor drops it at the same
// sim clock -- and then hands the transport over. A client's exit is untouched (the hub fast-drops it, as before).
void mp_leave_for_exit() {
    if (!net_hub_now() || other_active_humans() < 2) return;
    if (g_graceful_leave && *(const uint8_t *)ADDR_SESSION_MODE == 3 && !g_leave_in_progress && !local_is_spectator()) graceful_quit_body(); // mp:U54: a spectator has nothing to park
    mp_hub_leave("exit");
}

// The harness's graceful-exit knob ([harness] exit_process_mode=1) reaches the SAME seam the game's own exit
// takes (mh_harness.dll resolves this by name; tools/gen_harness_contract.py derives the row).
extern "C" void MH_Seam_LeaveForExit(void) { mp_leave_for_exit(); }

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
    // defang_tt_sync, defang_dismiss) are written well before that. Registering
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
    // free to move it, while any explicit lockstep_step_ms is an operator PIN (adaptive off). That is what keeps every existing rig -- mp_run.py and
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
    int    ls_log      = mh_ini_get_int("net", "lockstep_log", SHIP_LOG_LEVEL, g_ini);
    if (step_ms > 0.0) g_lockstep_step = (step_ms + step_eps_ms) / 1000.0;
    // Adaptive lookahead (P1). ON by default, but only when lockstep_step_ms was NOT given: an
    // explicit value is an operator pin (every rig ini sets one, so every existing gate keeps its
    // fixed lookahead).
    g_step_eps_ms = step_eps_ms;
    g_adaptive    = step_explicit ? 0 : SHIP_ADAPTIVE;
    g_ad_seed_ok  = !step_explicit; // mp:P14: only the SHIPPED start is a guess worth replacing
    // mp:P16: 3+-peer star -- size the lookahead for the relayed client->host->client path (default ON;
    // 0 = the pre-P16 host-link-only lookup, the negative arm). Needs a host that publishes in-match.
    g_relay_path = mh_ini_get_int("net", "lockstep_relay_path", 1, g_ini);
    // NOTE the effective floor is max(this, 3 x sim_step) -- see AD_SIM_FLOOR_MULT. Lowering this
    // knob alone will NOT take the lookahead under 3 sub-steps; that guard is what a frozen client
    // cost us.
    g_ls_min = mh_ini_get_int("net", "lockstep_min_ms", 30, g_ini) / 1000.0;
    g_ls_max = mh_ini_get_int("net", "lockstep_max_ms", 400, g_ini) / 1000.0;
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
    g_rx_spin = mh_ini_get_int("net", "rx_spin", SHIP_RX_SPIN, g_ini) != 0;
    // Experimental game-speed pin (percent; 100 = normal, 200 = 2x). Both peers must set the SAME value
    // (deterministic). Pinned every frame via the time_tick hook below (installed if this is set).
    int gs_pct = mh_ini_get_int("net", "game_speed_pct", 0, g_ini);
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
    g_graceful_leave = mh_ini_get_int("net", "graceful_leave", 1, g_ini); // U17 (a) clean-quit self-removal; default ON since U19 (B2 does NOT catch a quit-to-menu -- see g_graceful_leave)
    // mp:U19b: the quitter freezes its horizon and waits for the survivors to park on it before the
    // self-removal (see graceful_leave_park); the CS serialises that freeze against the heartbeat.
    if (!g_hb_cs_ready) {
        InitializeCriticalSection(&g_hb_cs); // DllMain-safe (kernel32 only, no loader re-entry)
        g_hb_cs_ready = true;
    }
    // mp:GS2: game-level peer-data timeout -- drop a peer whose horizon has not moved in this many ms
    // (data-silent, not merely link-silent). <= 0 disables. Default justified beside SHIP_DATA_TIMEOUT_MS.
    g_data_timeout_ms = mh_ini_get_int("net", "data_timeout_ms", SHIP_DATA_TIMEOUT_MS, g_ini);
    // Horizon heartbeat cadence (perf-decouple): >0 arms a thread that advertises the lockstep horizon
    // on real time, independent of render frames. Read here (DllMain, no thread); the thread starts in
    // lazy_start (off loader-lock). A good value is roughly lockstep_step_ms/4 .. /6 (e.g. 50 for 300).
    g_hb_ms = mh_ini_get_int("net", "horizon_heartbeat_ms", SHIP_HEARTBEAT_MS, g_ini);

    // Overlay de-fang (perf-decouple step 2): byte-patch out the in-game SYNCHRONIZING modal so a
    // horizon-wait costs ~1 frame not ~2 s. Byte-patch (VirtualProtect) -> loader-lock safe, like the
    // seam installs. Pairs with the heartbeat to make a tight lookahead actually playable.
    g_defang     = mh_ini_get_int("net", "defang_overlay", 0, g_ini);
    g_icon_count = mh_ini_get_int("net", "icon_count", 1, g_ini); // P4: count de-sync icon shows (default ON, diagnostic-only)
    // REPURPOSED 2026-07-24 (proven in-game): defang_overlay=1 now means the FREEZE FIX ONLY -- NOP the
    // extend_ui_enter mode-8 path (`xui`), the DOMINANT freeze (a routine lockstep-extend order fires
    // ~3x/sec and pins the game clock ~2 s each -> 0.13x sim rate; determinism-clean to NOP). The mode-3
    // overlays stay LIVE by default so the de-sync corner ICON (tt_wait) and the "Player not responding"
    // kick MODAL (tt_sync) still show on genuine stalls (display-only, don't pin the clock, det-safe). Each
    // group is individually overridable; set defang_tt_sync=1 to restore the old suppress-the-modal behavior.
    g_defang_xui          = mh_ini_get_int("net", "defang_xui", g_defang, g_ini);   // freeze fix (=defang_overlay)
    g_resync_trigger_gate = mh_ini_get_int("net", "resync_trigger_gate", 1, g_ini); // spurious-resync ROOT fix (option c); DEFAULT ON -- validated 2026-07-25 (0.98x/0 resyncs, det-clean, drop path preserved). Supersedes defang_xui as the freeze fix.
    g_resync_count_init   = mh_ini_get_int("net", "resync_count_init", 1, g_ini);   // mp:P9 ROOT fix: ACTIVE_PLAYER_COUNT recomputed at match start so the threshold is players*100, not 0; DEFAULT ON (see resync_count_init_tick)
    // MP U20. DEFAULT OFF pending the measurement it exists to be judged by: the icon storm's
    // proximate cause is the adaptive controller saddling its ceiling and probing down (2026-08-29),
    // and suppressing an icon that a correctly-sized lookahead already stops firing would be hiding a
    // signal for nothing. Reimpl-ONLY -- there is no byte patch, so an unpromoted run cannot carry
    // it; the arming line below says so rather than letting a run believe it is gated.
    g_desync_icon_gate = mh_ini_get_int("net", "desync_icon_gate", 0, g_ini);
    // MP U44. DEFAULT ON: the stall overlay no longer closes an open mode-3 dialog (sell/building) on
    // dismiss and no longer lets the kick modal share the screen with it. 0 = the retail rule.
    g_overlay_dialog_guard = mh_ini_get_int("net", "overlay_dialog_guard", 1, g_ini);
    // MP U19e. DEFAULT ON: without it the leader's re-broadcast of a peer drop overwrites the very
    // datagram it is dispatching (one shared _G_LLM_NET_SEND_BUF for TX and RX), and the parse walks
    // into the payload and raises outcome 7 on a clean quit. Carried by the promoted body AND (mp:U19i) a byte patch.
    g_gone_peer_frame_guard = mh_ini_get_int("net", "gone_peer_frame_guard", 1, g_ini);
    // MP U49. DEFAULT ON, SIM-AFFECTING (every peer must carry the same value): an undock order for a unit
    // that is not PARKED is dropped at apply time. Carried by the promoted dispatcher AND a byte patch.
    g_undock_reentry_fix = mh_ini_get_int("net", "undock_reentry_fix", 1, g_ini);
    // MP U45. DEFAULT ON, SIM-AFFECTING (every peer must carry the same value): 0xf4/0xf5 orders never supersede
    // each other in release_due. Carried by the promoted body AND a byte patch.
    g_diplo_order_dedup_fix = mh_ini_get_int("net", "diplo_order_dedup_fix", 1, g_ini);
    // MP U52. DEFAULT ON, SIM-AFFECTING: lobby teams -> relations at match start (+ Team mode victory/lock), and an ally
    // damaging an object does not flip the victim hostile. Both carried by the twins AND by byte patches.
    g_team_relations_fix       = mh_ini_get_int("net", "team_relations_fix", 1, g_ini);
    g_ally_damage_no_hostility = mh_ini_get_int("net", "ally_damage_no_hostility", 1, g_ini);
    // MP U56/U66. DEFAULT ON, SIM-AFFECTING: an eliminated human's flag flip is made by the sim at the elimination step.
    g_player_left_pin_fix = mh_ini_get_int("net", "player_left_pin_fix", 1, g_ini);
    // MP U54. DEFAULT ON, SIM-AFFECTING: a defeated human stays in the match as a spectator. Needs the pinned flip.
    g_spectate_after_defeat = mh_ini_get_int("net", "spectate_after_defeat", 1, g_ini) != 0 && g_player_left_pin_fix != 0;
    g_spectate_mask         = mh_ini_get_int("net", "spectate_mask", 1, g_ini) != 0; // test knob: 0 = the spectator bit is never exported
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
    static const char *const kIniCutWhy =
        "ini-cut wave 2026-10-06 -- the key was only ever read at its default (or never read), so "
        "the default is now unconditional and the knob is gone";
    static const retired_knob RETIRED_KNOBS[] = {
        {"overlay_gate", kIniCutWhy},
        {"resync_trigger_reset", kIniCutWhy},
        {"relay_room", "mp:R1e -- the relay room is minted by the lobby, never configured"},
        {"bulk_selftest_mb", kIniCutWhy},
        {"bulk_selftest_step", kIniCutWhy},
        {"log_gamemode", kIniCutWhy},
        {"graceful_drop", kIniCutWhy},
        {"graceful_leave_park", kIniCutWhy},
        {"graceful_leave_park_ms", kIniCutWhy},
        {"sync_gameover", kIniCutWhy},
        {"resync_receiver_deadline", kIniCutWhy},
        {"horizon_monotone", kIniCutWhy},
        {"lockstep_adaptive", kIniCutWhy},
        {"spectate_view", kIniCutWhy},
        {"focus_log", kIniCutWhy},
        {"failover_budget_ms", kIniCutWhy},
        {"hub_leave_timeout_ms", kIniCutWhy},
        {"udp_redundancy", kIniCutWhy},
        {"resync_order_horizon", kIniCutWhy},
        {"resync_wait_fix", kIniCutWhy},
        {"u3b_park", kIniCutWhy},
        {"u29_slide_dir", kIniCutWhy},
        {"u29_reroot", kIniCutWhy},
        {"u29_teardown", kIniCutWhy},
        {"u29_screen_gate", kIniCutWhy},
        {"dedup_cancel_slide", kIniCutWhy},
        {"dedup_dead_slide", kIniCutWhy},
        {"slide_diag", kIniCutWhy},
        {"map_entry_check", kIniCutWhy},
        {"start_slots", kIniCutWhy},
        {"diplo_echo_nop", kIniCutWhy},
        {"info_guard", kIniCutWhy},
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
    install_resync_wait_fix();
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
        fx.rig_fixed_step_loop = mh_ini_get_int("net", "rig_fixed_step_loop", 0, g_ini) != 0;
        // D17: the same knob the trampoline used, so it means ONE thing whichever implementation is
        // live -- which is the third property reimpl_fixes is documented to have. Note the ini
        // DEFAULT here is 1, unlike its `= false` initialiser in the struct: the initialiser is the
        // faithful-stock value a pure caller or a lockstest fixture gets, while the shipped run has
        // had this fix on since D14, and restoring that is the point of carrying it.
        // U20. Unlike every other field here, this knob has NO byte-patch carrier -- `defang_tt_wait`
        // was its nearest ancestor and C8-e dropped it, leaving wait_overlay_gate_thunk as a pure
        // counter with `g_icon_gate` hardcoded 0. So an UNPROMOTED run with this set has no gate at
        // all, and that has to be said out loud rather than discovered from an unchanged icon rate.
        fx.desync_icon_gate     = g_desync_icon_gate != 0;
        fx.overlay_dialog_guard = g_overlay_dialog_guard != 0; // U44
        // U19e. The promoted body's half of the guard; since mp:U19i an UNPROMOTED dispatch carries the
        // same fix as a byte patch (install_gone_peer_frame_guard), so the knob means one thing in both.
        fx.gone_peer_frame_guard = g_gone_peer_frame_guard != 0;
        // U45. SIM-AFFECTING (order release dedup), so every peer must carry the same value: the shipped
        // default is 1 and no one sets it in a real ini, same contract as sim_step_ms.
        fx.diplo_order_dedup_fix = g_diplo_order_dedup_fix != 0;
        seam_log(fx.diplo_order_dedup_fix
                     ? "; U45: diplo_order_dedup_fix=1 -- release_due keeps 0xf4 and 0xf5 from superseding each other\n"
                     : "; U45: diplo_order_dedup_fix=0 -- retail dedup: a relation + control change in one Apply loses the relation order (the reproduction arm)\n");
        fx.undock_reentry_fix = g_undock_reentry_fix != 0; // U49, see install_undock_reentry_fix
        // U56. SIM-AFFECTING (the elimination flag flip moves from CTL_PLAYER_LEFT's receive time into
        // presence_lost), so every peer must carry the same value: shipped default 1, same contract as U45.
        fx.player_left_pin_fix = mh_ini_get_int("net", "player_left_pin_fix", 1, g_ini) != 0;
        seam_log(fx.player_left_pin_fix
                     ? "; U56: player_left_pin_fix=1 -- an eliminated human's HUMAN/DEFEATED/GONE flip is made by the sim at the elimination step, not at CTL_PLAYER_LEFT receipt\n"
                     : "; U56: player_left_pin_fix=0 -- retail: CTL_PLAYER_LEFT flips the flags at receive time (survivors can disagree on the step; the reproduction arm)\n");
        // U52. SIM-AFFECTING (relations seeded from the lobby teams, the Team-mode ally victory + relation
        // lock), so every peer must carry the same value: shipped default 1, same contract as U45/U56.
        fx.team_relations_fix       = g_team_relations_fix != 0;
        fx.ally_damage_no_hostility = g_ally_damage_no_hostility != 0;
        seam_log(fx.ally_damage_no_hostility
                     ? "; U52: ally_damage_no_hostility=1 -- an ally damaging an object does not flip the victim hostile\n"
                     : "; U52: ally_damage_no_hostility=0 -- retail: any damage can flip the victim hostile toward an ally (the reproduction arm)\n");
        seam_log(fx.team_relations_fix
                     ? "; U52: team_relations_fix=1 -- lobby teams seed the relations at match start; Team mode adds allied victory + the relation lock\n"
                     : "; U52: team_relations_fix=0 -- retail: the lobby TEAM/MODE selectors have no effect on the match\n");
        // U54. SIM-AFFECTING (a defeated human is not dropped), same contract as U45/U56: shipped default 1.
        fx.spectate_after_defeat = g_spectate_after_defeat != 0;
        seam_log(fx.spectate_after_defeat
                     ? "; U54: spectate_after_defeat=1 -- a defeated human with >= 2 other humans alive stays in the match as a spectator\n"
                     : "; U54: spectate_after_defeat=0 -- retail: a defeated human drops out of the lockstep session (the reproduction arm)\n");
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
        if (!fx.overlay_dialog_guard)
            seam_log("; overlay_dialog_guard=0 (MP U44 OFF): retail stall-overlay dismiss -- closes an open "
                     "mode-3 dialog and the kick modal arms over it\n");
        // U19e's INERT line (lockstep promotion off -> no guard) is GONE since mp:U19i: an unpromoted
        // dispatch now gets the byte-patch carrier, and install_gone_peer_frame_guard's own boot line
        // says which of PATCHED / DISPLACED / MISMATCHED this run got.
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
    if (g_gone_peer_frame_guard) install_gone_peer_frame_guard(); // mp:U19i -- same three outcomes
    if (g_diplo_order_dedup_fix) install_diplo_order_dedup_fix(); // mp:U45 -- same three outcomes
    if (g_undock_reentry_fix) install_undock_reentry_fix();       // mp:U49 -- same three outcomes
    if (g_overlay_dialog_guard) install_overlay_dialog_guard();   // mp:U44 -- retail carrier; per-site DISPLACED under promotion
    else seam_log("; undock re-entry fix OFF ([net] undock_reentry_fix=0, MP U49): retail dispatch -- a second undock applied to a unit already walking out wedges it in EXIT_WAIT (the reproduction arm)\n");
    if (g_player_left_pin_fix) install_player_left_pin_fix();           // mp:U66 -- U56's pinned loser-flag flip (retail carriers)
    if (g_spectate_after_defeat) install_spectate();                    // mp:U54 -- spectate after defeat (retail carriers; displaced by promotion)
    if (g_spectate_after_defeat) install_spectate_view();               // mp:U54 -- the spectator's whole-map view (render-only)
    if (g_team_relations_fix) install_team_relations_fix();             // mp:U52 -- seed + lock (retail carriers; displaced by promotion)
    if (g_ally_damage_no_hostility) install_ally_damage_no_hostility(); // mp:U52 -- hostility guard
    install_resync_order_horizon();
    // mp:P9W: spliced UNCONDITIONALLY (like install_overlay_patches above) -- the thunk always runs
    // the original llm_wait_screen_frame first.
    install_resync_receiver_deadline();

    // Hi-res game clock (perf-decouple). The strategic sim clock (time::GetCurrentTime
    // = INT_00e654e4/100, written from GetTickCount in FUN_004cfec4/FUN_004cff80) advances in ~15.6 ms
    // GetTickCount quanta. At a tight lockstep lookahead the TOTAL=committed clamp discards ~half a quantum
    // (~8 ms) of un-simulatable overshoot every 100 ms step -> ~8% sim-rate loss (fps-INDEPENDENT; confirmed by
    // a vsync-off run at 891 fps showing no change). timeBeginPeriod(1) lowers the global system-timer period so
    // GetTickCount updates at ~1 ms -> the quantum (and thus the per-step discard) shrinks ~16x -> the sim
    // approaches 1.0x at step=100, with NO input-latency cost and NO sim change (wall-clock resolution only ->
    // determinism-safe). Gated by mh_net.ini [net] hires_clock. (Process exit restores the period.)
    if (mh_ini_get_int("net", "hires_clock", SHIP_HIRES_CLOCK, g_ini)) {
        timeBeginPeriod(1);
        seam_log("; hires_clock: timeBeginPeriod(1) armed (finer Sleep/scheduler; does NOT affect GetTickCount)\n");
    }
    // Hi-res game clock via QPC (the real timer-quantum fix; see install_qpc_clock). Redirects the game's
    // GetTickCount (IAT slot 0x01070414) to a QPC-derived ms count so TOTAL climbs in ~1 ms steps.
    if (mh_ini_get_int("net", "qpc_clock", SHIP_QPC_CLOCK, g_ini)) install_qpc_clock();

    int sp_log = mh_ini_get_int("net", "sp_clock_log", 0, g_ini); // SP clock cross-check (log every frame)
    if (ls_log || sp_log) {                                       // the path itself is composed per write (SES1: per session) in ls_log_tick
        g_ls_log    = ls_log != 0;
        g_ls_log_sp = sp_log != 0;
    }
    { // graceful_drop (always on since its knob was retired) needs on_time_tick every frame
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
// Then the game-over leave-lockstep detour (always installed).
void lockstep_install_present_gameover() {
    int ft_log    = mh_ini_get_int("net", "frametime_log", SHIP_LOG_LEVEL, g_ini);
    int eager_adv = mh_ini_get_int("net", "eager_advertise", SHIP_EAGER_ADV, g_ini);
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
    const bool go_armed = install_trampoline(
        ADDR_GAMEOVER_DLG, (void *)gameover_detour, &g_go_tramp, 8,
        mh::hook::entry_claim::exclusive,
        "the game-over leave-lockstep fix");
    if (go_armed)
        seam_log("; game-over leave-lockstep armed (own detour): loser -> SESSION "
                 "3->2 at the outcome dialog\n");
    else
        seam_log("; game-over leave-lockstep NOT armed -- see the [interlock] line for "
                 "the reason\n");

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
