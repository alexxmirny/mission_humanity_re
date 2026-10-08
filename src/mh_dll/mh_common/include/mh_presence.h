//
// mh_presence.h -- the live `presence.json` (dist RL15) the launcher's Discord module reads
// (presence.json schema v1). mh.dll WRITES it into the LOGS ROOT; the launcher polls.
//
// THREE RULES:
//   * Written only on a STATE CHANGE (mh_presence_publish compares against the last published
//     value, field by field, `updated_unix` and `pid` excluded) -- never per frame.
//   * Never on a game thread: publish() copies the value into a one-slot mailbox and wakes a tiny
//     worker thread that does the file I/O. Latest wins; a burst of changes writes the last one.
//   * Atomic: `presence.json.tmp` is written, then MoveFileEx(REPLACE_EXISTING) renames it, so the
//     launcher never reads half a file.
//
#ifndef MH_PRESENCE_H
#define MH_PRESENCE_H

#ifdef __cplusplus

#include "mh_session_dir.h" // mh_sd_put*, OS-free

#define MH_PRESENCE_TEXT_CAP 64

// Empty string = JSON null; players/max_players < 0 = null; started_unix 0 = null.
struct MH_Presence {
    char state[12]; // menu | lobby | match | campaign | tutorial
    char mode[12];  // skirmish | network | campaign | tutorial | tactical | ""
    char map[MH_PRESENCE_TEXT_CAP];
    char system[MH_PRESENCE_TEXT_CAP];
    char planet[MH_PRESENCE_TEXT_CAP];
    int  players;
    int  max_players;
    long started_unix;
};

inline void mh_presence_clear(MH_Presence *p) {
    char *b = (char *)p;
    for (int i = 0; i < (int)sizeof(MH_Presence); ++i) b[i] = '\0';
    p->players = p->max_players = -1;
    mh_sd_copy(p->state, sizeof(p->state), "menu");
}

inline bool mh_presence_same(const MH_Presence &a, const MH_Presence &b) {
    return lstrcmpA(a.state, b.state) == 0 && lstrcmpA(a.mode, b.mode) == 0 && lstrcmpA(a.map, b.map) == 0 &&
           lstrcmpA(a.system, b.system) == 0 && lstrcmpA(a.planet, b.planet) == 0 && a.players == b.players &&
           a.max_players == b.max_players && a.started_unix == b.started_unix;
}

namespace mh_presence_detail {
inline int put_str_or_null(char *dst, int cap, int at, const char *key, const char *v) {
    at = mh_sd_put(dst, cap, at, key);
    if (v == nullptr || v[0] == '\0') return mh_sd_put(dst, cap, at, "null");
    at = mh_sd_put(dst, cap, at, "\"");
    at = mh_sd_put_json(dst, cap, at, v);
    return mh_sd_put(dst, cap, at, "\"");
}
inline int put_int_or_null(char *dst, int cap, int at, const char *key, long v, bool null) {
    at = mh_sd_put(dst, cap, at, key);
    return null ? mh_sd_put(dst, cap, at, "null") : mh_sd_put_int(dst, cap, at, v);
}
} // namespace mh_presence_detail

// One line, schema v1, key order as the doc's example.
inline int mh_presence_json(const MH_Presence *p, unsigned long pid, long updated_unix, char *dst, int cap) {
    using namespace mh_presence_detail;
    int at = mh_sd_put(dst, cap, 0, "{\"schema\":1,\"pid\":");
    at     = mh_sd_put_int(dst, cap, at, (long)pid);
    at     = put_str_or_null(dst, cap, at, ",\"state\":", p->state);
    at     = put_str_or_null(dst, cap, at, ",\"mode\":", p->mode);
    at     = put_str_or_null(dst, cap, at, ",\"map\":", p->map);
    at     = put_int_or_null(dst, cap, at, ",\"players\":", p->players, p->players < 0);
    at     = put_int_or_null(dst, cap, at, ",\"max_players\":", p->max_players, p->max_players < 0);
    at     = put_str_or_null(dst, cap, at, ",\"system\":", p->system);
    at     = put_str_or_null(dst, cap, at, ",\"planet\":", p->planet);
    at     = put_int_or_null(dst, cap, at, ",\"started_unix\":", p->started_unix, p->started_unix <= 0);
    at     = mh_sd_put(dst, cap, at, ",\"updated_unix\":");
    at     = mh_sd_put_int(dst, cap, at, updated_unix);
    at     = mh_sd_put(dst, cap, at, "}\n");
    return at;
}

// ---- the OS half: mailbox + worker ---------------------------------------------------------------
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

inline long mh_presence_unix_now() {
    FILETIME f;
    GetSystemTimeAsFileTime(&f);
    const unsigned long long t = ((unsigned long long)f.dwHighDateTime << 32) | f.dwLowDateTime;
    return (long)((t - 116444736000000000ull) / 10000000ull);
}

// temp + rename. Returns false when either step fails.
inline bool mh_presence_write_file(const char *path, const char *text, int n) {
    char tmp[MAX_PATH];
    if (lstrlenA(path) + 5 >= MAX_PATH) return false;
    wsprintfA(tmp, "%s.tmp", path);
    HANDLE h = CreateFileA(tmp, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    BOOL  ok    = WriteFile(h, text, (DWORD)n, &wrote, nullptr);
    CloseHandle(h);
    if (!ok || (int)wrote != n) {
        DeleteFileA(tmp);
        return false;
    }
    if (!MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileA(tmp);
        return false;
    }
    return true;
}

namespace mh_presence_detail {

struct mailbox {
    CRITICAL_SECTION cs;
    bool             init;
    HANDLE           ev;
    HANDLE           thread;
    MH_Presence      pending;
    bool             has_pending;
    char             path[MAX_PATH];
    MH_Presence      last; // game-thread only
    bool             have_last;
    volatile LONG    taken, done;
    volatile LONG    writes;
};

inline mailbox &mb() {
    static mailbox m; // zero-initialised
    return m;
}

inline DWORD WINAPI worker(LPVOID) {
    mailbox &m = mb();
    for (;;) {
        WaitForSingleObject(m.ev, INFINITE);
        for (;;) {
            MH_Presence p;
            char        path[MAX_PATH];
            EnterCriticalSection(&m.cs);
            const bool has = m.has_pending;
            if (has) {
                p = m.pending;
                lstrcpyA(path, m.path);
                m.has_pending = false;
                ++m.taken;
            }
            LeaveCriticalSection(&m.cs);
            if (!has) break;
            char text[1024];
            int  n = mh_presence_json(&p, GetCurrentProcessId(), mh_presence_unix_now(), text, (int)sizeof(text));
            if (mh_presence_write_file(path, text, n)) InterlockedIncrement(&m.writes);
            InterlockedIncrement(&m.done);
        }
    }
}

} // namespace mh_presence_detail

// Call from a game thread whenever the CURRENT state may have changed (cheap: one struct compare
// when nothing did). `path` = "<logs root>\presence.json". Returns true when a write was queued.
inline bool mh_presence_publish(const MH_Presence &p, const char *path) {
    using namespace mh_presence_detail;
    mailbox &m = mb();
    if (m.have_last && mh_presence_same(m.last, p)) return false;
    if (!m.init) {
        InitializeCriticalSection(&m.cs);
        m.ev     = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        m.thread = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
        m.init   = true;
    }
    m.last      = p;
    m.have_last = true;
    EnterCriticalSection(&m.cs);
    m.pending     = p;
    m.has_pending = true;
    lstrcpynA(m.path, path, MAX_PATH);
    LeaveCriticalSection(&m.cs);
    SetEvent(m.ev);
    return true;
}

// Selftest/shutdown helper: wait (bounded) until every queued write has finished.
inline bool mh_presence_flush(DWORD ms) {
    using namespace mh_presence_detail;
    mailbox &m = mb();
    if (!m.init) return true;
    const DWORD t0 = GetTickCount();
    for (;;) {
        EnterCriticalSection(&m.cs);
        const bool idle = !m.has_pending && m.done == m.taken;
        LeaveCriticalSection(&m.cs);
        if (idle) return true;
        if (GetTickCount() - t0 > ms) return false;
        Sleep(5);
    }
}

#endif /* __cplusplus */
#endif /* MH_PRESENCE_H */
