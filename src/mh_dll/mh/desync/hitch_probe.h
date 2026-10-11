//
// desync/hitch_probe.h -- game-thread time spent in the recording paths (mp:Q4), max + p99 per path.
//
// WHY. The recording features (the state recorder, the order recording, the desync ring and the D25
// snapshots, the session folder's open/close) each promised "no file I/O on the game thread", and each
// promise was checked once, by reading the code. This makes it a NUMBER, per path, in every run: the
// game (sim) thread's time inside each path, as a count, a mean, a p99 and an exact max, logged as one
// `; [hitch]` line per path at every session close and then reset. A blocking write shows up as a max
// in milliseconds where the p99 is microseconds.
//
// THE PATHS. Per-step paths are sampled once per call on the sim thread (STEP_TOTAL is the whole
// [desync] step hook: tracker update + every listener + the order tick + the detector). The close/open
// paths are sampled once per session boundary; SESSION_CLOSE is the whole quit-to-menu / match-end
// close on the game thread (everything the close does, recording or not), and its parts are the
// *_CLOSE rows.
//
// COST. One QueryPerformanceCounter pair and one log2 per sample. Single-threaded by construction:
// every caller is the game's main thread (the sim step, the present-time session poll and the session
// seams all run there), so the histograms take no lock. HEADER-ONLY (C++17 inline variables): one copy
// per image, and only mh.dll (+ the selftest image) includes it.
//
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace mh::desync::hitch {

enum path : int {
    STEP_TOTAL,    // the whole [desync] per-step hook (tracker + listeners + order tick + detector)
    TRACKER,       // the shared tracker's update incl. every listener's run callbacks
    REC_STEP,      // the state recorder's own per-step work (file arm, chunk hand-over, keyframes)
    ORD_STEP,      // the order recording's step tick (open/close/batch hand-over)
    ORD_DISPATCH,  // the order recording's dispatch-entry copy
    RING_STEP,     // the desync ring's own per-step work (incl. the frozen window's hand-over)
    SNAPSHOT,      // a D25 grid snapshot (only after a detected desync)
    REC_CLOSE,     // the state recorder's match end
    RING_CLOSE,    // the desync ring's match end
    ORD_CLOSE,     // the order recording's session close
    SESSION_OPEN,  // a session folder's open (mp_session_open: directory, breadcrumb, record)
    SESSION_CLOSE, // a session's close on the game thread (all of it: rollups, record, directory switch)
    PATHS
};

inline const char *name(int p) {
    static const char *const N[PATHS] = {"step_total", "tracker", "rec_step", "ord_step",
                                         "ord_dispatch", "ring_step", "snapshot", "rec_close",
                                         "ring_close", "ord_close", "session_open", "session_close"};
    return (p >= 0 && p < PATHS) ? N[p] : "?";
}

// 8 buckets per octave of microseconds (bucket i >= 1 holds [2^((i-1)/8), 2^(i/8)) us, ~9% wide);
// bucket 0 holds < 1 us. 240 buckets reach 2^29.9 us (~17 minutes).
inline constexpr int NB = 240;

struct hist {
    uint32_t n;
    double   sum_us, max_us;
    uint32_t b[NB];
};

inline hist    g_hist[PATHS];
inline int64_t g_hz = 0;

inline double us_of(int64_t ticks) {
    if (g_hz == 0) {
        LARGE_INTEGER f;
        g_hz = QueryPerformanceFrequency(&f) ? f.QuadPart : 1;
    }
    return (double)ticks * 1e6 / (double)g_hz;
}

inline int bucket_of(double us) {
    if (us < 1.0) return 0;
    int i = 1 + (int)(8.0 * std::log2(us));
    return i < NB ? i : NB - 1;
}

// The upper edge of a bucket, in us (bucket 0: 1 us).
inline double bucket_hi(int i) { return i <= 0 ? 1.0 : std::exp2((double)i / 8.0); }

inline void add_us(int p, double us) {
    if (p < 0 || p >= PATHS) return;
    hist &h = g_hist[p];
    ++h.n;
    h.sum_us += us;
    if (us > h.max_us) h.max_us = us;
    ++h.b[bucket_of(us)];
}

inline void add_ticks(int p, int64_t ticks) { add_us(p, us_of(ticks)); }

// The q-quantile's bucket upper edge, clamped to the exact max (a p99 never reads above the max).
inline double quantile(const hist &h, double q) {
    if (h.n == 0) return 0.0;
    const double want = std::ceil(q * (double)h.n);
    double       cum  = 0.0;
    for (int i = 0; i < NB; ++i) {
        cum += h.b[i];
        if (cum >= want) {
            const double hi = bucket_hi(i);
            return hi < h.max_us ? hi : h.max_us;
        }
    }
    return h.max_us;
}

struct scope {
    int           p;
    LARGE_INTEGER t0;
    explicit scope(int path) : p(path) { QueryPerformanceCounter(&t0); }
    ~scope() {
        LARGE_INTEGER t1;
        QueryPerformanceCounter(&t1);
        add_ticks(p, t1.QuadPart - t0.QuadPart);
    }
    scope(const scope &)            = delete;
    scope &operator=(const scope &) = delete;
};

inline bool any() {
    for (const hist &h : g_hist)
        if (h.n) return true;
    return false;
}

inline void reset() { memset(g_hist, 0, sizeof(g_hist)); }

// One `; [hitch]` line per path that ran, then reset. `emit` takes a whole line.
inline void report(void (*emit)(const char *), const char *when) {
    if (!emit) return;
    for (int p = 0; p < PATHS; ++p) {
        const hist &h = g_hist[p];
        if (!h.n) continue;
        char line[256];
        _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "; [hitch] %-13s n=%lu mean=%.1f us p99=%.0f us max=%.0f us (game thread, %s)\n", name(p),
                    (unsigned long)h.n, h.sum_us / (double)h.n, quantile(h, 0.99), h.max_us, when);
        emit(line);
    }
    reset();
}

} // namespace mh::desync::hitch
