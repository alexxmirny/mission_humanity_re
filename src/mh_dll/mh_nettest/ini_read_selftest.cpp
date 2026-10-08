//
// ini_read_selftest.cpp -- `net_selftest.exe inireadtest`: TL-HARN4, the shared ini STRING-read
// helper (config/ini_read.h) that strips a trailing same-line `;comment` from a value.
//
// WHAT THE BUG WAS, AND WHY THIS SUITE EXISTS RATHER THAN A ONE-OFF TEST. Win32's
// GetPrivateProfileStringA returns everything after the `=` up to end-of-line as the STRING value --
// a `key=value ;comment` line reads back as `value ;comment`, comment and all (unlike an int read,
// where atoi/atof/strtol simply stop at the first non-numeric byte and never see the comment). The
// shipped mh_net.example.ini documents every key with exactly such a trailing comment, so a verbatim
// copy of it was booby-trapped for every STRING key: two sites used to fail CLOSED on it ([net]
// module, [config] mode -- both TERMINATE the process on an unrecognised value) and at least two used
// to fail open silently ([net] relay dialled the comment text, 2026-09-19; [fonts] probe_text drew it
// on every frame, 2026-09-20) before each was hand-patched at its own call site. TL-HARN4 is the
// durable fix -- ONE helper, routed through ~40 call sites across src/mh_dll -- and this suite is
// what proves the helper itself does what every one of those call sites now depends on, with a
// fixture ini shaped exactly like the trap: string keys with trailing comments, live AND in the
// two forms ([config] mode, [net] module) that used to terminate the process outright.
//
// WHAT IT DOES NOT COVER. The ~40 call sites themselves are not re-tested here one by one -- that
// would be re-deriving `docs/symbols.md`'s list by hand and would drift the moment a site is added or
// removed. This suite is about the ONE mechanism every one of them now shares; a call site is correct
// exactly to the extent that it is `mh::config::read_ini_string(...)` with the same arguments
// GetPrivateProfileStringA used to take (a mechanical, greppable property, not a behavioural one this
// suite could assert). The one deliberate NON-caller, net_diag.cpp's `[trace] funcs` (where `;` is
// one of the key's OWN token delimiters, so stripping would truncate a real list), is exercised by
// its own reasoning in that file's comment, not here.
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "config/ini_read.h"

namespace {

int g_checks = 0, g_fails = 0;

void check(const char *what, bool ok) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        printf("  FAIL: %s\n", what);
    }
}

bool write_file(const char *path, const char *bytes) {
    HANDLE h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD      w  = 0;
    const int  n  = (int)strlen(bytes);
    const bool ok = WriteFile(h, bytes, (DWORD)n, &w, nullptr) != 0 && (int)w == n;
    CloseHandle(h);
    return ok;
}

char g_ini[MAX_PATH];

bool make_fixture() {
    char base[MAX_PATH];
    if (GetTempPathA(MAX_PATH, base) == 0) return false;
    wsprintfA(g_ini, "%smh_inireadtest_%lu.ini", base, GetCurrentProcessId());
    // Shaped like the real trap: the two fail-CLOSED sites' own keys, the fail-OPEN relay/probe
    // shape, and a few plain string reads -- live `;comment` lines, exactly the form
    // mh_net.example.ini used to (and, for the still-unfixed `[trace] funcs` exception, still
    // does not) forbid.
    const char *body =
        "[config]\r\n"
        "mode=brokered ; the D11 selector -- brokered | original\r\n"
        "\r\n"
        "[net]\r\n"
        "module=auto;no space before the semicolon either\r\n"
        "host=127.0.0.1   ;   extra whitespace before AND after the semicolon\r\n"
        "relay=vps.example.org:7100 ; UDP only: the directory to browse\r\n"
        "bare=plainvalue\r\n"
        "onlyspace=   \r\n"
        "onlycomment=   ; nothing but a comment\r\n"
        "trailingtabs=value\t;\ttab before and after the semicolon\r\n"
        "semiconly=value;\r\n"
        "[missing]\r\n"
        "unrelated=1\r\n";
    return write_file(g_ini, body);
}

// ======================================================================================================
// RL2 / RL6 -- the ship gate. net_selftest's main() pins the gate OPEN for every suite (the suites'
// fixture inis are full of dev keys); these checks switch it to its real, ini-driven behaviour
// (force mode 0), run against fixture inis with and without `[dev] unlock=1`, and pin it open again.
// ======================================================================================================

char g_lines[64][256];
int  g_nlines = 0;

void capture(const char *s) {
    if (g_nlines < 64) lstrcpynA(g_lines[g_nlines++], s, 256);
}
void clear_lines() { g_nlines = 0; }
int  count_lines(const char *needle) {
    int c = 0;
    for (int i = 0; i < g_nlines; ++i)
        if (strstr(g_lines[i], needle)) ++c;
    return c;
}

void ini_path(char *out, const char *tag) {
    char base[MAX_PATH];
    GetTempPathA(MAX_PATH, base);
    wsprintfA(out, "%smh_inigate_%s_%lu.ini", base, tag, GetCurrentProcessId());
}

// A "player's" ini: dev keys planted, a fixed key moved, a user key set. The unlock variant appends
// `[dev] unlock=1`. Every planted dev key is one the rig actually uses.
const char *const SHIP_BODY =
    "[harness]\r\nenable=1\r\n"
    "[net]\r\nlockstep_step_ms=5\r\ntransport=tcp\r\nhub_migration=0\r\nzz_unregistered_key=7\r\npeers=3\r\n"
    "[input]\r\ncheat_gate=0\r\nmouse_absolute=1\r\n"
    "[pause]\r\nkey=0x19\r\n"
    "[video]\r\nwindow=borderless\r\ntaskbar_guard=0\r\n";

// The seven observer keys a level supplies, plus three detection keys no level may touch. Defaults are
// the CODE defaults the real call sites pass.
struct lv_probe {
    const char *sec;
    const char *key;
    int         code_def;
};
const lv_probe LV_PROBES[] = {
    {"net", "lockstep_log", 1}, {"net", "frametime_log", 1}, {"net", "sp_clock_log", 0}, {"trace", "temporal_sp", 0}, {"desync", "verbose", 0}, {"input", "mouse_trace", 0}, {"desync", "state_record", 0}, // the seven the level table owns
    {"desync", "enabled", 1},
    {"desync", "per_step", 1},
    {"desync", "state_ring", 1}, // detection
};
// Expected effective value per level {quiet, normal, debug}.
const int LV_EXPECT[3][10] = {
    {0, 0, 0, 0, 0, 0, 0, 1, 1, 1}, // quiet: the two per-frame logs off; detection untouched
    {1, 1, 0, 0, 0, 0, 0, 1, 1, 1}, // normal: every code default
    {1, 1, 1, 1, 1, 1, 1, 1, 1, 1}, // debug: the five observers on
};

bool level_set_is(const char *ini, int level_idx) {
    bool ok = true;
    for (int i = 0; i < 10; ++i)
        ok = ok && (int)mh_ini_get_int(LV_PROBES[i].sec, LV_PROBES[i].key, LV_PROBES[i].code_def, ini) == LV_EXPECT[level_idx][i];
    return ok;
}

void run_gate_checks() {
    printf("  -- RL2/RL6 ship gate --\n");
    mh_ini_gate_test_force(0, -1); // the real, ini-driven behaviour
    mh_ini_gate_reset();

    char ship[MAX_PATH], dev[MAX_PATH];
    ini_path(ship, "ship");
    ini_path(dev, "dev");
    char body[2048];
    write_file(ship, SHIP_BODY);
    wsprintfA(body, "%s[dev]\r\nunlock=1\r\n", SHIP_BODY);
    write_file(dev, body);

    // ---- the registry the gate is built from is ini_keys.def ------------------------------------
    check("registry: [net] transport is FIXED", mh_ini_key_class("net", "transport") == 'f');
    check("registry: [harness] enable is DEV", mh_ini_key_class("harness", "enable") == 'd');
    check("registry: [video] window is USER", mh_ini_key_class("video", "window") == 'u');
    check("registry: an unregistered key reads as '?' (treated as dev)", mh_ini_key_class("net", "zz_unregistered_key") == '?');
    check("registry: lookup is case-insensitive", mh_ini_key_class("NET", "Transport") == 'f');
    check("registry: the compile-time table is ini_keys.def (hundreds of rows)", mh_ini_registry_count(-1) > 100);

    // ---- SHIP ini (no unlock): dev keys ignored + logged ONCE, fixed forced, user honoured -------
    clear_lines();
    mh_ini_attach_logger(capture, ship); // a logger, and the start banner
    check("ship: no DEV UNLOCKED banner", count_lines("DEV UNLOCKED") == 0);
    check("ship: the start banner names the effective level set", count_lines("[log] level=normal effective:") == 1);
    check("ship: [dev] unlock reads 0", mh_ini_get_int("dev", "unlock", 0, ship) == 0 && !mh_ini_dev_unlocked(ship));

    char v[128];
    check("ship: planted [harness] enable=1 is IGNORED (reads the code default 0)", mh_ini_get_int("harness", "enable", 0, ship) == 0);
    check("ship: ...and was logged as IGNORED dev key [harness] enable=1", count_lines("IGNORED dev key [harness] enable=1") == 1);
    mh_ini_get_str("net", "lockstep_step_ms", "", v, sizeof(v), ship);
    check("ship: planted [net] lockstep_step_ms=5 is IGNORED (reads the code default \"\")", v[0] == '\0');
    check("ship: ...and logged", count_lines("IGNORED dev key [net] lockstep_step_ms=5") == 1);
    check("ship: a dev INT the code defaults to 1 stays 1 (peers=3 ignored)", mh_ini_get_int("net", "peers", 1, ship) == 1);
    check("ship: an UNREGISTERED key is treated as dev: ignored, caller default returned", mh_ini_get_int("net", "zz_unregistered_key", 4, ship) == 4);
    check("ship: ...and logged", count_lines("IGNORED dev key [net] zz_unregistered_key=7") == 1);
    for (int i = 0; i < 5; ++i) {
        mh_ini_get_int("harness", "enable", 0, ship);
        mh_ini_get_int("net", "peers", 1, ship);
    }
    check("ship: ONE log line per key however often it is read (dedupe)", count_lines("[harness] enable") == 1 && count_lines("[net] peers") == 1);
    check("ship: a dev key the ini does NOT carry logs nothing", (mh_ini_get_int("net", "bootstrap", 1, ship), count_lines("bootstrap") == 0));
    mh_ini_get_str("net", "transport", "tcp", v, sizeof(v), ship);
    check("ship: FIXED [net] transport reads udp whatever the ini says (and the caller's default)", strcmp(v, "udp") == 0);
    check("ship: FIXED [net] hub_migration reads 1 (ini says 0)", mh_ini_get_int("net", "hub_migration", 0, ship) == 1);
    check("ship: FIXED [input] cheat_gate reads 1 (ini says 0)", mh_ini_get_int("input", "cheat_gate", 0, ship) == 1);
    check("ship: FIXED [video] taskbar_guard reads 1 (ini says 0)", mh_ini_get_int("video", "taskbar_guard", 0, ship) == 1);
    check("ship: FIXED [input] mouse_absolute reads 0 (ini says 1)", mh_ini_get_int("input", "mouse_absolute", 1, ship) == 0);
    check("ship: FIXED [pause] key reads 0 = the big-map key is disabled (ini says 0x19, code default 0x0f)", mh_ini_get_int("pause", "key", 0x0f, ship) == 0);
    check("ship: FIXED [ui] key_repeat_fix reads 1 with the key absent", mh_ini_get_int("ui", "key_repeat_fix", 0, ship) == 1);
    check("ship: a fixed key the ini moved logs a FIXED line", count_lines("FIXED [net] transport (ini value tcp ignored") == 1 && count_lines("FIXED [pause] key (ini value 0x19 ignored") == 1);
    check("ship: a fixed key the ini does NOT carry logs nothing", count_lines("key_repeat_fix") == 0);
    mh_ini_get_str("video", "window", "windowed", v, sizeof(v), ship);
    check("ship: a USER key is honoured ([video] window=borderless)", strcmp(v, "borderless") == 0);
    check("ship: ...and never logged", count_lines("[video] window") == 0);

    // ---- UNLOCKED ini: everything honoured, banner logs DEV UNLOCKED -----------------------------
    mh_ini_gate_reset();
    clear_lines();
    mh_ini_attach_logger(capture, dev);
    check("unlock: the start banner logs DEV UNLOCKED", count_lines("DEV UNLOCKED") == 1);
    check("unlock: [dev] unlock reads 1", mh_ini_get_int("dev", "unlock", 0, dev) == 1 && mh_ini_dev_unlocked(dev));
    check("unlock: [harness] enable=1 honoured", mh_ini_get_int("harness", "enable", 0, dev) == 1);
    mh_ini_get_str("net", "lockstep_step_ms", "", v, sizeof(v), dev);
    check("unlock: [net] lockstep_step_ms=5 honoured", strcmp(v, "5") == 0);
    check("unlock: [net] peers=3 honoured", mh_ini_get_int("net", "peers", 1, dev) == 3);
    check("unlock: an unregistered key is read like any dev key", mh_ini_get_int("net", "zz_unregistered_key", 4, dev) == 7);
    mh_ini_get_str("net", "transport", "udp", v, sizeof(v), dev);
    check("unlock: a FIXED key follows the ini (the rig's fix-off negatives): transport=tcp", strcmp(v, "tcp") == 0);
    check("unlock: hub_migration=0, cheat_gate=0, taskbar_guard=0, mouse_absolute=1, [pause] key=0x19 all honoured",
          mh_ini_get_int("net", "hub_migration", 1, dev) == 0 && mh_ini_get_int("input", "cheat_gate", 1, dev) == 0 &&
              mh_ini_get_int("video", "taskbar_guard", 1, dev) == 0 && mh_ini_get_int("input", "mouse_absolute", 0, dev) == 1 &&
              mh_ini_get_int("pause", "key", 0x0f, dev) == 0x19);
    check("unlock: an ABSENT fixed key falls to the caller's default (the rig keeps its pre-RL2 behaviour)", mh_ini_get_int("ui", "key_repeat_fix", 1, dev) == 1);
    check("unlock: nothing is logged IGNORED or FIXED", count_lines("IGNORED") == 0 && count_lines("FIXED") == 0);

    // ---- lines buffered BEFORE a logger exists are flushed on attach ------------------------------
    mh_ini_gate_reset();
    clear_lines();
    mh_ini_get_int("harness", "enable", 0, ship); // DllMain-time read, no logger yet
    check("early read: nothing emitted before a logger is attached", g_nlines == 0);
    mh_ini_attach_logger(capture, ship);
    check("early read: the buffered IGNORED line is flushed on attach, after the banner",
          count_lines("IGNORED dev key [harness] enable=1") == 1 && count_lines("[log] level=normal") == 1);

    // ---- RL6: the three levels --------------------------------------------------------------------
    const char *const LEVEL_TEXT[3] = {"quiet", "normal", "debug"};
    for (int lv = 0; lv < 3; ++lv) {
        char lvini[MAX_PATH];
        ini_path(lvini, LEVEL_TEXT[lv]);
        wsprintfA(body, "[log]\r\nlevel=%s\r\n", LEVEL_TEXT[lv]);
        write_file(lvini, body);
        mh_ini_gate_reset();
        clear_lines();
        mh_ini_attach_logger(capture, lvini);
        char what[160];
        wsprintfA(what, "level=%s: effective key set = %s", LEVEL_TEXT[lv],
                  lv == 0   ? "lockstep_log/frametime_log off, detection on"
                  : lv == 1 ? "every code default"
                            : "the five observers on");
        check(what, level_set_is(lvini, lv));
        wsprintfA(what, "[log] level=%s effective:", LEVEL_TEXT[lv]);
        check("level: the effective set is logged at start (one line)", count_lines(what) == 1);
        if (lv == 0) check("quiet: the logged set names lockstep_log=0 and frametime_log=0", count_lines("net.lockstep_log=0") == 1 && count_lines("net.frametime_log=0") == 1);
        if (lv == 2)
            check("debug: the logged set names the five DEBUG_KEYS", count_lines("net.sp_clock_log=1") == 1 && count_lines("trace.temporal_sp=1") == 1 && count_lines("desync.verbose=1") == 1 &&
                                                                         count_lines("input.mouse_trace=1") == 1 && count_lines("desync.state_record=1") == 1);
        if (lv == 1) check("normal: the logged set says no overrides", count_lines("no overrides") == 1);
        check("level: detection keys are in no level's set", count_lines("per_step") == 0 && count_lines("state_ring") == 0);
        DeleteFileA(lvini);
    }
    {
        char lvini[MAX_PATH];
        ini_path(lvini, "bad");
        write_file(lvini, "[log]\r\nlevel=LOUD ; not a level\r\n");
        mh_ini_gate_reset();
        check("level: an unrecognised value falls back to normal", mh_ini_log_level(lvini) == 1);
        write_file(lvini, "[log]\r\nlevel=DEBUG ; case does not matter\r\n");
        mh_ini_gate_reset();
        check("level: values are case-insensitive and tolerate a trailing comment", mh_ini_log_level(lvini) == 2);
        DeleteFileA(lvini);
    }

    // ---- explicit per-key values: ignored without the unlock, win with it ---------------------------
    {
        char xs[MAX_PATH], xu[MAX_PATH];
        ini_path(xs, "xship");
        ini_path(xu, "xdev");
        const char *const X = "[log]\r\nlevel=debug\r\n[desync]\r\nverbose=0\r\n[net]\r\nlockstep_log=0\r\n";
        write_file(xs, X);
        wsprintfA(body, "%s[dev]\r\nunlock=1\r\n", X);
        write_file(xu, body);
        mh_ini_gate_reset();
        clear_lines();
        mh_ini_attach_logger(capture, xs);
        check("explicit keys, no unlock: level=debug still wins ([desync] verbose=0 ignored -> 1)", mh_ini_get_int("desync", "verbose", 0, xs) == 1);
        check("explicit keys, no unlock: [net] lockstep_log=0 is ignored -> code default 1 (debug leaves it alone)", mh_ini_get_int("net", "lockstep_log", 1, xs) == 1);
        check("explicit keys, no unlock: both are logged as IGNORED", count_lines("IGNORED dev key [desync] verbose=0") == 1 && count_lines("IGNORED dev key [net] lockstep_log=0") == 1);
        mh_ini_gate_reset();
        clear_lines();
        mh_ini_attach_logger(capture, xu);
        check("explicit keys, unlocked: [desync] verbose=0 WINS over level=debug", mh_ini_get_int("desync", "verbose", 0, xu) == 0);
        check("explicit keys, unlocked: [net] lockstep_log=0 honoured", mh_ini_get_int("net", "lockstep_log", 1, xu) == 0);
        check("explicit keys, unlocked: a key the ini does not set still takes the LEVEL's value (sp_clock_log=1)", mh_ini_get_int("net", "sp_clock_log", 0, xu) == 1);
        DeleteFileA(xs);
        DeleteFileA(xu);
    }

    // ---- the gate in front of read_ini_string (the helper almost every string read uses) -----------
    mh_ini_gate_reset();
    mh::config::read_ini_string("net", "transport", "tcp", v, sizeof(v), ship);
    check("read_ini_string: routed through the gate -- FIXED transport reads udp on the ship ini", strcmp(v, "udp") == 0);
    mh::config::read_ini_string("net", "lockstep_step_ms", "dflt ; c", v, sizeof(v), ship);
    check("read_ini_string: an ignored dev key returns the caller's default (comment-stripped as ever)", strcmp(v, "dflt") == 0);

    DeleteFileA(ship);
    DeleteFileA(dev);
    mh_ini_gate_reset();
    mh_ini_gate_test_force(1, 1); // back to the suites' pinned-open default
}

} // namespace

int run_inireadtest() {
    printf("=== inireadtest (TL-HARN4: the shared ini string-read comment-strip helper) ===\n");

    // ---- 1. strip_ini_comment() in isolation: the primitive every call site now shares -----------
    {
        char buf[128];

        strcpy_s(buf, "brokered");
        check("no `;` at all -- untouched, reports no strip", !mh::config::strip_ini_comment(buf) &&
                                                                  strcmp(buf, "brokered") == 0);

        strcpy_s(buf, "brokered ; a trailing comment");
        check("a space-led `;comment` is cut, and the space before it trimmed",
              mh::config::strip_ini_comment(buf) && strcmp(buf, "brokered") == 0);

        strcpy_s(buf, "auto;no space");
        check("a `;` with NO preceding space still cuts cleanly",
              mh::config::strip_ini_comment(buf) && strcmp(buf, "auto") == 0);

        strcpy_s(buf, "value\t;\ttab before and after");
        check("a TAB before the `;` is trimmed too (not just a plain space)",
              mh::config::strip_ini_comment(buf) && strcmp(buf, "value") == 0);

        strcpy_s(buf, "   ; only a comment, no real value");
        check("a value that is ENTIRELY a comment strips to an all-whitespace prefix "
              "(GetPrivateProfileString-style leading whitespace is a separate, pre-existing "
              "concern this helper does not touch)",
              mh::config::strip_ini_comment(buf) && strchr(buf, ';') == nullptr);

        strcpy_s(buf, "value;");
        check("a bare trailing `;` with nothing after it still cuts (empty comment)",
              mh::config::strip_ini_comment(buf) && strcmp(buf, "value") == 0);

        strcpy_s(buf, ";leading semicolon, no value at all");
        check("a `;` as the very FIRST byte strips to an empty string",
              mh::config::strip_ini_comment(buf) && buf[0] == '\0');

        buf[0] = '\0';
        check("an already-empty string is untouched, reports no strip",
              !mh::config::strip_ini_comment(buf) && buf[0] == '\0');
    }

    if (!make_fixture()) {
        printf("  FAIL: could not write the fixture ini\n");
        printf("  %d checks, %d failures\n", g_checks + 1, g_fails + 1);
        return 1;
    }

    // ---- 2. read_ini_string() against the fixture: the drop-in GetPrivateProfileStringA replacement
    {
        char v[128];

        // THE FAIL-CLOSED SHAPE: [config] mode / [net] module. Before TL-HARN4 this exact fixture
        // line made config.h's real resolve() and module_bind.cpp's real module_declined() refuse
        // the run and TerminateProcess -- this is the scenario those two sites now avoid.
        mh::config::read_ini_string("config", "mode", "brokered", v, sizeof(v), g_ini);
        check("[config] mode: the trailing comment is gone, leaving exactly what the strict "
              "compare (lstrcmpiA(v, \"brokered\")) needs",
              strcmp(v, "brokered") == 0);

        mh::config::read_ini_string("net", "module", "auto", v, sizeof(v), g_ini);
        check("[net] module: a comment with NO leading space still leaves a clean value",
              strcmp(v, "auto") == 0);

        // THE FAIL-OPEN SHAPE: [net] relay -- used to be DIALLED comment-and-all (2026-09-19).
        mh::config::read_ini_string("net", "relay", "", v, sizeof(v), g_ini);
        check("[net] relay: the address is clean, not `vps.example.org:7100 ; UDP only...`",
              strcmp(v, "vps.example.org:7100") == 0);

        mh::config::read_ini_string("net", "host", "0.0.0.0", v, sizeof(v), g_ini);
        check("[net] host: whitespace on BOTH sides of the `;` is handled",
              strcmp(v, "127.0.0.1") == 0);

        // A bare value with no comment at all must come back byte-identical -- the helper must not
        // perturb the overwhelming majority of real ini lines, which carry no same-line comment.
        mh::config::read_ini_string("net", "bare", "", v, sizeof(v), g_ini);
        check("a key with no comment at all is untouched", strcmp(v, "plainvalue") == 0);

        // `key=   ` (whitespace, no comment) is the ordinary "present but empty" shape (L1d's
        // explicit-override convention) -- the helper must not treat trailing whitespace alone as
        // something to strip; only a `;` triggers it.
        mh::config::read_ini_string("net", "onlyspace", "sentinel", v, sizeof(v), g_ini);
        check("a whitespace-only value (no `;`) is left alone, not replaced by the default",
              strcmp(v, "sentinel") != 0 && strchr(v, ';') == nullptr);

        // `key=   ; nothing but a comment` is the probe_text/L1d TRAP SHAPE: an explicit-override
        // key whose "empty means unset" contract depends on the comment NOT becoming part of the
        // value. Before its own fix this drew literally on every frame (2026-09-20).
        mh::config::read_ini_string("net", "onlycomment", "sentinel", v, sizeof(v), g_ini);
        check("a value that is ENTIRELY a comment reads back EMPTY, not the comment text",
              v[0] == '\0');

        mh::config::read_ini_string("net", "trailingtabs", "", v, sizeof(v), g_ini);
        check("tabs around the `;` (not just spaces) are trimmed via the full read path too",
              strcmp(v, "value") == 0);

        mh::config::read_ini_string("net", "semiconly", "", v, sizeof(v), g_ini);
        check("a bare trailing `;` with nothing after it (via the full read path)",
              strcmp(v, "value") == 0);

        // An ABSENT key still takes the default -- the helper must not change GetPrivateProfileStringA's
        // own "key not found" behaviour, only what happens to a value it DID find.
        mh::config::read_ini_string("missing", "no_such_key", "the_default", v, sizeof(v), g_ini);
        check("an absent key still falls back to the caller's default, unmodified",
              strcmp(v, "the_default") == 0);
        mh::config::read_ini_string("no_such_section", "mode", "the_default", v, sizeof(v), g_ini);
        check("an absent SECTION falls back to the default the same way", strcmp(v, "the_default") == 0);

        // THE EDGE CASE ini_read.h'S OWN COMMENT CALLS OUT: the helper strips whatever ends up in
        // `out`, including a `;` that came from a DEFAULT rather than the file (GetPrivateProfileStringA
        // copies an absent key's default into `out` verbatim, and this scans that copy same as any
        // other). None of the ~40 real call sites' compiled-in defaults contain a `;`, so this never
        // fires for them -- it is asserted here so the behaviour is a documented, tested property
        // instead of an unstated implementation detail.
        mh::config::read_ini_string("missing", "no_such_key", "def ; looks like a comment", v,
                                    sizeof(v), g_ini);
        check("a default containing `;` is ALSO stripped -- the helper scans `out`, not \"only "
              "real ini bytes\"",
              strcmp(v, "def") == 0);

        // The return value is the length of the STRIPPED string, matching GetPrivateProfileStringA's
        // own contract (the length actually written) -- a caller that checks it must see the
        // post-strip length, not the pre-strip one.
        DWORD n = mh::config::read_ini_string("net", "relay", "", v, sizeof(v), g_ini);
        check("the returned length matches the STRIPPED string, not the raw ini bytes",
              n == (DWORD)lstrlenA(v) && n == (DWORD)lstrlenA("vps.example.org:7100"));
    }

    DeleteFileA(g_ini);

    // ---- 3. RL2 / RL6: the ship gate and the log level (mh_common/include/mh_ini_gate.h) ----------
    run_gate_checks();

    printf("  %d checks, %d failures\n", g_checks, g_fails);
    printf(g_fails ? "=== FAIL ===\n" : "=== PASS ===\n");
    return g_fails ? 1 : 0;
}
