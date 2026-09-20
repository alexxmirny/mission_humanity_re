//
// net_discovery.cpp -- MP session DISCOVERY/BROWSER/JOIN, split out of net_seams.cpp (2026-07
// refactor, Phase 4 stage 2). The retail menu->browser->lobby path is
// dead (discovery/connect/advertise are empty stubs, the session-list pointer is NULL); this TU
// holds the replacement: the synthetic session record + browser build (force-entry + manual S3),
// the host's real SESSION_INFO advertisement (S2), the client-side record store + JOIN/START/LEAVE
// control-frame handlers (S3/S4/U2/U12), and the dead-stub detour BODIES. The INSTALLS stay in
// net_seams.cpp (install_mp_bootstrap also arms the lobby detours -- one ordered install site);
// this TU exports the detour symbols + handlers through the shared spine (net_internal.h).
// See the MP bootstrap notes (MP restoration, Workstream U).
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>
#include <string.h>

#include "include/mh_net_export.h"
#include "include/mh_seam_export.h"      // MH_Seam_StartTransport (async connect kick)
#include "include/mh_chatinput_export.h" // MH_ChatInput_Codepage -- the pinned input codepage (F3)
#include "mh_net_proto/session_info.h"   // SESSION_INFO descriptor + lobby-id (S1/S2 discovery)
#include "mh_net_proto/uuid7.h"          // match_id: UUIDv7 mint + the logged 32-hex form (SES0)
#include "mh_session_id.h"               // the OS half of the mint: unix-ms + CSPRNG bytes (SES0)
#include "mh_version.h"                  // MH_VERSION_FULL -- the build stamp in SESSION_BEGIN (SES1)
#include "include/mh_net_module.h"       // MH_NET_MODULE_ABI -- the module version in SESSION_BEGIN
#include "addr/mh_addrs.gen.h"           // generated EN VAs (tools/gen_dll_addrs.py)
#include "net_internal.h"                // shared spine: g_a, g_ini, g_host_join_seen, seam_log, mp_client_slot
#include "seams/map_transfer.h"          // mp:X2 -- the map content claim, the request and the Start gate
#include "hook/watcall.h"                // call_watcall1 (Watcom __watcall(EAX) bridge)
#include "ui/lobby_ui.h"                 // mp:R4a -- browser_notice_arm_relay (the relay-level notice)

#pragma comment(lib, "user32.lib") // wsprintfA

using mh::hook::call_watcall1;

extern "C" int         MH_MP_IsManual(void);        // launch.cpp -- 1 = pure manual session (gate manual-only work)
extern "C" void        MH_MP_ResetHostMirror(void); // launch.cpp -- U13/U40 host: reset the peer-mirror one-shot edge
extern "C" void        MH_MP_ArmManualLobby(void);  // launch.cpp -- run the proven lobby driver for the manual path
extern "C" const char *MH_MP_PeerName(int i);       // defined below; the SES1 roster builder runs ahead of it

namespace {

// mh.exe fixed VAs (generated EN header; image base 0x00400000, no ASLR).
constexpr uintptr_t ADDR_SESSION_LIST_PTR = mh::addr::_G_LLM_NET_SESSION_LIST;  // llm_net_session_desc*; NULL at rest
constexpr uintptr_t ADDR_SESSION_COUNT    = mh::addr::_G_LLM_NET_SESSION_COUNT; // uint
constexpr uintptr_t ADDR_UI_ROW_ARRAY     = mh::addr::browser_ui_rows;          // {void* handle, u32 adapter}[]
constexpr uintptr_t ADDR_PROTO_CEIL       = mh::addr::proto_version_ceiling;    // retail=1
constexpr uintptr_t ADDR_READMAPFILE      = mh::addr::cfg_ReadMapFile;          // map_header* in EAX -- header load
constexpr uintptr_t ADDR_CUR_MAP          = mh::addr::current_map_data;         // map_header, 0x17c
constexpr uintptr_t ADDR_NET_LOCALIDX     = mh::addr::_G_LLM_NET_LOCAL_PLAYER_INDEX;
constexpr uintptr_t ADDR_NET_PLAYERSIDE   = mh::addr::PlayerSide;                     // client drives its own slot
constexpr uintptr_t ADDR_MAP_RECV_DONE    = mh::addr::_G_LLM_NET_LOBBY_MAP_RECV_DONE; // client map-recv-complete flag
// llm_net_session_desc field offsets (stride 0x400); map_header field offsets
constexpr int SD_HANDLE = 0x00, SD_NAME = 0x04, SD_TOTAL = 0x24, SD_MAX = 0x28, SD_STATE = 0x30,
              SD_PCOUNT = 0x31, SD_MAPHDR = 0x32, SD_PROTO = 0x25e, MAP_DATA1_SIZE = 0x17c;
constexpr int MD_PATH = 0x18, MD_MAPNAME = 0xfc, MD_TLONAME = 0x11c;
const char    MP_BOOT_MAP_PATH[] = "Dane\\";
const char    MP_BOOT_MAP_NAME[] = "TUTORIAL.MP"; // fixed 2p PoC map (identical bytes on both peers => same seed)

// N1: the configured MP map ([net] mp_map) + its load path. The verb-lobby client's map comes from the
// SYNTH record (its join handler copies the record's map_header into current_map_data) -- so for a >2-player
// game the synth record must carry the SAME >=N-start map the host loads, else the client lands on the 2p
// boot map and desyncs. A .mpm loads loose from Maps\; a .MP campaign map from Dane\. Default = the boot map.
void mp_configured_map(char *name, int cap, const char **path_out) {
    GetPrivateProfileStringA("net", "mp_map", MP_BOOT_MAP_NAME, name, cap, g_ini);
    int n     = lstrlenA(name);
    *path_out = (n >= 4 && lstrcmpiA(name + n - 4, ".mpm") == 0) ? "Maps\\" : MP_BOOT_MAP_PATH;
}

unsigned char g_synth_desc[0x400]; // one synthetic llm_net_session_desc (client-side)
bool          g_synth_built = false;

// S3 / S5-core (client-side real discovery). LAN has ONE host (the typed IP), so the "session source"
// store is a single received SESSION_INFO (relay's multi-record store = R-src). Fed by
// on_session_info_recv (recv thread), consumed by build_browser_from_store (main thread) -- publish via
// the volatile flag AFTER the struct write (x86 TSO makes that ordering safe here).
mh_net_proto::SessionInfo g_store_rec{};
volatile LONG             g_store_valid    = 0; // 1 once a real SESSION_INFO has been received
volatile LONG             g_host_left_seen = 0; // U13: client saw the host leave its lobby (retail 0x0e)
// U23: WHY that lobby exit happened. g_host_left_seen answers "was this involuntary"; it cannot answer
// "was the host gone or was the wire gone", because R-live-ui deliberately routes BOTH through one
// synthesised 0x0e -> MH_MP_ClientOnHostLeft (a dead host and a departed host converge on ONE tested
// transition). So the cause rides alongside as a second flag rather than by forking that transition.
// HOST_LEFT is the DEFAULT (set inside ClientOnHostLeft), so a route that forgets to mark itself still
// says something true of every caller; only the link-lost synthesis site overrides it.
// ================= mp:R2 -- the RELAY SESSION DIRECTORY (client side) =============================
//
// S3's store above is a LAN store and it is deliberately one record: there is one host, the player
// typed its address, and the record arrives over the connection that address opened. A relay is
// the other shape -- MANY hosts are reachable through ONE address, and a browser has to see them
// BEFORE it is connected to any of them. So the relay keeps a directory of the descriptors its
// hosts registered (`src/relay`, ops 8..11) and hands a browsing peer the whole set.
//
// HOW A ROW GETS HERE. Through `on_session_info_recv`, the same handler a connected host's advert
// uses, because a directory row IS a SESSION_INFO -- the host registered the bytes verbatim and
// the relay stored them without looking inside. What tells the two apart is the `sender`:
// mh_net_proto's `session_sender_is_relay` (a value the transport ABI already had room for, and
// the reason R2 adds no module export). The room rides in the same field, because dialling that
// room is what joining a listed lobby MEANS.
//
// THREADING. The rows arrive on the transport module's tunnel thread and are read by the menu
// tick on the main thread, so they cross a thread boundary as a single-producer/single-consumer
// INBOX of whole records rather than as a locked table: the producer only ever writes a slot the
// consumer is not reading, and `g_relay_in_head` published after the write is the fence (x86 TSO,
// the same reasoning `g_store_valid` above rests on). The ROW TABLE itself is main-thread-only.
constexpr int   RELAY_ROW_MAX = 8;    // as many lobbies as we will track through one relay
constexpr int   RELAY_INBOX   = 16;   // SPSC ring; a full ring drops, it never blocks the tunnel
constexpr DWORD RELAY_ROW_TTL = 8000; // a row not re-listed this long is stale (the backstop)
constexpr DWORD RELAY_NO_ROOM = 0;    // "no listed lobby picked" -- room 0 is the directory room

struct RelayInbox {
    uint32_t                  room;
    int                       len;
    mh_net_proto::SessionInfo si;
    bool                      empty; // the relay answered and has NOTHING registered
    // WHEN THE RELAY SAID IT, not when we got round to reading it. The drain only runs while the
    // browser is up (see MH_Seam_ClientDiscoveryTick), so an answer can sit here for a whole match;
    // stamping the row at DRAIN time would hand a ten-minute-old lobby a fresh TTL and put a dead
    // game back on the browser for RELAY_ROW_TTL.
    DWORD at;
};
RelayInbox    g_relay_in[RELAY_INBOX];
volatile LONG g_relay_in_head = 0; // producer (tunnel thread)
volatile LONG g_relay_in_tail = 0; // consumer (main thread)

struct RelayRow {
    mh_net_proto::SessionInfo si;
    uint32_t                  room;
    DWORD                     last_seen;
    bool                      used;
};
RelayRow g_relay_rows[RELAY_ROW_MAX];      // main thread only
int      g_relay_pick     = -1;            // the row the browser shows and a join would dial
uint32_t g_relay_dialled  = RELAY_NO_ROOM; // the room the last connect kick asked for
bool     g_relay_room_set = false;         // ...and whether that latch has been initialised

// Defined with build_browser_from_store, which is the other end of the same mechanism; declared
// here because s3_kick_connect (above it in the file) is the one place that latches the room.
bool                             relay_configured();
uint32_t                         relay_default_room();
bool                             relay_rows_drain();
const mh_net_proto::SessionInfo *relay_picked_rec();
bool                             relay_force();      // mp:R7a -- `[net] force_relay` is set (pins the relayed path)
bool                             dial_wants_relay(); // mp:R7a -- is THIS client dial relayed or direct?

volatile LONG g_exit_cause = 0; // 0 = none, 1 = MH_MP_EXIT_HOST_LEFT, 2 = MH_MP_EXIT_LINK_LOST, 3 = JOIN_REFUSED (F3c)
// mp:F3c -- a REFUSED reply to our JOIN (a FLAG_ANNOUNCE of kind REFUSED addressed to our player id).
// Written on the recv thread (reason first, then the flag -- x86 TSO), consumed on the main thread by
// MH_Seam_PollRecv, which bounces the client out of the lobby it seated itself in through the SAME
// synthesised-0x0e transition a dead host uses. Cleared by the next JOIN we send, so a stale refusal
// from an earlier lobby can never bounce a fresh join.
volatile LONG g_join_refused                                         = 0;
char          g_join_refused_reason[mh_net_proto::ANNOUNCE_TEXT_CAP] = {0};
// mp:R6 -- a JOIN the player clicked on a RELAY-LISTED row before the link into that lobby's room
// was up. The transport drops a ctrl frame with no peer to carry it, and R2 left that as "click
// Join again" -- which was a rare race while a host's room was its port (the browsing dial usually
// landed on the host directly) and is the ORDINARY shape since R6: a host's room is minted, so a
// client's first dial never lands, the row arrives from the directory, the re-dial into its room
// starts at that same moment, and a click inside the handshake window (~0.2-0.5 s) was lost --
// with the retail lobby screen already pushed, so there was nothing left to click again (measured
// 2026-09-19: relay_browse_local's client sat in a lobby at players=1/8 until the timeout). So the
// click is REMEMBERED with the lobby it named, and the moment the connected host's advert arrives
// (the S3 record) the JOIN is sent again -- for THAT lobby only; a different host answering the
// re-dial drops the pending click rather than joining something the player never chose.
volatile LONG             g_join_pending      = 0;
uint32_t                  g_join_pending_room = 0; // the row's room -- what the re-dial is asking for
mh_net_proto::SessionInfo g_join_pending_rec;
bool                      g_join_resending = false; // the re-send must not re-arm itself
volatile LONG             g_s3_conn_kicked = 0;     // 1 once we've kicked the connect to the typed host
// mp:R7 -- 1 while the transport was started by a RELAY BROWSE dial (a client-role dial the first
// browser makes to list the directory). Consumed by on_host_advertise: a player who clicks Create
// game after browsing needs a HOST transport, not the client one the browse left running.
volatile LONG g_r7_browse_dialled = 0;
volatile LONG g_s3_conn_done      = 0; // S8: 1 once the kicked connect thread FINISHED (ok or failed)
volatile LONG g_s3_conn_done_at   = 0; // R7: GetTickCount() at that finish (the relayed idle re-dial clock)
// mp:R7a -- the kick site's per-dial decision, latched here so lazy_start (net_seams.cpp) reads the
// SAME value the log named. -1 = no manual dial decided yet (force-entry defaults to relay-if-configured);
// 0 = this dial is DIRECT (Internet server + typed IP); 1 = RELAYED (first browser / a relay-directory row).
volatile LONG g_dial_relayed   = -1;
volatile LONG g_s8_retry_armed = 0; // S8(b): 1 once a FAILED connect cleared the latches (retry-ready)
volatile LONG g_s3_listed      = 0; // 1 once we've re-armed the browser to show the received record
// mh.exe menu one-shot deferred-callback slot + the browser's registered rescan fn (Explore trace 2026-07-13):
// llm_ui_menu_state_tick fires PTR_FUNC_UNKNOWN once then clears it; browser_enter stores the rescan in
// INT_0065076f. Re-arming = copy the rescan back into the one-shot so the poll re-fires next frame.
constexpr uintptr_t ADDR_MENU_ONESHOT    = mh::addr::menu_oneshot_cb;         // PTR_FUNC_UNKNOWN slot
constexpr uintptr_t ADDR_MENU_REFRESH_FN = mh::addr::browser_refresh_fn_slot; // browser rescan fn

void bs_str_copy(char *dst, const char *src, int cap) {
    int i = 0;
    for (; i < cap - 1 && src[i]; ++i) dst[i] = src[i];
    dst[i] = 0;
}

// A valid placeholder map header (0x17c) loaded ONCE from the local boot .MP. Used by both the synth
// record and the S3 real-record browser row so neither ships raw/garbage current_map_data as a header
// (llm_lobby_join_handler copies the record's map_header into current_map_data at join -> garbage there
// crashed the client; the host's real map then overwrites it, but the header must be valid meanwhile).
unsigned char g_map_hdr_placeholder[MAP_DATA1_SIZE];
bool          g_map_hdr_ready = false;
void          ensure_placeholder_maphdr() {
    if (g_map_hdr_ready) return;
    char       *cm = (char *)ADDR_CUR_MAP;
    char        mname[64];
    const char *mpath;
    mp_configured_map(mname, sizeof(mname), &mpath); // N1: mp_map, not fixed boot map
    bs_str_copy(cm + MD_PATH, mpath, 128);
    bs_str_copy(cm + MD_MAPNAME, mname, 32);
    cm[MD_TLONAME] = 0;
    call_watcall1(ADDR_READMAPFILE, cm); // fill header (player count, seed byte, tlo) from disk
    memcpy(g_map_hdr_placeholder, cm, MAP_DATA1_SIZE);
    g_map_hdr_ready = true;
}

// (Re)point the game's session-list pointer + count at our one synthetic record each poll (a rescan
// zeroes the count); build the record ONCE from the local .MP header. Reuses the proven force-entry
// ReadMapFile-on-current_map_data sequence, then copies the 0x17c header into the record. JOIN later
// copies it back into current_map_data, so this is self-consistent.
void build_synth_session() {
    *(void **)ADDR_SESSION_LIST_PTR = g_synth_desc; // NULL at rest -> our static buffer
    *(int *)ADDR_SESSION_COUNT      = 1;
    // Seed browser UI-row 0 with the synth handle every poll. The rescan browser (FUN_004bd60d) builds
    // the VISIBLE list (DAT_0065014f) but NOT this handle array (only FUN_004bc6ba does) -- yet
    // llm_lobby_join_head matches session_handle == DAT_00e622a0[selected]. Without this the manual
    // browser-join always "no-matches" (text 0x310). Mirrors the proven force-entry mp_join_lobby.
    *(void **)ADDR_UI_ROW_ARRAY     = (void *)1; // row 0 handle = synth sentinel (== SD_HANDLE)
    *(int *)(ADDR_UI_ROW_ARRAY + 4) = 0;         // row 0 adapter idx
    if (g_synth_built) return;
    memset(g_synth_desc, 0, sizeof(g_synth_desc));
    char       *cm = (char *)ADDR_CUR_MAP;
    char        mname[64];
    const char *mpath;
    mp_configured_map(mname, sizeof(mname), &mpath); // N1: mp_map, not fixed boot map
    bs_str_copy(cm + MD_PATH, mpath, 128);
    bs_str_copy(cm + MD_MAPNAME, mname, 32);
    cm[MD_TLONAME] = 0;
    call_watcall1(ADDR_READMAPFILE, cm);                  // fill header (player count, seed byte, tlo) from disk
    memcpy(g_synth_desc + SD_MAPHDR, cm, MAP_DATA1_SIZE); // header -> record.map_header
    *(void **)(g_synth_desc + SD_HANDLE) = (void *)1;     // stable sentinel handle (matched by == on join)
    bs_str_copy((char *)(g_synth_desc + SD_NAME), "MH Host", 32);
    // S7: the row format is patched to u"%s\t%d/%d" -> "(SD_TOTAL - SD_PCOUNT)/SD_PCOUNT" = occ/cap.
    // Synth placeholder = host present (occ 1) on an unclosed map (cap = map player count from the header).
    int synth_cap = *(const int *)(cm + 0x08); // current_map_data player-slot count (just loaded)
    if (synth_cap < 1) synth_cap = 2;
    if (synth_cap > 8) synth_cap = 8;
    *(int *)(g_synth_desc + SD_TOTAL) = 1 + synth_cap;                 // occ(1) + cap
    *(int *)(g_synth_desc + SD_MAX)   = synth_cap;                     // (unused by the 2-arg format)
    g_synth_desc[SD_STATE]            = 0;                             // 0 = joinable (NORMAL branch + JOIN gate)
    g_synth_desc[SD_PCOUNT]           = (unsigned char)synth_cap;      // second field = cap
    *(int *)(g_synth_desc + SD_PROTO) = *(const int *)ADDR_PROTO_CEIL; // == ceiling -> passes the <= list gate
    g_synth_built                     = true;
    seam_log("; MP bootstrap: synthetic session record built from local .MP (browser lists 1 host)\n");
}

// ---- S2: host self-description (advertise the real SESSION_INFO) --------------------------------
// Build the host's own SessionInfo from live game state + a per-session random tag and broadcast it as a
// FLAG_SESSION_INFO control frame, so clients can list the REAL game (name/tag/map/players) instead of the
// fabricated "MH Host". Manual-host only + throttled; the control frame is routed to a handler, never to
// the game queue, so it can't perturb the lockstep. Tag uses a NON-SIM RNG (never the lockstep seed).
constexpr uintptr_t ADDR_GAME_NAME      = mh::addr::mp_game_name;       // host-typed create name (unnamed in Ghidra)
constexpr uintptr_t ADDR_MAP_NAME       = ADDR_CUR_MAP + MD_MAPNAME;    // current_map_data.map_name (0x006550e3)
constexpr uintptr_t ADDR_MAP_PCOUNT     = ADDR_CUR_MAP + 0x08;          // current_map_data player-slot count (build_players bound)
constexpr uintptr_t ADDR_MP_LOBBY_SLOTS = mh::addr::_G_LLM_LOBBY_SLOTS; // host + AI + humans
constexpr int       MP_SLOT_STRIDE      = 0x39;
constexpr int       MP_SLOT_STATUS      = 0x0b; // slot_status byte (0 = empty)
constexpr uint16_t  MH_MP_HOST_VERSION  = 2;    // bump on a wire/behaviour change (2 = SES0's match_id)

// slot_status enum (llm_lobby_player_slot.slot_status, offset 0x0b): OPEN=0, HUMAN=1, AI=2, CLOSED=3.
enum { SLOT_OPEN   = 0,
       SLOT_HUMAN  = 1,
       SLOT_AI     = 2,
       SLOT_CLOSED = 3 };

// S7: derive the browser count fields over the map's REAL slots [0, mapmax).
//   *occ = OCCUPIED slots (HUMAN + AI) -- "how full the game is", AI included.
//   *cap = CAPACITY after closed slots = slots that are NOT closed (OPEN + HUMAN + AI).
// (The old mp_lobby_occupied counted status != 0, which mis-counted a CLOSED slot as occupied.)
void mp_lobby_occ_cap(int mapmax, int *occ, int *cap) {
    int o = 0, c = 0;
    if (mapmax < 1) mapmax = 1;
    if (mapmax > 8) mapmax = 8; // the physical lobby-slot array is fixed at 8
    for (int i = 0; i < mapmax; ++i) {
        unsigned char s = *(const unsigned char *)(ADDR_MP_LOBBY_SLOTS + i * MP_SLOT_STRIDE + MP_SLOT_STATUS);
        if (s == SLOT_HUMAN || s == SLOT_AI) {
            ++o;
            ++c;
        } else if (s == SLOT_OPEN) {
            ++c;
        } // CLOSED contributes to neither
    }
    *occ = o;
    *cap = c;
}

uint32_t g_mp_tag = 0; // this host's per-session lobby-id tag (0 = not yet generated)

// SES0: this host's MACHINE session id -- a UUIDv7, minted beside the tag and re-minted with it when
// the host leaves and re-creates (MH_MP_HostResetSessionIdentity). All-zero = not yet minted, the same
// sentinel g_mp_tag==0 uses one line up. The tag stays the human lobby label; this is what makes two
// machines' logs one match.
uint8_t g_mp_match_id[mh_net_proto::UUID7_BYTES] = {0};

// The last match_id THIS peer wrote a `; [session] match_id=` line for. The line is emitted ONCE per
// distinct id per peer -- the host advertises at ~1 Hz and the client stores every advert, so an
// unguarded log would repeat the same id for the length of the lobby and bury the re-create case.
uint8_t g_mp_logged_match_id[mh_net_proto::UUID7_BYTES] = {0};

uint32_t mp_gen_tag() {
    // Non-sim RNG: wall clock + process/thread ids, xorshift-mixed. Never touches the lockstep RNG/seed.
    uint32_t s = GetTickCount() ^ (GetCurrentProcessId() << 16) ^ GetCurrentThreadId();
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s ? s : 0xA5A5A5A5u; // avoid the 0 "not generated" sentinel
}

// SES0: write `; [session] match_id=<32 hex>` to mh_net.log, once per distinct id. tools/mp_analyze.py
// parses this line to pair the peers' logs, so the text comes from mh_net_proto (one spelling) rather
// than from a wsprintfA here.
void mp_log_match_id(const uint8_t *id) {
    if (mh_net_proto::uuid7_is_nil(id)) return;                                      // nothing to correlate by
    if (memcmp(id, g_mp_logged_match_id, sizeof(g_mp_logged_match_id)) == 0) return; // already said
    memcpy(g_mp_logged_match_id, id, sizeof(g_mp_logged_match_id));
    char line[mh_net_proto::SESSION_LOG_LINE_CAP];
    seam_log(mh_net_proto::session_match_id_log_line(id, line, sizeof(line)));
}

} // namespace

// ================= SES1: the per-session run directory ============================================
//
// SES0 gave a match an IDENTITY; this is where that identity becomes a PLACE. A session opens when
// this peer commits to a lobby -- the host at the instant it mints its lobby id (mp_build_host_session
// _info's mint branch IS "the lobby was created"), the client at the instant it sends its JOIN -- and
// closes at whichever exit seam fires first: game over, either side leaving the lobby, a peer timing
// out, or quit-to-menu.
//
// WHY THE OPEN LIVES HERE AND NOT IN run_context.cpp: "when does a match begin" is a game question,
// and mh_common must not answer it. run_context owns the consequence (which directory the next log
// line goes into) and nothing else. The split is the same one SES0 drew through the match_id itself.
//
// EVERY EXIT PATH CALLS mp_session_close, AND ONLY THE FIRST ONE DOES ANYTHING. A conquest ends with
// on_gameover and then a quit-to-menu; a host-left ends with the client's finalize and then possibly
// a timeout kick. Making close idempotent is cheaper than reasoning about which seam wins the race,
// and the state machine (mh_session_dir.h) is where that idempotence is proven offline.
namespace {

MH_SessionRecord g_session_rec;

// [net] transport name for the record. One transport today; T1 adds a second, and a report that does
// not say which one carried the match is a report that cannot explain the loss pattern in it.
const char *session_transport() { return "tcp"; }

// The lobby roster as "<slot>:<kind><name>", comma-separated -- kind is H(uman)/A(I), and OPEN and
// CLOSED slots are simply absent (a roster is who is IN the match). Names are only known host-side
// (they arrive in each peer's JOIN); a client records the kinds and its own name, which is still
// enough to answer "how many players, of which kind" when the two peers' records are read together.
// NOT THE LOBBY SLOT ARRAY ALONE. The obvious implementation -- walk _G_LLM_LOBBY_SLOTS and take
// every HUMAN/AI status byte -- produced `roster=""` on every peer of SES1's own three-match
// acceptance run, with two players visibly seated on both screens. That reader is the same one
// mp_lobby_occ_cap uses, and its answer is visible in the S2 advert line beside it: `players=1/8`,
// i.e. occ=0 falling back to 1, for the whole lobby. The status bytes are simply not populated on
// this path at the moments a session opens and closes.
//
// So the slot array is ONE source, not the only one. A slot is in the roster if the array says so,
// OR if a peer sent a JOIN naming it (g_peer_player_name, which is how the host learns names at
// all), OR if it is our own. That makes the field say something true on both peers instead of
// nothing on either, and it degrades to the array's answer wherever the array is right.
void session_roster(char *dst, int cap) {
    int       at   = 0;
    const int mine = (g_a.local_player_index != nullptr) ? *g_a.local_player_index : -1;
    dst[0]         = '\0';
    for (int i = 0; i < 8; ++i) {
        unsigned char s       = *(const unsigned char *)(ADDR_MP_LOBBY_SLOTS + i * MP_SLOT_STRIDE + MP_SLOT_STATUS);
        const char   *nm      = MH_MP_PeerName(i);
        const bool    named   = (nm != nullptr && nm[0] != '\0');
        const bool    is_mine = (i == mine);
        if (s != SLOT_HUMAN && s != SLOT_AI && !named && !is_mine) continue;
        if (at > 0) at = mh_sd_put(dst, cap, at, ",");
        at = mh_sd_put_int(dst, cap, at, i);
        at = mh_sd_put(dst, cap, at, s == SLOT_AI ? ":A" : ":H");
        if (named) at = mh_sd_put(dst, cap, at, nm);
        else if (is_mine) at = mh_sd_put(dst, cap, at, (const char *)mh::addr::mp_player_name);
    }
}

// session.json, rewritten whole at both ends of the session. CREATE_ALWAYS rather than append: the
// file is a SNAPSHOT of the record, and a half-open session that the process never closed still
// leaves a readable one naming the match -- which is the case a crash report is made of.
void session_json_write() {
    char path[MAX_PATH];
    wsprintfA(path, "%ssession.json", MH_RunDir());
    char   text[2048];
    int    n = mh_session_json(&g_session_rec, text, (int)sizeof(text));
    HANDLE h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(h, text, (DWORD)n, &w, nullptr);
    CloseHandle(h);
}

} // namespace

// Fill the END-only half of the record from the pacing/lockstep TU (which owns the counters).
extern "C" void MH_Seam_SessionPacing(long *clock_ms, long *stall, long *icon_calls, long *icon_shown,
                                      int *step_ms, int *sim_step_ms); // net_lockstep.cpp

void mp_session_open(const unsigned char *match_id, int slot) {
    if (mh_net_proto::uuid7_is_nil(match_id)) return; // no id -> no session to name
    char hex[mh_net_proto::UUID7_HEX_CAP];
    mh_net_proto::uuid7_hex(match_id, hex, sizeof(hex));
    if (MH_RunDir_SessionActive()) {
        if (lstrcmpA(MH_RunDir_SessionMatchId(), hex) == 0) return; // the same lobby re-asserting itself
        // A DIFFERENT id with one still open: the previous match's exit seam never fired (a host that
        // re-created its lobby from a state we did not observe). Close it FIRST, so its SESSION_END
        // lands in ITS directory rather than in the new one.
        mp_session_close("rolled");
    }
    if (!(MH_RunDir_SessionBegin(hex, slot) & MH_SESSION_OPENED)) {
        seam_log("; [session] SESSION_BEGIN skipped -- the session directory could not be created; "
                 "output stays in the process folder (no logs are lost)\n");
        return;
    }
    mh_session_record_clear(&g_session_rec);
    mh_sd_copy(g_session_rec.match_id, MH_SESSION_MATCH_HEX_CAP, hex);
    g_session_rec.slot = slot;
    mh_sd_copy(g_session_rec.role, MH_SESSION_TEXT_CAP, MH_RunRole());
    mh_sd_copy(g_session_rec.build, MH_SESSION_TEXT_CAP, MH_VERSION_FULL);
    {
        char mods[MH_SESSION_TEXT_CAP];
        wsprintfA(mods, "net_abi=%08X/started=%d", (unsigned)MH_NET_MODULE_ABI, MH_Net_IsStarted() ? 1 : 0);
        mh_sd_copy(g_session_rec.modules, MH_SESSION_TEXT_CAP, mods);
    }
    lstrcpynA(g_session_rec.map, (const char *)ADDR_MAP_NAME, MH_SESSION_TEXT_CAP);
    g_session_rec.map_hash = mh_session_hash((const void *)ADDR_CUR_MAP, mh_net_proto::MAP_HEADER_SIZE);
    session_roster(g_session_rec.roster, MH_SESSION_ROSTER_CAP);
    MH_Seam_SessionPacing(nullptr, nullptr, nullptr, nullptr, &g_session_rec.lockstep_step_ms,
                          &g_session_rec.sim_step_ms);
    mh_sd_copy(g_session_rec.transport, MH_SESSION_TEXT_CAP, session_transport());
    MH_RunDir_UtcStamp(g_session_rec.began_utc, MH_SESSION_STAMP_CAP);
    mh_sd_copy(g_session_rec.process_dir, MH_SESSION_DIRNAME_CAP, MH_ProcessDirLeaf());

    char line[MH_SESSION_LINE_CAP];
    mh_session_begin_line(&g_session_rec, line, (int)sizeof(line));
    seam_log(line);
    session_json_write();
}

// U40: the session-boundary half of the re-host fix. A session closing for a MATCH-end reason means
// this peer's transport link has done its job -- and on 2026-09-01 it had literally died with the
// match (both conns reset at teardown, mirrored on every peer) while nothing existed to repair it.
// Both roles return to their PRE-LOBBY state here, and the two roles are asymmetric because the
// topology is: the host LISTENS (a retired conn already IS its pre-lobby state, so only its session
// IDENTITY has to be re-minted), the client DIALS (so it must be allowed to dial again).
//
// SCOPED TO THE MATCH-END REASONS. "leave" / "host_left" / "link_lost" / "rolled" are lobby-level
// exits, and U13's proven flow keeps the client's link across one (the re-created lobby's advert
// arrives over it -- that is what host_recreate tests). A MATCH ending is the boundary where the
// relationship really is over on both sides, so it is the one that resets.
namespace {

const char *const MATCH_END_REASONS[] = {"gameover", "quit", "timeout"};

bool is_match_end(const char *reason) {
    if (!reason) return false;
    for (const char *r : MATCH_END_REASONS)
        if (lstrcmpA(reason, r) == 0) return true;
    return false;
}

// Forget everything the client learned about the host it was playing: the stored advert (so the
// browser lists no ghost of the finished match) and the S3 one-shots that would otherwise refuse a
// second connect for the life of the process.
void client_relink_arm() {
    InterlockedExchange(&g_store_valid, 0);
    InterlockedExchange(&g_join_pending, 0); // mp:R6: the match is over; no click outlives it
    InterlockedExchange(&g_s3_conn_kicked, 0);
    InterlockedExchange(&g_s3_conn_done, 0);
    InterlockedExchange(&g_s3_listed, 0);
    InterlockedExchange(&g_net_relink, 1); // consumed by lazy_start, the only re-entrant InitEx site
    MH_Seam_ResetTransportInit();          // ...which is latched until this clears g_tried_init
    seam_log("; U40: match over -> client link released; the next browser refresh re-dials the host\n");
}

// Defined beside MH_MP_HostResetSessionIdentity (it touches the per-peer arrays declared below);
// forward-declared here because the session boundary is the OTHER caller.
void host_reset_identity();

} // namespace

void mp_session_close(const char *reason) {
    if (!MH_RunDir_SessionActive()) return; // idempotent: only the first exit seam does the work
    // F3c: the session's codepage was the host's; ours comes back with the session's end. Idempotent
    // and a no-op on the host (which never adopts), so it sits on the one funnel every exit uses.
    MH_ChatInput_RestoreCodepage();
    // The per-peer download bookkeeping and the host's content claim belong to the lobby that just
    // ended. The OPEN REDIRECT deliberately survives it -- see session_reset().
    mh::seams::maps::session_reset();
    mh_sd_copy(g_session_rec.reason, MH_SESSION_TEXT_CAP, (reason && reason[0]) ? reason : "unknown");
    MH_RunDir_UtcStamp(g_session_rec.ended_utc, MH_SESSION_STAMP_CAP);
    // RE-SAMPLE THE ROSTER. At the OPEN it is usually empty and that is not a bug: the host mints its
    // lobby id in the same breath as it creates the lobby, a frame or two before the retail code
    // populates even its own slot (measured -- the first session.json of every host run read
    // `roster=""`). The close is the only moment the roster is both complete and still standing, so
    // the CLOSING sample is the one worth keeping. The BEGIN line keeps whatever was known then --
    // it is a record of what the peer could see at the open, not a promise about the match.
    session_roster(g_session_rec.roster, MH_SESSION_ROSTER_CAP);
    MH_Seam_SessionPacing(&g_session_rec.final_clock_ms, &g_session_rec.stall_count,
                          &g_session_rec.icon_calls, &g_session_rec.icon_shown, nullptr, nullptr);
    char line[MH_SESSION_LINE_CAP];
    mh_session_end_line(&g_session_rec, line, (int)sizeof(line));
    seam_log(line);       // still the SESSION directory -- the switch is the next statement
    session_json_write(); // ...and so is this
    MH_RunDir_SessionEnd();

    // U40: ...and now the transport/identity half of the same boundary. AFTER SESSION_END, so its
    // own log lines belong to the run folder the next match will use, not to the one just closed.
    // Manual path only: the force-entry/harness path owns its own transport and never re-hosts.
    if (!is_match_end(reason) || !MH_MP_IsManual() || !g_a.is_host) return;
    if (*g_a.is_host) {
        // HOST. Its listener never went anywhere, so "pre-lobby" for a host is an IDENTITY question:
        // without this, a game re-created after a match reuses the finished match's tag AND its
        // match_id -- two matches sharing one id, which is exactly what SES0/SES1 exist to prevent.
        host_reset_identity();
        MH_MP_ResetHostMirror(); // U13: a re-JOIN re-seats the peer (the was_member edge is one-shot)
        seam_log("; U40: match over -> host session identity released (the next Create is a NEW lobby)\n");
    } else {
        client_relink_arm();
    }
}

namespace {

void mp_build_host_session_info(mh_net_proto::SessionInfo &si) {
    if (g_mp_tag == 0) {
        g_mp_tag = mp_gen_tag();
        // Mint the match_id in the same breath as the tag: "lobby created" is exactly this branch,
        // and a second mint point would be a second answer to when a match begins. The OS supplies
        // the clock and the entropy (mh_session_id.h); mh_net_proto does the RFC 9562 assembly.
        uint8_t rnd[mh_net_proto::UUID7_RAND_MAX];
        mh_session_random(rnd, (int)sizeof(rnd));
        mh_net_proto::uuid7_make(mh_session_unix_ms(), rnd, g_mp_match_id);
        mp_log_match_id(g_mp_match_id);
        // SES1: this branch IS "the lobby was created" -- the same reasoning that put the mint here.
        // Open the session directory in the same breath, so every line from here on (including the
        // S2 advert line this function's caller is about to write) belongs to the match. The host is
        // always slot 0; its local player index is authoritative but not yet populated at create.
        mp_session_open(g_mp_match_id, 0);
    }
    si     = mh_net_proto::SessionInfo{};
    si.tag = g_mp_tag;
    memcpy(si.match_id, g_mp_match_id, sizeof(si.match_id)); // SES0: advertise it every tick
    si.host_version = MH_MP_HOST_VERSION;
    // F3: advertise the input codepage this peer has pinned, every tick, beside the identity. It is
    // resolved rather than stored: MH_ChatInput_Codepage() answers GetACP() when the seam never armed,
    // so "no [input] codepage line in the ini" and "pinned to the machine default" are the same claim
    // on the wire -- which is what makes two default machines with DIFFERENT ACPs refuse each other
    // instead of silently disagreeing about what a chat byte means.
    si.codepage = (uint16_t)MH_ChatInput_Codepage();
    si.protocol = (uint16_t)*(const int *)ADDR_PROTO_CEIL;
    int mapmax  = *(const int *)ADDR_MAP_PCOUNT; // the map's own player capacity (2 for a 2-player map)
    int occ = 0, cap = 0;
    mp_lobby_occ_cap(mapmax, &occ, &cap); // occ = HUMAN+AI; cap = non-closed slots (S7)
    // cur_players = OCCUPANCY incl AI (fall back to 1 = host before its own slot populates).
    si.cur_players = (uint8_t)(occ ? occ : 1);
    // max_players = CAPACITY after excluding closed slots (fall back to the raw map max if the lobby
    // slots haven't populated yet, so the row never reads a nonsense "occ/0").
    if (cap < 1) { cap = mapmax < 1 ? 1 : (mapmax > 255 ? 255 : mapmax); }
    si.max_players = (uint8_t)cap;
    lstrcpynA(si.name, (const char *)ADDR_GAME_NAME, sizeof(si.name));
    lstrcpynA(si.map, (const char *)ADDR_MAP_NAME, sizeof(si.map));
    // S9: ship the host's REAL 0x17c map header so the client's browser preview shows the actual map
    // (biome/size), not the synth stub. current_map_data is the host's picked map while it sits in the lobby.
    memcpy(si.map_header, (const void *)ADDR_CUR_MAP, mh_net_proto::MAP_HEADER_SIZE);
    si.has_map_header = true;
    // X2: ...and WHICH BYTES that map name has to be. The header above is size/biome/start-count,
    // which two genuinely different maps share routinely, so the IDENTITY is the file's content hash
    // and it rides beside the name it qualifies.
    mh::seams::maps::host_fill_advert(si);
}

// S6: the player name each peer sent in its JOIN (host side), indexed by the JOIN frame's SENDER =
// the peer's transport-ASSIGNED player id (g_peer_player_name[sender & 7] at the write, on_join_recv --
// NOT positional/transport-connection index; corrected N2). The host stamps these into the lobby slot
// carrying that player_id + the peer table, and the slot snapshot broadcast carries them to the client.
// launch.cpp reads them via MH_MP_PeerName(id). g_lobby_left[] is keyed the same way.
char g_peer_player_name[8][32] = {{0}};

// U12: host-side "this player LEFT the lobby" mask (per player id). A client can Cancel out of the lobby
// back to the discovery browser while STAYING transport-connected (retail sends the host nothing here --
// net_udp's dead job), so MH_Net_PeerCount() never drops. on_leave_recv (FLAG_LEAVE) sets this; on_join_recv
// clears it on a clean re-join; mp_sync_host_peer_table (launch.cpp) reads it via MH_MP_IsLobbyLeft to drop
// the player from the mirrored peer set (which vacates its slot). Written on the recv thread -> volatile.
volatile LONG g_lobby_left[8] = {0};

// N2 (2026-07-22): host-side "this peer has sent ITS OWN JOIN" mask (per player id). A peer that has merely
// browse-connected (got its WELCOME/id) but not yet clicked Join must NOT appear in the host's lobby: the
// mirror used to slot EVERY transport-connected peer once the single global join gate (g_host_join_seen)
// opened, so a second joiner showed up (as the "Player2" placeholder) the instant the FIRST joiner opened the
// gate -- before it had joined at all. Gate the manual mirror per-peer on THIS flag instead. Set on that
// peer's own admitted JOIN (on_join_recv), cleared on its departure (the launch.cpp vacate loop). Recv-thread
// written -> volatile. MANUAL path only (the automated force-entry path never sends a UI JOIN -- see
// MH_MP_HasJoined's use in mp_sync_host_peer_table, gated on MH_MP_IsManual).
volatile LONG g_lobby_joined[8] = {0};

// ---- U2: manual Start gating (client side) ----------------------------------------------------
// The manual host enters only when the player clicks the lobby Start button (which drives
// llm_lobby_begin_map_load -- hooked in launch.cpp, where the host prep lives + FLAG_START is broadcast).
// The client must not auto-enter either: it waits for the host's FLAG_START. on_start_recv releases that
// gate; the manual client's mp_lobby_entry_tick reads it via MH_Seam_ClientStartReceived.
volatile LONG g_client_start_received = 0; // client: host's FLAG_START arrived

// Strict dotted-quad check: exactly 4 octets, each 0..255. Rejects a MID-TYPING partial like "192.168"
// (which a loose ">=7 chars + a dot" test wrongly accepted -> a connect to the wrong host, 2026-07-13).
bool mp_is_ipv4(const char *s) {
    int octets = 0, val = 0, digits = 0;
    for (const char *p = s;; ++p) {
        if (*p >= '0' && *p <= '9') {
            val = val * 10 + (*p - '0');
            digits++;
            if (digits > 3 || val > 255) return false;
        } else if (*p == '.' || *p == '\0') {
            if (digits == 0) return false; // empty octet
            ++octets;
            if (*p == '\0') break;
            val    = 0;
            digits = 0;
        } else {
            return false; // non-IPv4 char
        }
    }
    return octets == 4;
}

// S5-core: async connect-to-typed-host (off the UI thread; start_client blocks). Reuses the lazy
// transport bring-up (client role + option-B typed IP) via its exported wrapper. Kicked from
// on_discover_poll / MH_Seam_ClientDiscoveryTick.
DWORD WINAPI s3_connect_thread(LPVOID) {
    MH_Seam_StartTransport(); // lazy_start -> MH_Net_InitEx -> start_client (bounded connect, ~4s max on a dead IP -- S8)
    InterlockedExchange(&g_s3_conn_done_at, (LONG)GetTickCount());
    InterlockedExchange(&g_s3_conn_done, 1); // S8: the attempt finished -- MH_Net_IsStarted() is now authoritative
    return 0;
}

// The one-shot "dial the typed host" kick, factored out at U40 because there are now TWO places that
// want it: the browser's discovery poll (a first dial, as always) and the per-frame client tick (a
// RELINK -- see MH_Seam_ClientDiscoveryTick). `why` only names the site in the log.
//
// The gate widened from `!MH_Net_IsStarted()` to "no link we still believe in". A never-started
// transport is the first dial; g_net_relink is the SECOND -- a match ended, so the link that served it
// is spent whether or not its socket is still nominally open. That last clause is load-bearing: the
// 2026-09-01 capture had both conns reset at teardown, but a rig run's sockets survive the match
// intact, and a fix that only repaired the DEAD case would leave two different post-match states.
void s3_kick_connect(const char *why) {
    char ip[64];
    if (MH_Net_IsStarted() && !g_net_relink) return;
    if (g_s3_conn_kicked) return;
    // mp:R2/R7a -- IS THIS DIAL RELAYED OR DIRECT? A relayed dial needs no typed address (WHICH GAME
    // it wants is a room code the directory names, not an IP); a DIRECT dial (`[net] relay` set but the
    // player used *Internet server* + a typed IP) needs the IP. Before R7a `relay_configured()` alone
    // forced every dial through the relay -- so a typed address was dead weight. Now dial_wants_relay()
    // decides per dial (first browser / a relay-directory row -> relay; *Internet server* + typed IP ->
    // direct), and the answer is LATCHED so lazy_start builds the matching MH_NetConfig and the log
    // names the mode this connection actually used.
    const bool relay = dial_wants_relay();
    if (!relay && !mp_read_typed_join_ip(ip, sizeof(ip))) return;
    InterlockedExchange(&g_s3_conn_kicked, 1);
    InterlockedExchange(&g_s8_retry_armed, 0);           // fresh attempt -- re-arm the retry-ready gate
    InterlockedExchange(&g_dial_relayed, relay ? 1 : 0); // mp:R7a -- lazy_start reads this
    char b[192];
    if (relay) {
        // LATCH THE ROOM THIS DIAL IS FOR, here and nowhere else. lazy_start reads it back when
        // it builds MH_NetConfig (mp_relay_dial_room), and the menu tick compares the directory's
        // pick against it to decide whether a re-dial is owed -- so the three have to agree about
        // one value, which means one writer.
        const mh_net_proto::SessionInfo *pick = relay_picked_rec();
        g_relay_dialled                       = (pick != nullptr) ? g_relay_rows[g_relay_pick].room : relay_default_room();
        g_relay_room_set                      = true;
        wsprintfA(b, "; R2: relayed connect to room %u -> async dial (%s%s)\n",
                  (unsigned)g_relay_dialled, why, g_net_relink ? ", U40 relink" : "");
        InterlockedExchange(&g_r7_browse_dialled, 1); // R7: a Create after this dial re-inits as host
    } else {
        wsprintfA(b, "; S3: typed host '%s' -> async connect (%s%s)\n", ip, why,
                  g_net_relink ? ", U40 relink" : "");
    }
    seam_log(b);
    CloseHandle(CreateThread(nullptr, 0, s3_connect_thread, nullptr, 0, nullptr));
}

// ---- mp:R2: the relay directory, main-thread half ----------------------------------------------

// `[net] relay` is set, i.e. this peer reaches its games through a relay rather than by dialling a
// host's address. Read once (the ini is fixed for the run) into a TRIMMED cache: mp:R7a moved the ini
// read for the dial out of the module and into mh.dll, so the trailing-`;`-comment trim the module
// used to do (a `relay=<vps>:7100 ; comment` line was dialled comment-and-all, 2026-09-19) lives here
// now, once, and mp_relay_addr() hands the clean value to lazy_start.
const char *relay_addr_cached() {
    static char addr[80] = {0};
    static int  ready    = 0;
    if (!ready) {
        GetPrivateProfileStringA("net", "relay", "", addr, (DWORD)sizeof(addr), g_ini);
        // A TRAILING `; comment` IS PART OF THE VALUE to GetPrivateProfileString, and the example ini
        // documents every key with exactly such a comment. `;` cannot occur in a host name, a numeric
        // address or a port, so cut at the first one and trim the whitespace before it. Say so once
        // (the trim moved here from the module at mp:R7a, and the notice with it): the dialled value
        // and the ini's text differ from now on, which is the 2026-09-19 `relay=HOST:PORT ; comment`
        // rc3-player bug (log_formats id net.relay_trailing_comment).
        for (char *p = addr; *p != '\0'; ++p) {
            if (*p == ';') {
                *p = '\0';
                while (p > addr && (p[-1] == ' ' || p[-1] == '\t')) *--p = '\0';
                char b[200];
                wsprintfA(b,
                          "; R7a: [net] relay carried a trailing `;` comment; using `%s` (the comment "
                          "is not part of the address -- remove it from the ini)\n",
                          addr);
                seam_log(b);
                break;
            }
        }
        ready = 1;
    }
    return addr;
}
bool relay_configured() { return relay_addr_cached()[0] != '\0'; }

// mp:R3/R7a -- `[net] force_relay=1` PINS the relayed path. The module still reads it too (for its own
// promotion suppression); mh.dll reads it for the DECISION, because a pinned peer must relay every dial
// regardless of which browser or whether an IP was typed. Cached like the address.
bool relay_force() {
    static int cached = -1;
    if (cached < 0) cached = GetPrivateProfileIntA("net", "force_relay", 0, g_ini) != 0 ? 1 : 0;
    return cached != 0;
}

// mp:R7a -- THE PER-DIAL DECISION. With a relay configured a CLIENT dial is relayed when it is a
// relay-DISCOVERY dial and direct otherwise:
//   - force_relay=1                          -> relay (pinned; the relayed path only, mp:R3).
//   - the FIRST browser is on screen         -> relay (retail's LAN list, repurposed as the relay
//                                               directory in R7; local_browser_widget_origin).
//   - a relay-directory ROW is selected      -> relay + that row's room (a session-browser row that
//                                               came from the relay, mp:R2).
//   - otherwise (*Internet server* + a typed IP, the session browser) -> DIRECT, relay not contacted.
// With no relay configured this is always false (the LAN/direct path, unchanged).
bool dial_wants_relay() {
    if (!relay_configured()) return false;
    if (relay_force()) return true;
    const unsigned wl = *(const volatile unsigned *)mh::addr::_G_LLM_UI_MENU_WIDGET_LIST;
    if (wl == mh::addr::local_browser_widget_origin) return true;
    if (relay_picked_rec() != nullptr) return true;
    return false;
}

// The room the FIRST dial asks for, before the directory has said anything: `[net] port`, which is
// what the transport module defaults a client's room to. Keeping the two the same is what stops the
// directory's first answer from tearing down a connection that already worked. Since mp:R6 a host's
// room is minted, so this guess never lands on a host: the tunnel is refused `no_host`, parks on
// the directory leg, and the listed row's room is what the re-dial below asks for -- the ordinary
// path of every relayed client now, not a fault.
uint32_t relay_default_room() {
    return (uint32_t)GetPrivateProfileIntA("net", "port", 6501, g_ini);
}

// Main thread. Drain the inbox into the row table, expire what the directory stopped listing, and
// pick the row the browser shows. Returns true if the VISIBLE set changed (so the caller can
// re-arm the browser's one-shot poll and the player sees it without clicking anything).
// Forget one row -- and, if the CONNECTED store holds that same lobby, forget that too.
//
// THAT SECOND HALF IS NOT TIDINESS, it is the vanish clause. A client that merely BROWSED a host
// never receives the retail `0x0e` a host sends when it leaves (that arrives only to a peer sitting
// in the lobby), so `g_store_valid` -- a snapshot of the last advert that came over the still-open
// connection -- stood for the rest of the process, and the browser rendered a lobby that no longer
// existed. Measured exactly that way on R2's first rig run: the relay had `sessions_unregistered=1`
// and was answering 141 LISTs with an empty directory while the client's browser still showed one
// row. The relay IS the authority on "this lobby is still being advertised", so when it stops
// carrying a lobby, a record of that same lobby is stale by definition.
void relay_row_forget(int i, const char *why) {
    if (!g_relay_rows[i].used) return;
    char id[64];
    mh_net_proto::lobby_id_str(g_relay_rows[i].si, id, sizeof(id));
    char b[192];
    wsprintfA(b, "; R2 relay directory: %s %s -> row dropped\n", id, why);
    seam_log(b);
    if (g_store_valid && mh_net_proto::session_same_lobby(g_store_rec, g_relay_rows[i].si)) {
        InterlockedExchange(&g_store_valid, 0);
        seam_log("; R2: ...and it is the lobby our stored advert names -- withdrawn too (a "
                 "browsing client never receives the host's leave notice)\n");
    }
    g_relay_rows[i].used = false;
}

bool relay_rows_drain() {
    bool        changed = false;
    const DWORD now     = GetTickCount();
    LONG        tail    = g_relay_in_tail;
    while (tail != InterlockedCompareExchange(&g_relay_in_head, 0, 0)) {
        const RelayInbox &in = g_relay_in[tail];
        tail                 = (tail + 1) % RELAY_INBOX;
        // Too old to mean anything: the relay said this before the match we have just come out of.
        // Dropped rather than applied, so a finished lobby cannot be re-listed by its own backlog.
        if (now - in.at >= RELAY_ROW_TTL) continue;
        if (in.empty) {
            // The relay answered with nothing. Drop every row NOW: this is the host-left case, and
            // making the player wait out a TTL for a lobby the relay has already forgotten is the
            // difference between a browser that is right and one that is merely eventually right.
            for (int i = 0; i < RELAY_ROW_MAX; ++i) {
                if (!g_relay_rows[i].used) continue;
                relay_row_forget(i, "is no longer on the relay");
                changed = true;
            }
            continue;
        }
        int slot = -1;
        for (int i = 0; i < RELAY_ROW_MAX; ++i)
            if (g_relay_rows[i].used && mh_net_proto::session_same_lobby(g_relay_rows[i].si, in.si))
                slot = i;
        if (slot < 0) {
            for (int i = 0; i < RELAY_ROW_MAX && slot < 0; ++i)
                if (!g_relay_rows[i].used) slot = i;
            if (slot < 0) continue; // the table is full; a row must age out before a new one lands
            changed = true;
            char id[64];
            mh_net_proto::lobby_id_str(in.si, id, sizeof(id));
            char b[192];
            wsprintfA(b, "; R2 relay directory: %s map=%s players=%d/%d room=%u -> listed\n", id,
                      in.si.map, in.si.cur_players, in.si.max_players, (unsigned)in.room);
            seam_log(b);
        }
        g_relay_rows[slot].si        = in.si;
        g_relay_rows[slot].room      = in.room;
        g_relay_rows[slot].last_seen = in.at;
        g_relay_rows[slot].used      = true;
    }
    InterlockedExchange(&g_relay_in_tail, tail);

    for (int i = 0; i < RELAY_ROW_MAX; ++i) {
        if (!g_relay_rows[i].used || now - g_relay_rows[i].last_seen < RELAY_ROW_TTL) continue;
        relay_row_forget(i, "stopped being listed");
        changed = true;
    }

    // THE PICK is the lowest live row, deliberately: the browser renders ONE row (the retail
    // handle array's bound is not known, and writing past it would be a memory bug in the game's
    // own .bss), so the choice has to be stable rather than clever. With several lobbies on one
    // relay (mp:R6 made that a real state: every host mints its own room), the pick is the one
    // this peer dials -- a multi-row browser is mp:R2b's residue, not a room question.
    const int was = g_relay_pick;
    g_relay_pick  = -1;
    for (int i = 0; i < RELAY_ROW_MAX && g_relay_pick < 0; ++i)
        if (g_relay_rows[i].used) g_relay_pick = i;
    return changed || (was != g_relay_pick);
}

const mh_net_proto::SessionInfo *relay_picked_rec() {
    if (g_relay_pick < 0 || !g_relay_rows[g_relay_pick].used) return nullptr;
    return &g_relay_rows[g_relay_pick].si;
}

// S3: build the discovery browser from the received-record store instead of fabricating. Empty store ->
// NO game listed (kills the "MH Host" fabrication). One record -> a real, joinable row carrying the host's
// real name + player counts. Map header is a placeholder (not shown in the row; corrected at join by the
// host's current_map_data broadcast). Reuses the g_synth_desc buffer + the SD_* layout of build_synth_session.
//
// mp:R2 -- TWO SOURCES, ONE ROW. A connected host's own advert wins (it is live, and it is what the
// JOIN will name); with no connection, a lobby the RELAY listed is shown instead, which is what
// lets a player see a relayed game before dialling anything. They are the same record type from
// the same encoder, so only where it came from differs.
void build_browser_from_store() {
    const mh_net_proto::SessionInfo *rec = g_store_valid ? &g_store_rec : relay_picked_rec();
    if (rec == nullptr) { // no host reachable -> empty browser
        *(void **)ADDR_SESSION_LIST_PTR = nullptr;
        *(int *)ADDR_SESSION_COUNT      = 0;
        *(void **)ADDR_UI_ROW_ARRAY     = nullptr;
        return;
    }
    ensure_placeholder_maphdr(); // a VALID header (not raw current_map_data)
    memset(g_synth_desc, 0, sizeof(g_synth_desc));
    // S9: prefer the host's REAL map header (shipped in the v2 SESSION_INFO) so the browser PREVIEW shows
    // the actual biome/size; fall back to the placeholder for a v1 host (has_map_header == false).
    if (rec->has_map_header)
        memcpy(g_synth_desc + SD_MAPHDR, rec->map_header, MAP_DATA1_SIZE); // REAL host map header (preview)
    else
        memcpy(g_synth_desc + SD_MAPHDR, g_map_hdr_placeholder, MAP_DATA1_SIZE); // placeholder (v1 host)
    *(void **)(g_synth_desc + SD_HANDLE) = (void *)1;                            // sentinel handle (join matches ==1)
    bs_str_copy((char *)(g_synth_desc + SD_NAME), rec->name, 32);                // REAL host game name
    // S7: render the count as exactly "occ/cap". install_mp_bootstrap patches the row format string
    // (browser_row_count_fmt 0x503068) from u"%s\t%d+%d/%d" -> u"%s\t%d/%d", so the NORMAL-branch
    // renderer emits "<first>/<second>" where first = total_slots - player_count, second = player_count.
    //   second = cap  -> SD_PCOUNT = cap (= g_store_rec.max_players, the host's non-closed capacity)
    //   first  = occ  -> SD_TOTAL - player_count = occ -> SD_TOTAL = occ + cap
    // giving (SD_TOTAL - SD_PCOUNT)/SD_PCOUNT = occ/cap. occ = g_store_rec.cur_players (host+AI+humans);
    // it climbs by 1 as a peer joins. These fields are display-only (read ONLY by the two row renderers).
    int occ = rec->cur_players;                             // occupied incl AI
    int cap = rec->max_players;                             // capacity after closed slots
    if (cap < 1) cap = 1;                                   // never divide-render "occ/0"
    if (occ > cap) occ = cap;                               // clamp (a stale advert never reads "5/3")
    *(int *)(g_synth_desc + SD_TOTAL) = occ + cap;          // first field = total - pc = occ
    *(int *)(g_synth_desc + SD_MAX)   = cap;                // (unused by the patched 2-arg format; kept sane)
    g_synth_desc[SD_STATE]            = 0;                  // joinable -> NORMAL format branch
    g_synth_desc[SD_PCOUNT]           = (unsigned char)cap; // second field = pc = cap
    *(int *)(g_synth_desc + SD_PROTO) = *(const int *)ADDR_PROTO_CEIL;
    *(void **)ADDR_SESSION_LIST_PTR   = g_synth_desc;
    *(int *)ADDR_SESSION_COUNT        = 1;
    *(void **)ADDR_UI_ROW_ARRAY       = (void *)1;
    *(int *)(ADDR_UI_ROW_ARRAY + 4)   = 0;
}

} // namespace

// mp:R2 -- the two questions lazy_start (net_seams.cpp) asks about the relay. It builds
// MH_NetConfig and nothing else may, so the ROOM a relayed client dials has to reach it from
// here: the room is a lobby choice (which listed game do I want), and the lobby is this TU's.
bool mp_relay_configured() { return relay_configured(); }

// mp:R7a -- the trimmed `[net] relay` value, for lazy_start to put in MH_NetConfig.relay_addr on a
// relayed dial. Returns false (and out[0]='\0') when no relay is configured. External wrapper (the
// cache lives in the anonymous namespace above) -- the same shape as mp_relay_configured.
bool mp_relay_addr(char *out, int cap) {
    const char *a = relay_addr_cached();
    lstrcpynA(out, a, cap);
    return a[0] != '\0';
}

// mp:R7a -- lazy_start reads the kick site's latched relay-vs-direct decision for the current manual
// client dial (g_dial_relayed lives in the anonymous namespace; this is its external accessor).
bool mp_dial_is_relayed() { return InterlockedCompareExchange(&g_dial_relayed, 0, 0) == 1; }

// mp:R2a -- IS THE SESSION BROWSER THE ACTIVE SCREEN? The UDP transport module asks this from its
// relay tunnel thread, and the answer decides whether that tunnel keeps polling the relay's session
// directory (`OP_LIST`, every 2 s).
//
// WHY IT IS AN EXPORT AND NOT A MODULE ROW. MH_NET_MODULE_SYMBOLS is the mh.dll -> module contract
// and BOTH transports must answer all of it; a session directory is a UDP-relay concept, so a 27th
// row would oblige the TCP module to answer a question it cannot have (the same reasoning that kept
// R2's directory row off the table). This crosses the other way -- module -> mh.dll -- and is
// OPTIONAL by construction: `mh_net_udp.dll` resolves it with GetProcAddress against the mh.dll
// handle it already has and, finding nothing (net_selftest.exe, a standalone module, an older
// mh.dll), keeps R2's unconditional polling. So it can never be a load-time dependency.
//
// WHY THE PREDICATE IS COPIED RATHER THAN SHARED with MH_Seam_ClientDiscoveryTick's identical two
// lines: this is read on the TUNNEL thread and that one on the main thread, and keeping them two
// independent reads of one aligned 32-bit .bss word is what makes the cross-thread story trivial --
// a torn read is impossible and a stale one costs at most one 2 s poll either way.
extern "C" __declspec(dllexport) int MH_Seam_RelayBrowsing(void) {
    const unsigned wl = *(const volatile unsigned *)mh::addr::_G_LLM_UI_MENU_WIDGET_LIST;
    return (wl == mh::addr::browser_widget_array_ptr ||
            wl == mh::addr::local_browser_widget_origin)
               ? 1
               : 0;
}

uint32_t mp_relay_dial_room() {
    if (!relay_configured()) return RELAY_NO_ROOM;
    // The LATCH, not the current pick: s3_kick_connect decided which room this dial is for, and a
    // directory refresh between the kick and the worker thread's lazy_start must not silently
    // redirect a connection the log already named.
    return g_relay_room_set ? g_relay_dialled : relay_default_room();
}

// Read the in-game typed join IP (U1c). The IP field is a runtime-wired widget, so its buffer isn't
// statically knowable; scan the .bss addresses the RE trace + marker-scan identified (0xe60760 confirmed-
// live 2026-07-12; 0xe60720 fallback) and copy the FIRST buffer holding a COMPLETE IPv4 into out. Returns
// true if one was found. 0x005d0da8 is deliberately NOT scanned -- that is the game/session NAME.
bool mp_read_typed_join_ip(char *out, int cap) {
    const uintptr_t cand[] = {mh::addr::session_desc_candidates, mh::addr::session_desc_candidate_b};
    for (int k = 0; k < 2; ++k) {
        const char *p = (const char *)cand[k];
        char        ip[40];
        int         n  = 0;
        bool        ok = true;
        for (; n < 31 && p[n]; ++n) {
            char c = p[n];
            if (c < 0x20 || c > 0x7e) {
                ok = false;
                break;
            }
            ip[n] = c;
        }
        ip[n] = '\0';
        if (ok && mp_is_ipv4(ip)) {
            lstrcpynA(out, ip, cap);
            return true;
        }
    }
    return false;
}

void mp_host_advertise_session() {
    static DWORD last        = 0;
    static int   last_peers  = 0;
    int          peers       = MH_Net_PeerCount();
    DWORD        now         = GetTickCount();
    bool         peer_joined = (peers > last_peers); // a NEW peer connected -> advertise NOW (S3:
    last_peers               = peers;                // the client must get the record before we enter)
    if (!peer_joined && now - last < 1000) return;   // else ~1 Hz
    last = now;
    mh_net_proto::SessionInfo si;
    mp_build_host_session_info(si);
    uint8_t buf[mh_net_proto::SESSION_INFO_MAX_ENCODED];
    int     n = (int)mh_net_proto::session_info_encode(si, buf);
    char    id[64];
    mh_net_proto::lobby_id_str(si, id, sizeof(id));
    char b[192];
    wsprintfA(b, "; S2 host session: %s map=%s players=%d/%d ver=%d proto=%d (%d B, peers=%d%s)\n",
              id, si.map, si.cur_players, si.max_players, si.host_version, si.protocol, n, peers,
              peer_joined ? ", NEW" : "");
    seam_log(b);
    // mp:R2 -- UNCONDITIONAL, where it used to be `if (peers > 0)`. Broadcasting to nobody was
    // always a no-op (both transports loop over ACTIVE connections), so the guard bought nothing;
    // what it cost, once a relay existed, was the whole directory: the UDP module publishes the
    // host's lobby to the relay from inside this very call, and a lobby with no joiner yet is
    // exactly the one a browser needs to see. An empty lobby that never advertised was a game
    // nobody could find.
    MH_Net_SendSessionInfo(buf, n);
}

// Client side (recv thread): store each received SESSION_INFO. The main-thread MH_Seam_ClientDiscoveryTick
// then re-arms the browser to list it (S3). Write the record BEFORE publishing g_store_valid (x86 TSO).
void on_session_info_recv(int sender, const unsigned char *buf, int len) {
    // mp:R2 -- a row of the RELAY's session directory rather than a connected peer's advert. It
    // does NOT become the S3 store: that store means "the host we are connected to said this",
    // and nobody is connected here. It goes to the browser's relay rows instead, via the inbox
    // (this runs on the transport module's tunnel thread; see the R2 block above).
    if (mh_net_proto::session_sender_is_relay(sender)) {
        const LONG head = g_relay_in_head;
        const LONG next = (head + 1) % RELAY_INBOX;
        // Full ring -> DROP, never block the tunnel thread and never overwrite an unread slot.
        // The directory is re-listed every couple of seconds, so a drop costs one refresh.
        if (next == InterlockedCompareExchange(&g_relay_in_tail, 0, 0)) return;
        RelayInbox &slot = g_relay_in[head];
        slot.at          = GetTickCount();
        if (len <= 0) {
            slot.empty = true; // the relay answered with an empty directory
        } else {
            mh_net_proto::SessionInfo dir;
            if (!mh_net_proto::session_info_decode(buf, len, dir)) {
                seam_log("; R2 relay directory: malformed descriptor -- row ignored\n");
                return;
            }
            slot.empty = false;
            slot.si    = dir;
            slot.room  = mh_net_proto::session_sender_relay_room(sender);
            slot.len   = len;
        }
        InterlockedExchange(&g_relay_in_head, next); // publish AFTER the record is written
        return;
    }
    mh_net_proto::SessionInfo si;
    if (!mh_net_proto::session_info_decode(buf, len, si)) {
        seam_log("; S3 recv: malformed SESSION_INFO\n");
        return;
    }
    g_store_rec = si;
    InterlockedExchange(&g_store_valid, 1);
    // SES0: the CLIENT's side of "logged by every peer". The first advert carrying a given match_id
    // is this peer's join-side record of which match it is in; a host re-create arrives as a new id
    // and logs a second line. Guarded to one line per id (mp_log_match_id).
    mp_log_match_id(si.match_id);
    char id[64];
    mh_net_proto::lobby_id_str(si, id, sizeof(id));
    char b[192];
    wsprintfA(b, "; S3 recv SESSION_INFO from %d: %s map=%s players=%d/%d ver=%d -> store\n",
              sender, id, si.map, si.cur_players, si.max_players, si.host_version);
    seam_log(b);
    // X2: the advert is also where a joiner learns WHICH MAP BYTES this lobby requires. Acted on once
    // per distinct claim -- the advert repeats about once a second, so every later one is a compare.
    mh::seams::maps::client_on_advert(si);
}

// ---- S4: JOIN gating --------------------------------------------------------------------------
// The manual host must NOT treat a browse-connect as a full join (that slotted + flooded a peer still
// sitting in the browser -> client crash + "joined" spam). Instead it advertises SESSION_INFO to every
// connection but does the lobby work (slot mirror + map flood + auto-entry) ONLY once a peer has sent an
// explicit JOIN naming this host's lobby-id. g_host_join_seen (net_internal.h) is that gate.

// Accessor for launch.cpp (mp_sync_host_peer_table) -- launch.cpp wraps in an anon namespace, so bridge via
// an extern "C" function rather than a shared global. Returns "" for out-of-range or unset entries.
extern "C" const char *MH_MP_PeerName(int i) {
    return (i >= 0 && i < 8) ? g_peer_player_name[i] : "";
}
extern "C" int MH_MP_IsLobbyLeft(int player_id) { // U12: 1 = this player left the lobby (Cancel)
    return (player_id >= 0 && player_id < 8) ? g_lobby_left[player_id] : 0;
}
extern "C" int MH_MP_HasJoined(int player_id) { // N2: 1 = this peer sent its own admitted JOIN
    return (player_id >= 0 && player_id < 8) ? g_lobby_joined[player_id] : 0;
}

// U10 D2 / U12 (peer-departure hooks for launch.cpp mp_sync_host_peer_table). When a peer leaves, launch.cpp
// drives the retail lobby-slot vacate (llm_lobby_remove_player_slot); these clear the matching host-side S6
// state so a re-join re-populates cleanly instead of leaving the ghost slot/name behind.
extern "C" void MH_MP_ClearPeerName(int player_id) { // forget a departed peer's JOIN name (per player id)
    if (player_id >= 0 && player_id < 8) g_peer_player_name[player_id][0] = '\0';
}
extern "C" void MH_MP_ClearJoined(int player_id) { // N2: forget a departed peer's JOIN mark (per player id)
    if (player_id >= 0 && player_id < 8) InterlockedExchange(&g_lobby_joined[player_id], 0);
}
extern "C" void MH_MP_ResetJoinGate(void) { // all peers gone -> re-arm the S4 join gate
    InterlockedExchange(&g_host_join_seen, 0);
}

// S8(b): non-zero once a FAILED client connect has cleared the connect latches (retry-ready). Read by the
// ui_drive `retryready` predicate so the round-trip test gates the corrected-IP re-Connect on the actual
// latch-clear (no time wait). Re-armed to 0 each time a fresh connect is kicked (on_discover_poll).
extern "C" int MH_Seam_S8RetryArmed(void) {
    return g_s8_retry_armed;
}

// U13: the host left its lobby (its Cancel broadcast the retail 0x0e). CLIENT side (recv seam): withdraw the
// stored host session so the discovery browser -- which the retail 0x0e handling navigates the client to --
// lists NO ghost for the departed host, and record that this finalize is a host-left (not our own Cancel).
extern "C" void MH_MP_ClientOnHostLeft(void) {
    InterlockedExchange(&g_store_valid, 0);    // withdraw the stale session (no ghost in discovery)
    InterlockedExchange(&g_join_pending, 0);   // mp:R6: a click on a lobby that is gone is not re-sent
    InterlockedExchange(&g_host_left_seen, 1); // finalize detour reads this to skip the spurious LEAVE
    InterlockedExchange(&g_exit_cause, 1);     // U23: default cause = HOST_LEFT (link-lost overrides after)
    InterlockedExchange(&g_s3_conn_kicked, 0); // allow a fresh connect if the client re-types/refreshes
    InterlockedExchange(&g_s3_listed, 0);
    seam_log("; U13: host left lobby -> client withdrew the session (discovery shows no ghost)\n");
}

// U13: consumed by on_lobby_finalize -- 1 (once) if this lobby exit is a host-LEFT (skip the client LEAVE,
// the host is already gone), 0 if it is our own Cancel button (U12 sends LEAVE as before).
extern "C" int MH_MP_ConsumeHostLeft(void) { return InterlockedExchange(&g_host_left_seen, 0); }

// U23: called by the R-live-ui link-lost synthesis site ONLY, immediately after MH_MP_ClientOnHostLeft(),
// to correct the default cause. A genuinely received 0x0e wants the default and never calls this.
extern "C" void MH_MP_MarkExitCauseLinkLost(void) { InterlockedExchange(&g_exit_cause, 2); }

// U23: consumed by on_lobby_finalize alongside MH_MP_ConsumeHostLeft -- 1 = host left, 2 = link lost,
// 3 = our JOIN was refused (F3c), 0 = none of those (a deliberate Cancel, which must show the player nothing).
extern "C" int MH_MP_ConsumeExitCause(void) { return InterlockedExchange(&g_exit_cause, 0); }

// ---- mp:F3c -- the refused JOIN, client side ------------------------------------------------------
// Recv thread: the host's ANNOUNCE said our JOIN was refused, and why.
extern "C" void MH_MP_ClientOnJoinRefused(const char *reason) {
    lstrcpynA(g_join_refused_reason, reason ? reason : "refused", sizeof(g_join_refused_reason));
    InterlockedExchange(&g_join_refused, 1); // publish AFTER the text
    char b[192];
    wsprintfA(b, "; F3c: JOIN REFUSED by the host: %s -> leaving the lobby (the host never seated us)\n",
              g_join_refused_reason);
    seam_log(b);
}
// Main thread (MH_Seam_PollRecv): 1 once, when a refusal is pending; the caller then synthesises the
// lobby exit and marks the cause so the browser can name the reason.
extern "C" int         MH_MP_TakeJoinRefused(void) { return InterlockedExchange(&g_join_refused, 0); }
extern "C" void        MH_MP_MarkExitCauseJoinRefused(void) { InterlockedExchange(&g_exit_cause, 3); }
extern "C" const char *MH_MP_JoinRefusedReason(void) { return g_join_refused_reason; }

// ---- mp:R4a -- the relay's protocol-level notice, module -> mh.dll -----------------------------------
// The UDP module's relay tunnel (udp_relay.cpp) has compared the relay's protocol level with its own
// and found the relay behind (or speaking a leg version it cannot read). The one line in mh_net.log is
// its own; THIS is how the player learns it: one short ASCII line, handed to the browser's status line
// through F3c's carrier (browser_notice_arm_relay), which paints on the very screen a relay is dialled
// from -- the first browser (mp:R7). Same shape as MH_Seam_RelayBrowsing above and for the same reason:
// resolved by name from the module, so an mh.dll without it costs nothing but the notice.
//
// Threads, as F3c: written on the tunnel's PUMP thread (text first, then the flag -- x86 TSO), consumed
// on the MAIN thread by the present hook (MH_MP_DrainRelayNotice), which is the only thread allowed to
// arm a widget. One slot, latest wins: the tunnel reports a level once per change, so two in flight
// means a relay that was redeployed between two frames, and the later one is the true state.
volatile LONG g_relay_notice          = 0;
char          g_relay_notice_line[64] = {0};

extern "C" __declspec(dllexport) void MH_Seam_RelayNotice(const char *line) {
    lstrcpynA(g_relay_notice_line, line ? line : "Relay outdated", sizeof(g_relay_notice_line));
    InterlockedExchange(&g_relay_notice, 1); // publish AFTER the text
    char b[160];
    wsprintfA(b, "; R4a: relay notice queued for the browser: %s\n", g_relay_notice_line);
    seam_log(b);
}
// Main thread (the present hook, net_lockstep.cpp): arm the browser notice once per queued line.
extern "C" void MH_MP_DrainRelayNotice(void) {
    if (InterlockedExchange(&g_relay_notice, 0) == 0) return;
    mh::ui::browser_notice_arm_relay(g_relay_notice_line);
}

// U13: the manual HOST left its lobby (Cancel = llm_lobby_host_start_game). Reset this host's session
// IDENTITY + join state so a re-created game is a DISTINCT lobby (fresh tag -> new lobby-id, no ghost/dedup
// collision) and the still-connected client is NOT treated as pre-joined (it must send a fresh JOIN).
namespace {
// The identity reset ITSELF, with no session-close of its own. Two callers want it and only one of
// them may close a session: the U13 lobby-Cancel path below (where the identity going away IS the
// session ending) and U40's mp_session_close, which is already INSIDE the close and would recurse.
void host_reset_identity() {
    g_mp_tag = 0;                                    // next mp_build_host_session_info mints a FRESH tag
    memset(g_mp_match_id, 0, sizeof(g_mp_match_id)); // SES0: ...and a FRESH match_id with it
    InterlockedExchange(&g_host_join_seen, 0);       // re-arm the S4 gate (no auto-slot of the old peer)
    for (int i = 0; i < 8; ++i) {
        g_peer_player_name[i][0] = '\0';
        InterlockedExchange(&g_lobby_joined[i], 0);
        InterlockedExchange(&g_lobby_left[i], 0);
    }
}
} // namespace

extern "C" void MH_MP_HostResetSessionIdentity(void) {
    // SES1: the identity going away IS the session ending. Close before the reset, so SESSION_END is
    // the last line of the directory it describes and the NEXT mint opens a second, distinct one --
    // which is exactly the host_recreate case the acceptance criteria count directories for.
    mp_session_close("leave");
    host_reset_identity();
    seam_log("; U13: host left lobby -> reset session identity (fresh tag on re-create) + join gate\n");
}

// mp:F3c -- Host (recv thread): tell the refused peer. A FLAG_ANNOUNCE of kind REFUSED, addressed by
// player id; every connected peer receives it (the transports only broadcast) and only the named one
// acts. The text is the same spelling the host log carries (join_refusal_text), so what the joiner
// reads on its screen is what the host wrote down.
namespace {
void host_reply_refused(int sender, mh_net_proto::JoinAdmit verdict, const mh_net_proto::SessionInfo &mine,
                        const mh_net_proto::JoinRequest &jr) {
    if (sender < 0 || sender > 7) return; // no player id to address -- nothing a peer could match
    char reason[mh_net_proto::ANNOUNCE_TEXT_CAP];
    mh_net_proto::join_refusal_text(verdict, mine, jr, reason, sizeof(reason));
    uint8_t   ab[mh_net_proto::ANNOUNCE_MAX_ENCODED];
    const int n = (int)mh_net_proto::announce_refused_encode((uint8_t)sender, reason, ab);
    MH_Net_SendAnnounce(ab, n);
    char b[192];
    wsprintfA(b, "; F3c: sent JOIN_REFUSED to player %d: %s\n", sender, reason);
    seam_log(b);
}
} // namespace

// Host (recv thread): a client asked to join. Validate the lobby-id against our own advertised session
// (name+tag); on a match, open the join gate so the next lobby dispatch slots + floods this peer.
void on_join_recv(int sender, const unsigned char *buf, int len) {
    // U40: WE HAVE NO LOBBY -> there is nothing to admit into. Before the match-end identity reset
    // this branch was unreachable (the tag survived the whole process, so a JOIN always found one);
    // now a peer that re-dials between the old match ending and the new game being created would
    // otherwise make mp_build_host_session_info MINT a lobby -- from the recv thread, opening a
    // session directory for a lobby the player has not created. Refusing is also simply correct.
    if (g_mp_tag == 0) {
        char b[96];
        wsprintfA(b, "; S4 JOIN from %d -> IGNORED (this host has no lobby right now)\n", sender);
        seam_log(b);
        return;
    }
    mh_net_proto::SessionInfo mine;
    mp_build_host_session_info(mine); // our current lobby-id (name+tag+match_id)
    // SES0: ONE decision function, shared with the offline oracle (net_selftest.exe sessionidtest).
    // Every refusal is NAMED in the log; three of the four branches cannot be produced on the rig at
    // all, which is exactly why the decision does not live inline here.
    mh_net_proto::JoinRequest jr;
    mh_net_proto::JoinAdmit   verdict = mh_net_proto::join_admit(buf, len, mine, jr);
    char                      b[192];
    if (verdict == mh_net_proto::JoinAdmit::RefusedMalformed || verdict == mh_net_proto::JoinAdmit::RefusedNewerProtocol) {
        wsprintfA(b, "; S4 JOIN from %d -> REFUSED (%s; format %u, we speak %u..%u)\n",
                  sender, mh_net_proto::join_admit_reason(verdict), (unsigned)jr.format,
                  (unsigned)mh_net_proto::JOIN_REQUEST_MIN_FORMAT, (unsigned)mh_net_proto::JOIN_REQUEST_FORMAT);
        seam_log(b);
        // F3c: a NEWER client understands the REFUSED announce (it post-dates F3c by definition);
        // a malformed frame has no sender we can trust to be a client at all, so it gets nothing.
        if (verdict == mh_net_proto::JoinAdmit::RefusedNewerProtocol) host_reply_refused(sender, verdict, mine, jr);
        return;
    }
    if (verdict == mh_net_proto::JoinAdmit::RefusedCodepage) {
        // F3's negative case. Named in full on both sides of the comparison, because the fix is an ini
        // edit on ONE of the two machines and a log that says only "mismatch" does not say which.
        // Since F3c a joiner ADOPTS our codepage from the advert, so reaching here means that peer
        // declared it cannot switch ([input] codepage_adopt=0, or the codepage is not installed there)
        // -- and the refusal is now DELIVERED to it, not only logged here.
        wsprintfA(b, "; S4 JOIN from %d '%s' for '%s#%08X' -> REFUSED (%s; ours %u, theirs %u -- "
                     "that peer declined to adopt ours; pin [input] codepage to the same value on both)\n",
                  sender, jr.player_name, jr.name, jr.tag, mh_net_proto::join_admit_reason(verdict),
                  (unsigned)mine.codepage, (unsigned)jr.codepage);
        seam_log(b);
        host_reply_refused(sender, verdict, mine, jr);
        return;
    }
    if (verdict == mh_net_proto::JoinAdmit::RefusedOldProtocol) {
        wsprintfA(b, "; S4 JOIN from %d '%s' for '%s#%08X' -> REFUSED (%s; format %u < %u)\n",
                  sender, jr.player_name, jr.name, jr.tag, mh_net_proto::join_admit_reason(verdict),
                  (unsigned)jr.format, (unsigned)mh_net_proto::JOIN_REQUEST_MIN_FORMAT);
        seam_log(b);
        return;
    }
    if (verdict == mh_net_proto::JoinAdmit::Admit) {
        InterlockedExchange(&g_host_join_seen, 1);
        InterlockedExchange(&g_lobby_left[sender & 7], 0);             // U12: a (re)join clears any stale left-mark
        lstrcpynA(g_peer_player_name[sender & 7], jr.player_name, 32); // S6: remember the joiner's name FIRST...
        InterlockedExchange(&g_lobby_joined[sender & 7], 1);           // N2: ...THEN publish joined (release fence),
        //   so the main-thread mirror can never observe joined=1 with a not-yet-copied name (would freeze
        //   "Player2" into the change-gated peer-table write). Ordering makes the per-frame refresh redundant.
        // X2: an admitted JOIN also reports which map bytes that peer holds -- the REQUEST when it
        // disagrees with our claim, and the COMPLETION report when a re-sent JOIN agrees. The Start
        // gate reads nothing else, which is what makes "this peer has the map" an assertion at the
        // receiver rather than a sender's belief that it finished pushing.
        mh::seams::maps::host_on_join(sender, jr.player_name, jr.map_hash);
        wsprintfA(b, "; S4 JOIN from %d '%s' for '%s#%08X' -> ADMITTED (open slot/map gate)\n",
                  sender, jr.player_name, jr.name, jr.tag);
    } else {
        wsprintfA(b, "; S4 JOIN from %d for '%s#%08X' -> IGNORED (not our lobby '%s#%08X')\n",
                  sender, jr.name, jr.tag, mine.name, mine.tag);
    }
    seam_log(b);
}

// Host (recv thread): a client left the lobby (Cancel -> discovery browser) while staying connected. Mark it
// so mp_sync_host_peer_table drops it from the mirrored peer set (frees its lobby slot). A later re-JOIN
// clears the mark (on_join_recv). U12. Cheap + race-free (single volatile write; the main-thread mirror reads it).
void on_leave_recv(int sender) {
    if (sender >= 0 && sender < 8) InterlockedExchange(&g_lobby_left[sender & 7], 1);
    mh::seams::maps::host_on_leave(sender); // X2: a departed peer must never hold the Start gate shut
    char b[96];
    wsprintfA(b, "; U12 LEAVE from player %d -> mark left-lobby (host frees the slot)\n", sender);
    seam_log(b);
}

// mp:F3c -- the JOIN's player name, re-encoded for the session. The name was typed under OUR codepage
// before this peer knew the host's; the host stamps the JOIN's copy into its lobby slot and widens it
// under the SESSION codepage, so a non-ASCII name would otherwise show up there as different letters.
// Only the JOIN's copy is touched -- mh.exe's own name global is the player's saved setting and stays
// in the player's own codepage (its own lobby row is drawn by this peer, under the adopted codepage,
// which is the one cosmetic residue: a non-ASCII name in the joiner's OWN row). ASCII is a no-op.
namespace {
void fill_my_player_name(mh_net_proto::JoinRequest &jr) {
    lstrcpynA(jr.player_name, (LPCSTR)mh::addr::mp_player_name, sizeof(jr.player_name));
    MH_ChatInput_Transcode(jr.player_name, (int)sizeof(jr.player_name), MH_ChatInput_OwnCodepage(),
                           MH_ChatInput_Codepage());
}
} // namespace

// Client (game/UI thread): the player clicked "Вступить в игру" on the selected record. Send an explicit
// JOIN carrying the lobby-id of the received host session (g_store_rec) over the already-open browse
// connection. This is the distinct join action -- a browse-connect alone must NOT admit us. Manual client
// only; a no-op (return 0 = success) otherwise so the retail join handler proceeds into the lobby.
void on_join_connect() {
    if (!MH_MP_IsManual()) return;            // force-entry: keep the pure no-op
    if (!g_a.is_host || *g_a.is_host) return; // client only
    // mp:R2 -- the record the JOIN names is the one the BROWSER ROW was built from, which is the
    // connected host's advert when there is one and a relay directory row otherwise. They are the
    // same encoder's output, so the JOIN is identical either way; what differs is that a relay
    // row may be named before the link into its room has finished coming up, in which case the
    // send is dropped by the transport and the player's next click carries it. Saying so beats
    // the old bare refusal, which read as "the game is unreachable".
    const mh_net_proto::SessionInfo *rec = g_store_valid ? &g_store_rec : relay_picked_rec();
    if (rec == nullptr) {
        seam_log("; S4 join: no stored host record -> JOIN not sent\n");
        return;
    }
    if (!g_store_valid && !g_join_resending) {
        // mp:R6 -- remember it; the link coming up re-sends it (see g_join_pending).
        g_join_pending_rec  = *rec;
        g_join_pending_room = (g_relay_pick >= 0) ? g_relay_rows[g_relay_pick].room : 0;
        InterlockedExchange(&g_join_pending, 1);
        seam_log("; S4 join: naming a RELAY-LISTED lobby (the link into its room is still coming "
                 "up; the JOIN is re-sent once it is -- mp:R6)\n");
    } else if (!g_join_resending) {
        InterlockedExchange(&g_join_pending, 0);
    }
    // F3: the JOIN carries OUR pin, not the advert's -- join_request_for's two-argument form exists
    // precisely so a client cannot accidentally echo the host's value and make a mismatch invisible.
    // X2: the third argument is the same rule one field along -- the hash of the map file WE hold
    // under the advertised name, all-zero for "nothing that matches". That is simultaneously the
    // map request (the host arms a transfer when it disagrees) and, after a download, the
    // completion report (see mp_join_resend below).
    // mp:F3c: THE SESSION'S CODEPAGE IS THE HOST'S. The advert says which; adopt it for the length of
    // this match before the JOIN is built, so the JOIN carries the value the host will accept and
    // the three input hooks encode what we type the way the host will read it. Logged here, in
    // mh_net.log, because this is the file a two-peer verdict is read from. A peer that will not or
    // cannot switch sends its own value and is refused by name -- and, since F3c, TOLD (see
    // MH_MP_ClientOnJoinRefused). A fresh JOIN also clears any refusal still pending from an earlier
    // lobby, so the bounce below can only ever answer THIS join.
    InterlockedExchange(&g_join_refused, 0);
    {
        const unsigned host_cp = rec->codepage, own_cp = MH_ChatInput_OwnCodepage();
        char           cb[160];
        if (host_cp != 0 && host_cp != MH_ChatInput_Codepage()) {
            if (MH_ChatInput_AdoptCodepage(host_cp))
                wsprintfA(cb, "; F3c: adopted the host's input codepage %u for this session (ours is %u)\n",
                          host_cp, own_cp);
            else
                wsprintfA(cb, "; F3c: host pins input codepage %u, ours is %u and this peer will not switch "
                              "([input] codepage_adopt=0, or not installed) -- expect the host to refuse\n",
                          host_cp, own_cp);
            seam_log(cb);
        }
    }
    uint8_t                   mine[mh_net_proto::MAP_HASH_BYTES] = {0};
    const bool                have_map                           = mh::seams::maps::client_my_hash(mine);
    mh_net_proto::JoinRequest jr                                 = mh_net_proto::join_request_for(
        *rec, (uint16_t)MH_ChatInput_Codepage(), have_map ? mine : nullptr);
    fill_my_player_name(jr); // S6: my player name (re-encoded under the adopted codepage, F3c)
    uint8_t buf[mh_net_proto::JOIN_REQUEST_MAX_ENCODED];
    int     n = (int)mh_net_proto::join_request_encode(jr, buf);
    MH_Net_SendJoin(buf, n);
    // SES0: name the echoed match_id here too. The `; [session]` line above is the one tools parse;
    // this one is for a human reading the JOIN in sequence -- it says the echo carried the host's id
    // rather than a nil placeholder, which is the failure a correlated pair would otherwise hide.
    char hex[mh_net_proto::UUID7_HEX_CAP];
    mh_net_proto::uuid7_hex(jr.match_id, hex, sizeof(hex));
    char b[192];
    wsprintfA(b, "; S4 join: sent JOIN '%s' for '%s#%08X' match_id=%s (%d B) to host\n",
              jr.player_name, jr.name, jr.tag, hex, n);
    seam_log(b);
    // SES1: the CLIENT's session opens HERE -- at the JOIN, which is the moment this peer commits to
    // a particular lobby. Not at on_session_info_recv: an advert only means the browser listed a
    // host, and a player who scrolls past three lobbies would otherwise mint three directories.
    // The slot is the join-time one (mp_client_slot); the host may reassign it in its WELCOME, and
    // that assignment is in mh_net.log where it has always been. The directory name keeps the
    // join-time label so the folder is nameable before the WELCOME arrives.
    mp_session_open(jr.match_id, mp_client_slot());
}

// ---- mp:X2 -- THE COMPLETION REPORT, which is a re-sent JOIN ------------------------------------
//
// WHY A RE-SEND AND NOT A NEW FRAME. The host's Start gate is "does every seated joiner report our
// map hash", and the only place a peer reports that is its JOIN. A finished download therefore has
// exactly one thing to say and the JOIN already says it, so the completion is the same message with
// a new value in the same field -- no second frame, no second retry policy, and no second piece of
// host state to reconcile with the first.
//
// A RE-JOIN IS SAFE ON THE HOST'S SIDE, which is the part worth checking rather than assuming:
// `on_join_recv` re-admits by name+tag, clears any stale left-mark, re-copies the same player name
// and re-publishes the same joined flag. Every one of those is idempotent, so a second JOIN from a
// seated peer changes nothing except the map answer it came to change.
//
// It does NOT re-open the session directory (that is `on_join_connect`'s, at the player's click) --
// this is a smaller function on purpose: the lobby-id, the player name and the codepage are re-read
// from the same sources, and the map hash is re-read from the seam that just stored a file.
void mp_join_resend_map_report() {
    const mh_net_proto::SessionInfo *rec = g_store_valid ? &g_store_rec : relay_picked_rec();
    if (rec == nullptr) return;
    uint8_t                   mine[mh_net_proto::MAP_HASH_BYTES] = {0};
    const bool                have_map                           = mh::seams::maps::client_my_hash(mine);
    mh_net_proto::JoinRequest jr                                 = mh_net_proto::join_request_for(
        *rec, (uint16_t)MH_ChatInput_Codepage(), have_map ? mine : nullptr);
    fill_my_player_name(jr);
    uint8_t   buf[mh_net_proto::JOIN_REQUEST_MAX_ENCODED];
    const int n = (int)mh_net_proto::join_request_encode(jr, buf);
    MH_Net_SendJoin(buf, n);
    char hex[mh_net_proto::MAP_HASH_HEX_CAP];
    char b[192];
    wsprintfA(b, "; [map] client reported the stored map to the host (re-JOIN, sha=%s)\n",
              mh_net_proto::map_hash_hex(mine, hex, sizeof(hex)));
    seam_log(b);
}

extern "C" int MH_Seam_ClientStartReceived(void) { return g_client_start_received; }

// U28: the host's AUTHORITATIVE lobby slot array, as shipped with FLAG_START. Written on the recv
// thread, read once by mp_lobby_entry_tick on the main thread just before begin_map_load. The
// g_start_slots_valid latch is the publish and g_client_start_received (set AFTER it) is the fence --
// the entry tick cannot pass its Start gate until these bytes are already in place.
constexpr int START_SLOTS_BYTES = 0x39 * 8; // stride 0x39, 8 slots -- must match ADDR_LOBBY_SLOTS
unsigned char g_start_slots[START_SLOTS_BYTES];
volatile LONG g_start_slots_valid = 0;
// U29: disarm a stale Start when the client leaves the lobby. Without this a FLAG_START that arrived
// (or arrives) while the client is off on the browser is still latched, and the entry driver would
// act on it against the dead lobby's slots.
// U29 (b) TEST HOOK: latch the client's Start flag exactly as a BARE retail FLAG_START would --
// the same single write on_start_recv performs, with no slot payload, so g_start_slots_valid stays 0.
// It exists because (b) asks for something that cannot be arranged by playing the game: a FLAG_START
// arriving while the client sits on the discovery browser after a host-left. On the real wire that is
// a race against the host's own timing (the 2026-08-28 run missed it by seconds), and a race is not a
// test. Harness-only -- reached solely from the [uitest] interpreter's `injectstart`, which is absent
// from a shipping run.
extern "C" void MH_Seam_InjectStartReceived(void) {
    InterlockedExchange(&g_client_start_received, 1);
    seam_log("; U29 TEST HOOK: injected a bare FLAG_START (no payload) -- entry must NOT happen here\n");
}

extern "C" void MH_Seam_ClearStartReceived(void) {
    InterlockedExchange(&g_client_start_received, 0);
    InterlockedExchange(&g_start_slots_valid, 0); // U28 payload belongs to that Start; drop it too
}

// Returns START_SLOTS_BYTES and fills `out` iff the host shipped a full array with its Start; 0 if it
// sent the legacy bare signal, in which case the client keeps its own slots (pre-U28 behaviour).
extern "C" int MH_Seam_TakeStartSlots(unsigned char *out, int cap) {
    if (!g_start_slots_valid || out == nullptr || cap < START_SLOTS_BYTES) return 0;
    memcpy(out, g_start_slots, START_SLOTS_BYTES);
    return START_SLOTS_BYTES;
}

// Client (recv thread): the host clicked Start -> adopt its slot array, then release the entry gate.
void on_start_recv(int sender, const unsigned char *buf, int len) {
    char b[192];
    // ORDER MATTERS: publish the slots BEFORE the gate. The entry tick polls the gate on the main
    // thread and builds Players[] immediately after it opens, so releasing first would let it win the
    // race and build from its own stale copy -- which is the entire bug this payload exists to close.
    if (buf != nullptr && len == START_SLOTS_BYTES) {
        memcpy(g_start_slots, buf, START_SLOTS_BYTES);
        InterlockedExchange(&g_start_slots_valid, 1);
    } else if (len != 0) {
        // A wrong-sized payload is a BUILD MISMATCH, not something to paper over -- adopting a partial
        // array would be worse than adopting none. Say so, and fall back to the legacy behaviour.
        wsprintfA(b,
                  "; U28 START payload is %d bytes, expected %d -- IGNORED (peer build mismatch?); the "
                  "client will build from its OWN slots and may desync\n",
                  len, START_SLOTS_BYTES);
        seam_log(b);
    }
    InterlockedExchange(&g_client_start_received, 1);
    // X2: sample the player's OWN file under the advertised map name one last time. Paired with the
    // `; [map] local before` line, the two are the evidence a download never touched it.
    mh::seams::maps::client_on_start();
    wsprintfA(b, "; U2 START from %d -> client entering the game (U28 host slots: %s)\n", sender,
              g_start_slots_valid ? "ADOPTED" : "none sent -- using our own");
    seam_log(b);
}

// The role is implied by which stub fires: the client reaches discovery, the host reaches advertise.
// Set IS_HOST/LOCAL_PLAYER_INDEX so the lazily-started transport connects the right direction, AND
// game::g::PlayerSide -- the client MUST drive player 1+ (else both peers are player 0 -> same start/
// color/race -> lockstep drops the phantom). This is the manual-menu-join counterpart of the
// force-entry's per-frame PlayerSide re-affirm (launch.cpp).
// Client discovery poll. Force-entry keeps the proven unconditional synth; a manual client (real discovery)
// builds the browser from the received-record store -- empty until it connects to the typed host + gets its
// SESSION_INFO (S3). The connect + the re-arm that re-fires this poll are driven by MH_Seam_ClientDiscoveryTick.
void on_discover_poll() {
    int slot                    = mp_client_slot(); // N1: own/assigned slot (1 until the host WELCOMEs us / declared id)
    *g_a.is_host                = 0;
    *(int *)ADDR_NET_LOCALIDX   = slot;
    *(int *)ADDR_NET_PLAYERSIDE = slot;
    MH_MP_ArmManualLobby();
    if (!MH_MP_IsManual()) {
        build_synth_session();
        return;
    } // force-entry: proven synth (determinism path)
    // The poll fires on browser-open + the "update list" refresh (the player's explicit "go", so the typed
    // IP is complete by then). Kick the async connect to it once; the record then auto-lists (tick re-arm).
    s3_kick_connect("browser refresh");
    build_browser_from_store(); // manual: real record or empty
}
// U42/U43 (ruling Q8 -- ARM and explain, not gate): U42 believed this call site was already reached
// on a module=none host because "F3F left the seven protocol installs ungated on transport_present()"
// -- but the INSTALL of this detour (net_seams.cpp install_mp_bootstrap) was, in fact, grouped with
// the other six transport-dependent ones and skipped whenever `!transport_present()`, so this
// function body never ran at all on that lane: _G_LLM_NET_IS_HOST stayed at its game-init default of
// 0 (it is written NOWHERE ELSE in the binary -- confirmed by an EN find-cross-references sweep), so
// a manual "host" silently took retail's CLIENT branch in both llm_lobby_screen_open (Start stays
// HIDDEN) and llm_lobby_host_net_dispatch (the slot-row build never runs). That was the U43 bug: the
// whole slot-row panel empty, not even the host's own row, and Start absent outright.
//
// U43 fixed the INSTALL SITE (net_seams.cpp install_mp_bootstrap): this detour now installs
// UNCONDITIONALLY, because its body below is role-marking only and touches the wire not at all --
// it is exactly as safe as the nine UI-only installs net_seams.cpp already ran regardless of
// transport_present(). What Q8 also asked for is the explanation, and that does NOT belong here --
// this file is net-owned and mh::ui is the only thing allowed to touch a widget (the D4 boundary
// lobby_ui.h documents) -- it is already covered: MH_UI_Arm (net_seams.cpp) arms
// mh::ui::browser_notice_arm_no_module() once, unconditionally, whenever !transport_present(), and
// that notice paints on the manual lobby this call reaches, not only on the browsers (see
// lobby_notice.cpp / lobby_ui.h). No second arm call is needed here.
void on_host_advertise() {
    *g_a.is_host                = 1;
    *(int *)ADDR_NET_LOCALIDX   = 0;
    *(int *)ADDR_NET_PLAYERSIDE = 0;
    MH_MP_ArmManualLobby();
    // mp:R7 -- CREATE GAME AFTER A RELAY BROWSE. With a relay configured the first browser dials
    // the relay as a CLIENT to list the directory (see MH_Seam_ClientDiscoveryTick), so a player
    // who then clicks Create game has a client-role transport already up, and lazy_start would
    // keep it (g_tried_init) -- a host on a client socket, registering nothing. Hand it the U40
    // relink instead: the host's first PollRecv re-enters MH_Net_InitEx with role=host, which the
    // modules implement as stop + start (T1b). One-shot by the latch, so a per-frame caller of
    // this stub could not re-init a running host.
    if (InterlockedExchange(&g_r7_browse_dialled, 0) != 0 && MH_MP_IsManual()) {
        MH_Seam_ResetTransportInit(); // a still-failing browse dial also left g_tried_init set
        if (MH_Net_IsStarted()) InterlockedExchange(&g_net_relink, 1);
        InterlockedExchange(&g_s3_conn_kicked, 0);
        InterlockedExchange(&g_s3_conn_done, 0);
        seam_log("; R7: Create game after a relay browse dial -> the transport re-initialises as HOST "
                 "on the lobby's first poll\n");
    }
}

// ---- mp:R7: the FIRST browser lists the relay directory ------------------------------------
// The screen after the player name (llm_mp_local_browser_setup: Refresh list / Create game /
// Internet server / Join game) is retail's LAN enumerator, and it is dead in this build: its refresh
// (llm_mp_discovery_browser_refresh) loops over _G_LLM_MP_PROBE_SERVER_COUNT typed servers, which
// has ONE writer (the reset to 0 at boot) and no incrementer, so it polls nothing and the only way
// to a listed game was Internet server -> IP window -> Connect. With a relay configured the count
// is set to 1, so retail's own loop runs one probe: connect_prep(0) (our no-op), then the
// discovery-poll stub for ~2 s -- on_discover_poll, which kicks the relay dial and builds the list
// from the relay rows -- then the row build into the SAME arrays (_G_LLM_MP_BROWSER_ROW_*) the
// shared Join button (llm_lobby_join_head) reads; the row-select callback takes the NET_MODE<0
// branch that re-probes, which is the same detour again. Nothing else reads the count (measured
// 2026-09-19).
//
// WHERE THE WRITE LIVES, and why not the menu tick: MH_Seam_ClientDiscoveryTick only runs once
// g_manual_mp is set, and that is set BY the discovery poll -- which the first browser never
// reaches while the count is 0. The one retail call that precedes the refresh on that screen is
// llm_net_disconnect_stub, the first statement of llm_mp_local_browser_setup, and it was already
// ours (a return-0 no-op since U10). So the no-op grew one write. It is also called from the
// row-select path and the session browser, where re-writing 1 changes nothing.
// mp:R7a -- the SAVED-SERVER count: the `+0xc` MRU-entry-count field of the connect-IP field record
// _G_LLM_UI_NETSETUP_FLD_IP (0x656d9e; llm_mp_netsetup_field_build_mru reads param[3] at +0xc, record
// stride 0x18). Loaded at boot from setup.dat by llm_cfg_setup_dat_read_mru_lists, so it is readable
// on the first browser before any field is shown. Non-zero = the player has a server to dial directly.
constexpr uintptr_t ADDR_IP_MRU_COUNT = 0x00656daau;

void on_net_disconnect_stub() {
    if (!relay_configured()) return;
    if (*(int *)mh::addr::_G_LLM_MP_PROBE_SERVER_COUNT == 1) return;
    // mp:R7a -- THE FIRST BROWSER PROBES THE RELAY ONLY WHEN THE PLAYER HAS NO DIRECT TARGET. R7's own
    // done_when scopes the first-browser relay-discovery to "NO saved/typed server address": a player
    // WITH a saved server means to reach it by *Internet server* + a direct dial, and auto-probing the
    // relay would put a directory leg on the wire (a relay HELLO, a `leg UP`) for a connection that is
    // meant never to touch the relay. So probe only when the relay is FORCED (force_relay pins the
    // relayed path regardless) or there is no saved server -- otherwise leave the first browser dead
    // and let the player take the direct path. Without this, direct_dial_with_relay_set could not have
    // a truly untouched relay.
    if (!relay_force() && *(const int *)ADDR_IP_MRU_COUNT > 0) {
        seam_log("; R7a: a saved server exists -> the first browser does NOT probe the relay (the "
                 "direct path via Internet server is available; PROBE_SERVER_COUNT left 0)\n");
        return;
    }
    *(int *)mh::addr::_G_LLM_MP_PROBE_SERVER_COUNT = 1;
    seam_log("; R7: relay configured -> the first browser probes the directory (PROBE_SERVER_COUNT=1)\n");
}
__declspec(naked) void net_disconnect_detour() { // disconnect stub: arm the first browser's probe, return 0
    __asm {
        pushad
        pushfd
        call on_net_disconnect_stub
        popfd
        popad
        xor  eax, eax
        ret
    }
}

__declspec(naked) void discover_poll_detour() { // client: synth record, return 0 (>=0 = ok)
    __asm {
        pushad
        pushfd
        call on_discover_poll
        popfd
        popad
        xor  eax, eax
        ret
    }
}
__declspec(naked) void host_advertise_detour() { // host: mark role, return 0
    __asm {
        pushad
        pushfd
        call on_host_advertise
        popfd
        popad
        xor  eax, eax
        ret
    }
}
__declspec(naked) void ret_zero_detour() { // connect-prep / disconnect / map-send no-op
    __asm {
        xor  eax, eax
        ret
    }
}

// Client join-connect stub (llm_net_join_connect_stub, called from llm_lobby_join_handler on the explicit
// "join selected session" click): send our JOIN control frame, then return 0 (>=0 = success) so the retail
// handler proceeds into the lobby. Registers preserved across the send (the caller reads only EAX). (S4)
__declspec(naked) void join_connect_detour() {
    __asm {
        pushad
        pushfd
        call on_join_connect
        popfd
        popad
        xor  eax, eax
        ret
    }
}

// Client map-recv step (Phase 2): the retail stub loops until DAT_005d54c8 != 0, which nothing ever
// sets -> hang. Since both peers already hold the map, set the done flag so the client's async_step loop
// exits on the first iteration into build_players_from_slots + session_begin_multi.
void                   on_map_recv_step() { *(volatile int *)ADDR_MAP_RECV_DONE = 1; }
__declspec(naked) void map_recv_step_detour() {
    __asm {
        pushad
        pushfd
        call on_map_recv_step
        popfd
        popad
        xor  eax, eax
        ret
    }
}

// Scrollbar draw guard (FUN_004c18a6). param_1 (the widget) arrives in EAX; param_block = [eax+0x30].
// The browser scrollbar draw guard moved to mh/ui/lobby_widgets.cpp at fork F3E (D4): an
// empty-list draw crash is a widget defect, and its body reads nothing from the wire.

// S3/S5-core: per-menu-frame driver for a MANUAL CLIENT's real discovery (called from launch.cpp
// on_menu_tick). Runs on the main thread so the browser re-arm is race-free. (1) Once the player has
// typed a valid host IP, kick the async connect to it. (2) Once connected AND the host's SESSION_INFO has
// arrived, re-arm the browser's one-shot poll so it re-lists from the store (showing the real game).
// Client-only + one-shot per step; the host + force-entry never reach here (on_menu_tick gates on manual).
extern "C" void MH_Seam_ClientDiscoveryTick(void) {
    if (!g_a.is_host || *g_a.is_host) return; // manual CLIENT only
    // ---- mp:R2: the relay's session directory ----------------------------------------------
    // Drained on the MAIN thread, per menu frame, for the same reason the S3 auto-list re-arm is:
    // the browser re-arm is a write into mh.exe's menu state and the recv/tunnel threads may not
    // touch it. A change in the visible set re-fires the browser's one-shot poll, so a lobby that
    // appears on (or leaves) the relay shows up without the player clicking anything.
    //
    // ONLY WHILE THE SESSION BROWSER IS THE ACTIVE SCREEN, and that gate is load-bearing rather
    // than tidy. `on_menu_tick` does NOT stop at the match: U29DIAG keeps sampling gm=3 with
    // list=00000000 for the whole in-game half, so this seam runs every frame of a running game
    // too. Ungated, the host's own match start (which WITHDRAWS its lobby from the relay) made the
    // row vanish mid-match, and the vanish handler then wrote mh.exe's menu one-shot and dropped
    // g_store_valid while the client was playing. Measured twice on the rig (2026-09-18,
    // determinism.red-…T051054Z and …T053818Z): the client left the match ~4.5 s after entering it,
    // both peers stalled at ~3.2 s of game clock, and the run produced no comparable steps. The
    // 300-step gate had been passing only because it stops ~20 steps short of that. The browser is
    // also the only screen that can DO anything with a directory row, so nothing is lost: rows that
    // arrive elsewhere wait in the inbox and are drained the moment the browser comes up.
    const unsigned r2_wl = *(const unsigned *)mh::addr::_G_LLM_UI_MENU_WIDGET_LIST;
    const bool     r2_browser =
        (r2_wl == mh::addr::browser_widget_array_ptr || r2_wl == mh::addr::local_browser_widget_origin);
    if (relay_configured() && r2_browser) {
        if (relay_rows_drain() && !g_store_valid) {
            void *fn = *(void **)ADDR_MENU_REFRESH_FN;
            if (fn) *(void **)ADDR_MENU_ONESHOT = fn;
            seam_log("; R2: relay directory changed -> re-arming the browser\n");
        }
        // DIAL THE LISTED LOBBY'S ROOM. The first dial can only guess (`[net] port`, the module's
        // own default); once the directory has named a room, a peer that is not connected to
        // anything re-dials into it. That is also mp:R1e's footgun gone: two peers who disagree
        // about `[net] port` now agree about a room the DIRECTORY named, not one they both had to
        // configure. Gated on having no link at all, so this can never tear down a working one.
        const mh_net_proto::SessionInfo *pick = relay_picked_rec();
        if (pick != nullptr && !g_store_valid && MH_Net_PeerCount() == 0) {
            const uint32_t want     = g_relay_rows[g_relay_pick].room;
            const bool     mismatch = !g_relay_room_set || want != g_relay_dialled;
            // mp:R7 -- ...OR THE LAST DIAL LANDED NOWHERE. A peer that browsed BEFORE the host
            // existed dialled the default room, was told "not hosted" and was parked on a
            // directory-only leg; when the host's row then appears with that SAME room number the
            // mismatch test is false, and until R7 the S8 dead-IP retry below re-dialled every 2 s
            // by accident (PeerCount 0 read as a failed connect). S8 no longer fires for a relayed
            // client, so this is where a listed row re-dials: the last dial is finished
            // (g_s3_conn_done), no host advert came of it, and a row is listed -- dial its room.
            // Cadence = one dial's completion plus a 1.5 s grace -- a dial that DID land in a hosted
            // room reports done within ~50 ms and its advert (g_store_valid) follows within ~400 ms,
            // so the grace is what keeps this from tearing down a link that is still handshaking. A
            // row the relay stops listing expires (RELAY_ROW_TTL) and the retries stop with it.
            const bool idle_after_dial = g_s3_conn_kicked && g_s3_conn_done &&
                                         (GetTickCount() - (DWORD)g_s3_conn_done_at) > 1500;
            if (mismatch || idle_after_dial) {
                char b[160];
                wsprintfA(b, "; R2: the directory lists room %u and we dialled %u -> %s\n", (unsigned)want,
                          (unsigned)(g_relay_room_set ? g_relay_dialled : 0),
                          mismatch ? "re-dialling" : "re-dialling (the last dial found no host there)");
                seam_log(b);
                InterlockedExchange(&g_net_relink, 1);
                MH_Seam_ResetTransportInit();
                InterlockedExchange(&g_s3_conn_done, 0);
                InterlockedExchange(&g_s3_conn_kicked, 0);
                s3_kick_connect("relay directory");
            }
        }
    }
    // U40: A CLIENT ALREADY SITTING IN THE BROWSER RE-DIALS BY ITSELF. The discovery poll only fires
    // on browser-OPEN and on the explicit "update list", so without this the user's exact reported
    // shape -- peers left in the browser while the host re-creates -- would need a click nobody knows
    // to make. Gated three ways so it can only mean what it says: the RELINK latch (a match of ours
    // ended), a one-shot kick latch, and the session browser actually being the active screen. That
    // last gate is why this is not simply the poll's condition moved up a level: a first dial still
    // belongs to the player pressing Connect, and the ip_retry/S8 flow depends on it.
    if (g_net_relink) {
        const unsigned wl = *(const unsigned *)mh::addr::_G_LLM_UI_MENU_WIDGET_LIST;
        if (wl == mh::addr::browser_widget_array_ptr || wl == mh::addr::local_browser_widget_origin)
            s3_kick_connect("still in the browser");
    }
    // Auto-list: once connected AND the host's SESSION_INFO has arrived, re-arm the browser's one-shot poll
    // so it re-lists from the store (no second manual refresh). The connect is kicked from on_discover_poll
    // (the "update list" refresh). Main thread => the re-arm write is race-free.
    if (MH_Net_IsStarted() && g_store_valid && !g_s3_listed) {
        InterlockedExchange(&g_s3_listed, 1);
        // mp:R6 -- ONLY WHILE A BROWSER IS THE ACTIVE SCREEN. ADDR_MENU_ONESHOT is
        // _G_LLM_UI_MENU_ASYNC_CALLBACK_A, the slot the LOBBY's own frame callback
        // (llm_lobby_host_net_dispatch) lives in once the lobby is up; writing the browser's rescan
        // into it from inside the lobby replaces that callback, and the lobby dispatch never runs
        // again -- no poll, no keepalive, a client seated in a lobby that stopped listening.
        // Unreachable while the first dial always landed on the host (the store was valid on the
        // browser); since R6 a host's room is minted, the link comes up after the row was clicked,
        // and the record can land with the lobby already pushed (measured 2026-09-19: 16 ms after).
        const unsigned s3_wl = *(const unsigned *)mh::addr::_G_LLM_UI_MENU_WIDGET_LIST;
        if (s3_wl == mh::addr::browser_widget_array_ptr || s3_wl == mh::addr::local_browser_widget_origin) {
            void *fn = *(void **)ADDR_MENU_REFRESH_FN; // the browser's registered rescan
            if (fn) *(void **)ADDR_MENU_ONESHOT = fn;  // re-arm the one-shot -> re-lists from the store
            seam_log("; S3: record received -> re-arming browser to list the real host\n");
        } else {
            seam_log("; S3: record received with the browser not on screen -> no re-arm (the lobby "
                     "owns the async callback slot; mp:R6)\n");
        }
    }
    // mp:R6 -- THE RE-SENT JOIN. A peer is connected now, so a JOIN sent while the link was still
    // coming up (dropped by the transport, see g_join_pending) goes again -- if the link is into
    // the lobby the player clicked: by the advert when the store already holds it, by the room the
    // dial was for otherwise. On the FIRST tick with a peer, not on the advert's arrival: the retail
    // lobby's own first dispatch sends its 0x17 "client hello", which the host answers with the slot
    // table only for an ADMITTED peer, and a JOIN that lands after that hello leaves the client
    // seated in an empty lobby (measured: the advert-triggered form lost by 2 ms). The host side
    // closes the last of that window too -- net_seams.cpp replays an early hello at admission.
    // Independent of the g_s3_listed one-shot above, which a client that browsed an earlier host
    // has already spent.
    if (MH_Net_IsStarted() && MH_Net_PeerCount() > 0 &&
        InterlockedCompareExchange(&g_join_pending, 0, 0) != 0) {
        InterlockedExchange(&g_join_pending, 0);
        const bool same = g_store_valid ? mh_net_proto::session_same_lobby(g_store_rec, g_join_pending_rec)
                                        : (g_relay_room_set && g_relay_dialled == g_join_pending_room);
        if (same) {
            seam_log("; S4 join: the link into the relay-listed lobby is up -> re-sending the JOIN "
                     "that was dropped while it came up (mp:R6)\n");
            g_join_resending = true;
            on_join_connect();
            g_join_resending = false;
        } else {
            char id_want[64];
            mh_net_proto::lobby_id_str(g_join_pending_rec, id_want, sizeof(id_want));
            char b[192];
            wsprintfA(b, "; S4 join: pending JOIN for %s (room %u) dropped -- the link came up "
                         "somewhere else (room %u) (mp:R6)\n",
                      id_want, (unsigned)g_join_pending_room,
                      (unsigned)(g_relay_room_set ? g_relay_dialled : 0));
            seam_log(b);
        }
    }
    // S8: the kicked connect FINISHED but the transport never started (dead/typo'd IP -> start_client's
    // connect() failed -> g_started stays false, MH_Net_InitEx is re-callable). Clear the one-shot latches so
    // the NEXT Connect -- after the player corrects the IP (re-selects a saved MRU entry / retypes) -- re-kicks
    // the connect with the new IP, instead of being ignored until a game restart.
    // U40 widened the failure test from `!MH_Net_IsStarted()` to `PeerCount() == 0`, which is the
    // same thing for a first dial (an unstarted transport has no peers) and is ALSO true for the two
    // ways a RELINK can fail: the re-dial's connect() failed, or net_reset refused to stop the old
    // transport. Without this a refused relink left g_s3_conn_kicked latched and the client could
    // never try again -- i.e. it would fall back to exactly the pre-U40 dead end this item is about.
    // mp:R7 -- FOR A RELAYED CLIENT ONLY A DIAL THAT NEVER STARTED THE TRANSPORT IS A FAILURE. A
    // relay leg that is up with zero peers is the normal state of a player browsing an empty
    // directory (the module parks it on a directory-only leg when the room is not hosted), and
    // reading that as "dead IP" re-dialled the relay every 2 s for as long as the browser was open
    // (measured on the first R7 run: the host's own box, browsing before it created its game). The
    // relayed re-dial for a LISTED row is the R2 block above; this branch keeps only the case where
    // there is nothing to browse because the dial itself failed, so Refresh list can try again.
    const bool s8_failed = relay_configured() ? !MH_Net_IsStarted() : (MH_Net_PeerCount() == 0 && !g_store_valid);
    if (g_s3_conn_kicked && g_s3_conn_done && s8_failed) {
        MH_Seam_ResetTransportInit(); // net_seams: clear g_tried_init (lazy_start re-runs)
        InterlockedExchange(&g_s3_conn_done, 0);
        InterlockedExchange(&g_s3_conn_kicked, 0); // re-arm on_discover_poll to re-kick the corrected IP
        InterlockedExchange(&g_s8_retry_armed, 1); // S8(b): retry-ready -- the `retryready` UI-test gate holds now
        // lazy_start CONSUMED the relink latch on the attempt that just failed; a transport still
        // marked started needs it back, or the retry would be refused by lazy_start's own guard.
        if (MH_Net_IsStarted()) InterlockedExchange(&g_net_relink, 1);
        seam_log("; S8: connect failed (dead IP) -> cleared latches; a corrected IP retries on next Connect\n");
        // mp:R7a -- item (3): a DIRECT dial that failed while a relay IS configured. The typed IP is a
        // genuine direct dial (Internet server + address), so the host's port has to be reachable --
        // port-forwarded / not firewalled -- exactly as it did before a relay existed; the relay does
        // NOT rescue a typed direct dial. Reuse the existing S8 notice path (no new UI): tell the
        // player their alternative is the first browser, whose dials go through the configured relay.
        if (relay_configured() && !mp_dial_is_relayed())
            seam_log("; S8: (relay configured) that was a DIRECT dial to a typed address -- the host's "
                     "port must be reachable (forwarded/open); to reach it through the relay instead, "
                     "use the first browser (Refresh list), whose dials are relayed (mp:R7a)\n");
    }
}
