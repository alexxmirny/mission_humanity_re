//
// mh_log_prune.h -- LOG RETENTION (dist RL5). Prunes old session and process folders under the logs
// root, so a player who plays for months does not grow `logs\` without bound. Runs inside mh.dll
// (works with no launcher), once at start, on a worker thread (never the game thread).
//
// THE RULES (the decision is pure -- mh_prune_select -- and proven by `net_selftest sessiondirtest`):
//
//   * Two kinds of folder under the logs root: PROCESS folders (`<UTC>_menu_<role>`) and SESSION
//     folders (everything else shaped like `<UTC>_...`). Anything not shaped like that (the
//     launcher's own files and folders, crash markers, reports) is never touched.
//   * [log] keep_sessions = N (default 20): keep the newest N folders OF EACH KIND (by creation
//     time). 0 = this rule is off.
//   * [log] keep_days = D (default 0 = off): additionally keep every folder younger than D days.
//   * A folder is kept when EITHER rule keeps it. With both rules off nothing is pruned.
//   * NEVER pruned, and not counted against N: this run's own process folder; any folder tied to a
//     crash marker that has no `.reported` sibling. A marker is `mh_crash_<pid>.marker`, written by
//     the crash handler into the LOGS ROOT (or inside a folder, if the launcher put it there). It
//     protects: the folder it sits in; the session folder whose name carries the last 8 hex digits of
//     the marker's `match_id=` (that is the `<mid8>` of the folder name); and the newest PROCESS
//     folder created at or before the marker file (the run that crashed). The launcher's report flow
//     creates `<marker file name>.reported` (an empty file) once the crash has been sent or
//     dismissed (RL14); until then the evidence stays. After that the folder is prunable like any.
//
#ifndef MH_LOG_PRUNE_H
#define MH_LOG_PRUNE_H

#ifdef __cplusplus

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#define MH_PRUNE_MAX_ENTRIES           4096
#define MH_PRUNE_NAME_CAP              128
#define MH_PRUNE_DEFAULT_KEEP_SESSIONS 20

struct MH_PruneEntry {
    char               name[MH_PRUNE_NAME_CAP];
    unsigned long long created; // FILETIME, 100 ns ticks
    bool               is_process;
    bool               protect;
    bool               del; // output
};

// ---- the pure half -------------------------------------------------------------------------------

namespace mh_prune_detail {

inline bool is_digit(char c) { return c >= '0' && c <= '9'; }

// Is `a` newer than `b`? Ties broken by name so the order is total (a tie would otherwise let two
// folders both claim the last kept slot).
inline bool newer(const MH_PruneEntry &a, const MH_PruneEntry &b) {
    if (a.created != b.created) return a.created > b.created;
    return lstrcmpA(a.name, b.name) > 0;
}

inline bool contains(const char *hay, const char *needle) {
    const int nl = lstrlenA(needle);
    if (nl == 0) return true;
    for (const char *h = hay; *h; ++h) {
        int i = 0;
        while (i < nl && h[i] == needle[i]) ++i;
        if (i == nl) return true;
    }
    return false;
}

} // namespace mh_prune_detail

// Does `name` look like a folder WE created: a year, then `-` (new `2026-10-08T...`) or a digit
// (pre-SES8 `20260921_...`), and at least one `_`.
inline bool mh_prune_is_ours(const char *name) {
    using mh_prune_detail::is_digit;
    if (!is_digit(name[0]) || !is_digit(name[1]) || !is_digit(name[2]) || !is_digit(name[3])) return false;
    if (name[4] != '-' && !is_digit(name[4])) return false;
    for (const char *p = name; *p; ++p)
        if (*p == '_') return true;
    return false;
}

inline bool mh_prune_is_process_name(const char *name) { return mh_prune_detail::contains(name, "_menu_"); }

// Sets e[i].del. `now` and `created` are FILETIME ticks.
inline int mh_prune_select(MH_PruneEntry *e, int n, int keep_sessions, int keep_days, unsigned long long now) {
    const unsigned long long DAY     = 864000000000ull; // 24*3600*1e7
    int                      deleted = 0;
    for (int i = 0; i < n; ++i) {
        e[i].del = false;
        if (e[i].protect) continue;
        if (keep_sessions <= 0 && keep_days <= 0) continue; // both rules off: keep everything
        int rank = 0;                                       // how many UNPROTECTED same-kind folders are newer
        for (int j = 0; j < n; ++j)
            if (j != i && !e[j].protect && e[j].is_process == e[i].is_process && mh_prune_detail::newer(e[j], e[i]))
                ++rank;
        bool keep = false;
        if (keep_sessions > 0 && rank < keep_sessions) keep = true;
        if (keep_days > 0 && now >= e[i].created && (now - e[i].created) <= (unsigned long long)keep_days * DAY) keep = true;
        if (keep_days > 0 && now < e[i].created) keep = true; // a clock step backwards: never delete on it
        if (!keep) {
            e[i].del = true;
            ++deleted;
        }
    }
    return deleted;
}

// ---- the OS half ---------------------------------------------------------------------------------

namespace mh_prune_detail {

inline unsigned long long ft(const FILETIME &f) { return ((unsigned long long)f.dwHighDateTime << 32) | f.dwLowDateTime; }

inline bool join(char *dst, int cap, const char *a, const char *b) {
    const int al = lstrlenA(a), bl = lstrlenA(b);
    if (al + 1 + bl + 1 > cap) return false;
    for (int i = 0; i < al; ++i) dst[i] = a[i];
    dst[al] = '\\';
    for (int i = 0; i <= bl; ++i) dst[al + 1 + i] = b[i];
    return true;
}

inline bool starts_with(const char *s, const char *p) {
    while (*p) {
        if (*s++ != *p++) return false;
    }
    return true;
}

inline bool ends_with(const char *s, const char *suf) {
    const int sl = lstrlenA(s), fl = lstrlenA(suf);
    return sl >= fl && lstrcmpiA(s + sl - fl, suf) == 0;
}

inline bool is_marker_name(const char *n) { return starts_with(n, "mh_crash_") && ends_with(n, ".marker"); }

inline bool file_exists(const char *path) { return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES; }

// Recursive delete that never follows a reparse point (a junction inside a log folder is removed as
// a link, its target is left alone).
inline void remove_tree(const char *dir, int depth) {
    if (depth > 8) return;
    char pat[MAX_PATH];
    if (!join(pat, sizeof(pat), dir, "*")) return;
    WIN32_FIND_DATAA fd;
    HANDLE           h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (lstrcmpA(fd.cFileName, ".") == 0 || lstrcmpA(fd.cFileName, "..") == 0) continue;
            char p[MAX_PATH];
            if (!join(p, sizeof(p), dir, fd.cFileName)) continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) RemoveDirectoryA(p);
                else remove_tree(p, depth + 1);
            } else {
                SetFileAttributesA(p, FILE_ATTRIBUTE_NORMAL);
                DeleteFileA(p);
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(dir);
}

// Reads the `match_id=` value (32 hex) out of a marker file into out[9] (its last 8 digits).
inline bool marker_mid8(const char *path, char *out) {
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    char  buf[2048];
    DWORD got = 0;
    ReadFile(h, buf, sizeof(buf) - 1, &got, nullptr);
    CloseHandle(h);
    buf[got] = '\0';
    for (const char *p = buf; *p; ++p) {
        if ((p == buf || p[-1] == '\n') && starts_with(p, "match_id=")) {
            const char *v   = p + 9;
            int         len = 0;
            while (v[len] && v[len] != '\r' && v[len] != '\n') ++len;
            if (len < 8) return false;
            for (int i = 0; i < 8; ++i) out[i] = v[len - 8 + i];
            out[8] = '\0';
            return true;
        }
    }
    return false;
}

struct marker_info {
    char               mid8[9];
    unsigned long long created;
};

} // namespace mh_prune_detail

// Scan `root`, decide, delete. `keep_name` = this run's process folder leaf (never touched).
// Returns the number of folders removed; *scanned (optional) = the folders considered.
inline int mh_prune_run(const char *root, const char *keep_name, int keep_sessions, int keep_days, int *scanned) {
    using namespace mh_prune_detail;
    if (scanned) *scanned = 0;
    HANDLE         heap = GetProcessHeap();
    MH_PruneEntry *e    = (MH_PruneEntry *)HeapAlloc(heap, HEAP_ZERO_MEMORY, sizeof(MH_PruneEntry) * MH_PRUNE_MAX_ENTRIES);
    marker_info   *mk   = (marker_info *)HeapAlloc(heap, HEAP_ZERO_MEMORY, sizeof(marker_info) * 256);
    if (!e || !mk) {
        if (e) HeapFree(heap, 0, e);
        if (mk) HeapFree(heap, 0, mk);
        return 0;
    }
    int n = 0, nm = 0;

    char pat[MAX_PATH];
    if (!join(pat, sizeof(pat), root, "*")) {
        HeapFree(heap, 0, e);
        HeapFree(heap, 0, mk);
        return 0;
    }
    WIN32_FIND_DATAA fd;
    HANDLE           h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (lstrcmpA(fd.cFileName, ".") == 0 || lstrcmpA(fd.cFileName, "..") == 0) continue;
            char full[MAX_PATH];
            if (!join(full, sizeof(full), root, fd.cFileName)) continue;
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                // A marker in the root itself.
                if (is_marker_name(fd.cFileName) && nm < 256) {
                    char rep[MAX_PATH + 16];
                    wsprintfA(rep, "%s.reported", full);
                    if (!file_exists(rep)) {
                        mk[nm].mid8[0] = '\0';
                        marker_mid8(full, mk[nm].mid8);
                        mk[nm].created = ft(fd.ftCreationTime);
                        ++nm;
                    }
                }
                continue;
            }
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
            if (!mh_prune_is_ours(fd.cFileName) || lstrlenA(fd.cFileName) >= MH_PRUNE_NAME_CAP) continue;
            if (n >= MH_PRUNE_MAX_ENTRIES) continue;
            MH_PruneEntry &x = e[n++];
            lstrcpyA(x.name, fd.cFileName);
            x.created    = ft(fd.ftCreationTime);
            x.is_process = mh_prune_is_process_name(fd.cFileName);
            x.protect    = (keep_name != nullptr && lstrcmpA(fd.cFileName, keep_name) == 0);
            x.del        = false;
            // A marker inside the folder itself.
            char inner[MAX_PATH];
            if (join(inner, sizeof(inner), full, "mh_crash_*.marker")) {
                WIN32_FIND_DATAA fm;
                HANDLE           hm = FindFirstFileA(inner, &fm);
                if (hm != INVALID_HANDLE_VALUE) {
                    do {
                        char mp[MAX_PATH], rep[MAX_PATH + 16];
                        if (!join(mp, sizeof(mp), full, fm.cFileName)) continue;
                        wsprintfA(rep, "%s.reported", mp);
                        if (!file_exists(rep)) x.protect = true;
                    } while (FindNextFileA(hm, &fm));
                    FindClose(hm);
                }
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }

    // Root-level unreported markers protect: the session folder carrying their mid8, and the newest
    // process folder created at or before the marker.
    for (int m = 0; m < nm; ++m) {
        int best = -1;
        for (int i = 0; i < n; ++i) {
            if (mk[m].mid8[0] != '\0' && !e[i].is_process) {
                char needle[16];
                wsprintfA(needle, "_%s_", mk[m].mid8);
                if (contains(e[i].name, needle)) e[i].protect = true;
            }
            if (e[i].is_process && e[i].created <= mk[m].created && (best < 0 || e[i].created > e[best].created)) best = i;
        }
        if (best >= 0) e[best].protect = true;
    }

    FILETIME nowft;
    GetSystemTimeAsFileTime(&nowft);
    const int total = mh_prune_select(e, n, keep_sessions, keep_days, ft(nowft));
    int       done  = 0;
    for (int i = 0; i < n && total > 0; ++i) {
        if (!e[i].del) continue;
        char full[MAX_PATH];
        if (!join(full, sizeof(full), root, e[i].name)) continue;
        remove_tree(full, 0);
        ++done;
    }
    if (scanned) *scanned = n;
    HeapFree(heap, 0, e);
    HeapFree(heap, 0, mk);
    return done;
}

#endif /* __cplusplus */
#endif /* MH_LOG_PRUNE_H */
