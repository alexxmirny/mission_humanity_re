//
// session_dir_selftest.cpp -- `net_selftest.exe sessiondirtest`: the per-SESSION run directory
// (SES1). The NAME, the ROLLOVER state machine and the session.json / SESSION_BEGIN / SESSION_END
// record, with no game, no rig and no filesystem.
//
// WHY THIS SUITE EXISTS, given that SES1's acceptance criteria are written about directories on a
// disk. Three of the five clauses are really claims about a name and a state machine:
//
//   * "three matches hosted without restarting yield three directories" -- three OPENs with three
//     ids must produce three DISTINCT names, and the ~1 Hz re-advert of the SAME id in between must
//     produce none. On the rig that costs a two-VM run and a minute of wall clock per match, and it
//     can only ever demonstrate the case the operator managed to stage. Here it is nine lines.
//   * "a run that never joins a lobby still writes to a menu directory" -- the nil-id name.
//   * "mp_analyze pairs peers by match_id" -- which requires session.json to carry the id in a form
//     a tool reads without parsing prose, and to carry it BEFORE the session ends (a crashed peer
//     never writes its SESSION_END).
//
// The rig still has to prove the other two -- that the directories actually appear and that every
// stream lands in them -- because those are claims about CreateDirectory and about a dozen writers,
// which no offline test can stand in for. What this file removes is the part where a naming bug is
// discovered by staging three matches.
//
// The split that makes it possible is mh_common/include/mh_session_dir.h: OS-free by construction,
// exactly as SES0 split uuid7_make() from the clock and the CSPRNG that feed it.
//
#include <stdio.h>
#include <string.h>

#include "mh_session_dir.h"

namespace {

int g_checks = 0, g_fails = 0;

void check(const char *what, bool ok) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        printf("  FAIL: %s\n", what);
    }
}

const char *ID_A = "01a0af721310721a8d552b0ddedd707c"; // the match_launch id from SES0's rig proof
const char *ID_B = "01a0b00000007111aabbccddeeff0011";
const char *ID_C = "01a0c00000007222112233445566778899"; // deliberately over-long: 32 chars are read
const char *NIL  = "00000000000000000000000000000000";

// Substring search without <string.h>'s strstr, so a failure names the haystack rather than a
// pointer. (Also keeps the fixture honest about what "carries the field" means: the exact bytes.)
bool has(const char *hay, const char *needle) { return strstr(hay, needle) != nullptr; }

void fill(MH_SessionRecord *r, const char *id, int slot, const char *role) {
    mh_session_record_clear(r);
    mh_sd_copy(r->match_id, MH_SESSION_MATCH_HEX_CAP, id);
    r->slot = slot;
    mh_sd_copy(r->role, MH_SESSION_TEXT_CAP, role);
    mh_sd_copy(r->build, MH_SESSION_TEXT_CAP, "0.9.1+abcdef0");
    mh_sd_copy(r->modules, MH_SESSION_TEXT_CAP, "net_abi=F4B00001/started=1");
    mh_sd_copy(r->map, MH_SESSION_TEXT_CAP, "TUTORIAL.MP");
    r->map_hash = 0x1234abcdul;
    mh_sd_copy(r->roster, MH_SESSION_ROSTER_CAP, "0:Halice,1:Hbob");
    r->sim_step_ms      = 20;
    r->lockstep_step_ms = 100;
    mh_sd_copy(r->transport, MH_SESSION_TEXT_CAP, "tcp");
    mh_sd_copy(r->began_utc, MH_SESSION_STAMP_CAP, "20260917T164346Z");
    mh_sd_copy(r->process_dir, MH_SESSION_DIRNAME_CAP, "20260917T164300Z_menu_host");
}

} // namespace

int run_sessiondirtest() {
    printf("=== sessiondirtest (SES1: the per-session run directory -- name, rollover, record) ===\n");

    // ---- 1. the directory NAME -------------------------------------------------------------------
    {
        char n[MH_SESSION_DIRNAME_CAP];

        mh_session_dir_name(n, sizeof(n), "20260917T164346Z", ID_A, 1, "client");
        check("a session name is <stamp>_<mid8>_<slot>_<role>",
              strcmp(n, "20260917T164346Z_dedd707c_1_client") == 0);

        mh_session_dir_name(n, sizeof(n), "20260917T164346Z", ID_A, 0, "host");
        check("the host's slot 0 renders as 0, not as empty",
              strcmp(n, "20260917T164346Z_dedd707c_0_host") == 0);

        // THE SHORT FORM MUST DISCRIMINATE, and taking it from the FRONT of a UUIDv7 does not: the
        // leading 48 bits are a unix-ms timestamp, so three lobbies created inside the same ~65 s
        // share their first 8 hex digits exactly. SES1's own three-match rig run produced three
        // directories all reading `01a0b016`. These three ids are the ones that run minted.
        {
            const char *R1 = "01a0b0167ce5724c88e32eabad75d850";
            const char *R2 = "01a0b016c12070ad81df9e21a718d8d3";
            const char *R3 = "01a0b016da757d7483f40714b1fb3a50";
            char        s1[MH_SESSION_DIRNAME_CAP], s2[MH_SESSION_DIRNAME_CAP], s3[MH_SESSION_DIRNAME_CAP];
            mh_session_dir_name(s1, sizeof(s1), "20260917T155738Z", R1, 0, "host");
            mh_session_dir_name(s2, sizeof(s2), "20260917T155756Z", R2, 0, "host");
            mh_session_dir_name(s3, sizeof(s3), "20260917T155802Z", R3, 0, "host");
            check("three matches minted within a minute get three DIFFERENT short forms",
                  strcmp(s1 + 17, s2 + 17) != 0 && strcmp(s2 + 17, s3 + 17) != 0 &&
                      strcmp(s1 + 17, s3 + 17) != 0);
            check("...and the short form is the id's RANDOM tail, not its timestamp prefix",
                  strcmp(s1, "20260917T155738Z_ad75d850_0_host") == 0);
        }

        // THE GLOB CONTRACT. tools/mp_run.py's newest_run() looks for `*_host` / `*_client` and
        // tools/test_ui.py for `*_solo`. SES1 renamed every directory in the tree; had the role
        // stopped being the last field, every rig tool would have found nothing and reported the
        // peer as never having started -- a silent outage, not a red.
        int ln = (int)strlen(n);
        check("a session directory still ENDS in _<role> (the rig's glob)",
              ln > 5 && strcmp(n + ln - 5, "_host") == 0);

        mh_session_dir_name(n, sizeof(n), "20260917T164346Z", nullptr, 0, "solo");
        check("no match_id -> the MENU directory", strcmp(n, "20260917T164346Z_menu_solo") == 0);
        mh_session_dir_name(n, sizeof(n), "20260917T164346Z", NIL, 3, "host");
        check("an all-zero match_id is 'no id' too, slot ignored",
              strcmp(n, "20260917T164346Z_menu_host") == 0);
        mh_session_dir_name(n, sizeof(n), "20260917T164346Z", "", 3, "host");
        check("an empty match_id is 'no id' too", strcmp(n, "20260917T164346Z_menu_host") == 0);

        mh_session_dir_name(n, sizeof(n), "20260917T164346Z", ID_A, 1, "");
        check("an unknown role falls back to solo rather than a trailing underscore",
              strcmp(n, "20260917T164346Z_dedd707c_1_solo") == 0);

        // Two ids that share a prefix would share a SHORT form; two that do not, must not.
        char a[MH_SESSION_DIRNAME_CAP], b[MH_SESSION_DIRNAME_CAP];
        mh_session_dir_name(a, sizeof(a), "20260917T164346Z", ID_A, 0, "host");
        mh_session_dir_name(b, sizeof(b), "20260917T164346Z", ID_B, 0, "host");
        check("different match_ids give different directory names", strcmp(a, b) != 0);

        // A caller-side truncation must not walk off the buffer.
        char tiny[12];
        mh_session_dir_name(tiny, sizeof(tiny), "20260917T164346Z", ID_A, 1, "client");
        check("a short buffer truncates and stays NUL-terminated", strlen(tiny) < sizeof(tiny));
    }

    // ---- 2. the ROLLOVER state machine -----------------------------------------------------------
    // The acceptance clause reads "three matches hosted WITHOUT RESTARTING". Below is that run, in
    // the order the seams call it: open, re-advert, close, open, ..., with the awkward transitions
    // the rig would have to be provoked into staging.
    {
        MH_SessionState s;
        mh_session_state_clear(&s);

        check("idle: end() does nothing", mh_session_state_end(&s) == MH_SESSION_NONE);
        check("idle: a nil id opens nothing", mh_session_state_begin(&s, NIL, 0) == MH_SESSION_NONE);
        check("...and leaves the state idle", s.active == 0 && s.opened == 0);

        check("match 1 opens", mh_session_state_begin(&s, ID_A, 0) == MH_SESSION_OPENED);
        check("...and is active under its id", s.active == 1 && strcmp(s.match_id, ID_A) == 0);
        // The host re-advertises its SessionInfo at ~1 Hz and each advert re-asserts the same id. If
        // that minted a directory the lobby would grow one folder per second.
        check("the same id re-asserted opens nothing",
              mh_session_state_begin(&s, ID_A, 0) == MH_SESSION_NONE);
        check("...ten times over", [&] {
            for (int i = 0; i < 10; ++i)
                if (mh_session_state_begin(&s, ID_A, 0) != MH_SESSION_NONE) return false;
            return true;
        }());
        check("match 1 closes", mh_session_state_end(&s) == MH_SESSION_CLOSED);
        check("a second close does nothing (every exit seam calls it; only the first acts)",
              mh_session_state_end(&s) == MH_SESSION_NONE);

        check("match 2 opens", mh_session_state_begin(&s, ID_B, 0) == MH_SESSION_OPENED);
        check("match 2 closes", mh_session_state_end(&s) == MH_SESSION_CLOSED);
        check("match 3 opens", mh_session_state_begin(&s, ID_C, 0) == MH_SESSION_OPENED);
        check("three matches, three opens, none of them the same session", s.opened == 3);

        // The case no exit seam covers: a NEW id arriving while one is still open. It happens when a
        // host re-creates a lobby through a path this peer did not observe. The old session must
        // still be reported closed, so its SESSION_END is written before the directory switches.
        check("a DIFFERENT id while one is open rolls over (close AND open)",
              mh_session_state_begin(&s, ID_A, 1) == MH_SESSION_ROLLED);
        check("...the rollover counts as a fourth open", s.opened == 4);
        check("...and the state now carries the new id", strcmp(s.match_id, ID_A) == 0 && s.slot == 1);
        check("a nil id does NOT close an open session",
              mh_session_state_begin(&s, NIL, 0) == MH_SESSION_NONE && s.active == 1);
    }

    // ---- 3. the record: SESSION_BEGIN / SESSION_END / session.json --------------------------------
    {
        MH_SessionRecord r;
        fill(&r, ID_A, 1, "client");

        char line[MH_SESSION_LINE_CAP];
        mh_session_begin_line(&r, line, sizeof(line));
        check("SESSION_BEGIN is one line", strchr(line, '\n') == line + strlen(line) - 1);
        check("SESSION_BEGIN sits in mh_net.log's comment channel", line[0] == ';');
        check("SESSION_BEGIN carries the full 32-hex match_id", has(line, ID_A));
        check("SESSION_BEGIN carries the build", has(line, "build=0.9.1+abcdef0"));
        check("SESSION_BEGIN carries the module version", has(line, "net_abi=F4B00001"));
        check("SESSION_BEGIN carries the map and its hash",
              has(line, "map=TUTORIAL.MP") && has(line, "map_hash=305441741"));
        check("SESSION_BEGIN carries the roster", has(line, "roster=0:Halice,1:Hbob"));
        check("SESSION_BEGIN carries both step periods",
              has(line, "sim_step_ms=20") && has(line, "lockstep_step_ms=100"));
        check("SESSION_BEGIN carries the transport", has(line, "transport=tcp"));
        check("SESSION_BEGIN carries the slot and role", has(line, "slot=1") && has(line, "role=client"));

        // END before the fields exist: a session.json written at OPEN has no reason and no end stamp,
        // and must still be a well-formed record rather than a half-line.
        mh_session_end_line(&r, line, sizeof(line));
        check("SESSION_END with no reason set reads 'unknown' rather than blank",
              has(line, "reason=unknown"));

        mh_sd_copy(r.reason, MH_SESSION_TEXT_CAP, "gameover");
        mh_sd_copy(r.ended_utc, MH_SESSION_STAMP_CAP, "20260917T165501Z");
        r.final_clock_ms = 412300;
        r.stall_count    = 7;
        r.icon_calls     = 118;
        r.icon_shown     = 3;
        mh_session_end_line(&r, line, sizeof(line));
        check("SESSION_END names the match", has(line, ID_A));
        check("SESSION_END names the reason", has(line, "reason=gameover"));
        check("SESSION_END carries the final clock", has(line, "final_clock_ms=412300"));
        check("SESSION_END carries the stall total", has(line, "stall=7"));
        check("SESSION_END carries both icon counters",
              has(line, "icon_calls=118") && has(line, "icon_shown=3"));
        check("SESSION_END carries the end stamp", has(line, "ended=20260917T165501Z"));

        char js[2048];
        int  n = mh_session_json(&r, js, sizeof(js));
        check("session.json is a complete object", js[0] == '{' && n > 0 && js[n - 1] == '\n');
        check("session.json's match_id is a STRING (32 hex is not a number)",
              has(js, "\"match_id\": \"01a0af721310721a8d552b0ddedd707c\""));
        check("session.json's slot is a bare integer", has(js, "\"slot\": 1"));
        check("session.json's map_hash is a bare integer", has(js, "\"map_hash\": 305441741"));
        check("session.json names the process directory (where mh_harness.* live)",
              has(js, "\"process_dir\": \"20260917T164300Z_menu_host\""));
        check("session.json carries the reason and both stamps",
              has(js, "\"reason\": \"gameover\"") && has(js, "\"began\": \"20260917T164346Z\"") &&
                  has(js, "\"ended\": \"20260917T165501Z\""));
        check("session.json carries the same counters as SESSION_END",
              has(js, "\"stall\": 7") && has(js, "\"icon_calls\": 118") && has(js, "\"icon_shown\": 3"));

        // THE OPEN-TIME SNAPSHOT. A peer that crashes mid-match never writes SESSION_END, and the
        // file it leaves behind is the only thing naming the match. It must still parse.
        MH_SessionRecord open_only;
        fill(&open_only, ID_B, 0, "host");
        n = mh_session_json(&open_only, js, sizeof(js));
        check("a session.json written at OPEN is still a complete object",
              js[0] == '{' && js[n - 1] == '\n');
        check("...with the match_id already in it", has(js, ID_B));
        check("...and empty (not missing) end fields",
              has(js, "\"ended\": \"\"") && has(js, "\"reason\": \"\""));
    }

    // ---- 4. the untrusted field ------------------------------------------------------------------
    // The roster is built from names that arrived in a peer's JOIN, i.e. off the wire. A quote or a
    // backslash in a player name must not produce a session.json no tool can read.
    {
        MH_SessionRecord r;
        fill(&r, ID_A, 0, "host");
        mh_sd_copy(r.roster, MH_SESSION_ROSTER_CAP, "0:H\"ev\\il\x01name");
        char js[2048];
        mh_session_json(&r, js, sizeof(js));
        check("a quote in a player name is escaped", has(js, "\\\"ev"));
        check("a backslash in a player name is escaped", has(js, "\\\\il"));
        check("a control byte is dropped rather than emitted raw", !has(js, "\x01"));
        // Balanced quotes are the readable proxy for "this parses": an unescaped one would make the
        // roster value end early and the rest of the object garbage.
        int q = 0;
        for (const char *p = js; *p; ++p)
            if (*p == '"' && (p == js || p[-1] != '\\')) ++q;
        check("the object's quotes still balance", (q % 2) == 0);
    }

    // ---- 5. the primitives, at their edges -------------------------------------------------------
    {
        char b[8];
        b[0]   = '\0';
        int at = mh_sd_put_int(b, (int)sizeof(b), 0, -2147483647L - 1L); // LONG_MIN, the negation trap
        check("LONG_MIN does not overflow the negation", at > 0 && b[0] == '-');
        b[0] = '\0';
        mh_sd_put_int(b, (int)sizeof(b), 0, 0);
        check("zero renders as \"0\"", strcmp(b, "0") == 0);
        char big[32];
        big[0] = '\0';
        mh_sd_put_int(big, (int)sizeof(big), 0, (long)0xfffffffful); // 0xffffffff as a signed long on x86
        check("a 32-bit hash value renders in full", strcmp(big, "-1") == 0 || strcmp(big, "4294967295") == 0);
        check("nil detection: a SHORT id is not a real id", mh_sd_is_nil_hex("01a0af72"));
        check("nil detection: a full non-zero id is real", !mh_sd_is_nil_hex(ID_A));
        check("same_hex compares the whole 32", !mh_sd_same_hex(ID_A, ID_B));
        check("same_hex is reflexive", mh_sd_same_hex(ID_A, ID_A));
    }

    printf("  %d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
