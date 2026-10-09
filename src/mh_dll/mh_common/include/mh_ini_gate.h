//
// mh_ini_gate.h -- THE SHIP GATE for mh_net.ini (dist RL2 + RL6; plan decisions D10, D15, D2).
//
// ONE central place decides, per read, whether the ini is allowed to say what it says:
//
//   class C_USER   a player setting: always honoured.                         (ini_keys.def column 5)
//   class C_DEV    a developer/rig knob: honoured ONLY under `[dev] unlock=1`. Otherwise the CODE
//                  default is used (the caller's `def`) and, if the ini actually carries the key,
//                  ONE line `IGNORED dev key [s] k=v` is logged per key (deduped).
//   class C_FIXED  hardcoded to its SHIP value (ini_keys.def column 4): transport=udp, hub_migration=1,
//                  cheat_gate=1, taskbar_guard=1, key_repeat_fix=1, the mouse defaults, [pause] key=0.
//                  Without the unlock the ini cannot move it (one `FIXED [s] k (ini value v ignored)`
//                  line if it tried). WITH the unlock a fixed key reads like a dev key: the rig's
//                  fix-off negatives (cheat_gate=0, taskbar_guard=0, mouse_absolute=1, [pause]
//                  key=0x19) need exactly that, and "the rig tests the exact shipped binary" holds
//                  because the BINARY is the same -- only the unlock differs.
//   a key NOT in the registry is treated as dev (the default class of a new key).
//   `[dev] unlock` itself and `[log] level` are ALWAYS read.
//
// EVERY ini read in mh.dll / mh_net_udp.dll / libmh.dll goes through mh_ini_get_int / mh_ini_get_str
// (tools/gen_ini_registry.py READERS knows both; a raw GetPrivateProfile* call on a registered key
// is a lint-visible exception, not the norm). The table is generated at compile time from
// src/mh_dll/mh/config/ini_keys.def, the same file the launcher schema comes from -- ONE source.
//
// RL6 -- [log] level = quiet | normal | debug. The level SUPPLIES DEFAULTS for the observer keys
// (LEVEL_KEYS below); an explicit per-key value wins only under the unlock, exactly like any dev
// key. normal = today's ship behaviour (no overrides). debug = the former release_package
// DEBUG_KEYS set. quiet = lockstep_log=0 + frametime_log=0; the desync detection keys (per_step,
// state_ring, enabled) are NOT in any table, so quiet keeps detecting.
//
// HEADER-ONLY, CRT-LIGHT (lstrcmpiA / lstrlenA / wsprintfA only): mh_net_udp.dll and the satellites
// include it without linking mh_common. State is a function-local POD static (constant-initialised,
// no guard, no dynamic init), so each image has its own copy -- each image decides from the same
// ini, so the copies agree. Logging goes through a per-image callback (mh_ini_attach_logger);
// lines produced before one is attached (DllMain, before the log path exists) are buffered and
// flushed on attach.
//
#ifndef MH_INI_GATE_H
#define MH_INI_GATE_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#ifdef __cplusplus

namespace mh_ini_detail {

// The enumerators ini_keys.def's rows spell. Scoped here so they cannot collide with <windows.h>.
enum {
    T_INT,
    T_BOOL,
    T_STRING,
    T_ENUM,
    T_HOTKEY,
    C_USER,
    C_DEV,
    C_FIXED,
    S_NONE,
    S_DISPLAY,
    S_LANGUAGE,
    S_MULTIPLAYER,
    S_DIAGNOSTICS,
    F_NONE         = 0,
    F_EXPERIMENTAL = 1,
    F_RESTART      = 2,
    F_PENDING      = 4
};

struct reg_row {
    const char   *sec;
    const char   *key;
    unsigned char cls; // C_USER / C_DEV / C_FIXED
    const char   *dflt;
};

#define MH_INI_KEY(sec, key, type, dflt, cls, label, subtab, options, flags) {sec, key, (unsigned char)(cls), dflt},
inline const reg_row *reg_table(int *count) {
    static const reg_row rows[] = {
#include "../../mh/config/ini_keys.def"
    };
    *count = (int)(sizeof(rows) / sizeof(rows[0]));
    return rows;
}
#undef MH_INI_KEY

// ---- the level table (RL6) -------------------------------------------------------------------------
// One row per key whose default a log level supplies. -1 in a slot = "this level leaves it alone".
enum { LV_QUIET  = 0,
       LV_NORMAL = 1,
       LV_DEBUG  = 2 };
struct level_key {
    const char *sec;
    const char *key;
    int         quiet; // value under quiet,  or -1
    int         debug; // value under debug,   or -1
};
inline const level_key *level_table(int *count) {
    static const level_key t[] = {
        // quiet: drop the per-frame diagnostic streams. Detection (desync per_step / state_ring /
        // enabled) is deliberately absent from this table, so it stays on at every level.
        {"net", "lockstep_log", 0, -1},
        {"net", "frametime_log", 0, -1},
        // debug: exactly the former tools/release_package.py DEBUG_KEYS -- every one an OBSERVER.
        {"net", "sp_clock_log", -1, 1},
        {"trace", "temporal_sp", -1, 1},
        {"desync", "verbose", -1, 1},
        {"input", "mouse_trace", -1, 1},
        {"desync", "state_record", -1, 1},
    };
    *count = (int)(sizeof(t) / sizeof(t[0]));
    return t;
}

inline const char *level_name(int lv) { return lv == LV_QUIET ? "quiet" : lv == LV_DEBUG ? "debug"
                                                                                         : "normal"; }

// ---- small string helpers --------------------------------------------------------------------------
inline bool ieq(const char *a, const char *b) { return lstrcmpiA(a, b) == 0; }

inline unsigned hash_key(const char *sec, const char *key) {
    unsigned h = 2166136261u;
    for (const char *p = sec; *p; ++p) h = (h ^ (unsigned char)((*p >= 'A' && *p <= 'Z') ? *p + 32 : *p)) * 16777619u;
    h = (h ^ 0x2eu) * 16777619u;
    for (const char *p = key; *p; ++p) h = (h ^ (unsigned char)((*p >= 'A' && *p <= 'Z') ? *p + 32 : *p)) * 16777619u;
    return h == 0 ? 1u : h;
}

inline int parse_int(const char *s) {
    int v = 0, neg = 0;
    while (*s == ' ' || *s == '\t') ++s;
    if (*s == '-') {
        neg = 1;
        ++s;
    }
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}

// ---- the state ---------------------------------------------------------------------------------------
enum { MAX_INIS    = 4,
       MAX_SEEN    = 1024,
       MAX_PENDING = 48,
       LINE_CAP    = 240 };

struct ini_entry {
    char path[MAX_PATH];
    int  used;
    int  unlocked;
    int  level;
};

struct gate_state {
    volatile LONG lock;
    int           test_mode;  // 0 = read the ini; 1 = force unlocked; 2 = force locked
    int           test_level; // 0 = read the ini; else forced LV_* PLUS ONE (the state is zero-initialised)
    void (*logger)(const char *);
    int       banner_done;
    ini_entry inis[MAX_INIS];
    int       next_ini;
    unsigned  seen[MAX_SEEN]; // open-addressed key hashes already resolved while locked
    int       n_pending;
    char      pending[MAX_PENDING][LINE_CAP];
    int       n_dropped;
};

inline gate_state &st() {
    static gate_state s; // POD, zero-initialised: no guard, no dynamic init (loader-lock safe)
    return s;
}

inline void lock_acquire(gate_state &g) {
    while (InterlockedCompareExchange(&g.lock, 1, 0) != 0) Sleep(0);
}
inline void lock_release(gate_state &g) { InterlockedExchange(&g.lock, 0); }

// Emit one line. Never called with the lock held (the logger may re-enter the gate through its own
// ini reads -- seam_log's lazy [net] log_max_mb is exactly that).
inline void emit(const char *line) {
    gate_state &g = st();
    void (*fn)(const char *);
    lock_acquire(g);
    fn = g.logger;
    if (!fn) {
        if (g.n_pending < MAX_PENDING) lstrcpynA(g.pending[g.n_pending++], line, LINE_CAP);
        else ++g.n_dropped;
    }
    lock_release(g);
    if (fn) fn(line);
}

// First sighting of (sec,key) since start/reset? Marks it. False when the table is full (then the
// line is skipped: a runaway ini must not turn the log into a firehose).
inline bool first_sighting(const char *sec, const char *key) {
    gate_state    &g  = st();
    const unsigned h  = hash_key(sec, key);
    bool           ok = false;
    lock_acquire(g);
    for (unsigned i = 0, at = h % MAX_SEEN; i < MAX_SEEN; ++i, at = (at + 1) % MAX_SEEN) {
        if (g.seen[at] == h) break;
        if (g.seen[at] == 0) {
            g.seen[at] = h;
            ok         = true;
            break;
        }
    }
    lock_release(g);
    return ok;
}

inline void trim_comment(char *v) {
    for (char *p = v; *p; ++p)
        if (*p == ';') {
            char *e = p;
            while (e > v && (e[-1] == ' ' || e[-1] == '\t')) --e;
            *e = '\0';
            return;
        }
}

inline int level_from_text(const char *v) {
    if (ieq(v, "quiet")) return LV_QUIET;
    if (ieq(v, "debug")) return LV_DEBUG;
    return LV_NORMAL;
}

// The ini's unlock + level, cached per path (production has exactly one path). Test hooks override.
inline void entry_for(const char *ini, int *unlocked, int *level) {
    gate_state &g  = st();
    int         tm = g.test_mode, tl = g.test_level - 1; // -1 = no forced level
    if (tm != 0 && tl >= 0) {
        *unlocked = tm == 1;
        *level    = tl;
        return;
    }
    int u = 0, lv = LV_NORMAL, found = 0;
    lock_acquire(g);
    for (int i = 0; i < MAX_INIS; ++i)
        if (g.inis[i].used && lstrcmpiA(g.inis[i].path, ini) == 0) {
            u     = g.inis[i].unlocked;
            lv    = g.inis[i].level;
            found = 1;
            break;
        }
    lock_release(g);
    if (!found) {
        u = GetPrivateProfileIntA("dev", "unlock", 0, ini) != 0;
        char v[32];
        v[0] = '\0';
        GetPrivateProfileStringA("log", "level", "debug", v, sizeof(v), ini);
        trim_comment(v);
        lv = level_from_text(v);
        lock_acquire(g);
        ini_entry &e = g.inis[g.next_ini];
        g.next_ini   = (g.next_ini + 1) % MAX_INIS;
        lstrcpynA(e.path, ini, MAX_PATH);
        e.unlocked = u;
        e.level    = lv;
        e.used     = 1;
        lock_release(g);
    }
    *unlocked = tm == 1 ? 1 : tm == 2 ? 0
                                      : u;
    *level    = tl >= 0 ? tl : lv;
}

inline const reg_row *find_row(const char *sec, const char *key) {
    int            n;
    const reg_row *t = reg_table(&n);
    for (int i = 0; i < n; ++i)
        if (ieq(t[i].key, key) && ieq(t[i].sec, sec)) return &t[i];
    return nullptr;
}

// The level's default for (sec,key), if it supplies one.
inline bool level_default(int level, const char *sec, const char *key, int *out) {
    if (level == LV_NORMAL) return false;
    int              n;
    const level_key *t = level_table(&n);
    for (int i = 0; i < n; ++i)
        if (ieq(t[i].key, key) && ieq(t[i].sec, sec)) {
            const int v = level == LV_QUIET ? t[i].quiet : t[i].debug;
            if (v < 0) return false;
            *out = v;
            return true;
        }
    return false;
}

// "Does the ini carry this key, and what does it say?" (locked path only). Returns true if present.
inline bool probe_value(const char *section, const char *key, const char *ini, char *out, int cap) {
    static const char SENT[] = "\x01\x02<mh-ini-absent>";
    GetPrivateProfileStringA(section, key, SENT, out, (DWORD)cap, ini);
    if (lstrcmpA(out, SENT) == 0) return false;
    trim_comment(out);
    return true;
}

// Locked read of a dev/fixed key: log once if the ini tried to set it.
inline void note_ignored(const char *section, const char *key, const char *ini, const reg_row *row) {
    if (!first_sighting(section, key)) return;
    char v[160];
    if (!probe_value(section, key, ini, v, sizeof(v))) return; // absent: nothing was ignored
    char line[LINE_CAP];
    if (row && row->cls == C_FIXED)
        wsprintfA(line, "; FIXED [%s] %s (ini value %s ignored; ship value %s)\n", section, key, v, row->dflt);
    else
        wsprintfA(line, "; IGNORED dev key [%s] %s=%s\n", section, key, v);
    emit(line);
}

inline bool is_always_read(const char *sec, const char *key) {
    return (ieq(sec, "dev") && ieq(key, "unlock")) || (ieq(sec, "log") && ieq(key, "level"));
}

} // namespace mh_ini_detail

// ---- public API ----------------------------------------------------------------------------------------

// True when `[dev] unlock=1` is in effect for this ini.
inline bool mh_ini_dev_unlocked(const char *ini) {
    int u, lv;
    if (ini == nullptr || ini[0] == '\0') return false;
    mh_ini_detail::entry_for(ini, &u, &lv);
    return u != 0;
}

// The effective log level for this ini: 0 quiet, 1 normal, 2 debug.
inline int mh_ini_log_level(const char *ini) {
    int u, lv;
    if (ini == nullptr || ini[0] == '\0') return mh_ini_detail::LV_NORMAL;
    mh_ini_detail::entry_for(ini, &u, &lv);
    return lv;
}

// The gated GetPrivateProfileIntA. Same signature, same absent-key behaviour (returns `def`).
inline UINT mh_ini_get_int(const char *section, const char *key, int def, const char *ini) {
    using namespace mh_ini_detail;
    if (ini == nullptr || ini[0] == '\0' || is_always_read(section, key)) return GetPrivateProfileIntA(section, key, def, ini);
    const reg_row *row = find_row(section, key);
    const int      cls = row ? row->cls : C_DEV;
    if (cls == C_USER) return GetPrivateProfileIntA(section, key, def, ini);
    int unlocked, level;
    entry_for(ini, &unlocked, &level);
    int eff = def;
    if (cls == C_DEV) level_default(level, section, key, &eff);
    if (unlocked) return GetPrivateProfileIntA(section, key, eff, ini);
    note_ignored(section, key, ini, row);
    return (UINT)(cls == C_FIXED ? parse_int(row->dflt) : eff);
}

// The gated GetPrivateProfileStringA (NO ';comment' stripping -- mh::config::read_ini_string adds that).
inline DWORD mh_ini_get_str(const char *section, const char *key, const char *def, char *out, DWORD cap, const char *ini) {
    using namespace mh_ini_detail;
    if (ini == nullptr || ini[0] == '\0' || is_always_read(section, key)) return GetPrivateProfileStringA(section, key, def, out, cap, ini);
    const reg_row *row = find_row(section, key);
    const int      cls = row ? row->cls : C_DEV;
    if (cls == C_USER) return GetPrivateProfileStringA(section, key, def, out, cap, ini);
    int unlocked, level;
    entry_for(ini, &unlocked, &level);
    if (unlocked) return GetPrivateProfileStringA(section, key, def, out, cap, ini);
    note_ignored(section, key, ini, row);
    const char *v = cls == C_FIXED ? row->dflt : def;
    if (out == nullptr || cap == 0) return 0;
    lstrcpynA(out, v ? v : "", (int)cap);
    return (DWORD)lstrlenA(out);
}

// The effective set the level supplies, as ONE log line (RL6): "; [log] level=debug effective: ...".
inline int mh_ini_level_line(int level, char *out, int cap) {
    using namespace mh_ini_detail;
    char             tmp[LINE_CAP * 2];
    int              n = wsprintfA(tmp, "; [log] level=%s effective:", level_name(level));
    int              cnt;
    const level_key *t     = level_table(&cnt);
    int              shown = 0;
    for (int i = 0; i < cnt; ++i) {
        int v;
        if (!level_default(level, t[i].sec, t[i].key, &v)) continue;
        n += wsprintfA(tmp + n, " %s.%s=%d", t[i].sec, t[i].key, v);
        ++shown;
    }
    if (!shown) n += wsprintfA(tmp + n, " (no overrides; defaults)");
    n += wsprintfA(tmp + n, "\n");
    lstrcpynA(out, tmp, cap);
    return lstrlenA(out);
}

// Attach this image's logger and flush what was buffered. The FIRST attach for an ini also writes the
// start banner: `DEV UNLOCKED` when the unlock is set, and the effective [log] level set.
inline void mh_ini_attach_logger(void (*fn)(const char *), const char *ini) {
    using namespace mh_ini_detail;
    gate_state &g = st();
    int         n;
    char        batch[MAX_PENDING][LINE_CAP];
    lock_acquire(g);
    g.logger = fn;
    n        = g.n_pending;
    for (int i = 0; i < n; ++i) lstrcpynA(batch[i], g.pending[i], LINE_CAP);
    g.n_pending       = 0;
    const int dropped = g.n_dropped;
    g.n_dropped       = 0;
    const int banner  = !g.banner_done && fn != nullptr && ini != nullptr && ini[0] != '\0';
    if (banner) g.banner_done = 1;
    lock_release(g);
    if (!fn) return;
    if (banner) {
        if (mh_ini_dev_unlocked(ini))
            fn("; DEV UNLOCKED ([dev] unlock=1): dev and fixed keys in mh_net.ini are honoured\n");
        char line[LINE_CAP * 2];
        mh_ini_level_line(mh_ini_log_level(ini), line, sizeof(line));
        fn(line);
    }
    for (int i = 0; i < n; ++i) fn(batch[i]);
    if (dropped > 0) {
        char line[LINE_CAP];
        wsprintfA(line, "; ini gate: %d further line(s) dropped before the log was attached\n", dropped);
        fn(line);
    }
}

// Test hooks (selftests only; the product never calls these). mode: 0 = read the ini, 1 = force
// unlocked, 2 = force locked. level: -1 = read the ini, else 0 quiet / 1 normal / 2 debug (a forced
// mode needs a forced level to take effect, so pass both).
inline void mh_ini_gate_test_force(int mode, int level) {
    mh_ini_detail::gate_state &g = mh_ini_detail::st();
    g.test_mode                  = mode;
    g.test_level                 = level + 1; // stored +1: the zero-initialised product state means "not forced"
}
// Forget every cached ini, every dedupe mark, the logger and the pending buffer.
inline void mh_ini_gate_reset() {
    mh_ini_detail::gate_state &g = mh_ini_detail::st();
    mh_ini_detail::lock_acquire(g);
    const int tm = g.test_mode, tl = g.test_level;
    ZeroMemory(&g, sizeof(g));
    g.test_mode  = tm;
    g.test_level = tl;
    mh_ini_detail::lock_release(g);
}
// How many registry rows exist / of which class (selftest: the table is the .def).
inline int mh_ini_registry_count(int cls_enum_value) {
    int                           n;
    const mh_ini_detail::reg_row *t = mh_ini_detail::reg_table(&n);
    int                           c = 0;
    for (int i = 0; i < n; ++i)
        if (cls_enum_value < 0 || t[i].cls == cls_enum_value) ++c;
    return c;
}
// The class of a key: 'u' user, 'd' dev, 'f' fixed, '?' not in the registry (treated as dev).
inline char mh_ini_key_class(const char *section, const char *key) {
    const mh_ini_detail::reg_row *r = mh_ini_detail::find_row(section, key);
    if (!r) return '?';
    return r->cls == mh_ini_detail::C_USER ? 'u' : r->cls == mh_ini_detail::C_FIXED ? 'f'
                                                                                    : 'd';
}

#endif // __cplusplus
#endif // MH_INI_GATE_H
