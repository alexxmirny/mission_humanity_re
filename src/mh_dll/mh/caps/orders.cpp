//
// caps/orders.cpp -- the order layer of the cap raise (see orders.h). patching:PT-CAP9-U5.
//
#include "caps/orders.h"
#include "caps/orders_layout.gen.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "patch/inmem_patch.h"

namespace mh::caps::orders {
namespace {

namespace L = orders_layout;

struct geom_t {
    uint8_t *stock;
    int32_t *count;
};

geom_t g_geom[N] = {
    {reinterpret_cast<uint8_t *>(static_cast<uintptr_t>(L::ARRAYS[QUEUE].stock_base)), reinterpret_cast<int32_t *>(static_cast<uintptr_t>(L::ARRAYS[QUEUE].count_var))},
    {reinterpret_cast<uint8_t *>(static_cast<uintptr_t>(L::ARRAYS[PENDING].stock_base)), reinterpret_cast<int32_t *>(static_cast<uintptr_t>(L::ARRAYS[PENDING].count_var))},
    {reinterpret_cast<uint8_t *>(static_cast<uintptr_t>(L::ARRAYS[STAGING].stock_base)), reinterpret_cast<int32_t *>(static_cast<uintptr_t>(L::ARRAYS[STAGING].count_var))},
};

mh::patch::toggle_state g_ts;
uint8_t                *g_o             = nullptr; // .mhcapo
bool                    g_on            = false;
uint32_t                g_hw[N]         = {};
uint32_t                g_guard         = 0;
uint32_t                g_steps         = 0;
uint32_t                g_notice_step   = 0xFFFFFFFFu;
uint32_t                g_refuse_logged = 0;
bool                    g_over_logged   = false;
// the figures the last census line printed (a new line only when one moved)
uint32_t g_last[8]   = {};
bool     g_have_last = false;
// PT-CAP9-U7c: the counts scatter() clamped (0 = not clamped), and what the MHOV apply brought back since: records imported and the highest index + 1.
int32_t  g_pre[3]     = {};
uint32_t g_imp_n[3]   = {};
uint32_t g_imp_hi[3]  = {};
bool     g_unfinished = false;

void (*g_notice)(unsigned, bool)     = nullptr;
bool (*g_rebind)(uint32_t, uint32_t) = nullptr;
quota_state g_qs                     = {};

void (*g_sink)(const char *) = nullptr;

void olog(const char *fmt, ...) {
    if (!g_sink) return;
    char    b[420];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(b, sizeof(b), _TRUNCATE, fmt, ap);
    va_end(ap);
    g_sink(b);
}

volatile uint32_t &cell(uint32_t off) { return *reinterpret_cast<volatile uint32_t *>(g_o + off); }
uint8_t           *wide_ptr(which w) { return g_o + L::ARRAYS[w].off; }
uint32_t           stock_n(which w) { return L::ARRAYS[w].stock_count; }
uint32_t           wide_n(which w) { return L::ARRAYS[w].count; }

bool rec_nonzero(const uint8_t *r) {
    for (uint32_t i = 0; i < REC_BYTES; i += 4) {
        uint32_t v;
        memcpy(&v, r + i, 4);
        if (v) return true;
    }
    return false;
}

void write_guards() {
    for (unsigned i = 0; i < N; ++i) memset(wide_ptr(static_cast<which>(i)) + L::ARRAYS[i].bytes, L::GUARD_FILL, L::GUARD_BYTES);
}

} // namespace

void set_log_sink(void (*fn)(const char *)) { g_sink = fn; }

bool     staged() { return g_o != nullptr && g_ts.staged; }
bool     on() { return g_on && g_o != nullptr; }
uint32_t stock_cap(which w) { return stock_n(w); }
uint32_t wide_cap(which w) { return wide_n(w); }
uint8_t *base(which w) { return on() ? wide_ptr(w) : g_geom[w].stock; }
uint32_t cap(which w) { return on() ? wide_n(w) : stock_n(w); }

mh::patch::toggle_state &state() { return g_ts; }

void set_notice_hook(void (*fn)(unsigned, bool)) { g_notice = fn; }
void set_local_rebind(bool (*fn)(uint32_t, uint32_t)) { g_rebind = fn; }
void set_stock_for_test(which w, uint8_t *stock, int32_t *count_var) {
    g_geom[w].stock = stock ? stock : reinterpret_cast<uint8_t *>(static_cast<uintptr_t>(L::ARRAYS[w].stock_base));
    g_geom[w].count = count_var ? count_var : reinterpret_cast<int32_t *>(static_cast<uintptr_t>(L::ARRAYS[w].count_var));
}
void set_on_for_test(bool v) { g_on = v; }
void reset_for_test() {
    g_o  = nullptr;
    g_on = false;
    memset(g_hw, 0, sizeof(g_hw));
    g_guard         = 0;
    g_steps         = 0;
    g_notice_step   = 0xFFFFFFFFu;
    g_refuse_logged = 0;
    g_over_logged   = false;
    g_have_last     = false;
    memset(g_last, 0, sizeof(g_last));
    memset(&g_qs, 0, sizeof(g_qs));
    memset(&g_ts, 0, sizeof(g_ts));
    g_notice = nullptr;
    g_rebind = nullptr;
    memset(g_pre, 0, sizeof(g_pre));
    memset(g_imp_n, 0, sizeof(g_imp_n));
    memset(g_imp_hi, 0, sizeof(g_imp_hi));
    g_unfinished = false;
    for (unsigned i = 0; i < N; ++i) set_stock_for_test(static_cast<which>(i), nullptr, nullptr);
}

// The quota stub (below) is what the .mhcapr thunk jumps to; defined further down.
extern "C" void mh_orders_quota_stub();

// TEST KNOB (`[caps] orders_release_cap`, probe (d) of PT-CAP9-U5): the release bound of llm_strat_order_release_due (`cmp [queue count], 4096` at
// 0x004665db, immediate at 0x004665e1) lowered to N so a modest burst makes more records DUE than the queue will take, and the carry-over (the
// records left in PENDING for the next step) is exercised on every peer. 0 = off. Re-applied on every ON toggle; the OFF toggle restores 0x12c.
static uint32_t g_release_cap = 0;
void            set_release_cap_for_test(uint32_t n) { g_release_cap = n; }
static void     apply_release_cap() {
    if (g_release_cap == 0 || g_release_cap > wide_n(QUEUE)) return;
    uint8_t *const site = reinterpret_cast<uint8_t *>(static_cast<uintptr_t>(0x004665e1u));
    DWORD          old  = 0;
    if (!VirtualProtect(site, 4, PAGE_EXECUTE_READWRITE, &old)) return;
    memcpy(site, &g_release_cap, 4);
    VirtualProtect(site, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 4);
    olog("; [caps] orders: TEST release cap %u (probe d): llm_strat_order_release_due stops moving records into the queue at %u\n", g_release_cap, g_release_cap);
}

bool adopt(uint32_t o_base) {
    if (!o_base) return false;
    g_o = reinterpret_cast<uint8_t *>(static_cast<uintptr_t>(o_base));
    write_guards();
    cell(L::CELL_THUNK_CELL) = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&mh_orders_quota_stub));
    return true;
}

static void copy_to_wide() {
    for (unsigned i = 0; i < N; ++i) {
        const which w = static_cast<which>(i);
        memset(wide_ptr(w), 0, L::ARRAYS[i].bytes);
        memcpy(wide_ptr(w), g_geom[i].stock, static_cast<size_t>(stock_n(w)) * REC_BYTES);
    }
}

bool toggle(bool want_on, const char *why) {
    const char *who = why ? why : "?";
    if (want_on == g_on) return true;
    if (!staged()) {
        olog("; [caps] orders layer %s REFUSED (%s): not staged\n", want_on ? "ON" : "OFF", who);
        return false;
    }
    int         bad = -1;
    const char *w   = nullptr;
    if (!mh::patch::toggle(g_ts, want_on, &bad, &w)) {
        olog("; [caps] orders layer %s REFUSED (%s): %s\n", want_on ? "ON" : "OFF", who, w ? w : "toggle refused");
        return false;
    }
    const uint32_t stock_p = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_geom[PENDING].stock));
    const uint32_t wide_p  = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(wide_ptr(PENDING)));
    if (want_on) {
        // The stock arrays hold whatever the world has (idle at a quiescent point, but the counts may be non-zero): carry it over.
        copy_to_wide();
        write_guards();
        memset(g_hw, 0, sizeof(g_hw));
        cell(L::CELL_OVF_QUEUE) = cell(L::CELL_OVF_PENDING) = cell(L::CELL_OVF_STAGING) = cell(L::CELL_OVF_QUOTA) = 0;
        g_guard = g_refuse_logged = 0;
        g_have_last               = false;
        memset(&g_qs, 0, sizeof(g_qs));
        if (g_rebind && !g_rebind(stock_p, wide_p)) {
            (void)mh::patch::toggle(g_ts, false, nullptr, nullptr);
            olog("; [caps] orders layer ON REFUSED (%s): the O1c local-decode carrier could not be re-aimed at the wide pending array\n", who);
            return false;
        }
        g_on = true;
        apply_release_cap();
    } else {
        // Back to the stock arrays: whatever the wide ones hold past the stock window cannot be carried and is dropped (logged).
        for (unsigned i = 0; i < N; ++i) {
            const which w = static_cast<which>(i);
            if (*g_geom[i].count > static_cast<int32_t>(stock_n(w))) {
                olog("; [caps] orders layer OFF (%s): %s count %d is past the vanilla %u -- the records beyond it are DROPPED\n", who, L::ARRAYS[i].name,
                     *g_geom[i].count, stock_n(w));
                *g_geom[i].count = static_cast<int32_t>(stock_n(w));
            }
            memcpy(g_geom[i].stock, wide_ptr(w), static_cast<size_t>(stock_n(w)) * REC_BYTES);
        }
        if (g_rebind && !g_rebind(wide_p, stock_p)) {
            (void)mh::patch::toggle(g_ts, true, nullptr, nullptr);
            olog("; [caps] orders layer OFF REFUSED (%s): the O1c local-decode carrier could not be re-aimed at the stock pending array\n", who);
            return false;
        }
        g_on = false;
    }
    olog("; [caps] orders layer %s (%s): %d sites -> .mhcapo %08X; queue %u pending %u staging %u; sender quota %u records/player\n", want_on ? "ON" : "OFF", who,
         g_ts.m ? g_ts.m->site_count : 0, static_cast<unsigned>(reinterpret_cast<uintptr_t>(g_o)), cap(QUEUE), cap(PENDING), cap(STAGING), QUOTA);
    return true;
}

// ---- the canonical mirror --------------------------------------------------------------------------------------------
void gather() {
    if (!on()) return;
    for (unsigned i = 0; i < N; ++i) memcpy(g_geom[i].stock, wide_ptr(static_cast<which>(i)), static_cast<size_t>(stock_n(static_cast<which>(i))) * REC_BYTES);
    const int32_t q = *g_geom[QUEUE].count;
    if (q > static_cast<int32_t>(stock_n(QUEUE))) {
        if (!g_over_logged) {
            g_over_logged = true;
            olog("; [caps] orders: queue count %d is past the vanilla window (%u) -- a save taken now carries records %u..%d in the MHOV trailer (kind 10)\n", q, stock_n(QUEUE),
                 stock_n(QUEUE), q - 1);
        }
    } else {
        g_over_logged = false;
    }
}

void scatter() {
    finish_scatter(); // a previous scatter nobody finished: its clamp stands, logged
    memset(g_imp_n, 0, sizeof(g_imp_n));
    memset(g_imp_hi, 0, sizeof(g_imp_hi));
    if (!on()) return;
    for (unsigned i = 0; i < N; ++i) {
        const which w = static_cast<which>(i);
        memcpy(wide_ptr(w), g_geom[i].stock, static_cast<size_t>(stock_n(w)) * REC_BYTES);
        memset(wide_ptr(w) + static_cast<size_t>(stock_n(w)) * REC_BYTES, 0, static_cast<size_t>(wide_n(w) - stock_n(w)) * REC_BYTES);
        if (*g_geom[i].count > static_cast<int32_t>(stock_n(w))) {
            g_pre[i]         = *g_geom[i].count; // kept for finish_scatter(): the codec may bring the tail back
            *g_geom[i].count = static_cast<int32_t>(stock_n(w));
            g_unfinished     = true;
        }
    }
}

void finish_scatter() {
    if (!g_unfinished) return;
    g_unfinished = false;
    for (unsigned i = 0; i < N; ++i) {
        const int32_t pre = g_pre[i];
        g_pre[i]          = 0;
        if (pre <= 0) continue;
        const which    w    = static_cast<which>(i);
        const uint32_t from = stock_n(w);
        // The tail is whole when the codec restored every record [stock, pre) -- contiguous from the window, none missing, none past the count.
        if (static_cast<uint32_t>(pre) <= wide_n(w) && g_imp_n[i] == static_cast<uint32_t>(pre) - from && g_imp_hi[i] == static_cast<uint32_t>(pre)) {
            *g_geom[i].count = pre;
            olog("; [caps] orders: %s count %d restored (records %u..%d came from the MHOV blob)\n", L::ARRAYS[i].name, pre, from, pre - 1);
        } else {
            olog("; [caps] orders: %s count %d is past the vanilla window %u and the tail did not come back (%u of %d records) -- clamped to %u, the rest is DROPPED\n",
                 L::ARRAYS[i].name, pre, from, g_imp_n[i], pre - static_cast<int32_t>(from), from);
        }
    }
}

uint8_t *wide_record(which w, uint32_t idx) {
    if (w >= N || !on() || idx >= wide_n(w)) return nullptr;
    return wide_ptr(w) + static_cast<size_t>(idx) * REC_BYTES;
}

uint32_t live_count(which w) {
    if (w >= N) return 0;
    const int32_t c = *g_geom[w].count;
    if (c <= 0) return 0;
    return static_cast<uint32_t>(c) > cap(w) ? cap(w) : static_cast<uint32_t>(c);
}

void import_record(which w, uint32_t idx, const uint8_t *rec) {
    uint8_t *d = wide_record(w, idx);
    if (!d) return;
    memcpy(d, rec, REC_BYTES);
    ++g_imp_n[w];
    if (idx + 1 > g_imp_hi[w]) g_imp_hi[w] = idx + 1;
}

// ---- census ------------------------------------------------------------------------------------------------------------
uint32_t high_water(which w) { return w < N ? g_hw[w] : 0; }
uint32_t refused(which w) {
    if (!g_o) return 0;
    return w == QUEUE ? cell(L::CELL_OVF_QUEUE) : w == PENDING ? cell(L::CELL_OVF_PENDING)
                                                               : cell(L::CELL_OVF_STAGING);
}
uint32_t refused_quota() { return g_o ? cell(L::CELL_OVF_QUOTA) : 0; }
uint32_t guard_hits() { return g_guard; }

void format_line(char *out, unsigned cap_bytes) {
    _snprintf_s(out, cap_bytes, _TRUNCATE, "; [caps] orders step=%u hw q=%u p=%u s=%u (of %u/%u/%u) ovf q=%u p=%u s=%u quota=%u cnt q=%d p=%d s=%d guard=0x%X\n", g_steps,
                g_hw[QUEUE], g_hw[PENDING], g_hw[STAGING], wide_n(QUEUE), wide_n(PENDING), wide_n(STAGING), refused(QUEUE), refused(PENDING), refused(STAGING), refused_quota(),
                *g_geom[QUEUE].count, *g_geom[PENDING].count, *g_geom[STAGING].count, g_guard);
}

void step() {
    if (!on()) return;
    ++g_steps;
    for (unsigned i = 0; i < N; ++i) {
        const uint8_t *gp = wide_ptr(static_cast<which>(i)) + L::ARRAYS[i].bytes;
        uint32_t       k  = 0;
        while (k < L::GUARD_BYTES && gp[k] == L::GUARD_FILL) ++k;
        if (k != L::GUARD_BYTES && !(g_guard & (1u << i))) {
            g_guard |= 1u << i;
            olog("; [caps] ORDERS GUARD HIT array=%s step=%u: guard byte +%u is 0x%02X -- something wrote past the enlarged %s (%u records)\n", L::ARRAYS[i].name, g_steps, k, gp[k],
                 L::ARRAYS[i].name, L::ARRAYS[i].count);
        }
    }
    // The high-water mark: records fill from 0 and every record an order writes has non-zero bytes (the args default to -1), so the highest
    // non-empty record is the largest the array ever held. The arrays are never shrunk, so the scan only ever moves forward.
    for (unsigned i = 0; i < N; ++i) {
        const which    w = static_cast<which>(i);
        uint32_t       h = g_hw[i];
        const uint32_t n = wide_n(w);
        const uint8_t *b = wide_ptr(w);
        while (h < n && rec_nonzero(b + static_cast<size_t>(h) * REC_BYTES)) ++h;
        g_hw[i] = h;
    }
    const uint32_t now[8] = {g_hw[QUEUE], g_hw[PENDING], g_hw[STAGING], refused(QUEUE), refused(PENDING), refused(STAGING), refused_quota(), g_guard};
    if (!g_have_last || memcmp(now, g_last, sizeof(now)) != 0) {
        // Only a change that matters is a line: the first step, a new high-water mark, a refusal. (A step with no order costs nothing.)
        if (g_have_last || now[0] || now[1] || now[2] || now[3] || now[4] || now[5]) {
            char b[300];
            format_line(b, sizeof(b));
            olog("%s", b);
        }
        memcpy(g_last, now, sizeof(now));
        g_have_last = true;
    }
}

// ---- the sender quota ---------------------------------------------------------------------------------------------------
bool quota_accept(quota_state &qs, unsigned owner, bool echo, uint32_t staging_count, uint32_t own_in_flight) {
    owner &= 15u;
    const uint32_t need = echo ? 1u : 2u; // a primary reserves room for its 0xfb echo
    bool           ok   = staging_count + need <= wide_n(STAGING) && own_in_flight + need <= QUOTA;
    if (echo) {
        if (qs.orphan[owner]) ok = false; // never an echo without its order
    } else {
        qs.orphan[owner] = !ok;
    }
    return ok;
}
void quota_reset_for_test() { memset(&g_qs, 0, sizeof(g_qs)); }

namespace {
uint32_t in_flight_of(unsigned owner) {
    uint32_t n = 0;
    for (unsigned i = PENDING; i <= STAGING; ++i) {
        const which    w   = static_cast<which>(i);
        const uint8_t *b   = base(w);
        int32_t        cnt = *g_geom[i].count;
        if (cnt > static_cast<int32_t>(cap(w))) cnt = static_cast<int32_t>(cap(w));
        for (int32_t k = 0; k < cnt; ++k) {
            uint16_t ok;
            memcpy(&ok, b + static_cast<size_t>(k) * REC_BYTES + 0xa, 2);
            if ((ok & 15u) == owner) ++n;
        }
    }
    return n;
}
} // namespace

// Called by the stub with the game's frame pointer inside llm_strat_order_stage_scheduled: [ebp-0x18] unit_index, [ebp-0x14] owner_and_kind,
// [ebp-0x10] param0, [ebp-0xc] order_code. -1 = stage it (the following `test eax,eax; jge refuse` falls through), 0 = refuse.
extern "C" int __cdecl mh_orders_quota_decide(const uint8_t *frame) {
    uint16_t ok, p0, code;
    memcpy(&ok, frame - 0x14, 2);
    memcpy(&p0, frame - 0x10, 2);
    memcpy(&code, frame - 0xc, 2);
    const unsigned owner = ok & 15u;
    const bool     echo  = (p0 & 0xff) == 0xfb && (code & 0xff) == 0xfb;
    const int32_t  sc    = *g_geom[STAGING].count;
    const uint32_t own   = in_flight_of(owner);
    if (quota_accept(g_qs, owner, echo, sc < 0 ? 0u : static_cast<uint32_t>(sc), own)) return -1;
    const bool by_quota       = own + (echo ? 1u : 2u) > QUOTA;
    cell(L::CELL_OVF_STAGING) = cell(L::CELL_OVF_STAGING) + 1;
    if (by_quota) cell(L::CELL_OVF_QUOTA) = cell(L::CELL_OVF_QUOTA) + 1;
    if (g_refuse_logged < 20 && g_notice_step != g_steps) {
        ++g_refuse_logged;
        olog("; [caps] orders: REFUSED at the sender (step %u, player %u): %s -- staging %d/%u, this player's in-flight records %u/%u%s\n", g_steps, owner,
             by_quota ? "per-player quota" : "staging full", sc, wide_n(STAGING), own, QUOTA, echo ? " (echo of a refused order)" : "");
    }
    if (g_notice_step != g_steps) { // at most once per click (one sim step)
        g_notice_step = g_steps;
        if (g_notice) g_notice(owner, by_quota);
    }
    return 0;
}

// clang-format off
extern "C" __declspec(naked) void mh_orders_quota_stub() {
    __asm {
        pushad
        pushfd
        push ebp
        call mh_orders_quota_decide
        add  esp, 4
        mov  dword ptr [esp + 32], eax          // pushad's EAX slot (above the pushfd word)
        popfd
        popad
        ret
    }
}
// clang-format on

} // namespace mh::caps::orders
