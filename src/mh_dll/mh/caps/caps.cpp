//
// caps/caps.cpp -- the roster-geometry table (see caps.h), plus (slice C) the relocated layout, the
// limit-layer switch and the canonical-mirror gather/scatter.
//
// Slice B shipped the table at STOCK values; slice C's installer (patch/caps_install.cpp) fills it from
// the relocation apply (adopt_relocation) and flips the limits (set_limits). With relocation off or
// refused nothing here is ever called and every converted hook reads today's values.
//
// The stock constants are NOT retyped from memory where a generated source exists: base comes from the
// address manifest (mh::addr::*), elem from sizeof() of the generated record structs, and the vanilla
// row / limit from roster_caps.h's STOCK_ROSTER_CAPS (the documented stock strides). capstest asserts
// the result against hand-computed VAs, so a manifest drift is a red test, not a silent shift.
//
#include "caps/caps.h"
#include "caps/ai_scratch_layout.gen.h"
#include "caps/orders.h"
#include "caps/path_pool.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "patch/inmem_patch.h"

#include "addr/mh_addrs.gen.h"
#include "addr/mh_structs.gen.h"
#include "state/roster_caps.h"

namespace mh::caps {
namespace {

using namespace mh::game;

constexpr uint32_t P = static_cast<uint32_t>(mh::state::ROSTER_PLAYERS);
constexpr auto     S = mh::state::STOCK_ROSTER_CAPS;

constexpr uint32_t PROJECTILE_POOL_BASE = 0x00e1db98u; // _G_LLM_STRAT_PROJECTILE_POOL (RID_STRAT_PROJECTILE_POOL)

constexpr geom STOCK[MH_CAPS_COUNT] = {
    /* BUILDINGS    */ {static_cast<uint32_t>(mh::addr::buildings), sizeof(mh_map_object_building), (uint32_t)S.buildings, (uint32_t)S.buildings, P},
    /* UNIT_STORAGE */ {static_cast<uint32_t>(mh::addr::unit_storage), sizeof(mh_map_object_unit_storage), (uint32_t)S.storage, (uint32_t)S.storage, P},
    /* TURRETS      */ {static_cast<uint32_t>(mh::addr::turrets), sizeof(mh_map_object_turret), (uint32_t)S.turrets, (uint32_t)S.turrets, P},
    /* MINES        */ {static_cast<uint32_t>(mh::addr::mines), sizeof(mh_map_object_mine), (uint32_t)S.mines, (uint32_t)S.mines, P},
    /* LABS         */ {static_cast<uint32_t>(mh::addr::labs), sizeof(mh_map_object_lab), (uint32_t)S.labs, (uint32_t)S.labs, P},
    /* PRODUCTIONS  */ {static_cast<uint32_t>(mh::addr::productions), sizeof(mh_map_object_production), (uint32_t)S.productions, (uint32_t)S.productions, P},
    /* PROJECTILES  */ {PROJECTILE_POOL_BASE, sizeof(mh_llm_strat_projectile), 1500u, 1500u, 1u},
    /* UNITS        */ {static_cast<uint32_t>(mh::addr::units), sizeof(mh_map_object_unit), (uint32_t)S.units, (uint32_t)S.units, P},
    /* SOLDIERS     */ {static_cast<uint32_t>(mh::addr::_G_LLM_STRAT_SOLDIERS), sizeof(mh_llm_strat_crew_soldier), (uint32_t)S.soldiers, (uint32_t)S.soldiers, P},
};

geom g_geom[MH_CAPS_COUNT] = {STOCK[0], STOCK[1], STOCK[2], STOCK[3], STOCK[4],
                              STOCK[5], STOCK[6], STOCK[7], STOCK[8]};

// ---- slice C state ------------------------------------------------------------------------------
bool g_relocated             = false;
bool g_units_relocated       = false;
bool g_raised                = false;
bool g_mirror_check          = false;
bool g_have_step             = false;
void (*g_world_end_revert)() = nullptr; // PT-CAP9-U8b: a session-close revert deferred to the next quiescent point
bool     g_world_end_pending = false;
unsigned g_last_step_ms      = 0;
void (*g_log)(const char *)  = nullptr;
mh::patch::toggle_state g_limits;
// ---- PT-CAP11 AI scratch layer state ----
mh::patch::toggle_state g_ai;
uint32_t                g_ai_base                          = 0;
uint32_t                g_ai_hw[ai_scratch_layout::COUNT]  = {};
bool                    g_ai_hit[ai_scratch_layout::COUNT] = {};
uint32_t                g_ai_ticks                         = 0;
uint32_t                g_ai_last_log                      = 0;
mh::patch::toggle_state g_limits_u;
uint8_t                *g_stock_override[MH_CAPS_COUNT] = {};
uint8_t                *g_mirror_copy[WIDE_COUNT]       = {};
bool                    g_mirror_reported[WIDE_COUNT]   = {};

void clog(const char *fmt, ...) {
    if (!g_log) return;
    char    b[400];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(b, sizeof(b), _TRUNCATE, fmt, ap);
    va_end(ap);
    g_log(b);
}

uint8_t *stock_ptr(unsigned i) {
    return g_stock_override[i] ? g_stock_override[i] : reinterpret_cast<uint8_t *>(static_cast<uintptr_t>(STOCK[i].base));
}
uint8_t *wide_ptr(unsigned i) { return reinterpret_cast<uint8_t *>(static_cast<uintptr_t>(g_geom[i].base)); }

size_t stock_bytes(unsigned i) { return static_cast<size_t>(STOCK[i].players) * STOCK[i].row * STOCK[i].elem; }

bool is_wide(unsigned i) { return i < RAISED_COUNT ? g_relocated : g_units_relocated; }

void mirror_free() {
    for (unsigned i = 0; i < WIDE_COUNT; ++i) {
        free(g_mirror_copy[i]);
        g_mirror_copy[i]     = nullptr;
        g_mirror_reported[i] = false;
    }
}

void mirror_remember() {
    if (!g_mirror_check) return;
    for (unsigned i = 0; i < WIDE_COUNT; ++i) {
        if (!is_wide(i)) continue;
        if (!g_mirror_copy[i]) g_mirror_copy[i] = static_cast<uint8_t *>(malloc(stock_bytes(i)));
        if (g_mirror_copy[i]) memcpy(g_mirror_copy[i], stock_ptr(i), stock_bytes(i));
    }
}

void sync(unsigned i) {
    mh_caps_base[i]      = g_geom[i].base;
    mh_caps_row_bytes[i] = g_geom[i].row * g_geom[i].elem;
    mh_caps_limit[i]     = g_geom[i].limit;
}

} // namespace

const geom &g(arr a) { return g_geom[a]; }
const geom &stock(arr a) { return STOCK[a]; }

bool relocated() { return g_relocated; } // true once the reloc manifest is applied
bool units_relocated() { return g_units_relocated; }
bool wide(arr a) { return a < MH_CAPS_COUNT && is_wide(a); }
bool raised() { return g_raised; } // true while the limits are ON
void set_raised_for_test(bool on) { g_raised = on; }

bool configure(arr a, uint32_t base, uint32_t row, uint32_t limit) {
    if (a >= N) return false;
    if (!base || !row || !limit || limit > row) return false;
    g_geom[a].base  = base;
    g_geom[a].row   = row;
    g_geom[a].limit = limit;
    sync(a);
    return true;
}

void reset_stock() {
    for (unsigned i = 0; i < MH_CAPS_COUNT; ++i) {
        g_geom[i] = STOCK[i];
        sync(i);
        g_stock_override[i] = nullptr;
    }
    g_relocated         = false;
    g_units_relocated   = false;
    g_raised            = false;
    g_have_step         = false;
    g_world_end_pending = false;
    g_world_end_revert  = nullptr;
    g_mirror_check      = false;
    mirror_free();
    memset(&g_limits, 0, sizeof(g_limits));
    memset(&g_limits_u, 0, sizeof(g_limits_u));
    memset(&g_ai, 0, sizeof(g_ai));
    g_ai_base = 0;
    memset(g_ai_hw, 0, sizeof(g_ai_hw));
    memset(g_ai_hit, 0, sizeof(g_ai_hit));
    g_ai_ticks    = 0;
    g_ai_last_log = 0;
    orders::reset_for_test(); // PT-CAP9-U5: the order layer's state rides with the rest
    path::reset();
}

// ---- the relocated layout -----------------------------------------------------------------------
// Offsets inside `.mhcapd` (64-byte aligned, no overlap; generator-asserted) and the raised rows.
const uint32_t CAPD_OFFSET[RAISED_COUNT] = {0x000000u, 0x10a9c0u, 0x147240u, 0x154cc0u, 0x162b00u, 0x164ac0u, 0x1c9240u};
const uint32_t CAPU_OFFSET[2]            = {0x000000u, 0x0e38c0u}; // units, soldiers inside `.mhcapu`
const uint32_t RAISED_ROW[WIDE_COUNT]    = {500u, 127u, 127u, 127u, 127u, 127u, 4000u, 500u, 500u};

bool adopt_relocation(uint32_t capd_base) {
    if (!capd_base) return false;
    for (unsigned i = 0; i < RAISED_COUNT; ++i)
        if (!configure(static_cast<arr>(i), capd_base + CAPD_OFFSET[i], RAISED_ROW[i], STOCK[i].limit)) {
            for (unsigned k = 0; k < i; ++k) {
                g_geom[k] = STOCK[k];
                sync(k);
            }
            return false;
        }
    g_relocated = true;
    g_raised    = false;
    return true;
}

bool adopt_units_relocation(uint32_t capu_base) {
    if (!capu_base) return false;
    for (unsigned k = 0; k < 2; ++k) {
        const unsigned i = MH_CAPS_IDX_UNITS + k;
        if (!configure(static_cast<arr>(i), capu_base + CAPU_OFFSET[k], RAISED_ROW[i], STOCK[i].limit)) {
            for (unsigned j = MH_CAPS_IDX_UNITS; j < i; ++j) {
                g_geom[j] = STOCK[j];
                sync(j);
            }
            return false;
        }
    }
    g_units_relocated = true;
    return true;
}

mh::patch::toggle_state &limits_state() { return g_limits; }
mh::patch::toggle_state &units_limits_state() { return g_limits_u; }

void set_logger(void (*fn)(const char *)) {
    g_log = fn;
    orders::set_log_sink(fn);
    path::set_logger(fn);
}

// ---- PT-CAP11: the AI scratch layer ----------------------------------------------------------------
mh::patch::toggle_state &ai_state() { return g_ai; }

bool ai_on() { return g_ai.staged && g_ai.on && g_ai_base != 0; }

bool ai_adopt(uint32_t section_base) {
    if (!section_base) return false;
    g_ai_base = section_base;
    // The section is zero-initialised; the guard after each enlarged array is the only non-zero content.
    for (unsigned i = 0; i < ai_scratch_layout::COUNT; ++i) {
        const auto &e = ai_scratch_layout::ARRAYS[i];
        memset(reinterpret_cast<void *>(static_cast<uintptr_t>(section_base) + e.off + e.bytes), ai_scratch_layout::GUARD_FILL, ai_scratch_layout::GUARD_BYTES);
    }
    return true;
}

namespace {
// 1-based index of the last non-zero byte of [p, p+n) (0 = all zero). Scans backwards a dword at a time.
uint32_t last_nonzero(const uint8_t *p, uint32_t n) {
    uint32_t i = n;
    while (i && (i & 3u)) {
        if (p[--i]) return i + 1;
    }
    while (i >= 4) {
        uint32_t w;
        memcpy(&w, p + i - 4, 4);
        if (w) {
            while (!p[--i]) {
            }
            return i + 1;
        }
        i -= 4;
    }
    return 0;
}
} // namespace

uint32_t ai_high_water(unsigned i) { return i < ai_scratch_layout::COUNT ? g_ai_hw[i] : 0; }

uint32_t ai_guard_check() {
    if (!g_ai_base) return 0;
    uint32_t broken = 0;
    for (unsigned i = 0; i < ai_scratch_layout::COUNT; ++i) {
        const auto    &e  = ai_scratch_layout::ARRAYS[i];
        const uint8_t *gp = reinterpret_cast<const uint8_t *>(static_cast<uintptr_t>(g_ai_base) + e.off + e.bytes);
        uint32_t       k  = 0;
        while (k < ai_scratch_layout::GUARD_BYTES && gp[k] == ai_scratch_layout::GUARD_FILL) ++k;
        if (k != ai_scratch_layout::GUARD_BYTES && !g_ai_hit[i]) {
            g_ai_hit[i] = true;
            clog("; [caps] AI GUARD HIT array=%s tick=%u: guard byte +%u (of %u) is 0x%02X, not 0x%02X -- something wrote past the enlarged %s (%u elements)\n",
                 e.name, g_ai_ticks, k, ai_scratch_layout::GUARD_BYTES, gp[k], ai_scratch_layout::GUARD_FILL, e.name, e.count);
        }
        if (g_ai_hit[i]) broken |= 1u << i;
    }
    return broken;
}

// One sim step with the AI layer ON: the guard check (every step, 8 x 64 bytes) and the census (every 16 steps: the last non-zero
// element of each enlarged array; the arrays are not cleared between AI passes, so this is a high-water mark of every index the AI wrote).
static void ai_step() {
    if (!ai_on()) return;
    ++g_ai_ticks;
    (void)ai_guard_check();
    if ((g_ai_ticks & 15u) != 0) return;
    bool exceeded = false;
    for (unsigned i = 0; i < ai_scratch_layout::COUNT; ++i) {
        const auto    &e  = ai_scratch_layout::ARRAYS[i];
        const uint32_t n  = last_nonzero(reinterpret_cast<const uint8_t *>(static_cast<uintptr_t>(g_ai_base) + e.off), e.bytes);
        const uint32_t el = (n + e.elem - 1) / e.elem;
        if (el > g_ai_hw[i]) {
            const bool first = g_ai_hw[i] <= e.stock_count && el > e.stock_count;
            g_ai_hw[i]       = el;
            if (first) {
                exceeded = true;
                clog("; [caps] ai-census: %s element %u is past the vanilla %u -- a vanilla layout would have overflowed here (tick %u)\n", e.name, el, e.stock_count, g_ai_ticks);
            }
        }
    }
    (void)exceeded;
    if (g_ai_ticks - g_ai_last_log >= 600u) {
        g_ai_last_log = g_ai_ticks;
        char b[360];
        int  n = _snprintf_s(b, sizeof(b), _TRUNCATE, "; [caps] ai-census tick=%u hw:", g_ai_ticks);
        for (unsigned i = 0; i < ai_scratch_layout::COUNT && n > 0; ++i)
            n += _snprintf_s(b + n, sizeof(b) - n, _TRUNCATE, " %s=%u/%u", ai_scratch_layout::ARRAYS[i].name, g_ai_hw[i], ai_scratch_layout::ARRAYS[i].stock_count);
        if (n > 0) n += _snprintf_s(b + n, sizeof(b) - n, _TRUNCATE, " guard=0x%X\n", ai_guard_check());
        g_log ? g_log(b) : (void)0;
    }
}

void note_sim_step() {
    // NOT clearing a pending session-close revert here: the game-over dialog's modal loop keeps stepping the sim (measured -- a clear here
    // dropped the revert before the menu was reached), so a step after the close is not a new world.
    g_have_step    = true;
    g_last_step_ms = GetTickCount();
    ai_step();
    orders::step();   // PT-CAP9-U5: guards, high-water, refusal counters
    path::save_end(); // PT-CAP9-U7b: a save that aborted before its close leaves the file-shaped free counts in place -- put the live ones back
    path::step();     // PT-CAP9-U4: `[caps] path_check=1`
}
void note_world_ended() { g_have_step = false; }

// PT-CAP9-U8b. The session close can fire INSIDE the sim step (on_gameover runs in the unit tick), and the revert flips the limit
// sites; doing that mid-step lets the step's remaining code run against code that just changed under it. A world that has stepped
// therefore defers the revert to quiescent_point(); a close with no world (lobby-only) reverts at once.
bool end_world_deferred(void (*revert)()) {
    if (!g_have_step) {
        if (revert) revert();
        return false;
    }
    g_world_end_pending = true;
    g_world_end_revert  = revert;
    clog("; [caps] session closed with a world that stepped: the limit revert is deferred to the next quiescent point\n");
    return true;
}
bool world_end_pending() { return g_world_end_pending; }
void quiescent_point() {
    if (!g_world_end_pending) return;
    void (*fn)()        = g_world_end_revert;
    g_world_end_pending = false;
    g_world_end_revert  = nullptr;
    g_have_step         = false;
    clog("; [caps] quiescent point: running the deferred session-close revert\n");
    if (fn) fn();
}

static bool world_live() { return g_have_step && static_cast<unsigned>(GetTickCount() - g_last_step_ms) < 1500u; }

bool set_limits(bool on, const char *why) {
    if (on == g_raised) return true;
    const char *who = why ? why : "?";
    // PT-CAP9-U4e: the libmh sim pins the path pool at 100 slots. A promoted game function that touches the pool means libmh is
    // driving the sim: the raise is refused (the same surface as every other refusal here: a log line, false, nothing changes).
    if (on) {
        if (const char *owner = path::libmh_blocks()) {
            clog("; [caps] raise REFUSED (%s): libmh path functions are installed (%s is promoted) -- the libmh sim pins the path pool at 100 slots\n", who, owner);
            return false;
        }
    }
    if (on && !g_relocated) {
        clog("; [caps] raise REFUSED (%s): arrays not relocated -- the limits stay vanilla\n", who);
        return false;
    }
    if (!g_limits.staged) {
        clog("; [caps] %s REFUSED (%s): the limit layer is not staged\n", on ? "raise" : "lower", who);
        return false;
    }
    if (world_live()) {
        clog("; [caps] %s REFUSED (%s): a strategic world is live and stepping -- limits change only at a quiescent point\n",
             on ? "raise" : "lower", who);
        return false;
    }
    int         bad = -1;
    const char *w   = nullptr;
    // PT-CAP11: the AI scratch layer rides on the same switch (when staged: `[caps] ai_scratch=0` leaves it out -- the negative arm).
    // It goes first, so a refusal leaves the limits as they were; the limit layer's own refusal puts it back.
    const bool ai_staged = g_ai.staged;
    if (ai_staged && !mh::patch::toggle(g_ai, on, &bad, &w)) {
        clog("; [caps] %s REFUSED (%s): the AI scratch layer: %s\n", on ? "raise" : "lower", who, w ? w : "toggle refused");
        return false;
    }
    if (!mh::patch::toggle(g_limits, on, &bad, &w)) {
        if (ai_staged) (void)mh::patch::toggle(g_ai, !on, nullptr, nullptr);
        clog("; [caps] %s REFUSED (%s): %s\n", on ? "raise" : "lower", who, w ? w : "toggle refused");
        return false;
    }
    // PT-CAP9-U1: the units layer rides with the v0.2.2 one. It is optional (its manifest may have been refused at boot): without
    // it the units stay at 100 and everything else still raises. A refusal here puts the first layer back.
    const bool with_units = g_units_relocated && g_limits_u.staged;
    if (with_units && !mh::patch::toggle(g_limits_u, on, &bad, &w)) {
        clog("; [caps] %s REFUSED (%s): units layer: %s\n", on ? "raise" : "lower", who, w ? w : "toggle refused");
        mh::patch::toggle(g_limits, !on, &bad, &w);
        if (ai_staged) (void)mh::patch::toggle(g_ai, !on, nullptr, nullptr);
        return false;
    }
    // PT-CAP9-U5: the ORDER layer (queue / pending / staging arrays, bounds, counted refusals, the sender quota) rides on the same switch.
    // Optional like the AI layer: unstaged (`[caps] orders_layer=0`, or its manifest was refused at boot) leaves the vanilla order arrays.
    // A refusal puts every layer already flipped back.
    if (orders::staged() && !orders::toggle(on, who)) {
        if (with_units) (void)mh::patch::toggle(g_limits_u, !on, nullptr, nullptr);
        (void)mh::patch::toggle(g_limits, !on, nullptr, nullptr);
        if (ai_staged) (void)mh::patch::toggle(g_ai, !on, nullptr, nullptr);
        clog("; [caps] %s REFUSED (%s): the order layer\n", on ? "raise" : "lower", who);
        return false;
    }
    if (ai_staged && on) memset(g_ai_hw, 0, sizeof(g_ai_hw));
    if (ai_staged) clog("; [caps] ai-scratch %s (%s): %d sites -> .mhcapa %08X, %u arrays enlarged, guards armed\n", on ? "ON" : "OFF", who, g_ai.m ? g_ai.m->site_count : 0, (unsigned)g_ai_base, (unsigned)ai_scratch_layout::COUNT);
    for (unsigned i = 0; i < RAISED_COUNT; ++i) {
        g_geom[i].limit = on ? RAISED_ROW[i] : STOCK[i].limit;
        sync(i);
    }
    if (with_units)
        for (unsigned i = MH_CAPS_IDX_UNITS; i < WIDE_COUNT; ++i) {
            g_geom[i].limit = on ? RAISED_ROW[i] : STOCK[i].limit;
            sync(i);
        }
    g_raised = on;
    clog("; [caps] limits %s (%s): buildings %u, storage %u, turrets %u, mines %u, labs %u, productions %u, projectiles %u, units %u, soldiers %u\n",
         on ? "ON" : "OFF", who, g_geom[0].limit, g_geom[1].limit, g_geom[2].limit, g_geom[3].limit, g_geom[4].limit,
         g_geom[5].limit, g_geom[6].limit, g_geom[MH_CAPS_IDX_UNITS].limit, g_geom[MH_CAPS_IDX_SOLDIERS].limit);
    return true;
}

// ---- the canonical mirror (D1) --------------------------------------------------------------------
static void mirror_report(unsigned i, const uint8_t *now) {
    if (!g_mirror_check || !g_mirror_copy[i] || g_mirror_reported[i]) return;
    const size_t n = stock_bytes(i);
    if (memcmp(g_mirror_copy[i], now, n) == 0) return;
    size_t off = 0;
    while (off < n && g_mirror_copy[i][off] == now[off]) ++off;
    g_mirror_reported[i] = true;
    clog("; [caps] mirror_check: array %u stock byte +%u changed between two gathers -- a writer touched a STOCK "
         "address (a missed relocation, or a DLL/harness writer that does not scatter)\n",
         i, static_cast<unsigned>(off));
}

void canon_gather() {
    orders::gather(); // PT-CAP9-U5: the order arrays' stock windows (no-op unless the order layer is ON)
    if (!g_relocated && !g_units_relocated) return;
    for (unsigned i = 0; i < WIDE_COUNT; ++i) {
        if (!is_wide(i)) continue;
        const geom    &w    = g_geom[i];
        const geom    &sk   = STOCK[i];
        uint8_t       *dst  = stock_ptr(i);
        const uint8_t *src  = wide_ptr(i);
        const size_t   vrow = static_cast<size_t>(sk.row) * sk.elem;
        const size_t   wrow = static_cast<size_t>(w.row) * w.elem;
        mirror_report(i, dst);
        for (uint32_t p = 0; p < sk.players; ++p) memcpy(dst + p * vrow, src + p * wrow, vrow);
    }
    path::gather();
    mirror_remember();
}

void canon_scatter() {
    orders::scatter();
    if (!g_relocated && !g_units_relocated) return;
    for (unsigned i = 0; i < WIDE_COUNT; ++i) {
        if (!is_wide(i)) continue;
        const geom    &w    = g_geom[i];
        const geom    &sk   = STOCK[i];
        const uint8_t *src  = stock_ptr(i);
        uint8_t       *dst  = wide_ptr(i);
        const size_t   vrow = static_cast<size_t>(sk.row) * sk.elem;
        const size_t   wrow = static_cast<size_t>(w.row) * w.elem;
        for (uint32_t p = 0; p < sk.players; ++p) {
            memcpy(dst + p * wrow, src + p * vrow, vrow);
            if (wrow > vrow) memset(dst + p * wrow + vrow, 0, wrow - vrow);
        }
    }
    path::scatter();
    mirror_remember();
}

uint32_t overflow_records() {
    if (!g_relocated && !g_units_relocated) return 0;
    uint32_t n = 0;
    for (unsigned i = 0; i < WIDE_COUNT; ++i) {
        if (!is_wide(i)) continue;
        const geom    &w    = g_geom[i];
        const geom    &sk   = STOCK[i];
        const uint8_t *base = wide_ptr(i);
        for (uint32_t p = 0; p < sk.players; ++p)
            for (uint32_t r = sk.row; r < w.row; ++r) {
                const uint8_t *rp = base + (static_cast<size_t>(p) * w.row + r) * w.elem;
                for (uint32_t b = 0; b < w.elem; ++b)
                    if (rp[b]) {
                        ++n;
                        break;
                    }
            }
    }
    return n;
}

void set_mirror_check(bool on) {
    g_mirror_check = on;
    if (!on) mirror_free();
}

void set_canon_stock_for_test(arr a, uint8_t *p) {
    if (a < MH_CAPS_COUNT) g_stock_override[a] = p;
}

} // namespace mh::caps

// Constant-initialised so a stub that runs before any C++ initialiser still reads the stock values
// (capstest cross-checks these literals against the table built from the generated manifests).
extern "C" {
uint32_t mh_caps_base[MH_CAPS_COUNT]      = {0x00c3d2a0u, 0x00c727c0u, 0x00cc0fe0u, 0x00d03480u, 0x00d02d80u,
                                             0x00dd2648u, 0x00e1db98u, 0x00dd8c48u, 0x00e0e5a8u};
uint32_t mh_caps_row_bytes[MH_CAPS_COUNT] = {100u * 273u, 25u * 244u, 32u * 55u, 32u * 56u, 25u * 8u,
                                             8u * 405u, 1500u * 121u, 100u * 233u, 100u * 29u};
uint32_t mh_caps_limit[MH_CAPS_COUNT]     = {100u, 25u, 32u, 32u, 25u, 8u, 1500u, 100u, 100u};
}
