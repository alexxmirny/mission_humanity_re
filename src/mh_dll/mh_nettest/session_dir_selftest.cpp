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

#include <windows.h>
#include <string>

#include "mh_log_prune.h"
#include "mh_presence.h"
#include "mh_session_dir.h"
#include "seams/session_close_plan.h" // TL-HARN-CLEANCLOSE -- section 6

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
    mh_sd_copy(r->process_dir, MH_SESSION_DIRNAME_CAP, "2026-09-17T16-43-00Z_menu_host");
    mh_sd_copy(r->mode, MH_SESSION_TEXT_CAP, role);
}

// ---- section 6's recorder: the close's actions, as a trace -------------------------------------
// seams/session_close_plan.h takes its actions as function pointers; these append one letter each,
// so a check can assert the ORDER as a string: R=resets, C=record(reason), D=end_dir, T=tail,
// Q=rollups_only, S=detector_stop. The fake world is two flags the test sets.
char g_trace[64];
char g_last_reason[32];
bool g_fake_active = false, g_fake_net = false;

void tr(char c) {
    size_t n = strlen(g_trace);
    if (n + 1 < sizeof(g_trace)) {
        g_trace[n]     = c;
        g_trace[n + 1] = '\0';
    }
}
bool fk_active() { return g_fake_active; }
bool fk_net() { return g_fake_net; }
void fk_resets() { tr('R'); }
void fk_record(const char *r) {
    tr('C');
    snprintf(g_last_reason, sizeof(g_last_reason), "%s", r ? r : "");
}
void fk_end_dir() {
    tr('D');
    g_fake_active = false; // the directory switch IS the session ending
}
void                         fk_tail(const char *) { tr('T'); }
void                         fk_rollups() { tr('Q'); }
void                         fk_stop() { tr('S'); }
const mh::session_close::ops FK = {fk_active, fk_net, fk_resets, fk_record,
                                   fk_end_dir, fk_tail, fk_rollups, fk_stop};
void                         trace_reset() { g_trace[0] = '\0'; }

} // namespace

namespace {

// ---- section 7 (dist RL15): session.json players / ai_count / outcome, and presence.json -----------

std::string read_all(const char *path) {
    std::string out;
    FILE       *f = fopen(path, "rb");
    if (!f) return out;
    char   buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return out;
}

std::string temp_dir(const char *leaf) {
    char t[MAX_PATH];
    GetTempPathA(MAX_PATH, t);
    std::string d = std::string(t) + leaf + "_" + std::to_string(GetCurrentProcessId());
    CreateDirectoryA(d.c_str(), nullptr);
    return d;
}

void section_rl15() {
    // outcome words
    check("outcome: gameover -> finished", strcmp(mh_session_outcome("gameover", false), "finished") == 0);
    check("outcome: quit/leave/timeout/host_left -> quit",
          strcmp(mh_session_outcome("quit", false), "quit") == 0 && strcmp(mh_session_outcome("leave", false), "quit") == 0 &&
              strcmp(mh_session_outcome("timeout", false), "quit") == 0 &&
              strcmp(mh_session_outcome("host_left", false), "quit") == 0);
    check("outcome: a desync wins over gameover and quit",
          strcmp(mh_session_outcome("gameover", true), "desync") == 0 && strcmp(mh_session_outcome("quit", true), "desync") == 0);
    check("outcome: no reason -> quit", strcmp(mh_session_outcome(nullptr, false), "quit") == 0);

    // session.json: new fields at the open, and rewritten at the close; old fields kept
    MH_SessionRecord r;
    fill(&r, ID_A, 1, "host");
    r.player_count = 2;
    mh_sd_copy(r.players[0], MH_SESSION_PLAYER_NAME_CAP, "Alice");
    mh_sd_copy(r.players[1], MH_SESSION_PLAYER_NAME_CAP, "B\"ob");
    r.ai_count = 1;
    mh_sd_copy(r.outcome, MH_SESSION_TEXT_CAP, "running");
    char js[4096];
    mh_session_json(&r, js, sizeof(js));
    check("session.json: players in slot order, escaped", has(js, "\"players\": [\"Alice\", \"B\\\"ob\"]"));
    check("session.json: ai_count", has(js, "\"ai_count\": 1"));
    check("session.json: outcome running at the open", has(js, "\"outcome\": \"running\""));
    check("session.json: existing fields kept", has(js, "\"match_id\": \"") && has(js, "\"process_dir\": \"") && has(js, "\"roster\""));
    mh_sd_copy(r.outcome, MH_SESSION_TEXT_CAP, mh_session_outcome("gameover", false));
    mh_session_json(&r, js, sizeof(js));
    check("session.json: outcome finished after the close", has(js, "\"outcome\": \"finished\""));
    r.player_count = 0;
    mh_session_json(&r, js, sizeof(js));
    check("session.json: no players -> empty array", has(js, "\"players\": []"));

    // presence.json formatting
    MH_Presence p;
    mh_presence_clear(&p);
    char pj[1024];
    mh_presence_json(&p, 1234, 1760000100, pj, sizeof(pj));
    check("presence: menu with nulls", has(pj, "\"schema\":1,\"pid\":1234,\"state\":\"menu\",\"mode\":null,\"map\":null") &&
                                           has(pj, "\"players\":null") && has(pj, "\"started_unix\":null") &&
                                           has(pj, "\"updated_unix\":1760000100"));
    MH_Presence q = p;
    mh_sd_copy(q.state, sizeof(q.state), "lobby");
    mh_sd_copy(q.mode, sizeof(q.mode), "network");
    mh_sd_copy(q.map, sizeof(q.map), "Dunes");
    q.players     = 2;
    q.max_players = 4;
    mh_presence_json(&q, 1234, 1760000100, pj, sizeof(pj));
    check("presence: lobby n/max", has(pj, "\"state\":\"lobby\"") && has(pj, "\"map\":\"Dunes\"") && has(pj, "\"players\":2,\"max_players\":4"));
    check("presence: same / different", mh_presence_same(p, p) && !mh_presence_same(p, q));

    // the writer: transitions rewrite; an identical state does not; the planet switch rewrites
    std::string dir  = temp_dir("mh_pres");
    std::string path = dir + "\\presence.json";
    DeleteFileA(path.c_str());
    check("presence writer: menu queued", mh_presence_publish(p, path.c_str()));
    check("presence writer: flush", mh_presence_flush(5000));
    std::string t1 = read_all(path.c_str());
    char        pidtxt[40];
    wsprintfA(pidtxt, "\"pid\":%lu", GetCurrentProcessId());
    check("presence writer: file holds the menu state and OUR pid", has(t1.c_str(), "\"state\":\"menu\"") && has(t1.c_str(), pidtxt));
    check("presence writer: identical state is not rewritten", !mh_presence_publish(p, path.c_str()));
    MH_Presence c = p;
    mh_sd_copy(c.state, sizeof(c.state), "campaign");
    mh_sd_copy(c.mode, sizeof(c.mode), "campaign");
    mh_sd_copy(c.system, sizeof(c.system), "Sol");
    mh_sd_copy(c.planet, sizeof(c.planet), "Mars");
    c.started_unix = 1760000000;
    check("presence writer: campaign queued", mh_presence_publish(c, path.c_str()));
    mh_presence_flush(5000);
    std::string t2 = read_all(path.c_str());
    check("presence writer: campaign file names system and planet", has(t2.c_str(), "\"planet\":\"Mars\"") && has(t2.c_str(), "\"system\":\"Sol\""));
    mh_sd_copy(c.planet, sizeof(c.planet), "Venus");
    check("presence writer: a planet switch is a change", mh_presence_publish(c, path.c_str()));
    mh_presence_flush(5000);
    std::string t3 = read_all(path.c_str());
    check("presence writer: file rewritten with the new planet", has(t3.c_str(), "\"planet\":\"Venus\"") && !has(t3.c_str(), "Mars"));
    std::string tmp = path + ".tmp";
    check("presence writer: no temp file left behind", GetFileAttributesA(tmp.c_str()) == INVALID_FILE_ATTRIBUTES);
    DeleteFileA(path.c_str());
    RemoveDirectoryA(dir.c_str());
}

// ---- section 8 (dist RL5): log retention --------------------------------------------------------

void mk_folder(const std::string &root, const char *name, unsigned long long created) {
    std::string d = root + "\\" + name;
    CreateDirectoryA(d.c_str(), nullptr);
    std::string f  = d + "\\mh_net.log";
    FILE       *fp = fopen(f.c_str(), "wb");
    if (fp) {
        fputs("x\n", fp);
        fclose(fp);
    }
    HANDLE h = CreateFileA(d.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        FILETIME ft;
        ft.dwLowDateTime  = (DWORD)(created & 0xffffffffu);
        ft.dwHighDateTime = (DWORD)(created >> 32);
        SetFileTime(h, &ft, nullptr, nullptr);
        CloseHandle(h);
    }
}

bool exists_dir(const std::string &root, const char *name) {
    return GetFileAttributesA((root + "\\" + name).c_str()) != INVALID_FILE_ATTRIBUTES;
}

void write_marker(const std::string &root, const char *leaf, const char *match_hex) {
    FILE *f = fopen((root + "\\" + leaf).c_str(), "wb");
    if (!f) return;
    fprintf(f, "mh_crash=1\ncode=0xc0000005\npid=4242\nmatch_id=%s\n", match_hex);
    fclose(f);
}

void seed_30(const std::string &root, unsigned long long now, char names[30][96]) {
    const unsigned long long DAY = 864000000000ull;
    for (int i = 0; i < 30; ++i) {
        wsprintfA(names[i], "2026-09-%02dT10-00-00Z_%08x_map_host", i + 1, 0x1000 + i);
        mk_folder(root, names[i], now - (unsigned long long)(30 - i) * DAY); // i=29 is the newest (1 day old)
    }
}

unsigned long long now_ft() {
    FILETIME f;
    GetSystemTimeAsFileTime(&f);
    return ((unsigned long long)f.dwHighDateTime << 32) | f.dwLowDateTime;
}

int count_present(const std::string &root, char names[30][96]) {
    int n = 0;
    for (int i = 0; i < 30; ++i)
        if (exists_dir(root, names[i])) ++n;
    return n;
}

void section_rl5() {
    // the pure decision
    {
        MH_PruneEntry e[5] = {};
        for (int i = 0; i < 5; ++i) {
            wsprintfA(e[i].name, "2026-09-0%dT00-00-00Z_x_y_z", i + 1);
            e[i].created = 1000ull + (unsigned long long)i;
        }
        const int del = mh_prune_select(e, 5, 2, 0, 5000);
        check("prune select: keep 2 of 5 -> the 3 oldest go", del == 3 && e[0].del && e[1].del && e[2].del && !e[3].del && !e[4].del);
        e[0].protect = true;
        mh_prune_select(e, 5, 2, 0, 5000);
        check("prune select: a protected folder is kept and not counted against N", !e[0].del && e[3].del == false && e[1].del && e[2].del);
        check("prune select: both rules off prunes nothing", mh_prune_select(e, 5, 0, 0, 5000) == 0);
        check("prune names: ours vs foreign",
              mh_prune_is_ours("2026-10-08T10-00-00Z_menu_solo") && mh_prune_is_ours("20260921_101010_menu_host") &&
                  !mh_prune_is_ours("crash") && !mh_prune_is_ours("reports") && !mh_prune_is_ours("2026-nounderscore"));
    }
    // on disk: 30 seeded, default 20, crash-marked unreported kept
    {
        std::string              root = temp_dir("mh_prune");
        char                     names[30][96];
        const unsigned long long now = now_ft();
        seed_30(root, now, names);
        mk_folder(root, "2026-10-01T09-00-00Z_menu_host", now - 40ull * 864000000000ull); // an old process folder
        mk_folder(root, "2026-10-08T09-00-00Z_menu_host", now);                           // "this run"
        mk_folder(root, "reports", now - 99ull * 864000000000ull);                        // foreign: untouched
        // names[0] and names[1] are the two OLDEST. 0: crash marker, unreported. 1: reported.
        write_marker(root, "mh_crash_111.marker", "00000000000000000000000000001000");
        write_marker(root, "mh_crash_222.marker", "00000000000000000000000000001001");
        FILE *rep = fopen((root + "\\mh_crash_222.marker.reported").c_str(), "wb");
        if (rep) fclose(rep);
        int scanned = 0;
        int removed = mh_prune_run(root.c_str(), "2026-10-08T09-00-00Z_menu_host", 20, 0, &scanned);
        check("prune: 30 seeded + default 20 -> 20 plus the unreported crash folder remain", count_present(root, names) == 21);
        check("prune: the unreported crash-marked folder survives", exists_dir(root, names[0]));
        check("prune: a REPORTED crash folder is prunable", !exists_dir(root, names[1]));
        check("prune: the 20 newest all survive", [&] {
            for (int i = 10; i < 30; ++i)
                if (!exists_dir(root, names[i])) return false;
            return true;
        }());
        check("prune: this run's process folder and foreign folders are untouched",
              exists_dir(root, "2026-10-08T09-00-00Z_menu_host") && exists_dir(root, "reports"));
        check("prune: removed count is right (9 sessions; process folders are ranked apart from sessions)",
              removed == 9 && scanned == 32);
        check("prune: process folders are ranked separately and survive N=20", exists_dir(root, "2026-10-01T09-00-00Z_menu_host"));
        // a reported crash becomes prunable
        rep = fopen((root + "\\mh_crash_111.marker.reported").c_str(), "wb");
        if (rep) fclose(rep);
        mh_prune_run(root.c_str(), "2026-10-08T09-00-00Z_menu_host", 20, 0, nullptr);
        check("prune: once reported, the formerly protected folder is pruned", !exists_dir(root, names[0]) && count_present(root, names) == 20);
        // a changed N is honoured
        mh_prune_run(root.c_str(), "2026-10-08T09-00-00Z_menu_host", 5, 0, nullptr);
        check("prune: keep_sessions=5 is honoured", count_present(root, names) == 5);
        mh_prune_run(root.c_str(), "2026-10-08T09-00-00Z_menu_host", 0, 0, nullptr);
        check("prune: both rules off keeps everything", count_present(root, names) == 5);
        // remove the temp tree
        for (int i = 0; i < 30; ++i) mh_prune_detail::remove_tree((root + "\\" + names[i]).c_str(), 0);
        mh_prune_detail::remove_tree((root + "\\2026-10-08T09-00-00Z_menu_host").c_str(), 0);
        mh_prune_detail::remove_tree((root + "\\2026-10-01T09-00-00Z_menu_host").c_str(), 0);
        mh_prune_detail::remove_tree((root + "\\reports").c_str(), 0);
        mh_prune_detail::remove_tree(root.c_str(), 0);
    }
    // keep_days
    {
        std::string root = temp_dir("mh_prune_days");
        char        names[30][96];
        seed_30(root, now_ft(), names);
        mh_prune_run(root.c_str(), "", 0, 6, nullptr); // ages 1..30 days (+ms); keep <= 6 days old
        check("prune: keep_days=6 keeps exactly the 5 youngest (ages 1-5 d)", count_present(root, names) == 5 && exists_dir(root, names[25]) && !exists_dir(root, names[24]));
        for (int i = 0; i < 30; ++i) mh_prune_detail::remove_tree((root + "\\" + names[i]).c_str(), 0);
        mh_prune_detail::remove_tree(root.c_str(), 0);
    }
}

} // namespace

int run_sessiondirtest() {
    printf("=== sessiondirtest (SES1: the per-session run directory -- name, rollover, record) ===\n");

    // ---- 1. the directory NAME (SES8: "<dirstamp>_<mid8>_<map>_<mode>") ---------------------------
    {
        const char *ST = "2026-09-17T16-43-46Z";
        char        n[MH_SESSION_DIRNAME_CAP];

        // The stamp: ISO 8601 with dashes for the colons (illegal in a Windows path), UTC, `Z`-marked.
        mh_session_dir_stamp(n, sizeof(n), 2026, 9, 17, 16, 43, 46);
        check("the directory stamp is YYYY-MM-DDTHH-MM-SSZ", strcmp(n, ST) == 0);
        mh_session_dir_stamp(n, sizeof(n), 2027, 1, 2, 3, 4, 5);
        check("...zero-padded, fixed width", strcmp(n, "2027-01-02T03-04-05Z") == 0 && strlen(n) == 20);
        check("the stamp fits its cap", MH_SESSION_DIRSTAMP_CAP == 21);

        mh_session_dir_name(n, sizeof(n), ST, ID_A, "blue monday.mpm", "client");
        check("a session name is <stamp>_<mid8>_<map>_<mode>",
              strcmp(n, "2026-09-17T16-43-46Z_dedd707c_blue-monday_client") == 0);

        mh_session_dir_name(n, sizeof(n), ST, ID_A, "TUTORIAL.MP", "tutorial");
        check("the map keeps its case and loses its extension",
              strcmp(n, "2026-09-17T16-43-46Z_dedd707c_TUTORIAL_tutorial") == 0);

        // THE SHORT FORM MUST DISCRIMINATE, and taking it from the FRONT of a UUIDv7 does not: the
        // leading 48 bits are a unix-ms timestamp, so three lobbies created inside the same ~65 s
        // share their first 8 hex digits exactly. SES1's own three-match rig run produced three
        // directories all reading `01a0b016`. These three ids are the ones that run minted.
        {
            const char *R1 = "01a0b0167ce5724c88e32eabad75d850";
            const char *R2 = "01a0b016c12070ad81df9e21a718d8d3";
            const char *R3 = "01a0b016da757d7483f40714b1fb3a50";
            char        s1[MH_SESSION_DIRNAME_CAP], s2[MH_SESSION_DIRNAME_CAP], s3[MH_SESSION_DIRNAME_CAP];
            mh_session_dir_name(s1, sizeof(s1), "2026-09-17T15-57-38Z", R1, "m.mpm", "host");
            mh_session_dir_name(s2, sizeof(s2), "2026-09-17T15-57-56Z", R2, "m.mpm", "host");
            mh_session_dir_name(s3, sizeof(s3), "2026-09-17T15-58-02Z", R3, "m.mpm", "host");
            check("three matches minted within a minute get three DIFFERENT short forms",
                  strcmp(s1 + 21, s2 + 21) != 0 && strcmp(s2 + 21, s3 + 21) != 0 &&
                      strcmp(s1 + 21, s3 + 21) != 0);
            check("...and the short form is the id's RANDOM tail, not its timestamp prefix",
                  strcmp(s1, "2026-09-17T15-57-38Z_ad75d850_m_host") == 0);
        }

        // THE GLOB CONTRACT. tools/mp_run.py's newest_run() looks for `*_host` / `*_client` (the
        // force-entry verbs' boot role, which is also the lobby mode) and every tool finds the
        // process directory by `_menu_`. The mode is still the LAST field.
        mh_session_dir_name(n, sizeof(n), ST, ID_A, "blue monday.mpm", "host");
        int ln = (int)strlen(n);
        check("a session directory ENDS in _<mode> (the rig's glob)", ln > 5 && strcmp(n + ln - 5, "_host") == 0);

        // FOUR FIELDS, ALWAYS: a map name can hold anything, and none of it may reach the path as a
        // separator, a path character or a second `_`.
        {
            int us = 0;
            mh_session_dir_name(n, sizeof(n), ST, ID_A, "Dane\\my_map: v2 (final)!.mpm", "skirmish");
            for (const char *p = n; *p; ++p) us += (*p == '_');
            check("an awkward map name is sanitised to [A-Za-z0-9.-]",
                  strcmp(n, "2026-09-17T16-43-46Z_dedd707c_v2-final_skirmish") == 0);
            mh_session_dir_name(n, sizeof(n), ST, ID_A, "under_score map.mpm", "host");
            us = 0;
            for (const char *p = n; *p; ++p) us += (*p == '_');
            check("an underscore in the map never adds a field", us == 3 &&
                                                                     strcmp(n, "2026-09-17T16-43-46Z_dedd707c_under-score-map_host") == 0);
            mh_session_dir_name(n, sizeof(n), ST, ID_A, "", "campaign");
            check("no map -> 'nomap'", strcmp(n, "2026-09-17T16-43-46Z_dedd707c_nomap_campaign") == 0);
            mh_session_dir_name(n, sizeof(n), ST, ID_A, nullptr, "campaign");
            check("a null map -> 'nomap'", strcmp(n, "2026-09-17T16-43-46Z_dedd707c_nomap_campaign") == 0);
            mh_session_dir_name(n, sizeof(n), ST, ID_A, "!!!.mpm", "host");
            check("a map with nothing usable -> 'nomap'", strcmp(n, "2026-09-17T16-43-46Z_dedd707c_nomap_host") == 0);
            mh_session_dir_name(n, sizeof(n), ST, ID_A, "an extremely long map name that goes on.mpm", "host");
            const char *tok = n + 30; // past "<stamp>_<mid8>_"
            const char *us2 = strchr(tok, '_');
            check("the map token is capped at MH_SESSION_MAP_TOKEN_MAX",
                  us2 != nullptr && (int)(us2 - tok) <= MH_SESSION_MAP_TOKEN_MAX && us2[-1] != '-');
            mh_session_dir_name(n, sizeof(n), ST, ID_A, "x.mpm", "Host 2!");
            check("the mode is lowercase letters only", strcmp(n, "2026-09-17T16-43-46Z_dedd707c_x_host") == 0);
        }

        mh_session_dir_name(n, sizeof(n), ST, nullptr, nullptr, "solo");
        check("no match_id -> the MENU directory", strcmp(n, "2026-09-17T16-43-46Z_menu_solo") == 0);
        mh_session_dir_name(n, sizeof(n), ST, NIL, "ignored.mpm", "host");
        check("an all-zero match_id is 'no id' too, map ignored", strcmp(n, "2026-09-17T16-43-46Z_menu_host") == 0);
        mh_session_dir_name(n, sizeof(n), ST, "", nullptr, "host");
        check("an empty match_id is 'no id' too", strcmp(n, "2026-09-17T16-43-46Z_menu_host") == 0);

        mh_session_dir_name(n, sizeof(n), ST, ID_A, "m.mpm", "");
        check("an unknown mode falls back to solo rather than a trailing underscore",
              strcmp(n, "2026-09-17T16-43-46Z_dedd707c_m_solo") == 0);

        // Two ids that share a prefix would share a SHORT form; two that do not, must not.
        char a[MH_SESSION_DIRNAME_CAP], b[MH_SESSION_DIRNAME_CAP];
        mh_session_dir_name(a, sizeof(a), ST, ID_A, "m.mpm", "host");
        mh_session_dir_name(b, sizeof(b), ST, ID_B, "m.mpm", "host");
        check("different match_ids give different directory names", strcmp(a, b) != 0);

        // The worst case fits the cap: longest map token + the longest mode word in use.
        mh_session_dir_name(n, sizeof(n), ST, ID_A, "an extremely long map name that goes on.mpm", "campaign");
        check("the longest real name fits MH_SESSION_DIRNAME_CAP", (int)strlen(n) < MH_SESSION_DIRNAME_CAP - 8);

        // A caller-side truncation must not walk off the buffer.
        char tiny[12];
        mh_session_dir_name(tiny, sizeof(tiny), ST, ID_A, "blue monday.mpm", "client");
        check("a short buffer truncates and stays NUL-terminated", strlen(tiny) < sizeof(tiny));
        char tiny2[34];
        mh_session_dir_name(tiny2, sizeof(tiny2), ST, ID_A, "blue monday.mpm", "client");
        check("...including inside the map token", strlen(tiny2) < sizeof(tiny2));
    }

    // ---- 1b. SES8: which single-player match is starting (the solo tick's pure half) --------------
    {
        auto is = [](const char *got, const char *want) {
            return (got == nullptr && want == nullptr) || (got && want && strcmp(got, want) == 0);
        };
        check("strategic + SESSION_MODE 1 -> campaign", is(mh_session_solo_mode(2, 1, 0, false, false), "campaign"));
        check("strategic + SESSION_MODE 2 + tutorial step -> tutorial",
              is(mh_session_solo_mode(2, 2, 1, false, false), "tutorial"));
        check("strategic + SESSION_MODE 2, no tutorial -> skirmish",
              is(mh_session_solo_mode(2, 2, 0, false, false), "skirmish"));
        check("the tactical frame -> tactical", is(mh_session_solo_mode(6, 1, 0, false, false), "tactical"));
        check("lockstep MP never opens a solo session", is(mh_session_solo_mode(2, 3, 0, false, true), nullptr));
        check("the menu (clock still) is not a match start", is(mh_session_solo_mode(3, 1, 0, false, false), nullptr));
        // The tutorial_enter shape, measured: GAME_MODE stays 3 under the welcome/step dialogs for the
        // whole scenario while the game clock runs -- the running clock is what names the match.
        check("an overlay over a RUNNING sim is a match start",
              is(mh_session_solo_mode(3, 2, 1, false, true), "tutorial"));
        check("the intro is not a match start, clock or not", is(mh_session_solo_mode(7, 2, 0, false, true), nullptr));
        check("boot is not a match start, clock or not", is(mh_session_solo_mode(1, 2, 0, false, true), nullptr));
        check("a --tactical run waits for the mission, not the save's strategic frame",
              is(mh_session_solo_mode(2, 1, 0, true, true), nullptr) &&
                  is(mh_session_solo_mode(6, 1, 0, true, false), "tactical"));
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

        // mp:SES4: session.json/SESSION_BEGIN must name the module that actually BOUND, not
        // whatever an ini said -- the 2026-09-20 shape ("every session says tcp while the wire ran
        // udp") was net_discovery.cpp's session_transport() hardcoding "tcp" regardless of what
        // module_bind.cpp loaded. This record/format layer cannot exercise module_bind.cpp itself
        // (that needs a real LoadLibrary, i.e. the rig), but it is the layer SES4's own acceptance
        // clauses are about, and it must render each of the three real values correctly: a udp
        // run's field, a run with no transport bound at all, and a run whose configured value
        // disagreed with what actually loaded.
        {
            MH_SessionRecord ru;
            fill(&ru, ID_A, 1, "client");
            mh_sd_copy(ru.transport, MH_SESSION_TEXT_CAP, "udp"); // the shipping default since 2026-09-20
            char lu[MH_SESSION_LINE_CAP];
            mh_session_begin_line(&ru, lu, sizeof(lu));
            check("mp:SES4 a udp run's SESSION_BEGIN says transport=udp", has(lu, "transport=udp"));
            char ju[2048];
            mh_session_json(&ru, ju, sizeof(ju));
            check("mp:SES4 a udp run's session.json says \"transport\": \"udp\"",
                  has(ju, "\"transport\": \"udp\""));

            MH_SessionRecord rn;
            fill(&rn, ID_A, 1, "client");
            mh_sd_copy(rn.transport, MH_SESSION_TEXT_CAP, "none"); // the transport file never bound
            char ln[MH_SESSION_LINE_CAP];
            mh_session_begin_line(&rn, ln, sizeof(ln));
            check("mp:SES4 a run whose transport never bound says transport=none",
                  has(ln, "transport=none"));
            char jn[2048];
            mh_session_json(&rn, jn, sizeof(jn));
            check("mp:SES4 ...and session.json agrees", has(jn, "\"transport\": \"none\""));

            // The disagreement case: configured tcp, nothing actually bound (its module failed to
            // load). The second field must carry the mismatch; the ordinary (matching) fills above
            // must NOT -- confirming the field is a differ signal, not a second copy of the first.
            MH_SessionRecord rd;
            fill(&rd, ID_A, 1, "client");
            mh_sd_copy(rd.transport, MH_SESSION_TEXT_CAP, "none");
            mh_sd_copy(rd.transport_configured, MH_SESSION_TEXT_CAP, "tcp");
            char ld[MH_SESSION_LINE_CAP];
            mh_session_begin_line(&rd, ld, sizeof(ld));
            check("mp:SES4 configured!=bound shows transport_configured=tcp beside transport=none",
                  has(ld, "transport=none") && has(ld, "transport_configured=tcp"));
            char jd[2048];
            mh_session_json(&rd, jd, sizeof(jd));
            check("mp:SES4 ...and session.json carries both",
                  has(jd, "\"transport\": \"none\"") && has(jd, "\"transport_configured\": \"tcp\""));
            check("mp:SES4 an ordinary (matching) fill's transport_configured stays empty",
                  has(line, "transport_configured= began="));
        }

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
              has(js, "\"process_dir\": \"2026-09-17T16-43-00Z_menu_host\""));
        check("session.json carries the mode (SES8: the directory's last field)",
              has(js, "\"mode\": \"client\""));
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

    // ---- 6. TL-HARN-CLEANCLOSE: the close's sequencing, and the harness stop's second entry --------
    // The rig question -- "does a determinism run end its match through mp_session_close?" -- is two
    // offline questions first: does the ordinary close still do exactly what it did (resets, record,
    // directory, U40, once), and does the harness stop write the record WITHOUT the directory switch,
    // without a second record when a real seam closes afterwards, and only rollups when no session
    // was ever opened? net_discovery.cpp binds these same functions to the live actions.
    {
        using namespace mh::session_close;
        // (a) the ordinary close, unchanged.
        {
            state s{};
            g_fake_active = true;
            on_open(s);
            trace_reset();
            check("close: an open session runs resets, record, directory, U40 -- in that order",
                  close(s, FK, "gameover") && strcmp(g_trace, "RCDT") == 0);
            check("close: ...with the seam's own reason", strcmp(g_last_reason, "gameover") == 0);
            trace_reset();
            check("close: a second close does nothing", !close(s, FK, "quit") && g_trace[0] == '\0');
        }
        // (b) the harness stop on an open session: record only, the directory stays.
        {
            state s{};
            g_fake_active = true;
            on_open(s);
            trace_reset();
            check("harness stop: an open session is closed IN PLACE",
                  harness_stop(s, FK, "harness_stop") == stop_result::closed_in_place);
            check("harness stop: ...record, then the detector stops -- no resets, no directory "
                  "switch, no U40",
                  strcmp(g_trace, "CS") == 0);
            check("harness stop: ...under reason=harness_stop",
                  strcmp(g_last_reason, "harness_stop") == 0);
            check("harness stop: ...and the session is still open (the streams keep their folder)",
                  g_fake_active);
            trace_reset();
            check("harness stop: a second stop is ignored and writes nothing",
                  harness_stop(s, FK, "harness_stop") == stop_result::repeated && g_trace[0] == '\0');
            // A long run that reaches a real exit seam afterwards: that close switches the directory
            // and runs U40, but must NOT write a second record for the same match.
            trace_reset();
            check("a real close after the stop: resets, directory, U40 -- NO second record",
                  close(s, FK, "gameover") && strcmp(g_trace, "RDT") == 0);
            // A NEW session is a new match and gets its own record.
            g_fake_active = true;
            on_open(s);
            trace_reset();
            check("the NEXT session closes with its own record again",
                  close(s, FK, "quit") && strcmp(g_trace, "RCDT") == 0);
        }
        // (c) force-entry: a transport, no session ever -- the two rollups alone.
        {
            state s{};
            g_fake_active = false;
            g_fake_net    = true;
            trace_reset();
            check("harness stop: no session ever opened, transport up -> rollups, detector stop",
                  harness_stop(s, FK, "harness_stop") == stop_result::rollups_only &&
                      strcmp(g_trace, "QS") == 0);
        }
        // (d) the negatives: nothing to close.
        {
            state s{};
            g_fake_active = false;
            g_fake_net    = false;
            trace_reset();
            check("harness stop: no session and no transport (single-player) writes nothing",
                  harness_stop(s, FK, "harness_stop") == stop_result::nothing && g_trace[0] == '\0');
        }
        {
            // The match already closed through a real seam (a 2-peer quit ends the survivor's match):
            // its rollups exist, and a second pair would split one match's counters across two lines.
            state s{};
            g_fake_active = true;
            g_fake_net    = true;
            on_open(s);
            close(s, FK, "gameover");
            trace_reset();
            check("harness stop after the match already closed writes nothing",
                  harness_stop(s, FK, "harness_stop") == stop_result::nothing && g_trace[0] == '\0');
        }
        check("the result names are the HARNESS_STOP line's close= values",
              strcmp(stop_result_name(stop_result::closed_in_place), "in_place") == 0 &&
                  strcmp(stop_result_name(stop_result::rollups_only), "rollups_only") == 0 &&
                  strcmp(stop_result_name(stop_result::nothing), "nothing") == 0 &&
                  strcmp(stop_result_name(stop_result::repeated), "repeated") == 0);
    }

    section_rl15();
    section_rl5();

    printf("  %d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
