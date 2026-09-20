//
// log_rotate_selftest.cpp -- `net_selftest.exe logrottest`: the log-stream SIZE CAP and its
// one-generation rotation (mp:SES2).
//
// WHAT IT PROVES, and why each half is here rather than on the rig:
//
//   * THE DERIVATION AND THE REFUSALS (arms 1-4) -- ".log" -> ".prev.log", the ".log"-suffix check,
//     the buffer-too-small refusal, "0 means uncapped", the ini read, "a file that does not exist is
//     not over any cap", and the fact that a SECOND rotation replaces the one generation rather than
//     accumulating. None of these is reachable from a rig run: a gameplay session produces one
//     outcome, and the interesting cases are the ones where nothing is supposed to happen.
//
//   * THE 70 MB SYNTHETIC RUN (arm 5) -- SES2's acceptance clause verbatim: "A synthetic run emitting
//     70 MB to mh_net.log leaves a capped file plus one .prev". It is driven through the REAL
//     seam_log(), the real MH_RunDir() and the real default cap, so what it exercises is the shipping
//     path and not a re-statement of the helper. On the rig the same claim costs a two-VM lockstep
//     session long enough to emit 70 MB of DIAG lines -- tens of minutes of wall clock for a fact
//     that is about a file size.
//
// THE ARM THAT WOULD OTHERWISE BE VACUOUS. Asserting only "a .prev appeared" would pass for a cap of
// 1 byte, of 4 KB, or of anything else. Arm 5 asserts the .prev is in [cap, cap + one line] and the
// live file holds the remainder, which pins the DEFAULT VALUE (64 MB) as well as the mechanism -- so
// a change to the default reds this test by name instead of quietly shrinking everyone's evidence.
//
// WHAT IT DOES NOT COVER. mh_net.log is appended to by five writers in three images and only
// mh.dll's seam_log consults the cap. A session whose volume came entirely from mh_net.dll's logf()
// would not rotate until the next seam line. That is a real residue, recorded in
// the instrument-channels doc and in the tracker, not something this suite can assert away: the
// transport's writer belongs to another item's write set.
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "mh_log_rotate.h"
#include "mh_run_context.h"

// The real mh_net.log writer (mh/seams/net_seams.cpp, compiled into this exe). Declared rather than
// included because net_internal.h drags in the whole seam spine; the point of arm 5 is that this is
// the SAME function the game calls, so a local re-implementation would prove nothing.
void seam_log(const char *s);

namespace {

int g_checks = 0, g_fails = 0;

void check(const char *what, bool ok) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        printf("  FAIL: %s\n", what);
    }
}

long long file_size(const char *path) {
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fa)) return -1; // -1 = absent
    return ((long long)fa.nFileSizeHigh << 32) | (long long)fa.nFileSizeLow;
}

bool write_file(const char *path, const char *bytes, int n) {
    HANDLE h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD      w  = 0;
    const bool ok = WriteFile(h, bytes, (DWORD)n, &w, nullptr) != 0 && (int)w == n;
    CloseHandle(h);
    return ok;
}

bool grow_file(const char *path, long long want) {
    HANDLE h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER li;
    li.QuadPart   = want;
    const bool ok = SetFilePointerEx(h, li, nullptr, FILE_BEGIN) != 0 && SetEndOfFile(h) != 0;
    CloseHandle(h);
    return ok;
}

// A scratch directory of our own under %TEMP%, so nothing here touches the run directory the rig
// tools glob. Arm 5 is the deliberate exception -- it uses the real one, because that is the path
// under test.
char g_tmp[MAX_PATH];

bool make_tmp_dir() {
    char base[MAX_PATH];
    if (GetTempPathA(MAX_PATH, base) == 0) return false;
    wsprintfA(g_tmp, "%smh_logrot_%lu\\", base, GetCurrentProcessId());
    CreateDirectoryA(g_tmp, nullptr);
    return GetFileAttributesA(g_tmp) != INVALID_FILE_ATTRIBUTES;
}

void tmp_path(char *dst, const char *leaf) { wsprintfA(dst, "%s%s", g_tmp, leaf); }

} // namespace

int run_logrottest() {
    printf("=== logrottest (SES2: the log size cap + one-generation rotation) ===\n");

    // ---- 1. the ".log" -> ".prev.log" derivation, and what it REFUSES -----------------------------
    {
        char p[MAX_PATH];

        check("a .log path yields its .prev.log sibling",
              mh_log_prev_path(p, MAX_PATH, "C:\\g\\logs\\r\\mh_net.log") &&
                  strcmp(p, "C:\\g\\logs\\r\\mh_net.prev.log") == 0);
        check("the same derivation serves mh_temporal.log (the pre-SES2 caller)",
              mh_log_prev_path(p, MAX_PATH, "C:\\g\\logs\\r\\mh_temporal.log") &&
                  strcmp(p, "C:\\g\\logs\\r\\mh_temporal.prev.log") == 0);
        check("a bare leaf name works too (no directory required)",
              mh_log_prev_path(p, MAX_PATH, "mh_net.log") && strcmp(p, "mh_net.prev.log") == 0);
        check("the suffix test is case-insensitive",
              mh_log_prev_path(p, MAX_PATH, "MH_NET.LOG") && strcmp(p, "MH_NET.prev.log") == 0);

        // THE TRAP THE PRE-SES2 INLINE VERSION CARRIED: it overwrote the last four characters on the
        // strength of `n > 4` alone, so any path at all would have been "rotated" -- including the
        // desync snapshots that sit in the same directory. The suffix check is what makes this a
        // helper rather than a footgun, so it gets an arm.
        p[0] = 'x';
        check("a non-.log path is REFUSED, not mangled",
              !mh_log_prev_path(p, MAX_PATH, "C:\\g\\logs\\r\\mh_desync_snap_0.bin") && p[0] == '\0');
        check("a path that is only an extension is refused", !mh_log_prev_path(p, MAX_PATH, ".log"));
        check("an empty path is refused", !mh_log_prev_path(p, MAX_PATH, ""));
        check("a null path is refused", !mh_log_prev_path(p, MAX_PATH, nullptr));

        // A buffer that cannot hold the longer name must refuse rather than truncate: a truncated
        // rename target is a rename to somewhere else.
        char small[16];
        small[0] = 'x';
        check("a destination too small for the longer name is refused, and left empty",
              !mh_log_prev_path(small, (int)sizeof(small), "C:\\g\\logs\\mh_net.log") && small[0] == '\0');
        check("...but a destination that exactly fits is accepted",
              mh_log_prev_path(small, (int)sizeof(small), "mh_net.log") &&
                  strcmp(small, "mh_net.prev.log") == 0);
    }

    if (!make_tmp_dir()) {
        printf("  FAIL: could not create the scratch directory\n");
        printf("  %d checks, %d failures\n", g_checks + 1, g_fails + 1);
        return 1;
    }

    // ---- 2. the cap question: over / under / absent / uncapped ------------------------------------
    {
        char f[MAX_PATH];
        tmp_path(f, "cap.log");
        DeleteFileA(f);

        check("a file that does not exist is not over any cap", !mh_log_over_cap(f, 1024));

        check("grow the probe file to 1023 bytes", grow_file(f, 1023));
        check("under the cap answers false", !mh_log_over_cap(f, 1024));
        check("AT the cap answers true (>=, not >)", mh_log_over_cap(f, 1023));
        check("over the cap answers true", mh_log_over_cap(f, 1000));

        // ZERO MEANS UNCAPPED, and it has to, because that is the knob's documented "off".
        check("a cap of 0 never rotates", !mh_log_over_cap(f, 0));
        check("a negative cap never rotates", !mh_log_over_cap(f, -1));

        HANDLE h = CreateFileA(f, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        check("the open-handle form agrees with the path form", h != INVALID_HANDLE_VALUE &&
                                                                    mh_log_handle_over_cap(h, 1023) &&
                                                                    !mh_log_handle_over_cap(h, 1024));
        check("the open-handle form also treats 0 as uncapped", !mh_log_handle_over_cap(h, 0));
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        check("an invalid handle is not over any cap",
              !mh_log_handle_over_cap(INVALID_HANDLE_VALUE, 1));
        DeleteFileA(f);
    }

    // ---- 3. the ini read -------------------------------------------------------------------------
    {
        char ini[MAX_PATH];
        tmp_path(ini, "probe.ini");
        const char *body = "[net]\r\nlog_max_mb=7\r\n[trace]\r\ntemporal_max_mb=0\r\n";
        check("write the probe ini", write_file(ini, body, (int)strlen(body)));

        check("an absent ini path falls back to the compiled default",
              mh_log_cap_bytes(nullptr, "net", "log_max_mb", 64) == 64LL * 1024 * 1024);
        check("an empty ini path falls back to the compiled default (the DllMain window)",
              mh_log_cap_bytes("", "net", "log_max_mb", 64) == 64LL * 1024 * 1024);
        check("[net] log_max_mb is read and converted to bytes",
              mh_log_cap_bytes(ini, "net", "log_max_mb", 64) == 7LL * 1024 * 1024);
        check("[trace] temporal_max_mb=0 means UNCAPPED, not 'use the default'",
              mh_log_cap_bytes(ini, "trace", "temporal_max_mb", 64) == 0);
        check("a key that is absent from the ini takes the default",
              mh_log_cap_bytes(ini, "net", "no_such_key", 3) == 3LL * 1024 * 1024);
        check("a negative default is 'off' rather than a negative cap",
              mh_log_cap_bytes(nullptr, "net", "log_max_mb", -1) == 0);
        DeleteFileA(ini);
    }

    // ---- 4. rotation: ONE generation, and a rotate that fails is not a lost file ------------------
    {
        char cur[MAX_PATH], prev[MAX_PATH];
        tmp_path(cur, "rot.log");
        tmp_path(prev, "rot.prev.log");
        DeleteFileA(cur);
        DeleteFileA(prev);

        check("write generation A", write_file(cur, "AAAA", 4));
        check("first rotate reports success", mh_log_rotate(cur));
        check("the current file is gone after a rotation", file_size(cur) < 0);
        check("generation A is now the .prev", file_size(prev) == 4);

        check("write generation B into a fresh current file", write_file(cur, "BBBBBB", 6));
        check("second rotate reports success", mh_log_rotate(cur));
        // ONE GENERATION. The older content is REPLACED, and no .prev.prev ladder appears -- the
        // whole point of the policy is a bound anyone can state (2x the cap).
        check("the second rotation REPLACED the .prev rather than accumulating", file_size(prev) == 6);
        {
            char prev2[MAX_PATH];
            tmp_path(prev2, "rot.prev.prev.log");
            check("no second generation is created", file_size(prev2) < 0);
        }

        // A rotation of something that is not a .log is refused by the derivation, so nothing moves.
        char bin[MAX_PATH];
        tmp_path(bin, "snap.bin");
        check("write a non-log file", write_file(bin, "Z", 1));
        check("rotating a non-.log path is refused", !mh_log_rotate(bin));
        check("...and the file is untouched", file_size(bin) == 1);
        DeleteFileA(bin);

        // Rotating a file that does not exist fails and creates nothing.
        DeleteFileA(cur);
        DeleteFileA(prev);
        check("rotating an absent file fails", !mh_log_rotate(cur));
        check("...and does not conjure a .prev", file_size(prev) < 0);

        // SES1 GENERATION-FOLLOW. The writers resolve their path through mh_run_path() before they
        // reach the cap, so rotation always acts on the CURRENT directory's file. Two directories
        // standing in for two sessions: rotating one must not reach into the other.
        char da[MAX_PATH], db[MAX_PATH], fa[MAX_PATH], fb[MAX_PATH], pa[MAX_PATH];
        tmp_path(da, "sessA\\");
        tmp_path(db, "sessB\\");
        CreateDirectoryA(da, nullptr);
        CreateDirectoryA(db, nullptr);
        wsprintfA(fa, "%smh_net.log", da);
        wsprintfA(fb, "%smh_net.log", db);
        wsprintfA(pa, "%smh_net.prev.log", da);
        check("write a log in each of two session directories",
              write_file(fa, "A", 1) && write_file(fb, "BB", 2));
        check("rotating session A's file succeeds", mh_log_rotate(fa));
        check("session A rotated in place", file_size(pa) == 1 && file_size(fa) < 0);
        check("session B is untouched by session A's rotation", file_size(fb) == 2);
        DeleteFileA(pa);
        DeleteFileA(fb);
        RemoveDirectoryA(da);
        RemoveDirectoryA(db);
    }

    // ---- 5. SES2's acceptance clause: 70 MB through the REAL writer ------------------------------
    //
    // "A synthetic run emitting 70 MB to mh_net.log leaves a capped file plus one .prev."
    //
    // Driven through seam_log() itself, in the real run directory, with the real default cap (g_ini
    // is empty in this exe -- MH_Seam_Init never runs -- which is exactly the compiled-default path
    // the clause names).
    {
        const long long CAP   = 64LL * 1024 * 1024; // [net] log_max_mb default
        const long long TOTAL = 70LL * 1024 * 1024;

        char cur[MAX_PATH], prev[MAX_PATH], prev2[MAX_PATH];
        wsprintfA(cur, "%smh_net.log", MH_RunDir());
        wsprintfA(prev, "%smh_net.prev.log", MH_RunDir());
        wsprintfA(prev2, "%smh_net.prev.prev.log", MH_RunDir());
        DeleteFileA(cur);
        DeleteFileA(prev);
        DeleteFileA(prev2);

        // 8 KB per line: seam_log takes an already-formatted string of unbounded length, and the cost
        // of this arm is the open/append/close cycle per CALL, not the bytes. 8 KB keeps it to ~9k
        // cycles for 70 MB instead of over a million at a realistic line length.
        const int   LINE = 8192;
        static char line[8193];
        for (int i = 0; i < LINE - 1; ++i) line[i] = 'x';
        line[LINE - 1] = '\n';
        line[LINE]     = '\0';

        const DWORD t0    = GetTickCount();
        long long   wrote = 0;
        while (wrote < TOTAL) {
            seam_log(line);
            wrote += LINE; // plus the stamp seam_log prepends; counted below off the real sizes
        }
        const DWORD ms = GetTickCount() - t0;

        const long long cur_sz  = file_size(cur);
        const long long prev_sz = file_size(prev);
        printf("  70 MB synthetic run: %lu ms; mh_net.log=%I64d B, mh_net.prev.log=%I64d B\n", ms,
               cur_sz, prev_sz);

        check("the .prev generation exists", prev_sz > 0);
        check("no SECOND generation was created", file_size(prev2) < 0);
        check("the live mh_net.log is UNDER the cap", cur_sz >= 0 && cur_sz < CAP);
        // The teeth: the rotated generation lands in [cap, cap + one line + one stamp]. This pins the
        // DEFAULT as well as the mechanism -- a cap of 1 byte would satisfy "a .prev appeared".
        check("the rotated generation is exactly one cap's worth (the DEFAULT 64 MB, not merely 'some cap')",
              prev_sz >= CAP && prev_sz <= CAP + LINE + 64);
        // Nothing was dropped: the two files together hold everything that was written (each line
        // also carries seam_log's wall-clock stamp, so the total is slightly over the raw byte count).
        check("every written byte is still on disk across the two generations",
              cur_sz + prev_sz >= wrote);
        check("...and disk is bounded at 2x the cap", cur_sz + prev_sz < 2 * CAP + LINE + 64);

        DeleteFileA(cur);
        DeleteFileA(prev);
    }

    RemoveDirectoryA(g_tmp);

    printf("  %d checks, %d failures\n", g_checks, g_fails);
    printf(g_fails ? "=== FAIL ===\n" : "=== PASS ===\n");
    return g_fails ? 1 : 0;
}
