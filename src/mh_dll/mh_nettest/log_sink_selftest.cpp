//
// log_sink_selftest.cpp -- `net_selftest.exe logsinktest`: the process-wide async log sink
// (mh_common/mh_log_sink.{h,cpp}, mp:LOG1).
//
// WHAT IT PROVES, against the REAL sink and real files (no stand-in):
//
//   1. N threads x M lines into several files: every line present exactly once, and each producer's
//      lines appear in the order it enqueued them, per file. (done_when (b), first clause.)
//   2. A two-part record (stamp + line) lands CONTIGUOUSLY even with other threads interleaving
//      records into the same file -- the property seam_log's stamp/line split relies on.
//   3. The queue-full path: with the writer parked, enqueue more than MH_LOGQ_MAX_BYTES; the overflow
//      is DROPPED and COUNTED, memory stays bounded, the surviving lines keep their order, and the
//      writer then emits the counted-drop line into mh_net.log. (second clause)
//   4. A simulated crash: with the writer parked, a drain issued from the "dying" thread flushes
//      EVERYTHING enqueued before it, exactly once (a later writer wake-up must not duplicate).
//      (third clause)
//   5. THE DEADLOCK ARM: with the writer wedged HOLDING the consumer flag (the shape of a hung
//      WriteFile), the crash drain returns within its bound instead of hanging -- and the lines are
//      then written once the writer is released. This is the reason the drain is a deadline'd
//      take-over rather than a lock.
//   6. Rotation by the writer: the cap rotates a file to .prev; a rotate-now record executes IN ORDER
//      (everything before it in .prev, everything after in the fresh file).
//   6b. BINARY records (mp:LOG2): create/truncate-then-append is byte-exact and order-preserving (a
//      create after appends lands after them), an empty create makes an empty file, records past the
//      text limits (3 MB, 9 MB) are intact, binary records queued while the TEXT bound is overflowing
//      are not dropped (own budget), a ticket is done only once the writer has put everything on disk,
//      and a crash drain flushes a queued create + append.
//   7. Exit: mh_logq_shutdown drains inline; lines logged after it are written synchronously.
//      (Last, because it retires the sink for the process.)
//
// NOT COVERED HERE: the writer's own "slow op > 50 ms" line -- it needs a file operation that really
// is slow, which a unit test cannot summon honestly. It is read off the rig/field logs (done_when (d)).
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

#include "mh_log_sink.h"

void mh_logq_test_hold(int mode); // mh_log_sink.cpp -- park the writer (selftest hook)

namespace {

int g_checks = 0, g_fails = 0;

void check(const char *what, bool ok) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        printf("  FAIL: %s\n", what);
    }
}

char g_dir[MAX_PATH];

void path_of(char *dst, const char *leaf) { wsprintfA(dst, "%s%s", g_dir, leaf); }

std::string slurp(const char *path) {
    std::string out;
    HANDLE      h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return out;
    char  buf[65536];
    DWORD got = 0;
    while (ReadFile(h, buf, sizeof(buf), &got, nullptr) && got) out.append(buf, got);
    CloseHandle(h);
    return out;
}

std::vector<std::string> lines_of(const std::string &s) {
    std::vector<std::string> v;
    size_t                   p = 0;
    while (p < s.size()) {
        size_t e = s.find('\n', p);
        if (e == std::string::npos) e = s.size();
        v.push_back(s.substr(p, e - p));
        p = e + 1;
    }
    return v;
}

long long size_of(const char *path) {
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fa)) return -1;
    return ((long long)fa.nFileSizeHigh << 32) | (long long)fa.nFileSizeLow;
}

constexpr int NTHREADS = 8;
constexpr int NLINES   = 4000;

struct producer_arg {
    int  id;
    char paths[3][MAX_PATH];
};

DWORD WINAPI producer(LPVOID p) {
    producer_arg *a = (producer_arg *)p;
    for (int i = 0; i < NLINES; ++i) {
        char line[64];
        int  n = wsprintfA(line, "T%d L%d\n", a->id, i);
        // Spread the records over the three files by sequence number, so every file sees every thread.
        mh_logq_write(a->paths[i % 3], line, n);
    }
    return 0;
}

struct pair_arg {
    char path[MAX_PATH];
    int  id;
};
DWORD WINAPI pair_producer(LPVOID p) {
    pair_arg *a = (pair_arg *)p;
    for (int i = 0; i < 1500; ++i) {
        char head[32], tail[48];
        int  hn = wsprintfA(head, "[S%d.%d] ", a->id, i);
        int  tn = wsprintfA(tail, "payload-%d-%d\n", a->id, i);
        mh_logq_write2(a->path, head, hn, tail, tn);
    }
    return 0;
}

} // namespace

int run_logsinktest() {
    printf("=== logsinktest (mp:LOG1: the async log sink) ===\n");

    char base[MAX_PATH];
    GetTempPathA(MAX_PATH, base);
    wsprintfA(g_dir, "%smh_logsink_%lu\\", base, GetCurrentProcessId());
    CreateDirectoryA(g_dir, nullptr);

    mh_logq_init();
    check("the sink published its table", mh_logq_local_api() != nullptr);
    if (mh_logq_local_api() == nullptr) {
        printf("  %d checks, %d failures\n=== FAIL ===\n", g_checks, g_fails + 1);
        return 1;
    }
    check("the client finds it through the named mapping", mh_logq_detail::find_api() != nullptr);

    // ---- 1. N threads x M lines, several files ------------------------------------------------------
    {
        producer_arg args[NTHREADS];
        HANDLE       th[NTHREADS];
        for (int t = 0; t < NTHREADS; ++t) {
            args[t].id = t;
            for (int f = 0; f < 3; ++f) {
                char leaf[32];
                wsprintfA(leaf, "multi%d.log", f);
                path_of(args[t].paths[f], leaf);
            }
        }
        for (int f = 0; f < 3; ++f) {
            char p[MAX_PATH], leaf[32];
            wsprintfA(leaf, "multi%d.log", f);
            path_of(p, leaf);
            DeleteFileA(p);
        }
        for (int t = 0; t < NTHREADS; ++t) th[t] = CreateThread(nullptr, 0, producer, &args[t], 0, nullptr);
        WaitForMultipleObjects(NTHREADS, th, TRUE, 60000);
        for (int t = 0; t < NTHREADS; ++t) CloseHandle(th[t]);
        mh_logq_flush(20000);

        long total    = 0;
        bool order_ok = true, dup_ok = true;
        for (int f = 0; f < 3; ++f) {
            char p[MAX_PATH], leaf[32];
            wsprintfA(leaf, "multi%d.log", f);
            path_of(p, leaf);
            std::vector<std::string> ls = lines_of(slurp(p));
            total += (long)ls.size();
            int last[NTHREADS];
            for (int t = 0; t < NTHREADS; ++t) last[t] = -1;
            for (const std::string &l : ls) {
                int t = -1, i = -1;
                if (sscanf(l.c_str(), "T%d L%d", &t, &i) != 2 || t < 0 || t >= NTHREADS) {
                    order_ok = false;
                    continue;
                }
                if (i <= last[t]) order_ok = false; // out of order or repeated
                if (i == last[t]) dup_ok = false;
                last[t] = i;
            }
        }
        check("every line of every thread reached disk exactly once", total == (long)NTHREADS * NLINES);
        check("each producer's lines are in enqueue order within each file", order_ok);
        check("no line was written twice", dup_ok);
        MH_LogQStats st;
        mh_logq_stats(&st);
        check("nothing was dropped on the normal path", st.dropped == 0);
    }

    // ---- 2. a two-part record is contiguous under contention ------------------------------------------
    {
        pair_arg a[4];
        HANDLE   th[4];
        char     p[MAX_PATH];
        path_of(p, "pairs.log");
        DeleteFileA(p);
        for (int t = 0; t < 4; ++t) {
            lstrcpynA(a[t].path, p, MAX_PATH);
            a[t].id = t;
            th[t]   = CreateThread(nullptr, 0, pair_producer, &a[t], 0, nullptr);
        }
        WaitForMultipleObjects(4, th, TRUE, 60000);
        for (int t = 0; t < 4; ++t) CloseHandle(th[t]);
        mh_logq_flush(20000);
        std::vector<std::string> ls = lines_of(slurp(p));
        bool                     ok = ls.size() == 4u * 1500u;
        for (const std::string &l : ls) {
            int s = -1, i = -1, s2 = -1, i2 = -1;
            if (sscanf(l.c_str(), "[S%d.%d] payload-%d-%d", &s, &i, &s2, &i2) != 4 || s != s2 || i != i2) ok = false;
        }
        check("stamp+line records never interleave with another thread's record", ok);
    }

    // ---- 3. queue-full: dropped, counted, bounded, ordered, reported ------------------------------------
    {
        char net[MAX_PATH];
        path_of(net, "mh_net.log"); // the sink reports drops into the last mh_net.log it has seen
        DeleteFileA(net);
        mh_logq_write(net, "; seed line so the sink learns its home log\n", 44);
        mh_logq_flush(5000);
        MH_LogQStats before;
        mh_logq_stats(&before);

        mh_logq_test_hold(1);
        Sleep(50);
        const int   LINE = 1024 * 1024;
        std::string big((size_t)LINE - 16, 'x');
        const int   COUNT = (MH_LOGQ_MAX_BYTES / LINE) + 6; // comfortably past the bound
        for (int i = 0; i < COUNT; ++i) {
            char head[16];
            int  hn = wsprintfA(head, "#%03d ", i);
            mh_logq_write2(net, head, hn, big.c_str(), (int)big.size());
            mh_logq_write(net, "\n", 1);
        }
        MH_LogQStats mid;
        mh_logq_stats(&mid);
        check("memory stayed bounded while the writer was parked", mid.queued_bytes <= MH_LOGQ_MAX_BYTES + 4 * LINE);
        const long dropped = mid.dropped - before.dropped;
        check("the overflow was dropped and counted", dropped >= 4 && dropped < COUNT);
        mh_logq_test_hold(0);
        Sleep(30); // let the parked writer observe the release before the next arm re-parks it
        mh_logq_flush(20000);
        Sleep(100); // the writer enqueues the drop-report line itself, after its first pass
        mh_logq_flush(20000);
        std::vector<std::string> ls   = lines_of(slurp(net));
        int                      kept = 0, last = -1;
        bool                     ordered = true, saw_report = false;
        long                     reported = -1;
        for (const std::string &l : ls) {
            int idx = -1;
            if (l.size() > 5 && l[0] == '#' && sscanf(l.c_str(), "#%d ", &idx) == 1) {
                ++kept;
                if (idx <= last) ordered = false;
                last = idx;
            }
            const char *p = strstr(l.c_str(), "log sink: queue full, dropped ");
            if (p) {
                saw_report = true;
                sscanf(p, "log sink: queue full, dropped %ld", &reported);
            }
        }
        check("surviving lines kept their order", ordered);
        check("accepted + dropped accounts for every record enqueued (two per iteration)",
              (mid.enqueued - before.enqueued) + dropped == 2L * COUNT);
        check("some lines survived, and not all of them", kept > 0 && kept < COUNT);
        check("the writer emitted the counted-drop line into mh_net.log", saw_report);
        check("...and its count is the real one", reported == dropped);
    }

    // ---- 4. simulated crash: the drain flushes everything enqueued before it, once -----------------------
    {
        char p[MAX_PATH];
        path_of(p, "crash.log");
        DeleteFileA(p);
        mh_logq_test_hold(1);
        Sleep(50);
        for (int i = 0; i < 300; ++i) {
            char line[40];
            int  n = wsprintfA(line, "pre-crash %d\n", i);
            mh_logq_write(p, line, n);
        }
        check("nothing is on disk while the writer is parked", size_of(p) <= 0);
        const DWORD t0 = GetTickCount();
        mh_logq_local_api()->crash_drain(2000);
        const DWORD              ms = GetTickCount() - t0;
        std::vector<std::string> ls = lines_of(slurp(p));
        check("the crash drain wrote every line enqueued before it", ls.size() == 300);
        bool ordered = true;
        for (size_t i = 0; i < ls.size(); ++i) {
            char want[40];
            wsprintfA(want, "pre-crash %d", (int)i);
            if (ls[i] != want) ordered = false;
        }
        check("...in order", ordered);
        check("...promptly (it did not sit out the bound)", ms < 1500);
        // Regression (2026-10-04 gate): a non-freeing drain must still RELEASE the byte budget, or each one
        // shrinks the 8 MB bound for good and the sink ends up dropping every line.
        MH_LogQStats cs;
        mh_logq_stats(&cs);
        check("...and released the queue's byte budget (queued_bytes back to 0)", cs.queued_bytes == 0);
        mh_logq_test_hold(0);
        Sleep(30); // let the parked writer observe the release before the next arm re-parks it
        mh_logq_flush(5000);
        check("releasing the writer afterwards does not duplicate a line", lines_of(slurp(p)).size() == 300);
    }

    // ---- 5. the deadlock arm: wedged writer, bounded drain --------------------------------------------------
    {
        char p[MAX_PATH];
        path_of(p, "wedged.log");
        DeleteFileA(p);
        mh_logq_test_hold(2);
        Sleep(50);
        mh_logq_write(p, "queued while the writer was wedged\n", 35);
        Sleep(150); // let the writer wake, take the consumer flag, and park holding it
        const DWORD t0 = GetTickCount();
        mh_logq_local_api()->crash_drain(300);
        const DWORD ms = GetTickCount() - t0;
        check("the crash drain RETURNED while the consumer flag was held by a wedged writer", ms < 3000);
        printf("  wedged drain returned after %lu ms; file size at that moment %lld\n", (unsigned long)ms, size_of(p));
        check("...it waited out (about) its bound rather than returning at once", ms >= 250);
        mh_logq_test_hold(0);
        Sleep(30); // let the parked writer observe the release before the next arm re-parks it
        mh_logq_flush(5000);
        check("once the writer is released the line is written, exactly once",
              lines_of(slurp(p)).size() == 1 && slurp(p) == "queued while the writer was wedged\n");
    }

    // ---- 6. rotation by the writer -----------------------------------------------------------------------
    {
        char cur[MAX_PATH], prev[MAX_PATH];
        path_of(cur, "rot.log");
        path_of(prev, "rot.prev.log");
        DeleteFileA(cur);
        DeleteFileA(prev);
        const long long CAP = 4096;
        std::string     line(99, 'r');
        line += '\n';
        for (int batch = 0; batch < 3; ++batch) {
            for (int i = 0; i < 50; ++i) mh_logq_write(cur, line.c_str(), (int)line.size(), CAP);
            mh_logq_flush(5000);
        }
        check("a capped file rotated to its .prev", size_of(prev) >= CAP);
        check("the live file is under cap + one batch", size_of(cur) >= 0 && size_of(cur) < CAP + 65536);

        // A fresh pair of names: the sink still holds a handle on rot.log (it idles out after ~1 s), and
        // deleting a file out from under an open handle would send "A" to a delete-pending file.
        path_of(cur, "rot2.log");
        path_of(prev, "rot2.prev.log");
        DeleteFileA(cur);
        DeleteFileA(prev);
        mh_logq_write(cur, "A\n", 2);
        mh_logq_rotate_now(cur);
        mh_logq_write(cur, "B\n", 2);
        mh_logq_flush(5000);
        check("rotate-now: what came before is in .prev", slurp(prev) == "A\n");
        check("rotate-now: what came after is in the fresh file", slurp(cur) == "B\n");
    }

    // ---- 6b. BINARY records (mp:LOG2): create/truncate, order, size, reliability, ticket -----------------------
    {
        // (a) create truncates what was there, then the appends follow, in order, byte-exact.
        char p[MAX_PATH];
        path_of(p, "bin_a.bin");
        {
            HANDLE h = CreateFileA(p, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            DWORD  w = 0;
            WriteFile(h, "OLDOLDOLDOLDOLDOLD", 18, &w, nullptr);
            CloseHandle(h);
        }
        const unsigned char hdr[8] = {'M', 'H', 'O', 'R', 1, 0, 0, 0};
        mh_logq_create_bin(p, hdr, 8);
        std::string expect((const char *)hdr, 8);
        for (int i = 0; i < 5000; ++i) { // 5000 tiny records: the writer coalesces them, bytes unchanged
            unsigned char rec[8];
            for (int k = 0; k < 8; ++k) rec[k] = (unsigned char)(i * 7 + k);
            mh_logq_write_bin(p, rec, 8);
            expect.append((const char *)rec, 8);
        }
        mh_logq_flush(5000);
        check("create_bin truncated the old file and wrote header + 5000 records byte-exact", slurp(p) == expect);

        // (b) a create issued AFTER appends to the same path lands after them: only Y+Z survive.
        path_of(p, "bin_b.bin");
        mh_logq_write_bin(p, "XXXXXXXX", 8);
        mh_logq_create_bin(p, "YYYY", 4);
        mh_logq_write_bin(p, "ZZ", 2);
        mh_logq_flush(5000);
        check("create after appends keeps per-file order (X truncated, Y then Z)", slurp(p) == "YYYYZZ");

        // (c) an empty create yields an empty file; and a reopen-create truncates again.
        path_of(p, "bin_c.bin");
        mh_logq_create_bin(p, nullptr, 0);
        mh_logq_flush(5000);
        check("an empty create_bin makes a zero-byte file", size_of(p) == 0);

        // (d) records far past the TEXT limits are intact: 3 MB (> MAX_LINE) and 9 MB (> the 8 MB text bound).
        path_of(p, "bin_d.bin");
        std::string big3((size_t)3 << 20, 0), big9((size_t)9 << 20, 0);
        for (size_t i = 0; i < big3.size(); ++i) big3[i] = (char)(i * 31 + (i >> 8));
        for (size_t i = 0; i < big9.size(); ++i) big9[i] = (char)(i * 17 + (i >> 11));
        MH_LogQStats b0;
        mh_logq_stats(&b0);
        mh_logq_create_bin(p, big3.data(), (int)big3.size());
        char p9[MAX_PATH];
        path_of(p9, "bin_d9.bin");
        mh_logq_create_bin(p9, big9.data(), (int)big9.size());
        mh_logq_flush(20000);
        check("a 3 MB binary record is not truncated at the text line limit", slurp(p) == big3);
        check("a 9 MB binary record passes the 8 MB text bound intact", slurp(p9) == big9);
        MH_LogQStats b1;
        mh_logq_stats(&b1);
        check("...with nothing dropped and both budgets back to 0", b1.dropped == b0.dropped && b1.bin_dropped == 0 &&
                                                                        b1.queued_bytes == 0 && b1.bin_queued_bytes == 0);

        // (e) reliability under text pressure: park the writer, overflow the TEXT bound, then queue binary
        // records. The text overflow drops (as in arm 3); the binary records must not.
        char net[MAX_PATH], pe[MAX_PATH];
        path_of(net, "pressure.log");
        path_of(pe, "bin_e.bin");
        mh_logq_test_hold(1);
        Sleep(50);
        std::string line((size_t)1024 * 1024 - 16, 'y');
        for (int i = 0; i < 12; ++i) mh_logq_write(net, line.data(), (int)line.size());
        MH_LogQStats pe0;
        mh_logq_stats(&pe0);
        check("the text bound was hit (some text lines dropped while parked)", pe0.dropped > b1.dropped);
        mh_logq_create_bin(pe, hdr, 8);
        for (int i = 0; i < 6; ++i) mh_logq_write_bin(pe, big3.data(), (int)big3.size());
        MH_LogQStats pe1;
        mh_logq_stats(&pe1);
        check("binary records queued past the text bound were NOT dropped", pe1.bin_dropped == 0 && pe1.dropped == pe0.dropped);
        check("...and are accounted against their own budget", pe1.bin_queued_bytes >= 6 * (3 << 20));
        mh_logq_test_hold(0);
        Sleep(30);
        mh_logq_flush(20000);
        check("every binary record reached disk once the writer ran", size_of(pe) == 8 + 6LL * (3 << 20));

        // (f) the ticket: not done while the writer is parked, done after, with no waiting by the poller.
        char pf[MAX_PATH];
        path_of(pf, "bin_f.bin");
        mh_logq_test_hold(1);
        Sleep(50);
        mh_logq_create_bin(pf, big3.data(), (int)big3.size());
        const long tk = mh_logq_ticket();
        check("a ticket is not done while the writer is parked", !mh_logq_ticket_done(tk));
        mh_logq_test_hold(0);
        const DWORD t0 = GetTickCount();
        while (!mh_logq_ticket_done(tk) && GetTickCount() - t0 < 10000) Sleep(1);
        check("...and becomes done once the writer has run", mh_logq_ticket_done(tk));
        check("...at which point the whole file is on disk", size_of(pf) == (long long)big3.size());
        // ...and the writer no longer holds it: a reader that shares READ only (map_transfer resolve()) can
        // open it. A cached write handle failed exactly this open (x2a/map_absent red, 2026-10-04).
        HANDLE rh = CreateFileA(pf, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        check("...and a FILE_SHARE_READ-only reader can open it (the writer closed its handle)",
              rh != INVALID_HANDLE_VALUE);
        if (rh != INVALID_HANDLE_VALUE) CloseHandle(rh);

        // (g) a crash drain flushes a create + appends that were queued before it.
        char pg[MAX_PATH];
        path_of(pg, "bin_g.bin");
        mh_logq_test_hold(1);
        Sleep(50);
        mh_logq_create_bin(pg, "HEAD", 4);
        mh_logq_write_bin(pg, "body", 4);
        mh_logq_local_api()->crash_drain(2000);
        check("the crash drain wrote a queued create + append", slurp(pg) == "HEADbody");
        mh_logq_test_hold(0);
        Sleep(30);
        mh_logq_flush(5000);
        check("releasing the writer afterwards does not duplicate or re-truncate", slurp(pg) == "HEADbody");
    }

    // ---- 7. exit -------------------------------------------------------------------------------------------
    {
        char p[MAX_PATH];
        path_of(p, "exit.log");
        DeleteFileA(p);
        for (int i = 0; i < 200; ++i) {
            char line[32];
            int  n = wsprintfA(line, "exit %d\n", i);
            mh_logq_write(p, line, n);
        }
        mh_logq_shutdown(5000);
        check("shutdown drained everything that was queued", lines_of(slurp(p)).size() == 200);
        mh_logq_write(p, "after shutdown\n", 15);
        std::vector<std::string> ls = lines_of(slurp(p));
        check("a line after shutdown is written synchronously, not lost", ls.size() == 201 && ls.back() == "after shutdown");
    }

    // best-effort cleanup of the scratch directory
    {
        char             pat[MAX_PATH];
        WIN32_FIND_DATAA fd;
        wsprintfA(pat, "%s*", g_dir);
        HANDLE f = FindFirstFileA(pat, &fd);
        if (f != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                    char fp[MAX_PATH];
                    path_of(fp, fd.cFileName);
                    DeleteFileA(fp);
                }
            } while (FindNextFileA(f, &fd));
            FindClose(f);
        }
        RemoveDirectoryA(g_dir);
    }

    printf("  %d checks, %d failures\n", g_checks, g_fails);
    printf(g_fails ? "=== FAIL ===\n" : "=== PASS ===\n");
    return g_fails ? 1 : 0;
}
