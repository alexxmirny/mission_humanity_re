//
// desync/state_tracker.cpp -- the shared per-step tracker (state_tracker.h).
//
#include "desync/state_tracker.h"

#include <windows.h>

namespace mh::desync::state_hub {
namespace {

void                   *g_arena = nullptr;
mh::state::inc::tracker g_tr;
bool                    g_primed_state = false; // the tracker holds a shadow that is one step old
bool                    g_last_primed  = false;
int64_t                 g_last_ticks   = 0;
listener               *g_ls[MAX_LISTENERS];
int                     g_nls = 0;

struct fanout {
    listener *const *ls;
    int              n;
    void             run(int r, uint32_t off, uint32_t len, const uint8_t *b) {
        for (int i = 0; i < n; ++i) ls[i]->run(r, off, len, b);
    }
    void rebased(int r) {
        for (int i = 0; i < n; ++i) ls[i]->rebased(r);
    }
};

} // namespace

bool enable(const char *who, void (*log)(const char *fmt, ...)) {
    if (g_arena) {
        if (log) log("; [desync] STATE TRACKER shared with %s (one shadow, one update per step)\n", who);
        return true;
    }
    const size_t n = mh::state::inc::tracker::arena_bytes();
    g_arena        = VirtualAlloc(nullptr, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_arena) {
        if (log)
            log("; [desync] STATE TRACKER NOT allocated for %s: cannot allocate the %lu-byte arena (err %lu)\n", who,
                (unsigned long)n, GetLastError());
        return false;
    }
    // The SAME masks the in-band verdict uses (mask_ctrl_group=1, mask_soldier_anim=1,
    // mask_planets_gfx=1: hash_slice's defaults and the shipped harness's).
    g_tr.attach(g_arena, mh::state::inc::knobs{});
    g_primed_state = false;
    if (log)
        log("; [desync] STATE TRACKER allocated for %s: %lu-byte arena (shadow + 64-B block hashes of %d "
            "slices, HASH_KIND %lu), updated once per sim step and shared by every consumer\n",
            who, (unsigned long)n, mh::state::HASH_REGION_COUNT, (unsigned long)mh::state::inc::HASH_KIND);
    return true;
}

bool   allocated() { return g_arena != nullptr; }
size_t arena_bytes() { return g_arena ? mh::state::inc::tracker::arena_bytes() : 0; }

void add_listener(listener *l) {
    for (int i = 0; i < g_nls; ++i)
        if (g_ls[i] == l) return;
    if (g_nls < MAX_LISTENERS) g_ls[g_nls++] = l;
}

void set_journal(mh::state::inc::journal *j) { g_tr.set_journal(j); }

void reset() { g_primed_state = false; }

bool step(uint32_t s) {
    g_last_primed = false;
    g_last_ticks  = 0;
    if (!g_arena) return false;
    listener *act[MAX_LISTENERS];
    int       n = 0;
    for (int i = 0; i < g_nls; ++i)
        if (g_ls[i]->wants_step()) act[n++] = g_ls[i];
    if (n == 0) {
        g_primed_state = false; // skipped: the next update would be a multi-step delta
        return false;
    }
    const bool prime = !g_primed_state;
    for (int i = 0; i < n; ++i) act[i]->step_begin(s, prime);
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    if (prime) {
        g_tr.prime();
    } else {
        fanout f{act, n};
        g_tr.update(f, 0); // gap 0: EXACT runs
    }
    QueryPerformanceCounter(&t1);
    g_last_ticks   = t1.QuadPart - t0.QuadPart;
    g_primed_state = true;
    g_last_primed  = prime;
    for (int i = 0; i < n; ++i) act[i]->step_end(s, prime);
    return true;
}

const mh::state::inc::tracker &tracker() { return g_tr; }
bool                           last_primed() { return g_last_primed; }
int64_t                        last_ticks() { return g_last_ticks; }

} // namespace mh::desync::state_hub
