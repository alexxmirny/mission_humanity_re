//
// desync/seed_sanity.cpp -- the off-thread range scan of every replay seed (seed_sanity.h).
//
#include "desync/seed_sanity.h"

#include <windows.h>

#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "addr/mh_structs.gen.h" // the record layouts (no addresses)
#include "mh_log_sink.h"

namespace mh::desync::sanity {
namespace {

using mh::game::mh_map_object_building;
using mh::game::mh_map_object_unit;
using mh::game::mh_map_object_unit_storage;

constexpr int PLAYERS = 8, BLDGS = 100, UNITS = 100, STORES = 25;
constexpr int POP_STRIDE = 0x34;
static_assert(sizeof(mh_map_object_building) == 0x111, "buildings stride");
static_assert(sizeof(mh_map_object_unit) == 0xe9, "units stride");
static_assert(sizeof(mh_map_object_unit_storage) == 0xf4, "unit_storage stride");
static_assert(sizeof(mh::game::mh_llm_strat_pop_stats) == POP_STRIDE, "pop_stats stride");

template <class T>
T rd(const uint8_t *p) {
    T v;
    memcpy(&v, p, sizeof(v));
    return v;
}

struct ctx_t {
    emit_fn emit;
    void   *ctx;
    int     n;
};

void out(ctx_t &c, const char *key, const char *fmt, ...) {
    char    text[200];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(text, sizeof(text), _TRUNCATE, fmt, ap);
    va_end(ap);
    ++c.n;
    if (c.emit) c.emit(c.ctx, key, text);
}

#define FINDING(field, ...)                                                         \
    do {                                                                            \
        char k_[64];                                                                \
        _snprintf_s(k_, sizeof(k_), _TRUNCATE, "%s:%d:%d:%s", region, p, i, field); \
        out(c, k_, __VA_ARGS__);                                                    \
    } while (0)

bool fits(const layout &, int64_t off, uint32_t len, size_t need) {
    return off >= 0 && (uint64_t)off + need <= len;
}

void scan_buildings(ctx_t &c, const uint8_t *base, const extras &X) {
    const char *region = "buildings";
    for (int p = 0; p < PLAYERS; ++p)
        for (int i = 0; i < BLDGS; ++i) {
            const uint8_t *r = base + ((size_t)p * BLDGS + i) * sizeof(mh_map_object_building);
            if (rd<uint16_t>(r + offsetof(mh_map_object_building, building_id)) == 0) continue;
            for (int s = 0; s < 4; ++s) {
                const int32_t v = rd<int32_t>(r + offsetof(mh_map_object_building, pip_level) + 4 * s);
                if (v < 0 || v > 4) {
                    char f[16];
                    _snprintf_s(f, sizeof(f), _TRUNCATE, "pip_level%d", s);
                    FINDING(f, "buildings p%d i%d id %u: pip_level[%d]=%d (0x%08X), legal 0..4", p, i,
                            rd<uint16_t>(r + offsetof(mh_map_object_building, building_id)), s, v, (unsigned)v);
                }
            }
            const int32_t pac = rd<int32_t>(r + offsetof(mh_map_object_building, pip_active_count));
            if (pac < 0 || pac > 16)
                FINDING("pip_active_count", "buildings p%d i%d: pip_active_count=%d (0x%08X), legal 0..16", p, i, pac,
                        (unsigned)pac);
            const uint16_t st = rd<uint16_t>(r + offsetof(mh_map_object_building, state));
            if (st >= 255 || (X.have_states && !X.bldg_state_ok[st]))
                FINDING("state", "buildings p%d i%d: state=%u has no handler", p, i, st);
            const uint8_t ss = rd<uint8_t>(r + offsetof(mh_map_object_building, shuttle_slot));
            if (ss >= 10) FINDING("shuttle_slot", "buildings p%d i%d: shuttle_slot=%u, legal 0..9", p, i, ss);
        }
}

// A packed owner|kind ref + slot index: owner nibble < 8, index < 100.
void ref_pair(ctx_t &c, const char *region, int p, int i, const char *field, uint32_t ref, int32_t idx) {
    if ((ref & 0xff) == 0) return;
    if ((ref & 0x0f) >= 8 || idx < 0 || idx >= 100)
        FINDING(field, "units p%d i%d: %s ref=0x%04X index=%d (owner < 8, index 0..99)", p, i, field, ref, idx);
}

void scan_units(ctx_t &c, const uint8_t *base, const extras &X) {
    const char *region = "units";
    using U            = mh_map_object_unit;
    for (int p = 0; p < PLAYERS; ++p)
        for (int i = 0; i < UNITS; ++i) {
            const uint8_t *r = base + ((size_t)p * UNITS + i) * sizeof(U);
            if (rd<uint16_t>(r + offsetof(U, unit_proto_id)) == 0) continue;
            const uint16_t st = rd<uint16_t>(r + offsetof(U, state));
            if (st >= 255 || (X.have_states && !X.unit_state_ok[st]))
                FINDING("state", "units p%d i%d: state=%u has no handler", p, i, st);
            const uint16_t od = rd<uint16_t>(r + offsetof(U, order));
            if (od >= 255) FINDING("order", "units p%d i%d: order=%u, legal < 255", p, i, od);
            ref_pair(c, region, p, i, "target", rd<uint16_t>(r + offsetof(U, target_ref)),
                     rd<int16_t>(r + offsetof(U, target_index)));
            ref_pair(c, region, p, i, "target2", rd<uint16_t>(r + offsetof(U, target2_ref)),
                     rd<int16_t>(r + offsetof(U, target2_index)));
            ref_pair(c, region, p, i, "passive_engage", rd<uint16_t>(r + offsetof(U, passive_engage_target_ref)),
                     rd<uint16_t>(r + offsetof(U, passive_engage_target_index)));
            ref_pair(c, region, p, i, "ai_contact", rd<uint16_t>(r + offsetof(U, ai_contact_ref)),
                     rd<uint16_t>(r + offsetof(U, ai_contact_index)));
            const uint8_t hs = rd<uint8_t>(r + offsetof(U, home_storage_slot));
            if (hs >= STORES && hs != 0xff)
                FINDING("home_storage_slot", "units p%d i%d: home_storage_slot=%u, legal 0..24", p, i, hs);
            const uint8_t ss = rd<uint8_t>(r + offsetof(U, shuttle_slot));
            if (ss >= 10 && ss != 0xff) FINDING("shuttle_slot", "units p%d i%d: shuttle_slot=%u, legal 0..9", p, i, ss);
            const uint8_t ps = rd<uint8_t>(r + offsetof(U, path_slot_id));
            if (ps >= 100 && ps != 0xff)
                FINDING("path_slot_id", "units p%d i%d: path_slot_id=%u, legal 0..99 or 0xff", p, i, ps);
            const uint8_t sw = rd<uint8_t>(r + offsetof(U, selected_weapon));
            if (sw > 4 && sw != 100) // 4 is seen in clean saves (2-almost: four player-0 units)
                FINDING("selected_weapon", "units p%d i%d: selected_weapon=%u, legal 0..4 or 100", p, i, sw);
            const uint16_t ag = rd<uint16_t>(r + offsetof(U, ai_group_index));
            if (ag != 0xffff && ag >= 0x400)
                FINDING("ai_group_index", "units p%d i%d: ai_group_index=0x%04X, legal < 0x400 or 0xffff", p, i, ag);
            const uint16_t an = rd<uint16_t>(r + offsetof(U, ai_group_next));
            const uint16_t ap = rd<uint16_t>(r + offsetof(U, ai_group_prev));
            if (an >= UNITS || ap >= UNITS)
                FINDING("ai_group_link", "units p%d i%d: ai_group_next=%u prev=%u, legal 0..99", p, i, an, ap);
        }
}

void scan_storage(ctx_t &c, const uint8_t *base) {
    const char *region = "unit_storage";
    using S            = mh_map_object_unit_storage;
    for (int p = 0; p < PLAYERS; ++p)
        for (int i = 0; i < STORES; ++i) {
            const uint8_t *r  = base + ((size_t)p * STORES + i) * sizeof(S);
            const int32_t  bi = rd<int32_t>(r + offsetof(S, b_index));
            if (bi < 0 || bi >= BLDGS) FINDING("b_index", "unit_storage p%d s%d: b_index=%d, legal 0..99", p, i, bi);
            const int32_t dc = rd<int32_t>(r + offsetof(S, docked_count));
            if (dc < 0 || dc > 50)
                FINDING("docked_count", "unit_storage p%d s%d: docked_count=%d, legal 0..50", p, i, dc);
            else
                for (int k = 0; k < dc; ++k) {
                    const int32_t u = rd<int32_t>(r + offsetof(S, docked_units) + 4 * k);
                    if (u < 0 || u >= UNITS) {
                        FINDING("docked_units", "unit_storage p%d s%d: docked_units[%d]=%d, legal 0..99", p, i, k, u);
                        break;
                    }
                }
            const int32_t dm = rd<int32_t>(r + offsetof(S, door_mutex_unit));
            if (dm < 0 || dm >= UNITS)
                FINDING("door_mutex_unit", "unit_storage p%d s%d: door_mutex_unit=%d, legal 0..99", p, i, dm);
            const int32_t dw = rd<int32_t>(r + offsetof(S, door_waiter_count));
            if (dw < 0 || dw > UNITS)
                FINDING("door_waiter_count", "unit_storage p%d s%d: door_waiter_count=%d, legal 0..100", p, i, dw);
            const int32_t oc = rd<int32_t>(r + offsetof(S, occupancy));
            if (oc < 0 || oc > 200)
                FINDING("occupancy", "unit_storage p%d s%d: occupancy=%d, legal 0..200", p, i, oc);
        }
}

void scan_pop(ctx_t &c, const uint8_t *pop) {
    const char *region = "pop_stats";
    using P            = mh::game::mh_llm_strat_pop_stats;
    for (int p = 0; p < PLAYERS; ++p) {
        const int      i     = 0;
        const uint8_t *r     = pop + p * POP_STRIDE;
        const int32_t  total = rd<int32_t>(r + offsetof(P, pop_total));
        const int32_t  emp   = rd<int32_t>(r + offsetof(P, workers_employed));
        const int32_t  fld   = rd<int32_t>(r + offsetof(P, human_in_field));
        const int32_t  idle  = rd<int32_t>(r + offsetof(P, human));
        if (total < 0 || emp < 0 || fld < 0 || idle < 0)
            FINDING("negative", "pop_stats p%d: a negative counter: total=%d employed=%d in_field=%d idle=%d", p,
                    total, emp, fld, idle);
        else if (idle > total)
            FINDING("idle_gt_total", "pop_stats p%d: idle %d > total %d (employed %d, in_field %d)", p, idle, total,
                    emp, fld);
    }
}

#undef FINDING

// ---- the async half ------------------------------------------------------------------------------

log_fn        g_log        = nullptr;
bool          g_on         = false;
volatile LONG g_active     = 0;
SRWLOCK       g_scan_lock  = SRWLOCK_INIT; // one scan at a time (the seen-key set is shared)
int           g_last_n     = 0;
int           g_last_new   = 0;
unsigned long g_seen_gen   = 0;
constexpr int SEEN_MAX     = 1024;
constexpr int NEW_LINE_MAX = 40; // per scan; the summary still counts the rest
struct seen_t {
    char key[64];
    int  segment;
};
seen_t g_seen[SEEN_MAX];
int    g_seen_n = 0;

struct job {
    uint8_t      *blob;
    uint32_t      len;
    layout        L;
    extras        X;
    unsigned long gen;
    int           segment;
    char          label[32];
    uint32_t      step;
    char          seg_path[MAX_PATH];
    int           n_new;
    int           n_lines;
};

void put_line(const job &j, const char *line) {
    if (g_log) g_log("; [desync] %s", line);
    if (j.seg_path[0]) mh_logq_write_bin(j.seg_path, line, lstrlenA(line));
}

void on_finding(void *ctx, const char *key, const char *text) {
    job &j = *static_cast<job *>(ctx);
    for (int s = 0; s < g_seen_n; ++s)
        if (lstrcmpA(g_seen[s].key, key) == 0) return; // already reported by an earlier seed
    if (g_seen_n < SEEN_MAX) {
        lstrcpynA(g_seen[g_seen_n].key, key, sizeof(g_seen[0].key));
        g_seen[g_seen_n].segment = j.segment;
        ++g_seen_n;
    }
    ++j.n_new;
    if (j.n_lines >= NEW_LINE_MAX) return;
    ++j.n_lines;
    char line[320];
    _snprintf_s(line, sizeof(line), _TRUNCATE, "sanity NEW seg=%d step=%lu boundary=%s: %s\n", j.segment,
                (unsigned long)j.step, j.label, text);
    put_line(j, line);
}

DWORD WINAPI scan_main(void *arg) {
    job *j = static_cast<job *>(arg);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    AcquireSRWLockExclusive(&g_scan_lock);
    if (j->gen != g_seen_gen) { // a new session: nothing has been seen yet
        g_seen_gen = j->gen;
        g_seen_n   = 0;
    }
    LARGE_INTEGER t0, t1, hz;
    QueryPerformanceCounter(&t0);
    const int n = scan(j->blob, j->len, j->L, j->X, &on_finding, j);
    QueryPerformanceCounter(&t1);
    const double ms = QueryPerformanceFrequency(&hz) && hz.QuadPart
                          ? (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)hz.QuadPart
                          : 0.0;
    g_last_n        = n;
    g_last_new      = j->n_new;
    char line[320];
    _snprintf_s(line, sizeof(line), _TRUNCATE,
                "sanity seg=%d step=%lu boundary=%s: %d finding(s), %d new%s; scanned buildings/units/unit_storage%s "
                "in %.2f ms off the game thread\n",
                j->segment, (unsigned long)j->step, j->label, n, j->n_new,
                j->n_new > j->n_lines ? " (new lines capped)" : "", j->X.have_pop ? "/pop_stats" : "", ms);
    put_line(*j, line);
    ReleaseSRWLockExclusive(&g_scan_lock);
    HeapFree(GetProcessHeap(), 0, j->blob);
    free(j);
    InterlockedDecrement(&g_active);
    return 0;
}

} // namespace

int scan(const uint8_t *blob, uint32_t len, const layout &L, const extras &X, emit_fn emit, void *ctx) {
    ctx_t c = {emit, ctx, 0};
    if (fits(L, L.buildings, len, sizeof(mh_map_object_building) * PLAYERS * BLDGS))
        scan_buildings(c, blob + L.buildings, X);
    if (fits(L, L.units, len, sizeof(mh_map_object_unit) * PLAYERS * UNITS)) scan_units(c, blob + L.units, X);
    if (fits(L, L.unit_storage, len, sizeof(mh_map_object_unit_storage) * PLAYERS * STORES))
        scan_storage(c, blob + L.unit_storage);
    if (X.have_pop) scan_pop(c, X.pop);
    return c.n;
}

void configure(int on, log_fn log) {
    g_on  = on != 0;
    g_log = log;
}
bool enabled() { return g_on; }

void scan_async(uint8_t *blob, uint32_t len, const layout &L, const extras &X, unsigned long gen, int segment,
                const char *label, uint32_t step, const char *segments_path) {
    job *j = static_cast<job *>(malloc(sizeof(job)));
    if (!j) {
        HeapFree(GetProcessHeap(), 0, blob);
        return;
    }
    j->blob    = blob;
    j->len     = len;
    j->L       = L;
    j->X       = X;
    j->gen     = gen;
    j->segment = segment;
    lstrcpynA(j->label, label ? label : "?", sizeof(j->label));
    j->step = step;
    lstrcpynA(j->seg_path, segments_path ? segments_path : "", sizeof(j->seg_path));
    j->n_new   = 0;
    j->n_lines = 0;
    InterlockedIncrement(&g_active);
    HANDLE t = CreateThread(nullptr, 0, scan_main, j, 0, nullptr);
    if (!t) {
        InterlockedDecrement(&g_active);
        if (g_log) g_log("; [desync] sanity scan thread not started (err %lu): seed %d not scanned\n", GetLastError(), segment);
        HeapFree(GetProcessHeap(), 0, blob);
        free(j);
        return;
    }
    CloseHandle(t);
}

bool wait_idle(unsigned ms) {
    const DWORD t0 = GetTickCount();
    while (g_active != 0) {
        if (GetTickCount() - t0 > ms) return false;
        Sleep(2);
    }
    return true;
}
int last_findings() { return g_last_n; }
int last_new() { return g_last_new; }

} // namespace mh::desync::sanity
