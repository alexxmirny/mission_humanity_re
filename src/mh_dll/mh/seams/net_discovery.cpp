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
#include "include/mh_seam_export.h"    // MH_Seam_StartTransport (async connect kick)
#include "mh_net_proto/session_info.h" // SESSION_INFO descriptor + lobby-id (S1/S2 discovery)
#include "addr/mh_addrs.gen.h"         // generated EN VAs (tools/gen_dll_addrs.py)
#include "net_internal.h"              // shared spine: g_a, g_ini, g_host_join_seen, seam_log, mp_client_slot
#include "hook/watcall.h"              // call_watcall1 (Watcom __watcall(EAX) bridge)

#pragma comment(lib, "user32.lib") // wsprintfA

using mh::hook::call_watcall1;

extern "C" int  MH_MP_IsManual(void);       // launch.cpp -- 1 = pure manual session (gate manual-only work)
extern "C" void MH_MP_ArmManualLobby(void); // launch.cpp -- run the proven lobby driver for the manual path

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
volatile LONG g_exit_cause     = 0; // 0 = none, 1 = MH_MP_EXIT_HOST_LEFT, 2 = MH_MP_EXIT_LINK_LOST
volatile LONG g_s3_conn_kicked = 0; // 1 once we've kicked the connect to the typed host
volatile LONG g_s3_conn_done   = 0; // S8: 1 once the kicked connect thread FINISHED (ok or failed)
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
constexpr uint16_t  MH_MP_HOST_VERSION  = 1;    // bump on a wire/behaviour change

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

uint32_t mp_gen_tag() {
    // Non-sim RNG: wall clock + process/thread ids, xorshift-mixed. Never touches the lockstep RNG/seed.
    uint32_t s = GetTickCount() ^ (GetCurrentProcessId() << 16) ^ GetCurrentThreadId();
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s ? s : 0xA5A5A5A5u; // avoid the 0 "not generated" sentinel
}

void mp_build_host_session_info(mh_net_proto::SessionInfo &si) {
    if (g_mp_tag == 0) g_mp_tag = mp_gen_tag();
    si              = mh_net_proto::SessionInfo{};
    si.tag          = g_mp_tag;
    si.host_version = MH_MP_HOST_VERSION;
    si.protocol     = (uint16_t)*(const int *)ADDR_PROTO_CEIL;
    int mapmax      = *(const int *)ADDR_MAP_PCOUNT; // the map's own player capacity (2 for a 2-player map)
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
    MH_Seam_StartTransport();                // lazy_start -> MH_Net_InitEx -> start_client (bounded connect, ~4s max on a dead IP -- S8)
    InterlockedExchange(&g_s3_conn_done, 1); // S8: the attempt finished -- MH_Net_IsStarted() is now authoritative
    return 0;
}

// S3: build the discovery browser from the received-record store instead of fabricating. Empty store ->
// NO game listed (kills the "MH Host" fabrication). One record -> a real, joinable row carrying the host's
// real name + player counts. Map header is a placeholder (not shown in the row; corrected at join by the
// host's current_map_data broadcast). Reuses the g_synth_desc buffer + the SD_* layout of build_synth_session.
void build_browser_from_store() {
    if (!g_store_valid) { // no host reachable -> empty browser
        *(void **)ADDR_SESSION_LIST_PTR = nullptr;
        *(int *)ADDR_SESSION_COUNT      = 0;
        *(void **)ADDR_UI_ROW_ARRAY     = nullptr;
        return;
    }
    ensure_placeholder_maphdr(); // a VALID header (not raw current_map_data)
    memset(g_synth_desc, 0, sizeof(g_synth_desc));
    // S9: prefer the host's REAL map header (shipped in the v2 SESSION_INFO) so the browser PREVIEW shows
    // the actual biome/size; fall back to the placeholder for a v1 host (has_map_header == false).
    if (g_store_rec.has_map_header)
        memcpy(g_synth_desc + SD_MAPHDR, g_store_rec.map_header, MAP_DATA1_SIZE); // REAL host map header (preview)
    else
        memcpy(g_synth_desc + SD_MAPHDR, g_map_hdr_placeholder, MAP_DATA1_SIZE); // placeholder (v1 host)
    *(void **)(g_synth_desc + SD_HANDLE) = (void *)1;                            // sentinel handle (join matches ==1)
    bs_str_copy((char *)(g_synth_desc + SD_NAME), g_store_rec.name, 32);         // REAL host game name
    // S7: render the count as exactly "occ/cap". install_mp_bootstrap patches the row format string
    // (browser_row_count_fmt 0x503068) from u"%s\t%d+%d/%d" -> u"%s\t%d/%d", so the NORMAL-branch
    // renderer emits "<first>/<second>" where first = total_slots - player_count, second = player_count.
    //   second = cap  -> SD_PCOUNT = cap (= g_store_rec.max_players, the host's non-closed capacity)
    //   first  = occ  -> SD_TOTAL - player_count = occ -> SD_TOTAL = occ + cap
    // giving (SD_TOTAL - SD_PCOUNT)/SD_PCOUNT = occ/cap. occ = g_store_rec.cur_players (host+AI+humans);
    // it climbs by 1 as a peer joins. These fields are display-only (read ONLY by the two row renderers).
    int occ = g_store_rec.cur_players;                      // occupied incl AI
    int cap = g_store_rec.max_players;                      // capacity after closed slots
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
    if (peers > 0) MH_Net_SendSessionInfo(buf, n);
}

// Client side (recv thread): store each received SESSION_INFO. The main-thread MH_Seam_ClientDiscoveryTick
// then re-arms the browser to list it (S3). Write the record BEFORE publishing g_store_valid (x86 TSO).
void on_session_info_recv(int sender, const unsigned char *buf, int len) {
    mh_net_proto::SessionInfo si;
    if (!mh_net_proto::session_info_decode(buf, len, si)) {
        seam_log("; S3 recv: malformed SESSION_INFO\n");
        return;
    }
    g_store_rec = si;
    InterlockedExchange(&g_store_valid, 1);
    char id[64];
    mh_net_proto::lobby_id_str(si, id, sizeof(id));
    char b[192];
    wsprintfA(b, "; S3 recv SESSION_INFO from %d: %s map=%s players=%d/%d ver=%d -> store\n",
              sender, id, si.map, si.cur_players, si.max_players, si.host_version);
    seam_log(b);
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
// 0 = neither (a deliberate Cancel, which must show the player nothing).
extern "C" int MH_MP_ConsumeExitCause(void) { return InterlockedExchange(&g_exit_cause, 0); }

// U13: the manual HOST left its lobby (Cancel = llm_lobby_host_start_game). Reset this host's session
// IDENTITY + join state so a re-created game is a DISTINCT lobby (fresh tag -> new lobby-id, no ghost/dedup
// collision) and the still-connected client is NOT treated as pre-joined (it must send a fresh JOIN).
extern "C" void MH_MP_HostResetSessionIdentity(void) {
    g_mp_tag = 0;                              // next mp_build_host_session_info mints a FRESH tag
    InterlockedExchange(&g_host_join_seen, 0); // re-arm the S4 gate (no auto-slot of the old peer)
    for (int i = 0; i < 8; ++i) {
        g_peer_player_name[i][0] = '\0';
        InterlockedExchange(&g_lobby_joined[i], 0);
        InterlockedExchange(&g_lobby_left[i], 0);
    }
    seam_log("; U13: host left lobby -> reset session identity (fresh tag on re-create) + join gate\n");
}

// Host (recv thread): a client asked to join. Validate the lobby-id against our own advertised session
// (name+tag); on a match, open the join gate so the next lobby dispatch slots + floods this peer.
void on_join_recv(int sender, const unsigned char *buf, int len) {
    mh_net_proto::JoinRequest jr;
    if (!mh_net_proto::join_request_decode(buf, len, jr)) {
        seam_log("; S4 recv: malformed JOIN\n");
        return;
    }
    mh_net_proto::SessionInfo mine;
    mp_build_host_session_info(mine); // our current lobby-id (name+tag)
    char b[192];
    if (mh_net_proto::join_matches_session(jr, mine)) {
        InterlockedExchange(&g_host_join_seen, 1);
        InterlockedExchange(&g_lobby_left[sender & 7], 0);             // U12: a (re)join clears any stale left-mark
        lstrcpynA(g_peer_player_name[sender & 7], jr.player_name, 32); // S6: remember the joiner's name FIRST...
        InterlockedExchange(&g_lobby_joined[sender & 7], 1);           // N2: ...THEN publish joined (release fence),
        //   so the main-thread mirror can never observe joined=1 with a not-yet-copied name (would freeze
        //   "Player2" into the change-gated peer-table write). Ordering makes the per-frame refresh redundant.
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
    char b[96];
    wsprintfA(b, "; U12 LEAVE from player %d -> mark left-lobby (host frees the slot)\n", sender);
    seam_log(b);
}

// Client (game/UI thread): the player clicked "Вступить в игру" on the selected record. Send an explicit
// JOIN carrying the lobby-id of the received host session (g_store_rec) over the already-open browse
// connection. This is the distinct join action -- a browse-connect alone must NOT admit us. Manual client
// only; a no-op (return 0 = success) otherwise so the retail join handler proceeds into the lobby.
void on_join_connect() {
    if (!MH_MP_IsManual()) return;            // force-entry: keep the pure no-op
    if (!g_a.is_host || *g_a.is_host) return; // client only
    if (!g_store_valid) {
        seam_log("; S4 join: no stored host record -> JOIN not sent\n");
        return;
    }
    mh_net_proto::JoinRequest jr = mh_net_proto::join_request_for(g_store_rec);
    lstrcpynA(jr.player_name, (LPCSTR)mh::addr::mp_player_name, sizeof(jr.player_name)); // S6: my player name
    uint8_t buf[mh_net_proto::JOIN_REQUEST_MAX_ENCODED];
    int     n = (int)mh_net_proto::join_request_encode(jr, buf);
    MH_Net_SendJoin(buf, n);
    char b[128];
    wsprintfA(b, "; S4 join: sent JOIN '%s' for '%s#%08X' (%d B) to host\n",
              jr.player_name, jr.name, jr.tag, n);
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
    char ip[64];
    if (!MH_Net_IsStarted() && !g_s3_conn_kicked && mp_read_typed_join_ip(ip, sizeof(ip))) {
        InterlockedExchange(&g_s3_conn_kicked, 1);
        InterlockedExchange(&g_s8_retry_armed, 0); // fresh attempt -- re-arm the retry-ready gate
        char b[96];
        wsprintfA(b, "; S3: typed host '%s' -> async connect (browser refresh)\n", ip);
        seam_log(b);
        CloseHandle(CreateThread(nullptr, 0, s3_connect_thread, nullptr, 0, nullptr));
    }
    build_browser_from_store(); // manual: real record or empty
}
void on_host_advertise() {
    *g_a.is_host                = 1;
    *(int *)ADDR_NET_LOCALIDX   = 0;
    *(int *)ADDR_NET_PLAYERSIDE = 0;
    MH_MP_ArmManualLobby();
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
    // Auto-list: once connected AND the host's SESSION_INFO has arrived, re-arm the browser's one-shot poll
    // so it re-lists from the store (no second manual refresh). The connect is kicked from on_discover_poll
    // (the "update list" refresh). Main thread => the re-arm write is race-free.
    if (MH_Net_IsStarted() && g_store_valid && !g_s3_listed) {
        InterlockedExchange(&g_s3_listed, 1);
        void *fn = *(void **)ADDR_MENU_REFRESH_FN; // the browser's registered rescan
        if (fn) *(void **)ADDR_MENU_ONESHOT = fn;  // re-arm the one-shot -> re-lists from the store
        seam_log("; S3: record received -> re-arming browser to list the real host\n");
    }
    // S8: the kicked connect FINISHED but the transport never started (dead/typo'd IP -> start_client's
    // connect() failed -> g_started stays false, MH_Net_InitEx is re-callable). Clear the one-shot latches so
    // the NEXT Connect -- after the player corrects the IP (re-selects a saved MRU entry / retypes) -- re-kicks
    // the connect with the new IP, instead of being ignored until a game restart.
    if (g_s3_conn_kicked && g_s3_conn_done && !MH_Net_IsStarted() && !g_store_valid) {
        MH_Seam_ResetTransportInit(); // net_seams: clear g_tried_init (lazy_start re-runs)
        InterlockedExchange(&g_s3_conn_done, 0);
        InterlockedExchange(&g_s3_conn_kicked, 0); // re-arm on_discover_poll to re-kick the corrected IP
        InterlockedExchange(&g_s8_retry_armed, 1); // S8(b): retry-ready -- the `retryready` UI-test gate holds now
        seam_log("; S8: connect failed (dead IP) -> cleared latches; a corrected IP retries on next Connect\n");
    }
}
