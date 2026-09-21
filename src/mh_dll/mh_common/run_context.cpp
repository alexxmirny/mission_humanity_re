//
// run_context.cpp -- see include/mh_run_context.h. Owns the process's log directories.
//
// Until SES1 this file computed ONE directory per process and handed it to everybody. It now owns
// two: the PROCESS ("menu") directory, minted once exactly as before, and the CURRENT directory,
// which is the menu directory except while a match is in progress. The naming, the rollover rules
// and the record written into each session directory are all in mh_common/include/mh_session_dir.h,
// which is OS-free and proven by `net_selftest.exe sessiondirtest`; this file is the half that talks
// to the filesystem and the clock.
//
#include <windows.h>
#include "include/mh_run_context.h"
#include "include/mh_session_dir.h"

namespace {

char          g_exe_dir[MAX_PATH]  = {0}; // "...\"
char          g_proc_dir[MAX_PATH] = {0}; // "...\logs\<stamp>_menu_<role>\"  -- the process directory
char          g_run_dir[MAX_PATH]  = {0}; // the CURRENT directory: g_proc_dir, or the open session's
char          g_role[16]           = {0};
volatile LONG g_state              = 0; // 0=uninit, 1=initialising, 2=ready
volatile LONG g_generation         = 1; // bumped every time g_run_dir changes (see MH_RunDirGeneration)

MH_SessionState g_session = {0, {0}, 0, 0};

// case-insensitive substring test (no CRT dependence in the injected context)
bool contains_ci(const char *hay, const char *needle) {
    for (const char *h = hay; *h; ++h) {
        int i = 0;
        while (needle[i]) {
            char a = h[i], b = needle[i];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) break;
            ++i;
        }
        if (!needle[i]) return true;
    }
    return false;
}

void compute_exe_dir() {
    GetModuleFileNameA(nullptr, g_exe_dir, MAX_PATH);
    char *slash = g_exe_dir;
    for (char *p = g_exe_dir; *p; ++p)
        if (*p == '\\' || *p == '/') slash = p;
    slash[1] = '\0'; // keep trailing backslash
}

// Role: cmdline verb first (authoritative per peer), then mh_net.ini [net] role, else "solo".
const char *detect_role() {
    const char *cl = GetCommandLineA();
    if (cl) {
        if (contains_ci(cl, "--mp-host")) return "host";
        if (contains_ci(cl, "--mp-join")) return "client";
    }
    char ini[MAX_PATH], role[32] = {0};
    wsprintfA(ini, "%smh_net.ini", g_exe_dir);
    GetPrivateProfileStringA("net", "role", "", role, sizeof(role), ini);
    if (role[0] == 'h' || role[0] == 'H') return "host";
    if (role[0] == 'c' || role[0] == 'C') return "client";
    return "solo";
}

// "YYYYMMDDTHHMMSSZ" -- UTC. See MH_SESSION_STAMP_CAP's note on why this one is not local time.
void utc_stamp(char *dst) {
    SYSTEMTIME st;
    GetSystemTime(&st);
    wsprintfA(dst, "%04d%02d%02dT%02d%02d%02dZ",
              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
}

// Point mh_run.txt at the newest directory. It is the breadcrumb a human (and tools/crash_report.py)
// follows when nothing else says where the logs went, and SES1 keeps its meaning by rewriting it on
// every switch rather than only at boot: "the newest directory" is what it always claimed to be.
void write_breadcrumb() {
    char bc[MAX_PATH];
    wsprintfA(bc, "%smh_run.txt", g_exe_dir);
    HANDLE h = CreateFileA(bc, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w;
    WriteFile(h, g_run_dir, lstrlenA(g_run_dir), &w, nullptr);
    WriteFile(h, "\r\n", 2, &w, nullptr);
    CloseHandle(h);
}

// "<base><sep>?<leaf>" into dst if it (plus a NUL) fits in cap bytes; false (dst untouched)
// otherwise. wsprintfA does not bounds-check its destination, so composing a path through it
// against a fixed MAX_PATH buffer is silently unsafe once an install sits at a long enough path
// (LA10: a real player's install did) -- every join in this file goes through this check first.
bool safe_join(char *dst, int cap, const char *base, const char *leaf) {
    int  blen           = lstrlenA(base);
    int  llen           = lstrlenA(leaf);
    bool base_has_slash = blen > 0 && (base[blen - 1] == '\\' || base[blen - 1] == '/');
    int  need           = blen + (base_has_slash ? 0 : 1) + llen + 1; // + separator? + leaf + NUL
    if (need > cap) return false;
    wsprintfA(dst, base_has_slash ? "%s%s" : "%s\\%s", base, leaf);
    return true;
}

// Create "<exedir>logs\<name>\" and write it (with the trailing backslash) into dst.
//
// Returns false only when even the BARE "logs\" root could not be made (e.g. permission denied) --
// the caller then keeps whatever it had, the fallback that has always guaranteed logs are never
// silently lost. A "logs\<name>" that does not fit MAX_PATH, or that Windows' CreateDirectory
// refuses for its own tighter path-length reasons (its documented ceiling is nearer 248 chars than
// MAX_PATH's 260), degrades to the bare "logs\" root INSTEAD of failing outright: a deep install
// path used to make every stream for the whole process scatter loose into the game's own install
// folder, indistinguishable from the DLLs sitting next to it, with nothing under logs\ at all and
// no way to tell from outside the process that it had happened (LA10 -- reproduced on the rig by
// copying a real install to a ~230-char path and driving it through the launcher: `logs\` stayed
// completely empty while mh_net.log/mh_capture.log/mh_input.log/mh_video.log/mh_uidrive.log all
// landed in the install root, and mh_run.txt kept naming an EARLIER, unrelated run's directory).
bool make_dir(const char *name, char *dst) {
    char logs_root[MAX_PATH];
    if (!safe_join(logs_root, sizeof(logs_root), g_exe_dir, "logs")) return false; // exe path itself absurd
    CreateDirectoryA(logs_root, nullptr);                                          // ok if it already exists

    char mk[MAX_PATH];
    if (safe_join(mk, sizeof(mk), logs_root, name)) {
        BOOL  ok  = CreateDirectoryA(mk, nullptr);
        DWORD err = GetLastError();
        // The trailing "\\" this function promises its callers needs one more byte than mk alone.
        if ((ok || err == ERROR_ALREADY_EXISTS) && lstrlenA(mk) < MAX_PATH - 2) {
            wsprintfA(dst, "%s\\", mk);
            return true;
        }
    }
    lstrcpynA(dst, logs_root, MAX_PATH);
    int n = lstrlenA(dst);
    if (n > 0 && n < MAX_PATH - 1) {
        dst[n]     = '\\';
        dst[n + 1] = '\0';
    }
    return true;
}

void do_init() {
    compute_exe_dir();
    lstrcpynA(g_role, detect_role(), sizeof(g_role));

    char stamp[MH_SESSION_STAMP_CAP];
    utc_stamp(stamp);
    char name[MH_SESSION_DIRNAME_CAP];
    mh_session_dir_name(name, sizeof(name), stamp, nullptr, 0, g_role); // nil id -> "<stamp>_menu_<role>"

    if (!make_dir(name, g_proc_dir)) {
        lstrcpynA(g_proc_dir, g_exe_dir, MAX_PATH); // fallback: never lose logs
    }
    lstrcpynA(g_run_dir, g_proc_dir, MAX_PATH);
    // ALWAYS write the breadcrumb, even in the exe-dir fallback above: a STALE one left over from an
    // earlier process is actively misleading, not merely uninformative -- that is exactly what made
    // LA10 look like "no directory was ever created" instead of "the fallback fired" (mh_run.txt sat
    // there naming a prior, unrelated run's directory the whole time). An honest breadcrumb naming
    // the exe dir at least says the truth.
    write_breadcrumb();
}

void ensure_init() {
    if (g_state == 2) return;
    if (InterlockedCompareExchange(&g_state, 1, 0) == 0) {
        do_init();
        g_state = 2;
    } else {
        while (g_state != 2) Sleep(0); // another thread is initialising
    }
}

} // namespace

extern "C" const char *MH_RunDir(void) {
    ensure_init();
    return g_run_dir;
}
extern "C" const char *MH_ProcessDir(void) {
    ensure_init();
    return g_proc_dir;
}
extern "C" const char *MH_ExeDir(void) {
    ensure_init();
    return g_exe_dir;
}
extern "C" const char *MH_RunRole(void) {
    ensure_init();
    return g_role;
}
extern "C" unsigned long MH_RunDirGeneration(void) {
    ensure_init();
    return (unsigned long)g_generation;
}
extern "C" int MH_RunDir_SessionActive(void) {
    ensure_init();
    return g_session.active;
}
extern "C" const char *MH_RunDir_SessionMatchId(void) {
    ensure_init();
    return g_session.active ? g_session.match_id : "";
}

// ---- the session transitions ---------------------------------------------------------------------
//
// These are called from ONE place each (mh/seams/net_discovery.cpp's mp_session_open/close), which is
// deliberate: "when does a match begin" is a game question and this file must not answer it. What it
// owns is the consequence -- which directory the next line of every log goes into.
//
// THE GENERATION IS THE WHOLE MECHANISM ON THE READER'S SIDE. Every writer that caches a composed
// path (and nearly all of them do -- they compose once and then open/append/close per line) compares
// MH_RunDirGeneration() with the value it composed at and rebuilds when they differ. One integer
// load per log line, and no writer needs to know a session exists.

extern "C" int MH_RunDir_SessionBegin(const char *match_id_hex, int slot) {
    ensure_init();
    int t = mh_session_state_begin(&g_session, match_id_hex, slot);
    if (!(t & MH_SESSION_OPENED)) return t; // nil id, or the same lobby re-asserting itself
    char stamp[MH_SESSION_STAMP_CAP];
    utc_stamp(stamp);
    char name[MH_SESSION_DIRNAME_CAP];
    mh_session_dir_name(name, sizeof(name), stamp, g_session.match_id, slot, g_role);
    char dir[MAX_PATH];
    if (!make_dir(name, dir)) {
        // The directory could not be created. Keep writing where we were (the per-process fallback
        // this file has always had) and report the session as NOT opened, so the caller does not
        // announce a SESSION_BEGIN into a directory that is really the menu one.
        mh_session_state_end(&g_session);
        return t & MH_SESSION_CLOSED;
    }
    lstrcpynA(g_run_dir, dir, MAX_PATH);
    InterlockedIncrement(&g_generation);
    write_breadcrumb();
    return t;
}

extern "C" int MH_RunDir_SessionEnd(void) {
    ensure_init();
    if (mh_session_state_end(&g_session) == MH_SESSION_NONE) return MH_SESSION_NONE;
    lstrcpynA(g_run_dir, g_proc_dir, MAX_PATH);
    InterlockedIncrement(&g_generation);
    write_breadcrumb();
    return MH_SESSION_CLOSED;
}

extern "C" int MH_RunDir_UtcStamp(char *dst, int cap) {
    if (dst == nullptr || cap < MH_SESSION_STAMP_CAP) return 0;
    utc_stamp(dst);
    return 1;
}

// The process directory's LEAF name ("<stamp>_menu_<role>"), which is what session.json records so a
// tool can find the process-scoped streams (mh_harness.*, the transport module's own banner lines)
// from inside a session directory. A full path would be wrong there: the two directories are copied
// off a VM together and the absolute path does not survive the trip.
extern "C" const char *MH_ProcessDirLeaf(void) {
    ensure_init();
    static char leaf[MH_SESSION_DIRNAME_CAP] = {0};
    if (leaf[0] != '\0') return leaf;
    int n = lstrlenA(g_proc_dir);
    if (n > 0 && (g_proc_dir[n - 1] == '\\' || g_proc_dir[n - 1] == '/')) --n;
    int start = n;
    while (start > 0 && g_proc_dir[start - 1] != '\\' && g_proc_dir[start - 1] != '/') --start;
    int len = n - start;
    if (len <= 0 || len >= (int)sizeof(leaf)) return "";
    for (int i = 0; i < len; ++i) leaf[i] = g_proc_dir[start + i];
    leaf[len] = '\0';
    return leaf;
}
