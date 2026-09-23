//
// MP seam wiring -- connect mh.exe's four dead transport stubs to the A0 TCP transport
// (net_transport.cpp). See mh_seam_export.h and the lobby RE.
//   Phase A1 (LOBBY):   llm_net_send_packet 0x004bc4f3 / llm_net_poll_recv 0x004bc532
//   Phase A2 (IN-GAME): llm_net_transport_send 0x0049b635 / llm_net_transport_recv 0x0049b65b
// The lobby pair carries slot/map/chat during setup; the in-game pair carries the mode-3 lockstep
// order/horizon stream. The force-entry launch path (launch.cpp) bypasses the lobby and enters the
// game directly, so the A2 pair is what makes a running 2-player game work.
//
// Retail mh.exe carries a complete lobby + lockstep protocol (type-byte dispatch, slot lobby, map
// streaming, CRC framing, per-peer horizon commit) that is dead at the wire -- the send/recv
// primitives do nothing. We full-replace all four so real datagrams flow; the intact state machines
// (llm_lobby_host_net_dispatch, llm_net_lockstep_dispatch) do the rest.
//
// CRC note (why we can ignore it): the lobby's ONLY checksum test is in llm_lobby_host_net_dispatch
// (~line 277): `if (RX_CRC32_COMPUTED != *(u32*)(RX_TYPE+1)) drop;` -- recomputed-vs-embedded SELF-
// consistency, nothing verifies correctness. So on receive we set both fields equal (0) and the
// check passes; on send we skip stamping entirely. TCP already guarantees integrity, and this also
// sidesteps the send/recv CRC-length mismatch (send CRCs over `len`, recv recomputes over a fixed
// 0x3f8) that would otherwise drop every packet. Restore a real CRC only if a lossy/relayed
// transport is added later (Milestone R).
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>
#include <string.h>

#include "include/mh_net_export.h"
#include "state/region_runtime.h" // SB-HOSTFREE: live_base/ptr -- a movable region is read
                                  // where it IS, not where the binary put it
#include "include/mh_seam_export.h"
#include "include/mh_harness_export.h"     // D18: MH_Harness_LateArm -- the harness work that needs to
                                           // know which implementation owns an entry, so it runs here
#include "include/mh_mpmenu_export.h"      // MH_Menu_Install (mp_menu.cpp) -- restore the MP menu button
#include "include/mh_capture_export.h"     // MH_Capture_Install (gfx_capture.cpp) -- UI frame capture
#include "include/mh_overlay_export.h"     // MH_Overlay_Install (gfx_overlay.cpp) -- debug overlay
#include "include/mh_keyrepeat_export.h"   // MH_KeyRepeat_Install (ui_keyrepeat.cpp) -- U24 modal key-repeat fix
#include "include/mh_pause_export.h"       // MH_Pause_Install (ui_pause.cpp) -- D19 pause-screen (mode 5) hotkey
#include "include/mh_fontguard_export.h"   // MH_FontGuard_Install (gfx_font_guard.cpp) -- F2 glyph-table bounds guard
#include "include/mh_chatinput_export.h"   // MH_ChatInput_Install (ui_chat_input.cpp) -- F3 layout-aware typed input
#include "include/mh_cheatgate_export.h"   // MH_CheatGate_Install (ui_cheat_gate.cpp) -- CH1 the SP cheat console refused in a network game
#include "include/mh_diploecho_export.h"   // MH_DiploEcho_Install (ui_diplomacy_echo.cpp) -- U39 the diplomacy dialog's relation echo NOPed
#include "include/mh_canceltask_export.h"  // MH_CancelTask_Install (ui_bldg_cancel_task.cpp) -- D28 the building dialog's cancel-task Yes as a replicated order
#include "include/mh_uidrive_export.h"     // MH_UIDrive_Install (ui_drive.cpp) -- UI automation Phase 2
#include "include/mh_video_export.h"       // MH_Video_Install (video.cpp) -- D13 display-mode selection
#include "include/mh_standalone_export.h"  // MH_Standalone_Install (standalone.cpp) -- boot a stock exe
#include "include/mh_inmem_patch_export.h" // MH_InMemPatch_Install (patch/inmem_install.cpp) -- F1E, default OFF
#include "include/mh_transport_present.h"  // F3F: is there a network transport at all -- NOT `[net] enable`
#include "ui/lobby_ui.h"                   // D4: the UI-owned lobby/browser fixup module (mh/ui)
#include "ui/lobby_ping.h"                 // mp:L1b: per-slot SRTT column, ticked from the lobby's own frame
#include "mh_net_proto/session_info.h"     // F3c: the REFUSED announce kind + its decoder
#include "include/mh_run_context.h"        // MH_RunDir (per-run log folder), MH_ExeDir (config inputs)
#include "include/mh_log_rotate.h"         // SES2: the shared size cap + one-generation rotation
#include "addr/mh_addrs.gen.h"             // generated EN VAs (tools/gen_dll_addrs.py)
#include "addr/mh_patches.gen.h"           // promotable-function extents (the C1 interlock table)
#include "addr/mh_tombstones.gen.h"        // ledger-dead body extents (the X-TOMB dead table)
#include "include/mh_hostapi_bind.h"       // LIB-ABI: the thunk-backed host-callback table (mh.dll's host half)
#include "state/host_api.h"                // LIB-ABI: libmh_set_host_api + the unbound-walk (libmh's half)
#include "state/host_bind.h"               // SB-BIND: the state ABI (region count / bound count)
#include "state/host_events.h"             // LIFT-EVQ: event_sink_dispatch_count (the [hostevt] line)
#include "state/host_in.h"                 // LIB-REF-IN: the inbound surface's arm-time report
#include "include/mh_libmh_hook_bind.h"    // F4D: MH_LibMH_BindHookApi -- the hook table's rc
#include "include/mh_module_bind.h"        // F4D: MH_LibmhModule_IsBound -- which configuration
#include "state/hook_api.h"                // F4D: libmh_hook_api_unbound -- and its unbound walk
#include "hook/host_event_sink.h"          // LIFT-EVQ: bind_host_event_sink + unknown count
#include "addr/mh_rebind.gen.h"            // LIB-REBIND R11: report_arming at the arm-time report
#include "en_guard.h"                      // EN-only build gate
#include "net_internal.h"                  // shared spine: PROLOGUE, TEV_*, init-written globals, net_diag decls
#include "config/ini_read.h"               // TL-HARN4: read_ini_string -- strips a trailing `;comment`
#include "seams/map_transfer.h"            // mp:X2: the map download, its Start gate and its resolve seam
#include "hook/detour.h"                   // install_jmp / install_trampoline (shared toolkit)
#include "hook/hookpoint.h"                // D5/R7: the named hook points -- the C10 session-begin observer
#include "hook/tombstone.h"                // X-TOMB: trap-fill every body we claim dead
#include "hook/export.h"                   // set_export_logger (P0-EXPORT arm reporting)
#include "desync/desync_watch.h"           // D21: runtime desync detector (install / session_reset)
#include "sim/sim_step.h"                  // ROOTS-LIVE: the promoted root D21 must chain onto
#include "hook/patch.h"                    // patch_bytes_guarded (S7 browser-row format string)
#include "hook/watcall.h"                  // call_watcall1 (Watcom __watcall(EAX) bridge)

#pragma comment(lib, "user32.lib") // wsprintfA

// Inline-detour toolkit (hook/): install_jmp = full-body replace (dead-stub substitution),
// install_trampoline = steal-prologue run-before/wrap, call_watcall1 = Watcom __watcall(EAX)
// bridge. File scope so the extern "C" MH_Seam_* inits see them.
using mh::hook::call_watcall1;
using mh::hook::entry_claim; // U30: every install below names its claim and itself
using mh::hook::install_jmp;
using mh::hook::install_trampoline;
using mh::hook::patch_bytes_guarded;

// The mh.exe lobby RX/role addresses (declared in net_internal.h -- shared with net_discovery.cpp;
// MH_Seam_SetAddrs overrides them for tests so MH_Seam_PollRecv writes a local mock RX region).
MH_SeamAddrs g_a = {
    (unsigned char *)mh::addr::lobby_rx_type,
    (int *)mh::addr::lobby_rx_sender_id,
    (unsigned int *)mh::addr::lobby_rx_crc_computed,
    (unsigned int *)mh::addr::lobby_rx_crc_embedded, // rx_type + 1
    (unsigned int *)mh::addr::lobby_rx_len,
    (int *)mh::addr::_G_LLM_NET_LOCAL_PLAYER_INDEX,
    (int *)mh::addr::_G_LLM_NET_IS_HOST,
};

// SES2: the mh_net.log SIZE CAP. `[net] log_max_mb`, default 64 MB, 0 = uncapped; at the cap the
// file is renamed over mh_net.prev.log and the next line opens a fresh one, so disk is bounded at
// 2x the cap however long a session runs (the same one-generation policy mh_temporal.log has had
// since it was capped -- mh_common/include/mh_log_rotate.h now owns both).
//
// WHY mh_net.log AND NOT ONLY mh_temporal.log. This is the stream a bug report is read from and the
// only one FIVE writers in THREE images append to (seam_log here, mh_net.dll's logf, mp_menu.cpp,
// libmh_bind.cpp, module_bind.cpp). It was the last unbounded stream in the tree, and a
// `[net] lockstep_log=1` session writes the bulk of it through THIS function -- net_lockstep.cpp's
// per-step DIAG lines are 43 of the 138 seam_log call sites -- so the cap belongs here rather than
// in any of the event-driven writers.
//
// THE SIZE IS THE FILE'S, NOT A COUNTER OF OUR OWN. With five appenders a per-writer byte count
// would each believe the file is a fraction of its real size; GetFileSizeEx on the handle we have
// just opened costs one call and is right for all of them.
//
// READ LAZILY. seam_log runs from DllMain for the arm banner, before build_paths() has filled g_ini,
// so the ini cannot be consulted on the first lines. Until it can, the compiled default applies and
// nothing is cached -- the first call after the ini path exists settles the cap for the process.
long long        g_log_max_bytes = -1; // -1 = not read yet
static long long seam_log_cap() {
    if (g_log_max_bytes >= 0) return g_log_max_bytes;
    if (g_ini[0] == '\0') return 64LL * 1024 * 1024; // the DllMain window: default, uncached
    g_log_max_bytes = mh_log_cap_bytes(g_ini, "net", "log_max_mb", 64);
    return g_log_max_bytes;
}

// Append one line to mh_net.log (the transport's own log; used for the arm banner, which happens in
// DllMain before the transport's logger is configured). EXTERNAL linkage (declared in net_internal.h)
// -- the sibling seam TUs (net_discovery.cpp) log through this too.
void seam_log(const char *s) {
    // SES1: resolve the directory at OPEN time, every line. mh_net.log is the headline per-SESSION
    // stream -- the one a bug report is read from -- so when a lobby opens the next line goes into
    // the new folder without any writer here knowing a session exists. One integer compare per line
    // (the generation), against a file this function already opens and closes per line anyway.
    seam_paths_tick();
    HANDLE h = CreateFileA(g_log, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    // SES2: rotate at the cap. seam_paths_tick() has already re-pointed g_log at the CURRENT
    // session's directory, so this acts inside the open match's folder -- a rotation never reaches
    // back into an earlier match's, and each new folder starts at zero bytes. A failed rename leaves
    // the handle usable and this line still lands (see mh_log_rotate.h); we simply try again next
    // line rather than dropping the line that might say why the session died.
    if (mh_log_rotate_open_handle(&h, g_log, seam_log_cap())) {
        h = CreateFileA(g_log, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
    }
    SetFilePointer(h, 0, nullptr, FILE_END);
    DWORD wrote = 0;
    // Same local wall-clock stamp the transport's logf() writes, so the "; " seam lines and the
    // "net: " transport lines interleave into ONE readable timeline (2026-08-06). Written as a
    // separate WriteFile rather than composed into a buffer because seam_log takes an
    // already-formatted string of unbounded length -- and both writes are inside the same
    // FILE_APPEND_DATA handle, which is atomic per write at the end of the file.
    char stamp[24];
    int  sn = mh_log_stamp(stamp);
    WriteFile(h, stamp, sn, &wrote, nullptr);
    WriteFile(h, s, lstrlenA(s), &wrote, nullptr);
    CloseHandle(h);
}

namespace {

// mh.exe fixed VAs (generated EN header; image base 0x00400000, no ASLR).
constexpr uintptr_t ADDR_SEND_PACKET    = mh::addr::llm_net_send_packet;    // lobby
constexpr uintptr_t ADDR_POLL_RECV      = mh::addr::llm_net_poll_recv;      // lobby
constexpr uintptr_t ADDR_GAME_SEND      = mh::addr::llm_net_transport_send; // in-game; EAX=buf EDX=len
constexpr uintptr_t ADDR_GAME_RECV      = mh::addr::llm_net_transport_recv; // in-game; EAX=sender* EDX=buf EBX=len*
constexpr uintptr_t ADDR_NET_PLAYERSIDE = mh::addr::PlayerSide;             // client drives its own slot; anti-desync
// PROLOGUE, the TEV_* ids, and the init-written shared globals (g_ini/g_log/g_main_tid/g_qpc_freq/
// g_temporal/g_tev_path/g_trace_n) now live in net_internal.h (shared with net_diag.cpp).

extern "C" int MH_Net_IdAssigned(void); // net_transport: 1 once this client's id is settled (WELCOME arrived) (N2)
// mp_client_slot (N1) + MH_Net_LocalPlayerId -> net_internal.h (net_discovery's poll uses them too).

// The lockstep pacing/perf ADDR set (SIM_STEP_INT/TOTAL_TIME/HORIZON/COMMITTED/PUMP/...) moved to
// net_lockstep.cpp with the cluster; the presence_lost/savegame/GAME_MODE diagnostic loggers (and
// their ADDR set) moved to net_diag.cpp. These stay for the sbm entry logger + PLAYERDUMP.
constexpr uintptr_t ADDR_SESSION_MODE  = mh::addr::_G_LLM_GAME_SESSION_MODE;         // byte; 3 = lockstep
constexpr uintptr_t ADDR_GAME_CLOCK    = mh::addr::_G_LLM_STRAT_GAME_CLOCK;          // double
constexpr uintptr_t ADDR_PLAYER_COUNT  = mh::addr::_G_LLM_NET_LOCKSTEP_PLAYER_COUNT; // int
constexpr uintptr_t ADDR_PLAYERCT_54BC = mh::addr::mode3_trigger_player_count;       // decremented on removal


constexpr int RX_SIZE = 0x3f8; // lobby RX packet region size (0x0065d66e .. gate 0x0065da66)

bool g_armed      = false;
int  g_tried_init = 0;
// g_ini / g_log -> net_internal.h (shared with net_diag.cpp)


// Manual-menu lobby sync (Workstream U Phase 2). Per-lobby-frame detour on the lobby dispatch: mirror our
// TCP peers into the game peer table (host only) so the lobby allocates the client slot + broadcasts the
// snapshot. Without a launch verb the force-entry's per-frame support never runs; this reinstates it.
constexpr uintptr_t    ADDR_LOBBY_DISPATCH = mh::addr::llm_lobby_host_net_dispatch; // per-lobby-frame, both roles
extern "C" void        MH_MP_SyncHostPeerTable(void);                               // launch.cpp -- mirror transport peers -> game peer table
extern "C" void        MH_MP_ArmManualLobby(void);                                  // launch.cpp -- run the proven lobby driver for the manual path
extern "C" void        MH_MP_HostEntryTick(void);                                   // launch.cpp -- host: auto-enter once 2 slots synced (dispatch-driven)
extern "C" int         MH_MP_IsManual(void);                                        // launch.cpp -- 1 = pure manual session (gate all manual host work)
extern "C" void        MH_MP_ClientOnHostLeft(void);                                // net_discovery -- U13 client: withdraw session on host 0x0e
extern "C" int         MH_MP_ConsumeHostLeft(void);                                 // net_discovery -- U13: 1 iff this finalize is a host-LEFT
extern "C" void        MH_MP_MarkExitCauseLinkLost(void);                           // net_discovery -- U23: correct the cause to LINK_LOST
extern "C" int         MH_MP_ConsumeExitCause(void);                                // net_discovery -- U23: 1 host-left, 2 link-lost, 3 join-refused (F3c), 0 Cancel
extern "C" void        MH_MP_ClientOnJoinRefused(const char *reason);               // net_discovery -- F3c client (recv thread): the host refused our JOIN
extern "C" int         MH_MP_TakeJoinRefused(void);                                 // net_discovery -- F3c: 1 once while a refusal is pending
extern "C" int         MH_MP_HasJoined(int player_id);                              // net_discovery -- N2: 1 = this peer's own JOIN was admitted (mp:R6 hello replay)
extern "C" int         MH_MP_JoinLinkPending(void);                                 // net_discovery -- mp:R2b: a clicked JOIN waits for the link into ITS room
extern "C" void        MH_MP_MarkExitCauseJoinRefused(void);                        // net_discovery -- F3c: the exit cause the refusal bounce sets
extern "C" const char *MH_MP_JoinRefusedReason(void);                               // net_discovery -- F3c: the host's reason text
extern "C" void        MH_MP_HostResetSessionIdentity(void);                        // net_discovery -- U13 host: fresh tag + join gate on leave
extern "C" void        MH_MP_ResetHostMirror(void);                                 // launch.cpp -- U13 host: reset peer-mirror edge on leave
void                  *g_lobby_tramp = nullptr;
void                   host_send_map(void); // fwd (defined near the recv seam, where ADDR_CUR_MAP is in scope)
bool                   g_map_recv = false;  // client: host's selected map has arrived (entry gate)


int g_hold_start = 0; // [net] hold_start=1: manual host does NOT auto-enter at 2 slots (dev
                      // gate for S3 browse-testing; superseded by S4 join-gating + U2 Start)


// The lockstep pacing/perf cluster (time_tick/present detours, overlay de-fang, qpc/hires clock,
// horizon heartbeat, game-over leave-lockstep, the mh_lockstep.log writer) -> net_lockstep.cpp;
// g_ls_log + ms_of -> net_internal.h.
// mp_host_advertise_session + g_host_join_seen -> net_internal.h (net_discovery.cpp owns them).
// g_trace_n + MH_Seam_TraceDump + install_trace_hooks + temporal_capture/flush -> net_internal.h.
extern "C" int MH_MP_MapReceived(void); // client: host's real map has arrived (defined near the recv seam) -- U8 Gap 2

// Per-lobby-frame: reaffirm PlayerSide (host 0 / client 1 -- build_players reads it at game entry) and,
// on the host, mirror the transport into the game peer table so the client slot allocates + the snapshot
// broadcasts. Runs for BOTH roles (the dispatch is the shared lobby callback); the mirror is host-gated.
// U10 audit (E1): the PlayerSide reaffirm is KEPT -- retail has no lobby-phase writer that sets it per role
// for our transport (net_udp, which would have, is dead), and build_players_from_slots reads it at entry, so
// it must be held correct every frame; cheap idempotent write, not fighting anything. See the MP seam audit.
// ---- U16: show the "<name> joined/left" line on EVERY lobby peer, not just the host --------------
// The host broadcasts a FLAG_ANNOUNCE {is_join:1, affected player id, name[..]} on each JOIN/LEFT
// delta (launch.cpp mp_sync). THE NET HALF IS THE HANDLER; the queue and the render are the UI
// module's (mh/ui/lobby_announce.cpp), because rendering an announce is a call into retail's own
// lobby-log path and has nothing to do with the wire. This function's whole job is the one question
// only the net side can answer: is this announce about US?
// Payload: [0]=is_join, [1]=affected player id, [2..]=name (NUL-terminated ANSI).
void on_announce_recv(int /*sender*/, const unsigned char *buf, int len) {
    if (!buf || len < 3) return;
    // mp:F3c: the third kind is ADDRESSED, not rendered. "Your JOIN was refused: <reason>" is for
    // the peer whose id is byte [1] and for nobody else -- the others log it (the host never seated
    // that peer, so there is no row to announce about) and do nothing.
    if (buf[0] == mh_net_proto::ANNOUNCE_REFUSED) {
        uint8_t target = 0;
        char    reason[mh_net_proto::ANNOUNCE_TEXT_CAP];
        if (!mh_net_proto::announce_refused_decode(buf, (size_t)len, &target, reason, sizeof(reason))) return;
        if ((int)target == MH_Net_LocalPlayerId()) {
            MH_MP_ClientOnJoinRefused(reason);
        } else {
            char b[160];
            wsprintfA(b, "; F3c: host refused player %u's JOIN: %s\n", (unsigned)target, reason);
            seam_log(b);
        }
        return;
    }
    // mp:L1f: the fourth kind is a TABLE, not a line of text -- the host's per-slot ping summary,
    // which is the only way a client ever learns another client's ping (the transport is a
    // client-server star; see session_info.h's ANNOUNCE_PING block). Byte [1] is an entry COUNT
    // here rather than a player id, which is safe precisely because the kind is dispatched first.
    if (buf[0] == mh_net_proto::ANNOUNCE_PING) {
        mh::ui::lobby_ping_on_published(buf, len);
        return;
    }
    // ...AND THE REFUSAL, which is the half mp:F3c left implicit. Everything past this point treats
    // buf+2 as a NUL-terminated NAME and paints it in the lobby log, so a kind this build does not
    // know would be rendered as garbage text once per frame it arrived -- exactly what a version
    // gate is supposed to prevent. An unknown kind is therefore DROPPED, loudly enough to debug
    // (once is not worth a rate limiter: a peer new enough to send one cannot join this lobby at
    // all -- JOIN_REQUEST_MIN_FORMAT refuses it -- so this is a "cannot happen" that says so).
    if (buf[0] > mh_net_proto::ANNOUNCE_PING) {
        char b[96];
        wsprintfA(b, "; announce: REFUSED unknown kind %u (%d bytes) -- newer peer?\n",
                  (unsigned)buf[0], len);
        seam_log(b);
        return;
    }
    // Skip an announce about OURSELVES: the joiner already gets its own "X joined" from the retail path,
    // so also rendering our broadcast would double the joiner's own line. Other peers keep it. (U16)
    if ((int)buf[1] == MH_Net_LocalPlayerId()) return;
    mh::ui::announce_post((const char *)(buf + 2), buf[0]); // recv thread -> the UI ring
}

// ---- mp:L1f -- THE HOST PUBLISHES ITS PER-SLOT PING MEASUREMENTS -------------------------------
//
// WHY THE HOST AND NOBODY ELSE. The restored transport is a client-server STAR (the user's
// 2026-07-09 decision): every client holds exactly ONE connection, to the host, so its
// MH_NetStats::lat[] can only ever hold one row and no client can measure another client. Only the
// host holds them all. Without this, every occupied row but two is permanently blank on a client's
// lobby screen -- not a renderer bug, a topology fact.
//
// WHY IT CANNOT TOUCH DETERMINISM, stated where it is written rather than only in a doc:
//   * it rides FLAG_ANNOUNCE, which both transports route to the client's announce handler and
//     NEVER to the game queue (mh_net/net_transport.cpp says so at MH_Net_SetAnnounceHandler:
//     "clients' g_announce_cb, never the game queue -- determinism-safe, exactly like SESSION_INFO");
//   * the receiver's only consumer is mh/ui/lobby_ping.cpp's display table, which nothing else in
//     the DLL reads -- no order is issued from it, no gate consults it, no byte of it is hashed;
//   * it is sent from the LOBBY tick only, so it does not exist during a match at all: there is no
//     lockstep clock running when this writes, and nothing it can race.
// The two oracles that would catch a mistake here anyway: the D21 in-band desync watch (the per-
// sample MATCH lines every multi-peer row carries) and the determinism gate's lockstep-hash compare.
//
// CADENCE: ~1 s. A lobby ping cell that updates once a second is already smoother than a human
// reads it, and the frame is ~34 bytes.
namespace {
DWORD           g_ping_pub_next = 0;
constexpr DWORD PING_PUB_MS     = 1000;

void mp_lobby_ping_publish() {
    const DWORD now = GetTickCount();
    if ((long)(now - g_ping_pub_next) < 0) return;
    g_ping_pub_next = now + PING_PUB_MS;

    MH_NetStats st;
    MH_Net_GetStats(&st);
    if (!st.lat_supported) return; // the TCP module measures nothing; there is nothing to publish

    mh_net_proto::AnnouncePingEntry e[mh_net_proto::ANNOUNCE_PING_MAX_ENTRIES];
    size_t                          n  = 0;
    const int                       me = MH_Net_LocalPlayerId();
    for (int i = 0; i < st.lat_count && i < MH_NET_MAX_PEERS &&
                    n < mh_net_proto::ANNOUNCE_PING_MAX_ENTRIES;
         ++i) {
        const MH_NetPeerLatency &L = st.lat[i];
        if (L.samples <= 0) continue;                     // nothing measured on this link yet
        if (L.player_id < 0 || L.player_id > 7) continue; // no lobby row a client could match it to
        if (L.player_id == me) continue;                  // our own row is never on the wire
        int ms = (L.srtt_us + 500) / 1000;
        if (ms < 0) ms = 0;
        e[n].player_id = (uint8_t)L.player_id;
        e[n].srtt_ms   = (uint16_t)(ms > (int)mh_net_proto::ANNOUNCE_PING_SRTT_CLAMP
                                        ? mh_net_proto::ANNOUNCE_PING_SRTT_CLAMP
                                        : ms);
        e[n].relay     = L.relayed == 1   ? mh_net_proto::ANNOUNCE_PING_RELAY_RELAYED
                         : L.relayed == 0 ? mh_net_proto::ANNOUNCE_PING_RELAY_DIRECT
                                          : mh_net_proto::ANNOUNCE_PING_RELAY_UNKNOWN;
        ++n;
    }
    // AN EMPTY TABLE IS STILL SENT, and that is deliberate: it is how a client learns that the peer
    // whose number it was showing is gone (lobby_ping.cpp REPLACES its table on every summary). It
    // is also what keeps the receiver's staleness clock ticking while a lobby genuinely has nobody
    // else in it, instead of letting an idle host look like a dead one.
    uint8_t   ab[mh_net_proto::ANNOUNCE_PING_MAX_ENCODED];
    const int len = (int)mh_net_proto::announce_ping_encode(e, n, ab);
    MH_Net_SendAnnounce(ab, len);
    char b[96];
    wsprintfA(b, "; [lobbypub] entries=%d bytes=%d\n", (int)n, len);
    seam_log(b);
}
} // namespace

// The slide take-over's gate, and the reason it is HERE: `mh::ui` may not read the transport, so the
// one transport question its detour needs -- "am I a client with a live session?" -- is answered by
// the net side and handed over as a function pointer at arm time. Exactly the pair of tests the
// take-over used to make inline (the lobby-slide notes, "The gate").
int ui_client_session_gate(void) {
    if (!g_a.is_host || *g_a.is_host) return 0; // client only -- the host's slide terminates anyway
    return MH_Net_IsStarted() ? 1 : 0;          // ...and a session has to exist
}

// ---- The menu slide take-over (U3b / U22 / U29 / U37 / U38) -> mh/ui/lobby_slide.cpp -----------
// 400 lines of widget geometry, direction handling and the slide_diag instruments moved to the UI
// module at fork F3E, and the mechanism was RE-DERIVED rather than carried: the lobby-slide notes.
// The only thing that stayed on this side is ui_client_session_gate above -- the take-over's one
// transport question, answered here and injected there.

void on_lobby_dispatch() {
    mh::ui::announce_drain(); // U16: render any host-broadcast join/left lines (main thread)
    int is_host = *g_a.is_host;
    // ---- mp:X2: the map download, driven from the one per-lobby-frame tick both roles run -------
    // HOST: refresh the content claim if the picker moved, arm the next peer's transfer, and open or
    // close the Start gate. CLIENT: drive the receive and, when a download lands, report it back.
    // Both sides repaint the lobby's status line, which is where the refusal names the peer.
    mh::seams::maps::lobby_tick(is_host);
    if (!is_host && mh::seams::maps::client_take_rejoin()) mp_join_resend_map_report();
    // mp:L1b: refresh the lobby slot-row panel's per-slot ping cells from THIS tick, both roles --
    // see ui/lobby_ping.cpp for why this has to be lobby-tick-driven rather than present-hook-driven.
    mh::ui::lobby_ping_tick();
    // mp:L1f: ...and on the HOST, publish what only the host can measure, so every client can paint
    // the rows its single star-topology connection makes it blind to. Lobby-only by construction
    // (this is the lobby dispatch); see mp_lobby_ping_publish for why it cannot touch determinism.
    if (is_host) mp_lobby_ping_publish();
    // mp:GS1(b): retail PlayerSide wants our ARRAY INDEX, not our wire id -- mp_client_slot() (the old
    // value here) is the wire id and is wrong the instant a reused/compacted slot puts them at
    // different numbers (see mp_lobby_array_index's banner in net_internal.h). This write runs right
    // before falling back into retail's own lobby dispatch, which is what actually consumes PlayerSide
    // at the Start trigger (build_players_from_slots_finish's diagonal stamp, then session_begin_multi).
    *(int *)ADDR_NET_PLAYERSIDE = mp_lobby_array_index(is_host != 0); // N1: own array slot (was hardcoded 1)
    // ---- N2 item 1: pin the client's own player-id + lobby slot index every frame (N>2 corruption root) -
    // At N>2 the interactive lobby corrupted a human slot to player_id=0/host-name + spawned a duplicate.
    // Two stale-state roots, both client-side: (a) _G_LLM_NET_LOCAL_PLAYER_INDEX (0x5d55ac) is written ONCE
    // at discovery (= mp_client_slot() = 1, BEFORE the host WELCOMEs an assigned id), and nothing re-affirms
    // it -- so a 3rd+ joiner (assigned id 2+) keeps LOCALIDX=1; (b) llm_lobby_build_slot_widgets (0x4bf99b)
    // FORCES _G_LLM_LOBBY_LOCAL_SLOT_INDEX=0 on every REBUILD (else-branch @0x4bf9f8 when WIDGETS_BUILT!=0).
    // With LOCAL_SLOT_INDEX=0 (the host/alice row), the spinner callbacks' push_local_slot_state (0x0c) send
    // SLOTS[0] (host record, player_id 0) to the host, which memcpys it into the SENDER's slot -> pid-0
    // collision, then slot_find_or_alloc misses the real id and allocs a NEW row -> the duplicate. Fix, manual
    // client only (the automated determinism path sets these itself in launch.cpp -- leave it untouched, so the
    // mp_run --lobby regression stays bit-identical), every frame, OVERRIDING the retail force-0: re-affirm
    // LOCALIDX to the live transport-assigned id (mirrors the PLAYERSIDE re-affirm above + keeps the retail
    // build_slot_widgets scan target + the U8 peer-id registration below correct), then scan LOBBY_SLOTS for
    // OUR row (player_id == assigned id) and pin LOCAL_SLOT_INDEX to it. Leave it alone on a miss (transient
    // pre-sync window) rather than forcing a wrong index. (N2 item 1)
    if (!is_host && MH_MP_IsManual() && MH_Net_IdAssigned()) {
        constexpr uintptr_t A_LOCALIDX_N2    = mh::addr::_G_LLM_NET_LOCAL_PLAYER_INDEX;
        constexpr uintptr_t A_LOBBY_SLOTS_N2 = mh::addr::_G_LLM_LOBBY_SLOTS; // stride 0x39, player_id@+0x01
        constexpr uintptr_t A_LOCAL_SLOTIDX  = mh::addr::_G_LLM_LOBBY_LOCAL_SLOT_INDEX;
        const int           assigned         = mp_client_slot(); // live host-assigned id (>=1 once WELCOMEd)
        *(int *)A_LOCALIDX_N2                = assigned;         // keep LOCALIDX live (was set once at discovery)
        // Scan all 8 physical slots (not just map_pcount -- that is the STUB count until the map arrives, so
        // a 3rd+ player's row at index 2 would be missed early). Find OUR row by assigned id.
        for (int i = 0; i < 8; ++i) {
            if (*(const int *)(A_LOBBY_SLOTS_N2 + (uintptr_t)i * 0x39 + 0x01) == assigned) {
                *(int *)A_LOCAL_SLOTIDX = i; // override the retail force-0 rebuild
                break;
            }
        }
    }
    // ---- U3 + U7: the client's lobby frame/right panel settle on-screen (mh/ui) ----------------
    // Geometry, so the body is the UI module's (lobby_widgets.cpp); the ROLE is ours. The screen
    // test and the is-it-parked test live with the geometry, so this is one call per lobby frame on
    // the client and nothing at all on the host.
    if (!is_host) mh::ui::lobby_frame_snap_on_screen();
    // ---- U8 fix: register the client's local player in the retail net peer-id table -----------------
    // The retail lobby dispatch (llm_lobby_host_net_dispatch) gates its ENTIRE per-slot render loop
    // behind FUN_0049e00a(_G_LLM_NET_LOCAL_PLAYER_INDEX) >= 0 (line 86). That helper scans the peer-id
    // table _G_LLM_NET_HOST_PLAYER_ID (0x5d336c, 8 entries, stride 0x400, player_id@+0) for the local
    // id and returns -1 if absent -> the dispatch shows a (silent) error dialog and RETURNS before the
    // render loop. Our DLL synth never populates that table (zero .bss), so the HOST passes only by
    // accident -- it searches for id 0, which matches the zero entry[0] -- while the CLIENT searches for
    // 1, finds nothing, and bails EVERY frame. Net effect: the client's slot render loop never runs, so
    // occupied rows never get a name / team-color / "Люди" and all show "Открыто". Pinned live 2026-07-13
    // by bp-counting on a held client lobby (guard86-bail 64 hits / 40 frames, loop-entry 0). Fix: put
    // the local player's id at its own playerside index so the guard passes. entry[0] stays the host id
    // (0) -- it's the client->host send target (llm_lobby_push_local_slot_state) -- so don't touch it.
    // Idempotent, client-only. (U8)
    if (!is_host) {
        constexpr uintptr_t A_PEERIDTAB = mh::addr::_G_LLM_NET_HOST_PLAYER_ID;     // stride 0x400, id@+0
        constexpr uintptr_t A_LOCALIDX  = mh::addr::_G_LLM_NET_LOCAL_PLAYER_INDEX; // =1 on the client
        const int           localidx    = *(const int *)A_LOCALIDX;
        if (localidx > 0 && localidx < 8) {
            int *slot = (int *)(A_PEERIDTAB + (uintptr_t)localidx * 0x400);
            if (*slot != localidx) {
                *slot = localidx;
                if (g_ls_log) seam_log("; U8FIX registered client local player in net peer-id table\n");
            }
        }
    }
    // ---- U8 Gap 2: the client adopted the host's map OUT OF BAND -> refresh what shows it -------
    // The gate is ours (MH_MP_MapReceived is set with release AFTER the recv thread's copy
    // completes, so we never hand the UI a torn current_map_data); re-running the two retail
    // refreshes is the UI module's (lobby_widgets.cpp), which also owns the per-map latch.
    if (!is_host && MH_MP_MapReceived()) mh::ui::lobby_refresh_for_map();
    // Manual host work ONLY (peer mirror / map broadcast / auto-entry). A force-entry run drives all of
    // this from its own verb path (mp_lobby_entry_tick) -- running it here too broke its determinism.
    // S4 join-gating: ADVERTISE unconditionally (so browsers see us + get our SESSION_INFO), but slot +
    // flood + auto-enter a peer ONLY after it has sent an explicit JOIN naming our lobby-id. A browse-only
    // connect (S3) therefore gets discovery but no slot/flood -> no more browser-side crash / "joined" spam.
    if (is_host && MH_MP_IsManual()) {
        mp_host_advertise_session();
        if (g_host_join_seen) {
            MH_MP_SyncHostPeerTable();
            host_send_map();
            MH_MP_HostEntryTick();
        } else {
            // U14 spam fix (2026-07-23): with no admitted peer, mp_sync is gated out and can't zero the
            // session-list count. When the LAST peer departs, mp_sync writes its LEFT delta (count=1) and, the
            // same frame, MH_MP_ResetJoinGate() closes g_host_join_seen -> mp_sync stops running while count
            // stays 1, so the retail admin loop (llm_lobby_host_net_dispatch) re-processes that ONE stale delta
            // every frame == "<name> left the game" spam. Zero it here so the loop no-ops. (This is the same
            // persistent-count class as U6, but triggered at the final departure.)
            volatile int *pc = (volatile int *)mh::addr::peer_table_admin_count;
            if (*pc != 0) {
                *pc = 0;
                if (g_ls_log) seam_log("; U14: no admitted peer -> cleared stale session-list count (spam guard)\n");
            }
        }
    }
    if (g_ls_log) { // DIAG: lobby dispatch + U3 render-state (both roles)
        static int  c         = 0;
        static char last[224] = {0}; // mp:SES5 decision (3): dedupe -- U3DIAG measured at 8 Hz with
                                     // runs of identical lines; log only when the text actually changes.
        if ((c++ % 120) == 0) {
            // The three menu_frame draw gates (0x004b79a8): draws the lobby ONLY if GAME_MODE==3 &&
            // (DLG_FLAGS0 & 0x10); if SAVED_STATE==1 it draws widget_list+0x14 (the PARENT) not the lobby.
            void    *wl  = *(void **)mh::addr::_G_LLM_UI_MENU_WIDGET_LIST;
            unsigned ms  = *(const unsigned char *)mh::addr::_G_LLM_UI_MENU_STATE;
            unsigned sv  = *(const unsigned char *)mh::addr::_G_LLM_UI_MENU_SAVED_STATE; // 1 => draw parent
            unsigned gm  = *(const unsigned char *)mh::addr::_G_LLM_GAME_MODE;           // must be 3 to draw menu
            unsigned dlg = *(const unsigned char *)mh::addr::_G_LLM_DLG_STATE_FLAGS;     // .flags0 (bit4 0x10 = DRAW gate)
            char     b[224];
            wsprintfA(b, "; U3DIAG is_host=%d gm=%u menu_state=%u saved=%u dlg=%02X (draw=%d) widget_list=%08X (lobby=%d)\n",
                      is_host, gm, ms, sv, dlg, (dlg & 0x10) != 0 && gm == 3, (unsigned)wl, wl == (void *)mh::addr::lobby_widget_origin);
            if (lstrcmpA(b, last) != 0) {
                seam_log(b);
                lstrcpynA(last, b, sizeof(last));
            }
        }
    }
    if (g_trace_n > 0) { // periodic trace-count dump (both roles run this dispatch)
        static int tc = 0;
        if ((tc++ % 600) == 0) MH_Seam_TraceDump(is_host ? "host-lobby" : "client-lobby");
    }
}
__declspec(naked) void lobby_dispatch_detour() {
    __asm {
        pushad
        pushfd
        call on_lobby_dispatch
        popfd
        popad
        jmp  dword ptr [g_lobby_tramp] // stolen 8-byte prologue + jmp back to dispatch+8
    }
}


// Manual-join initial-state DESYNC diagnostic (next-session (a), 2026-07-12). build_players_from_slots
// runs immediately before session_begin_multi, so at this entry Players[] holds exactly what it produced.
// Dump the INPUTS (PlayerSide, map player count, the raw lobby slots incl. status+relation) and the
// OUTPUTS (each Players[] desc: controller_flags, relation[], credits, side_id) on BOTH peers, so a
// single 2-machine run can be diffed host-vs-client to localize the desync WITHOUT guessing (the notes'
// prescribed method). controller_flags 7=active vs 0xb=neutral pivots on slot_status<2 vs ==2 -- a
// slot_status[1] mismatch would flip player 1 human<->neutral => the observed p1_ai_econ/strat_players
// divergence. Read-only; gated on lockstep_log so it rides the standard mp_run --lobby diagnostic ini.
void dump_players_state(const char *tag) {
    constexpr uintptr_t ADDR_LOBBY_SLOTS = mh::addr::_G_LLM_LOBBY_SLOTS;
    constexpr int       SLOT_STRIDE      = 0x39;
    constexpr uintptr_t ADDR_PLAYERS     = mh::addr::Players; // game::g::Players[8] (llm_strat_player_desc, 0x34)
    constexpr uintptr_t A_LOCALIDX       = mh::addr::_G_LLM_NET_LOCAL_PLAYER_INDEX;
    constexpr uintptr_t A_CURMAP         = mh::addr::current_map_data; // map::g::current_map_data
    const int           is_host          = (g_a.is_host && *g_a.is_host) ? 1 : 0;
    const int           pside            = *(const int *)ADDR_NET_PLAYERSIDE;
    const int           localidx         = *(const int *)A_LOCALIDX;
    const int           mappc            = *(const int *)(A_CURMAP + 0x08);           // current_map_data.field2_0x8 (build_players bound)
    const unsigned char seed             = *(const unsigned char *)(A_CURMAP + 0x14); // ch0 RNG seed byte (session_begin_multi)
    char                b[256];
    wsprintfA(b, "; PLAYERDUMP[%s] host=%d PlayerSide=%d localidx=%d map_pcount=%d rng_seed=0x%02x p54bc=%d\n",
              tag, is_host, pside, localidx, mappc, seed, *(const int *)ADDR_PLAYERCT_54BC);
    seam_log(b);
    for (int i = 0; i < 3; ++i) {
        const unsigned char *s = (const unsigned char *)(ADDR_LOBBY_SLOTS + i * SLOT_STRIDE);
        wsprintfA(b, ";   slot[%d] status=%d pid=%d race=%d color=%d rel=%02x %02x %02x %02x %02x %02x %02x %02x\n",
                  i, s[0x0b], *(const int *)(s + 0x01), s[0x05], s[0x06],
                  s[0x0d], s[0x0e], s[0x0f], s[0x10], s[0x11], s[0x12], s[0x13], s[0x14]);
        seam_log(b);
    }
    for (int i = 0; i < 3; ++i) {
        const unsigned char *p = (const unsigned char *)(ADDR_PLAYERS + i * 0x34);
        wsprintfA(b, ";   P[%d] ctrl=0x%02x race=%d color=%d sprite=%u side=%d rel=%02x %02x %02x %02x %02x %02x %02x %02x\n",
                  i, p[0x06], p[0x00], p[0x01], *(const unsigned *)(p + 0x02), *(const int *)(p + 0x30),
                  p[0x08], p[0x09], p[0x0a], p[0x0b], p[0x0c], p[0x0d], p[0x0e], p[0x0f]);
        seam_log(b);
        // D23: THE WHOLE 0x34 STRIDE, RAW. The decoded line above prints six of the eight fields, and
        // the two it omits are exactly the two the D23 measurement could only trace STRUCTURALLY --
        // the reserved byte at +0x07 and name[32] at +0x10. Hashing a byte whose peer-invariance was
        // argued from the code and never seen equal in a capture is the D11/rng_state trap, so this
        // line is the instrument that closes it: 52 bytes per player, host-vs-peer diffable by eye.
        // Kept SEPARATE from the decoded line rather than replacing it -- the decoded one is what
        // every existing note, and D18's own evidence, is written against.
        char  hx[176];
        char *w = hx;
        for (int k = 0; k < 0x34; ++k) {
            static const char HEX[] = "0123456789abcdef";
            *w++                    = HEX[p[k] >> 4];
            *w++                    = HEX[p[k] & 0xf];
            *w++                    = (k == 0x33) ? '\0' : ' ';
        }
        wsprintfA(b, ";   P[%d] raw %s\n", i, hx);
        seam_log(b);
    }
}

// Phase 2c thread-1: session_begin_multi entry logger. It writes SESSION_MODE = (DAT_005d54bc<2 ? 2 : 3).
// If it runs a SECOND time on the client with the peer count momentarily <2, it downgrades 3->2. Log
// every entry with the count + current mode to catch a re-entry. Gated by lockstep_log.
constexpr uintptr_t ADDR_SESSION_BEGIN_MULTI = mh::addr::llm_strat_session_begin_multi;
void               *g_sbm_tramp              = nullptr;
void                on_session_begin_multi() {
    // Manual-menu MP fix: the HOST's net player count DAT_005d54bc gets reset 2->0 in the async gap
    // between begin_map_load and this call, so session_begin_multi would build a mode-2 (skirmish, solo)
    // game instead of mode-3 lockstep. Force it to the real connected count (peers + self) here, right
    // before the original runs -- this is the value session_begin_multi keys the lockstep mode off (the
    // client already reaches this with p54bc=2). Host-only + transport-up, so SP and the client are untouched.
    if (MH_MP_IsManual() && g_a.is_host && *g_a.is_host) {
        int n = MH_Net_PeerCount() + 1;
        if (n >= 2) *(int *)ADDR_PLAYERCT_54BC = n;
    }
    // D21: the desync sampler's step key is only meaningful relative to a session both peers entered
    // together, so it is zeroed HERE -- ahead of the g_ls_log gate, which is a logging switch and must
    // not decide whether the detector runs.
    mh::desync::session_reset();
    if (!g_ls_log) return;
    char b[160];
    wsprintfA(b, "; session_begin_multi ENTER p54bc=%d pcount=%d sess(before)=%d gclk=%ld\n",
                             *(const int *)ADDR_PLAYERCT_54BC, *(const int *)ADDR_PLAYER_COUNT,
                             (int)*(const uint8_t *)ADDR_SESSION_MODE, ms_of(ADDR_GAME_CLOCK));
    seam_log(b);
    dump_players_state("sbm"); // initial-state desync diag: Players[] as build_players_from_slots left it
}
__declspec(naked) void session_begin_multi_detour() {
    __asm {
        pushad
        pushfd
        call on_session_begin_multi
        popfd
        popad
        jmp  dword ptr [g_sbm_tramp]
    }
}

// ---- D21: the SHIP-path per-step hook for the runtime desync detector ---------------------------
// The detector needs a hook at llm_strat_sim_step's pre-body boundary. In a run with the harness armed
// the determinism harness already owns that entry and feeds the detector from its own hash, so this
// install is REFUSED (the entry no longer opens with the Watcom prologue) and that is the correct
// outcome -- exactly one of the two hooks is ever live. In a shipped game, which is what D21 is
// for, no harness arms and this is the only sampling point that exists: the turn engine's own
// `calls.sim_step` edge is dead there, because llm_strat_sim_tick is not in the default
// `[promote] lockstep` closure. Same 8-byte prologue steal + naked shape as the harness's own
// detour; the CMP-free tail means the flags POPFD restored are the ones the prologue sees.
constexpr uintptr_t    ADDR_DESYNC_SIM_STEP = mh::addr::llm_strat_sim_step;
void                  *g_desync_tramp       = nullptr;
void                   on_desync_sim_step() { mh::desync::on_sim_step(); }
__declspec(naked) void desync_sim_step_detour() {
    __asm {
        pushad
        pushfd
        call on_desync_sim_step
        popfd
        popad
        jmp  dword ptr [g_desync_tramp]
    }
}

// Arm the detector and give it whichever hook this run leaves available. Reports BOTH outcomes by
// name: "armed" and "armed but never sampling" are the pair this repo keeps having to tell apart.
void install_desync_watch() {
    mh::desync::set_logger(seam_log);
    if (!mh::desync::install(g_ini)) return; // it has already said why
    if (install_trampoline(ADDR_DESYNC_SIM_STEP, (void *)desync_sim_step_detour, &g_desync_tramp, 8,
                           mh::hook::entry_claim::exclusive,
                           "the D21 desync sampler's sim_step hook")) {
        seam_log("; [desync] sim_step hook INSTALLED -- no determinism harness this run, so this is "
                 "the sampling point\n");
        return;
    }
    // REFUSED -- and there are now TWO reasons, which is why this is no longer one else-branch.
    // ROOTS-LIVE (2026-09-04) made llm_strat_sim_step promote by DIRECT ENTRY INSTALL in any run the
    // harness did not arm, so in the SHIP configuration the entry is OURS, not free and not the
    // harness's. Until this branch existed the detector printed the harness line, sampled nothing for
    // the whole run, and looked armed -- a default-ON instrument silently switched off, with the log
    // naming a cause that was not present. That is G68/U30's shape and it is measured, not theoretical.
    //
    // The fix is a route, not a fallback: chain the sampler onto our promoted body, which is the same
    // "the instrument still runs first" contract the harness rebind has always given it.
    //
    // F3D: the registration goes through the NAMED POINT, not through mh::sim. F3C converted the
    // other three R7 installs and deliberately left this one raw, because it was a live SELFTEST
    // ANCHOR of check_net_lockstep_refs (the tool refuses rather than passing when it cannot see a
    // known coupling, so deleting an anchor and the reference it watches in different commits would
    // have gone RED for the wrong reason). F3D re-picks the anchors in this same commit, so the
    // conversion lands here. `point::sim_step_pre` forwards into mh::sim::set_sim_step_pre_hook
    // inside mh.dll and returns exactly what it returned -- including FALSE when the single-subscriber
    // slot is already taken, which is the branch the second log line below reports.
    if (mh::sim::sim_step_promoted()) {
        if (mh::hook::register_callback(mh::hook::point::sim_step_pre, &on_desync_sim_step))
            seam_log("; [desync] sim_step hook CHAINED onto the PROMOTED root -- llm_strat_sim_step's "
                     "entry is ours this run ([promote] sim_step), so the sampler runs at the top of "
                     "our body instead of from a trampoline. Same sampling point, same cadence.\n");
        else
            seam_log("; [desync] sim_step hook NOT installed -- the root is promoted but the pre-hook "
                     "slot is already taken. THE DETECTOR IS ARMED AND SAMPLING NOTHING: treat a "
                     "clean verdict from this run as no verdict at all.\n");
        return;
    }
    seam_log("; [desync] sim_step hook not installed -- the determinism harness owns that entry "
             "this run and feeds the detector its own per-step hash instead (expected; the "
             "reason is named in the [interlock] summary if it is anything else)\n");
}

void build_paths() {
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    char *slash = exe;
    for (char *p = exe; *p; ++p)
        if (*p == '\\' || *p == '/') slash = p;
    slash[1] = '\0';
    wsprintfA(g_ini, "%smh_net.ini", exe); // config INPUT stays next to the exe
    // log OUTPUT -> the CURRENT run folder. Seeded here so the arm-time derivations below
    // (mh_lockstep / mh_frametime / mh_temporal / mh_trace / mh_gamemode all take their directory
    // from g_log) have something to derive from; seam_paths_tick keeps it current thereafter.
    seam_paths_tick();
}


// mp_is_ipv4 + mp_read_typed_join_ip (U1c join-by-IP) -> net_discovery.cpp (net_internal.h).

// Bring the transport up lazily on the first poll, reading role + player id straight from mh.exe's
// own lobby state so they can never disagree with the game; connection params come from mh_net.ini.
void lazy_start() {
    if (g_tried_init) return;
    g_tried_init = 1;
    // U40: THE ONE PLACE ALLOWED TO RE-ENTER MH_Net_InitEx ON A STARTED TRANSPORT. The relink latch
    // is set only by mp_session_close, only at a MATCH-end reason, only on a manual client -- and it
    // is CONSUMED here, so one boundary buys exactly one re-dial. Without the latch this guard still
    // means what it always meant ("a test / an earlier dial already started the transport, leave it
    // alone"), which is what keeps the force-entry and harness paths byte-for-byte unchanged.
    const bool relink = (InterlockedExchange(&g_net_relink, 0) != 0);
    if (MH_Net_IsStarted() && !relink) return;
    if (relink) seam_log("; U40 relink: re-initialising the transport for a fresh dial to the host\n");
    MH_NetConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.role      = (*g_a.is_host != 0) ? 0 : 1; // 0 = host, 1 = client -- mirrors mh.exe
    cfg.player_id = *g_a.local_player_index;
    mh::config::read_ini_string("net", "host", "127.0.0.1", cfg.host, sizeof(cfg.host), g_ini); // TL-HARN4
    // U1c (join-by-IP, option B): a manual-menu CLIENT prefers the IP the player typed in-game over the
    // ini host=. The in-game IP field is a runtime-wired widget, so its edit buffer isn't statically
    // knowable; we scan the .bss addresses the RE trace + marker-scan identified for the mp_join browser's
    // IP field / MRU store and use the FIRST holding a valid dotted host. CONFIRMED in-game 2026-07-12: the
    // live buffer is **0xe60760** (0xe60720 was empty). We deliberately do NOT scan 0x005d0da8 -- that is the
    // game/session NAME (_G_LLM_MP_GAME_NAME), which could contain dots and false-positive as an IP. Every
    // candidate is logged so one run reveals which is live. Host + non-manual (force-entry/test) sessions
    // never enter this branch, so determinism runs are untouched.
    if (cfg.role == 1 && MH_MP_IsManual()) {
        char ip[64];
        if (mp_read_typed_join_ip(ip, sizeof(ip))) {
            lstrcpynA(cfg.host, ip, sizeof(cfg.host));
            char b[96];
            wsprintfA(b, "; U1c join-by-IP: using in-game typed host '%s' (overrides ini)\n", cfg.host);
            seam_log(b);
        } else {
            char b[96];
            wsprintfA(b, "; U1c join-by-IP: no typed IP found -> ini host '%s'\n", cfg.host);
            seam_log(b);
        }
    }
    cfg.port        = GetPrivateProfileIntA("net", "port", 6501, g_ini);
    cfg.peers       = GetPrivateProfileIntA("net", "peers", 1, g_ini);
    cfg.log         = GetPrivateProfileIntA("net", "log", 1, g_ini);
    cfg.host_assign = GetPrivateProfileIntA("net", "host_assign", 0, g_ini); // N1: host auto-assigns joiner ids
    // R-live link watchdog: 0 = the transport's shipping defaults (ping 1 s / drop after 10 s silent).
    cfg.ping_ms       = GetPrivateProfileIntA("net", "ping_ms", 0, g_ini);
    cfg.rx_timeout_ms = GetPrivateProfileIntA("net", "rx_timeout_ms", 0, g_ini);
    // U15b (2026-07-23): the hand-clicked N-player path REQUIRES host-assign. A manual joiner has no way to
    // declare a distinct player_id -- every client defaults to id 1 (mp_client_slot()) -- so with the declared-
    // id mode (host_assign=0) a 2nd joiner (carol) collides onto the 1st (bob): same id -> slot_find_or_alloc
    // returns the same slot -> carol overwrites bob + no JOIN edge fires. Force host-assign for the manual path
    // on BOTH roles (this init runs with MH_MP_IsManual() known, per the join-by-IP block above): the host then
    // WELCOMEs each conn a distinct first-free id, and the client waits for it. The verb/harness path
    // (IsManual()==0) keeps the ini value -- it uses declared, distinct ids from mp_run.
    if (MH_MP_IsManual()) cfg.host_assign = 1;
    // mp:R7a -- THE RELAY DIAL REACHES THE MODULE THROUGH MH_NetConfig, decided HERE. The module no
    // longer reads `[net] relay` for the dial (it tunnels iff cfg.relay_addr is non-empty), so a peer
    // with a relay configured finally has a direct mode: only a relay-discovery dial is relayed.
    //   - HOST: always registers on the relay when one is configured (hosting is not a dial -- mp:R6
    //     mints its room, cfg.relay_room=0 tells the module to mint).
    //   - MANUAL CLIENT: the kick site's per-dial decision (dial_wants_relay: first browser / a relay
    //     row -> relay; *Internet server* + typed IP -> direct). A direct dial leaves relay_addr empty,
    //     and cfg.host is already the typed IP (the U1c block above), so the module dials it directly.
    //   - FORCE-ENTRY / determinism (not manual): relay when configured, unchanged -- that path has no
    //     browser to read a decision from, and its relay runs are the point of the gate.
    // cfg was memset to 0, so relay_addr is empty (a direct dial) unless this sets it.
    {
        char relay_ini[80];
        if (mp_relay_addr(relay_ini, (int)sizeof(relay_ini))) {
            const bool relayed =
                (cfg.role == 0) ? true : (!MH_MP_IsManual() ? true : mp_dial_is_relayed());
            if (relayed) {
                lstrcpynA(cfg.relay_addr, relay_ini, (int)sizeof(cfg.relay_addr));
                // Host: 0 => the module mints (mp:R6). Client: the directory's pick, or `[net] port`
                // as the pre-directory guess (mp_relay_dial_room), which was cfg.port until R7a.
                cfg.relay_room = (cfg.role == 0) ? 0u : (unsigned)mp_relay_dial_room();
                char b[160];
                wsprintfA(b, "; R7a: %s dial is RELAYED via %s (room %u)\n",
                          cfg.role == 0 ? "host" : "client", cfg.relay_addr, cfg.relay_room);
                seam_log(b);
            } else {
                seam_log("; R7a: client dial is DIRECT (Internet server + typed IP) -- `[net] relay` "
                         "is configured but NOT used for this connection\n");
            }
        }
    }
    MH_Net_InitEx(&cfg);

    // Start the horizon heartbeat once the transport is up (off loader-lock, like the transport
    // threads) -- net_lockstep.cpp owns the thread + its [net] horizon_heartbeat_ms gate.
    lockstep_transport_started();

    // GAME_MODE write logger (diagnostic, net_diag.cpp): lazy-arm from a helper thread.
    gm_logger_lazy_arm();
}


// __watcall(mode=EAX, dest=EDX, buf=EBX, len=ECX) -> MH_Seam_Send; plain RET (all 4 args in regs).
__declspec(naked) void send_packet_detour() {
    __asm {
        push ecx // len
        push ebx // buf
        push edx // dest
        push eax // mode
        call MH_Seam_Send
        add  esp, 16
        ret
    }
}

// In-game lockstep seams (Phase A2). Args arrive in Watcom registers; marshal to a cdecl call. EBX/
// ESI/EDI/EBP are callee-preserved by the C body, so the __watcall contract holds; args are pushed
// before any clobber. Both original stubs are plain RET (register args), so we RET the same way.

// __watcall(buf=EAX, len=EDX) -> MH_Seam_GameSend; plain RET.
__declspec(naked) void game_send_detour() {
    __asm {
        push edx // len
        push eax // buf
        call MH_Seam_GameSend
        add  esp, 8
        ret
    }
}

// __watcall(out_sender=EAX, buf=EDX, inout_len=EBX) -> MH_Seam_GameRecv; returns *inout_len in EAX.
__declspec(naked) void game_recv_detour() {
    __asm {
        push ebx // inout_len*
        push edx // buf
        push eax // out_sender*
        call MH_Seam_GameRecv
        add  esp, 12
        ret // EAX already holds the return (= *inout_len, matching the stub)
    }
}

// ================= Workstream U: MP bootstrap (menu -> browser -> lobby) ===========================
// The retail menu->browser->lobby path is dead: session discovery/connect/advertise are empty stubs,
// the session-list pointer is NULL, and the browser's scrollbar draw null-derefs on the (always) empty
// list. We do NOT rebuild retail's DirectPlay-style enumeration -- the TCP transport is already ours
// (A0-A2). Instead: (1) synthesize ONE session record on the client from the LOCAL .MP so the browser
// lists the host and JOIN's state==0 + protocol gates pass; (2) success-no-op the connect/advertise
// stubs (the real socket is our transport, started lazily when the lobby first polls); (3) guard the
// scrollbar draw so an empty/unbound list can't crash it. The intact lobby/slot code then runs on our
// transport (slot sync + 0x0e handoff = Phase 2). Determinism: both peers name the SAME .MP -> identical
// seed byte (current_map_data+0x14), exactly like the force-entry. All stubs share prologue 55 89 e5 68
// and end in a plain RET, so a "return 0 (>=0 = ok)" replacement is safe. RE fan-out 2026-07-11; see
// The MP bootstrap notes (Workstream U).
// The record/browser-side ADDR_* and the whole record/JOIN/START cluster moved to net_discovery.cpp
// (Phase 4 stage 2). The INSTALL-site ADDR_* stay here: install_mp_bootstrap arms everything in ONE
// ordered place (the install-order invariants live together). current_map_data stays for the
// map-propagation seams below (host_send_map / MH_Seam_PollRecv / MH_MP_ClientPollMap).
constexpr uintptr_t ADDR_CUR_MAP = mh::addr::current_map_data; // map_header, 0x17c
// mh.exe dead net-stub entries (all prologue 55 89 e5 68 <imm32>, plain RET)
constexpr uintptr_t ADDR_DISCOVER_POLL  = mh::addr::discovery_poll_stub; // client discovery poll -> synth record
constexpr uintptr_t ADDR_CONNECT_PREP   = mh::addr::connect_prep_stub;   // select/connect-prep -> no-op ok
constexpr uintptr_t ADDR_JOIN_CONNECT   = mh::addr::join_connect_stub;   // client join-connect -> no-op ok
constexpr uintptr_t ADDR_HOST_ADVERTISE = mh::addr::host_advertise_stub; // host session-create/advertise -> mark host + ok
constexpr uintptr_t ADDR_NET_DISCONNECT = mh::addr::net_disconnect_stub; // disconnect -> no-op ok (U10 audit I3d:
                                                                         // retail stub is already a true void no-op with no
                                                                         // caller reading its return -> this detour is inert;
                                                                         // kept only for uniformity with the stub quartet)
// The browser scrollbar guard's entry + detour -> mh/ui/lobby_widgets.cpp (an empty-list draw
// crash is a widget bug, not a wire one); it installs from install_mp_bootstrap below, in place.
// Phase 2: the lobby->game entry (llm_lobby_map_load_async_step 0x004be990 -> build_players_from_slots +
// session_begin_multi) is gated by two dead map stubs. Host proceeds only if map_send_step_stub returns 0;
// client loops map_recv_step_stub while DAT_005d54c8==0 (never set retail -> hang). Both peers pre-load the
// same .MP, so no bytes need to cross: no-op the send stub (->0) and make the recv stub set the done flag.
constexpr uintptr_t ADDR_MAP_SEND_STUB = mh::addr::map_send_step_stub; // host async_step: ==0 -> proceed
constexpr uintptr_t ADDR_MAP_RECV_STUB = mh::addr::map_recv_step_stub; // client async_step loop
constexpr int       MAP_DATA1_SIZE     = 0x17c;                        // map_header size (the map-propagation payload below)


// ---- U12: client lobby-leave notification -------------------------------------------------------
// llm_lobby_finalize_transfer_or_enter (0x4bebed) is the CLIENT's lobby Cancel action (wired in
// llm_lobby_screen_open): it closes the map file + navigates back to the discovery browser -- but sends the
// host NOTHING (net_udp's dead leave-notify). Run-BEFORE it, tell the host we left so it frees our slot.
// Gated client-only + transport-up + lobby-is-the-active-screen. In our flow the dispatch never reaches
// finalize (the 0x0a case takes begin_map_load since no map transfer is open, and we never send the 0x0e
// start -- both verified), so on the client finalize == the Cancel button only. No spurious leave.
// ---- U23 + the no-module standing line -> mh/ui/lobby_notice.cpp -------------------------------
// The notice is menu text on a browser's status-line widget, repainted from the present hook -- pure
// UI, so the carrier, the dwell and the two literals live in the UI module. What stays here is the
// only net-shaped part: deciding the CAUSE (U13's ConsumeHostLeft + MH_MP_ConsumeExitCause) and
// handing it over as an int.

constexpr uintptr_t ADDR_FINALIZE    = mh::addr::llm_lobby_finalize_transfer_or_enter; // client Cancel/leave
void               *g_finalize_tramp = nullptr;
void                on_lobby_finalize() {
    if (g_a.is_host && *g_a.is_host) return; // host: its Cancel is a different fn (host_start_game)
    if (!MH_Net_IsStarted()) return;
    if (*(void **)mh::addr::_G_LLM_UI_MENU_WIDGET_LIST != (void *)mh::addr::lobby_widget_origin) return; // only when leaving FROM the lobby screen
    // U13: this finalize can be our own Cancel (U12: tell the host we left) OR the retail 0x0e the HOST
    // broadcasts on ITS leave (the recv seam already withdrew our session). On a host-LEFT, skip the LEAVE --
    // the host is already gone, and a LEAVE to a departed host is noise.
    if (MH_MP_ConsumeHostLeft()) {
        if (g_ls_log) seam_log("; U13: client exit is a host-LEFT -> to discovery (no LEAVE sent)\n");
        // SES1: the exit CAUSE is already decided here, so the session's reason is free. U23's codes:
        // 1 = the host broadcast its leave, 2 = the link died under us, 3 = the host refused our JOIN
        // (F3c -- the host never seated us, so like a host-left there is no LEAVE worth sending).
        const int cause = MH_MP_ConsumeExitCause();
        mp_session_close(cause == 2 ? "link_lost" : cause == 3 ? "join_refused"
                                                                              : "host_left");
        if (cause == 3) mh::ui::browser_notice_arm_refused(MH_MP_JoinRefusedReason()); // F3c: the reason, verbatim
        else mh::ui::browser_notice_arm(cause);                                        // U23: tell the player WHY, on the destination screen
        return;
    }
    MH_MP_ConsumeExitCause(); // U23: a deliberate Cancel shows nothing -- and cannot inherit a stale cause
    MH_Net_SendLeave();
    if (g_ls_log) seam_log("; U12: client leaving lobby (Cancel) -> sent LEAVE to host\n");
    mp_session_close("leave"); // SES1: our own Cancel -- the last line of this match's directory
}
__declspec(naked) void finalize_detour() {
    __asm {
        pushad
        pushfd
        call on_lobby_finalize
        popfd
        popad
        jmp  dword ptr [g_finalize_tramp] // stolen 8-byte prologue + jmp back to finalize+8
    }
}

// U13: the HOST's lobby Cancel = llm_lobby_host_start_game (0x4bf058, widget 006504c3). Run-BEFORE its
// retail body (which broadcasts the 0x0e to clients + navigates the host to the map picker/browser): reset
// this host's session identity so a re-created game is a DISTINCT lobby (fresh tag) and the still-connected
// client is not treated as pre-joined. Manual host only; the force-entry host never uses this button.
constexpr uintptr_t ADDR_HOST_LEAVE    = mh::addr::llm_lobby_host_start_game;
void               *g_host_leave_tramp = nullptr;
void                on_host_leave_lobby() {
    if (!MH_MP_IsManual() || !g_a.is_host || !*g_a.is_host) return;
    MH_MP_HostResetSessionIdentity();
    MH_MP_ResetHostMirror(); // U13: a re-JOIN into the re-created lobby re-seats the peer (fresh edge)
}
__declspec(naked) void host_leave_detour() {
    __asm {
        pushad
        pushfd
        call on_host_leave_lobby
        popfd
        popad
        jmp  dword ptr [g_host_leave_tramp] // stolen 8-byte prologue + jmp back to host_start_game+8
    }
}


// ---- The host-Start clear-loop fix + the N2 host-slot0 guard -> mh/ui/lobby_widgets.cpp --------
// Both are lobby SLOT-TABLE fixes with no transport half at all: one zeroes an add-only peer-table
// count so retail's clear loop terminates, the other refuses to remove player 0. They install from
// here, in this order, through mh::ui::install_peer_clear_fix / install_remove_player_slot_guard.

// --- Manual-host lobby-slot snapshot/restore (initial-state desync fix, 2026-07-12) --------------------
// On the MANUAL menu-join host, the lobby slot array is CLEARED somewhere in the lobby->game transition
// (llm_lobby_begin_map_load NULLs the dispatch + calls FUN_004bb78a; the next frame's async_step runs
// build_players_from_slots). Diagnosed via PLAYERDUMP: at the entry TRIGGER slot[1] is status=1
// (name=Player2), but by build_players it's status=0 -> the host builds a 1-PLAYER game (P[1] empty)
// while the client builds 2 -> hard desync from step 1 (p1_ai_econ/strat_players). Force-entry doesn't
// hit this (its host enters from a different per-frame context; exact clear culprit not pinned). Robust
// fix: snapshot the (correct) slots at the entry trigger and restore them in a run-BEFORE hook on
// build_players_from_slots, so it reads the same slots the client does. Tightly gated: manual host only,
// one-shot (cleared after the single restore so a later skirmish/SP build_players is untouched).
// U10 audit (F2): KEEP -- the lobby->game transition genuinely clears the slots (retail behavior on this
// out-of-lobby-order entry), and there is no reachable retail path that both clears and re-populates them in
// the mod's flow; the one-shot manual-host gate keeps it from touching any SP/skirmish build. Load-bearing.
constexpr uintptr_t ADDR_LOBBY_SLOTS_G = mh::addr::_G_LLM_LOBBY_SLOTS;
constexpr uintptr_t ADDR_BUILD_PLAYERS = mh::addr::llm_lobby_build_players_from_slots; // prologue 55 89 e5 68
constexpr int       LOBBY_SLOTS_BYTES  = 0x39 * 8;                                     // stride 0x39 * 8 slots
unsigned char       g_host_slot_snap[LOBBY_SLOTS_BYTES];
bool                g_host_slot_saved = false;
void               *g_bp_tramp        = nullptr;

void on_build_players_pre() {
    if (g_host_slot_saved && MH_MP_IsManual() && g_a.is_host && *g_a.is_host) {
        memcpy((void *)ADDR_LOBBY_SLOTS_G, g_host_slot_snap, LOBBY_SLOTS_BYTES);
        g_host_slot_saved = false; // one-shot: don't touch a later SP/skirmish build
        seam_log("; manual host: lobby slots RESTORED before build_players_from_slots\n");
    }
}
__declspec(naked) void build_players_detour() {
    __asm {
        pushad
        pushfd
        call on_build_players_pre
        popfd
        popad
        jmp  dword ptr [g_bp_tramp] // stolen 8-byte prologue + jmp back to build_players+8
    }
}

// The generic function-entry tracer ([trace] funcs=VA,... -> mh_trace.log) moved to net_diag.cpp;
// MH_Seam_Init calls install_trace_hooks() and the lobby dispatch calls MH_Seam_TraceDump()
// (both declared in net_internal.h).

// Install the bootstrap: synth-discovery + host-role detours, no-op the connect/advertise stubs, guard
// the scrollbar. All prologue-guarded (a mismatch leaves that site untouched). ini [net] bootstrap=1.
void install_mp_bootstrap() {
    if (!GetPrivateProfileIntA("net", "bootstrap", 1, g_ini)) {
        seam_log("; MP bootstrap disabled (ini)\n");
        return;
    }
    // F3F: FIVE OF THE SEVEN PROTOCOL STUBS ARE TRANSPORT-DEPENDENT; HOST-ADVERTISE AND THE MAP-SEND
    // NO-OP ARE NOT (U43); THE NINE BELOW THEM ARE NOT EITHER.
    //
    // discover/join/connect/disconnect/map_send/map_recv drive OUR transport directly where net_udp's
    // dead job used to be (on_discover_poll kicks the S3 connect and builds the browser from the
    // received-record store, on_join_connect sends the S4 JOIN frame, connect/disconnect/map_send/
    // map_recv are the no-ops that clear the retail path AHEAD of it). With no module there is
    // nothing for any of them to do, and running them would drive a transport that is not there.
    //
    // on_host_advertise() (ADDR_HOST_ADVERTISE) is DIFFERENT, and was misclassified with the other
    // six until U43: read its body (net_discovery.cpp) -- it sets is_host/LOCALIDX/PLAYERSIDE and
    // arms the manual-lobby menu-tick hook, and touches the wire NOT AT ALL. Leaving it gated on
    // net_mod meant _G_LLM_NET_IS_HOST -- written NOWHERE ELSE in the whole binary except this one
    // hook (llm_net_session_globals_reset only ever zeroes it at boot; confirmed by an EN
    // find-cross-references sweep of _G_LLM_NET_IS_HOST) -- stayed at its game-init default of 0 for
    // a module=none manual host. Retail's own llm_lobby_host_net_dispatch (0x004bfd35 EN) and
    // llm_lobby_screen_open (0x004be8a7 EN) both branch on that flag: with it false the HOST silently
    // took the CLIENT half of both functions -- llm_lobby_screen_open leaves Start's 0x80 HIDDEN bit
    // set (host branch clears it) and llm_lobby_host_net_dispatch's first tick sends a doomed 0x17
    // "client hello" and sets _G_LLM_LOBBY_WIDGETS_BUILT=0 instead of calling
    // llm_lobby_build_slot_widgets() (0x004bf99b EN) -- which is why U42 measured the WHOLE slot-row
    // panel empty, not even the host's own row, and Start absent from the widget list outright. So
    // install this one unconditionally: it is exactly as wire-free as the nine UI-only installs
    // below, and everything it enables downstream either touches only local widget/slot state or
    // calls an MH_Net_* forwarding shim (module_bind.cpp MH_NET_BIND_SHIM), which already returns its
    // real "not started" value when no module is bound -- the same contract MH_Seam_GameSend/
    // MH_Seam_GameRecv already lean on unconditionally in the in-game lockstep path. A module=none
    // host that fills its open slots (AI or otherwise) can Start and reach gameplay the same way a
    // module=none game already reaches it once launched: `on_begin_map_load()`'s
    // `MH_Net_PeerCount() < 1` branch (launch.cpp) already leaves a peerless host's Start click to
    // retail's own body, untouched -- the "skirmish-vs-AI" path this fix now makes reachable from the
    // manual-menu Create flow was already load-bearing for force-entry.
    //
    // The other five keep the ORIGINAL retail body (NOT DEAD-STUBBED), deliberately (map_send is the
    // measured exception, see the Phase 2 comment below): the tempting
    // alternative -- point them all at ret_zero_detour so nothing retail runs -- requires knowing
    // each entry's success convention, and getting one wrong (a "nothing received" that reads as "a
    // packet arrived") would hand the lobby garbage. The retail bodies these replace are the binary's
    // own net stubs, which have no socket layer to reach (the lobby RE, "dead at the wire"), so
    // leaving them in place is the measured-safe answer and the one the F3F scope names.
    const bool net_mod = mh::net::transport_present();
    if (!net_mod)
        seam_log("; MP bootstrap: NO NETWORK MODULE -- the five wire-touching protocol stubs (discover, "
                 "connect, join, disconnect, map_recv) are NOT installed and report zero below; "
                 "host_adv (role-marking only) and map_send (a no-op, U43) and the nine UI installs "
                 "after them arm as usual (ruling Q2/Q8)\n");
    // U30: the `if (*ADDR == PROLOGUE)` each of these used to carry is GONE -- the byte compare now
    // lives inside install_jmp/install_trampoline, which is the only place that can also ask who owns
    // the entry and say which of the two refused it. Each site passes the name its refusal, and the
    // end-of-arming summary, will print.
    bool okd = false, okh = false, okp = false, okj = false, okc = false, okx = false;
    // U43: host_advertise installs UNCONDITIONALLY -- it is role-marking, not a wire primitive (see
    // the block comment above). This is the one line that makes a module=none host's own slot row +
    // Start reachable; everything else in this function is unchanged.
    okh = install_jmp(ADDR_HOST_ADVERTISE, (void *)host_advertise_detour, entry_claim::exclusive, "the host-advertise stub");
    if (net_mod) {
        okd = install_jmp(ADDR_DISCOVER_POLL, (void *)discover_poll_detour, entry_claim::exclusive, "the synth discovery-poll stub");
        okp = install_jmp(ADDR_CONNECT_PREP, (void *)ret_zero_detour, entry_claim::exclusive, "the connect-prep no-op stub");
        okj = install_jmp(ADDR_JOIN_CONNECT, (void *)join_connect_detour, entry_claim::exclusive, "the S4 join-on-click stub");
        okc = install_jmp(ADDR_NET_DISCONNECT, (void *)net_disconnect_detour, entry_claim::exclusive, "the net-disconnect no-op stub (+ the R7 first-browser probe arm)");
    }
    okx = mh::ui::install_scrollbar_guard();
    // Phase 2: unblock the lobby->game async entry (both peers pre-load the map, so skip streaming).
    bool okms = false, okmr = false;
    // U43: the map-send no-op installs UNCONDITIONALLY as well. Retail's llm_lobby_map_send_step_stub
    // (0x0049bc19 EN) is hollow and returns an UNINITIALISED stack value, and the host's async
    // map-load step (llm_lobby_map_load_async_step, 0x004bee94 EN) treats any non-zero as failure --
    // the "Can't create the game" dialog (text 0x30c) a module=none host got on Start once its slot
    // rows existed. ret_zero touches no wire, and the retail body it replaces never did either.
    // map_recv stays gated: it is the CLIENT loop, and a client cannot exist without a transport.
    okms = install_jmp(ADDR_MAP_SEND_STUB, (void *)ret_zero_detour, entry_claim::exclusive, "the map-send no-op stub");
    if (net_mod) {
        okmr = install_jmp(ADDR_MAP_RECV_STUB, (void *)map_recv_step_detour, entry_claim::exclusive, "the map-recv step stub");
    }
    // Phase 2c: suppress the spurious self-removal modal at entry (a dialog, so the UI module owns
    // the NOP and the reason it is safe -- mh/ui/lobby_widgets.cpp).
    bool oksr = mh::ui::suppress_self_removal_dialog();
    // Host-Start hang fix: break the infinite clear-loop (the AI-slot count is never decremented).
    bool okpc = mh::ui::install_peer_clear_fix();
    // Manual-menu lobby sync: per-lobby-frame PlayerSide reaffirm + host peer-table mirror (see the detour).
    bool okld = false;
    okld      = install_trampoline(ADDR_LOBBY_DISPATCH, (void *)lobby_dispatch_detour, &g_lobby_tramp, 8,
                                   entry_claim::exclusive, "the lobby-dispatch PlayerSide/peer-mirror detour");
    // Manual-host slot-restore: run-before build_players_from_slots to restore the slots the transition clears.
    bool okbp = false;
    okbp      = install_trampoline(ADDR_BUILD_PLAYERS, (void *)build_players_detour, &g_bp_tramp, 8,
                                   entry_claim::exclusive, "the manual-host slot-restore detour");
    // U12: run-before the client's lobby Cancel (llm_lobby_finalize_transfer_or_enter) to notify the host it left.
    bool okfz = false;
    okfz      = install_trampoline(ADDR_FINALIZE, (void *)finalize_detour, &g_finalize_tramp, 8,
                                   entry_claim::exclusive, "the U12 client lobby-Cancel notifier");
    // U13: run-before the HOST's lobby Cancel (llm_lobby_host_start_game) to reset its session identity.
    bool okhl = false;
    okhl      = install_trampoline(ADDR_HOST_LEAVE, (void *)host_leave_detour, &g_host_leave_tramp, 8,
                                   entry_claim::exclusive, "the U13 host lobby-Cancel identity reset");
    // U3b/U29: settle the client's lobby slide at its end state instead of animating it -- see
    // The lobby-slide notes. Whole-body-conditional inside the detour.
    bool okiw = mh::ui::install_lobby_slide_takeover();
    // The second slide loop, observe-only apart from U38's Create-game suppression.
    bool okst = mh::ui::install_screen_slide_observer();
    // N2: host-protect guard -- remove_player_slot(0) must never compact the host's slot0.
    bool okrp = mh::ui::install_remove_player_slot_guard();
    // S7: the browser row's "occ/cap" count format -- a format STRING, patched in place against its
    // exact original bytes by the UI module (mh/ui/lobby_widgets.cpp).
    bool okrf = mh::ui::patch_browser_row_format();
    char b[600];
    wsprintfA(b, "; MP bootstrap armed: discover=%d host_adv=%d connect=%d join=%d disc=%d scrollbar_guard=%d map_send=%d map_recv=%d selfremove_dlg_suppressed=%d peer_clear_fix=%d lobby_sync=%d slot_restore=%d leave_notify=%d remove_guard=%d rowfmt_occ_cap=%d host_leave=%d\n",
              okd, okh, okp, okj, okc, okx, okms, okmr, oksr, okpc, okld, okbp, okfz, okrp, okrf, okhl);
    seam_log(b);
}

// --- Manual-menu map propagation (Workstream U Phase 2) -----------------------------------------------
// The host's map-picker selection lives in current_map_data, but the 0x1cd lobby snapshot does NOT cover
// it (it ends just before current_map_data at 0x654fe7), so the client would session_begin on its own
// synth map and desync. The host broadcasts its current_map_data (0x17c) as a custom-type datagram each
// lobby frame (throttled); the client (recv seam) copies it over its own current_map_data and flags
// MH_MP_MapReceived, which gates the client's game entry. Both then load the SAME .MP (identical seed).
constexpr unsigned char MAP_MSG_TYPE = 0x21;
void                    host_send_map(void) {
    static int ctr = 0;
    if ((ctr++ % 15) != 0) return; // ~4/s is plenty; don't flood the lobby stream
    unsigned char msg[1 + MAP_DATA1_SIZE];
    msg[0] = MAP_MSG_TYPE;
    memcpy(msg + 1, (const void *)ADDR_CUR_MAP, MAP_DATA1_SIZE);
    MH_Net_Send(MH_NET_BROADCAST, msg, 1 + MAP_DATA1_SIZE);
}

// --- Generic memory-marker finder (U1c: locate the in-game IP-entry field's buffer) -------------------
// The user types a DISTINCTIVE marker string into the target text field in-game; this background thread
// scans the process's committed memory for it (ASCII + UTF-16LE) and logs the VA(s). Then we xref that
// buffer in Ghidra to find the owning widget/screen code. ini [menu] findstr=<marker>. Read-only; runs
// regardless of game state. Use a marker unlikely to occur elsewhere (e.g. "QZ9.8Q7.6Q5.4").
char g_findstr[80] = {0};
void log_marker_hit(uintptr_t va, const char *kind, const unsigned char *p) {
    char b[160], hex[3 * 24 + 1];
    int  n = 0;
    for (int i = 0; i < 24; ++i) n += wsprintfA(hex + n, "%02x ", p[i]);
    wsprintfA(b, "; FINDSTR hit @0x%08x (%s)  ctx: %s\n", (unsigned)va, kind, hex);
    seam_log(b);
}
DWORD WINAPI marker_scan_thread(LPVOID) {
    const int ml = lstrlenA(g_findstr);
    if (ml < 3) return 0; // too short -> too many false hits
    wchar_t   wmark[80];
    int       wl = MultiByteToWideChar(CP_ACP, 0, g_findstr, ml, wmark, 80); // no NUL
    uintptr_t seen[128];
    int       nseen = 0;
    {
        char b[128];
        wsprintfA(b, "; FINDSTR scanner armed: marker=\"%s\" (ascii %d, wide %d)\n", g_findstr, ml, wl);
        seam_log(b);
    }
    for (;;) {
        Sleep(750);
        MEMORY_BASIC_INFORMATION mbi;
        for (uintptr_t a = 0x00400000; a < 0x01100000;) {
            if (!VirtualQuery((void *)a, &mbi, sizeof(mbi))) {
                a += 0x1000;
                continue;
            }
            uintptr_t base = (uintptr_t)mbi.BaseAddress, sz = mbi.RegionSize;
            uintptr_t next     = base + sz;
            DWORD     rp       = mbi.Protect & 0xff;
            bool      readable = mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD) &&
                            (rp == PAGE_READONLY || rp == PAGE_READWRITE || rp == PAGE_WRITECOPY ||
                             rp == PAGE_EXECUTE_READ || rp == PAGE_EXECUTE_READWRITE || rp == PAGE_EXECUTE_WRITECOPY);
            if (readable && sz > 0 && sz < 0x2000000) {
                const unsigned char *p = (const unsigned char *)base;
                for (uintptr_t i = 0; i + (uintptr_t)ml <= sz; ++i) {
                    if (p[i] == (unsigned char)g_findstr[0] && memcmp(p + i, g_findstr, ml) == 0) {
                        uintptr_t va  = base + i;
                        bool      dup = false;
                        for (int k = 0; k < nseen; ++k)
                            if (seen[k] == va) {
                                dup = true;
                                break;
                            }
                        if (!dup) {
                            if (nseen < 128) seen[nseen++] = va;
                            log_marker_hit(va, "ascii", p + i);
                        }
                    }
                }
                if (wl > 0)
                    for (uintptr_t i = 0; i + (uintptr_t)wl * 2 <= sz; ++i) {
                        if (memcmp(p + i, wmark, (size_t)wl * 2) == 0) {
                            uintptr_t va  = base + i;
                            bool      dup = false;
                            for (int k = 0; k < nseen; ++k)
                                if (seen[k] == va) {
                                    dup = true;
                                    break;
                                }
                            if (!dup) {
                                if (nseen < 128) seen[nseen++] = va;
                                log_marker_hit(va, "wide", p + i);
                            }
                        }
                    }
            }
            a = next > base ? next : a + 0x1000;
        }
    }
}
void install_marker_scan() {
    mh::config::read_ini_string("menu", "findstr", "", g_findstr, sizeof(g_findstr), g_ini); // TL-HARN4
    if (g_findstr[0]) CreateThread(nullptr, 0, marker_scan_thread, nullptr, 0, nullptr);
}

} // namespace

extern "C" void MH_Seam_SetAddrs(const MH_SeamAddrs *a) {
    if (a) g_a = *a;
}

// Called by launch.cpp MH_MP_HostEntryTick at the entry trigger (slots correct, occ=2) to snapshot the
// lobby slots so the build_players_from_slots run-before hook can restore them after the transition clear.
extern "C" void MH_Seam_SaveHostSlots(void) {
    memcpy(g_host_slot_snap, (const void *)ADDR_LOBBY_SLOTS_G, LOBBY_SLOTS_BYTES);
    g_host_slot_saved = true;
    seam_log("; manual host: lobby-slot snapshot taken at entry trigger\n");
}

extern "C" void MH_Seam_Send(int mode, int dest, unsigned char *buf, int len) {
    if (len < 0) return;
    // Routing (Phase 2b): the lobby uses `send_packet(0, slots[0].player_id, ...)` as a "send to the
    // session" idiom -- on the HOST, slots[0].player_id == its own local index, so a naive unicast to
    // `dest` sends to itself and vanishes (this is why the game-start 0x0a never reached the client).
    // Treat mode==-1 OR dest==our-own-index as a broadcast (reaches all real peers); the client's real
    // sends target the host (dest != its own index) so they still unicast correctly. Retail net_udp
    // broadcast semantics for a 2-player star; a relayed >2-player build restores per-dest routing.
    int self  = (g_a.local_player_index != nullptr) ? *g_a.local_player_index : -2;
    int bcast = (mode == -1) || (dest == self);
    MH_Net_Send(bcast ? MH_NET_BROADCAST : dest, buf, len);
}

extern "C" int MH_MP_MapReceived(void) { return g_map_recv ? 1 : 0; }
// U29: forget the departed lobby's map. The received-map flag is one of the three gates the manual
// client's entry driver reads, and after a host-left it keeps reporting the CLOSED lobby's map --
// which is also why the phantom lobby panel kept repainting the new host's map name (the recv seam
// memcpy's MAP_MSG_TYPE straight into current_map_data, no lobby dispatch required).
extern "C" void MH_MP_ResetMapReceived(void) { g_map_recv = 0; }

// Client-side map drain (called from the reliable menu-tick driver once slots have synced). The client
// sits on the intro-wait screen where its LOBBY dispatch stops polling, so the host's map datagram (0x21)
// never arrives via MH_Seam_PollRecv -> the entry map-gate deadlocks. Here we drain the transport queue
// ourselves and apply the map. Safe: the client already synced its slots (occ>=2) before this is called,
// so discarding the other (re-broadcast) lobby datagrams loses nothing needed.

extern "C" void MH_MP_ClientPollMap(void) {
    if (g_map_recv || !MH_Net_IsStarted()) return;
    static unsigned char buf[RX_SIZE];
    for (int guard = 0; guard < 128; ++guard) {
        int sender = -1, len = RX_SIZE;
        if (!MH_Net_Recv(&sender, buf, &len)) return; // queue drained
        if (len >= 1 + (int)MAP_DATA1_SIZE && buf[0] == MAP_MSG_TYPE) {
            memcpy((void *)ADDR_CUR_MAP, buf + 1, MAP_DATA1_SIZE);
            g_map_recv = true;
            return;
        }
        // else: a re-broadcast lobby datagram the stalled dispatch won't consume -> discard and keep draining
    }
}

// mp:GS1 (a): discard what the game queue holds at the moment the player clicks JOIN. Anything queued
// BEFORE our JOIN cannot concern the lobby we are joining -- the host sends that lobby's traffic only
// after admitting a JOIN it has not received yet -- so it is the residue of a lobby we were never in:
// a browsing client holds a live link to the host (it dialled the room for SESSION_INFO) and does not
// poll the game queue on the browser, so a host that cancels + re-creates under it leaves its retail
// 0x0e (llm_lobby_host_start_game's broadcast, `len=5`: the type byte + 4 UNINITIALISED stack bytes --
// MOV byte ptr [EBP-0x20],0xe / LEA EDX,[EBP-0x20] / MOV EBX,5 -- nothing identifies the lobby it was
// for, and the client's case-6 handler takes no payload) queued on that link. The new lobby's first
// PollRecv then drained it and U13 withdrew the seat the host had just granted: the ghost slot behind
// the field's 9999 freeze (`0189fdb8`), reproduced by the `ghost_churn` scenario. The retail dispatch
// never saw this shape because retail's transport delivered a leave notice only to a seated peer.
// Discarded here rather than tagged in PollRecv because the packet carries nothing to tag on, and
// MH_Net_Recv pops -- a type-selective drain would need a peek export on mh_net.dll's binder table for
// a queue that, on the browser, holds nothing worth keeping. Never in a live match (the lockstep
// stream owns the queue there; a JOIN click cannot happen in one, the guard says so structurally).
void mp_drain_pre_join_queue() {
    if (!MH_Net_IsStarted()) return;
    if (*(const unsigned char *)ADDR_SESSION_MODE == 3) return;
    static unsigned char buf[RX_SIZE];
    int                  n = 0, n0e = 0, first = -1;
    for (int guard = 0; guard < 1024; ++guard) { // the inbound ring is 256 deep; the bound is for a livelock
        int sender = -1, len = RX_SIZE;
        if (!MH_Net_Recv(&sender, buf, &len)) break;
        ++n;
        if (len >= 1 && buf[0] == 0x0e) ++n0e;
        if (first < 0) first = len >= 1 ? buf[0] : 0;
    }
    if (n == 0) return;
    char b[160];
    wsprintfA(b, "; GS1: JOIN click -> %d stale datagram(s) queued before the JOIN discarded (first type=0x%02x, "
                 "0x0e x%d) -- traffic of a lobby we never sat in\n",
              n, first, n0e);
    seam_log(b);
}

// R-live-ui: has this client's link to the host died while it sits in the lobby?
//
// Nothing else can notice. The retail lobby is purely reactive -- it acts on packets, and a dead link
// delivers none -- so on 2026-07-26 a client rendered a perfectly healthy lobby for ~90 s (~4100
// dispatches) after its host was gone, waiting for a Start that could never arrive. R-live gave the
// transport the ability to KNOW; this is the client acting on it.
//
// `armed` (rather than a bare PeerCount()==0) matters: the count is legitimately 0 before the client
// connects, so without it every client would bail out of the lobby on entry.
bool link_lost_in_lobby() {
    static bool armed = false, fired = false;
    if (!MH_MP_IsManual() || !g_a.is_host || *g_a.is_host) return false;               // manual CLIENT only
    if (*(const unsigned char *)mh::addr::_G_LLM_GAME_SESSION_MODE == 3) return false; // in a live match: not ours
    if (*(void **)mh::addr::_G_LLM_UI_MENU_WIDGET_LIST != (void *)mh::addr::lobby_widget_origin)
        return false; // only while the LOBBY is the active screen (the browser has its own retry path)
    // mp:R2b -- a JOIN on a row whose room this peer is not linked to tears the old link down and
    // dials the row's room WITH THE LOBBY ALREADY PUSHED (retail seats the client at the click).
    // The peer count seen here then goes 1 (the old link, for a frame or two) -> 0 (the re-dial)
    // -> 1 (the new room), and the 1 -> 0 edge is exactly what this test reads as a dead host.
    // While that JOIN is still waiting for its link, the link is not one to be lost: stay disarmed.
    if (MH_MP_JoinLinkPending()) {
        armed = false;
        return false;
    }
    if (MH_Net_PeerCount() > 0) {
        armed = true;
        return false;
    }
    if (!armed || fired) return false;
    fired = true;
    return true;
}

// mp:R6 -- A CLIENT'S RETAIL 0x17 "HELLO" THAT ARRIVED BEFORE ITS S4 JOIN WAS ADMITTED. The retail
// lobby's first dispatch on the client sends 0x17, and the retail host answers it with the slot
// table (0x08/0x13/0x09) -- but only for a peer its admin loop can see, i.e. one the S4 mirror has
// seated, i.e. one whose JOIN this host has ADMITTED. Since R6 a relayed client's link comes up
// AFTER the browser row it clicked (the room is learned from the directory and dialled then), so
// its JOIN is re-sent when the link is up (net_discovery.cpp g_join_pending) and can land a tick
// after the hello: measured 2026-09-19, hello at .535, JOIN admitted at .537, the client seated
// itself in a lobby the host never populated. So an early hello is HELD here, per sender, and
// handed to the retail dispatch again once that sender's JOIN is admitted -- one packet, same
// bytes, delivered in the order the retail host requires. Held rather than dropped and rather than
// passed through: retail did nothing with the early one (the measurement), and a second hello from
// the client never comes. A hold outliving its peer (a client that dropped before admission, its id
// later re-assigned) replays one extra hello at the next admission of that id -- retail answers it
// with one more snapshot broadcast, which every peer already tolerates at 1 Hz.
namespace {
constexpr int EARLY_HELLO_LEN = 5; // the 0x17 the client sends: type + 4 bytes, measured
unsigned char g_early_hello[8][EARLY_HELLO_LEN];
volatile LONG g_early_hello_held[8] = {0};
} // namespace

extern "C" int MH_Seam_PollRecv(void) {
    lazy_start();
    if (!MH_Net_IsStarted()) return 0;

    // mp:R6 -- replay a held hello whose sender has been admitted since (see above).
    if (MH_MP_IsManual() && g_a.is_host && *g_a.is_host) {
        for (int s = 1; s < 8; ++s) {
            if (!g_early_hello_held[s] || !MH_MP_HasJoined(s)) continue;
            InterlockedExchange(&g_early_hello_held[s], 0);
            char b[96];
            wsprintfA(b, "; R6: player %d's early 0x17 hello replayed now that its JOIN is admitted\n", s);
            seam_log(b);
            memset(g_a.rx_type, 0, RX_SIZE);
            memcpy(g_a.rx_type, g_early_hello[s], EARLY_HELLO_LEN);
            *g_a.rx_sender_id    = s;
            *g_a.rx_crc_embedded = 0;
            *g_a.rx_crc_computed = 0;
            *g_a.rx_len          = RX_SIZE;
            return RX_SIZE;
        }
    }

    // Manufacture the retail 0x0e ("host left / transition") the host WOULD have sent had it left
    // cleanly. Synthesising the packet rather than calling the exit path directly is deliberate: 0x0e
    // is already handled below and by the retail dispatch, which U13 proved navigates a client out of
    // the lobby to the discovery browser -- so a dead host and a departed host converge on ONE tested
    // transition instead of two. The sender is the host (0); the dispatch only needs the type byte.
    // mp:F3c: the host REFUSED our JOIN. The retail join path seated us in our own lobby before any
    // reply could exist, so the reply's job is to un-seat us: the same synthesised 0x0e a dead or
    // departed host uses (one tested transition, three causes), with the cause set to JOIN_REFUSED so
    // the browser we land on names the host's reason. Only while the lobby is the active screen: a
    // refusal that lands during the slide-in waits a frame, and one that lands after the player
    // already cancelled out has nothing to bounce -- the flag is cleared by the next JOIN we send.
    if (MH_MP_IsManual() && g_a.is_host && !*g_a.is_host &&
        *(void **)mh::addr::_G_LLM_UI_MENU_WIDGET_LIST == (void *)mh::addr::lobby_widget_origin && MH_MP_TakeJoinRefused()) {
        seam_log("; F3c: JOIN refused -> synthesising retail 0x0e (leave to browser, the notice names the reason)\n");
        memset(g_a.rx_type, 0, RX_SIZE);
        g_a.rx_type[0]       = 0x0e;
        *g_a.rx_sender_id    = 0;
        *g_a.rx_crc_embedded = 0;
        *g_a.rx_crc_computed = 0;
        *g_a.rx_len          = RX_SIZE;
        MH_MP_ClientOnHostLeft();         // withdraw the stored session + suppress the LEAVE (we were never seated)
        MH_MP_MarkExitCauseJoinRefused(); // ...and correct the default HOST_LEFT cause to the refusal
        return RX_SIZE;
    }
    if (link_lost_in_lobby()) {
        seam_log("; R-live-ui: host link lost in the lobby -> synthesising retail 0x0e (leave to browser)\n");
        memset(g_a.rx_type, 0, RX_SIZE);
        g_a.rx_type[0]       = 0x0e;
        *g_a.rx_sender_id    = 0;
        *g_a.rx_crc_embedded = 0;
        *g_a.rx_crc_computed = 0;
        *g_a.rx_len          = RX_SIZE;
        MH_MP_ClientOnHostLeft();      // withdraw the stale session + suppress the LEAVE we cannot deliver
        MH_MP_MarkExitCauseLinkLost(); // U23: correct the default HOST_LEFT cause -- only THIS route is a dead wire
        return RX_SIZE;
    }

    for (;;) {
        int sender = -1;
        int len    = RX_SIZE;
        if (!MH_Net_Recv(&sender, g_a.rx_type, &len)) return 0; // nothing queued -> EAX = 0
        // Intercept the host's map-propagation datagram: copy into current_map_data + fetch the next real
        // message so the lobby dispatch never sees the custom type.
        if (len >= 1 + (int)MAP_DATA1_SIZE && (unsigned char)g_a.rx_type[0] == MAP_MSG_TYPE) {
            memcpy((void *)ADDR_CUR_MAP, g_a.rx_type + 1, MAP_DATA1_SIZE);
            g_map_recv = true;
            continue;
        }
        // Phase 2c thread-1 residual: log what the LOBBY recv delivers -- the host enters the game slightly
        // before the client, so its in-game lockstep/heartbeat packets (types 1..5) can arrive while the
        // client is still in the lobby, where the lobby dispatch mishandles them (transient removal popup).
        if (g_ls_log && len > 0) {
            char b[96];
            wsprintfA(b, "; PollRecv sender=%d len=%d type=0x%02x\n", sender, len, (unsigned char)g_a.rx_type[0]);
            seam_log(b);
        }
        // U13: the host's lobby Cancel (llm_lobby_host_start_game) broadcasts retail 0x0e ("host left /
        // transition"). On the CLIENT the retail dispatch turns 0x0e into llm_lobby_finalize_transfer_or_enter,
        // which navigates to the discovery browser -- the desired destination. Withdraw the stored host session
        // FIRST so that browser lists no ghost, and flag it so the finalize detour skips the spurious LEAVE.
        // (Our OWN start uses FLAG_START, never retail 0x0e, so a received 0x0e is unambiguously a host-leave.)
        if ((unsigned char)g_a.rx_type[0] == 0x0e && MH_MP_IsManual() && g_a.is_host && !*g_a.is_host) {
            MH_MP_ClientOnHostLeft();
        }
        // mp:R6 -- an early 0x17 hello from a peer whose JOIN is not admitted yet: hold it, replay it
        // at admission (the block at the top of this function). See the note above PollRecv.
        if ((unsigned char)g_a.rx_type[0] == 0x17 && MH_MP_IsManual() && g_a.is_host && *g_a.is_host &&
            sender >= 1 && sender < 8 && !MH_MP_HasJoined(sender) && len <= EARLY_HELLO_LEN) {
            memset(g_early_hello[sender], 0, EARLY_HELLO_LEN);
            memcpy(g_early_hello[sender], g_a.rx_type, (size_t)len);
            InterlockedExchange(&g_early_hello_held[sender], 1);
            if (g_ls_log) {
                char b[96];
                wsprintfA(b, "; R6: held player %d's 0x17 hello -- its JOIN is not admitted yet\n", sender);
                seam_log(b);
            }
            continue;
        }
        // N2: guard the retail host slot-push handler (0x0c / 0x0b) against an OUT-OF-BOUNDS write. That
        // handler resolves the sender's slot via slot_for_player(sender) and memcpy's the pushed 0x38-byte
        // record into it -- but on a MISS it returns -1 and writes one slot (0x39 bytes) BEFORE
        // _G_LLM_LOBBY_SLOTS (memory corruption). A miss is possible in the transient window between a peer's
        // connect and the mirror seating its slot (or adversarially). Drop the push if no lobby slot carries
        // the sender's id, so the retail handler never runs the -1 memcpy; the pushing client restores its
        // local value and re-pushes once seated. (The N2 invariant fix makes a SEATED peer always resolve;
        // this closes the connect/rejoin edge.) _G_LLM_LOBBY_SLOTS is a DATA global -- IDENTICAL RU<->EN.
        {
            unsigned char t = (unsigned char)g_a.rx_type[0];
            if ((t == 0x0c || t == 0x0b) && sender >= 0) {
                const char *sl     = (const char *)mh::addr::_G_LLM_LOBBY_SLOTS; // stride 0x39, player_id@+0x01
                bool        seated = false;
                for (int si = 0; si < 8; ++si)
                    if (*(const int *)(sl + si * 0x39 + 0x01) == sender) {
                        seated = true;
                        break;
                    }
                if (!seated) {
                    if (g_ls_log) {
                        char b[80];
                        wsprintfA(b, "; N2 drop 0x%02x push from unseated player %d\n", t, sender);
                        seam_log(b);
                    }
                    continue; // fetch the next datagram; never deliver an unseatable push to the retail handler
                }
            }
        }
        if (len < 0) len = 0;
        if (len > RX_SIZE) len = RX_SIZE;
        if (len < RX_SIZE) memset(g_a.rx_type + len, 0, RX_SIZE - len); // clear stale tail bytes
        *g_a.rx_sender_id    = sender;
        *g_a.rx_crc_embedded = 0; // force dispatch's recomputed-vs-embedded self-check to pass
        *g_a.rx_crc_computed = 0;
        *g_a.rx_len          = RX_SIZE;
        return RX_SIZE; // nonzero -> "packet present"
    }
}

// --- Phase A2: in-game lockstep seam bodies (see mh_seam_export.h) --------------------------------
// The lockstep model broadcasts every peer's orders (type1) + horizon extends (type2) to all others,
// so the in-game send is unconditionally a broadcast (the seam has no dest arg, unlike the lobby one).
extern "C" int MH_Seam_LeaveFrozenHorizon(double *out); // net_lockstep.cpp -- mp:U19h

extern "C" void MH_Seam_GameSend(unsigned char *buf, int len) {
    lazy_start(); // force-entry skips the lobby poll -> the transport may start HERE
    if (len <= 0) return;
    // mp:U19h -- THE HORIZON FREEZE, enforced at the wire rather than at each advert site. A peer
    // that has frozen its horizon for a clean quit must not advertise a different one afterwards:
    // llm_net_player_remove's record carries the frozen value, the receiver compares it against the
    // last horizon it recorded for us (rx_dispatch.cpp's handle_peer_drop), and a disagreement ends
    // the match for EVERY remaining player. net_lockstep.cpp froze the two advert paths it owns, and
    // the one that broke it was libmh's -- the MSG_KEEPALIVE emergency bump, fired from inside the
    // park's own dispatch drain. So the gate sits HERE, at the single funnel every GAME-side send
    // passes through (llm_net_transport_send -> game_send_detour -> this), where an arm nobody has
    // enumerated is covered too. Type 2 only: orders, keepalives and the removal record itself must
    // still go out, and the seam's own deliberate adverts call MH_Net_Send directly, below this.
    if (buf && buf[0] == 2 && MH_Seam_LeaveFrozenHorizon(nullptr)) {
        static int said = 0;
        if (said < 4) {
            ++said;
            double h = 0.0;
            memcpy(&h, buf + 1, sizeof(double));
            char w[128];
            wsprintfA(w, "; [u19h] advert SUPPRESSED (%ld ms): our horizon is frozen for a clean quit\n",
                      (long)(h * 1000.0 + 0.5));
            seam_log(w);
        }
        return;
    }
    MH_Net_Send(MH_NET_BROADCAST, buf, len);
}

// Dequeue one datagram into the game's buffer (buf = &_G_LLM_NET_SEND_BUF, capacity *inout_len=0x3f8).
// MH_Net_Recv's contract matches exactly (out_sender, buf, inout_len). On empty queue set *len=0 so
// the dispatch loop (`while (len != 0)`) terminates. Return *len to mirror the original stub.
// Phase 2c thread-1 FIX: the lobby and in-game lockstep reuse ONE transport queue. During the
// lobby->game transition a stale LOBBY message (e.g. type 0x13, 41B) can still be queued when the
// client switches to the in-game recv; llm_net_lockstep_dispatch switches on buf[0]-1 with valid cases
// only 0..4 (in-game types 1..5) and its DEFAULT case TEARS DOWN the session (sets STATUS_FLAGS|=0x20,
// marks peers removed, calls FUN_004c674b(7) -> game-over). That was the "client drops at ~step 31"
// bug: the client left lockstep at frame ~3 on the first leaked lobby message. So the in-game recv must
// deliver ONLY in-game messages -- drop any dequeued datagram whose type byte is not 1..5 and fetch the
// next, so a leaked lobby packet never reaches the dispatch. (Confirmed by GameRecv logging 2026-07-11.)
inline bool is_ingame_type(unsigned char t) { return t >= 1 && t <= 5; }

// mp:SES5 decision (1) -- the 50 ms horizon-extend heartbeat (type=0x02, 9 B: [type][double horizon])
// was 95.7 MB of a measured 96 MB mh_net.log (1.88M lines, ~700/s), rotating the [desync]/session
// evidence off disk within a 43-minute match. It carries no per-packet information a reader needs
// (every one just restates "still alive, horizon=X"), so roll it up to a 1 Hz line per sender:
// count since the last flush + the last horizon seen. `GameRecv DROP` (below) and every other
// in-game type keep logging per packet -- only the 0x02 heartbeat is diet-affected.
struct GameRecv02Slot {
    int    sender = -2; // -2 = free slot; -1 is a real "unknown sender" value elsewhere in this file
    int    count  = 0;
    double last_h = 0.0;
};
GameRecv02Slot g_gr02[8]; // 8 = LS_LATE_PEERS' width (net_lockstep.cpp) -- ample for any real sender id
DWORD          g_gr02_flush_t = 0;

void gamerecv02_rollup(int sender, double horizon) {
    GameRecv02Slot *slot = nullptr;
    for (auto &s : g_gr02) {
        if (s.sender == sender) {
            slot = &s;
            break;
        }
    }
    if (!slot) {
        for (auto &s : g_gr02) {
            if (s.sender == -2) {
                slot         = &s;
                slot->sender = sender;
                break;
            }
        }
    }
    if (slot) {
        ++slot->count;
        slot->last_h = horizon;
    }
    DWORD now = GetTickCount();
    if (now - g_gr02_flush_t < 1000) return; // 1 Hz rollup cadence
    g_gr02_flush_t = now;
    for (auto &s : g_gr02) {
        if (s.sender == -2 || s.count == 0) continue;
        char b[128];
        wsprintfA(b, "; GameRecv sender=%d type=0x02 count=%d last_horizon_ms=%ld (1s)\n", s.sender,
                  s.count, (long)(s.last_h * 1000.0));
        seam_log(b);
        s.count = 0;
    }
}

extern "C" int MH_Seam_GameRecv(int *out_sender, unsigned char *buf, int *inout_len) {
    lazy_start();                                     // idempotent (g_tried_init); starts the transport on the first frame
    const int cap = inout_len ? *inout_len : RX_SIZE; // game preloads capacity (0x3f8) in *inout_len
    for (;;) {
        int len = cap;
        if (!MH_Net_IsStarted() || !MH_Net_Recv(out_sender, buf, &len)) {
            if (inout_len) *inout_len = 0;
            return 0; // queue empty
        }
        if (len > 0 && is_ingame_type(buf[0])) { // a real in-game lockstep message
            if (inout_len) *inout_len = len;
            if (g_ls_log) {
                int sender = out_sender ? *out_sender : -1;
                if (buf[0] == 2 && len >= 9) {
                    double h;
                    memcpy(&h, buf + 1, sizeof(double));
                    gamerecv02_rollup(sender, h);
                } else {
                    char b[128];
                    wsprintfA(b, "; GameRecv sender=%d len=%d type=0x%02x\n", sender, len, buf[0]);
                    seam_log(b);
                }
            }
            return len;
        }
        // stale lobby / non-lockstep message -> drop it and pull the next
        if (g_ls_log && len > 0) {
            char b[128];
            wsprintfA(b, "; GameRecv DROP non-lockstep sender=%d len=%d type=0x%02x\n",
                      out_sender ? *out_sender : -1, len, buf[0]);
            seam_log(b);
        }
    }
}

// Start the transport on demand -- reads role from the game's IS_HOST/LOCAL_PLAYER_INDEX (which the
// caller must set first) + host/port from mh_net.ini. Exposed so the launch-to-state MP force-entry
// can bring the transport up AT THE MENU and gate the sim start on peer presence (F-gate), rather than
// relying on the first in-game poll. Idempotent (lazy_start guards on g_tried_init).
extern "C" void MH_Seam_StartTransport(void) { lazy_start(); }

// S8: re-arm the transport-init latch after a failed connect, so lazy_start re-runs on the next kick and
// re-reads the (corrected) typed IP. Safe after a FAILED connect because g_started stays false, so the
// re-call simply re-runs start_client with the new host.
// U40 made this key TWO things rather than one, and the difference is the relink latch, not this
// function: on a started transport lazy_start still refuses unless g_net_relink is set, and when it IS
// set MH_Net_InitEx stops the old transport before dialling (net_transport.cpp's net_reset).
extern "C" void MH_Seam_ResetTransportInit(void) { g_tried_init = 0; }

// S3 dev gate: 1 => the manual host must NOT auto-enter the game at 2 occupied slots (so it stays in the
// lobby advertising while a client browses). Read from [net] hold_start. Superseded by S4 join-gating.
extern "C" int MH_Seam_HoldStart(void) { return g_hold_start; }


// ---- parallel test lanes: give this process its OWN single-instance guard ------------------------
//
// WinMain @0x004a0b38 opens with the classic single-instance dance:
//
//     mutex = CreateMutexA(NULL, /*bInitialOwner*/ TRUE, "MHMutex");
//     do { r = WaitForSingleObject(mutex, 0);
//          if (r == WAIT_TIMEOUT) { hWnd = FindWindowA(cls, title);
//                                   if (hWnd) { SetForegroundWindow(hWnd); return 0; } }
//          else if (r == WAIT_FAILED) return 0;
//     } while (r != WAIT_OBJECT_0);
//
// THE NAME HAS NO PATH COMPONENT. It is a bare, machine-wide kernel object, so running two copies
// from two different install folders still collides -- per-folder deployment does not buy per-process
// instancing on its own, which is the thing that is easy to assume and wrong.
//
// Note the second-instance failure mode when the window is NOT found (a headless VM has no findable
// window): the loop does not exit, it SPINS on WaitForSingleObject until the first instance quits.
// That presents as a HUNG peer, not as a refused launch -- worth knowing before diagnosing one.
//
// `[uitest] lane=N` rewrites the string IN PLACE to "MHMutNN". Same length (7 chars + NUL both ways),
// so nothing moves, no other reference needs fixing, and the guard keeps working -- this RENAMES
// rather than REMOVES, so each lane still gets exactly one game instance, which is the property we
// actually want. lane=0 (the default) leaves the shipped name untouched, so a normal install is
// bit-identical in behaviour to before.
//
// Timing: this runs from MH_Seam_Init, i.e. from DllMain, which for a statically-imported DLL
// executes BEFORE the exe's entry point -- so WinMain has not read the string yet. Same window
// apply_dpi_awareness relies on.
// F2G: the key moved out of its own one-key `[test]` section into `[uitest]`, where the rest of
// the harness-driven test surface already lives (enable/script/timeout_frames). A `[test]`
// section is REFUSED from mh::config now, so a stale fragment cannot silently put two lanes on
// one mutex -- which presents as a launch that never happens, not as a wrong value.
static void apply_test_lane(void) {
    const int lane = GetPrivateProfileIntA("uitest", "lane", 0, g_ini);
    if (lane <= 0) return;

    char *name = reinterpret_cast<char *>(mh::addr::_G_LLM_SINGLE_INSTANCE_MUTEX_NAME);
    // Expected-bytes guard, same discipline as every other patch in this DLL: if the string is not
    // what we generated against, do nothing and say so rather than corrupt a neighbouring literal.
    if (memcmp(name, "MHMutex", 8) != 0) {
        seam_log("; [uitest] lane NOT applied -- mutex name string is not \"MHMutex\" (wrong build?)\n");
        return;
    }
    DWORD prot = 0;
    if (!VirtualProtect(name, 8, PAGE_READWRITE, &prot)) {
        seam_log("; [uitest] lane NOT applied -- VirtualProtect failed on the mutex name\n");
        return;
    }
    wsprintfA(name, "MHMut%02d", lane % 100);
    VirtualProtect(name, 8, prot, &prot);

    char b[128];
    wsprintfA(b, "; [uitest] lane=%d -- single-instance mutex renamed to \"%s\" (parallel lanes)\n", lane, name);
    seam_log(b);
}

// ==== THE ARM SPINE (fork F3B / plan D2) =========================================================
//
// MH_Seam_Init used to be ONE 355-line body behind ONE gate, `[net] enable`. That gate disabled
// everything below it: the whole reimpl spine, the in-memory patcher, the tombstones, the
// hostapi/libmh_in/hostevt reports, the instruments and 14 of the 18 ini sections this DLL reads --
// all of it switched off by a NETWORKING key. F3B splits the body into three named arms plus the
// harness hand-off, and DEMOTES the gate: `[net] enable=0` now skips the nine NET steps and
// nothing else.
//
//   MH_Core_Arm  -- mh.dll core, unconditional. Paths, the EN guard, the promotion registry and the
//                   promotions, the in-memory patcher, the pacing/diagnostic seams, the instruments
//                   (video / standalone / keyrepeat / pause / overlay / capture / uidrive), the
//                   desync detector, the tracer, the tombstones and every arm-time report.
//                   Its EARLY phase -- paths + the state / host-api / inbound / event binds + the
//                   rebind arm -- is MH_Core_Arm_Early(), which DllMain calls BEFORE
//                   MH_Harness_Init because that is G104's earliest common point. See
//                   mh/include/mh_core_arm_export.h; it still lives in harness.cpp, which is the
//                   F4 file cut, not this item's.
//   MH_Net_Arm   -- the transport, and the ONLY thing `[net] enable=0` skips: the enable gate, the
//                   all-or-nothing prologue guard, the four transport install_jmps, hold_start and
//                   the five control-frame handler binds.
//   MH_UI_Arm    -- the MP menu/lobby surface: install_mp_bootstrap + MH_Menu_Install.
//   the hand-off -- MH_Harness_LateArm (D18) and MH_Harness_ReportRelocation: the two points where
//                   the instrument takes its turn inside our arm sequence.
//
// THE ORDER IS THE CONTRACT -- tools/check_arm_order.py gates the log this sequence produces. The
// three arms are NOT called as siblings in sequence, and that is deliberate, not an unfinished
// split: every "after every install above" invariant in this file is a fact about the WHOLE
// sequence rather than about one arm -- install_trace_hooks runs last so an already-hooked entry is
// auto-skipped, tomb_install trap-fills only what nothing claimed, report_entry_refusals reports on
// installs that have already happened. So the core arm is the spine and it calls the other two at
// the points those invariants fix.
//
// THE ONE RULED REORDER (F3 ruling Q1). The four transport install_jmps used to run FIRST, ahead of
// set_owner_table + reimpl_probe_install. They now run after them, and after the in-memory patcher.
// Two reasons, and the second is a bug fix rather than a rearrangement: (a) the all-or-nothing
// prologue guard that forced them early is internal to the net arm -- it says "do not half-install
// the transport", not "do not arm anything else"; and (b) G68's order is
// set_owner_table + reimpl_probe_install -> MH_InMemPatch_Install -> DETOURS, and these four ARE
// detours. They sat on the wrong side of the patcher, so a static patch landing inside a transport
// body met no registry that could name the collision. The move shows in the arm log as exactly ONE
// line changing position (`; ==== MP transport seams armed ====`), recorded with that reason in
// both tools/data/arm_order baselines.
// =================================================================================================

// ---- MH_Net_Arm -- the transport arm (the nine N-steps) -----------------------------------------
//
// Everything in here is a networking question, and since F3B nothing outside it is. The gate's old
// reading ("arm nothing") is gone; this is the whole of what `[net] enable=0` now costs a run.
//
// F3F ADDS THE SECOND AXIS, ahead of the first. `[net] enable` is the operator's off switch for a
// transport that IS here; `mh::net::transport_present()` answers whether there is one at all (today
// `[net] module`, at F4 "did mh_net.dll load" -- mh/include/mh_transport_present.h). The absent case
// is checked FIRST because it is the stronger statement: `enable=0` on a build with no module is not
// a configuration anyone can act on, and reporting it as the reason would name a switch instead of a
// missing binary. It is also the line the arm-order gate selects the no-module baseline by, so it has
// to be the one that prints.
static void MH_Net_Arm(void) {
    if (!mh::net::transport_present()) {
        // THE PARENTHETICAL `([net] module=none)` WAS HERE UNTIL F4B AND HAD TO GO, because from
        // F4B there are TWO ways to reach this line and only one of them is that key: the run
        // declined the module (`[net] module=none` -> `NOT ATTEMPTED`) or mh_net.dll is not on disk
        // (`NOT BOUND`). Naming one of them in the other's run is a log that lies, and the WHY is
        // already stated -- loudly, with the path and the Win32 error -- by the `; [modules] mh_net:`
        // line module_bind.cpp writes at the head of this same file. One line says why, this one
        // says what: the cause belongs to the loader, the consequence belongs to the arm.
        //
        // This line is ALSO check_arm_order's `_nomodule` baseline selector (NOMODULE_RE), and it is
        // a better one for the change: it is derived from what the run DID rather than from what a
        // key said, which is F3F's own stated design for that key. Both no-module shapes now select
        // the same baseline, which is the point -- they arm identically.
        seam_log("; ==== NO NETWORK MODULE -- there is no transport in this process, so the "
                 "lobby/lockstep seams and the control-frame handler binds are NOT installed and "
                 "the retail net entries keep their own (dead-at-the-wire) bodies. The core arm is "
                 "unaffected. ====\n");
        return;
    }
    // SHIP BUILD (2026-07-25): the seams arm with NO mh_net.ini present. Multiplayer is the point of
    // this DLL, so a plain install must be a working host AND client with zero configuration; every
    // [net] key now carries a shipping default (see mh_net.example.ini) and the ini is purely an
    // override file. `[net] enable=0` is the explicit off switch -- the old "no ini => inert" rule
    // only worked as a gate because the hosted pool needed the DLL to stay out of the way, and the
    // pool now detects itself (MH_Pool_Needed, mh.c).
    if (GetFileAttributesA(g_ini) != INVALID_FILE_ATTRIBUTES &&
        !GetPrivateProfileIntA("net", "enable", 1, g_ini)) {
        seam_log("; MP transport NOT armed: [net] enable=0 -- the core arm is unaffected (F3B: this "
                 "key now skips the nine net steps, not the DLL)\n");
        return;
    }

    uint32_t p_send  = *(const uint32_t *)ADDR_SEND_PACKET;
    uint32_t p_poll  = *(const uint32_t *)ADDR_POLL_RECV;
    uint32_t p_gsend = *(const uint32_t *)ADDR_GAME_SEND;
    uint32_t p_grecv = *(const uint32_t *)ADDR_GAME_RECV;
    if (p_send != PROLOGUE || p_poll != PROLOGUE || p_gsend != PROLOGUE || p_grecv != PROLOGUE) {
        char b[200]; // wrong build guard (defense in depth)
        wsprintfA(b, "; MP seams NOT armed: unexpected prologue (send=%08X poll=%08X gsend=%08X grecv=%08X, wrong mh.exe build)\n",
                  p_send, p_poll, p_gsend, p_grecv);
        seam_log(b);
        return;
    }
    // The four-at-once guard above is KEPT deliberately (U30 collapsed the one-site-one-check form
    // everywhere else): it is all-or-nothing on purpose -- a half-installed transport is worse than
    // none -- so it has to run before the first write, not per site. Each install still carries the
    // default byte expectation, so the primitive re-checks and can still name an owner. It is also
    // why this guard is NOT a reason to arm the transport ahead of the core (F3 ruling Q1): it
    // scopes the four installs to each other, not the four installs to the rest of the DLL.
    bool ok1 = install_jmp(ADDR_SEND_PACKET, (void *)send_packet_detour, entry_claim::exclusive, "the lobby send seam");   // lobby send
    bool ok2 = install_jmp(ADDR_POLL_RECV, (void *)MH_Seam_PollRecv, entry_claim::exclusive, "the lobby recv seam");       // lobby recv
    bool ok3 = install_jmp(ADDR_GAME_SEND, (void *)game_send_detour, entry_claim::exclusive, "the A2 lockstep send seam"); // in-game lockstep send (A2)
    bool ok4 = install_jmp(ADDR_GAME_RECV, (void *)game_recv_detour, entry_claim::exclusive, "the A2 lockstep recv seam"); // in-game lockstep recv (A2)
    g_armed  = ok1 && ok2 && ok3 && ok4;
    seam_log(g_armed ? "; ==== MP transport seams armed (lobby send/recv + in-game lockstep send/recv hooked) ====\n"
                     : "; MP seams FAILED to install (VirtualProtect?)\n");
    g_hold_start = GetPrivateProfileIntA("net", "hold_start", 0, g_ini); // S3 dev gate (no host auto-enter)

    // The five recv-thread control-frame handler binds. They used to sit near the END of the old
    // body, between install_marker_scan and install_desync_watch, and they are here now because they
    // are NET steps: with no transport there is nothing to hand a control frame to. Moving them is
    // free of ordering risk and invisible to the arm-log gate -- they are plain function-pointer
    // setters on the transport module, they emit no line, and the recv thread that reads them does
    // not exist until lazy_start, long after every install in this file.
    MH_Net_SetSessionInfoHandler(on_session_info_recv); // S2: log received host SESSION_INFO (client side)
    MH_Net_SetJoinHandler(on_join_recv);                // S4: host admits a peer on an explicit lobby-id JOIN
    MH_Net_SetStartHandler(on_start_recv);              // U2: client enters when the host broadcasts FLAG_START
    MH_Net_SetLeaveHandler(on_leave_recv);              // U12: host frees the slot when a client leaves the lobby
    MH_Net_SetAnnounceHandler(on_announce_recv);        // U16: client renders host-broadcast "<name> joined/left" lines
}

// ---- MH_UI_Arm -- the MP menu/lobby surface -----------------------------------------------------
//
// F3F LANDED RULING Q2 HERE, and the shape it landed is "this arm is unconditional". The MP menu
// surface does not ask whether there is a transport: MH_Menu_Install arms the NETWORK GAME button in
// every configuration, the nine UI installs inside install_mp_bootstrap arm with it, and the menu
// geometry stays one shape. What the predicate decides is narrower and lives one level down -- the
// seven protocol stubs inside install_mp_bootstrap (see the note there) -- plus the one thing the
// player can actually see: with no module the browser lists nothing, so it carries a line saying why
// rather than an empty panel the player has to interpret.
//
// Arming the button either way is the point of the ruling, not an oversight. A menu that grows and
// shrinks with the build is a menu whose absence of an entry has to be told apart from a bug; a menu
// that always offers multiplayer and explains what is missing is one screen and one sentence.
// F3E: the arm also WIRES the UI module, and that is the whole of what mh::ui knows about this side.
// The three injections are deliberately the only edge -- mh/ui names no net symbol, so the D4
// boundary is checkable by grep rather than by reading (mh/ui/lobby_ui.h says why each exists). They
// must precede install_mp_bootstrap, because its installs log through the first of them.
static void MH_UI_Arm(void) {
    mh::ui::set_logger(seam_log);                                              // the module's diagnostic lines
    mh::ui::set_diag_flag(&g_ls_log);                                          // [net] lockstep_log, BY POINTER
    mh::ui::set_client_session_gate(ui_client_session_gate);                   // the slide take-over's one net question
    install_mp_bootstrap();                                                    // Workstream U Phase 1: synth discovery + no-op connect stubs + scrollbar guard
    MH_Menu_Install();                                                         // Workstream U: restore the severed Multiplayer main-menu button (best-effort)
    mh::seams::maps::install();                                                // mp:X2: the map-load resolve seam (the replaced utils_open_file)
    if (!mh::net::transport_present()) mh::ui::browser_notice_arm_no_module(); // ruling Q2's visible half
}

// ---- MH_Core_Arm -- mh.dll core, and the spine the other arms hang off --------------------------
static int MH_Core_Arm(void) {
    build_paths();

    // BOTH of these used to be called out as running BEFORE the [net] enable gate, for a reason F3B
    // has now generalised to the whole core arm: neither is a networking question, and both must
    // still happen in a build with the MP seams switched off.
    //   * which process instance we are (the single-instance mutex name), and
    //   * DPI awareness, which additionally MUST precede the first window.
    // MH_Video_ApplyProcessAttrs used to ride inside MH_Video_Install, which sat far below that gate
    // -- so `[net] enable=0` silently gave back the DPI-virtualised, compositor-stretched window.
    // Found 2026-07-28 standing up parallel test lanes (their ini sets enable=0). That whole class
    // of bug is what the demotion removes: nothing below is behind a networking key any more.
    apply_test_lane();
    MH_Video_ApplyProcessAttrs();

    // EN-only build gate: every VA in this TU is an EN VA; on any other
    // image arm NOTHING (the per-hook prologue/expected-bytes guards remain as the second layer).
    // This is a CORE gate, not a net one -- it is the reason the whole arm may touch fixed VAs --
    // so since F3B it stands ahead of the net arm instead of behind `[net] enable`.
    if (!mh::en_build_ok()) {
        seam_log("; NOTHING armed: not the EN build\n");
        return 0;
    }
    seam_log("; build=EN (EN-only DLL; probe ok)\n");

    // GAME_MODE write logger (diagnostic, net_diag.cpp). Capture the main/frame thread id HERE
    // (DllMain runs on it for a static import) so the arm thread can set its debug registers;
    // config + lazy arming live in net_diag (gm_logger_configure / gm_logger_lazy_arm).
    g_main_tid = GetCurrentThreadId();
    gm_logger_configure();

    // P0-EXPORT proof: replace a game function with a C++ body through the generated entry thunk.
    // Self-gating -- it checks itself against the original before patching and arms nothing on a
    // mismatch, so a wrong reimplementation costs a log line, not the run.
    mh::hook::set_export_logger(seam_log);
    // The patch/seam interlock (C1). Must be installed BEFORE the first promotion and before the
    // first byte patch below, because it is what stops a patch landing inside a body we have JMP'd
    // away -- an inert fix whose arming line still reads "armed".
    mh::hook::set_promotion_logger(seam_log);
    mh::hook::set_owner_table(mh::addr::promotable_ranges, mh::addr::promotable_range_count);
    reimpl_probe_install();

    // F1E, THE IN-MEMORY STATIC-PATCH SPIKE. Default OFF ([patch] inmem, and with the key absent
    // nothing here reads or writes anything). Placed HERE and not earlier for one reason: the C1
    // registry it consults is populated by the two lines above, and a static patch landing inside a
    // body this run promoted is precisely the collision the interlock exists to name. It must equally
    // run BEFORE the byte patches and detours below, so that whichever of the two arrives second is
    // the one refused. (F3B: it no longer sits behind `[net] enable` -- a static patch is not a
    // networking question, and that gate is the net arm's alone now. F3G gives `[patch] inmem` the
    // arm point it deserves on its own terms; this item only stops a networking key deciding it.)
    MH_InMemPatch_Install(g_ini);

    // ---- THE NET ARM (F3 ruling Q1) -------------------------------------------------------------
    // HERE, and not at the top of this function where the four transport installs used to be. G68's
    // order is set_owner_table + reimpl_probe_install -> MH_InMemPatch_Install -> detours, and the
    // transport seams ARE detours: installed ahead of the registry, a promotion or a static patch
    // colliding with a transport body had nothing to be refused by. Everything MH_Net_Arm does is
    // gated by `[net] enable`; nothing above or below it is. This call site is the one ruled arm-log
    // reorder of F3B -- the `MP transport seams armed` line moves from the top of the log to here.
    MH_Net_Arm();

    // Lockstep pacing/perf seams (net_lockstep.cpp): the [net] pacing knobs + overlay de-fang +
    // hires/qpc clock + the per-frame timing log + the time_tick hook -- same install order as
    // pre-split (arm-log parity gate). Sets g_ls_log (read by the DIAG gates below + net_diag).
    lockstep_install_core();

    // Promotions are installed above (reimpl_probe_install) and the byte patches just now, so this is
    // the first point at which the interlock can state the whole picture: which registered fixes were
    // displaced by a promotion, and what carries each of them instead. Unconditional -- a run with no
    // promotions says so in one line, which is what makes the promoted case legible by contrast.
    mh::hook::report_interlock(mh::addr::registered_patches, mh::addr::registered_patch_count);

    // D15: the exit witness, before the gated loggers and never gated itself -- a run that dies
    // silently is exactly the run nobody remembered to turn logging on for.
    install_exit_witness();

    // Phase 2c thread-1: presence_lost entry logger (diagnose the client's early SESSION 3->2 drop).
    // Armed with the lockstep log so an MP diagnostic run captures it automatically.
    if (g_ls_log) {
        install_presence_lost_logger(); // net_diag: presence_lost entry logger (pure log)
        if (install_trampoline(ADDR_SESSION_BEGIN_MULTI, (void *)session_begin_multi_detour, &g_sbm_tramp, 8,
                               entry_claim::exclusive, "the session_begin_multi logger"))
            seam_log("; session_begin_multi logger armed\n");
        else {
            // C10: THE ENTRY WAS LOST, SO REGISTER INTO THE BODY INSTEAD -- the D18 arrangement, not a
            // fallback for tidiness. sim_resid's promotion of llm_strat_session_begin_multi runs before
            // this install (measured: 10:02:04.219 vs 10:02:04.524), so at the ship default this branch
            // is ALWAYS the one taken, and until now it merely logged and moved on. What was being
            // dropped is not a log line: on_session_begin_multi carries the manual-menu MP host-count
            // fix-up -- without it a host builds a mode-2 solo game while its client builds mode-3 --
            // and mh::desync::session_reset(). Registered only here, never alongside a live trampoline,
            // so the observer fires exactly once per entry either way.
            mh::hook::register_callback(mh::hook::point::session_begin_multi,
                                        &on_session_begin_multi);
            seam_log("; session_begin_multi logger REBOUND into the promoted body (C10) -- the entry is "
                     "owned by a promotion, so the host-count fix-up and desync session_reset run from "
                     "the body's first statement instead of a trampoline\n");
        }
        install_savegame_err_logger(); // net_diag: error-dlg CALLER logger (pure log)
    }


    // Present hook (frametime / eager-advertise / temporal drain) + the game-over leave-lockstep
    // detour (net_lockstep.cpp) -- after the g_ls_log loggers, same install order as pre-split.
    lockstep_install_present_gameover();

    MH_UI_Arm();        // the MP menu/lobby surface: install_mp_bootstrap + MH_Menu_Install (F3B)
    MH_Video_Install(); // D13: [video] size_mode -> pin the persisted display mode before the menu caches it
    {
        // Stock-exe support: the runtime twins of the no_cd / run_without_focus manifests. Both
        // are inert (guard mismatch) on an exe that already carries the static patch.
        const int st = MH_Standalone_Install();
        char      m[160];
        wsprintfA(m, "; standalone: no-CD fallback=%s run-without-focus=%s\n",
                  (st & MH_STANDALONE_NO_CD) ? "armed" : "inert",
                  (st & MH_STANDALONE_FOCUS) ? "armed" : "inert");
        seam_log(m);
    }
    {
        // U21: the WM_DEVICECHANGE NULL-lParam crash. ON by default -- it is a strict bug fix (a
        // message with no header describes no volume, so the original had nothing to do with it
        // anyway), and the crash it prevents kills the process silently on a box with WER off.
        // The probe is the reproduction: [compat] devchange_probe_ms > 0 posts ONE malformed
        // broadcast to our own window that many ms after init, which with devchange_guard=0
        // reproduces the crash on demand. See U21.
        const int guard = GetPrivateProfileIntA("compat", "devchange_guard", 1, g_ini);
        const int probe = GetPrivateProfileIntA("compat", "devchange_probe_ms", 0, g_ini);
        const int st    = MH_Standalone_InstallDevChangeGuard(guard, probe);
        char      m[200];
        wsprintfA(m, "; compat: WM_DEVICECHANGE NULL-lParam guard=%s%s\n",
                  (st & MH_STANDALONE_DEVCHANGE) ? "armed"
                                                 : (guard ? "NOT armed (unexpected prologue)"
                                                          : "off ([compat] devchange_guard=0)"),
                  probe ? " -- probe armed, one malformed broadcast is coming" : "");
        seam_log(m);
    }
    MH_KeyRepeat_Install();  // U24: modal key pump -> one dispatch per keystroke in menu/lobby text fields (best-effort)
    MH_Pause_Install();      // D19: [pause] key -> enter the orphaned mode-5 PAUSE screen from the strategic view (best-effort)
    MH_FontGuard_Install();  // F2: bounds-guard glyph_table[code_unit] in llm_gfx_font_layout_text + the [fonts] probe (best-effort)
    MH_ChatInput_Install();  // F3: layout-aware key translate + in-game chat codec + the pinned [input] codepage (best-effort)
    MH_CheatGate_Install();  // CH1: the SP cheat console (Shift+Enter line) refused in a lockstep match + the redacted chat submit log (best-effort)
    MH_DiploEcho_Install();  // U39: the diplomacy dialog's optimistic relation write NOPed -- the 0xf4 commit is the hashed cell's only writer (best-effort)
    MH_CancelTask_Install(); // D28: the building dialog's cancel-task Yes issues the equivalent building order in a lockstep match (best-effort)
    MH_Overlay_Install();    // debug overlay: [debug] ini pages -> painted on present BEFORE capture reads (best-effort)
    MH_Capture_Install();    // UI capture harness: hook present-flip -> F12/[capture] frame dump (best-effort)
    MH_UIDrive_Install();    // UI automation harness (Phase 2): [uitest] click-driver via the mouse ring (best-effort)
    install_marker_scan();   // U1c diag: [menu] findstr=<marker> -> locate the in-game IP-entry buffer
    // (the five MH_Net_Set*Handler binds stood here until F3B; they are net steps and moved into
    //  MH_Net_Arm above -- see the note there for why the move is order-free and log-invisible.)
    install_desync_watch(); // D21: runtime desync detector -- [desync] ini section,
                            //      the FLAG_HASH handler, and the ship-path sim_step hook
    install_trace_hooks();  // debug tracer LAST: [trace] funcs=VA -> mh_trace.log. After all self-hooks so
                            // an already-hooked entry (jmp != PROLOGUE) is auto-skipped, never double-hooked.
    // ---- THE HARNESS HAND-OFF (1 of 2; the other is MH_Harness_ReportRelocation below) ----------
    // D18, and it must stay after reimpl_probe_install above: this is where the harness does the work
    // that needs to know WHICH implementation owns an entry. MH_Harness_Init runs before this whole
    // function, so anything it decided at its own init time predates every promotion -- which is how
    // `[harness] replay_suppress_enqueue` came to take llm_strat_order_enqueue's entry first and leave
    // `[promote] orders` reporting "8/9 seams installed -- PARTIAL, treat this run as invalid".
    MH_Harness_LateArm();

    // X-TOMB (the endgame plan D-E2): tombstones over every body this run claims dead -- the
    // dead remainder of each note_promoted body, plus the ledger-adjudicated dead table. It MUST
    // come after MH_Harness_LateArm and every install above: the runtime promoted set is read here,
    // and a promotion recorded after the fill would leave its body live-looking and unarmed. The
    // report joins the arming summaries below -- the same end-of-arming, affirmative-case
    // discipline, because an unarmed tombstone claim is the U30 shape all over again.
    {
        mh::hook::tomb_options topt{};
        topt.arm_promoted = GetPrivateProfileIntA("tombstone", "enable", SHIP_TOMBSTONE, g_ini) != 0;
        // arm_dead is OPT-IN (default 0), NOT part of the ship default -- MEASURED 2026-09-01: a
        // migration ledger's `dead` row means "unreached within THAT domain's measured closure", not
        // "never executes in any mode". llm_strat_ai_build_target_list is `dead` in the AI ledger and
        // executes in a real single-player strategic game against the AI opponent, so arming the dead
        // set by default TERMINATED four SP-strategic UI scenarios. The airtight claim is the
        // promoted-body remainder (entry is provably JMP'd away); the dead-claim is a DELIBERATE audit
        // run via `[tombstone] arm_dead=1`, which is where you want it to fire so you can investigate.
        topt.arm_dead =
            topt.arm_promoted && GetPrivateProfileIntA("tombstone", "arm_dead", 0, g_ini) != 0;
        static char tomb_force[512];
        mh::config::read_ini_string("tombstone", "force_arm", "", tomb_force, sizeof(tomb_force), // TL-HARN4
                                    g_ini);
        topt.force_arm = tomb_force[0] ? tomb_force : nullptr;
        // The same negative arm addressed BY DOMAIN ("sim,tact"). The by-name key above cannot
        // express a whole-domain sweep: its list is parsed through a bounded buffer and one domain's
        // names run to kilobytes, so a hand-written sweep would truncate and report itself armed.
        // ROOTS-LIVE needs the per-domain form because its acceptance test is per-domain -- a
        // domain that armed nothing is a failure there, and a global total cannot say which domain
        // that was.
        static char tomb_force_dom[256];
        mh::config::read_ini_string("tombstone", "force_arm_domain", "", tomb_force_dom, // TL-HARN4
                                    sizeof(tomb_force_dom), g_ini);
        topt.force_arm_domain = tomb_force_dom[0] ? tomb_force_dom : nullptr;
        // Safe-end caps: never trap-fill bytes that are not exclusively the function's own (shared
        // epilogue tails, disjoint-body gaps -- the 2026-09-01 arm_dead false hit).
        topt.tail_caps      = mh::addr::tombstone_tail_caps;
        topt.tail_cap_count = mh::addr::tombstone_tail_cap_count;
        // The set a per-domain sweep must cover: every ledger row at `verified`, not merely the ones
        // with an MH_EXPORT_REPLACE (TACT1-P C4 -- tact is 98 verified against 11 promotable, and a
        // sweep over the 11 reports itself armed).
        topt.verified_tbl = mh::addr::verified_ranges;
        topt.verified_n   = mh::addr::verified_range_count;
        // X-SPINE (B): the section-5 residue, derived by gen_tombstones.py from the SAME
        // adjudication file the promotion reconciliation's `--check` reads. These `verified` rows
        // have their originals reachable from host code on purpose, so a force-arm must skip them --
        // otherwise the sweep's colour tracks the scenario rather than the claim (measured: three
        // scenarios green, `tact_panel` terminated on llm_map_fog_of_war_recompute via the
        // un-promoted planet load walker). Every skip prints its class; see tomb_options::residue.
        topt.residue       = mh::addr::tombstone_residue_exclusions;
        topt.residue_count = mh::addr::tombstone_residue_exclusion_count;
        mh::hook::tomb_install(mh::addr::tombstone_dead_ranges, mh::addr::tombstone_dead_range_count,
                               topt);
        mh::hook::tomb_report();
    }

    // U30 (c). ONE enumerated line naming every install_jmp/install_trampoline that was refused, and
    // an affirmative line when none was.
    //
    // WHY HERE AND NOT BESIDE report_interlock ABOVE, which is the obvious place and is wrong. That
    // line answers a question that is settled by its own call site: every promotion and every byte
    // patch has already happened, so it can state the whole picture. Detour refusals cannot -- more
    // than half of them happen AFTER it (lockstep_install_present_gameover, install_mp_bootstrap,
    // MH_Menu_Install, MH_Video_Install, the standalone/keyrepeat/pause/overlay/capture installs and
    // install_trace_hooks all run below it), and the refusal this whole item exists for is one of
    // them. Reporting before the thing you are reporting on is how U30 happened; doing it again one
    // layer up would be worse, because the line would look authoritative.
    //
    // The table itself starts filling in MH_Harness_Init, which runs BEFORE this function, so the
    // summary spans both inits even though the logger only exists from here.
    // LIB-ABI stage B: bind the host-callback table -- mh.dll's THIN IMPL, every entry a
    // mh::call:: thunk forwarder -- and state the unbound-walk. Zero unbound is by construction
    // (one generator emits both the struct and the fill), so this line exists to make a
    // generator gap or a version skew a visible red at arm time, not a null call mid-frame.
    {
        // SB-BIND T1: the state bind's evidence. The bind itself happened in MH_Harness_Init; this
        // is where the logger exists, so this is where it reports.
        //
        // THE VERDICT IS ON THE TWO COUNTERS bind_stock() SAMPLED AROUND ITSELF, not on a live
        // count taken here -- and that distinction is a correction, not a nicety. The first version
        // of this line asserted "relocated == 0" against the live registry and reported FAILED on
        // its first real run, because under the ship default [promote] ai=1 the AI island has
        // legitimately relocated 48 regions by the time this executes. The bind's own postcondition
        // is the thing worth checking, and it can only be checked where the bind happens.
        //
        //   before  must be 0 -- a region relocated BEFORE the host answered gets silently
        //                        un-relocated by the stock bind (and the AI island poisons the
        //                        .bss it left, so the symptom would be reading 0xCD).
        //   after   must be 0 -- the no-op property.
        //   moved   is REPORTED, not judged: it is the AI island doing its job.
        const int n_regions = (int)libmh_region_count();
        const int n_bound   = libmh_bound_count();
        const int before    = mh::state::moved_before_bind();
        const int after     = mh::state::moved_after_bind();
        int       n_moved   = 0;
        for (int i = 0; i < n_regions; ++i)
            if (mh::state::is_rebased((mh::state::region_id)i)) ++n_moved;
        char sb[256];
        // SB-HOSTFREE: WHICH POSTCONDITION APPLIES DEPENDS ON WHICH BIND RAN, and getting that
        // wrong is the same mistake this block already made once in the other direction. The stock
        // bind's postcondition is "nothing moved"; the RELOCATING bind's is the exact opposite, so
        // judging a relocating run by `after == 0` would print FAILED on the arrangement
        // SB-HOSTFREE exists to prove. What survives both is `n_bound == n_regions` -- a host
        // answers for every region whether or not it moves it -- so that is the shared clause, and
        // the moved-count clause is applied only on the stock path. The relocation's own
        // postconditions are asserted by [reloc], which knows what it asked for.
        const int  reloc = mh::state::relocated_count();
        const bool ok    = (n_bound == n_regions) && (reloc >= 0 || (before == 0 && after == 0));
        wsprintfA(sb,
                  "; [statebind] regions bound: %d/%d, moved at bind: before=%d after=%d, "
                  "relocated now %d (%s)%s\n",
                  n_bound, n_regions, before, after, n_moved,
                  reloc >= 0 ? "HOST RELOCATION + AI island" : "AI island", ok ? "" : "  <-- FAILED");
        seam_log(sb);
        // ---- THE HARNESS HAND-OFF (2 of 2) ----
        MH_Harness_ReportRelocation();
        // LIB-IFACE-SPLIT: two tables, two handshakes, two unbound-walks -- the report line
        // prints both counts, and either walk nonzero (or either rc nonzero) is a FAILED arm.
        const auto log_unbound = +[](const char *name) {
            char u[160];
            wsprintfA(u, "; [hostapi] UNBOUND entry: %s\n", name);
            seam_log(u);
        };
        const int bind_rc = libmh_set_host_api(&mh::hostapi::mhdll_table(), LIBMH_HOST_API_VERSION);
        const int unbound = libmh_host_api_unbound(log_unbound);
        const int tact_rc =
            libmh_set_tact_host_api(&mh::hostapi::mhdll_tact_table(), LIBMH_TACT_HOST_API_VERSION);
        const int tact_unbound = libmh_tact_host_api_unbound(log_unbound);
        char      m[224];
        wsprintfA(m,
                  "; [hostapi] host-callback tables bound: sim rc=%d %u entries unbound %d, "
                  "tact rc=%d %u entries unbound %d%s\n",
                  bind_rc, (unsigned)LIBMH_HOST_API_ENTRY_COUNT, unbound, tact_rc,
                  (unsigned)LIBMH_TACT_HOST_API_ENTRY_COUNT, tact_unbound,
                  (bind_rc == 0 && unbound == 0 && tact_rc == 0 && tact_unbound == 0)
                      ? ""
                      : "  <-- FAILED");
        seam_log(m);
        // LIB-REF-IN: the INBOUND surface's arm-time evidence, beside its outbound siblings and for
        // the same reason -- the open itself happened back in MH_Harness_Init (it has to: the
        // sim_hostreach adapters forward through these entries and refuse while closed), where no
        // logger existed. A closed surface here means every one of those 21 promoted leaves has
        // been a silent no-op, which is exactly the class of failure that must not be inferred from
        // a missing line. The walk's zero is the holey-table arm's positive control.
        {
            const int in_open    = libmh_in_is_open();
            const int in_unbound = libmh_in_unbound(log_unbound);
            wsprintfA(m,
                      "; [libmh_in] inbound surface: open=%d, version 0x%08X, %u entries + %u "
                      "order ids, unbound %d%s\n",
                      in_open, (unsigned)LIBMH_HOST_IN_VERSION, (unsigned)LIBMH_IN_ENTRY_COUNT,
                      (unsigned)LIBMH_ORD_COUNT, in_unbound,
                      (in_open == 1 && in_unbound == 0) ? "" : "  <-- FAILED");
            seam_log(m);
        }
        // F4D: the HOOK-SERVICE table's arm-time evidence, the last of the four and the one
        // F4D-PRE deliberately did not add. That item closed libmh's outbound edge onto this table
        // but could not report it, because the arm log is a gated artifact (check_arm_order) and
        // F4D-PRE's done_when said the baselines were untouchable there; F4D lands the line as a
        // ruled edit, beside the three siblings above.
        //
        // WHY IT MATTERS MORE AFTER THE SPLIT THAN BEFORE. Until F4D these five rows were a
        // same-image pointer assignment: if the table was bound at all, it was bound correctly.
        // Now libmh_set_hook_api is one of ~100 GetProcAddress rows, so "bound" is a question with
        // a real answer -- and an unbound hook table does not abort, by deliberate design
        // (libmh/state/hook_api.h: absence is a configuration, because a standalone libmh has no
        // harness). The failure mode is therefore SILENT: every [promote] installer refuses, the
        // game runs original bodies, and nothing says why. This line is what says why.
        //
        // Re-bound here rather than only read, exactly as [hostapi] re-binds above: the rc is half
        // the evidence, and a bind that returns non-zero at arm time would otherwise be invisible
        // (the real bind happened in MH_Core_Arm_Early, where there is no logger).
        //
        // AND IT CARRIES THE SPINE'S PRESENCE, which the three lines above do not need and this one
        // does. With no libmh.dll the generated thunks answer zero, so `rc=0 unbound=0` would read
        // as a perfectly healthy bind of a table that does not exist -- a log that lies in exactly
        // the configuration it is most needed in. The real answer for libmh_hook_api_unbound with
        // no table is -1 ("no table is bound at all"), and the generated stub cannot give it: zero
        // is correct for the other ~100 rows and this is not one of them. So the line names the
        // configuration instead, and FAILED is asserted only where a failure is possible.
        {
            const int  hook_rc      = MH_LibMH_BindHookApi();
            const int  hook_unbound = libmh_hook_api_unbound(log_unbound);
            const bool spine        = MH_LibmhModule_IsBound() != 0;
            wsprintfA(m,
                      "; [hookapi] hook-service table bound: rc=%d, %u entries, unbound %d, "
                      "spine=%s%s\n",
                      hook_rc, (unsigned)LIBMH_HOOK_API_ENTRY_COUNT, hook_unbound,
                      spine ? "bound" : "absent (configuration 1)",
                      (!spine || (hook_rc == 0 && hook_unbound == 0)) ? "" : "  <-- FAILED");
            seam_log(m);
        }
        // LIFT-EVQ: the event-channel sink's arm-time evidence -- re-bind is idempotent (same
        // routing fn), its rc catches a version skew, dispatched proves synchronous consumption
        // actually ran, unknown must stay 0 (an unroutable record is a contract breach).
        mh::hook::set_host_event_sink_logger(+[](const char *line) { seam_log(line); });
        const int evt_rc = mh::hook::bind_host_event_sink();
        wsprintfA(m, "; [hostevt] sink bound: rc=%d, version 0x%08X, dispatched %u, unknown %u%s\n",
                  evt_rc, (unsigned)LIBMH_HOST_EVENTS_VERSION,
                  (unsigned)mh::state::event_sink_dispatch_count(),
                  (unsigned)mh::hook::host_event_sink_unknown_count(),
                  (evt_rc == 0 && mh::hook::host_event_sink_unknown_count() == 0) ? ""
                                                                                  : "  <-- FAILED");
        seam_log(m);
        // LIB-REBIND R11: the rebind's arm-time evidence, beside its two siblings and for the same
        // reason -- the gates were LOADED back in MH_Core_Arm_Early (they were in MH_Harness_Init
        // until F3B; same statement, same place in the boot, a name that says whose work it is),
        // where no logger existed yet. This report is affirmative at zero ("armed 0 of N -- entirely
        // ORIGINAL"), because the ship default IS zero and a silently absent line is
        // indistinguishable from a report that was never reached. G106 is the whole argument: an
        // unreported arm set is how a 0.8% live set passed for a promoted closure.
        mh::rebind::report_arming(+[](const char *line) { seam_log(line); });
    }
    // U30 (c) closes the arm: every install in every arm above has happened by now, the net arm's
    // included -- which is why MH_Net_Arm is called from inside this function and not after it.
    mh::hook::report_entry_refusals();
    return g_armed ? 1 : 0;
}

// ---- MH_Seam_Init -- the exported entry point DllMain calls -------------------------------------
// Thin on purpose since F3B: the arm order is the contract, so it lives in one place (MH_Core_Arm,
// the spine above) rather than being re-stated here. The return value is unchanged -- non-zero iff
// the four transport seams armed -- and nothing reads it; DllMain ignores it.
extern "C" int MH_Seam_Init(void) { return MH_Core_Arm(); }
