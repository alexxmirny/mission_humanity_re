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

    printf("  %d checks, %d failures\n", g_checks, g_fails);
    printf(g_fails ? "=== FAIL ===\n" : "=== PASS ===\n");
    return g_fails ? 1 : 0;
}
