//
// sim/rng_trace.cpp -- see sim/rng_trace.h.
//
#include "sim/rng_trace.h"

namespace mh::sim {
namespace {

// 64k draws covers a several-step window with room to spare; the window is meant to be a handful of
// steps around a known divergence, not a whole run. OVERFLOW IS RECORDED rather than wrapped: a ring
// that silently overwrote its start would make the "first differing index" meaningless, which is the
// one number this instrument exists to produce.
constexpr int   CAP = 65536;
rng_trace_entry g_buf[CAP];
int             g_n    = 0;
bool            g_over = false;
uint32_t        g_lo   = 1u;
uint32_t        g_hi   = 0u; // lo > hi == disarmed
uint32_t        g_step = 0u;
const void     *g_site = nullptr;
rng_trace_note  g_notes[CAP];
int             g_nn = 0;

} // namespace

void rng_trace_window(uint32_t lo, uint32_t hi) {
    g_lo   = lo;
    g_hi   = hi;
    g_n    = 0;
    g_nn   = 0;
    g_over = false;
}

void rng_trace_set_site(const void *site) {
    g_site = site;
}

void rng_trace_set_step(uint32_t step) {
    g_step = step;
}

void rng_trace_record(int32_t channel, uint32_t after, const void *ra) {
    if (g_step < g_lo || g_step > g_hi) return;
    if (g_n >= CAP) {
        g_over = true;
        return;
    }
    rng_trace_entry &e = g_buf[g_n++];
    e.step             = g_step;
    e.channel          = channel;
    e.after            = after;
    e.ra               = ra;
    e.site             = g_site;
}

void rng_trace_add_note(uint32_t tag, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e,
                        uint32_t f) {
    if (g_step < g_lo || g_step > g_hi) return;
    if (g_nn >= CAP) {
        g_over = true;
        return;
    }
    rng_trace_note &n = g_notes[g_nn++];
    n.step            = g_step;
    n.tag             = tag;
    n.a               = a;
    n.b               = b;
    n.c               = c;
    n.d               = d;
    n.e               = e;
    n.f               = f;
}

int rng_trace_note_count() {
    return g_nn;
}

const rng_trace_note &rng_trace_note_at(int i) {
    return g_notes[i];
}

int rng_trace_count() {
    return g_n;
}

const rng_trace_entry &rng_trace_at(int i) {
    return g_buf[i];
}

bool rng_trace_overflowed() {
    return g_over;
}

} // namespace mh::sim
