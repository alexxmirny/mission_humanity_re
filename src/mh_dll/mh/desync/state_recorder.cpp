//
// desync/state_recorder.cpp -- mp:D40, the whole-match state recorder (state_recorder.h).
//
#include "desync/state_recorder.h"

#include <windows.h>

#include <cstdlib>
#include <cstring>

#include "addr/mh_regions.gen.h"
#include "desync/state_compress.h"
#include "desync/state_record.h"
#include "include/mh_run_context.h" // MH_RunDir: the match's folder, where the D25 snapshots land

namespace mh::desync::recorder {
namespace {

namespace srec  = mh::desync::srec;
constexpr int N = mh::state::HASH_REGION_COUNT;

log_fn   g_log    = nullptr;
bool     g_on     = false;
uint32_t g_every  = srec::DEFAULT_KEYFRAME_EVERY;
uint64_t g_fp     = 0;
int64_t  g_qpc_hz = 0;
uint32_t g_total  = 0; // bytes per keyframe state (sum of slice lengths)
uint32_t g_lens[N];
bool     g_compress = true; // mp:D46: gzip the file in the background once it is closed

#define RLOG(...)                      \
    do {                               \
        if (g_log) g_log(__VA_ARGS__); \
    } while (0)

// ---- the writer thread's side ------------------------------------------------------------------
struct job {
    job     *next;
    uint8_t *p; // malloc'd; the writer frees it
    size_t   len;
    bool     seal; // one complete chunk whose CRC the writer computes (a keyframe)
};

struct rec_file {
    HANDLE           h, ev, thread;
    CRITICAL_SECTION cs;
    job             *head, *tail; // guarded by cs
    bool             closing;     // guarded by cs: no job will follow; close after the last one
    bool             with_end;    // informational, for the writer's final line
    volatile LONG    refs;        // the sim side and the writer thread each hold one
    volatile LONG    queued;      // bytes handed over and not yet written
    volatile LONG    failed;      // a write failed: the writer discards from here on
    volatile LONG    done;        // the writer closed the file
    DWORD            err;
    // writer-owned stats (read by the sim side only after `done`)
    uint64_t written;
    uint32_t writes;
    int64_t  write_ticks, max_write_ticks;
    char     path[MAX_PATH];
};

void release(rec_file *f) {
    if (InterlockedDecrement(&f->refs) != 0) return;
    DeleteCriticalSection(&f->cs);
    if (f->ev) CloseHandle(f->ev);
    if (f->thread) CloseHandle(f->thread);
    free(f);
}

DWORD WINAPI writer_main(LPVOID arg) {
    rec_file *f = static_cast<rec_file *>(arg);
    for (;;) {
        WaitForSingleObject(f->ev, INFINITE);
        bool finish = false;
        for (;;) {
            EnterCriticalSection(&f->cs);
            job *j = f->head;
            if (j) {
                f->head = j->next;
                if (!f->head) f->tail = nullptr;
            }
            const bool closing = f->closing;
            LeaveCriticalSection(&f->cs);
            if (!j) {
                finish = closing;
                break;
            }
            if (!f->failed) {
                if (j->seal) srec::seal_chunk(j->p);
                LARGE_INTEGER t0, t1;
                QueryPerformanceCounter(&t0);
                DWORD      w  = 0;
                const BOOL ok = WriteFile(f->h, j->p, (DWORD)j->len, &w, nullptr);
                QueryPerformanceCounter(&t1);
                const int64_t dt = t1.QuadPart - t0.QuadPart;
                f->write_ticks += dt;
                if (dt > f->max_write_ticks) f->max_write_ticks = dt;
                ++f->writes;
                f->written += w;
                if (!ok || w != (DWORD)j->len) {
                    f->err = GetLastError();
                    InterlockedExchange(&f->failed, 1);
                }
            }
            InterlockedExchangeAdd(&f->queued, -(LONG)j->len);
            free(j->p);
            free(j);
        }
        if (finish) break;
    }
    CloseHandle(f->h);
    const double ms = g_qpc_hz ? (double)f->max_write_ticks * 1000.0 / (double)g_qpc_hz : 0.0;
    if (f->failed)
        RLOG("; [desync] STATE RECORD writer: WRITE FAILED on %s (err %lu) after %lu bytes -- the file is a valid "
             "prefix up to its last complete chunk\n",
             f->path, (unsigned long)f->err, (unsigned long)f->written);
    else
        RLOG("; [desync] STATE RECORD writer: closed %s -- %lu bytes in %lu writes, max write %.1f ms%s\n", f->path,
             (unsigned long)f->written, (unsigned long)f->writes, ms,
             f->with_end ? "" : " (no END chunk: recording was stopped early)");
    // mp:D46: the file is closed and complete (valid prefix + END). Compression runs on a thread of its own so
    // neither this writer nor match_end()'s CLOSE_WAIT_MS wait ever covers it; the raw file stays until the .gz
    // is verified (state_compress.h).
    if (g_compress && !f->failed) gz::compress_async(f->path, g_log);
    InterlockedExchange(&f->done, 1);
    release(f);
    return 0;
}

// ---- the sim thread's side ---------------------------------------------------------------------
rec_file           *g_f       = nullptr; // the open file, or null
bool                g_pending = false;   // session_start armed a file; it opens at the next step
recorder::axis_info g_axis    = {};
srec::buf           g_batch   = {};
srec::step_builder  g_sb;
bool                g_step_open = false; // a STEP chunk was begun in g_batch this step

// This match's counters (reset at open, reported at match_end).
uint32_t g_first = 0, g_last = 0, g_steps = 0, g_keyframes = 0, g_enqueued_through = 0;
uint32_t g_last_flush_step = 0;
uint64_t g_file_bytes = 0, g_runs = 0, g_step_bytes = 0;
int64_t  g_upd_ticks = 0, g_own_ticks = 0, g_max_ticks = 0;
long     g_peak_queued = 0;
uint32_t g_rebased     = 0;
bool     g_stopped     = false;

void reset_counters() {
    g_first = g_last = g_steps = g_keyframes = g_enqueued_through = 0;
    g_last_flush_step                                             = 0;
    g_file_bytes = g_runs = g_step_bytes = 0;
    g_upd_ticks = g_own_ticks = g_max_ticks = 0;
    g_peak_queued                           = 0;
    g_rebased                               = 0;
    g_stopped                               = false;
}

double us(int64_t ticks) { return g_qpc_hz ? (double)ticks * 1e6 / (double)g_qpc_hz : 0.0; }

// Hand a buffer's bytes to the writer. Takes ownership of b.p (b is zeroed). False = recording must
// stop (the caller has already been told why in the log by stop()).
void stop(const char *why, uint32_t step);

bool enqueue(srec::buf &b, bool seal, uint32_t through_step) {
    if (b.len == 0) {
        b.release();
        return true;
    }
    if (b.oom) {
        b.release();
        stop("an allocation failed building the chunk", through_step);
        return false;
    }
    if (g_f->failed) {
        b.release();
        stop("the writer reported a failed write", through_step);
        return false;
    }
    const LONG q = InterlockedExchangeAdd(&g_f->queued, (LONG)b.len) + (LONG)b.len;
    if (q > QUEUE_CAP) {
        InterlockedExchangeAdd(&g_f->queued, -(LONG)b.len);
        b.release();
        RLOG("; [desync] STATE RECORD writer FELL BEHIND: %ld bytes handed over and unwritten would exceed the %ld-byte "
             "cap\n",
             (long)q, (long)QUEUE_CAP);
        stop("the writer fell behind", through_step);
        return false;
    }
    if (q > g_peak_queued) g_peak_queued = q;
    job *j = static_cast<job *>(malloc(sizeof(job)));
    if (!j) {
        InterlockedExchangeAdd(&g_f->queued, -(LONG)b.len);
        b.release();
        stop("an allocation failed handing a batch to the writer", through_step);
        return false;
    }
    j->next = nullptr;
    j->p    = b.p;
    j->len  = b.len;
    j->seal = seal;
    g_file_bytes += b.len;
    b = srec::buf{};
    EnterCriticalSection(&g_f->cs);
    if (g_f->tail) g_f->tail->next = j;
    else g_f->head = j;
    g_f->tail = j;
    LeaveCriticalSection(&g_f->cs);
    SetEvent(g_f->ev);
    g_enqueued_through = through_step;
    return true;
}

// Ask the writer to finish after what it has, and let go of the file. Waits up to `wait_ms` for it.
// Returns true if the writer is done.
bool close_file(bool with_end, unsigned wait_ms) {
    rec_file *f = g_f;
    g_f         = nullptr;
    EnterCriticalSection(&f->cs);
    f->closing  = true;
    f->with_end = with_end;
    LeaveCriticalSection(&f->cs);
    SetEvent(f->ev);
    bool done = false;
    if (wait_ms) done = WaitForSingleObject(f->thread, wait_ms) == WAIT_OBJECT_0;
    if (done) {
        const double ms = us(f->max_write_ticks) / 1000.0;
        RLOG("; [desync] STATE RECORD writer stats: %lu bytes written in %lu writes (mean %.2f ms, max %.1f ms), peak "
             "queue %ld KB of the %ld KB cap%s\n",
             (unsigned long)f->written, (unsigned long)f->writes,
             f->writes ? us(f->write_ticks) / 1000.0 / (double)f->writes : 0.0, ms, g_peak_queued / 1024L,
             QUEUE_CAP / 1024L, f->failed ? ", A WRITE FAILED" : "");
    } else if (wait_ms) {
        RLOG("; [desync] STATE RECORD writer still flushing after %u ms (%ld bytes queued) -- it logs its own line when "
             "the file is closed\n",
             wait_ms, (long)f->queued);
    }
    release(f);
    return done;
}

void stop(const char *why, uint32_t step) {
    if (!g_f) return;
    RLOG("; [desync] STATE RECORD STOPPED at step %lu: %s. %s stays a valid file through step %lu (every chunk "
         "written is complete and CRC-sealed; there is no END chunk). Nothing further is recorded this match.\n",
         (unsigned long)step, why, g_f->path, (unsigned long)g_enqueued_through);
    g_batch.release();
    g_step_open = false;
    g_stopped   = true;
    close_file(false, 0);
}

bool open_file(uint32_t step) {
    rec_file *f = static_cast<rec_file *>(calloc(1, sizeof(rec_file)));
    if (!f) {
        RLOG("; [desync] STATE RECORD NOT started: out of memory\n");
        return false;
    }
    const char *dir = MH_RunDir();
    HANDLE      h   = INVALID_HANDLE_VALUE;
    for (int k = 1; k <= 99 && h == INVALID_HANDLE_VALUE; ++k) {
        if (k == 1) wsprintfA(f->path, "%smh_match_state.bin", dir);
        else wsprintfA(f->path, "%smh_match_state_%d.bin", dir, k);
        // mp:D46: a finished earlier match in this folder left only <name>.gz (its raw file was deleted),
        // so the raw name being free does not make it this match's.
        char gzp[MAX_PATH + 4];
        wsprintfA(gzp, "%s.gz", f->path);
        if (GetFileAttributesA(gzp) != INVALID_FILE_ATTRIBUTES) continue;
        h = CreateFileA(f->path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE && GetLastError() != ERROR_FILE_EXISTS) break;
    }
    if (h == INVALID_HANDLE_VALUE) {
        RLOG("; [desync] STATE RECORD NOT started: cannot create %s (err %lu)\n", f->path, GetLastError());
        free(f);
        return false;
    }
    f->h    = h;
    f->refs = 2;
    InitializeCriticalSection(&f->cs);
    f->ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (f->ev) f->thread = CreateThread(nullptr, 0, writer_main, f, 0, nullptr);
    if (!f->ev || !f->thread) {
        // No writer thread means no way to record without blocking the sim on I/O: refuse, loudly.
        RLOG("; [desync] STATE RECORD NOT started: cannot create the writer thread (err %lu); %s left empty\n",
             GetLastError(), f->path);
        CloseHandle(h);
        DeleteCriticalSection(&f->cs);
        if (f->ev) CloseHandle(f->ev);
        free(f);
        return false;
    }
    g_f = f;
    reset_counters();
    g_first = g_last  = step;
    g_last_flush_step = step;
    RLOG("; [desync] STATE RECORD -> %s: %d slices, %lu bytes per keyframe, keyframe every %lu steps, manifest fp "
         "%08X%08X, format docs/state-record.md v%u; writer thread owns the file\n",
         f->path, N, (unsigned long)g_total, (unsigned long)g_every, (unsigned)(g_fp >> 32), (unsigned)g_fp,
         (unsigned)srec::VERSION);
    // The step axis, once per file (docs/state-record.md: the writer logs it so the two can be joined).
    if (g_axis.session_folder)
        RLOG("; [desync] STATE RECORD step axis: file step k = desync step k (0 at session_begin_multi, first "
             "recorded step %lu). %lu sim step(s) ran in this session folder before session_begin_multi, and the "
             "harness opens mh_match_harness.log's segment on the FIRST sim step that sees the folder, so harness "
             "match step = file step + %lu (process step = step_base + that)%s\n",
             (unsigned long)step, (unsigned long)g_axis.pre_steps, (unsigned long)g_axis.pre_steps,
             g_axis.pre_steps == 0 ? " -- THE SAME AXIS" : "");
    else
        RLOG("; [desync] STATE RECORD step axis: file step k = desync step k (0 at session_begin_multi, first "
             "recorded step %lu). No session folder is open (force-entry / harness-direct run), so the harness "
             "writes no match segment and step_base does not exist; its mh_harness.log lines carry process steps, "
             "whose offset to this axis is not observable from here\n",
             (unsigned long)step);
    return true;
}

// The keyframe: every slice's shadow (== live memory after this step's prime/update), sealed later.
void keyframe(uint32_t step) {
    const mh::state::inc::tracker &t = state_hub::tracker();
    const uint8_t                 *parts[N];
    for (int i = 0; i < N; ++i) parts[i] = t.shadow(i);
    srec::buf k = {};
    srec::put_keyf(k, step, parts, g_lens, N, false);
    if (enqueue(k, true, step)) ++g_keyframes;
}

struct listener_impl final : state_hub::listener {
    bool wants_step() const override { return g_on && (g_pending || g_f != nullptr); }

    void step_begin(uint32_t step, bool primed) override {
        g_step_open = false;
        if (g_f && !primed) {
            g_sb.begin(g_batch, step);
            g_step_open = true;
        }
    }

    void run(int region, uint32_t off, uint32_t len, const uint8_t *bytes) override {
        if (!g_step_open) return;
        g_sb.run(region, off, len, bytes);
        ++g_runs;
        g_step_bytes += len;
    }

    void rebased(int) override { ++g_rebased; }

    void step_end(uint32_t step, bool primed) override {
        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        if (g_pending) {
            // The match's first step: header + KEYF. Whether the tracker primed or updated here, its
            // shadow now equals live memory, which is all a keyframe needs.
            g_pending = false;
            if (!open_file(step)) {
                g_stopped = true;
                return;
            }
            const char *names[N];
            for (int i = 0; i < N; ++i) names[i] = mh::state::HASH_REGIONS[i].name;
            srec::put_header(g_batch, 0, g_fp, g_every, names, g_lens, N);
            if (!enqueue(g_batch, false, step)) return;
            keyframe(step);
            if (g_f) g_steps = 1;
        } else if (g_f) {
            if (g_step_open) {
                g_sb.end();
                g_step_open = false;
            } else if (primed) {
                // The tracker re-primed mid-recording (nothing does this today): this step's delta is
                // unknown, so re-base the reader with a keyframe instead of a STEP chunk.
                if (!enqueue(g_batch, false, g_last)) return;
                keyframe(step);
            }
            ++g_steps;
            g_last = step;
            if (g_f && srec::keyframe_due(step, g_first, g_every)) {
                if (enqueue(g_batch, false, step)) keyframe(step);
                g_last_flush_step = step;
            } else if (g_f && (step - g_last_flush_step >= FLUSH_STEPS || g_batch.len >= FLUSH_BYTES)) {
                if (g_batch.oom) stop("an allocation failed building a STEP chunk", step);
                else if (enqueue(g_batch, false, step)) g_last_flush_step = step;
            }
        } else {
            return;
        }
        QueryPerformanceCounter(&t1);
        const int64_t own = t1.QuadPart - t0.QuadPart;
        const int64_t upd = state_hub::last_ticks();
        g_own_ticks += own;
        g_upd_ticks += upd;
        if (own + upd > g_max_ticks) g_max_ticks = own + upd;
    }
};

listener_impl g_listener;

} // namespace

bool configure(int enabled_key, int keyframe_every, uint64_t manifest_fp, log_fn log) {
    g_log = log;
    g_on  = enabled_key != 0;
    g_fp  = manifest_fp;
    LARGE_INTEGER hz;
    g_qpc_hz = QueryPerformanceFrequency(&hz) ? hz.QuadPart : 0;
    g_total  = 0;
    for (int i = 0; i < N; ++i) {
        g_lens[i] = mh::state::HASH_REGIONS[i].len;
        g_total += g_lens[i];
    }
    if (keyframe_every < 1) {
        RLOG("; [desync] state_keyframe_every=%d refused (must be >= 1) -- using %lu\n", keyframe_every,
             (unsigned long)srec::DEFAULT_KEYFRAME_EVERY);
        g_every = srec::DEFAULT_KEYFRAME_EVERY;
    } else {
        g_every = (uint32_t)keyframe_every;
    }
    return g_on;
}

bool                 enabled() { return g_on; }
void                 set_compress(bool on) { g_compress = on; }
state_hub::listener *listener() { return &g_listener; }

void session_start(const axis_info &axis) {
    if (!g_on) return;
    if (!state_hub::allocated()) {
        RLOG("; [desync] STATE RECORD not started: the shared state tracker has no arena\n");
        return;
    }
    g_axis    = axis;
    g_pending = true;
    g_stopped = false;
}

void match_end() {
    g_pending = false;
    if (!g_f) {
        if (g_on && g_stopped && g_steps) {
            RLOG("; [desync] STATE RECORD match end: recording had STOPPED at step %lu (see above); %lu steps "
                 "recorded before it\n",
                 (unsigned long)g_enqueued_through, (unsigned long)g_steps);
            g_stopped = false;
        }
        return;
    }
    // A STEP chunk cannot be half-built here (match_end runs between steps), but be exact about it.
    g_step_open        = false;
    const uint64_t end = g_file_bytes + g_batch.len; // the file's size before the END chunk
    srec::put_end(g_batch, g_last, g_steps, end);
    const uint32_t last = g_last;
    if (!enqueue(g_batch, false, last)) return; // stop() already reported
    const double steps = g_steps ? (double)g_steps : 1.0;
    RLOG("; [desync] STATE RECORD match end: %s steps %lu..%lu (%lu recorded), %lu keyframe(s), file %lu bytes "
         "(%.0f changed B/step in %.1f exact runs/step), %lu slice rebase(s) | sim thread mean %.1f us/step (tracker "
         "update incl. every run sink %.1f + recorder append %.1f), max %.0f us\n",
         g_f->path, (unsigned long)g_first, (unsigned long)last, (unsigned long)g_steps, (unsigned long)g_keyframes,
         (unsigned long)g_file_bytes, (double)g_step_bytes / steps, (double)g_runs / steps, (unsigned long)g_rebased,
         us(g_upd_ticks + g_own_ticks) / steps, us(g_upd_ticks) / steps, us(g_own_ticks) / steps, us(g_max_ticks));
    close_file(true, CLOSE_WAIT_MS);
}

} // namespace mh::desync::recorder
