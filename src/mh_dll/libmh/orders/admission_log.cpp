//
// orders/admission_log.cpp -- see admission_log.h. The taps live in order_queue.cpp
// (detail::pending_enqueue and detail::schedule); this file is the storage and the resync-side reader.
//
#include "orders/admission_log.h"

#include <cstring>

namespace mh::orders::admission {

namespace {

struct entry {
    uint32_t idx; // per-source ordinal, 1-based
    uint32_t src;
    order    rec; // the record as stored in ORDER_PENDING (0x44 bytes)
};

// ~300 KB of static storage: the whole memory cost of the feature. Not a registry region, so no
// determinism slice can see it and no bind moves it.
entry  g_ring[RING_CAP];
status g_st; // zero-initialised static; reset() re-zeroes it per match

} // namespace

void reset() {
    std::memset(&g_st, 0, sizeof(g_st));
    // The ring itself is not cleared: total == 0 makes every slot unreachable, and clearing 300 KB
    // per match would buy nothing.
}

void note(const order &stored) {
    if (g_st.muted) return;
    const int src = source_of(stored);
    entry    &e   = g_ring[g_st.total % RING_CAP];
    if (g_st.total >= RING_CAP) {
        // The slot's previous occupant falls off. Ordinals of a source only ever grow, so the last
        // one dropped is the highest.
        g_st.dropped_upto[e.src] = e.idx;
        ++g_st.ring_wraps;
    }
    e.idx = ++g_st.n_src[src];
    e.src = static_cast<uint32_t>(src);
    std::memcpy(&e.rec, &stored, sizeof(order));
    ++g_st.total;
}

void note_overflow() {
    ++g_st.overflow_resets;
    g_st.abort_worthy = true;
}

void set_muted(bool on) { g_st.muted = on; }

const status &current() { return g_st; }

void counts(uint32_t out[SOURCES]) { std::memcpy(out, g_st.n_src, sizeof(g_st.n_src)); }

int plan(const uint32_t n_src[SOURCES], plan_report *rep) {
    plan_report  local;
    plan_report &r = rep ? *rep : local;
    r.need         = 0;
    r.bad_src      = -1;
    if (g_st.abort_worthy) return ERR_ABORT;
    for (int s = 0; s < SOURCES; ++s) {
        if (n_src[s] > g_st.n_src[s]) {
            r.bad_src = s;
            return ERR_AHEAD;
        }
        // A needed ordinal is one in (n_src[s], count]. It is lost iff something >= n_src[s]+1 dropped.
        if (g_st.dropped_upto[s] > n_src[s]) {
            r.bad_src = s;
            return ERR_RING_SHORT;
        }
        r.need += g_st.n_src[s] - n_src[s];
    }
    return 0;
}

int readmit(const container_state &st, const uint32_t n_src[SOURCES], readmit_report *rep) {
    readmit_report  local;
    readmit_report &r = rep ? *rep : local;
    r.readmitted      = 0;
    r.pending_after   = static_cast<uint32_t>(*st.pending_count);

    const uint32_t first = g_st.total > RING_CAP ? g_st.total - RING_CAP : 0u; // oldest live sequence
    int            rc    = 0;
    for (uint32_t seq = first; seq < g_st.total; ++seq) {
        const entry &e = g_ring[seq % RING_CAP];
        if (e.idx <= n_src[e.src]) continue;
        if (*st.pending_count >= PENDING_CAP) {
            rc = ERR_PENDING_FULL;
            break;
        }
        std::memcpy(&st.pending[*st.pending_count], &e.rec, sizeof(order));
        *st.pending_count += 1;
        ++r.readmitted;
    }
    r.pending_after = static_cast<uint32_t>(*st.pending_count);
    return rc;
}

// ---- staged re-admission --------------------------------------------------------------------------

namespace {
struct stage_t {
    bool     active;
    uint32_t end_seq;        // ring sequence one past the last target record
    uint32_t low_seq;        // first target record not yet fed (everything below it is fed or not a target)
    uint32_t remaining;      // target records not yet fed
    uint32_t n_src[SOURCES]; // the host's counters: a target has ordinal > n_src[src]
};
stage_t g_stage;
uint8_t g_fed[(RING_CAP + 7) / 8]; // per ring slot: has this target been fed

inline bool fed_get(uint32_t seq) {
    const uint32_t k = seq % RING_CAP;
    return (g_fed[k >> 3] >> (k & 7u)) & 1u;
}
inline void fed_set(uint32_t seq) { g_fed[(seq % RING_CAP) >> 3] |= static_cast<uint8_t>(1u << ((seq % RING_CAP) & 7u)); }
} // namespace

int stage_begin(const uint32_t n_src[SOURCES], plan_report *rep) {
    std::memset(&g_stage, 0, sizeof(g_stage));
    const int rc = plan(n_src, rep);
    if (rc != 0) return rc;
    std::memset(g_fed, 0, sizeof(g_fed));
    std::memcpy(g_stage.n_src, n_src, sizeof(g_stage.n_src));
    g_stage.end_seq = g_st.total;
    g_stage.low_seq = g_st.total > RING_CAP ? g_st.total - RING_CAP : 0u;
    for (uint32_t seq = g_stage.low_seq; seq < g_stage.end_seq; ++seq) {
        const entry &e = g_ring[seq % RING_CAP];
        if (e.idx > n_src[e.src]) ++g_stage.remaining;
    }
    g_stage.active = true;
    // Skip the leading non-targets so low_seq sits on the first record still to feed.
    while (g_stage.low_seq < g_stage.end_seq && g_ring[g_stage.low_seq % RING_CAP].idx <= n_src[g_ring[g_stage.low_seq % RING_CAP].src])
        ++g_stage.low_seq;
    return 0;
}

int stage_feed(const container_state &st, double horizon, stage_report *rep) {
    stage_report  local;
    stage_report &r = rep ? *rep : local;
    r.fed           = 0;
    r.remaining     = g_stage.remaining;
    r.pending_after = static_cast<uint32_t>(*st.pending_count);
    if (!g_stage.active || g_stage.remaining == 0) return 0;
    if (g_st.abort_worthy) return ERR_ABORT;

    // Everything below the oldest live ring sequence is gone. low_seq sits on a target that is NOT yet
    // fed, so falling behind it means that record was lost before it could be delivered.
    const uint32_t oldest = g_st.total > RING_CAP ? g_st.total - RING_CAP : 0u;
    if (g_stage.low_seq < oldest) return ERR_RING_SHORT;

    int rc = 0;
    for (uint32_t seq = g_stage.low_seq; seq < g_stage.end_seq; ++seq) {
        const entry &e = g_ring[seq % RING_CAP];
        if (e.idx <= g_stage.n_src[e.src] || fed_get(seq)) continue;
        // `!(exec_time > horizon)`: release_due's own comparison, so NaN takes the same branch.
        if (e.rec.exec_time > horizon) continue;
        if (*st.pending_count >= PENDING_CAP) {
            rc = ERR_PENDING_FULL;
            break;
        }
        std::memcpy(&st.pending[*st.pending_count], &e.rec, sizeof(order));
        *st.pending_count += 1;
        fed_set(seq);
        --g_stage.remaining;
        ++r.fed;
    }
    while (g_stage.low_seq < g_stage.end_seq) {
        const entry &e = g_ring[g_stage.low_seq % RING_CAP];
        if (e.idx > g_stage.n_src[e.src] && !fed_get(g_stage.low_seq)) break;
        ++g_stage.low_seq;
    }
    r.remaining     = g_stage.remaining;
    r.pending_after = static_cast<uint32_t>(*st.pending_count);
    if (g_stage.remaining == 0) g_stage.active = false;
    return rc;
}

int stage_feed(double horizon, stage_report *rep) { return stage_feed(mh::orders::state(), horizon, rep); }

bool     stage_active() { return g_stage.active; }
uint32_t stage_remaining() { return g_stage.remaining; }
void     stage_cancel() { std::memset(&g_stage, 0, sizeof(g_stage)); }

} // namespace mh::orders::admission
