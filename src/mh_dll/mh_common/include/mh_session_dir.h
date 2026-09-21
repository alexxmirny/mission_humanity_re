//
// mh_session_dir.h -- the PURE half of the per-session run directory (SES1).
//
// SES0 gave a match its identity (a UUIDv7 `match_id`, logged as `; [session] match_id=<32 hex>`).
// SES1 gives it a PLACE: the run directory stops being per PROCESS and becomes per SESSION, opened
// when a lobby is created (host) or a JOIN is sent (client) and closed when the peer leaves lockstep
// or the lobby. Before any session -- and after one closes -- output goes to the process's `menu`
// directory, which is exactly today's per-process folder under a new name.
//
// EVERYTHING IN THIS HEADER IS OS-FREE, and that is the point rather than a style choice. The three
// things SES1's acceptance criteria actually assert about naming and rollover --
//
//     * three matches hosted without restarting produce three DISTINCT directory names,
//     * a run that never joins a lobby produces a `menu` directory,
//     * the same fields reach `session.json` as reach the SESSION_BEGIN/SESSION_END log lines,
//
// -- are properties of a NAME and a STATE MACHINE, not of CreateDirectory. Keeping them here lets
// `net_selftest.exe sessiondirtest` prove all three offline, on every build, instead of only on the
// two-VM rig where a third match costs a minute of wall clock. The OS half (GetSystemTime,
// CreateDirectoryA, the breadcrumb) lives in mh_common/run_context.cpp and does nothing this file
// cannot describe. Same split as SES0's uuid7.h / mh_session_id.h pair, for the same reason.
//
// NO CRT. mh.dll is injected into a Watcom binary and mh_common is compiled into it, so the string
// and integer formatting below is hand-rolled (the project's standing rule; see run_context.cpp's
// contains_ci for the same discipline one level down).
//
#ifndef MH_SESSION_DIR_H
#define MH_SESSION_DIR_H

/* "YYYYMMDDTHHMMSSZ" + NUL -- the UTC stamp every directory name starts with. UTC, not local time:
 * a session directory is the unit a bug report ships, and two peers in different time zones must
 * sort into one order. (The LINE stamps inside the logs stay local wall clock -- they are read
 * beside a human's own clock and mh_log_stamp's format is a committed contract.) */
#define MH_SESSION_STAMP_CAP 18

/* "<stamp>_<mid8>_<slot>_<role>" + NUL. 16 + 1 + 8 + 1 + 2 + 1 + 6 = 35 at most today. */
#define MH_SESSION_DIRNAME_CAP 64

/* 32 lowercase hex + NUL. Matches mh_net_proto::UUID7_HEX_CAP without importing it: this header is
 * reachable from mh_common, which does not depend on mh_net_proto. */
#define MH_SESSION_MATCH_HEX_CAP 33

/* The short form that names the directory: 8 hex digits of the match_id.
 *
 * THE LAST EIGHT, NOT THE FIRST, and that is a correction to plan D9's wording rather than a
 * deviation from its intent. A UUIDv7's leading 48 bits are a unix MILLISECOND TIMESTAMP, so the
 * first 8 hex digits are the top 32 bits of it -- constant for 2^16 ms, i.e. just over a minute.
 * MEASURED on SES1's own three-match acceptance run: three lobbies created 24 s apart produced
 * three directories whose short forms were all `01a0b016`, so the handle that exists to tell the
 * matches apart told them apart not at all. The trailing 32 bits are CSPRNG bytes (RFC 9562 5.7
 * rand_b), which is what a handle wants. Uniqueness of the directory NAME never depended on it --
 * the UTC stamp leads every name -- but a human picking one of three folders did.
 *
 * The full id is in every log line and in session.json; this is a handle, never the identity. */
#define MH_SESSION_SHORT_HEX 8

#define MH_SESSION_TEXT_CAP   64
#define MH_SESSION_ROSTER_CAP 256
#define MH_SESSION_LINE_CAP   1024

#ifdef __cplusplus

// ---- formatting primitives (no CRT, no OS) ------------------------------------------------------
// Every one takes (dst, cap, at) and returns the new offset, always leaving dst NUL-terminated and
// never writing at or past dst[cap-1]. A truncated field is a truncated field -- these never fail,
// because a logger that can fail has a second failure mode nobody tests.

inline int mh_sd_put(char *dst, int cap, int at, const char *s) {
    if (s == nullptr) return at;
    while (*s != '\0' && at < cap - 1) dst[at++] = *s++;
    if (at < cap) dst[at] = '\0';
    return at;
}

inline int mh_sd_put_int(char *dst, int cap, int at, long v) {
    char          tmp[24];
    int           n = 0;
    unsigned long u;
    if (v < 0) {
        if (at < cap - 1) dst[at++] = '-';
        u = (unsigned long)(-(v + 1)) + 1u; // LONG_MIN-safe
    } else {
        u = (unsigned long)v;
    }
    if (u == 0) tmp[n++] = '0';
    while (u != 0 && n < (int)sizeof(tmp)) {
        tmp[n++] = (char)('0' + (int)(u % 10u));
        u /= 10u;
    }
    while (n > 0 && at < cap - 1) dst[at++] = tmp[--n];
    if (at < cap) dst[at] = '\0';
    return at;
}

// JSON string body (no surrounding quotes). Escapes the two characters JSON requires and drops
// control bytes -- a player name arrives here from the wire, so this is the untrusted path.
inline int mh_sd_put_json(char *dst, int cap, int at, const char *s) {
    if (s == nullptr) return at;
    for (; *s != '\0' && at < cap - 2; ++s) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            dst[at++] = '\\';
            dst[at++] = (char)c;
        } else if (c >= 0x20) {
            dst[at++] = (char)c;
        }
    }
    if (at < cap) dst[at] = '\0';
    return at;
}

inline int mh_sd_copy(char *dst, int cap, const char *src) {
    int at = mh_sd_put(dst, cap, 0, src);
    if (cap > 0) dst[at < cap ? at : cap - 1] = '\0';
    return at;
}

// "No usable match_id": absent, SHORTER than the 32 hex digits uuid7_hex always writes, or all
// zeroes (uuid7_is_nil's spelling one level up). The length check is not pedantry -- it is what makes
// a truncated id fall back to the `menu` name instead of producing a directory named after a prefix
// that could collide with a real one.
inline bool mh_sd_is_nil_hex(const char *hex) {
    if (hex == nullptr) return true;
    bool all_zero = true;
    for (int i = 0; i < 32; ++i) {
        if (hex[i] == '\0') return true; // short = not a real id
        if (hex[i] != '0') all_zero = false;
    }
    return all_zero;
}

inline bool mh_sd_same_hex(const char *a, const char *b) {
    if (a == nullptr || b == nullptr) return false;
    for (int i = 0; i < 32; ++i) {
        if (a[i] != b[i]) return false;
        if (a[i] == '\0') return true;
    }
    return true;
}

// ---- the directory name -------------------------------------------------------------------------
//
//   in a session : "<stamp>_<match_id[24..31]>_<slot>_<role>" e.g. 20260917T164346Z_dedd707c_1_client
//   otherwise    : "<stamp>_menu_<role>"                      e.g. 20260917T164346Z_menu_solo
//
// BOTH FORMS STILL END IN `_<role>`, and that is load-bearing rather than tidy: tools/mp_run.py's
// newest_run() globs `*_host` / `*_client` and tools/test_ui.py globs `*_solo`. The rename would
// otherwise have been a silent rig outage -- the runs would happen and nothing would find them.
inline int mh_session_dir_name(char *dst, int cap, const char *stamp, const char *match_hex,
                               int slot, const char *role) {
    int at = mh_sd_put(dst, cap, 0, stamp);
    at     = mh_sd_put(dst, cap, at, "_");
    if (mh_sd_is_nil_hex(match_hex)) {
        at = mh_sd_put(dst, cap, at, "menu");
    } else {
        // mh_sd_is_nil_hex has already established that match_hex is at least 32 chars long, so the
        // trailing window below cannot read past the id.
        const char *tail = match_hex + (32 - MH_SESSION_SHORT_HEX);
        for (int i = 0; i < MH_SESSION_SHORT_HEX && tail[i] != '\0' && at < cap - 1; ++i)
            dst[at++] = tail[i];
        if (at < cap) dst[at] = '\0';
        at = mh_sd_put(dst, cap, at, "_");
        at = mh_sd_put_int(dst, cap, at, slot);
    }
    at = mh_sd_put(dst, cap, at, "_");
    at = mh_sd_put(dst, cap, at, (role != nullptr && role[0] != '\0') ? role : "solo");
    return at;
}

// ---- the record ----------------------------------------------------------------------------------
//
// ONE struct for both halves. SESSION_BEGIN prints the fields known at the open; SESSION_END prints
// the ones only the close can know, and session.json carries ALL of them so a tool never has to
// parse the prose. That is the whole reason session.json exists next to two perfectly readable log
// lines: `tools/mp_analyze.py` pairs peers by match_id, and a pairing that depends on a regex over a
// human-facing sentence is a pairing that breaks the first time the sentence is improved.
struct MH_SessionRecord {
    char          match_id[MH_SESSION_MATCH_HEX_CAP];
    int           slot;
    char          role[MH_SESSION_TEXT_CAP];
    char          build[MH_SESSION_TEXT_CAP];
    char          modules[MH_SESSION_TEXT_CAP];
    char          map[MH_SESSION_TEXT_CAP];
    unsigned long map_hash; // FNV-1a over the 0x17c map header (0 = not known)
    char          roster[MH_SESSION_ROSTER_CAP];
    int           sim_step_ms;
    int           lockstep_step_ms;
    char          transport[MH_SESSION_TEXT_CAP];            // mp:SES4: the module that actually BOUND (udp/tcp/none)
    char          transport_configured[MH_SESSION_TEXT_CAP]; // mp:SES4: `[net] transport`'s own value,
        // left EMPTY when it matches `transport` above -- non-empty is the tell that the configured
        // transport did not end up being the one that bound (e.g. its module failed to load).
    char began_utc[MH_SESSION_STAMP_CAP];
    char ended_utc[MH_SESSION_STAMP_CAP];
    char reason[MH_SESSION_TEXT_CAP]; // gameover|leave|host_left|link_lost|timeout|quit
    long final_clock_ms;
    long stall_count;
    long icon_calls;
    long icon_shown;
    char process_dir[MH_SESSION_DIRNAME_CAP]; // the menu directory this session hangs off
};

inline void mh_session_record_clear(MH_SessionRecord *r) {
    char *p = (char *)r;
    for (int i = 0; i < (int)sizeof(MH_SessionRecord); ++i) p[i] = '\0';
}

// FNV-1a, 32-bit. Cheap enough to run over the 380-byte map header on the frame a lobby opens, which
// is what "a map-file hash if cheap" asked for: it answers "are both peers looking at the same map"
// without a file read.
inline unsigned long mh_session_hash(const void *data, int len) {
    const unsigned char *p = (const unsigned char *)data;
    unsigned long        h = 2166136261ul;
    for (int i = 0; i < len; ++i) {
        h ^= (unsigned long)p[i];
        h *= 16777619ul;
    }
    return h;
}

// ---- the two log lines ---------------------------------------------------------------------------
// key=value, one line, `;`-prefixed so they sit in mh_net.log's comment channel exactly like every
// other seam line. Written by the peer that owns the session, into the SESSION directory.

inline int mh_session_begin_line(const MH_SessionRecord *r, char *dst, int cap) {
    int at = mh_sd_put(dst, cap, 0, "; [session] SESSION_BEGIN match_id=");
    at     = mh_sd_put(dst, cap, at, r->match_id);
    at     = mh_sd_put(dst, cap, at, " slot=");
    at     = mh_sd_put_int(dst, cap, at, r->slot);
    at     = mh_sd_put(dst, cap, at, " role=");
    at     = mh_sd_put(dst, cap, at, r->role);
    at     = mh_sd_put(dst, cap, at, " build=");
    at     = mh_sd_put(dst, cap, at, r->build);
    at     = mh_sd_put(dst, cap, at, " modules=");
    at     = mh_sd_put(dst, cap, at, r->modules);
    at     = mh_sd_put(dst, cap, at, " map=");
    at     = mh_sd_put(dst, cap, at, r->map);
    at     = mh_sd_put(dst, cap, at, " map_hash=");
    at     = mh_sd_put_int(dst, cap, at, (long)r->map_hash);
    at     = mh_sd_put(dst, cap, at, " roster=");
    at     = mh_sd_put(dst, cap, at, r->roster);
    at     = mh_sd_put(dst, cap, at, " sim_step_ms=");
    at     = mh_sd_put_int(dst, cap, at, r->sim_step_ms);
    at     = mh_sd_put(dst, cap, at, " lockstep_step_ms=");
    at     = mh_sd_put_int(dst, cap, at, r->lockstep_step_ms);
    at     = mh_sd_put(dst, cap, at, " transport=");
    at     = mh_sd_put(dst, cap, at, r->transport);
    // mp:SES4: the KEY is always here (a fixed line shape, like every other field); the VALUE is
    // only ever non-empty when the operator asked for something else than what bound -- an ordinary
    // run (configured == bound) reads "transport_configured=" with nothing after it.
    at = mh_sd_put(dst, cap, at, " transport_configured=");
    at = mh_sd_put(dst, cap, at, r->transport_configured);
    at = mh_sd_put(dst, cap, at, " began=");
    at = mh_sd_put(dst, cap, at, r->began_utc);
    at = mh_sd_put(dst, cap, at, "\n");
    return at;
}

inline int mh_session_end_line(const MH_SessionRecord *r, char *dst, int cap) {
    int at = mh_sd_put(dst, cap, 0, "; [session] SESSION_END match_id=");
    at     = mh_sd_put(dst, cap, at, r->match_id);
    at     = mh_sd_put(dst, cap, at, " reason=");
    at     = mh_sd_put(dst, cap, at, r->reason[0] ? r->reason : "unknown");
    at     = mh_sd_put(dst, cap, at, " final_clock_ms=");
    at     = mh_sd_put_int(dst, cap, at, r->final_clock_ms);
    at     = mh_sd_put(dst, cap, at, " stall=");
    at     = mh_sd_put_int(dst, cap, at, r->stall_count);
    at     = mh_sd_put(dst, cap, at, " icon_calls=");
    at     = mh_sd_put_int(dst, cap, at, r->icon_calls);
    at     = mh_sd_put(dst, cap, at, " icon_shown=");
    at     = mh_sd_put_int(dst, cap, at, r->icon_shown);
    at     = mh_sd_put(dst, cap, at, " ended=");
    at     = mh_sd_put(dst, cap, at, r->ended_utc);
    at     = mh_sd_put(dst, cap, at, "\n");
    return at;
}

// ---- session.json --------------------------------------------------------------------------------
// One flat object, no nesting, every value either a JSON string or a bare integer. Written at
// SESSION_BEGIN (with `ended`/`reason` empty) and REWRITTEN whole at SESSION_END, so a session the
// process never closed -- a crash, a kill -- still leaves a file naming the match, which is the case
// a report collector cares about most.
inline int mh_session_json(const MH_SessionRecord *r, char *dst, int cap) {
    int at = mh_sd_put(dst, cap, 0, "{\n  \"match_id\": \"");
    at     = mh_sd_put_json(dst, cap, at, r->match_id);
    at     = mh_sd_put(dst, cap, at, "\",\n  \"slot\": ");
    at     = mh_sd_put_int(dst, cap, at, r->slot);
    at     = mh_sd_put(dst, cap, at, ",\n  \"role\": \"");
    at     = mh_sd_put_json(dst, cap, at, r->role);
    at     = mh_sd_put(dst, cap, at, "\",\n  \"build\": \"");
    at     = mh_sd_put_json(dst, cap, at, r->build);
    at     = mh_sd_put(dst, cap, at, "\",\n  \"modules\": \"");
    at     = mh_sd_put_json(dst, cap, at, r->modules);
    at     = mh_sd_put(dst, cap, at, "\",\n  \"map\": \"");
    at     = mh_sd_put_json(dst, cap, at, r->map);
    at     = mh_sd_put(dst, cap, at, "\",\n  \"map_hash\": ");
    at     = mh_sd_put_int(dst, cap, at, (long)r->map_hash);
    at     = mh_sd_put(dst, cap, at, ",\n  \"roster\": \"");
    at     = mh_sd_put_json(dst, cap, at, r->roster);
    at     = mh_sd_put(dst, cap, at, "\",\n  \"sim_step_ms\": ");
    at     = mh_sd_put_int(dst, cap, at, r->sim_step_ms);
    at     = mh_sd_put(dst, cap, at, ",\n  \"lockstep_step_ms\": ");
    at     = mh_sd_put_int(dst, cap, at, r->lockstep_step_ms);
    at     = mh_sd_put(dst, cap, at, ",\n  \"transport\": \"");
    at     = mh_sd_put_json(dst, cap, at, r->transport);
    // mp:SES4: empty unless the ini's configured transport differs from what actually bound.
    at = mh_sd_put(dst, cap, at, "\",\n  \"transport_configured\": \"");
    at = mh_sd_put_json(dst, cap, at, r->transport_configured);
    at = mh_sd_put(dst, cap, at, "\",\n  \"began\": \"");
    at = mh_sd_put_json(dst, cap, at, r->began_utc);
    at = mh_sd_put(dst, cap, at, "\",\n  \"ended\": \"");
    at = mh_sd_put_json(dst, cap, at, r->ended_utc);
    at = mh_sd_put(dst, cap, at, "\",\n  \"reason\": \"");
    at = mh_sd_put_json(dst, cap, at, r->reason);
    at = mh_sd_put(dst, cap, at, "\",\n  \"final_clock_ms\": ");
    at = mh_sd_put_int(dst, cap, at, r->final_clock_ms);
    at = mh_sd_put(dst, cap, at, ",\n  \"stall\": ");
    at = mh_sd_put_int(dst, cap, at, r->stall_count);
    at = mh_sd_put(dst, cap, at, ",\n  \"icon_calls\": ");
    at = mh_sd_put_int(dst, cap, at, r->icon_calls);
    at = mh_sd_put(dst, cap, at, ",\n  \"icon_shown\": ");
    at = mh_sd_put_int(dst, cap, at, r->icon_shown);
    at = mh_sd_put(dst, cap, at, ",\n  \"process_dir\": \"");
    at = mh_sd_put_json(dst, cap, at, r->process_dir);
    at = mh_sd_put(dst, cap, at, "\"\n}\n");
    return at;
}

// ---- the rollover state machine ------------------------------------------------------------------
//
// THREE MATCHES IN ONE PROCESS IS THE CASE THIS EXISTS FOR, and the interesting transitions are the
// ones that are NOT a clean open/close pair:
//
//   begin(X) while idle      -> OPEN X                 (the ordinary case)
//   begin(X) while X is open -> nothing                (the host advertises at ~1 Hz; every tick
//                                                       re-asserts the same lobby and must not mint
//                                                       a directory per second)
//   begin(Y) while X is open -> CLOSE X, then OPEN Y   (a host that leaves and re-creates without
//                                                       the leave reaching us; the old session must
//                                                       still get its SESSION_END)
//   begin(nil)               -> nothing                (a peer with no id has no session to name)
//   end() while idle         -> nothing                (every exit path calls end(); only the first
//                                                       one that finds a session open does anything)
//
// The caller acts on the RETURN: MH_SESSION_OPENED means "create the directory and write
// SESSION_BEGIN", MH_SESSION_CLOSED means "write SESSION_END and fall back to the menu directory",
// and MH_SESSION_ROLLED means both, in that order.
enum { MH_SESSION_NONE   = 0,
       MH_SESSION_OPENED = 1,
       MH_SESSION_CLOSED = 2,
       MH_SESSION_ROLLED = 3 };

struct MH_SessionState {
    int           active;
    char          match_id[MH_SESSION_MATCH_HEX_CAP];
    int           slot;
    unsigned long opened; // how many sessions this process has opened (1-based while active)
};

inline void mh_session_state_clear(MH_SessionState *s) {
    s->active      = 0;
    s->slot        = 0;
    s->opened      = 0;
    s->match_id[0] = '\0';
}

inline int mh_session_state_begin(MH_SessionState *s, const char *match_hex, int slot) {
    if (mh_sd_is_nil_hex(match_hex)) return MH_SESSION_NONE;
    if (s->active && mh_sd_same_hex(s->match_id, match_hex)) return MH_SESSION_NONE;
    int rolled = s->active ? MH_SESSION_CLOSED : 0;
    mh_sd_copy(s->match_id, MH_SESSION_MATCH_HEX_CAP, match_hex);
    s->slot   = slot;
    s->active = 1;
    s->opened += 1u;
    return rolled | MH_SESSION_OPENED;
}

inline int mh_session_state_end(MH_SessionState *s) {
    if (!s->active) return MH_SESSION_NONE;
    s->active = 0;
    return MH_SESSION_CLOSED;
}

#endif /* __cplusplus */

#endif /* MH_SESSION_DIR_H */
