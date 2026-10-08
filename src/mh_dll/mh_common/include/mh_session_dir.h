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

#define MH_SESSION_MAX_PLAYERS     8
#define MH_SESSION_PLAYER_NAME_CAP 64 /* UTF-8 bytes + NUL */

/* "YYYYMMDDTHHMMSSZ" + NUL -- the compact UTC stamp of the RECORD fields (session.json `began` /
 * `ended`, the SESSION_BEGIN/END lines). UTC, not local time: a session directory is the unit a bug
 * report ships, and two peers in different time zones must sort into one order. (The LINE stamps
 * inside the logs stay local wall clock -- they are read beside a human's own clock and
 * mh_log_stamp's format is a committed contract.) */
#define MH_SESSION_STAMP_CAP 18

/* "YYYY-MM-DDTHH-MM-SSZ" + NUL -- the stamp every DIRECTORY name starts with (SES8, 2026-09-29).
 *
 * ISO 8601 with the time's colons replaced by dashes: `:` is illegal in a Windows path, and the
 * ask was a date a human reads at a glance (`2026-09-29T08-15-02Z`, not `20260929T081502Z`).
 * STILL UTC, STILL WITH THE `Z`, for the reason above -- the trailing `Z` is the unambiguous marker
 * that this is not the reader's local clock. Fixed width (20 chars), so a plain string sort of NEW
 * names is still a time sort. A sort that MIXES these with the pre-SES8 compact names is not (`-`
 * sorts before `0`), so readers normalise both to digits first (tools/_rundir.py, the launcher's
 * paths.rs). */
#define MH_SESSION_DIRSTAMP_CAP 21

/* "<dirstamp>_<mid8>_<map>_<mode>" + NUL. 20 + 1 + 8 + 1 + 24 + 1 + 8 = 63 at most today; the
 * slack is for a longer mode word, never for the map (MH_SESSION_MAP_TOKEN_MAX caps it). */
#define MH_SESSION_DIRNAME_CAP 80

/* The map token's cap. A map name is player-authored text (an adopted `.mpm` can be called
 * anything), so it is sanitised AND capped before it reaches a path; see mh_sd_put_map_token. */
#define MH_SESSION_MAP_TOKEN_MAX 24

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
//   in a session : "<dirstamp>_<match_id[24..31]>_<map>_<mode>"
//                    e.g. 2026-09-29T08-15-02Z_dedd707c_blue-monday_host
//                         2026-09-29T08-15-02Z_4be1a9c3_TUTORIAL_tutorial
//   otherwise    : "<dirstamp>_menu_<role>"      e.g. 2026-09-29T08-15-02Z_menu_solo
//
// SES8 (2026-09-29) replaced SES1's `<stamp>_<mid8>_<slot>_<role>`, whose last field was the BOOT
// role -- "solo" for every manual-menu run, so every folder a player ever saw ended `_solo`. The
// mode is now the MATCH's: host / client for a lobby (this peer's role IN THAT LOBBY), campaign /
// tutorial / skirmish / tactical for a single-player match (net_discovery.cpp's solo tick decides),
// and the slot lives in session.json only (it always was there too).
//
// FOUR `_`-SEPARATED FIELDS, ALWAYS. The map token is sanitised so it can never contain `_`, so a
// reader splits on `_` and gets stamp / id-or-"menu" / map / mode without a regex that has to know
// what a map may be called. The process directory keeps its three-field `_menu_<role>` shape
// (tools glob `*_menu_*`; mp_run.py globs `*_<role>` and prefers the `_menu_` one).
//
// The map token: the leaf of the name (no directory part), extension dropped, every byte outside
// [A-Za-z0-9.-] -> `-`, runs collapsed, leading/trailing `-` and `.` trimmed, capped at
// MH_SESSION_MAP_TOKEN_MAX. Nothing left -> "nomap". "blue monday.mpm" -> "blue-monday".
inline int mh_sd_put_map_token(char *dst, int cap, int at, const char *map) {
    const char *leaf = map;
    for (const char *p = map; p != nullptr && *p != '\0'; ++p)
        if (*p == '\\' || *p == '/' || *p == ':') leaf = p + 1;
    const char *end = nullptr; // the LAST '.' of the leaf, if any, ends the name
    for (const char *p = leaf; p != nullptr && *p != '\0'; ++p)
        if (*p == '.') end = p;
    if (end == leaf) end = nullptr; // ".mpm" alone is a name, not an extension
    const int start   = at;
    int       written = 0;
    bool      pending = false; // a '-' owed before the next kept byte
    for (const char *p = leaf; p != nullptr && *p != '\0' && p != end; ++p) {
        const unsigned char c  = (unsigned char)*p;
        const bool          ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '.' || c == '-';
        if (!ok || c == '-') {
            pending = written > 0;
            continue;
        }
        if (c == '.' && written == 0) continue; // no leading '.'
        if (pending) {
            if (written + 2 > MH_SESSION_MAP_TOKEN_MAX || at >= cap - 2) break;
            dst[at++] = '-';
            ++written;
            pending = false;
        }
        if (written >= MH_SESSION_MAP_TOKEN_MAX || at >= cap - 1) break;
        dst[at++] = (char)c;
        ++written;
    }
    while (at > start && (dst[at - 1] == '-' || dst[at - 1] == '.')) --at; // no trailing '-' / '.'
    if (at < cap) dst[at] = '\0';
    if (at == start) at = mh_sd_put(dst, cap, at, "nomap");
    return at;
}

// A mode/role word: lowercase ASCII letters only, so it can never break the four-field split.
inline int mh_sd_put_word(char *dst, int cap, int at, const char *w, const char *fallback) {
    const int start = at;
    for (const char *p = w; p != nullptr && *p != '\0' && at < cap - 1; ++p) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if (c >= 'a' && c <= 'z') dst[at++] = c;
    }
    if (at < cap) dst[at] = '\0';
    if (at == start) at = mh_sd_put(dst, cap, at, fallback);
    return at;
}

// "YYYY-MM-DDTHH-MM-SSZ" from calendar fields (the OS half supplies them from GetSystemTime).
inline int mh_session_dir_stamp(char *dst, int cap, int y, int mo, int d, int h, int mi, int s) {
    const int  f[6]   = {y, mo, d, h, mi, s};
    const char sep[6] = {'-', '-', 'T', '-', '-', 'Z'};
    int        at     = 0;
    for (int i = 0; i < 6; ++i) {
        const int w = (i == 0) ? 4 : 2;
        int       v = f[i] < 0 ? 0 : f[i];
        char      tmp[4];
        for (int k = w - 1; k >= 0; --k) {
            tmp[k] = (char)('0' + v % 10);
            v /= 10;
        }
        for (int k = 0; k < w && at < cap - 1; ++k) dst[at++] = tmp[k];
        if (at < cap - 1) dst[at++] = sep[i];
    }
    if (at < cap) dst[at] = '\0';
    return at;
}

inline int mh_session_dir_name(char *dst, int cap, const char *stamp, const char *match_hex,
                               const char *map, const char *mode) {
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
        at = mh_sd_put_map_token(dst, cap, at, map);
    }
    at = mh_sd_put(dst, cap, at, "_");
    at = mh_sd_put_word(dst, cap, at, mode, "solo");
    return at;
}

// ---- SES8: which SINGLE-PLAYER match is starting ------------------------------------------------
//
// Before SES8 only a lobby opened a session, so a campaign, a tutorial or a loaded skirmish wrote
// its whole match into the process's menu folder. mh/seams/net_discovery.cpp's solo tick now opens
// one at the first IN-MATCH frame, and this is the pure half of that decision: given the two mode
// axes (docs/architecture.md "Two axes of mode") and the tutorial step, which mode word names the
// match -- or nullptr for "this frame is not a single-player match start".
//
// "In a match" is the strategic frame (GAME_MODE 2) OR the sim running under an overlay
// (`sim_running`: the game clock advanced since the last present). The second arm is not an edge
// case: a match's first frames are routinely GAME_MODE 3 -- the tutorial's welcome and step dialogs
// keep it there, with the sim ticking underneath, for as long as they are up.
//
//   GAME_MODE 6 (tactical frame)          -> "tactical"
//   in a match + SESSION_MODE 1           -> "campaign"
//   in a match + SESSION_MODE 2, step!=0  -> "tutorial"   (llm_game_start_tutorial sets step 1)
//   in a match + SESSION_MODE 2, step==0  -> "skirmish"   (Path B without a lobby: a loaded save)
//   SESSION_MODE 3, boot/intro, a still   -> nullptr      (lockstep MP owns its session; a menu
//   clock outside GAME_MODE 2                              has no running sim)
//
// A `--tactical` run (the launch verb that loads a save and synthesises a mission) waits for mode 6
// instead of naming the save's strategic frame -- that process exists to play the mission.
inline const char *mh_session_solo_mode(int game_mode, int session_mode, int tutorial_step, bool tactical_verb,
                                        bool sim_running) {
    if (tactical_verb) return game_mode == 6 ? "tactical" : nullptr;
    if (game_mode == 6) return "tactical";
    if (game_mode == 1 || game_mode == 7) return nullptr; // boot stages, the intro movie
    if (game_mode != 2 && !sim_running) return nullptr;
    if (session_mode == 1) return "campaign";
    if (session_mode == 2) return tutorial_step != 0 ? "tutorial" : "skirmish";
    return nullptr;
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
    char          mode[MH_SESSION_TEXT_CAP]; // SES8: the directory's last field (host/client/campaign/...)
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
    // RL15 (match list for the launcher's report flow). Human player names in slot order (UTF-8),
    // the AI-slot count, and how the match ended. `outcome` is "running" from the open until the
    // close writes finished | quit | desync. It is never "crash" in the file: a process that dies
    // leaves "running" behind, and the LAUNCHER infers crash from that (process gone, outcome still
    // "running", usually with a crash marker) -- the crashing process cannot be trusted to rewrite it.
    int  player_count;
    char players[MH_SESSION_MAX_PLAYERS][MH_SESSION_PLAYER_NAME_CAP];
    int  ai_count;
    char outcome[MH_SESSION_TEXT_CAP];
};

// RL15: the session close `reason` (gameover|leave|host_left|link_lost|timeout|quit|rolled) plus
// "did the desync watch fire during this match" -> the outcome word. A desync wins over everything
// (the match's result is not trustworthy); gameover is a played-out match; every other way out is
// the player (or the link) leaving, i.e. quit.
inline const char *mh_session_outcome(const char *reason, bool desync_fired) {
    if (desync_fired) return "desync";
    if (reason != nullptr && reason[0] == 'g' && reason[1] == 'a') return "finished"; // "gameover"
    return "quit";
}

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
    at     = mh_sd_put(dst, cap, at, "\",\n  \"mode\": \"");
    at     = mh_sd_put_json(dst, cap, at, r->mode);
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
    at = mh_sd_put(dst, cap, at, "\",\n  \"players\": [");
    for (int i = 0; i < r->player_count && i < MH_SESSION_MAX_PLAYERS; ++i) {
        if (i > 0) at = mh_sd_put(dst, cap, at, ", ");
        at = mh_sd_put(dst, cap, at, "\"");
        at = mh_sd_put_json(dst, cap, at, r->players[i]);
        at = mh_sd_put(dst, cap, at, "\"");
    }
    at = mh_sd_put(dst, cap, at, "],\n  \"ai_count\": ");
    at = mh_sd_put_int(dst, cap, at, r->ai_count);
    at = mh_sd_put(dst, cap, at, ",\n  \"outcome\": \"");
    at = mh_sd_put_json(dst, cap, at, r->outcome);
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
