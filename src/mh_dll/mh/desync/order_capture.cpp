#include "desync/order_capture.h"
#include "desync/hitch_probe.h"
#include <windows.h>
#include <algorithm>
#include <cstring>
#include "desync/state_compress.h"
#include "mh_log_sink.h"

namespace mh::desync::orders {
namespace {
namespace stream    = mh::order_stream;
log_fn        g_log = nullptr;
bool          g_on = false, g_compress = true, g_open = false;
unsigned long g_gen = 0, g_closed_gen = 0;
char          g_orders[MAX_PATH], g_clock[MAX_PATH];
uint32_t      g_step       = 0;
uint64_t      g_clock_bits = 0;
// Fixed BSS storage: no allocation when disabled and no transient heap in simulation.
uint8_t  g_pair[2 * (stream::SAMPLE_HEADER_BYTES + ORDER_QCAP * ORDER_SIZE)];
size_t   g_pair_bytes    = 0;
bool     g_close_pending = false;
char     g_close_why[128];
bool     g_have_hash = false;
uint64_t g_prev[ORDER_QCAP], g_cur[ORDER_QCAP];
int      g_prev_n = 0;
stats    g_st     = {};
int64_t  g_qpc_hz = 0;
#define OLOG(...)                      \
    do {                               \
        if (g_log) g_log(__VA_ARGS__); \
    } while (0)

uint64_t fnv(const uint8_t *p, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}
void close_file(const char *why) {
    if (!g_open) return;
    g_open       = false;
    g_st.open    = false;
    g_closed_gen = g_gen;
    // Only complete pairs and their clocks were ever handed to the writer.
    g_have_hash         = false;
    g_pair_bytes        = 0;
    g_close_pending     = false;
    const long   ticket = mh_logq_ticket();
    const double steps  = g_st.steps ? double(g_st.steps) : 1.0;
    const double us     = g_qpc_hz ? double(g_st.ticks) * 1e6 / double(g_qpc_hz) : 0.0;
    OLOG("; [desync] ORDER RECORD closed (%s): %s -- %lu step(s), %lu ENTRY observation(s), %lu order record(s) "
         "(%.1f%% repeat the previous step's queue; MHOR v2 exact HASH/ENTRY), %llu bytes + %lu clock doubles, "
         "sim thread mean %.2f us/step\n",
         why, g_orders, (unsigned long)g_st.steps,
         (unsigned long)g_st.calls, (unsigned long)g_st.records,
         g_st.records ? 100.0 * double(g_st.repeats) / double(g_st.records) : 0.0,
         (unsigned long long)g_st.bytes, (unsigned long)g_st.steps, us / steps);
    if (g_compress) {
        gz::compress_async(g_orders, g_log, ticket, "ORDER RECORD");
        gz::compress_async(g_clock, g_log, ticket, "ORDER RECORD clock");
    }
}
void broken(const char *why) {
    OLOG("; [desync] ORDER RECORD FAILED at step %lu: %s; no incomplete pair published\n",
         (unsigned long)g_step, why);
    close_file(why);
}
void open_file(const char *dir, unsigned long gen) {
    wsprintfA(g_orders, "%s%s", dir, ORDERS_NAME);
    wsprintfA(g_clock, "%s%s", dir, CLOCK_NAME);
    uint8_t hdr[stream::HEADER_BYTES];
    stream::write_header(hdr);
    mh_logq_create_bin(g_orders, hdr, int(sizeof(hdr)));
    mh_logq_create_bin(g_clock, nullptr, 0);
    g_open               = true;
    g_gen                = gen;
    g_step               = 0;
    g_prev_n             = 0;
    g_pair_bytes         = 0;
    g_have_hash          = false;
    const uint32_t files = g_st.files + 1;
    g_st                 = {};
    g_st.files           = files;
    g_st.open            = true;
    g_st.bytes           = sizeof(hdr);
    OLOG("; [desync] ORDER RECORD -> %s + %s: MHOR v2 exact HASH/ENTRY (including empty); "
         "step 1 = first sim step in this session folder\n",
         g_orders, CLOCK_NAME);
}
bool valid_queue(const uint8_t *q, int count) {
    return count >= 0 && count <= ORDER_QCAP && (!count || q);
}
void append_sample(stream::phase phase, const uint8_t *q, int count) {
    stream::write_sample_header(g_pair + g_pair_bytes, g_step, phase, uint32_t(count));
    g_pair_bytes += stream::SAMPLE_HEADER_BYTES;
    if (count) memcpy(g_pair + g_pair_bytes, q, size_t(count) * ORDER_SIZE);
    g_pair_bytes += size_t(count) * ORDER_SIZE;
}
} // namespace

bool configure(int enabled_key, const char *refused, log_fn log) {
    g_log = log;
    LARGE_INTEGER hz;
    g_qpc_hz = QueryPerformanceFrequency(&hz) ? hz.QuadPart : 0;
    g_on     = enabled_key != 0 && !refused;
    if (enabled_key && refused) OLOG("; [desync] ORDER RECORD off: %s\n", refused);
    return g_on;
}
bool  enabled() { return g_on; }
void  set_compress(bool on) { g_compress = on; }
stats current() { return g_st; }

void step(const char *dir, unsigned long gen, bool settled) {
    if (!g_on) return;
    if (settled && g_open && (!dir || gen != g_gen)) close_file("the session folder changed");
    if (settled && !g_open && dir && gen != g_closed_gen) open_file(dir, gen);
    if (!g_open) return;
    if (g_have_hash) {
        broken("missing preceding ENTRY observation");
        return;
    }
    if (g_step == UINT32_MAX) {
        broken("step axis overflow");
        return;
    }
    ++g_step;
}
void hash(const uint8_t *q, int count, uint64_t clock_bits) {
    if (!g_on || !g_open || !g_step) return;
    if (g_have_hash || g_st.steps + 1 != g_step || !valid_queue(q, count)) {
        broken("invalid or duplicate HASH observation");
        return;
    }
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    g_clock_bits = clock_bits;
    g_pair_bytes = 0;
    append_sample(stream::phase::hash, q, count);
    g_have_hash = true;
    QueryPerformanceCounter(&t1);
    g_st.ticks += t1.QuadPart - t0.QuadPart;
    hitch::add_ticks(hitch::ORD_STEP, t1.QuadPart - t0.QuadPart);
}
void entry(const uint8_t *q, int count) {
    if (!g_on || !g_open || !g_step) return;
    if (!g_have_hash || !valid_queue(q, count)) {
        broken("invalid or unpaired ENTRY observation");
        return;
    }
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    append_sample(stream::phase::entry, q, count);
    // One reliable sink record owns a WHOLE pair, even when both counts are zero.
    mh_logq_write_bin(g_orders, g_pair, int(g_pair_bytes));
    mh_logq_write_bin(g_clock, &g_clock_bits, int(sizeof(g_clock_bits)));
    g_st.bytes += g_pair_bytes;
    ++g_st.calls;
    ++g_st.steps;
    g_st.records += uint32_t(count);
    for (int i = 0; i < count; ++i) {
        const uint64_t h = fnv(q + size_t(i) * ORDER_SIZE, ORDER_SIZE);
        if (g_prev_n && std::binary_search(g_prev, g_prev + g_prev_n, h)) ++g_st.repeats;
        g_cur[i] = h;
    }
    std::sort(g_cur, g_cur + count);
    if (count) memcpy(g_prev, g_cur, size_t(count) * sizeof(uint64_t));
    g_prev_n     = count;
    g_have_hash  = false;
    g_pair_bytes = 0;
    QueryPerformanceCounter(&t1);
    g_st.ticks += t1.QuadPart - t0.QuadPart;
    hitch::add_ticks(hitch::ORD_DISPATCH, t1.QuadPart - t0.QuadPart);
    if (g_close_pending) close_file(g_close_why);
}
void session_close(const char *why) {
    if (!g_on) return;
    hitch::scope hs(hitch::ORD_CLOSE);
    if (g_have_hash) {
        g_close_pending = true;
        lstrcpynA(g_close_why, why ? why : "session end", int(sizeof(g_close_why)));
    } else {
        close_file(why ? why : "session end");
    }
}
} // namespace mh::desync::orders
