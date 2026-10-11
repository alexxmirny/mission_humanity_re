//
// desync/order_capture.cpp -- mp:Q3, mh.dll's own order recording (order_capture.h).
//
#include "desync/order_capture.h"
#include "desync/hitch_probe.h" // mp:Q4: game-thread time per recording path

#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "desync/state_compress.h"
#include "mh_log_sink.h" // the async sink: reliable binary records, no I/O on this thread

namespace mh::desync::orders {
namespace {

log_fn g_log      = nullptr;
bool   g_on       = false;
bool   g_compress = true;

#define OLOG(...)                      \
    do {                               \
        if (g_log) g_log(__VA_ARGS__); \
    } while (0)

// ---- the open file -----------------------------------------------------------------------------
bool          g_open       = false;
unsigned long g_gen        = 0; // the session-folder generation the open file belongs to
unsigned long g_closed_gen = 0; // the last generation closed: never reopened (a close in place)
char          g_orders[MAX_PATH];
char          g_clock[MAX_PATH];
uint32_t      g_step       = 0; // this file's step axis: 1 = the first step with the folder open
uint32_t      g_last_flush = 0;

// The batch: order records and clock doubles staged since the last hand-over.
record  *g_rec_buf = nullptr;
uint32_t g_rec_n = 0, g_rec_cap = 0;
uint64_t g_clk_buf[FLUSH_STEPS + 1];
uint32_t g_clk_n = 0;

// The previous step's snapshot, as sorted per-record hashes (the repeat count).
uint64_t g_prev[ORDER_QCAP], g_cur[ORDER_QCAP];
int      g_prev_n = 0, g_cur_n = 0;
uint32_t g_cur_step = 0; // the step g_cur describes (a second dispatch in one step replaces it)

stats   g_st     = {};
int64_t g_qpc_hz = 0;
bool    g_oom    = false;

uint64_t fnv(const uint8_t *p, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

void flush_batch() {
    if (g_rec_n) {
        mh_logq_write_bin(g_orders, g_rec_buf, (int)(g_rec_n * sizeof(record)));
        g_st.bytes += (uint64_t)g_rec_n * sizeof(record);
        g_rec_n = 0;
    }
    if (g_clk_n) {
        mh_logq_write_bin(g_clock, g_clk_buf, (int)(g_clk_n * sizeof(uint64_t)));
        g_clk_n = 0;
    }
    g_last_flush = g_step;
}

void close_file(const char *why) {
    if (!g_open) return;
    flush_batch();
    g_open              = false;
    g_st.open           = false;
    g_closed_gen        = g_gen;
    const long   ticket = mh_logq_ticket();
    const double steps  = g_st.steps ? (double)g_st.steps : 1.0;
    const double us     = g_qpc_hz ? (double)g_st.ticks * 1e6 / (double)g_qpc_hz : 0.0;
    OLOG("; [desync] ORDER RECORD closed (%s): %s -- %lu step(s), %lu dispatch call(s), %lu order record(s) (%.1f%% "
         "repeat the previous step's queue; MHOR v1 is a per-step snapshot, see order_capture.h), %llu bytes + %lu "
         "clock doubles, sim thread mean %.2f us/step%s\n",
         why, g_orders, (unsigned long)g_st.steps, (unsigned long)g_st.calls, (unsigned long)g_st.records,
         g_st.records ? 100.0 * (double)g_st.repeats / (double)g_st.records : 0.0, (unsigned long long)g_st.bytes,
         (unsigned long)g_st.steps, us / steps, g_oom ? " -- A BATCH ALLOCATION FAILED, records were lost" : "");
    if (g_compress) {
        gz::compress_async(g_orders, g_log, ticket, "ORDER RECORD");
        gz::compress_async(g_clock, g_log, ticket, "ORDER RECORD clock");
    }
}

void open_file(const char *dir, unsigned long gen) {
    wsprintfA(g_orders, "%s%s", dir, ORDERS_NAME);
    wsprintfA(g_clock, "%s%s", dir, CLOCK_NAME);
    const struct {
        uint32_t magic, ver;
    } hdr = {MAGIC, VERSION};
    mh_logq_create_bin(g_orders, &hdr, (int)sizeof(hdr));
    mh_logq_create_bin(g_clock, nullptr, 0);
    g_open       = true;
    g_gen        = gen;
    g_step       = 0;
    g_last_flush = 0;
    g_rec_n = g_clk_n = 0;
    g_prev_n = g_cur_n   = 0;
    g_cur_step           = 0;
    const uint32_t files = g_st.files + 1;
    g_st                 = {};
    g_st.files           = files;
    g_st.open            = true;
    g_st.bytes           = sizeof(hdr);
    g_oom                = false;
    OLOG("; [desync] ORDER RECORD -> %s + %s (mp:Q3): MHOR v1 records {i32 step; u8 order[0x44]} of every step's "
         "due-now queue at dispatch entry + the pre-body game clock per step; step 1 = the first sim step with "
         "this session folder open (the harness segment's axis); batched to the log sink every %lu steps\n",
         g_orders, CLOCK_NAME, (unsigned long)FLUSH_STEPS);
}

bool reserve(uint32_t n) {
    if (g_rec_n + n <= g_rec_cap) return true;
    uint32_t cap = g_rec_cap ? g_rec_cap : 1024u;
    while (cap < g_rec_n + n) cap *= 2;
    record *p = static_cast<record *>(realloc(g_rec_buf, (size_t)cap * sizeof(record)));
    if (!p) return false;
    g_rec_buf = p;
    g_rec_cap = cap;
    return true;
}

} // namespace

bool configure(int enabled_key, const char *refused, log_fn log) {
    g_log = log;
    LARGE_INTEGER hz;
    g_qpc_hz = QueryPerformanceFrequency(&hz) ? hz.QuadPart : 0;
    g_on     = enabled_key != 0 && refused == nullptr;
    if (enabled_key && refused) OLOG("; [desync] ORDER RECORD off: %s\n", refused);
    return g_on;
}

bool  enabled() { return g_on; }
void  set_compress(bool on) { g_compress = on; }
stats current() { return g_st; }

void step(const char *session_dir, unsigned long gen, bool settled, uint64_t clock_bits) {
    if (!g_on) return;
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    if (settled && g_open && (session_dir == nullptr || gen != g_gen)) close_file("the session folder changed");
    if (settled && !g_open && session_dir != nullptr && gen != g_closed_gen) open_file(session_dir, gen);
    if (g_open) {
        // The hand-over comes FIRST, so a batch always holds WHOLE steps: every step it has a clock for,
        // it has that step's orders too (they were dispatched during the step, i.e. before this call).
        // A process killed between hand-overs (the rig's runner ends a match that way) therefore leaves
        // two files that end on the same step -- measured the other way round first: the clock of the
        // last flushed step was on disk and its orders were still in the batch.
        if (g_clk_n >= FLUSH_STEPS || g_step - g_last_flush >= FLUSH_STEPS ||
            (uint64_t)g_rec_n * sizeof(record) >= FLUSH_BYTES)
            flush_batch();
        ++g_step;
        ++g_st.steps;
        g_clk_buf[g_clk_n++] = clock_bits;
        // The previous step's snapshot becomes the comparison set for this one.
        if (g_cur_step != 0) {
            memcpy(g_prev, g_cur, sizeof(uint64_t) * (size_t)g_cur_n);
            g_prev_n = g_cur_n;
        } else {
            g_prev_n = 0;
        }
        g_cur_n    = 0;
        g_cur_step = 0;
    }
    QueryPerformanceCounter(&t1);
    g_st.ticks += t1.QuadPart - t0.QuadPart;
    hitch::add_ticks(hitch::ORD_STEP, t1.QuadPart - t0.QuadPart);
}

void dispatch(const uint8_t *queue, int count) {
    if (!g_on || !g_open || g_step == 0) return;
    ++g_st.calls;
    if (count <= 0 || count > ORDER_QCAP || queue == nullptr) return; // the harness skips these too
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    if (g_cur_step == g_step) {
        // A second dispatch in one step (not seen in practice): the harness records it again, so do we,
        // but the repeat count compares against the step before, not against ourselves.
        g_cur_n = 0;
    }
    g_cur_step = g_step;
    if (!reserve((uint32_t)count)) {
        g_oom = true;
    } else {
        for (int i = 0; i < count; ++i) {
            record &r = g_rec_buf[g_rec_n++];
            r.step    = (int32_t)g_step;
            memcpy(r.order, queue + (size_t)i * ORDER_SIZE, ORDER_SIZE);
            const uint64_t h = fnv(r.order, ORDER_SIZE);
            if (g_prev_n && std::binary_search(g_prev, g_prev + g_prev_n, h)) ++g_st.repeats;
            g_cur[g_cur_n++] = h;
        }
        std::sort(g_cur, g_cur + g_cur_n);
        g_st.records += (uint32_t)count;
    }
    QueryPerformanceCounter(&t1);
    g_st.ticks += t1.QuadPart - t0.QuadPart;
    hitch::add_ticks(hitch::ORD_DISPATCH, t1.QuadPart - t0.QuadPart);
}

void session_close(const char *why) {
    if (!g_on) return;
    hitch::scope hs(hitch::ORD_CLOSE);
    close_file(why ? why : "session end");
}

} // namespace mh::desync::orders
