//
// desync/replay_record.cpp -- the field-report replay inputs (replay_record.h).
//
#include "desync/replay_record.h"

#include <windows.h>

#include <cstdio>

#include "desync/state_compress.h"
#include "mh_log_sink.h" // reliable binary records, no I/O on this thread

namespace mh::desync::replay_rec {
namespace {

log_fn g_log        = nullptr;
hooks  g_h          = {};
bool   g_seed_on    = false;
int    g_hash_every = 0;
bool   g_compress   = true;

#define RLOG(...)                      \
    do {                               \
        if (g_log) g_log(__VA_ARGS__); \
    } while (0)

bool          g_open       = false;
unsigned long g_gen        = 0;
unsigned long g_closed_gen = 0;
uint32_t      g_step       = 0; // 1 = the first step with the folder open
uint32_t      g_hashes     = 0; // checkpoints written to the open file
uint32_t      g_hash_step  = 0; // the step of the last checkpoint written (no duplicate at a segment start)
int           g_segments   = 0; // segment seeds/lines past the step-1 seed
char          g_dir[MAX_PATH];
char          g_seed_path[MAX_PATH];
char          g_hash_path[MAX_PATH];
char          g_seg_path[MAX_PATH];

// The per-step poll's memory: the previous step's clock and planet (valid once `g_poll_have`).
bool   g_poll_have   = false;
double g_poll_clock  = 0.0;
int    g_poll_planet = -1;

// The boundary a seam noted, consumed at the next step. Guarded: the planet transition notes it from
// the transition's own thread.
struct pending_t {
    bool     set;
    boundary kind;
    uint32_t step_before; // the last step that ran on the old state
    double   clock_before;
    int      planet_before;
    bool     hash_known;
    uint64_t hash_before;
    int      planet; // the planet involved, -1 = n/a
    char     what[96];
    char     file[MAX_PATH];
    bool     file_ok;
    uint64_t file_bytes, file_fnv;
};
SRWLOCK   g_pend_lock = SRWLOCK_INIT;
pending_t g_pend      = {};

void hash_line(const char *s) { mh_logq_write_bin(g_hash_path, s, lstrlenA(s)); }
void seg_line(const char *s) { mh_logq_write_bin(g_seg_path, s, lstrlenA(s)); }

void close_file(const char *why) {
    if (!g_open) return;
    g_open       = false;
    g_closed_gen = g_gen;
    if (g_hash_every > 0) {
        char line[160];
        _snprintf_s(line, sizeof(line), _TRUNCATE, "; end at step %lu: %lu checkpoint(s), %s\n", (unsigned long)g_step,
                    (unsigned long)g_hashes, why);
        hash_line(line);
    }
    if (enabled()) {
        char line[160];
        _snprintf_s(line, sizeof(line), _TRUNCATE, "; end at step %lu: %d segment(s) after the step-1 seed, %s\n",
                    (unsigned long)g_step, g_segments, why);
        seg_line(line);
        RLOG("; [desync] REPLAY RECORD closed (%s): %lu step(s), %lu hash checkpoint(s), %d segment(s) after the "
             "step-1 seed\n",
             why, (unsigned long)g_step, (unsigned long)g_hashes, g_segments);
    }
}

void open_file(const char *dir, unsigned long gen) {
    g_open      = true;
    g_gen       = gen;
    g_step      = 0;
    g_hashes    = 0;
    g_hash_step = 0;
    g_segments  = 0;
    g_poll_have = false;
    lstrcpynA(g_dir, dir, MAX_PATH);
    _snprintf_s(g_seed_path, sizeof(g_seed_path), _TRUNCATE, "%s%s", dir, SEED_NAME);
    _snprintf_s(g_hash_path, sizeof(g_hash_path), _TRUNCATE, "%s%s", dir, HASH_NAME);
    _snprintf_s(g_seg_path, sizeof(g_seg_path), _TRUNCATE, "%s%s", dir, SEGMENTS_NAME);
    // A boundary noted while no session was open (a main-menu load, a new campaign) belongs to the
    // state the opening session STARTS from: its step-1 seed covers it.
    AcquireSRWLockExclusive(&g_pend_lock);
    g_pend.set = false;
    ReleaseSRWLockExclusive(&g_pend_lock);
}

double now_clock() { return g_h.clock ? g_h.clock() : 0.0; }
int    now_planet() { return g_h.planet ? g_h.planet() : -1; }

void checkpoint(uint64_t s) {
    char line[48];
    _snprintf_s(line, sizeof(line), _TRUNCATE, "%lu %08X%08X\n", (unsigned long)g_step, (unsigned)(s >> 32),
                (unsigned)s);
    hash_line(line);
    ++g_hashes;
    g_hash_step = g_step;
}

// Stage a seed (the step-1 one or segment n's) and queue its gzip. -> bytes handed over (0 = none).
uint64_t write_seed(const char *path, int segment, const char *label) {
    if (!g_seed_on || !g_h.seed) return 0;
    LARGE_INTEGER t0, t1, hz;
    QueryPerformanceCounter(&t0);
    const uint64_t n = g_h.seed(path, segment, label, g_gen, g_step, g_seg_path);
    QueryPerformanceCounter(&t1);
    const double ms = QueryPerformanceFrequency(&hz) && hz.QuadPart
                          ? (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)hz.QuadPart
                          : 0.0;
    RLOG("; [desync] SEED RECORD -> %s: segment %d (%s) at step %lu, %llu bytes (every hash region, PERSIST; "
         "the harness's seed format), %.2f ms on the sim thread; gzipped next (replay input, "
         "tools/replay_match_segment.py)\n",
         path, segment, label, (unsigned long)g_step, (unsigned long long)n, ms);
    if (g_compress && n) gz::compress_async(path, g_log, mh_logq_ticket(), "SEED RECORD");
    return n;
}

// Match step 1: the segments file's header + segment 0, the seed, then (with the hash on) the hash
// file's header.
void first_step() {
    const int cfg = g_h.configuration ? g_h.configuration() : 0;
    if (enabled()) {
        char line[512];
        _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "; mh_rec_segments v1: one line per seed. segment <n> step=<first step of n on the session "
                    "axis of mh_rec_orders/clock/hash> kind=<what replaced the state> ... seed=<file>. "
                    "configuration (%d)\n",
                    cfg);
        mh_logq_create_bin(g_seg_path, line, lstrlenA(line));
        _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "segment 0 step=1 kind=%s clock_after=%.3f planet_after=%d seed=%s\n",
                    boundary_name(boundary::session_start), now_clock(), now_planet(),
                    g_seed_on ? SEED_NAME : "none");
        seg_line(line);
    }
    if (g_hash_every > 0) {
        char line[256];
        _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "; mh_rec_hash v1: <match step> <state hash> every %d steps, hash_kind=1 (the harness "
                    "`state` column), configuration (%d)\n",
                    g_hash_every, cfg);
        mh_logq_create_bin(g_hash_path, line, lstrlenA(line));
    }
    write_seed(g_seed_path, 0, boundary_name(boundary::session_start));
}

// The first step after a boundary: segment n's seed, its line, its checkpoint.
void start_segment(const pending_t &p) {
    const int      n       = ++g_segments;
    const double   clk     = now_clock();
    const int      pl      = now_planet();
    const uint64_t h_after = g_h.hash ? g_h.hash() : 0;
    char           seed_leaf[48];
    char           seed_path[MAX_PATH];
    uint64_t       nbytes = 0;
    _snprintf_s(seed_leaf, sizeof(seed_leaf), _TRUNCATE, "mh_rec_seed_%d.bin", n);
    _snprintf_s(seed_path, sizeof(seed_path), _TRUNCATE, "%s%s", g_dir, seed_leaf);
    if (n <= SEGMENTS_MAX) nbytes = write_seed(seed_path, n, boundary_name(p.kind));
    char hb[24], fh[160];
    if (p.hash_known)
        _snprintf_s(hb, sizeof(hb), _TRUNCATE, "%08X%08X", (unsigned)(p.hash_before >> 32), (unsigned)p.hash_before);
    else
        lstrcpynA(hb, "unknown", sizeof(hb));
    if (p.file[0] && p.file_ok)
        _snprintf_s(fh, sizeof(fh), _TRUNCATE, " file_bytes=%llu file_fnv=%08X%08X",
                    (unsigned long long)p.file_bytes, (unsigned)(p.file_fnv >> 32), (unsigned)p.file_fnv);
    else if (p.file[0])
        lstrcpynA(fh, " file_bytes=unreadable", sizeof(fh));
    else
        fh[0] = '\0';
    char line[1024];
    _snprintf_s(line, sizeof(line), _TRUNCATE,
                "segment %d step=%lu kind=%s prev_step=%lu clock_before=%.3f clock_after=%.3f planet_before=%d "
                "planet_after=%d planet=%d hash_before=%s hash_after=%08X%08X source=\"%s\" file=\"%s\"%s seed=%s\n",
                n, (unsigned long)g_step, boundary_name(p.kind), (unsigned long)p.step_before, p.clock_before, clk,
                p.planet_before, pl, p.planet, hb, (unsigned)(h_after >> 32), (unsigned)h_after, p.what, p.file, fh,
                n > SEGMENTS_MAX ? "none(cap)" : (g_seed_on && nbytes ? seed_leaf : "none"));
    seg_line(line);
    RLOG("; [desync] SEGMENT %s", line);
    if (g_hash_every > 0 && g_h.hash) {
        g_hash_step = 0; // the post-boundary hash IS this step's checkpoint
        checkpoint(h_after);
    }
}

// Fill `p` with the pre-boundary facts of the state the session is running on now.
void take_before(pending_t &p, bool with_hash) {
    p.step_before   = g_step;
    p.clock_before  = now_clock();
    p.planet_before = now_planet();
    p.hash_known    = with_hash && g_h.hash != nullptr;
    p.hash_before   = p.hash_known ? g_h.hash() : 0;
}

} // namespace

const char *boundary_name(boundary b) {
    switch (b) {
        case boundary::session_start: return "session_start";
        case boundary::save_load: return "save_load";
        case boundary::planet_transition: return "planet_transition";
        case boundary::tact_return: return "tact_return";
        case boundary::new_game: return "new_game";
        case boundary::unhooked: return "unhooked";
    }
    return "?";
}

bool file_fnv64(const char *path, uint64_t *fnv, uint64_t *bytes) {
    *fnv   = 0;
    *bytes = 0;
    if (!path || !path[0]) return false;
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    uint64_t h = 0xcbf29ce484222325ull, n = 0;
    uint8_t  buf[16384];
    DWORD    got = 0;
    bool     ok  = true;
    for (;;) {
        if (!ReadFile(f, buf, sizeof(buf), &got, nullptr)) {
            ok = false;
            break;
        }
        if (got == 0) break;
        for (DWORD i = 0; i < got; ++i) {
            h ^= buf[i];
            h *= 0x100000001b3ull;
        }
        n += got;
    }
    CloseHandle(f);
    *fnv   = ok ? h : 0;
    *bytes = ok ? n : 0;
    return ok;
}

void configure(int seed_on, int hash_every, const hooks &h, log_fn log) {
    g_log        = log;
    g_h          = h;
    g_seed_on    = seed_on != 0;
    g_hash_every = hash_every > 0 ? hash_every : 0;
    if (g_seed_on || g_hash_every)
        RLOG("; [desync] REPLAY RECORD armed: seed_record=%d (%s, + %s per state-replacing boundary, up to %d) "
             "hash_record_every=%d (%s), boundaries -> %s\n",
             g_seed_on ? 1 : 0, SEED_NAME, "mh_rec_seed_<n>.bin", SEGMENTS_MAX, g_hash_every, HASH_NAME,
             SEGMENTS_NAME);
}

bool     enabled() { return g_seed_on || g_hash_every > 0; }
void     set_compress(bool on) { g_compress = on; }
uint32_t current_step() { return g_open ? g_step : 0; }
int      segments() { return g_segments; }

void note_boundary(boundary kind, const char *what, const char *file, int planet) {
    if (!enabled()) return;
    if (!g_open) {
        RLOG("; [desync] BOUNDARY %s (%s) with no session folder open: the next session's step-1 seed covers "
             "it\n",
             boundary_name(kind), what ? what : "");
        return;
    }
    pending_t p = {};
    take_before(p, true);
    p.set    = true;
    p.kind   = kind;
    p.planet = planet;
    lstrcpynA(p.what, what ? what : "", sizeof(p.what));
    lstrcpynA(p.file, file ? file : "", sizeof(p.file));
    if (p.file[0]) p.file_ok = file_fnv64(p.file, &p.file_fnv, &p.file_bytes);
    AcquireSRWLockExclusive(&g_pend_lock);
    if (g_pend.set) { // keep the first note's view of the OLD state, take the later source
        p.step_before   = g_pend.step_before;
        p.clock_before  = g_pend.clock_before;
        p.planet_before = g_pend.planet_before;
        p.hash_known    = g_pend.hash_known;
        p.hash_before   = g_pend.hash_before;
    }
    g_pend = p;
    ReleaseSRWLockExclusive(&g_pend_lock);
    RLOG("; [desync] BOUNDARY noted: %s (%s) after step %lu, clock %.3f -- segment seed at the next step\n",
         boundary_name(kind), p.what, (unsigned long)p.step_before, p.clock_before);
}

void step(const char *session_dir, unsigned long gen, bool settled) {
    // The AXIS is tracked even with both recordings off: the door watch stamps its lines with it.
    if (settled && g_open && (session_dir == nullptr || gen != g_gen)) close_file("the session folder changed");
    if (settled && !g_open && session_dir != nullptr && gen != g_closed_gen) open_file(session_dir, gen);
    if (!g_open) return;
    ++g_step;
    if (g_step == 1) first_step();
    if (enabled() && g_step > 1) {
        pending_t p = {};
        AcquireSRWLockExclusive(&g_pend_lock);
        if (g_pend.set) {
            p          = g_pend;
            g_pend.set = false;
        }
        ReleaseSRWLockExclusive(&g_pend_lock);
        // THE POLL: a replacement no seam saw. The planet changed, or the clock went backwards (the game
        // clock only moves forward while the sim runs; a load or a resync rewinds it).
        if (!p.set && g_poll_have) {
            const double clk = now_clock();
            const int    pl  = now_planet();
            if (pl != g_poll_planet || clk < g_poll_clock) {
                p.set           = true;
                p.kind          = boundary::unhooked;
                p.step_before   = g_step - 1;
                p.clock_before  = g_poll_clock;
                p.planet_before = g_poll_planet;
                p.planet        = pl;
                _snprintf_s(p.what, sizeof(p.what), _TRUNCATE, "poll: %s",
                            pl != g_poll_planet ? "the planet index changed" : "the game clock went backwards");
            }
        }
        if (p.set) start_segment(p);
    }
    if (g_h.clock || g_h.planet) {
        g_poll_clock  = now_clock();
        g_poll_planet = now_planet();
        g_poll_have   = true;
    }
    if (g_hash_every > 0 && g_h.hash && g_hash_step != g_step &&
        (g_step == 1 || (g_step % (uint32_t)g_hash_every) == 0))
        checkpoint(g_h.hash());
}

void session_close(const char *why) {
    close_file(why ? why : "session end");
}

} // namespace mh::desync::replay_rec
