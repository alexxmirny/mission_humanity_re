//
// mh_log_sink.h -- the ONE async log sink for every module that runs inside the game process
// (mp:LOG1, 2026-10-04).
//
// WHY. Until LOG1 about thirty sites did CreateFile(FILE_APPEND_DATA) -> WriteFile -> CloseHandle per
// LINE on whatever thread logged -- the game's main/sim/present thread included -- and a few more
// held a handle but still WriteFile'd synchronously. With real-time antivirus on, each close of a
// modified multi-MB file can be scanned, and field match 638fc214 showed 0.7-1.6 s main-thread
// hitches sitting between two consecutive mtrace lines. User ruling 2026-10-04: NO log I/O on a game
// thread, whether or not it is the cause. The caller now formats its line, resolves the target path
// (mh_run_path -- the folder that is current WHEN IT LOGS, so a session change moves the next line
// into the new folder), and ENQUEUES. One writer thread owns every handle.
//
// THE CONTRACT
//   * mh_logq_write / mh_logq_write2 never perform file I/O on the calling thread (when a sink is
//     published). They allocate one node from a private heap, link it into a lock-free MPSC queue,
//     and maybe SetEvent.
//   * Per-file line order is preserved (one FIFO, one consumer); two-part writes (stamp + line) land
//     contiguously.
//   * Memory is bounded (MH_LOGQ_MAX_BYTES). When full a line is DROPPED and counted; the writer
//     then emits one "; log sink dropped N line(s)" line into mh_net.log.
//   * BINARY RECORDS (mp:LOG2, 2026-10-04). A recording (orders/clock/seed/snapshot streams, a received
//     map) is not a log line: a dropped or truncated record corrupts the file, so these use the two
//     negative `cap` values MH_LOGQ_CAP_BIN (append) and MH_LOGQ_CAP_BIN_CREATE (the writer first
//     CREATE_ALWAYS-truncates the file, then appends the payload; an empty payload just creates it).
//     They are RELIABLE: exempt from the 8 MB text bound and from the 1 MB per-record truncation,
//     accounted against their own MH_LOGQ_MAX_BIN_BYTES budget (a record is dropped, and counted in
//     the sink's drop line, only past THAT). Per-file order still holds, create included: a create
//     issued after appends to the same path lands after them. Whole files (a 2.8 MB seed, an 8 MB
//     world blob) go in as consecutive records: one create, then appends.
//     mh_logq_ticket()/mh_logq_ticket_done(): "is everything enqueued so far on disk" WITHOUT
//     waiting -- for a caller that must read the file back later (the received map) and would
//     rather poll on a later frame than block this one.
//   * `rotate_cap_bytes` > 0 asks the WRITER to apply mh_log_rotate.h's one-generation rotation to
//     that file at that cap (0 = never rotate). The decision is made from the open handle's size, so
//     several writers of one file need not agree on a counter.
//   * The writer times its own CreateFile/WriteFile/CloseHandle and logs any > 50 ms into mh_net.log.
//   * mh_logq_flush(ms): synchronous, bounded, from any thread; returns when everything enqueued
//     BEFORE the call is on disk (or the bound passes). Used before TerminateProcess/_exit witnesses.
//   * mh_logq_crash_drain(ms): same, but safe from a dying thread -- see mh_log_sink.cpp "THE CRASH
//     DRAIN" for the no-deadlock argument.
//
// ONE SINK PER PROCESS, SEVERAL IMAGES. mh.dll links mh_common.lib and owns the sink
// (mh_logq_init() from DllMain publishes it). mh_net.dll / mh_harness.dll / mh_net_udp.dll do NOT link
// mh_common.lib, so they use only this header: it finds the published function table through a named
// file mapping (`Local\mh_logq1_<pid>`) and, when none is published (a selftest that did not start a
// sink, a module loaded alone), falls back to the old synchronous append -- correct, just not async.
//
#ifndef MH_LOG_SINK_H
#define MH_LOG_SINK_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "mh_log_rotate.h" // the sync fallback honours a rotate-now record too

#ifdef __cplusplus

#define MH_LOGQ_ABI           1
#define MH_LOGQ_MAX_BYTES     (8 * 1024 * 1024)   /* queued-but-unwritten payload bound (text lines) */
#define MH_LOGQ_MAX_BIN_BYTES (512 * 1024 * 1024) /* the same bound for BINARY records (mp:LOG2) */

// The `rotate_cap_bytes` argument doubles as the record kind when negative.
#define MH_LOGQ_CAP_ROTATE_NOW (-1LL) /* order-preserving rotation request (empty payload) */
#define MH_LOGQ_CAP_BIN        (-2LL) /* reliable binary append */
#define MH_LOGQ_CAP_BIN_CREATE (-3LL) /* reliable: truncate/create the file, then append the payload */

struct MH_LogQApi {
    DWORD abi;
    DWORD size;
    void (*write2)(const char *path, const void *a, int alen, const void *b, int blen, long long cap);
    void (*flush)(DWORD ms);
    void (*crash_drain)(DWORD ms);
    long (*enq_count)(void);  // records enqueued so far (mp:LOG2: ticket)
    long (*done_count)(void); // records fully on disk so far
};

// ---- implemented by mh_common/mh_log_sink.cpp (images that link mh_common.lib) -------------------
// Start the sink (writer thread + published table). Idempotent. Safe from DllMain: it creates a
// thread but never waits on it.
void mh_logq_init(void);
// Stop at process/DLL exit: drain with the bound, close every handle. Idempotent.
void mh_logq_shutdown(DWORD ms);
// The table this image's sink published (nullptr before mh_logq_init).
const MH_LogQApi *mh_logq_local_api(void);
// Counters for the selftest and the debug overlay.
struct MH_LogQStats {
    long enqueued, written, dropped, queued_bytes, slow_ops;
    long bin_queued_bytes, bin_dropped; // mp:LOG2
};
void mh_logq_stats(MH_LogQStats *out);

// ---- the client half: header-only, usable from any image -----------------------------------------
namespace mh_logq_detail {

inline void sync_append(const char *path, const void *a, int al, const void *b, int bl) {
    HANDLE h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wrote = 0;
    if (a && al > 0) WriteFile(h, a, (DWORD)al, &wrote, nullptr);
    if (b && bl > 0) WriteFile(h, b, (DWORD)bl, &wrote, nullptr);
    CloseHandle(h);
}

// The fallback for a BIN_CREATE record when no sink is published: truncate/create, then write.
inline void sync_create(const char *path, const void *a, int al, const void *b, int bl) {
    HANDLE h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wrote = 0;
    if (a && al > 0) WriteFile(h, a, (DWORD)al, &wrote, nullptr);
    if (b && bl > 0) WriteFile(h, b, (DWORD)bl, &wrote, nullptr);
    CloseHandle(h);
}

// Per-image cache of the table. A miss is retried at most every 500 ms so an image that runs
// without a sink does not pay an OpenFileMapping per line.
inline const MH_LogQApi *find_api() {
    static const MH_LogQApi *volatile s_api = nullptr;
    static volatile DWORD s_next_try        = 0;
    const MH_LogQApi     *a                 = s_api;
    if (a) return a;
    const DWORD now = GetTickCount();
    if (s_next_try != 0 && (LONG)(now - s_next_try) < 0) return nullptr;
    s_next_try = now + 500;
    if (s_next_try == 0) s_next_try = 1;
    char name[64];
    wsprintfA(name, "Local\\mh_logq%u_%lu", (unsigned)MH_LOGQ_ABI, GetCurrentProcessId());
    HANDLE m = OpenFileMappingA(FILE_MAP_READ, FALSE, name);
    if (!m) return nullptr;
    const MH_LogQApi *v = (const MH_LogQApi *)MapViewOfFile(m, FILE_MAP_READ, 0, 0, sizeof(MH_LogQApi));
    CloseHandle(m); // the view keeps the section alive
    if (!v) return nullptr;
    if (v->abi != MH_LOGQ_ABI || v->size != sizeof(MH_LogQApi) || v->write2 == nullptr) {
        UnmapViewOfFile(v);
        return nullptr;
    }
    s_api = v;
    return v;
}

} // namespace mh_logq_detail

// Enqueue [a][b] as ONE contiguous record for `path`. Either part may be empty.
inline void mh_logq_write2(const char *path, const void *a, int alen, const void *b, int blen,
                           long long rotate_cap_bytes = 0) {
    if (path == nullptr || path[0] == '\0') return;
    if (rotate_cap_bytes == MH_LOGQ_CAP_ROTATE_NOW && alen <= 0 && blen <= 0) { // the rotate-now record, see below
        const MH_LogQApi *r = mh_logq_detail::find_api();
        if (r) r->write2(path, nullptr, 0, nullptr, 0, -1);
        else mh_log_rotate(path);
        return;
    }
    const MH_LogQApi *api = mh_logq_detail::find_api();
    if (api) api->write2(path, a, alen, b, blen, rotate_cap_bytes);
    else if (rotate_cap_bytes == MH_LOGQ_CAP_BIN_CREATE) mh_logq_detail::sync_create(path, a, alen, b, blen);
    else mh_logq_detail::sync_append(path, a, alen, b, blen);
}
inline void mh_logq_write(const char *path, const void *data, int len, long long rotate_cap_bytes = 0) {
    mh_logq_write2(path, data, len, nullptr, 0, rotate_cap_bytes);
}
// NUL-terminated convenience.
inline void mh_logq_puts(const char *path, const char *s, long long rotate_cap_bytes = 0) {
    mh_logq_write2(path, s, s ? lstrlenA(s) : 0, nullptr, 0, rotate_cap_bytes);
}
// Ask the WRITER to rotate `path` (rename over its .prev, mh_log_rotate.h) at this point in the
// file's line order: everything enqueued before it lands in the rotated generation, everything after
// in the fresh file. For a stream that rotates on its own byte counter (mh_temporal.log).
inline void mh_logq_rotate_now(const char *path) {
    mh_logq_write2(path, nullptr, 0, nullptr, 0, MH_LOGQ_CAP_ROTATE_NOW);
}
// mp:LOG2: a reliable binary append (never dropped by the text bound, never truncated).
inline void mh_logq_write_bin(const char *path, const void *data, int len) {
    if (len > 0) mh_logq_write2(path, data, len, nullptr, 0, MH_LOGQ_CAP_BIN);
}
// mp:LOG2: create/truncate `path` (writer-side CREATE_ALWAYS), then append `data` (may be empty).
inline void mh_logq_create_bin(const char *path, const void *data, int len) {
    mh_logq_write2(path, data, len > 0 ? len : 0, nullptr, 0, MH_LOGQ_CAP_BIN_CREATE);
}
// mp:LOG2: a position in the queue, and whether the writer has put everything up to it on disk.
// Without a sink (selftests) every write was synchronous, so a ticket is always done.
inline long mh_logq_ticket() {
    const MH_LogQApi *api = mh_logq_detail::find_api();
    return (api && api->enq_count) ? api->enq_count() : 0;
}
inline bool mh_logq_ticket_done(long ticket) {
    const MH_LogQApi *api = mh_logq_detail::find_api();
    if (!api || !api->done_count) return true;
    return (long)(api->done_count() - ticket) >= 0;
}
// Everything enqueued before this call is on disk when it returns (or `ms` elapsed).
inline void mh_logq_flush(DWORD ms) {
    const MH_LogQApi *api = mh_logq_detail::find_api();
    if (api && api->flush) api->flush(ms);
}

#endif // __cplusplus
#endif // MH_LOG_SINK_H
