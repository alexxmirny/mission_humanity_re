//
// desync/state_recorder.cpp -- mp:D40, the whole-match state recorder (state_recorder.h).
//
#include "desync/state_recorder.h"
#include "desync/hitch_probe.h" // mp:Q4: game-thread time per recording path

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
uint32_t g_base[N];         // each slice's offset inside a keyframe's concatenated state
bool     g_compress = true; // mp:D46: gzip the file in the background once it is closed

volatile LONG g_writers = 0; // writer threads not finished yet (wait_idle; the exit drain)

#define RLOG(...)                      \
    do {                               \
        if (g_log) g_log(__VA_ARGS__); \
    } while (0)

// ---- the writer thread's side ------------------------------------------------------------------
//
// mp:Q4: THE WRITER OWNS EVERY FILE OPERATION AND EVERY KEYFRAME BUT THE FIRST. It creates the file
// (the name probe and CreateFile used to run on the sim thread at the match's first step), and it
// keeps the keyframe IMAGE: the first KEYF chunk the sim thread hands it (a copy of the tracker's
// shadow at step 1) stays with the writer, every STEP chunk it writes is applied to that image, so at
// a later keyframe step the image IS the state at that step and the writer re-stamps and re-seals the
// same buffer. The sim thread used to copy the 2.8 MB shadow at every keyframe (once a minute).
enum job_kind : uint8_t {
    J_RAW,       // bytes to write as they are (the header)
    J_CHUNKS,    // a batch of complete chunks: written, and every STEP chunk applied to the image
    J_KEYF_INIT, // a whole KEYF chunk (unsealed) copied from the shadow: becomes the image
    J_KEYF_AT,   // no bytes: write the image as the KEYF of `step`
};

struct job {
    job     *next;
    uint8_t *p; // malloc'd; the writer frees it (J_KEYF_INIT: keeps it as the image)
    size_t   len;
    job_kind kind;
    uint32_t step; // J_KEYF_AT
};

struct rec_file {
    HANDLE           h, ev, thread;
    CRITICAL_SECTION cs;
    job             *head, *tail; // guarded by cs
    bool             closing;     // guarded by cs: no job will follow; close after the last one
    bool             with_end;    // informational, for the writer's final line
    volatile LONG    refs;        // the sim side and the writer thread each hold one
    volatile LONG    queued;      // bytes handed over and not yet written
    volatile LONG    failed;      // the file could not be created, or a write failed: discard from here on
    volatile LONG    done;        // the writer closed the file
    volatile LONG    path_ready;  // the writer has created the file and `path` names it
    volatile LONG    peak_queued; // the sim side's peak, for the writer's final line
    DWORD            err;
    // writer-owned
    uint8_t *image; // the last KEYF chunk (CHUNK_HDR + u32 step + the state), or null
    size_t   image_len;
    uint32_t keyframes;
    uint64_t written;
    uint32_t writes;
    int64_t  write_ticks, max_write_ticks;
    int64_t  open_ticks;
    char     dir[MAX_PATH];
    char     path[MAX_PATH];
};

void release(rec_file *f) {
    if (InterlockedDecrement(&f->refs) != 0) return;
    DeleteCriticalSection(&f->cs);
    if (f->ev) CloseHandle(f->ev);
    if (f->thread) CloseHandle(f->thread);
    free(f->image);
    free(f);
}

double us(int64_t ticks) { return g_qpc_hz ? (double)ticks * 1e6 / (double)g_qpc_hz : 0.0; }

// The name probe + CreateFile, on the writer thread. A finished earlier match in this folder left only
// <name>.gz (mp:D46: its raw file was deleted), so the raw name being free does not make it this match's.
bool create_file(rec_file *f) {
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    HANDLE h = INVALID_HANDLE_VALUE;
    for (int k = 1; k <= 99 && h == INVALID_HANDLE_VALUE; ++k) {
        if (k == 1) wsprintfA(f->path, "%smh_match_state.bin", f->dir);
        else wsprintfA(f->path, "%smh_match_state_%d.bin", f->dir, k);
        char gzp[MAX_PATH + 4];
        wsprintfA(gzp, "%s.gz", f->path);
        if (GetFileAttributesA(gzp) != INVALID_FILE_ATTRIBUTES) continue;
        h = CreateFileA(f->path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE && GetLastError() != ERROR_FILE_EXISTS) break;
    }
    QueryPerformanceCounter(&t1);
    f->open_ticks = t1.QuadPart - t0.QuadPart;
    if (h == INVALID_HANDLE_VALUE) {
        f->err = GetLastError();
        RLOG("; [desync] STATE RECORD NOT started: cannot create %s (err %lu) -- nothing is recorded this match\n",
             f->path, (unsigned long)f->err);
        InterlockedExchange(&f->failed, 1);
        return false;
    }
    f->h = h;
    InterlockedExchange(&f->path_ready, 1);
    RLOG("; [desync] STATE RECORD -> %s: %d slices, %lu bytes per keyframe, keyframe every %lu steps, manifest fp "
         "%08X%08X, format docs/state-record.md v%u; the writer thread created the file (%.1f ms) and owns it\n",
         f->path, N, (unsigned long)g_total, (unsigned long)g_every, (unsigned)(g_fp >> 32), (unsigned)g_fp,
         (unsigned)srec::VERSION, us(f->open_ticks) / 1000.0);
    return true;
}

void write_bytes(rec_file *f, const uint8_t *p, size_t len) {
    if (f->failed || len == 0) return;
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    DWORD      w  = 0;
    const BOOL ok = WriteFile(f->h, p, (DWORD)len, &w, nullptr);
    QueryPerformanceCounter(&t1);
    const int64_t dt = t1.QuadPart - t0.QuadPart;
    f->write_ticks += dt;
    if (dt > f->max_write_ticks) f->max_write_ticks = dt;
    ++f->writes;
    f->written += w;
    if (!ok || w != (DWORD)len) {
        f->err = GetLastError();
        InterlockedExchange(&f->failed, 1);
    }
}

// Apply every STEP chunk in a batch of complete chunks to the image (bounds-checked: a malformed run
// is skipped, never written outside the image).
void apply_steps(rec_file *f, const uint8_t *p, size_t len) {
    if (!f->image) return;
    uint8_t     *state = f->image + srec::CHUNK_HDR + 4;
    size_t       pos   = 0;
    const size_t HDR   = srec::CHUNK_HDR;
    while (pos + HDR <= len) {
        uint32_t tag, plen;
        memcpy(&tag, p + pos, 4);
        memcpy(&plen, p + pos + 4, 4);
        if (pos + HDR + plen > len) return;
        if (tag == srec::TAG_STEP && plen >= 8) {
            const uint8_t *q = p + pos + HDR, *e = q + plen;
            uint32_t       runs;
            memcpy(&runs, q + 4, 4);
            q += 8;
            for (uint32_t r = 0; r < runs && q + srec::RUN_HDR <= e; ++r) {
                uint16_t reg, rl;
                uint32_t off;
                memcpy(&reg, q, 2);
                memcpy(&off, q + 2, 4);
                memcpy(&rl, q + 6, 2);
                q += srec::RUN_HDR;
                if (q + rl > e) return;
                if (reg < N && (uint64_t)off + rl <= g_lens[reg]) memcpy(state + g_base[reg] + off, q, rl);
                q += rl;
            }
        }
        pos += HDR + plen;
    }
}

void write_image_at(rec_file *f, uint32_t step) {
    if (!f->image) return;
    srec::poke32(f->image + srec::CHUNK_HDR, step);
    srec::seal_chunk(f->image);
    write_bytes(f, f->image, f->image_len);
    ++f->keyframes;
}

DWORD WINAPI writer_main(LPVOID arg) {
    rec_file *f = static_cast<rec_file *>(arg);
    create_file(f);
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
            switch (j->kind) {
                case J_RAW:
                    write_bytes(f, j->p, j->len);
                    break;
                case J_CHUNKS:
                    write_bytes(f, j->p, j->len);
                    apply_steps(f, j->p, j->len);
                    break;
                case J_KEYF_INIT: {
                    free(f->image);
                    f->image     = j->p;
                    f->image_len = j->len;
                    j->p         = nullptr; // kept as the image
                    uint32_t step;
                    memcpy(&step, f->image + srec::CHUNK_HDR, 4);
                    write_image_at(f, step);
                    break;
                }
                case J_KEYF_AT:
                    write_image_at(f, j->step);
                    break;
            }
            InterlockedExchangeAdd(&f->queued, -(LONG)j->len);
            free(j->p);
            free(j);
        }
        if (finish) break;
    }
    if (f->h != INVALID_HANDLE_VALUE) CloseHandle(f->h);
    free(f->image);
    f->image = nullptr;
    if (f->path_ready) {
        if (f->failed)
            RLOG("; [desync] STATE RECORD writer: WRITE FAILED on %s (err %lu) after %lu bytes -- the file is a valid "
                 "prefix up to its last complete chunk\n",
                 f->path, (unsigned long)f->err, (unsigned long)f->written);
        else
            RLOG("; [desync] STATE RECORD writer: closed %s -- %lu bytes in %lu writes (mean %.2f ms, max %.1f ms), "
                 "%lu keyframe(s) written from the writer's image, peak queue %ld KB of the %ld KB cap, create %.1f "
                 "ms%s\n",
                 f->path, (unsigned long)f->written, (unsigned long)f->writes,
                 f->writes ? us(f->write_ticks) / 1000.0 / (double)f->writes : 0.0, us(f->max_write_ticks) / 1000.0,
                 (unsigned long)f->keyframes, (long)f->peak_queued / 1024L, QUEUE_CAP / 1024L,
                 us(f->open_ticks) / 1000.0, f->with_end ? "" : " (no END chunk: recording was stopped early)");
        // mp:D46: the file is closed and complete (valid prefix + END). Compression runs on a thread of
        // its own; the raw file stays until the .gz is verified (state_compress.h).
        if (g_compress && !f->failed) gz::compress_async(f->path, g_log);
    }
    InterlockedExchange(&f->done, 1);
    release(f);
    InterlockedDecrement(&g_writers);
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

void stop(const char *why, uint32_t step);

const char *writer_failure() {
    return g_f->path_ready ? "the writer reported a failed write" : "the writer could not create the file";
}

// Link a job onto the writer's queue (ownership of j and j->p passes to the writer).
void push_job(job *j) {
    EnterCriticalSection(&g_f->cs);
    if (g_f->tail) g_f->tail->next = j;
    else g_f->head = j;
    g_f->tail = j;
    LeaveCriticalSection(&g_f->cs);
    SetEvent(g_f->ev);
}

// Hand a buffer's bytes to the writer. Takes ownership of b.p (b is zeroed). False = recording must
// stop (the caller has already been told why in the log by stop()).
bool enqueue(srec::buf &b, job_kind kind, uint32_t through_step) {
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
        stop(writer_failure(), through_step);
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
    if (q > g_peak_queued) {
        g_peak_queued = q;
        InterlockedExchange(&g_f->peak_queued, q);
    }
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
    j->kind = kind;
    j->step = through_step;
    g_file_bytes += b.len;
    b = srec::buf{};
    push_job(j);
    g_enqueued_through = through_step;
    return true;
}

// Ask the writer to finish after what it has, and let go of the file. mp:Q4: NEVER waits -- the writer
// logs its own closing line, and a process that ends before it finishes leaves a valid prefix.
void close_file(bool with_end) {
    rec_file *f = g_f;
    g_f         = nullptr;
    EnterCriticalSection(&f->cs);
    f->closing  = true;
    f->with_end = with_end;
    LeaveCriticalSection(&f->cs);
    SetEvent(f->ev);
    release(f);
}

void stop(const char *why, uint32_t step) {
    if (!g_f) return;
    RLOG("; [desync] STATE RECORD STOPPED at step %lu: %s. %s stays a valid file through step %lu (every chunk "
         "written is complete and CRC-sealed; there is no END chunk). Nothing further is recorded this match.\n",
         (unsigned long)step, why, g_f->path_ready ? g_f->path : "the file", (unsigned long)g_enqueued_through);
    g_batch.release();
    g_step_open = false;
    g_stopped   = true;
    close_file(false);
}

// Start the writer thread for this match. mp:Q4: no file operation here -- the writer creates the file.
bool open_file(uint32_t step) {
    rec_file *f = static_cast<rec_file *>(calloc(1, sizeof(rec_file)));
    if (!f) {
        RLOG("; [desync] STATE RECORD NOT started: out of memory\n");
        return false;
    }
    const char *dir = MH_RunDir();
    lstrcpynA(f->dir, dir ? dir : "", MAX_PATH);
    f->h    = INVALID_HANDLE_VALUE;
    f->refs = 2;
    InitializeCriticalSection(&f->cs);
    f->ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    InterlockedIncrement(&g_writers);
    if (f->ev) f->thread = CreateThread(nullptr, 0, writer_main, f, 0, nullptr);
    if (!f->ev || !f->thread) {
        // No writer thread means no way to record without blocking the sim on I/O: refuse, loudly.
        InterlockedDecrement(&g_writers);
        RLOG("; [desync] STATE RECORD NOT started: cannot create the writer thread (err %lu)\n", GetLastError());
        DeleteCriticalSection(&f->cs);
        if (f->ev) CloseHandle(f->ev);
        free(f);
        return false;
    }
    g_f = f;
    reset_counters();
    g_first = g_last  = step;
    g_last_flush_step = step;
    // The step axis, once per file (docs/state-record.md: the writer logs it so the two can be joined).
    if (g_axis.solo)
        RLOG("; [desync] STATE RECORD step axis (SOLO session, mp:Q2): file step k = the k-th sim step after the "
             "solo session folder opened (first recorded step %lu). %lu sim step(s) ran in this folder before the "
             "recorder armed, so harness match step = file step + %lu%s\n",
             (unsigned long)step, (unsigned long)g_axis.pre_steps, (unsigned long)g_axis.pre_steps,
             g_axis.pre_steps == 0 ? " -- THE SAME AXIS" : "");
    else if (g_axis.session_folder)
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

// A keyframe COPIED from the shadow (== live memory after this step's prime/update): the match's first
// step, and a re-prime. The writer keeps it as its image and seals it.
void keyframe_copy(uint32_t step) {
    const mh::state::inc::tracker &t = state_hub::tracker();
    const uint8_t                 *parts[N];
    for (int i = 0; i < N; ++i) parts[i] = t.shadow(i);
    srec::buf k = {};
    srec::put_keyf(k, step, parts, g_lens, N, false);
    if (enqueue(k, J_KEYF_INIT, step)) ++g_keyframes;
}

// A periodic keyframe: the writer writes its image, which every earlier STEP chunk has brought to this
// step. No bytes cross from the sim thread.
void keyframe_at(uint32_t step) {
    if (g_f->failed) {
        stop(writer_failure(), step);
        return;
    }
    job *j = static_cast<job *>(calloc(1, sizeof(job)));
    if (!j) {
        stop("an allocation failed handing a keyframe to the writer", step);
        return;
    }
    j->kind = J_KEYF_AT;
    j->step = step;
    g_file_bytes += srec::keyf_chunk_bytes(g_total);
    push_job(j);
    g_enqueued_through = step;
    ++g_keyframes;
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
            if (!enqueue(g_batch, J_RAW, step)) return;
            keyframe_copy(step);
            if (g_f) g_steps = 1;
        } else if (g_f) {
            if (g_step_open) {
                g_sb.end();
                g_step_open = false;
            } else if (primed) {
                // The tracker re-primed mid-recording (nothing does this today): this step's delta is
                // unknown, so re-base the reader (and the writer's image) with a copied keyframe.
                if (!enqueue(g_batch, J_CHUNKS, g_last)) return;
                keyframe_copy(step);
            }
            ++g_steps;
            g_last = step;
            if (g_f && srec::keyframe_due(step, g_first, g_every)) {
                if (enqueue(g_batch, J_CHUNKS, step) && g_f) keyframe_at(step);
                g_last_flush_step = step;
            } else if (g_f && (step - g_last_flush_step >= FLUSH_STEPS || g_batch.len >= FLUSH_BYTES)) {
                if (g_batch.oom) stop("an allocation failed building a STEP chunk", step);
                else if (enqueue(g_batch, J_CHUNKS, step)) g_last_flush_step = step;
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
        hitch::add_ticks(hitch::REC_STEP, own);
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
        g_base[i] = g_total;
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
    hitch::scope hs(hitch::REC_CLOSE);
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
    if (!enqueue(g_batch, J_CHUNKS, last)) return; // stop() already reported
    const double steps = g_steps ? (double)g_steps : 1.0;
    RLOG("; [desync] STATE RECORD match end: %s steps %lu..%lu (%lu recorded), %lu keyframe(s), file %lu bytes "
         "(%.0f changed B/step in %.1f exact runs/step), %lu slice rebase(s) | sim thread mean %.1f us/step (tracker "
         "update incl. every run sink %.1f + recorder append %.1f), max %.0f us; the writer finishes the file in the "
         "background (its own closing line follows)\n",
         g_f->path_ready ? g_f->path : "(file still being created)", (unsigned long)g_first, (unsigned long)last,
         (unsigned long)g_steps, (unsigned long)g_keyframes, (unsigned long)g_file_bytes,
         (double)g_step_bytes / steps, (double)g_runs / steps, (unsigned long)g_rebased,
         us(g_upd_ticks + g_own_ticks) / steps, us(g_upd_ticks) / steps, us(g_own_ticks) / steps, us(g_max_ticks));
    close_file(true);
}

bool wait_idle(unsigned ms) {
    const DWORD t0 = GetTickCount();
    while (g_writers != 0) {
        if (GetTickCount() - t0 >= ms) return false;
        Sleep(2);
    }
    return true;
}

long active() { return g_writers; }

} // namespace mh::desync::recorder
