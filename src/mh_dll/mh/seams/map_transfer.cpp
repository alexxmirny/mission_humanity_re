//
// map_transfer.cpp -- mp:X2. The rules, the reasoning and the three constraints are in the header;
// this file is how they are carried out. Read map_transfer.h first.
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>
#include <string.h>

#include "seams/map_transfer.h"

#include "include/mh_net_export.h"   // MH_Net_PeerCount -- "is anyone even here"
#include "include/mh_net_module.h"   // MH_Net_Snapshot{Send,Poll,Status} + MH_SNAP_*
#include "mh_net_proto/net_crypto.h" // sha256
#include "addr/mh_addrs.gen.h"       // generated EN VAs (tools/gen_dll_addrs.py)
#include "addr/mh_calls.gen.h"       // mh::call::_fsopen / rsr_GetFileEntry (the loader's own edges)
#include "addr/mh_export.gen.h"      // MH_EXPORT_REPLACE(utils_open_file, ...)
#include "addr/mh_structs.gen.h"     // mh_llm_ui_widget / mh_llm_ui_widget_list (the Start button)
#include "en_guard.h"                // mh::en_build_ok -- "is a real mh.exe under us at all"
#include "net_internal.h"            // g_ini, seam_log
#include "config/ini_read.h"         // TL-HARN4: read_ini_string -- strips a trailing `;comment`

#pragma comment(lib, "user32.lib") // wsprintfA

namespace mh {
namespace seams {
namespace maps {

namespace {

namespace np = mh_net_proto;

constexpr int MAP_NAME_CAP = 64; // current_map_data.map_name is 32 bytes; 64 is slack
constexpr int HASH_N       = np::MAP_HASH_BYTES;

// THE CEILING ON A MAP, and it is not a round number picked for comfort. The stock set runs
// 115,943 B (`Cold War.mpm`) to 467,065 B (`Island Warfare.mpm`); the tracker's own figure for the
// item is "115 KB (.MP) / 296,960 B (.MAP)". 8 MiB is more than an order of magnitude above the
// largest thing anyone has shipped and still far under the snapshot pipeline's 32 MiB cap, so a
// file past it is a mis-selected file rather than a big map -- refused with a named line instead of
// read into memory and pushed across a link for a minute.
constexpr uint32_t MAP_MAX_BYTES = 8u * 1024u * 1024u;

// ---- logging ------------------------------------------------------------------------------------
// Every line here is `; [map] ` prefixed, which is the channel-tag form tools/lint_log_formats.py's
// arm B can actually discover -- a free-form line is registered but undiscoverable, so a new tag is
// worth the eight characters.
void mlog(const char *s) { seam_log(s); }

char g_line[400];

// ---- the loader's own resolution order ----------------------------------------------------------

bool name_is_mpm(const char *n) {
    const int len = lstrlenA(n);
    return len >= 4 && lstrcmpiA(n + len - 4, ".mpm") == 0;
}

// True iff the resource packs hold this name -- i.e. iff `GetResourseFilePtr` would answer and the
// loose file would never be consulted. `rsr_GetFileEntry` is the pure LOOKUP half of that call: it
// walks the loaded packs' directories and fills `*rsr_out`, and unlike GetResourseFilePtr it neither
// mallocs nor decompresses, which is what makes it safe to ask once a frame.
bool in_resource_pack(const char *name) {
    // NO GAME UNDER US, NO PACKS. `net_selftest.exe maptest` links this TU without loading mh.exe,
    // so every line in this file that reads a fixed VA or calls into the image is behind this
    // predicate -- `mh::en_build_ok()` is the DLL's standing "is the English build actually here"
    // probe (SEH-guarded, cached), and it is the same gate every other seam module arms behind.
    if (!mh::en_build_ok()) return false;
    void *pack = nullptr;
    char  nm[MAP_NAME_CAP];
    lstrcpynA(nm, name, sizeof(nm));
    return mh::call::rsr_GetFileEntry(nm, &pack) != nullptr;
}

// ---- host state ---------------------------------------------------------------------------------

struct PeerMap {
    bool    seated;      // an admitted JOIN has been seen from this peer id
    bool    holds;       // ...and its last report matched our claim
    bool    blocked;     // ...and it CANNOT hold it (its packs shadow the name)
    uint8_t had[HASH_N]; // what it last reported (all-zero = nothing)
    char    name[32];    // its player name, for the notice
};

PeerMap  g_peer[8];
char     g_host_map[MAP_NAME_CAP]; // the map name our claim is FOR (so a picker change re-hashes)
uint8_t  g_host_hash[HASH_N];
uint32_t g_host_size  = 0;
bool     g_host_claim = false; // g_host_hash is a real claim rather than the no-claim zero

int      g_tx_peer  = -1; // the peer a transfer is armed for, or -1
DWORD    g_tx_armed = 0;  // GetTickCount when it was armed (the re-arm timeout)
uint32_t g_tx_bytes = 0;

// ONE AT A TIME, and the header's reason restated because this constant is where it bites: channel C
// carries one transfer per link at a time (Endpoint::bulk_send_src refuses a second), and a map is
// 115-300 KB, so serialising three joiners costs seconds rather than the multiplexing machinery a
// simultaneous push would need. mp:T2a is the item that would change that, and it is not this one.
constexpr DWORD TX_REARM_MS = 90000; // a transfer this old is presumed lost; re-arm it

// ---- THE HOST LOCK (mp:T6) ----------------------------------------------------------------------
//
// THE RACE THIS CLOSES, named by its two threads. `host_on_join` runs on the TRANSPORT'S RECV THREAD
// (net_discovery.cpp on_join_recv, for every admitted JOIN). `host_pump_transfer` runs on the MAIN
// THREAD (the lobby tick). Both read and write `g_peer[]` and `g_tx_peer`. The first cut had no lock,
// and host_on_join published a peer in TWO steps: `seated = true` at the top, `holds = now` at the
// bottom, with a formatted line and a log-file write in between. A pump that ran inside that gap saw
// "seated, does not hold", read the 462 KB map, and armed the snapshot. The recv thread then logged
// "holds the map -- nothing to transfer" and set `holds`, too late. The rig showed it as
// `snapshot SEND armed` ~4 ms AFTER the "holds the map" line, in about 1 run in 3 (wave-4 shim runs
// r3/r6/r8 + c1_180_r2; 20 of the 67 D30 runs where the joiner held the map). The ordering was lost
// at the torn publish, not in the pump, so a re-check in the pump alone would have narrowed the
// window, not closed it.
//
// THE FIX, in two parts:
//   1. A peer's whole report (seated + holds + blocked + had + name) is published under this lock in
//      ONE step. A pump either sees nothing of a JOIN or all of it.
//   2. The pump RESERVES the peer (`g_tx_peer = want`) under the lock when it chooses, does the slow
//      file read outside it, and RE-CHECKS under the lock just before sending. A report that lands in
//      between (a completion re-JOIN, a leave) clears the reservation, and the pump withdraws.
//
// A LEAF LOCK. Nothing is called under it: no log line (seam_log has its own lock), no transport call
// (MH_Net_SnapshotSend / SnapshotStatus). The recv thread may hold transport locks when it calls in,
// so a transport call made while holding this lock could deadlock against it. Lines are formatted
// into locals under the lock and written after it is released.
//
// The one window left is between the final re-check and MH_Net_SnapshotSend. Only a peer whose own
// JOIN said it LACKS the map can be in it, so it is a real transfer to a peer that reports the map
// during a few microseconds. That is not the race above, which sent to a peer whose first and only
// report said it held the map.
SRWLOCK g_host_lock = SRWLOCK_INIT;

struct HostLock {
    HostLock() { AcquireSRWLockExclusive(&g_host_lock); }
    ~HostLock() { ReleaseSRWLockExclusive(&g_host_lock); }
    HostLock(const HostLock &)            = delete;
    HostLock &operator=(const HostLock &) = delete;
};

// maptest's doors onto the two interleavings (see set_pump_hooks_for_test in the header). Null in
// every game process.
const PumpTestHooks *g_pump_hooks = nullptr;

// ---- client state -------------------------------------------------------------------------------

enum ClientState {
    CS_IDLE = 0, // no advert yet, or the host makes no claim
    CS_HAVE,     // we hold the wanted content (at the base name or a stored one)
    CS_NEED,     // we do not, and the transfer is what we are waiting for
    CS_BLOCKED,  // we cannot ever hold it: our packs shadow the name
    CS_REFUSED,  // something arrived and did not hash right
};

int      g_cs = CS_IDLE;
char     g_want_map[MAP_NAME_CAP];
uint8_t  g_want_hash[HASH_N];
uint32_t g_want_size  = 0;
bool     g_want_valid = false;
uint8_t  g_my_hash[HASH_N]; // what WE hold for g_want_map (all-zero = nothing)
DWORD    g_need_since = 0;

uint8_t *g_rx_buf     = nullptr; // the client's delivery buffer (MAP_MAX_BYTES, on first need)
bool     g_rejoin_due = false;   // a stored download owes the host a re-JOIN (see client_tick)
// mp:X2d -- what the LAST JOIN told the host we hold (client_my_hash is only ever asked by a JOIN
// builder). client_resolve_now compares against it: a resolve that changes the answer after the host
// was told owes it a re-JOIN, download or not. Without it a JOIN that raced ahead of the advert
// ("nothing") and an advert that then resolved "have" left the host transferring forever.
uint8_t g_reported[HASH_N];
bool    g_reported_valid = false;

// ---- THE CLIENT LOCK (mp:X2d) --------------------------------------------------------------------
//
// The client state above had ONE writer thread until X2d: client_on_advert ran on the recv thread
// (the advert) and client_tick read on the main one. X2d adds a MAIN-thread client_on_advert (the
// JOIN names its lobby's claim before the advert lands), so two resolves can now overlap -- and with
// a stale directory row and a changed map they would tear g_want_* between two claims. Unlike THE
// HOST LOCK this one is NOT a leaf: a resolve logs and asks can_carry() (a transport call) under it.
// That is safe because of WHO WAITS: the main thread blocks for it, the recv thread only TRIES (it
// may hold transport locks, so it must never wait) and drops an advert it cannot take -- the host
// re-advertises ~1 Hz and the next one is a compare or a fresh resolve.
SRWLOCK g_client_lock = SRWLOCK_INIT;

struct ClientLock {
    bool held;
    explicit ClientLock(bool try_only)
        : held(try_only ? TryAcquireSRWLockExclusive(&g_client_lock) != FALSE
                        : (AcquireSRWLockExclusive(&g_client_lock), true)) {}
    ~ClientLock() {
        if (held) ReleaseSRWLockExclusive(&g_client_lock);
    }
    ClientLock(const ClientLock &)            = delete;
    ClientLock &operator=(const ClientLock &) = delete;
};

// ---- the redirect -------------------------------------------------------------------------------

char g_redir_base[MAP_NAME_CAP];
char g_redir_to[STORED_PATH_CAP];
bool g_redir_on = false;

// ---- configuration ------------------------------------------------------------------------------

int  g_enabled   = -1; // [net] map_transfer, resolved once
bool g_installed = false;

int enabled() {
    if (g_enabled < 0) g_enabled = (int)GetPrivateProfileIntA("net", "map_transfer", 1, g_ini);
    return g_enabled;
}

// ---- CAN THIS TRANSPORT CARRY A MAP AT ALL? -------------------------------------------------------
//
// THE DEADLOCK THIS PREVENTS, stated first because the guard is otherwise easy to read as belt and
// braces. The Start gate is "no seated joiner's download is incomplete". Channel C is
// mh_net_udp.dll's; the TCP module answers the three snapshot rows `MH_SNAP_UNSUPPORTED` (a named
// status, by X1b's ruling, never a crash). On TCP, therefore, a joiner that lacked the map could
// never finish a download that can never start -- and the gate would hold Start SHUT FOREVER, with
// a lobby notice naming a peer nobody could help. A feature that makes a working configuration
// unplayable is worse than the problem it solves.
//
// So on a transport without the channel THE TRANSFER is inert -- never armed, never waited for --
// but THE CLAIM IS NOT (mp:X2b, 2026-09-23). The first cut made no claim at all on TCP, which left
// a TCP pair free to desync on a same-named different map. Now the host still advertises its hash on
// every transport, every joiner still reports what it holds in its JOIN, and a joiner whose report
// disagrees is a peer the host REFUSES TO START WITH, by name -- a refusal in the lobby beats a desync
// 8000 steps into the match. What stays impossible on TCP is the rescue (the download); what is no
// longer possible is playing the wrong map without being told.
//
// `supported` and not `state`: X1b drew that distinction deliberately. `supported` is a property of
// the BUILD ("this module has channel C"), answered 1 even while the link is down; `state` is what
// is happening right now. The question here is the first one.
bool transport_can_carry() {
    MH_NetSnapshotStatus st;
    memset(&st, 0, sizeof(st));
    MH_Net_SnapshotStatus(&st);
    return st.supported != 0;
}

// maptest's door onto the question above (-1 = ask the module, 0/1 = pretend). The offline suite
// cannot swap transport modules mid-run, and "a TCP-shaped host refuses Start on a mismatch" is a
// claim about the gate's logic, not about the module -- so the answer is staged here.
int g_carry_test = -1;

bool can_carry() { return g_carry_test >= 0 ? g_carry_test != 0 : transport_can_carry(); }

// ---- small helpers ------------------------------------------------------------------------------

void join_path(char *out, size_t cap, const char *dir, const char *name) {
    out[0] = '\0';
    lstrcpynA(out, dir, (int)cap);
    lstrcpynA(out + lstrlenA(out), name, (int)cap - lstrlenA(out));
}

const char *hexof(const uint8_t h[HASH_N], char *out, size_t cap) {
    return np::map_hash_hex(h, out, cap);
}

// The basename of a path the game asked us to open -- the part after the last separator.
const char *basename_of(const char *p) {
    const char *b = p;
    for (const char *q = p; *q; ++q)
        if (*q == '\\' || *q == '/') b = q + 1;
    return b;
}

} // namespace

// =================================================================================================
// THE FILE LAYER
// =================================================================================================

const char *dir_for(const char *name) { return name_is_mpm(name) ? "Maps\\" : "Dane\\"; }

namespace {
char g_dl_dir[MAX_PATH] = {0}; // empty = the default; set_dl_dir is maptest's override
}

const char *dl_dir() { return g_dl_dir[0] ? g_dl_dir : DL_DIR_DEFAULT; }

void set_dl_dir(const char *dir_with_trailing_slash) {
    if (dir_with_trailing_slash == nullptr)
        g_dl_dir[0] = '\0';
    else
        lstrcpynA(g_dl_dir, dir_with_trailing_slash, sizeof(g_dl_dir));
}

uint8_t *read_file(const char *dir, const char *name, uint32_t cap_bytes, uint32_t *out_len) {
    if (out_len) *out_len = 0;
    char path[MAX_PATH];
    join_path(path, sizeof(path), dir, name);
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return nullptr;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || (uint64_t)sz.QuadPart > (uint64_t)cap_bytes) {
        CloseHandle(h);
        return nullptr;
    }
    const uint32_t n   = (uint32_t)sz.QuadPart;
    uint8_t       *buf = (uint8_t *)VirtualAlloc(nullptr, n, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (buf == nullptr) {
        CloseHandle(h);
        return nullptr;
    }
    DWORD got = 0;
    // ONE ReadFile, then a length check -- not a loop that accumulates. A short read on a local file
    // means something took the file away mid-read, and half a map is not a map.
    const BOOL ok = ReadFile(h, buf, n, &got, nullptr);
    CloseHandle(h);
    if (!ok || got != n) {
        VirtualFree(buf, 0, MEM_RELEASE);
        return nullptr;
    }
    if (out_len) *out_len = n;
    return buf;
}

void free_bytes(uint8_t *p) {
    if (p != nullptr) VirtualFree(p, 0, MEM_RELEASE);
}

bool file_hash(const char *dir, const char *name, uint8_t out[HASH_N], uint32_t *out_size) {
    uint32_t n   = 0;
    uint8_t *buf = read_file(dir, name, MAP_MAX_BYTES, &n);
    if (buf == nullptr) return false;
    uint8_t full[np::SHA256_LEN];
    np::sha256(buf, n, full);
    np::map_hash_from_sha256(full, out);
    free_bytes(buf);
    if (out_size) *out_size = n;
    return true;
}

Resolve resolve(const char *dir, const char *base, const uint8_t want[HASH_N], char *out_path,
                size_t cap, bool skip_base) {
    uint8_t h[HASH_N];
    // THE BASE NAME FIRST, and by HASH. A local file called `Cold War.mpm` is a candidate exactly
    // when its bytes are the wanted ones -- never because it has the right name, which is the whole
    // confusion this item removes. `skip_base` is the harness's door (see the pretend knob).
    if (!skip_base && file_hash(dir, base, h, nullptr) && np::map_hash_equal(h, want)) {
        join_path(out_path, cap, dir, base);
        return Resolve::Base;
    }
    // Then the one stored path this content could have, in the download directory. There is no
    // directory scan: the stored name is a FUNCTION of (base, hash), so the one candidate is
    // computable and enumerating would only find files whose names we would then have to re-derive
    // anyway.
    char stored[np::MAP_STORED_NAME_CAP];
    if (np::map_stored_name(base, want, stored, sizeof(stored)) == 0) return Resolve::Missing;
    char path[STORED_PATH_CAP];
    join_path(path, sizeof(path), dl_dir(), stored);
    if (file_hash("", path, h, nullptr) && np::map_hash_equal(h, want)) {
        lstrcpynA(out_path, path, (int)cap);
        return Resolve::Stored;
    }
    return Resolve::Missing;
}

bool store(const char *base, const uint8_t hash[HASH_N], const void *bytes, uint32_t len,
           char *out_path, size_t cap) {
    if (bytes == nullptr || len == 0 || len > MAP_MAX_BYTES) return false;
    // THE LAST CHECK BEFORE IT BECOMES A FILE. The transfer already verified the blob against the
    // manifest its sender committed to, and that proves the bytes CROSSED intact; it does not prove
    // they are the bytes the ADVERT named. Two different claims, so two checks -- and this is the
    // one that stops a file being written under a name that lies about its content.
    uint8_t full[np::SHA256_LEN], got[HASH_N];
    np::sha256((const uint8_t *)bytes, len, full);
    np::map_hash_from_sha256(full, got);
    if (!np::map_hash_equal(got, hash)) return false;

    char stored[np::MAP_STORED_NAME_CAP];
    if (np::map_stored_name(base, hash, stored, sizeof(stored)) == 0) return false;
    // ...AND IT MUST NOT BE THE BASE NAME. map_stored_name cannot produce one (it always inserts a
    // dot and sixteen hex characters), but rule 1 of the header is the kind of rule that must be
    // enforced where the write happens rather than inferred from a helper's shape.
    if (lstrcmpiA(stored, base) == 0) return false;

    // The download directory is made on demand, by the peer that first downloads something. A
    // player who never joins a game missing a map never grows one -- and it is NOT under `Maps\`,
    // where the picker's folder scan would turn it into the list's pre-selected first row.
    CreateDirectoryA(dl_dir(), nullptr); // already-exists is the expected answer after the first

    char path[STORED_PATH_CAP];
    join_path(path, sizeof(path), dl_dir(), stored);
    // CREATE_ALWAYS is correct HERE and only here: the target is content-addressed, so an existing
    // file of this name is either the identical content or a truncated earlier attempt, and either
    // way the right answer is these bytes.
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD      wrote = 0;
    const BOOL ok    = WriteFile(h, bytes, len, &wrote, nullptr);
    CloseHandle(h);
    if (!ok || wrote != len) {
        DeleteFileA(path); // leave no half-map behind for resolve() to hash next time
        return false;
    }
    lstrcpynA(out_path, path, (int)cap);
    return true;
}

// =================================================================================================
// THE OPEN REDIRECT
// =================================================================================================

void redirect_set(const char *base, const char *stored) {
    lstrcpynA(g_redir_base, base, sizeof(g_redir_base));
    lstrcpynA(g_redir_to, stored, sizeof(g_redir_to));
    g_redir_on = true;
}

void redirect_clear() { g_redir_on = false; }

// THE WHOLE PATH IS REPLACED, not the basename spliced into the caller's directory. The stored file
// is not "the map folder's file under another name" -- it is somewhere else entirely, in `mh_dl\`,
// because the picker's folder scan makes `Maps\` unusable for it. Matching is still on the BASENAME:
// the loader asks for `Maps\Cold War.mpm` in one place and a bare `Cold War.mpm` in another, and
// both mean the same map.
const char *redirect_apply(const char *in, char *scratch, size_t cap) {
    if (!g_redir_on || in == nullptr || scratch == nullptr) return in;
    if (lstrcmpiA(basename_of(in), g_redir_base) != 0) return in;
    if ((size_t)lstrlenA(g_redir_to) + 1 > cap) return in; // no room: open what was asked for
    lstrcpynA(scratch, g_redir_to, (int)cap);
    return scratch;
}

// =================================================================================================
// THE REPLACED `utils_open_file`
//
// A WHOLE-BODY REPLACEMENT AND NOT A RUN-BEFORE TRAMPOLINE, and the reason is mechanical rather
// than stylistic: the function is ten bytes -- `push ebx; xor ebx,ebx; call _fsopen` -- so the five
// bytes a trampoline steals for its jump would land inside that RELATIVE call, and a stolen `E8`
// re-executed from a different address calls a different function. Replacing it outright is exact
// instead: the body is one line, `_fsopen` has its own VA in the generated call table, and the
// entry-byte guard in `install_export_ok` refuses to arm against a build whose ten bytes differ.
//
// THE COST OF SITTING ON A GLOBAL EDGE, stated plainly: this is the game's single file-open wrapper,
// forty call sites, everything from `Msgs.dat` to a screenshot. What runs on every one of them is
// `g_redir_on` -- one bool, false in every process that never joined a lobby needing a map -- and on
// the one path where it is true, one case-insensitive compare of a basename. The redirect writes
// into a scratch buffer and never touches the caller's string, so a caller passing a pointer into
// read-only .rdata (several do) is untouched by construction rather than by luck.
// =================================================================================================

namespace {

int32_t __cdecl open_file_replacement(char *filename, char *mode) {
    char        scratch[MAX_PATH];
    const char *use = redirect_apply(filename, scratch, sizeof(scratch));
    return (int32_t)(uintptr_t)mh::call::_fsopen((char *)use, mode, 0);
}

} // namespace

MH_EXPORT_REPLACE(utils_open_file, open_file_replacement)

// =================================================================================================
// THE LOBBY NOTICE + THE START GATE
// =================================================================================================

namespace {

constexpr uintptr_t ADDR_MAP_STATUS_LINE = mh::addr::_G_LLM_LOBBY_MAP_STATUS_LINE; // wchar_t[256]
constexpr uintptr_t ADDR_MENU_LIST       = mh::addr::_G_LLM_UI_MENU_WIDGET_LIST;
constexpr uintptr_t ADDR_LOBBY_LIST      = mh::addr::lobby_widget_origin;
constexpr uintptr_t ADDR_BEGIN_MAP_LOAD  = mh::addr::llm_lobby_begin_map_load;

bool on_the_lobby_screen() {
    if (!mh::en_build_ok()) return false; // the offline suite has no widget list to read
    return *(void **)ADDR_MENU_LIST == (void *)ADDR_LOBBY_LIST;
}

// REPAINTED EVERY FRAME, for U23's reason and not a different one: the buffer is not ours. It is
// NULLed by the browser's enter path and re-cleared by every rescan that auto-selects a row, so any
// single write races those. One lstrcpynW on a menu screen is the price; determinism-neutral (menu
// text, no sim state, no RNG).
void paint(const wchar_t *s) {
    if (!on_the_lobby_screen()) return;
    lstrcpynW((wchar_t *)ADDR_MAP_STATUS_LINE, s, 256);
}

wchar_t g_notice[256];
bool    g_notice_on = false;

void paint_notice() {
    if (g_notice_on) paint(g_notice);
}

// THE START BUTTON, FOUND BY WHAT IT DOES rather than by its address. `llm_lobby_screen_open` sets
// `action_cb = llm_lobby_begin_map_load` on widget 0x006502a3; the widget's VA is not in the
// generated address header and adding one would mean a Ghidra write this item does not otherwise
// need, so the button is identified as "the lobby's child whose action callback is begin_map_load",
// which is also the more honest description of what makes it the Start button.
// THE START BUTTON IS A FIXED WIDGET, AND IT TOOK TWO RIG RUNS TO STOP GUESSING AT IT. The first
// version walked `lobby_widget_origin`'s children for the one whose `action_cb` is
// `llm_lobby_begin_map_load`, on the reasoning that identifying a button by what it DOES beats
// hard-coding an address. The reasoning is fine and the walk was in the wrong place: the run's own
// diagnostic dumped the twelve children of that list and Start is not among them (their callbacks
// are the log list's mouse handler, the chat send, and the map picker's proceed). The button the
// harness clicks is `006502A3` -- the address is in the frame every passing lobby script writes:
//
//     ; click click_label -> widget 006502A3 @ (501,294) label='S'
//
// and `llm_lobby_screen_open` (EN 0x004be8a7) is where it is wired, in as many words:
//
//     llm_ui_widget_006502a3.label     = G_TEXT_PTRS[0x284];
//     llm_ui_widget_006502a3.action_cb = llm_lobby_begin_map_load;   // host branch
//     llm_ui_widget_006502a3.flags    |= 0x40;                       // ...and starts DISABLED
//
// So the widget is addressed directly, and the CALLBACK becomes the sanity check rather than the
// search key: if `006502A3` is not wired to the lobby's own entry point we are looking at something
// else and the gate says so instead of setting a bit in a stranger. A literal address with the
// evidence beside it, in the ui_drive.cpp manner -- adding it to the generated manifest would mean
// a Ghidra write this item does not otherwise need.
constexpr uintptr_t ADDR_START_WIDGET = 0x006502a3u; // llm_ui_widget_006502a3, the lobby's Start

mh::game::mh_llm_ui_widget *find_start_widget() {
    if (!mh::en_build_ok()) return nullptr;
    auto *w = (mh::game::mh_llm_ui_widget *)ADDR_START_WIDGET;
    // THE WIRING CHECK, and it is the HOST branch specifically -- which is the only branch that
    // matters, because gate_close/gate_open are reached from host_tick alone. On a client
    // llm_lobby_screen_open hides this widget (flags |= 0xc0) and wires the entry to a different
    // one; a callback that is not begin_map_load therefore means the host lobby has not opened yet
    // (nothing to grey) or that this address is not what it was.
    const bool wired = w->action_cb == (void *)ADDR_BEGIN_MAP_LOAD;
    // Told once per OUTCOME, not once per call: the first call can land before the lobby has
    // wired anything, and burning the one-shot on that would hide the sighting that matters.
    static bool told_wired = false, told_not = false;
    bool       *told = wired ? &told_wired : &told_not;
    if (!*told) {
        *told = true;
        wsprintfA(g_line, "; [map] gate Start widget %08X cb=%08X flags=%08X (%s)\n",
                  (unsigned)ADDR_START_WIDGET, (unsigned)(uintptr_t)w->action_cb,
                  (unsigned)w->flags,
                  wired ? "wired by the lobby -- the gate can grey it"
                        : "NOT wired to the lobby's entry point; the refusal will be log-only");
        mlog(g_line);
    }
    return wired ? w : nullptr;
}

constexpr uint32_t WIDGET_DISABLED = 0x40u; // mh_structs.gen.h: hit-test and hotkey both skip it

// WE ARE NOT THE ONLY WRITER OF THIS BIT, which an earlier draft of this file asserted and the
// decompile refutes: `llm_lobby_screen_open` does `flags |= 0x40` on the host branch, so Start is
// DISABLED the moment the lobby opens and the retail dispatch clears it once the slots settle --
// the rig's own `peers 1` step waits 8.5 s for exactly that. So the gate may only ever clear a bit
// it SET.
//
// CLOSED per frame, OPENED on the edge. Closing re-asserts because the widget is not ours and the
// lobby rebuilds it; opening happens once, and afterwards the bit belongs to retail again. The one
// residue is a single frame wide: if retail wanted Start disabled at the moment the last joiner
// reported the map, our release enables it until retail's next dispatch re-disables it. That is
// the cheapest of the available wrong answers -- the alternative, sampling retail's intent when
// the gate closes and restoring it when it opens, restores a value that is stale by construction,
// because at close time retail has the button disabled almost always and the gate would then never
// reopen at all.
bool g_gate_shut = false; // WE set the DISABLED bit and owe exactly one clear

void gate_close() {
    mh::game::mh_llm_ui_widget *w = find_start_widget();
    if (w == nullptr) return; // the lobby's widgets are not built yet
    w->flags |= WIDGET_DISABLED;
    g_gate_shut = true;
}

void gate_open() {
    if (!g_gate_shut) return; // never ours to clear
    mh::game::mh_llm_ui_widget *w = find_start_widget();
    if (w == nullptr) return;
    w->flags &= ~WIDGET_DISABLED;
    g_gate_shut = false;
}

} // namespace

// =================================================================================================
// THE HOST
// =================================================================================================

namespace {

constexpr uintptr_t ADDR_CUR_MAP  = mh::addr::current_map_data;
constexpr int       MD_MAPNAME    = 0xfc;
constexpr uintptr_t ADDR_MAP_NAME = ADDR_CUR_MAP + MD_MAPNAME;

// Recompute our own claim when the lobby's picked map changes. The picker writes
// `current_map_data.map_name`, so that string IS the question; hashing on every frame would re-read
// a 300 KB file sixty times a second, so the name is the cache key.
void host_refresh_claim() {
    if (!mh::en_build_ok()) return; // the claim comes out of current_map_data; offline there is none
    // mp:X2b: NO EARLY RETURN ON A TRANSPORT WITHOUT CHANNEL C any more. The claim is made on every
    // transport; only the transfer is gated on the channel (host_pump_transfer and
    // host_next_peer_needing_map), and host_start_blocked turns a mismatch it cannot rescue into a
    // named refusal. Told once, so a TCP log says which of the two behaviours it is running.
    if (!can_carry()) {
        static bool said = false;
        if (!said) {
            said = true;
            mlog("; [map] host nocarry -- this transport has no bulk channel: the map claim is still "
                 "advertised, but a joiner holding different content (or none) cannot be sent it, so "
                 "Start is REFUSED naming that joiner rather than risking a desync (mp:X2b)\n");
        }
    }
    const char *live = (const char *)ADDR_MAP_NAME;
    if (live[0] == '\0') return;
    if (lstrcmpiA(live, g_host_map) == 0) return; // unchanged
    {
        HostLock l; // mp:T6: the recv thread reads these three in host_on_join
        lstrcpynA(g_host_map, live, sizeof(g_host_map));
        g_host_claim = false;
        g_host_size  = 0;
        memset(g_host_hash, 0, HASH_N);
    }
    char hex[np::MAP_HASH_HEX_CAP];

    if (in_resource_pack(g_host_map)) {
        // NO CLAIM, and the header explains why this is the right answer rather than a gap: a map
        // the resource packs answer for is the same bytes on every install of this build, and a
        // claim about a loose file we would not load would be a claim about the wrong bytes.
        wsprintfA(g_line, "; [map] host noclaim %s (the resource packs answer for this name -- a "
                          "stock map is install-identical)\n",
                  g_host_map);
        mlog(g_line);
        return;
    }
    if (!file_hash(dir_for(g_host_map), g_host_map, g_host_hash, &g_host_size)) {
        memset(g_host_hash, 0, HASH_N);
        wsprintfA(g_line, "; [map] host noclaim %s (no readable file at %s%s -- joiners fall back "
                          "to their own copy, as before X2)\n",
                  g_host_map, dir_for(g_host_map), g_host_map);
        mlog(g_line);
        return;
    }
    {
        HostLock l; // host_on_join reads the claim on the recv thread (mp:T6)
        g_host_claim = true;
        // A new map invalidates every peer's answer: they reported against the OLD content.
        for (int i = 0; i < 8; ++i) {
            if (!g_peer[i].seated) continue;
            g_peer[i].holds   = false;
            g_peer[i].blocked = false;
        }
        g_tx_peer = -1;
    }
    wsprintfA(g_line, "; [map] host claim %s sha=%s size=%lu\n", g_host_map,
              hexof(g_host_hash, hex, sizeof(hex)), (unsigned long)g_host_size);
    mlog(g_line);
}

// The transfer target, or -1. Caller holds g_host_lock and has already asked can_carry() (a
// transport call, so never made under the lock).
int next_peer_locked() {
    if (!g_host_claim) return -1;
    for (int i = 0; i < 8; ++i)
        if (g_peer[i].seated && !g_peer[i].holds && !g_peer[i].blocked) return i;
    return -1;
}

int send_snapshot(int peer, const void *body, int len) {
    if (g_pump_hooks != nullptr && g_pump_hooks->send != nullptr)
        return g_pump_hooks->send(peer, body, len, g_pump_hooks->ctx);
    return MH_Net_SnapshotSend(peer, body, len);
}

// Arm the transfer for the first seated peer that does not hold the map. One at a time; see
// TX_REARM_MS for why a stuck one is re-armed rather than waited on forever.
//
// mp:T6: CHOOSE and RESERVE under the lock, read the file outside it, RE-CHECK under the lock, then
// send. The lock's block comment (THE HOST LOCK) names the race; this function is its second half.
void host_pump_transfer() {
    if (!g_host_claim) return;
    if (!can_carry()) return; // mp:X2b: never arm a transfer the link cannot carry
    int timed_out = -1, want = -1;
    {
        HostLock l;
        if (g_tx_peer >= 0) {
            if (GetTickCount() - g_tx_armed < TX_REARM_MS) return; // still believed to be running
            timed_out = g_tx_peer;
            g_tx_peer = -1;
        }
        want = next_peer_locked();
        if (want >= 0) {
            g_tx_peer  = want; // the reservation: a report from this peer now clears it
            g_tx_armed = GetTickCount();
        }
    }
    if (timed_out >= 0) {
        wsprintfA(g_line, "; [map] send timed out for peer %d after %lu ms -- re-arming\n", timed_out,
                  (unsigned long)TX_REARM_MS);
        mlog(g_line);
    }
    if (want < 0) return;

    uint32_t       n    = 0;
    uint8_t       *body = nullptr;
    const uint8_t *src  = nullptr;
    if (g_pump_hooks != nullptr && g_pump_hooks->body != nullptr) {
        src = g_pump_hooks->body; // maptest: no Maps\ directory under the suite
        n   = g_pump_hooks->len;
    } else {
        body = read_file(dir_for(g_host_map), g_host_map, MAP_MAX_BYTES, &n);
        src  = body;
    }
    if (src == nullptr) {
        {
            HostLock l;
            if (g_tx_peer == want) g_tx_peer = -1;
            g_host_claim = false; // stop promising something we cannot deliver
        }
        wsprintfA(g_line, "; [map] send REFUSED -- %s%s could not be read\n", dir_for(g_host_map),
                  g_host_map);
        mlog(g_line);
        return;
    }
    if (g_pump_hooks != nullptr && g_pump_hooks->between != nullptr)
        g_pump_hooks->between(want, g_pump_hooks->ctx); // maptest: a JOIN lands HERE

    // THE RE-CHECK. The reservation survives only if nothing this peer reported since the choice
    // says it holds the map (host_on_join clears it) or that it left (host_on_leave clears it).
    bool still = false;
    char name[32];
    {
        HostLock l;
        still = g_tx_peer == want && g_peer[want].seated && !g_peer[want].holds &&
                !g_peer[want].blocked;
        if (!still && g_tx_peer == want) g_tx_peer = -1;
        lstrcpynA(name, g_peer[want].name, sizeof(name));
    }
    if (!still) {
        free_bytes(body);
        wsprintfA(g_line, "; [map] send WITHDRAWN for peer %d '%s' -- it reported the map (or left) "
                          "while the transfer was being prepared (mp:T6)\n",
                  want, name);
        mlog(g_line);
        return;
    }
    // MH_Net_SnapshotSend COPIES (mh_net_module.h's ownership rule), so the buffer is ours to
    // release the instant it returns rather than for the length of the transfer.
    const int armed = send_snapshot(want, src, (int)n);
    free_bytes(body);
    if (!armed) { // not admitted yet, or a transfer is already running -- the next tick retries
        HostLock l;
        if (g_tx_peer == want) g_tx_peer = -1;
        return;
    }
    g_tx_bytes = n;
    wsprintfA(g_line, "; [map] send armed to peer %d '%s' (%lu B of %s)\n", want, name,
              (unsigned long)n, g_host_map);
    mlog(g_line);
}

void host_tick() {
    host_refresh_claim();
    host_pump_transfer();

    char        peer[32];
    bool        unfetchable = false;
    static bool was_blocked = false, was_unfetchable = false;
    if (host_start_blocked(peer, sizeof(peer), &unfetchable)) {
        wchar_t wname[32], wmap[MAP_NAME_CAP];
        MultiByteToWideChar(CP_ACP, 0, peer, -1, wname, 32);
        MultiByteToWideChar(CP_ACP, 0, g_host_map, -1, wmap, MAP_NAME_CAP);
        // mp:X2b: the two refusals say different things to the player, because only one of them
        // ends by itself. A download finishes; a mismatch on a link that cannot carry the map does
        // not, and the notice must say what would fix it (the same file on both machines).
        if (unfetchable)
            wsprintfW(g_notice, L"%s has a different copy of %s -- this connection cannot send maps, "
                                L"so Start is refused.",
                      wname, wmap);
        else
            wsprintfW(g_notice, L"Sending the map to %s -- Start is held until it arrives.", wname);
        g_notice_on = true;
        gate_close();
        // LOGGED ON THE EDGE, not every lobby frame: this runs at frame rate, and a refusal that
        // wrote sixty lines a second would be the same fact told until it was unreadable. The edge
        // is also what an oracle wants -- "the gate closed, for this peer", once per closing.
        if (!was_blocked || was_unfetchable != unfetchable) {
            if (unfetchable)
                wsprintfA(g_line, "; [map] start REFUSED -- '%s' holds a different %s and this "
                                  "transport cannot carry maps (mp:X2b: a refusal, not a desync)\n",
                          peer, g_host_map);
            else
                wsprintfA(g_line, "; [map] start REFUSED -- waiting for '%s' to finish downloading the "
                                  "map\n",
                          peer);
            mlog(g_line);
            was_blocked     = true;
            was_unfetchable = unfetchable;
        }
    } else {
        if (was_blocked) {
            mlog("; [map] start OK -- every joiner reports the map we advertised\n");
            was_blocked = false;
        }
        if (g_notice_on) {
            g_notice_on = false;
            if (mh::en_build_ok())
                *(wchar_t *)ADDR_MAP_STATUS_LINE = L'\0'; // hand it back to retail map status
        }
        gate_open();
    }
    paint_notice();
}

} // namespace

void host_fill_advert(np::SessionInfo &si) {
    if (!enabled()) return;
    // MAIN THREAD ONLY, and the guard is not decoration. `mp_build_host_session_info` -- this
    // function's caller -- runs on the ~1 Hz advertise path AND on the RECV thread inside
    // on_join_recv, and refreshing the claim means hashing a 300 KB file into the same three
    // globals the lobby tick writes. The recv path does not need it (it uses the record for its
    // lobby-id only), so the refresh stays where its writer is single: the lobby tick. A claim that
    // is one advert stale is harmless -- the client acts on the first advert carrying a hash, and
    // the next one is a second later.
    if (GetCurrentThreadId() == g_main_tid) host_refresh_claim();
    if (!g_host_claim) return; // leave the all-zero no-claim in place
    memcpy(si.map_hash, g_host_hash, HASH_N);
    si.map_size = g_host_size;
}

// RECV THREAD. mp:T6: the whole report is built in a local and published under g_host_lock in one
// step -- see THE HOST LOCK for the torn publish this replaced. The log line is written after the
// lock is released.
void host_on_join(int sender, const char *player_name, const uint8_t map_hash[HASH_N]) {
    if (!enabled()) return;
    if (sender < 0 || sender > 7) return;
    const bool carry = can_carry(); // a transport call: asked before the lock, never under it
    if (g_pump_hooks != nullptr && g_pump_hooks->join_prepublish != nullptr)
        g_pump_hooks->join_prepublish(sender, g_pump_hooks->ctx); // maptest: a pump runs HERE

    char line[400];
    line[0] = '\0';
    {
        HostLock l;
        PeerMap  p = g_peer[sender & 7];
        p.seated   = true;
        if (player_name != nullptr && player_name[0] != '\0') lstrcpynA(p.name, player_name, sizeof(p.name));
        else if (p.name[0] == '\0') wsprintfA(p.name, "Player%d", sender + 1);
        memcpy(p.had, map_hash, HASH_N);

        char hex[np::MAP_HASH_HEX_CAP], hex2[np::MAP_HASH_HEX_CAP];
        if (!g_host_claim) {
            p.holds = true; // nothing to hold: we make no claim, so nobody can fail it
        } else {
            const bool now = np::map_hash_equal(p.had, g_host_hash);
            if (now && !p.holds) {
                wsprintfA(line, "; [map] peer %d '%s' holds the map (sha=%s) -- nothing to transfer\n",
                          sender, p.name, hexof(g_host_hash, hex, sizeof(hex)));
                // Its transfer is done (or, mid-pump, withdrawn); let the next peer have the link.
                if (g_tx_peer == sender) g_tx_peer = -1;
            } else if (!now && !p.blocked) {
                wsprintfA(line, "; [map] peer %d '%s' needs the map (has=%s want=%s)%s\n", sender,
                          p.name, np::map_hash_is_none(p.had) ? "none" : hexof(p.had, hex, sizeof(hex)),
                          hexof(g_host_hash, hex2, sizeof(hex2)),
                          carry ? "" : " -- this transport cannot send it; Start will be refused");
            }
            p.holds = now;
        }
        g_peer[sender & 7] = p; // THE publish: seated and holds become visible together
    }
    if (line[0] != '\0') mlog(line);
}

void host_on_leave(int sender) {
    if (sender < 0 || sender > 7) return;
    HostLock l;
    g_peer[sender & 7] = PeerMap{};
    if (g_tx_peer == sender) g_tx_peer = -1;
}

int host_next_peer_needing_map() {
    if (!enabled() || !g_host_claim) return -1;
    if (!can_carry()) return -1; // mp:X2b: nobody is a transfer target on a link without channel C
    HostLock l;
    return next_peer_locked();
}

void host_set_claim_for_test(const char *map_name, const uint8_t hash[HASH_N], uint32_t size) {
    HostLock l;
    lstrcpynA(g_host_map, map_name, sizeof(g_host_map));
    memcpy(g_host_hash, hash, HASH_N);
    g_host_size  = size;
    g_host_claim = !np::map_hash_is_none(g_host_hash);
}

void set_can_carry_for_test(int v) { g_carry_test = v; }

void set_pump_hooks_for_test(const PumpTestHooks *h) { g_pump_hooks = h; }

void host_pump_for_test() { host_pump_transfer(); }

bool host_start_blocked(char *out_peer, int cap, bool *out_unfetchable) {
    if (out_unfetchable != nullptr) *out_unfetchable = false;
    if (!enabled() || !g_host_claim) return false;
    const bool carry = can_carry();
    HostLock   l;
    for (int i = 0; i < 8; ++i) {
        if (g_peer[i].seated && !g_peer[i].holds) {
            if (out_peer != nullptr && cap > 0) lstrcpynA(out_peer, g_peer[i].name, cap);
            // mp:X2b: without channel C this is not a wait, it is a refusal -- nothing will arrive.
            if (out_unfetchable != nullptr) *out_unfetchable = !carry;
            return true;
        }
    }
    return false;
}

bool host_refuse_start_click() {
    char peer[32];
    bool unfetchable = false;
    if (!host_start_blocked(peer, sizeof(peer), &unfetchable)) return false;
    wsprintfA(g_line,
              "; [map] start CLICK REFUSED -- '%s' %s; begin_map_load NOT run (mp:X2/X2b: the "
              "activation is refused, not only the widget greyed)\n",
              peer,
              unfetchable ? "holds a different map and this transport cannot carry it"
                          : "is still downloading the map");
    mlog(g_line);
    return true;
}

// =================================================================================================
// THE CLIENT
// =================================================================================================

namespace {

// Sample the file the player already had under the advertised name, and say so in the log. Two
// samples of this line -- one when the advert lands, one when the match starts -- are the assertion
// that a download never touched it.
void client_sample_local(const char *when) {
    uint8_t  h[HASH_N];
    uint32_t n = 0;
    char     hex[np::MAP_HASH_HEX_CAP];
    if (file_hash(dir_for(g_want_map), g_want_map, h, &n))
        wsprintfA(g_line, "; [map] local %s %s sha=%s size=%lu\n", when, g_want_map,
                  hexof(h, hex, sizeof(hex)), (unsigned long)n);
    else
        wsprintfA(g_line, "; [map] local %s %s absent\n", when, g_want_map);
    mlog(g_line);
}

// ---- THE UI-HARNESS KNOB, and why it stages a DECISION rather than a FILE ------------------------
//
// X2's acceptance clauses all begin "a joiner whose local map set ..." -- i.e. each one needs the
// CLIENT peer to enter the run holding something particular (nothing, or a different file of the
// same name) while the HOST holds the real map. The rig harness has no per-peer file staging (it
// copies the same mh.dll, ini and key to every peer), so the only place that asymmetry can be
// created is inside the DLL, where the role is known. The knob is therefore SYMMETRIC in the ini
// and asymmetric in effect: both peers read it, only a client acts on it.
//
// THE FIRST VERSION OF THIS MOVED FILES, AND THAT WAS WRONG -- measured, not suspected. It renamed
// `Maps\<name>` aside to `<name>.uitest_held` on the client. But `tools/make_lane.py` builds a local
// lane with `LINKED_DIRS = ["Res", "Maps"]`: every lane's `Maps` is a SYMLINK to the one shared game
// image. So the client's "make it absent for me" took the map away from the HOST too, the host
// logged `send REFUSED -- Maps\<name> could not be read`, both scripts timed out, and the shared
// image was left holding a `.uitest_held` file until it was restored by hand. A staging mechanism
// that can damage the thing every other scenario reads is not a staging mechanism.
//
// So the knob stages the DECISION instead, and touches nothing:
//   * `map_test_pretend=none`  -- this client reports holding NOTHING for the advertised map, and
//     its resolver ignores the base-name candidate for the session. The host sees `has=none`, arms
//     the transfer, and the download, the content-addressed write and the redirect all really
//     happen -- only the premise is staged.
//   * `map_test_pretend=other` -- the same, but the JOIN reports a fixed hash that is deliberately
//     not the host's, so the host's log reads `has=<hex>` instead. This is the "same name, different
//     content" start state at the one place it is decided.
// The player's own file is then untouched BY CONSTRUCTION rather than by care, which is also what
// makes the `local before` / `local after` samples in the log honest: nothing in the run is capable
// of changing them, and they are read from the real file both times.
//
// What this knob CANNOT prove on a local lane, stated plainly rather than left to be assumed: the
// base file's bytes here ARE the host's content, so a broken redirect would still open a matching
// map and the scenario would still pass. The redirect's own proof is `maptest` arm E and the
// two-endpoint arm W, where the local file is genuinely different bytes. The rig proves the
// integration -- claim, gate, transfer, content-addressed write, and a match that plays IDENTICAL.
enum { PRETEND_OFF   = 0,
       PRETEND_NONE  = 1,
       PRETEND_OTHER = 2 };
int g_pretend = -1; // -1 = not yet read from the ini

// A hash no real map will have: it is the truncation of SHA-256("mh:X2 uitest pretend other"),
// computed once here rather than written as a magic constant so its provenance is in the code.
const uint8_t *pretend_other_hash() {
    static uint8_t h[HASH_N];
    static bool    made = false;
    if (!made) {
        static const char seed[] = "mh:X2 uitest pretend other";
        uint8_t           full[np::SHA256_LEN];
        np::sha256((const uint8_t *)seed, sizeof(seed) - 1, full);
        np::map_hash_from_sha256(full, h);
        made = true;
    }
    return h;
}

int pretend_mode() {
    if (g_pretend < 0) {
        char v[32];
        mh::config::read_ini_string("net", "map_test_pretend", "", v, sizeof(v), g_ini); // TL-HARN4
        if (lstrcmpiA(v, "none") == 0)
            g_pretend = PRETEND_NONE;
        else if (lstrcmpiA(v, "other") == 0)
            g_pretend = PRETEND_OTHER;
        else
            g_pretend = PRETEND_OFF;
        if (g_pretend != PRETEND_OFF) {
            wsprintfA(g_line,
                      "; [map] uitest pretend=%s -- this client behaves as though it holds %s for "
                      "the advertised map; no file is moved, copied or deleted\n",
                      g_pretend == PRETEND_NONE ? "none" : "other",
                      g_pretend == PRETEND_NONE ? "nothing"
                                                : "a different file of the same name");
            mlog(g_line);
        }
    }
    return g_pretend;
}

// Re-decide where the wanted content lives and set the redirect accordingly. Called when the advert
// lands and again after a download is stored.
void client_resolve_now() {
    char          name[STORED_PATH_CAP];
    const int     pm = pretend_mode();
    const Resolve r =
        resolve(dir_for(g_want_map), g_want_map, g_want_hash, name, sizeof(name), pm != PRETEND_OFF);
    if (r == Resolve::Base) {
        redirect_clear();
        g_cs = CS_HAVE;
        memcpy(g_my_hash, g_want_hash, HASH_N);
        mlog("; [map] client resolve base -- the local file already IS the host's content\n");
    } else if (r == Resolve::Stored) {
        redirect_set(g_want_map, name);
        g_cs = CS_HAVE;
        memcpy(g_my_hash, g_want_hash, HASH_N);
        wsprintfA(g_line, "; [map] client resolve stored %s -- redirecting %s to it\n", name,
                  g_want_map);
        mlog(g_line);
    } else {
        redirect_clear();
        // WHAT WE ACTUALLY HOLD, which is what the JOIN must report -- the hash of the base file if
        // there is one, and the no-claim zero if there is not. Reporting the WANTED hash here would
        // make every mismatch look like agreement, which is the failure the JOIN field is shaped to
        // prevent (see join_request_for's three-argument form).
        if (pm == PRETEND_NONE)
            memset(g_my_hash, 0, HASH_N);
        else if (pm == PRETEND_OTHER)
            memcpy(g_my_hash, pretend_other_hash(), HASH_N);
        else if (!file_hash(dir_for(g_want_map), g_want_map, g_my_hash, nullptr))
            memset(g_my_hash, 0, HASH_N);
        if (!can_carry()) {
            // mp:X2b: REACHABLE NOW, and expected -- the host claims on every transport. This peer
            // cannot fetch the host's copy, so it does not wait for one: it reports what it holds
            // (g_my_hash, above) and the HOST refuses Start naming it. Nothing here waits.
            g_cs = CS_BLOCKED;
            wsprintfA(g_line, "; [map] client BLOCKED %s -- this transport has no bulk channel, so "
                              "the host's copy cannot be fetched; the host will refuse Start until "
                              "both peers hold the same file\n",
                      g_want_map);
            mlog(g_line);
        } else if (in_resource_pack(g_want_map)) {
            g_cs = CS_BLOCKED;
            wsprintfA(g_line, "; [map] client BLOCKED %s -- this install's resource packs answer for "
                              "that name, so a downloaded copy could never be loaded under it\n",
                      g_want_map);
            mlog(g_line);
        } else {
            g_cs         = CS_NEED;
            g_need_since = GetTickCount();
            mlog("; [map] client resolve missing -- waiting for the host's copy over channel C\n");
        }
    }
    // mp:X2d -- THE BACKSTOP. The host's verdict is whatever our last JOIN said; if this resolve
    // changed the answer, say it again through the same re-JOIN a finished download uses (the host's
    // admit path recomputes from the field, and "holds the map" there clears the transfer).
    if (g_reported_valid && !np::map_hash_equal(g_reported, g_my_hash)) {
        g_rejoin_due = true;
        char hex[np::MAP_HASH_HEX_CAP], hex2[np::MAP_HASH_HEX_CAP];
        wsprintfA(g_line, "; [map] client re-report due -- the JOIN said has=%s, the resolve now says "
                          "has=%s (mp:X2d)\n",
                  np::map_hash_is_none(g_reported) ? "none" : hexof(g_reported, hex, sizeof(hex)),
                  np::map_hash_is_none(g_my_hash) ? "none" : hexof(g_my_hash, hex2, sizeof(hex2)));
        mlog(g_line);
    }
}

// mp:X2d -- THE HARDENING: a peer that holds the map still DRAINS channel C. A transfer the host
// armed before it learned we hold the file (a JOIN that raced the advert, or a stale directory row)
// is otherwise never taken: only MH_Net_SnapshotPoll moves chunks out of the lane, the lane refuses
// at 4 (udp_channel_c.cpp rx_try_admit, backpressure), and the host re-sends chunks 0-3 until its
// 90 s re-arm -- a link held busy for nothing, and one a real transfer to another peer then waits on.
// So the delivery is taken and discarded; its content is, by the host's own claim, what we hold.
void client_drain_unwanted() {
    int len   = g_rx_buf != nullptr ? (int)MAP_MAX_BYTES : 0;
    int state = 0;
    if (!MH_Net_SnapshotPoll(g_rx_buf, &len, &state)) {
        if ((state == MH_SNAP_RECEIVING || state == MH_SNAP_REFUSED) && g_rx_buf == nullptr) {
            g_rx_buf = (uint8_t *)VirtualAlloc(nullptr, MAP_MAX_BYTES, MEM_RESERVE | MEM_COMMIT,
                                               PAGE_READWRITE);
            mlog("; [map] client draining an unwanted map transfer -- this peer already holds the "
                 "content (mp:X2d)\n");
        }
        return;
    }
    wsprintfA(g_line, "; [map] client discarded an unwanted delivery (%d B) -- this peer already "
                      "holds %s (mp:X2d)\n",
              len, g_want_map);
    mlog(g_line);
}

void client_tick() {
    if (!g_want_valid) return;
    if (g_cs == CS_HAVE) { // mp:X2d: holding the map is no reason to leave channel C undrained
        client_drain_unwanted();
        return;
    }
    if (g_cs != CS_NEED) return;
    if (g_rx_buf == nullptr) {
        g_rx_buf = (uint8_t *)VirtualAlloc(nullptr, MAP_MAX_BYTES, MEM_RESERVE | MEM_COMMIT,
                                           PAGE_READWRITE);
        if (g_rx_buf == nullptr) return; // retried next frame; nothing else to do about it
    }
    int len   = (int)MAP_MAX_BYTES;
    int state = 0;
    if (!MH_Net_SnapshotPoll(g_rx_buf, &len, &state)) {
        if (state == MH_SNAP_RECEIVING) {
            MH_NetSnapshotStatus st;
            MH_Net_SnapshotStatus(&st);
            const unsigned pct =
                st.rx_chunks ? (unsigned)((st.rx_verified * 100u) / st.rx_chunks) : 0u;
            wsprintfW(g_notice, L"Downloading the map from the host (%u%%)...", pct);
            g_notice_on = true;
        } else if (!g_notice_on) {
            lstrcpynW(g_notice, L"Waiting for the host to send the map...", 256);
            g_notice_on = true;
        }
        paint_notice();
        return;
    }

    char stored[STORED_PATH_CAP];
    char hex[np::MAP_HASH_HEX_CAP];
    if (!store(g_want_map, g_want_hash, g_rx_buf, (uint32_t)len, stored,
               sizeof(stored))) {
        g_cs = CS_REFUSED;
        wsprintfA(g_line, "; [map] client REFUSED the delivered map (%d B; it does not hash to the "
                          "advertised %s, or it could not be written)\n",
                  len, hexof(g_want_hash, hex, sizeof(hex)));
        mlog(g_line);
        lstrcpynW(g_notice, L"The map the host sent did not match its advert.", 256);
        g_notice_on = true;
        paint_notice();
        return;
    }
    wsprintfA(g_line, "; [map] client stored %s (%d B, sha=%s) -- the local %s is untouched\n",
              stored, len, hexof(g_want_hash, hex, sizeof(hex)), g_want_map);
    mlog(g_line);
    {
        ClientLock l(false);  // mp:X2d: main thread -- waits
        client_resolve_now(); // -> CS_HAVE + the redirect, from the file we just wrote
    }
    g_notice_on                      = false;
    *(wchar_t *)ADDR_MAP_STATUS_LINE = L'\0';
    // TELL THE HOST, and tell it by RE-JOINING rather than through a new frame: the JOIN already
    // carries "what I hold", the host's admit path already recomputes its verdict from that field,
    // and a completion the host learns from the PEER is the receiver-side assertion the gate needs.
    // net_discovery owns the SEND (it is the TU that knows the stored host record and the player
    // name); this is the one-shot flag it drains.
    g_rejoin_due = true;
}

} // namespace

void client_on_advert(const np::SessionInfo &rec) {
    if (!enabled()) return;
    if (np::map_hash_is_none(rec.map_hash) || rec.map[0] == '\0') {
        // A pre-X2 host, or one whose map the packs answer for. Nothing to do, and nothing to hold
        // the Start gate on either side: this is exactly the behaviour that shipped before X2.
        return;
    }
    // mp:X2d: the recv thread only TRIES (THE CLIENT LOCK) -- a dropped advert is re-sent in ~1 s.
    ClientLock l(GetCurrentThreadId() != g_main_tid);
    if (!l.held) {
        mlog("; [map] client advert deferred -- a resolve is running on the main thread (mp:X2d)\n");
        return;
    }
    if (g_want_valid && lstrcmpiA(g_want_map, rec.map) == 0 &&
        np::map_hash_equal(g_want_hash, rec.map_hash))
        return; // the same claim we already acted on -- the advert repeats ~1 Hz
    lstrcpynA(g_want_map, rec.map, sizeof(g_want_map));
    memcpy(g_want_hash, rec.map_hash, HASH_N);
    g_want_size  = rec.map_size;
    g_want_valid = true;
    char hex[np::MAP_HASH_HEX_CAP];
    wsprintfA(g_line, "; [map] client want %s sha=%s size=%lu\n", g_want_map,
              hexof(g_want_hash, hex, sizeof(hex)), (unsigned long)g_want_size);
    mlog(g_line);
    pretend_mode(); // harness-only; a no-op unless [net] map_test_pretend is set
    client_sample_local("before");
    client_resolve_now();
}

bool client_my_hash(uint8_t out[HASH_N]) {
    if (!enabled()) return false;
    ClientLock l(false); // main thread (the JOIN builders) -- waits
    // mp:X2d: every caller is a JOIN builder, so this IS "what the host was told" -- the backstop
    // in client_resolve_now compares against it. No claim known = the no-claim zero was reported.
    if (g_want_valid) memcpy(g_reported, g_my_hash, HASH_N);
    else memset(g_reported, 0, HASH_N);
    g_reported_valid = true;
    if (!g_want_valid) return false;
    memcpy(out, g_my_hash, HASH_N);
    return !np::map_hash_is_none(out);
}

void client_on_start() {
    if (!enabled() || !g_want_valid) return;
    client_sample_local("after");
}

bool client_take_rejoin() {
    if (!g_rejoin_due) return false;
    g_rejoin_due = false;
    return true;
}

// =================================================================================================
// THE SEAM
// =================================================================================================

void install() {
    if (g_installed || !enabled()) return;
    g_installed = true;
    // The replacement is armed even on a peer that never joins anything: `g_redir_on` is what
    // decides whether it does anything, and arming once at MH_Core_Arm keeps the install out of the
    // lobby's frame path. install_export_ok checks the entry bytes and logs its own refusal.
    if (!mh_export_install_utils_open_file())
        mlog("; [map] the file-open resolve seam did NOT arm -- a downloaded map cannot be loaded "
             "this run (the host's Start gate still refuses, so nobody plays the wrong map)\n");
}

void lobby_tick(int is_host) {
    if (!enabled()) return;
    if (is_host) host_tick();
    else client_tick();
}

void session_reset() {
    {
        HostLock l;
        for (int i = 0; i < 8; ++i) g_peer[i] = PeerMap{};
        g_tx_peer    = -1;
        g_host_claim = false;
    }
    ClientLock cl(false); // mp:X2d
    g_want_valid     = false;
    g_reported_valid = false;
    g_cs             = CS_IDLE;
    g_notice_on      = false;
    g_host_map[0]    = '\0';
    g_host_claim     = false;
    g_rejoin_due     = false;
    memset(g_host_hash, 0, HASH_N);
    memset(g_my_hash, 0, HASH_N);
    gate_open(); // hands the DISABLED bit back to retail if we were the ones holding it
    // THE REDIRECT IS NOT CLEARED HERE, and that is deliberate: the lobby ends when the MATCH
    // BEGINS, and the match is precisely when the map file is opened. Clearing it at the lobby exit
    // would un-redirect the load the redirect exists for. It is replaced by the next lobby's, and a
    // process that never joins another one keeps a mapping nothing asks about.
}

} // namespace maps
} // namespace seams
} // namespace mh
