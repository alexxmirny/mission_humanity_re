//
// desync/state_ring.cpp -- mp:D41, the ship `net` state ring (state_ring.h).
//
#include "desync/state_ring.h"

#include <windows.h>

#include <cstdlib>
#include <cstring>

#include "addr/mh_regions.gen.h"
#include "include/mh_run_context.h" // MH_RunDir: the match's folder, where the D25 snapshots land

namespace mh::desync::state_ring {
namespace {

namespace srec  = mh::desync::srec;
constexpr int N = mh::state::HASH_REGION_COUNT;

log_fn g_log           = nullptr;
bool   g_on            = false;
bool (*g_live)()       = nullptr;
uint64_t      g_fp     = 0;
int64_t       g_qpc_hz = 0;
uint32_t      g_lens[N];
uint64_t      g_total = 0; // bytes of one full state (sum of slice lengths)
sring::config g_cfg   = {};
settings      g_set;

#define GLOG(...)                      \
    do {                               \
        if (g_log) g_log(__VA_ARGS__); \
    } while (0)

// ---- the one-shot writer ------------------------------------------------------------------------
struct dump_job {
    sring::frozen f;
    srec::buf     header;
    char          dir[MAX_PATH];
    uint32_t      mismatch_step, trigger_step;
    const char   *why;
};

volatile LONG g_writers = 0; // background writers not finished yet (selftest: writer_idle)

bool put_file(void *ctx, const void *p, size_t n) {
    HANDLE h = *static_cast<HANDLE *>(ctx);
    while (n) {
        const DWORD want = n > 0x100000u ? 0x100000u : (DWORD)n;
        DWORD       w    = 0;
        if (!WriteFile(h, p, want, &w, nullptr) || w != want) return false;
        p = static_cast<const uint8_t *>(p) + w;
        n -= w;
    }
    return true;
}

DWORD WINAPI writer_main(LPVOID arg) {
    dump_job *j = static_cast<dump_job *>(arg);
    char      path[MAX_PATH];
    HANDLE    h = INVALID_HANDLE_VALUE;
    for (int k = 1; k <= 99 && h == INVALID_HANDLE_VALUE; ++k) {
        if (k == 1) wsprintfA(path, "%smh_desync_state.bin", j->dir);
        else wsprintfA(path, "%smh_desync_state_%d.bin", j->dir, k);
        h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE && GetLastError() != ERROR_FILE_EXISTS) break;
    }
    if (h == INVALID_HANDLE_VALUE) {
        GLOG("; [desync] STATE RING NOT WRITTEN: cannot create %s (err %lu)\n", path, GetLastError());
    } else {
        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        const bool  ok  = sring::serialize(j->header, j->f, &put_file, &h);
        const DWORD err = ok ? 0 : GetLastError();
        CloseHandle(h);
        QueryPerformanceCounter(&t1);
        const double   ms    = g_qpc_hz ? (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)g_qpc_hz : 0.0;
        const uint64_t bytes = sring::file_bytes(j->header, j->f);
        if (ok)
            GLOG("; [desync] STATE RING -> %s: %lu bytes, steps %lu..%lu (KEYF + %lu STEP), %lu before the mismatch "
                 "at step %lu, %lu after the trigger at step %lu (%s); %.1f ms on a background thread\n",
                 path, (unsigned long)bytes, (unsigned long)j->f.base_step, (unsigned long)j->f.last_step,
                 (unsigned long)j->f.chunks,
                 (unsigned long)(j->mismatch_step > j->f.base_step ? j->mismatch_step - j->f.base_step : 0),
                 (unsigned long)j->mismatch_step, (unsigned long)(j->f.last_step - j->trigger_step),
                 (unsigned long)j->trigger_step, j->why, ms);
        else
            GLOG("; [desync] STATE RING WRITE FAILED on %s (err %lu): the file is a valid prefix up to its last "
                 "complete chunk\n",
                 path, (unsigned long)err);
    }
    j->f.release();
    j->header.release();
    free(j);
    InterlockedDecrement(&g_writers);
    return 0;
}

// ---- the sim thread's side ---------------------------------------------------------------------
sring::core g_core;
bool        g_armed_match = false; // session_start armed this match; cleared when it dumps or ends
bool        g_dumped      = false; // this match already dumped (or tried to)
bool        g_step_open   = false;
bool        g_alloc_said  = false;

// This match's counters.
uint32_t g_steps = 0, g_rekeys = 0, g_first_live = 0;
int64_t  g_upd_ticks = 0, g_own_ticks = 0, g_max_ticks = 0;
size_t   g_peak_ram = 0;

void reset_counters() {
    g_steps = g_rekeys = g_first_live = 0;
    g_upd_ticks = g_own_ticks = g_max_ticks = 0;
    g_peak_ram                              = 0;
}

double us(int64_t t) { return g_qpc_hz ? (double)t * 1e6 / (double)g_qpc_hz : 0.0; }

bool ensure_alloc() {
    if (g_core.allocated()) return true;
    if (!g_core.alloc(g_lens, N, g_cfg)) {
        GLOG("; [desync] STATE RING NOT allocated: out of memory for the %lu-byte base + %lu-byte chunk ring -- no "
             "ring this match\n",
             (unsigned long)g_total, (unsigned long)g_cfg.cap_bytes);
        g_armed_match = false;
        return false;
    }
    if (!g_alloc_said) {
        g_alloc_said = true;
        GLOG("; [desync] STATE RING allocated at the first live lockstep step: %lu KB (base image %lu KB + chunk ring "
             "%lu KB)\n",
             (unsigned long)(g_core.ram_bytes() / 1024), (unsigned long)(g_core.state_bytes() / 1024),
             (unsigned long)(g_cfg.cap_bytes / 1024));
    }
    return true;
}

void rekey_from_shadow(uint32_t step) {
    const mh::state::inc::tracker &t = state_hub::tracker();
    const uint8_t                 *parts[N];
    for (int i = 0; i < N; ++i) parts[i] = t.shadow(i);
    g_core.rekey(step, parts);
    ++g_rekeys;
}

// Hand the frozen window to a background writer. Once per match.
void finish(const char *why) {
    if (g_core.ph() != sring::phase::tail) return;
    const uint32_t mismatch = g_core.mismatch_step(), trig = g_core.trigger_step();
    dump_job      *j = static_cast<dump_job *>(calloc(1, sizeof(dump_job)));
    if (!j) {
        GLOG("; [desync] STATE RING NOT WRITTEN: out of memory for the writer job\n");
        g_core.clear();
        return;
    }
    const char *names[N];
    for (int i = 0; i < N; ++i) names[i] = mh::state::HASH_REGIONS[i].name;
    srec::put_header(j->header, srec::FLAG_RING, g_fp, 0, names, g_lens, N);
    if (j->header.oom) {
        GLOG("; [desync] STATE RING NOT WRITTEN: out of memory for the header\n");
        free(j);
        g_core.clear();
        return;
    }
    const char *dir = MH_RunDir();
    lstrcpynA(j->dir, dir ? dir : "", MAX_PATH);
    j->mismatch_step = mismatch;
    j->trigger_step  = trig;
    j->why           = why;
    j->f             = g_core.detach(); // the core is empty now; the next match allocates afresh
    GLOG("; [desync] STATE RING frozen (%s): steps %lu..%lu, %lu KB of STEP chunks + the %lu KB base, handed to a "
         "background writer\n",
         why, (unsigned long)j->f.base_step, (unsigned long)j->f.last_step, (unsigned long)(j->f.used / 1024),
         (unsigned long)(j->f.base_len / 1024));
    InterlockedIncrement(&g_writers);
    HANDLE t = CreateThread(nullptr, 0, writer_main, j, 0, nullptr);
    if (!t) {
        GLOG("; [desync] STATE RING: no writer thread (err %lu) -- writing on the sim thread instead\n",
             GetLastError());
        writer_main(j);
        return;
    }
    CloseHandle(t);
}

struct listener_impl final : state_hub::listener {
    bool wants_step() const override {
        if (!g_on || !g_armed_match) return false;
        if (g_core.ph() == sring::phase::tail) return true; // the tail owes its evidence regardless
        if (g_dumped) return false;                         // dumped (or nothing to dump): done this match
        return g_live && g_live();
    }

    void step_begin(uint32_t step, bool primed) override {
        g_step_open = false;
        if (!primed && g_core.allocated() && g_core.ph() != sring::phase::empty) {
            g_core.begin_step(step);
            g_step_open = true;
        }
    }

    void run(int region, uint32_t off, uint32_t len, const uint8_t *bytes) override {
        if (g_step_open) g_core.run(region, off, len, bytes);
    }

    void step_end(uint32_t step, bool primed) override {
        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        ++g_steps;
        if (g_step_open) {
            g_step_open         = false;
            const sring::push r = g_core.end_step();
            if (r != sring::push::ok) {
                if (g_core.ph() == sring::phase::tail) {
                    finish(r == sring::push::tail_full ? "the byte cap filled during the tail"
                                                       : "the tracker lost a step during the tail");
                } else {
                    if (r == sring::push::oom)
                        GLOG("; [desync] STATE RING: out of memory building step %lu's chunk -- re-keyed there\n",
                             (unsigned long)step);
                    rekey_from_shadow(step); // too big / gap / oom: the full state at this step, window empty
                }
            }
        } else if (primed || g_core.ph() == sring::phase::empty) {
            // The match's first live step, or the tracker re-primed (steps were skipped while the
            // session was not live). The shadow equals live memory after a prime: key the window here.
            if (g_core.ph() == sring::phase::tail) finish("the tracker re-primed during the tail");
            else if (ensure_alloc()) {
                if (!g_first_live) g_first_live = step;
                rekey_from_shadow(step);
            }
        }
        if (g_core.tail_complete()) finish("the tail is complete");
        const size_t ram = g_core.ram_bytes();
        if (ram > g_peak_ram) g_peak_ram = ram;
        QueryPerformanceCounter(&t1);
        const int64_t own = t1.QuadPart - t0.QuadPart, upd = state_hub::last_ticks();
        g_own_ticks += own;
        g_upd_ticks += upd;
        if (own + upd > g_max_ticks) g_max_ticks = own + upd;
    }
};

listener_impl g_listener;

} // namespace

bool configure(const settings &s, bool recorder_on, uint64_t manifest_fp, bool (*live)(), log_fn log) {
    g_log  = log;
    g_set  = s;
    g_live = live;
    g_fp   = manifest_fp;
    g_on   = false;
    LARGE_INTEGER hz;
    g_qpc_hz = QueryPerformanceFrequency(&hz) ? hz.QuadPart : 0;
    g_total  = 0;
    for (int i = 0; i < N; ++i) {
        g_lens[i] = mh::state::HASH_REGIONS[i].len;
        g_total += g_lens[i];
    }
    if (!s.enabled) {
        GLOG("; [desync] STATE RING off: [desync] state_ring=0 -- a desync writes no mh_desync_state.bin\n");
        return false;
    }
    if (recorder_on) {
        GLOG("; [desync] STATE RING off: state_record=1 records the whole match into mh_match_state.bin, which "
             "already holds every step the ring would (a desync's run-up is in that file)\n");
        return false;
    }
    if (s.seconds < 1) {
        GLOG("; [desync] state_ring_s=%d refused (must be >= 1) -- using %d\n", s.seconds, settings{}.seconds);
        g_set.seconds = settings{}.seconds;
    }
    if (s.tail_s < 0) {
        GLOG("; [desync] state_ring_tail_s=%d refused (must be >= 0) -- using 0\n", s.tail_s);
        g_set.tail_s = 0;
    }
    if (s.max_kb < 64) {
        GLOG("; [desync] state_ring_max_kb=%d refused (must be >= 64) -- using %d\n", s.max_kb, settings{}.max_kb);
        g_set.max_kb = settings{}.max_kb;
    }
    g_cfg.keep_steps   = (uint32_t)g_set.seconds * sring::STEPS_PER_S + sring::DETECT_SLACK_STEPS;
    g_cfg.tail_steps   = (uint32_t)g_set.tail_s * sring::STEPS_PER_S;
    g_cfg.cap_bytes    = (uint32_t)g_set.max_kb * 1024u;
    g_cfg.tail_reserve = g_cfg.cap_bytes / 4u;
    g_on               = true;
    GLOG("; [desync] STATE RING armed: the last %d s of state (%lu steps incl. %lu of detection slack) before the "
         "first detected desync + %d s after it -> mh_desync_state.bin once per match (docs/state-record.md v%u, "
         "flags RING)\n",
         g_set.seconds, (unsigned long)g_cfg.keep_steps, (unsigned long)sring::DETECT_SLACK_STEPS, g_set.tail_s,
         (unsigned)srec::VERSION);
    GLOG("; [desync] STATE RING memory: %lu KB base image + %lu KB chunk cap (%lu KB run-up, %lu KB tail), allocated "
         "at the first live lockstep step; it wants the shared tracker only while the session is live lockstep\n",
         (unsigned long)(g_total / 1024), (unsigned long)(g_cfg.cap_bytes / 1024),
         (unsigned long)((g_cfg.cap_bytes - g_cfg.tail_reserve) / 1024), (unsigned long)(g_cfg.tail_reserve / 1024));
    return true;
}

bool                 enabled() { return g_on; }
state_hub::listener *listener() { return &g_listener; }

void session_start() {
    if (!g_on) return;
    if (g_core.ph() == sring::phase::tail) finish("a new session began during the tail");
    g_core.clear();
    g_armed_match = state_hub::allocated();
    g_dumped      = false;
    g_step_open   = false;
    reset_counters();
    if (!g_armed_match) GLOG("; [desync] STATE RING not armed this match: the shared state tracker has no arena\n");
}

bool on_first_mismatch(uint32_t mismatch_step) {
    if (!g_on || !g_armed_match) return false;
    if (g_dumped) return false; // once per match: a later mismatch writes nothing more
    g_dumped = true;
    if (g_core.ph() != sring::phase::rolling) {
        GLOG("; [desync] STATE RING has nothing to dump for the mismatch at step %lu: no live step was recorded "
             "this match\n",
             (unsigned long)mismatch_step);
        return false;
    }
    const uint32_t newest = g_core.last_step(), oldest = g_core.base_step();
    g_core.trigger(mismatch_step);
    GLOG("; [desync] STATE RING TRIGGERED by the mismatch at step %lu (now %lu): holds steps %lu..%lu (%lu before "
         "the mismatch, state_ring_s wants %lu); recording %lu more step(s)\n",
         (unsigned long)mismatch_step, (unsigned long)newest, (unsigned long)oldest, (unsigned long)newest,
         (unsigned long)(mismatch_step > oldest ? mismatch_step - oldest : 0),
         (unsigned long)((uint32_t)g_set.seconds * sring::STEPS_PER_S), (unsigned long)g_cfg.tail_steps);
    if (g_core.tail_complete()) finish("tail length 0");
    return true;
}

void match_end() {
    if (!g_on) return;
    const bool in_tail = g_core.ph() == sring::phase::tail;
    const bool held    = g_core.ph() == sring::phase::rolling;
    if (in_tail) finish("the match ended during the tail");
    if (g_steps) {
        const double n = (double)g_steps;
        GLOG("; [desync] STATE RING match end: %s. %lu step(s) from step %lu, keyed %lu time(s); RAM peak %lu KB "
             "(cap %lu KB base + %lu KB chunks) | sim thread mean %.1f us/step (tracker update incl. run sinks %.1f "
             "+ ring %.1f), max %.0f us\n",
             g_dumped ? "a desync was dumped (lines above)" : "no desync, nothing written",
             (unsigned long)g_steps, (unsigned long)g_first_live, (unsigned long)g_rekeys,
             (unsigned long)(g_peak_ram / 1024), (unsigned long)(g_total / 1024),
             (unsigned long)(g_cfg.cap_bytes / 1024), us(g_upd_ticks + g_own_ticks) / n,
             us(g_upd_ticks) / n, us(g_own_ticks) / n, us(g_max_ticks));
        if (held)
            GLOG("; [desync] STATE RING at match end held steps %lu..%lu (%lu KB of the %lu KB run-up budget, peak %lu "
                 "KB; %lu chunk(s) evicted by bytes before age)\n",
                 (unsigned long)g_core.base_step(), (unsigned long)g_core.last_step(),
                 (unsigned long)(g_core.used() / 1024),
                 (unsigned long)((g_cfg.cap_bytes - g_cfg.tail_reserve) / 1024),
                 (unsigned long)(g_core.peak_used() / 1024), (unsigned long)g_core.budget_pops());
    }
    g_core.clear(); // keep the allocation for the next match
    g_armed_match = false;
    reset_counters();
}

const sring::core &core_view() { return g_core; }
bool               writer_idle() { return g_writers == 0; }

} // namespace mh::desync::state_ring
