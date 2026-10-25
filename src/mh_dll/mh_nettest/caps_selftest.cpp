//
// caps_selftest.cpp -- `net_selftest capstest`: the roster-geometry table (patching:PT-CAP8 slice B).
//
// WHAT IT PROVES. Slice B converts every shipped hook that hard-coded vanilla geometry to read
// mh::caps, and the whole conversion is only behaviour-neutral if the table's STOCK values equal what
// those hooks used to spell. So the arms here pin the table against HAND-COMPUTED numbers (the stock
// VAs and strides of the nine arrays, worked out from the roster's own record sizes, not copied out
// of caps.cpp), against the generated region registry (extent = players * row * elem), and against
// the naked-stub mirrors (the extern "C" dword arrays the __asm stubs actually read, through a real
// naked function so the symbol and the MH_CAPS_IDX_* offsets are exercised, not assumed).
//
// It also drives the slice-C seam, configure(): a fake apply moves the table to a wider layout and
// the same arithmetic must follow (rec(), row_bytes, the asm mirrors), refusals leave the table
// untouched, and reset_stock() restores every entry exactly. No game memory is read or written.
//
#include "caps/ai_scratch_layout.gen.h"
#include "caps/caps.h"
#include "caps/orders.h"
#include "caps/orders_layout.gen.h"
#include "caps/path_pool.h"
#include "caps/path_pool_layout.gen.h"

#include "addr/mh_addrs.gen.h"
#include "addr/mh_regions.gen.h"
#include "addr/mh_structs.gen.h"
#include "hook/promoted.h"
#include "patch/inmem_patch.h"
#include "patch/manifests.gen.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace mh::caps;

namespace {

int g_checks = 0, g_fails = 0;

void ck(bool ok, const char *what) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        printf("  FAIL: %s\n", what);
    }
}

// Hand-computed stock geometry. base / elem / row (= limit) / players, worked out from the exe's own
// strides: buildings 0x6aa4 = 100 * 0x111, storage 0x17d4 = 25 * 0xf4, turret row 0x6e0 = 32 * 0x37,
// mine row 0x700 = 32 * 0x38, lab row 0xc8 = 25 * 8, production row 0xca8 = 8 * 0x195, unit row
// 0x5b04 = 100 * 0xe9, soldier row 0xb54 = 100 * 0x1d, projectile pool 1500 * 0x79 = 0x2c4fc.
struct want {
    arr         a;
    const char *name;
    uint32_t    base, elem, row, players;
    uint32_t    row_bytes;
    uint32_t    rec37; // &array[min(players-1,3)][min(row-1,7)], hand-computed
    uint32_t    last;  // &array[players-1][row-1]
    uint32_t    end;   // base + players*row*elem
};
const want W[MH_CAPS_COUNT] = {
    {BUILDINGS, "buildings", 0x00c3d2a0, 273, 100, 8, 0x6aa4, 0x00c51a03, 0x00c726af, 0x00c727c0},
    {UNIT_STORAGE, "unit_storage", 0x00c727c0, 244, 25, 8, 0x17d4, 0x00c775e8, 0x00c7e56c, 0x00c7e660},
    {TURRETS, "turrets", 0x00cc0fe0, 55, 32, 8, 0x6e0, 0x00cc2601, 0x00cc46a9, 0x00cc46e0},
    {MINES, "mines", 0x00d03480, 56, 32, 8, 0x700, 0x00d04b08, 0x00d06c48, 0x00d06c80},
    {LABS, "labs", 0x00d02d80, 8, 25, 8, 0xc8, 0x00d03010, 0x00d033b8, 0x00d033c0},
    {PRODUCTIONS, "productions", 0x00dd2648, 405, 8, 8, 0xca8, 0x00dd5753, 0x00dd89f3, 0x00dd8b88},
    {PROJECTILES, "projectiles", 0x00e1db98, 121, 1500, 1, 0x2c4fc, 0x00e1dee7, 0x00e4a01b, 0x00e4a094},
    {UNITS, "units", 0x00dd8c48, 233, 100, 8, 0x5b04, 0x00dea3b3, 0x00e0637f, 0x00e06468},
    {SOLDIERS, "soldiers", 0x00e0e5a8, 29, 100, 8, 0xb54, 0x00e1086f, 0x00e1402b, 0x00e14048},
};

// The naked-stub view: the same extern "C" arrays an __asm block reads, through real inline asm with
// the MH_CAPS_IDX_* macros (so a macro that drifts from the enum, or a symbol that is not the array
// the C++ side writes, shows here).
__declspec(naked) uint32_t asm_base(uint32_t) {
    __asm {
        mov   eax, dword ptr [esp + 4]
        mov   eax, dword ptr [mh_caps_base + eax * 4]
        ret
    }
}
__declspec(naked) uint32_t asm_row_bytes(uint32_t) {
    __asm {
        mov   eax, dword ptr [esp + 4]
        mov   eax, dword ptr [mh_caps_row_bytes + eax * 4]
        ret
    }
}
__declspec(naked) uint32_t asm_limit(uint32_t) {
    __asm {
        mov   eax, dword ptr [esp + 4]
        mov   eax, dword ptr [mh_caps_limit + eax * 4]
        ret
    }
}
// The exact shapes the converted stubs use, with constant (macro) indices.
__declspec(naked) uint32_t asm_turret_base() {
    __asm {
        mov   eax, dword ptr [mh_caps_base + 4 * MH_CAPS_IDX_TURRETS]
        ret
    }
}
__declspec(naked) uint32_t asm_unit_row_bytes_of_player(uint32_t) {
    __asm {
        mov   edx, dword ptr [esp + 4]
        imul  edx, dword ptr [mh_caps_row_bytes + 4 * MH_CAPS_IDX_UNITS]
        mov   eax, edx
        ret
    }
}
__declspec(naked) uint32_t asm_bldg_player_off(uint32_t) {
    __asm {
        mov   eax, dword ptr [esp + 4]
        mov   ecx, eax
        imul  ecx, dword ptr [mh_caps_row_bytes + 4 * MH_CAPS_IDX_BUILDINGS]
        add   ecx, dword ptr [mh_caps_base + 4 * MH_CAPS_IDX_BUILDINGS]
        mov   eax, ecx
        ret
    }
}

bool mirrors_match_table() {
    for (unsigned i = 0; i < MH_CAPS_COUNT; ++i) {
        const geom &x = g(static_cast<arr>(i));
        if (asm_base(i) != x.base || asm_row_bytes(i) != x.row * x.elem || asm_limit(i) != x.limit) return false;
        if (mh_caps_base[i] != x.base || mh_caps_row_bytes[i] != x.row * x.elem || mh_caps_limit[i] != x.limit) return false;
    }
    return true;
}


// ---- PT-CAP8 slice C helpers --------------------------------------------------------------------
//
// A heap window over 0x00400000..0x004F0000 stands in for the game image so set_limits() can flip the
// REAL limit manifest (51 sites) without a game; sections are calloc'd.
constexpr uint32_t ILO   = 0x00400000u;
constexpr uint32_t ISIZE = 0x000F0000u;
uint8_t           *g_img = nullptr;
void              *g_sec = nullptr;

void *f_res(uint32_t, uint32_t size) {
    g_sec = calloc(size, 1);
    return g_sec;
}
void *f_res_any(uint32_t size) { return f_res(0, size); }
void  f_rel(void *p, uint32_t) {
    free(p);
    if (p == g_sec) g_sec = nullptr;
}
bool f_prot(void *, uint32_t, uint32_t, uint32_t *old_out) {
    if (old_out) *old_out = 0x20;
    return true;
}
uint8_t                    *f_at(uint32_t va) { return g_img + (va - ILO); }
const mh::patch::patch_host FAKE_HOST = {&f_res, &f_res_any, &f_rel, &f_prot, &f_at, nullptr};

void seed_limits_image() {
    memset(g_img, 0xCC, ISIZE);
    const mh::patch::inmem_manifest &m = *mh::patch::CAPS_LIMITS;
    for (int i = 0; i < m.site_count; ++i) memcpy(f_at(m.sites[i].va), m.sites[i].expect, m.sites[i].len);
}

char g_cap_log[512];
int  g_cap_log_n = 0;
void cap_log(const char *s) {
    strncpy_s(g_cap_log, sizeof(g_cap_log), s, _TRUNCATE);
    ++g_cap_log_n;
}
bool cap_logged(const char *needle) { return strstr(g_cap_log, needle) != nullptr; }

// Deterministic fill so a copied byte is attributable to its source.
uint8_t pat(unsigned arr_i, uint32_t p, uint32_t r, uint32_t b) { return (uint8_t)(1u + ((arr_i * 131u + p * 31u + r * 7u + b * 3u) % 251u)); }

} // namespace

int run_capstest() {
    printf("== roster geometry table selftest (PT-CAP8 slices B+C: mh::caps) ==\n");
    reset_stock();

    // ---- (1) the enum and the asm index macros are the same numbers, in the spec's order ----
    ck(BUILDINGS == 0 && UNIT_STORAGE == 1 && TURRETS == 2 && MINES == 3 && LABS == 4 && PRODUCTIONS == 5 &&
           PROJECTILES == 6 && UNITS == 7 && SOLDIERS == 8 && N == 9 && MH_CAPS_COUNT == 9,
       "enum arr and MH_CAPS_IDX_* agree: 9 arrays in the spec's order");
    ck(MH_CAPS_IDX_TURRETS == (int)TURRETS && MH_CAPS_IDX_UNITS == (int)UNITS && MH_CAPS_IDX_BUILDINGS == (int)BUILDINGS,
       "the macros the stubs use equal the enum values");

    // ---- (2) stock geometry == hand-computed, for all nine ----
    for (const want &w : W) {
        const geom &x = g(w.a);
        char        m[160];
        snprintf(m, sizeof m, "%s: stock base/elem/row/limit/players", w.name);
        ck(x.base == w.base && x.elem == w.elem && x.row == w.row && x.limit == w.row && x.players == w.players, m);
        snprintf(m, sizeof m, "%s: row_bytes == the game's stride immediate", w.name);
        ck(row_bytes(w.a) == w.row_bytes, m);
        snprintf(m, sizeof m, "%s: rec(0,0) is the base", w.name);
        ck(reinterpret_cast<uintptr_t>(rec<uint8_t>(w.a, 0, 0)) == w.base, m);
        const uint32_t pp = w.players > 3 ? 3 : w.players - 1, ii = w.row > 7 ? 7 : w.row - 1;
        snprintf(m, sizeof m, "%s: rec(%u,%u) == hand-computed VA", w.name, pp, ii);
        ck(reinterpret_cast<uintptr_t>(rec<uint8_t>(w.a, pp, ii)) == w.rec37, m);
        snprintf(m, sizeof m, "%s: last record == hand-computed VA", w.name);
        ck(reinterpret_cast<uintptr_t>(rec<uint8_t>(w.a, w.players - 1, w.row - 1)) == w.last, m);
        snprintf(m, sizeof m, "%s: base + players*row*elem == hand-computed end", w.name);
        ck(w.base + w.players * w.row * w.elem == w.end, m);
        snprintf(m, sizeof m, "%s: stock() == g() before any configure", w.name);
        ck(stock(w.a).base == x.base && stock(w.a).row == x.row && stock(w.a).limit == x.limit, m);
    }
    // the same pointer type for a typed access
    ck(rec<mh::game::mh_map_object_building>(BUILDINGS, 1, 0) ==
           reinterpret_cast<mh::game::mh_map_object_building *>(0x00c3d2a0 + 0x6aa4),
       "rec<building>(1,0) == buildings + one stock row");

    // ---- (3) the table agrees with the generated registry: extent == players * row * elem ----
    {
        using namespace mh::state;
        struct pair {
            arr       a;
            region_id r;
        };
        const pair P[MH_CAPS_COUNT] = {{BUILDINGS, RID_BUILDINGS}, {UNIT_STORAGE, RID_UNIT_STORAGE}, {TURRETS, RID_TURRETS}, {MINES, RID_MINES}, {LABS, RID_LABS}, {PRODUCTIONS, RID_PRODUCTIONS}, {PROJECTILES, RID_STRAT_PROJECTILE_POOL}, {UNITS, RID_UNITS}, {SOLDIERS, RID_STRAT_SOLDIERS}};
        for (const pair &p : P) {
            const geom &x = g(p.a);
            char        m[160];
            snprintf(m, sizeof m, "%s: registry size == players*row*elem", W[p.a].name);
            ck(REGIONS[p.r].size == x.players * x.row * x.elem, m);
#ifndef MH_LIBMH_BUILD
            snprintf(m, sizeof m, "%s: registry stock base == table base", W[p.a].name);
            ck(REGIONS[p.r].base == x.base, m);
#endif
        }
    }

    // ---- (4) asm-global layout: the naked mirrors ARE the table ----
    ck(sizeof(mh_caps_base) == 36 && sizeof(mh_caps_row_bytes) == 36 && sizeof(mh_caps_limit) == 36,
       "each mirror is nine dwords (the stubs scale the index by 4)");
    ck(mirrors_match_table(), "stock: every mirror dword equals the table, read in C++ and through real inline asm");
    ck(asm_turret_base() == 0x00cc0fe0, "[mh_caps_base + 4*MH_CAPS_IDX_TURRETS] reads the turret base");
    ck(asm_unit_row_bytes_of_player(3) == 3 * 0x5b04, "imul by [mh_caps_row_bytes + 4*IDX_UNITS] == player * 0x5b04");
    ck(asm_bldg_player_off(2) == 0x00c3d2a0 + 2 * 0x6aa4, "the turret-reload stub's building row arithmetic (stock)");

    // ---- (5) relocated()/raised() are false in slice B ----
    ck(!relocated() && !raised(), "slice B: not relocated, not raised");

    // ---- (6) a fake apply: configure() moves base/row/limit and everything follows ----
    ck(configure(BUILDINGS, 0x010f0000, 500, 100), "configure(BUILDINGS, relocated, row 500, limit 100) accepted");
    {
        const geom &x = g(BUILDINGS);
        ck(x.base == 0x010f0000 && x.row == 500 && x.limit == 100 && x.elem == 273 && x.players == 8,
           "OFF-after-relocate: base moved, row 500, limit still the vanilla 100");
        ck(row_bytes(BUILDINGS) == 500 * 273, "row_bytes follows the row (136500 = 0x21534)");
        ck(reinterpret_cast<uintptr_t>(rec<uint8_t>(BUILDINGS, 3, 7)) == 0x010f0000u + (3u * 500 + 7) * 273,
           "rec(3,7) follows base + row");
        ck(mirrors_match_table(), "mirrors follow configure()");
        ck(asm_bldg_player_off(2) == 0x010f0000u + 2 * 136500u, "the asm building row arithmetic follows (500-wide rows)");
        ck(stock(BUILDINGS).base == 0x00c3d2a0 && stock(BUILDINGS).row == 100, "stock() is unaffected by configure()");
    }
    ck(configure(BUILDINGS, 0x010f0000, 500, 500), "limit raised to 500 (set_limits(1) shape)");
    ck(g(BUILDINGS).limit == 500 && mh_caps_limit[BUILDINGS] == 500 && asm_limit(BUILDINGS) == 500,
       "limit 500 visible in the table and both mirrors");
    ck(configure(UNIT_STORAGE, 0x011a0000, 127, 127) && configure(TURRETS, 0x011b0000, 127, 127) &&
           configure(MINES, 0x011c0000, 127, 127) && configure(LABS, 0x011d0000, 127, 127) &&
           configure(PRODUCTIONS, 0x011e0000, 127, 127) && configure(PROJECTILES, 0x01200000, 4000, 4000),
       "the other six raised arrays accept their raised layout");
    ck(row_bytes(PRODUCTIONS) == 127 * 405 && row_bytes(PROJECTILES) == 4000 * 121 && g(PROJECTILES).players == 1,
       "raised row bytes: productions 127*405, projectile pool 4000*121 (one row)");
    ck(reinterpret_cast<uintptr_t>(rec<uint8_t>(PROJECTILES, 0, 3999)) == 0x01200000u + 3999u * 121,
       "projectile pool last slot at the raised limit");
    ck(mirrors_match_table(), "mirrors match after all seven are raised");
    ck(g(UNITS).base == 0x00dd8c48 && g(UNITS).row == 100 && g(SOLDIERS).row == 100,
       "units + soldiers are untouched (stock, PT-CAP9)");

    // ---- (7) refusals leave the table untouched ----
    {
        const geom before = g(BUILDINGS);
        ck(!configure(BUILDINGS, 0, 500, 500), "base 0 refused");
        ck(!configure(BUILDINGS, 0x010f0000, 0, 0), "row 0 refused");
        ck(!configure(BUILDINGS, 0x010f0000, 500, 0), "limit 0 refused");
        ck(!configure(BUILDINGS, 0x010f0000, 100, 500), "limit > row refused");
        ck(!configure(UNITS, 0x01350000, 100, 500), "UNITS: limit > row refused");
        ck(!configure(SOLDIERS, 0, 500, 500), "SOLDIERS: base 0 refused");
        ck(!configure(N, 0x01300000, 5, 5), "an out-of-range array id is refused");
        const geom &after = g(BUILDINGS);
        ck(after.base == before.base && after.row == before.row && after.limit == before.limit && mirrors_match_table(),
           "refusals changed nothing");
    }

    // ---- (8) reset_stock() restores every entry exactly ----
    reset_stock();
    {
        bool same = true;
        for (const want &w : W) {
            const geom &x = g(w.a);
            same          = same && x.base == w.base && x.elem == w.elem && x.row == w.row && x.limit == w.row && x.players == w.players;
        }
        ck(same, "reset_stock(): all nine entries are the hand-computed stock geometry again");
        ck(mirrors_match_table(), "reset_stock(): the mirrors are back to stock");
    }


    // ====================================================================================================
    // ---- slice C: the relocated layout, the canonical mirror, the limit layer ------------------------
    // ====================================================================================================
    mh::caps::set_logger(&cap_log);
    mh::hook::registry_reset_for_test();
    mh::patch::reset_applied_for_test();

    // ---- (9) THE LAYOUT: disjoint, 64-aligned, inside .mhcapd, and the manifest's own refs agree ---------
    {
        constexpr uint32_t CAPD_VSIZE = 0x240000u;
        bool               aligned = true, ordered = true;
        uint32_t           end_of[RAISED_COUNT] = {};
        for (unsigned i = 0; i < RAISED_COUNT; ++i) {
            const geom &st = stock(static_cast<arr>(i));
            end_of[i]      = CAPD_OFFSET[i] + st.players * RAISED_ROW[i] * st.elem;
            aligned        = aligned && (CAPD_OFFSET[i] % 64u == 0);
            if (i + 1 < RAISED_COUNT) ordered = ordered && end_of[i] <= CAPD_OFFSET[i + 1];
        }
        ck(aligned, "(9) every raised array starts 64-byte aligned inside .mhcapd");
        ck(ordered && end_of[RAISED_COUNT - 1] <= CAPD_VSIZE, "...the extents are disjoint, in order, and end inside the 0x240000 section");
        ck(end_of[0] == 1092000u && end_of[6] == 0x1c9240u + 484000u, "...buildings are 8 x 500 x 273 = 1,092,000 bytes; the pool 4000 x 121 = 484,000");
        const mh::patch::inmem_manifest &m                 = *mh::patch::CAPS_RELOC;
        int                              hit[RAISED_COUNT] = {};
        bool                             inside            = true;
        for (int i = 0; i < m.ref_count; ++i) {
            const mh::patch::inmem_ref &r = m.refs[i];
            if (r.kind != mh::patch::ref_kind::abs32_to_section || r.target_section != 0) continue;
            bool ok = false;
            for (unsigned a = 0; a < RAISED_COUNT; ++a)
                if (r.target >= CAPD_OFFSET[a] && r.target <= end_of[a]) {
                    ++hit[a];
                    ok = true;
                    break;
                }
            inside = inside && ok;
        }
        ck(inside, "...every ref the reloc manifest bakes into .mhcapd lands INSIDE one of these extents (a drifted offset table fails here)");
        bool every = true;
        for (unsigned a = 0; a < RAISED_COUNT; ++a) every = every && hit[a] > 0;
        ck(every, "...and every one of the seven arrays is referenced at least once");
    }

    // ---- (10) adopt_relocation: the table follows the placement -----------------------------------------
    uint8_t       *block = static_cast<uint8_t *>(calloc(0x240000u, 1));
    const uint32_t capd  = (uint32_t)(uintptr_t)block;
    ck(!adopt_relocation(0), "(10) adopt_relocation refuses a zero base");
    ck(!relocated(), "...and stays unrelocated");
    ck(adopt_relocation(capd), "adopt_relocation(.mhcapd base) accepted");
    ck(relocated() && !raised(), "...relocated, not raised");
    {
        bool all = true;
        for (unsigned i = 0; i < RAISED_COUNT; ++i) {
            const geom &x = g(static_cast<arr>(i));
            all           = all && x.base == capd + CAPD_OFFSET[i] && x.row == RAISED_ROW[i] && x.limit == stock(static_cast<arr>(i)).limit;
        }
        ck(all, "...base = .mhcapd + offset, row = the raised row, limit = the VANILLA limit (OFF)");
        ck(g(UNITS).base == 0x00dd8c48 && g(SOLDIERS).row == 100 && g(PROJECTILES).players == 1, "...units/soldiers stay stock (the v0.2.2 relocation does not touch them); the pool stays one row");
        ck(!units_relocated() && !wide(UNITS) && wide(BUILDINGS), "...wide() names the seven relocated arrays only");
        ck(mirrors_match_table(), "...the naked-stub mirrors follow");
        ck(reinterpret_cast<uintptr_t>(rec<uint8_t>(TURRETS, 2, 5)) == capd + 0x147240u + (2u * 127 + 5) * 55, "...rec<>() uses the 127-wide turret row");
    }

    // ---- (10b) PT-CAP9-U1: adopt_units_relocation -- units + soldiers in .mhcapu --------------------------------
    {
        constexpr uint32_t CAPU_VSIZE = 0x100000u;
        uint8_t           *ublock     = static_cast<uint8_t *>(calloc(CAPU_VSIZE, 1));
        const uint32_t     capu       = (uint32_t)(uintptr_t)ublock;
        ck(!adopt_units_relocation(0), "(10b) adopt_units_relocation refuses a zero base");
        ck(!units_relocated() && g(UNITS).base == 0x00dd8c48, "...and leaves the units stock");
        const uint32_t u_end = CAPU_OFFSET[0] + 8u * RAISED_ROW[MH_CAPS_IDX_UNITS] * 233u;
        const uint32_t s_end = CAPU_OFFSET[1] + 8u * RAISED_ROW[MH_CAPS_IDX_SOLDIERS] * 29u;
        ck(u_end == 932000u && s_end - CAPU_OFFSET[1] == 116000u, "...units are 8 x 500 x 233 = 932,000 bytes; soldiers 8 x 500 x 29 = 116,000");
        ck(CAPU_OFFSET[0] % 64u == 0 && CAPU_OFFSET[1] % 64u == 0 && u_end <= CAPU_OFFSET[1] && s_end <= CAPU_VSIZE,
           "...64-byte aligned, disjoint, inside the 0x100000 section");
        {
            const mh::patch::inmem_manifest &m      = *mh::patch::CAPS_UNITS_RELOC;
            bool                             inside = m.ref_count > 0, h0 = false, h1 = false;
            for (int i = 0; i < m.ref_count; ++i) {
                const mh::patch::inmem_ref &r = m.refs[i];
                if (r.kind != mh::patch::ref_kind::abs32_to_section || r.target_section != 0) continue;
                const bool in0 = r.target >= CAPU_OFFSET[0] && r.target <= u_end, in1 = r.target >= CAPU_OFFSET[1] && r.target <= s_end;
                h0     = h0 || in0;
                h1     = h1 || in1;
                inside = inside && (in0 || in1);
            }
            ck(inside && h0 && h1, "...every ref the units manifest bakes into .mhcapu lands inside the units or the soldiers extent");
        }
        ck(adopt_units_relocation(capu), "adopt_units_relocation(.mhcapu base) accepted");
        ck(units_relocated() && !raised(), "...units relocated, not raised");
        ck(g(UNITS).base == capu + CAPU_OFFSET[0] && g(UNITS).row == 500 && g(UNITS).limit == 100, "...units: base in .mhcapu, row 500, limit still 100 (OFF)");
        ck(g(SOLDIERS).base == capu + CAPU_OFFSET[1] && g(SOLDIERS).row == 500 && g(SOLDIERS).limit == 100, "...soldiers: the same");
        ck(wide(UNITS) && wide(SOLDIERS) && mirrors_match_table(), "...wide() now includes both, the naked-stub mirrors follow");
        ck(reinterpret_cast<uintptr_t>(rec<uint8_t>(UNITS, 3, 7)) == capu + CAPU_OFFSET[0] + (3u * 500 + 7) * 233, "...rec<>() uses the 500-wide unit row");
        ck(row_bytes(UNITS) == 500u * 233u, "...row_bytes(UNITS) is the raised 116,500-byte player stride");
        reset_stock();
        ck(!units_relocated() && g(UNITS).base == 0x00dd8c48 && g(SOLDIERS).row == 100 && mirrors_match_table(), "...reset_stock() puts them back");
        free(ublock);
        // the rest of the file runs against the v0.2.2 relocation only (as before)
        ck(adopt_relocation(capd), "...and the v0.2.2 relocation is re-adopted for the checks that follow");
    }

    // ---- (11) THE CANONICAL MIRROR: gather / scatter on a synthetic wide/stock pair --------------------------
    {
        uint8_t *stockbuf[RAISED_COUNT];
        for (unsigned i = 0; i < RAISED_COUNT; ++i) {
            const geom &st = stock(static_cast<arr>(i));
            stockbuf[i]    = static_cast<uint8_t *>(calloc(st.players * st.row * st.elem, 1));
            set_canon_stock_for_test(static_cast<arr>(i), stockbuf[i]);
            // fill the WIDE array: every record of every player row, past the vanilla window too
            for (uint32_t p = 0; p < st.players; ++p)
                for (uint32_t r = 0; r < RAISED_ROW[i]; ++r)
                    for (uint32_t b = 0; b < st.elem; ++b) rec<uint8_t>(static_cast<arr>(i), p, r)[b] = pat(i, p, r, b);
        }
        canon_gather();
        bool gather_ok = true, wide_untouched = true;
        for (unsigned i = 0; i < RAISED_COUNT; ++i) {
            const geom &st = stock(static_cast<arr>(i));
            for (uint32_t p = 0; p < st.players; ++p)
                for (uint32_t r = 0; r < st.row; ++r)
                    if (memcmp(stockbuf[i] + ((size_t)p * st.row + r) * st.elem, rec<uint8_t>(static_cast<arr>(i), p, r), st.elem) != 0) gather_ok = false;
            for (uint32_t b = 0; b < st.elem; ++b)
                if (rec<uint8_t>(static_cast<arr>(i), st.players - 1, RAISED_ROW[i] - 1)[b] != pat(i, st.players - 1, RAISED_ROW[i] - 1, b)) wide_untouched = false;
        }
        ck(gather_ok, "(11) canon_gather: every vanilla-window record of every player row of all seven arrays is copied wide -> stock");
        ck(wide_untouched, "...and the wide arrays (tails included) are untouched by a gather");
        ck(overflow_records() > 0, "...a filled tail counts as overflow records");

        // a writer that (wrongly or rightly) wrote the MIRROR: poke one record per array, then scatter
        for (unsigned i = 0; i < RAISED_COUNT; ++i) {
            const geom &st = stock(static_cast<arr>(i));
            stockbuf[i][((size_t)(st.players - 1) * st.row + 2) * st.elem + 1] ^= 0xFF;
        }
        canon_scatter();
        bool scatter_ok = true, tails_zero = true, stock_untouched = true;
        for (unsigned i = 0; i < RAISED_COUNT; ++i) {
            const geom &st = stock(static_cast<arr>(i));
            for (uint32_t p = 0; p < st.players; ++p)
                for (uint32_t r = 0; r < st.row; ++r)
                    if (memcmp(stockbuf[i] + ((size_t)p * st.row + r) * st.elem, rec<uint8_t>(static_cast<arr>(i), p, r), st.elem) != 0) scatter_ok = false;
                    else {}
            for (uint32_t p = 0; p < st.players; ++p)
                for (uint32_t r = st.row; r < RAISED_ROW[i]; ++r)
                    for (uint32_t b = 0; b < st.elem; ++b)
                        if (rec<uint8_t>(static_cast<arr>(i), p, r)[b] != 0) tails_zero = false;
            if (stockbuf[i][((size_t)(st.players - 1) * st.row + 2) * st.elem + 1] != (uint8_t)(pat(i, st.players - 1, 2, 1) ^ 0xFF)) stock_untouched = false;
        }
        ck(scatter_ok, "canon_scatter: stock -> wide for the whole vanilla window (the poked records included)");
        ck(tails_zero, "...and every record past the vanilla window is ZEROED (a loaded world has nothing there)");
        ck(stock_untouched, "...and the stock mirror is untouched by a scatter");
        ck(overflow_records() == 0, "...so overflow_records() is 0 after a scatter");
        // overflow accounting: one record in the buildings tail, one in the pool tail
        rec<uint8_t>(BUILDINGS, 2, 100)[17]     = 9;
        rec<uint8_t>(PROJECTILES, 0, 1500)[0]   = 1;
        rec<uint8_t>(PROJECTILES, 0, 3999)[120] = 1;
        ck(overflow_records() == 3, "overflow_records() counts records (not bytes) holding anything past the window: 3");

        // ---- (12) mirror_check names a writer that touched a STOCK address between two gathers ----
        g_cap_log_n  = 0;
        g_cap_log[0] = 0;
        set_mirror_check(true);
        canon_gather(); // remembers
        ck(g_cap_log_n == 0, "(12) mirror_check: a first gather reports nothing");
        stockbuf[TURRETS][100] ^= 0x5A; // somebody wrote a stock turret byte
        canon_gather();
        ck(g_cap_log_n == 1 && cap_logged("mirror_check") && cap_logged("array 2"), "...a stock byte changed between gathers is reported ONCE, naming the array");
        stockbuf[TURRETS][101] ^= 0x5A;
        canon_gather();
        ck(g_cap_log_n == 1, "...and not again for the same array");
        set_mirror_check(false);

        for (unsigned i = 0; i < RAISED_COUNT; ++i) set_canon_stock_for_test(static_cast<arr>(i), nullptr), free(stockbuf[i]);
    }

    // ---- (13) the limit layer: set_limits over the REAL limit manifest -------------------------------------
    {
        g_img = static_cast<uint8_t *>(malloc(ISIZE));
        mh::patch::set_host(&FAKE_HOST);
        mh::patch::set_logger(&cap_log);
        seed_limits_image();

        reset_stock();
        mh::caps::set_logger(&cap_log);
        ck(!set_limits(true, "t"), "(13) set_limits(1) is REFUSED while the arrays are not relocated");
        ck(cap_logged("not relocated") && !raised(), "...saying so, and staying OFF");
        ck(set_limits(false, "t"), "...and asking for OFF while OFF is a successful no-op");

        ck(adopt_relocation(capd), "relocate again");
        ck(!set_limits(true, "t") && cap_logged("not staged"), "...relocated but with no staged limit layer: REFUSED, naming it");

        const mh::patch::apply_result ar = mh::patch::apply(*mh::patch::CAPS_LIMITS, &limits_state(), mh::patch::APPLY_QUIET_OK);
        ck(ar.applied && limits_state().staged, "stage the real cap_v022_limits_EN manifest");
        ck(set_limits(true, "t"), "set_limits(1) succeeds once relocated and staged");
        ck(raised(), "...raised()");
        {
            ck(g(BUILDINGS).limit == 500 && g(UNIT_STORAGE).limit == 127 && g(TURRETS).limit == 127 && g(MINES).limit == 127 &&
                   g(LABS).limit == 127 && g(PRODUCTIONS).limit == 127 && g(PROJECTILES).limit == 4000,
               "...the seven limits are 500 / 127 x5 / 4000");
            ck(g(UNITS).limit == 100 && g(SOLDIERS).limit == 100, "...units and soldiers keep 100");
            ck(g(BUILDINGS).row == 500 && mirrors_match_table(), "...the rows are unchanged and the stub mirrors follow the limits");
            ck(*f_at(0x004562e3u) == 0x7f, "...and the code bytes really flipped (map_FillDefaults loop bound 0x4562e3 -> 0x7f)");
        }
        ck(set_limits(true, "t"), "asking for ON while ON is a no-op");

        note_sim_step();
        ck(!set_limits(false, "t") && cap_logged("quiescent") && raised() && *f_at(0x004562e3u) == 0x7f,
           "a strategic world that is stepping REFUSES the change (and nothing moves)");
        note_world_ended();
        ck(set_limits(false, "t"), "once the world has ended the limits can come down");
        ck(!raised() && g(BUILDINGS).limit == 100 && g(PROJECTILES).limit == 1500 && g(TURRETS).limit == 32 && g(UNIT_STORAGE).limit == 25 &&
               g(PRODUCTIONS).limit == 8 && g(LABS).limit == 25 && g(MINES).limit == 32,
           "...OFF restores the vanilla limits exactly (the table's vanilla values, not configured ones)");
        {
            bool                             same = true;
            const mh::patch::inmem_manifest &m    = *mh::patch::CAPS_LIMITS;
            for (int i = 0; i < m.site_count; ++i) same = same && memcmp(f_at(m.sites[i].va), m.sites[i].expect, m.sites[i].len) == 0;
            ck(same, "...and every one of the 51 sites is byte-identical to the vanilla `expect`");
        }
        // a foreign byte
        f_at(0x004562e3u)[0] = 0x55;
        ck(!set_limits(true, "t") && !raised() && cap_logged("foreign"), "a foreign byte at a limit site REFUSES the raise, naming the cause");
        f_at(0x004562e3u)[0] = mh::patch::CAPS_LIMITS->sites[0].expect[0]; // (any site's vanilla would do; restore below)
        seed_limits_image();
        ck(set_limits(true, "t"), "...and with the foreign writer gone it works");
        ck(set_limits(false, "t"), "...and back");

        // PT-CAP9-U8b: the session close defers the revert while a world has stepped (it can fire INSIDE the sim step)
        {
            static int s_reverts = 0;
            s_reverts            = 0;
            auto revert          = []() { ++s_reverts; (void)set_limits(false, "close"); };
            ck(set_limits(true, "t"), "(13b) raise for the deferred-close arm");
            note_sim_step();
            ck(mh::caps::end_world_deferred(revert) && s_reverts == 0 && raised() && mh::caps::world_end_pending(),
               "(13b) a close with a stepped world DEFERS the revert: nothing ran, still raised, pending");
            ck(raised() && *f_at(0x004562e3u) == 0x7f, "...and the limit sites did not move");
            mh::caps::quiescent_point();
            ck(s_reverts == 1 && !raised() && !mh::caps::world_end_pending() && *f_at(0x004562e3u) != 0x7f,
               "...the quiescent point ends the world and runs the revert once (the live guard no longer refuses it)");
            mh::caps::quiescent_point();
            ck(s_reverts == 1, "...a second quiescent point is a no-op");
            ck(set_limits(true, "t"), "(13b) raise again");
            ck(!mh::caps::end_world_deferred(revert) && s_reverts == 2 && !raised(), "(13b) a close with NO stepped world reverts immediately");
            ck(set_limits(true, "t"), "(13b) raise once more");
            note_sim_step();
            (void)mh::caps::end_world_deferred(revert);
            note_sim_step(); // the game-over dialog's modal loop keeps stepping the sim after the close
            ck(mh::caps::world_end_pending() && raised(), "(13b) a sim step after the close does NOT drop the pending revert");
            mh::caps::quiescent_point();
            ck(s_reverts == 3 && !raised(), "(13b) ...it runs at the quiescent point");
            ck(set_limits(true, "t"), "(13b) raise for the last arm");
            note_world_ended();
            ck(set_limits(false, "t"), "(13b) back down");
        }

        mh::patch::set_host(nullptr);
        mh::patch::set_logger(nullptr);
        free(g_img);
        g_img = nullptr;
        free(g_sec);
        g_sec = nullptr;
    }
    // ---- (14) PT-CAP11: the AI scratch layer's guard bookkeeping over a heap section --------------------
    {
        namespace asl = mh::caps::ai_scratch_layout;
        reset_stock();
        mh::caps::set_logger(&cap_log);
        ck(!mh::caps::ai_adopt(0) && !mh::caps::ai_on() && mh::caps::ai_guard_check() == 0, "(14) no section: the AI layer is off and the guard check is a no-op");
        uint8_t *sec = static_cast<uint8_t *>(calloc(1, asl::SECTION_VSIZE));
        ck(sec != nullptr, "...a zeroed stand-in for .mhcapa");
        if (sec) {
            ck(mh::caps::ai_adopt(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(sec))), "ai_adopt takes the section");
            bool fill = true;
            for (unsigned i = 0; i < asl::COUNT; ++i) {
                const uint8_t *gp = sec + asl::ARRAYS[i].off + asl::ARRAYS[i].bytes;
                for (unsigned k = 0; k < asl::GUARD_BYTES; ++k) fill = fill && gp[k] == asl::GUARD_FILL;
                ck(asl::ARRAYS[i].count > asl::ARRAYS[i].stock_count, "...every array is enlarged past its vanilla size");
                ck(asl::ARRAYS[i].off + asl::ARRAYS[i].bytes + asl::GUARD_BYTES <= asl::SECTION_VSIZE, "...and fits the section with its guard");
            }
            ck(fill, "...a 0xA5 guard follows every array");
            ck(mh::caps::ai_guard_check() == 0, "...and an untouched section has no guard hit");
            sec[asl::ARRAYS[5].off + asl::ARRAYS[5].bytes + 3] = 0;
            ck(mh::caps::ai_guard_check() == (1u << 5) && cap_logged("AI GUARD HIT"), "a clobbered guard byte after engage_scratch is a red verdict naming the array");
            ck(mh::caps::ai_guard_check() == (1u << 5), "...and the verdict is sticky");
            free(sec);
        }
        reset_stock();
        ck(mh::caps::ai_guard_check() == 0 && !mh::caps::ai_on(), "reset_stock() clears the AI layer");
    }
    // ---- (15) PT-CAP9-U4: the path-slot pool (mh::caps::path) ------------------------------------------------------
    {
        namespace pl = mh::caps::path_layout;
        namespace pp = mh::caps::path;
        reset_stock();
        mh::caps::set_logger(&cap_log);
        // the generated layout is self-consistent and agrees with the address manifest and the units manifest it was cut from
        ck(pl::STOCK_BUFFERS == mh::addr::_G_LLM_STRAT_PATH_BUFFERS && pl::STOCK_COUNT == mh::addr::_G_LLM_STRAT_PATH_FREE_SLOT_COUNT,
           "(15) the layout's stock buffer / free-count addresses are the address manifest's");
        ck(pl::ROW_SLOTS == pl::RAISED_SLOTS + 1 && pl::BUF_PLAYER_BYTES == pl::ROW_SLOTS * pl::SLOT_BYTES && pl::BUF_BYTES == pl::PLAYERS * pl::BUF_PLAYER_BYTES &&
               pl::SLOT_BYTES == 600 && pl::STOCK_SLOTS == 100,
           "...rows of 501 slots (500 + the sink), 600 bytes a slot, 8 players");
        ck(pl::BUF_OFFSET % 64u == 0 && pl::FLAGS_OFFSET % 64u == 0 && pl::BUF_OFFSET + pl::BUF_BYTES <= pl::FLAGS_OFFSET && pl::FLAGS_OFFSET + pl::FLAGS_BYTES <= pl::HOST_VSIZE,
           "...buffers then flags, 64-aligned, both inside the host section");
        const mh::patch::inmem_manifest &um = *mh::patch::CAPS_UNITS_RELOC;
        ck(um.section_count == 3 && strcmp(um.sections[0].name, ".mhcapu") == 0 && strcmp(um.sections[1].name, ".mhcapp") == 0 && strcmp(um.sections[2].name, ".mhcapq") == 0 &&
               um.sections[1].vsize == pl::HOST_VSIZE && um.sections[2].data_len > 0 && (um.sections[2].chars & 0x20000000u) != 0,
           "...the units manifest carries .mhcapu, .mhcapp (the pool host, zero-init) and .mhcapq (code)");
        {
            bool inside = true;
            int  nbuf = 0, nflags = 0, nsec_u = 0;
            for (int i = 0; i < um.ref_count; ++i) {
                const mh::patch::inmem_ref &r = um.refs[i];
                if (r.kind != mh::patch::ref_kind::abs32_to_section) continue;
                if (r.target_section == 1) {
                    const bool b = r.target + 8u >= pl::BUF_OFFSET && r.target <= pl::BUF_OFFSET + pl::BUF_BYTES, f = r.target >= pl::FLAGS_OFFSET && r.target <= pl::FLAGS_OFFSET + pl::FLAGS_BYTES;
                    nbuf += b ? 1 : 0;
                    nflags += f ? 1 : 0;
                    inside = inside && (b || f);
                } else if (r.target_section == 0) {
                    ++nsec_u;
                    inside = inside && r.target < 0x100000u; // never a value BEFORE .mhcapp read as an offset into the units host
                }
            }
            ck(inside && nbuf > 100 && nflags >= 10 && nsec_u > 1000, "...every ref into .mhcapp lands in the buffers or the flags; none of the units refs spills past its host");
        }
        // geometry: not relocated -> the stock arrays; adopt -> the host
        ck(!pp::relocated() && pp::buffers() == reinterpret_cast<uint8_t *>(pl::STOCK_BUFFERS) && pp::flags() == reinterpret_cast<uint8_t *>(pl::STOCK_FLAGS) &&
               pp::buffer_player_bytes() == 60000u && pp::flags_row() == 100u && pp::slot_limit() == 100u,
           "...not relocated: stock arrays, 60000-byte player stride, 100-byte flag rows, 100 slots");
        ck(!pp::adopt(0) && !pp::relocated(), "...adopt refuses a zero base");
        uint8_t *host = static_cast<uint8_t *>(calloc(pl::HOST_VSIZE, 1));
        ck(host && pp::adopt(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(host))), "...adopt takes the host");
        ck(pp::buffers() == host + pl::BUF_OFFSET && pp::flags() == host + pl::FLAGS_OFFSET && pp::buffer_player_bytes() == pl::BUF_PLAYER_BYTES && pp::flags_row() == 501u &&
               pp::slot_limit() == 100u,
           "...relocated, raise OFF: the host, 300600-byte player stride, 501-byte flag rows, still 100 slots");
        ck(pp::waypoint(2, 7, 3) == host + pl::BUF_OFFSET + 2u * pl::BUF_PLAYER_BYTES + 7u * 600u + 6u, "...waypoint(player, slot, entry) == base + p*stride + slot*600 + entry*2");
        set_raised_for_test(true);
        ck(pp::slot_limit() == 500u, "...raised: 500 slots");
        set_raised_for_test(false);

        // the canonical mirror: gather copies slots 0..99 of every row, scatter copies them back and ZEROES the tails
        {
            std::vector<uint8_t> sb(8u * 60000u), sf(800u);
            for (size_t i = 0; i < sb.size(); ++i) sb[i] = static_cast<uint8_t>(1 + i % 251);
            for (size_t i = 0; i < sf.size(); ++i) sf[i] = static_cast<uint8_t>(i % 2);
            pp::set_stock_for_test(sb.data(), sf.data());
            memset(host, 0xAB, pl::HOST_VSIZE);
            pp::scatter();
            bool head = true, tail = true, ftail = true, fhead = true;
            for (uint32_t pl_ = 0; pl_ < 8; ++pl_) {
                const uint8_t *b = host + pl::BUF_OFFSET + pl_ * pl::BUF_PLAYER_BYTES;
                head             = head && memcmp(b, sb.data() + pl_ * 60000u, 60000u) == 0;
                for (uint32_t k = 60000u; k < pl::BUF_PLAYER_BYTES; ++k) tail = tail && b[k] == 0;
                const uint8_t *f = host + pl::FLAGS_OFFSET + pl_ * 501u;
                fhead            = fhead && memcmp(f, sf.data() + pl_ * 100u, 100u) == 0;
                for (uint32_t k = 100; k < 501u; ++k) ftail = ftail && f[k] == 0;
            }
            ck(head && tail && fhead && ftail, "(15) scatter(): slots 0..99 of every row come from the stock mirror, slots 100..500 are zeroed");
            host[pl::BUF_OFFSET + 3u * pl::BUF_PLAYER_BYTES + 5u]           = 0x5A;
            host[pl::FLAGS_OFFSET + 3u * 501u + 7u]                         = 9;
            host[pl::BUF_OFFSET + 3u * pl::BUF_PLAYER_BYTES + 60000u + 11u] = 0x77; // past the window: never copied
            pp::gather();
            ck(sb[3u * 60000u + 5u] == 0x5A && sf[3u * 100u + 7u] == 9, "...gather(): the wide rows' first 100 slots land in the mirror");
            bool untouched = sb[3u * 60000u + 6u] == static_cast<uint8_t>(1 + (3u * 60000u + 6u) % 251);
            ck(untouched, "...and gather copies nothing else");
            pp::set_stock_for_test(nullptr, nullptr);
        }

        // the unit record: the byte-shaped and the 16-bit id
        uint8_t unit[233]     = {};
        unit[pl::ID_BYTE_OFF] = 42;
        ck(pp::unit_slot(unit) == 42u && pp::unit_slot_u8(unit) == 42, "...raise OFF: the id is the byte");
        unit[pl::ID_BYTE_OFF] = 0xff;
        ck(pp::unit_slot(unit) == pp::NO_PATH && pp::unit_slot_u8(unit) == 0xff, "...raise OFF: 0xff is no path");
        set_raised_for_test(true);
        {
            const uint16_t w = 321;
            memcpy(unit + pl::ID_WORD_OFF, &w, 2);
            unit[pl::ID_BYTE_OFF] = 0xfe;
            ck(pp::unit_slot(unit) == 321u, "...raised: the id is the 16-bit field");
            ck(pp::unit_slot_u8(unit) == 0xff, "...raised: a byte-shaped reader sees an id past 0xfd as `no path` (fails safe, never indexes another unit's slot)");
            const uint16_t w2 = 0xfe;
            memcpy(unit + pl::ID_WORD_OFF, &w2, 2);
            ck(pp::unit_slot_u8(unit) == 0xfe, "...but id 254 itself is exact");
            const uint16_t w3 = 77;
            memcpy(unit + pl::ID_WORD_OFF, &w3, 2);
            unit[pl::ID_BYTE_OFF] = 77;
            ck(pp::unit_slot_u8(unit) == 77 && pp::unit_slot(unit) == 77u, "...raised: below 0xfe the byte is the id");
            const uint16_t w4 = pl::RAISED_SLOTS;
            memcpy(unit + pl::ID_WORD_OFF, &w4, 2);
            unit[pl::ID_BYTE_OFF] = 0xff;
            ck(pp::unit_slot(unit) == pp::NO_PATH, "...raised: 500 is no path");
        }
        set_raised_for_test(false);

        // after_load + check over a fake unit row (units relocated into a heap block)
        {
            uint8_t *ublock = static_cast<uint8_t *>(calloc(0x100000u, 1));
            ck(ublock && adopt_units_relocation(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ublock))), "...a stand-in units host is adopted");
            auto    U      = [&](uint32_t p, uint32_t i) { return rec<uint8_t>(UNITS, p, i); };
            int32_t cnt[8] = {1, 2, 3, 4, 5, 6, 7, 8};
            pp::set_stock_for_test(nullptr, nullptr, cnt);
            // raise ON: units of player 1 -- slot 5 busy, slot 7 free (a stale id), slot 150 (outside the planet file), no path, slot 9 busy
            memset(host, 0, pl::HOST_VSIZE);
            uint8_t *f1 = host + pl::FLAGS_OFFSET + 1u * 501u;
            f1[5]       = 1;
            f1[9]       = 1;
            auto mk     = [&](uint32_t i, uint8_t byte) {
                uint8_t *u = U(1, i);
                memset(u, 0, 233);
                u[2]               = 1; // unit_proto_id != 0: the slot is in use
                u[pl::ID_BYTE_OFF] = byte;
            };
            mk(1, 5);
            mk(2, 7);
            mk(3, 150);
            mk(4, 0xff);
            mk(5, 9);
            set_raised_for_test(true);
            cap_log("");
            pp::after_load();
            ck(pp::unit_slot(U(1, 1)) == 5u && pp::unit_slot(U(1, 5)) == 9u && pp::unit_slot(U(1, 2)) == pp::NO_PATH && pp::unit_slot(U(1, 3)) == pp::NO_PATH &&
                   pp::unit_slot(U(1, 4)) == pp::NO_PATH,
               "...after_load: a busy slot < 100 keeps its id, a free slot / a slot outside the file / no path becomes `no path`");
            ck(U(1, 2)[pl::ID_BYTE_OFF] == 0xff && U(1, 3)[pl::ID_BYTE_OFF] == 0xff && U(1, 1)[pl::ID_BYTE_OFF] == 5, "...and the byte follows (0xff for a dropped id)");
            ck(cnt[1] == 498 && cnt[0] == 500 && cnt[7] == 500, "...the free count is recomputed from the busy flags (500 - 2 for player 1, 500 for the empty rows)");
            ck(cap_logged("after the load 2 unit path(s) kept"), "...and the load is summarised in one line");
            ck(pp::check_now() == 0, "...the loaded state passes every invariant");
            const pp::census c = pp::take_census(1);
            ck(c.holding == 2 && c.max_id == 9 && c.flags_set == 2, "...census: two units hold paths, highest id 9, two busy flags");
            // the invariants (the free count is the game's stock array: skip that kind by making it irrelevant -- raise it to match)
            // a holder of a free slot is a violation
            f1[9] = 0;
            cap_log("");
            const uint32_t v0 = pp::violations_total();
            const uint32_t v  = pp::check_now();
            ck(v >= 1 && pp::violations_total() > v0 && cap_logged("holder of a free slot"), "...check_now: a unit holding a slot whose flag is clear is reported by name");
            f1[9] = 1;
            U(1, 5)
            [pl::ID_BYTE_OFF] = 3; // byte and word disagree
            cap_log("");
            const uint32_t v2 = pp::check_now();
            ck(v2 >= 1 && cap_logged("byte/word disagree"), "...check_now: a byte that is not min(id, 0xfe) is reported");
            U(1, 5)
            [pl::ID_BYTE_OFF]                         = 9;
            host[pl::FLAGS_OFFSET + 1u * 501u + 500u] = 1;
            cap_log("");
            ck(pp::check_now() >= 1 && cap_logged("sink slot"), "...check_now: a busy flag on the sink slot is reported");
            host[pl::FLAGS_OFFSET + 1u * 501u + 500u] = 0;
            set_raised_for_test(false);
            // raise OFF: the hole must stay unwritten
            U(2, 1)
            [2]                 = 1;
            const uint16_t junk = 5;
            memcpy(U(2, 1) + pl::ID_WORD_OFF, &junk, 2);
            cap_log("");
            ck(pp::check_now() >= 1 && cap_logged("hole written"), "...check_now with the raise OFF: a written hole is reported");
            free(ublock);
            reset_stock();
            pp::reset();
            pp::adopt(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(host)));
        }

        // U4e: a promoted pool function refuses the raise
        {
            static const char *OWNER = "sim_path_alloc_slot";
            pp::set_promoted_probe_for_test([](uintptr_t a) -> const char * { return a == pl::POOL_FUNCTIONS[3] ? OWNER : nullptr; });
            ck(pp::libmh_blocks() == OWNER, "(15e) libmh_blocks() names the promoted pool function");
            cap_log("");
            ck(!set_limits(true, "capstest") && cap_logged("libmh path functions are installed"), "...set_limits(true) is REFUSED with the libmh line, before anything is touched");
            pp::set_promoted_probe_for_test([](uintptr_t) -> const char * { return nullptr; });
            ck(pp::libmh_blocks() == nullptr, "...no promoted pool function: not blocked");
            pp::set_promoted_probe_for_test(nullptr);
        }
        pp::set_stock_for_test(nullptr, nullptr, nullptr);
        free(host);
        pp::reset();
        reset_stock();
        ck(!pp::relocated(), "...reset_stock() un-adopts the pool");
    }
    // ---- (16) PT-CAP9-U5: the order layer over heap stand-ins (no game memory, no manifest) ----------------
    {
        namespace od = mh::caps::orders;
        namespace ol = mh::caps::orders_layout;
        reset_stock();
        mh::caps::set_logger(&cap_log);
        static uint8_t sq[300 * 0x44], sp[1000 * 0x44], ss[300 * 0x44];
        static int32_t cq = 0, cp = 0, cs = 0;
        od::set_stock_for_test(od::QUEUE, sq, &cq);
        od::set_stock_for_test(od::PENDING, sp, &cp);
        od::set_stock_for_test(od::STAGING, ss, &cs);
        ck(!od::on() && !od::staged() && od::cap(od::QUEUE) == 300 && od::cap(od::PENDING) == 1000 && od::cap(od::STAGING) == 300 && od::base(od::QUEUE) == sq,
           "(16) not staged: the geometry is the vanilla one (300 / 1000 / 300 over the stock arrays)");
        ck(od::wide_cap(od::QUEUE) == 4096 && od::wide_cap(od::PENDING) == 32768 && od::wide_cap(od::STAGING) == 2048, "...and the raised bounds are 4096 / 32768 / 2048");
        ck(!od::adopt(0), "adopt(0) is refused");
        uint8_t *sec = static_cast<uint8_t *>(calloc(1, ol::SECTION_VSIZE));
        ck(sec != nullptr, "...a zeroed stand-in for .mhcapo");
        if (sec) {
            ck(od::adopt(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(sec))), "adopt takes the section");
            bool fill = true;
            for (unsigned i = 0; i < ol::COUNT; ++i) {
                const uint8_t *gp = sec + ol::ARRAYS[i].off + ol::ARRAYS[i].bytes;
                for (unsigned k = 0; k < ol::GUARD_BYTES; ++k) fill = fill && gp[k] == ol::GUARD_FILL;
                ck(ol::ARRAYS[i].off + ol::ARRAYS[i].bytes + ol::GUARD_BYTES <= ol::SECTION_VSIZE, "...every array fits the section with its guard");
            }
            ck(fill, "...a 0xA5 guard follows every array");
            uint32_t thunk;
            memcpy(&thunk, sec + ol::CELL_THUNK_CELL, 4);
            ck(thunk != 0, "...and the thunk cell holds the quota stub's address");
            ck(od::staged() == false, "...but without a staged manifest the layer is not staged (toggle refuses)");
            ck(!od::toggle(true, "t") && cap_logged("not staged"), "toggle(ON) without a staged manifest REFUSES, saying so");
            od::set_on_for_test(true);
            ck(od::on() && od::cap(od::QUEUE) == 4096 && od::cap(od::PENDING) == 32768 && od::cap(od::STAGING) == 2048 && od::base(od::QUEUE) == sec + ol::ARRAYS[0].off,
               "ON: the geometry is the wide one over .mhcapo");
            // gather / scatter / clamp
            for (int i = 0; i < 300 * 0x44; ++i) sec[ol::ARRAYS[0].off + i] = static_cast<uint8_t>(i * 7 + 1);
            od::gather();
            ck(memcmp(sq, sec + ol::ARRAYS[0].off, 300 * 0x44) == 0, "gather copies the wide queue's first 300 records into the stock mirror");
            // write past the window, then scatter a different stock image
            for (int i = 0; i < 0x44; ++i) sec[ol::ARRAYS[0].off + 400 * 0x44 + i] = 0xEE;
            for (int i = 0; i < 300 * 0x44; ++i) sq[i] = static_cast<uint8_t>(i * 3 + 5);
            cq = 450; // a loaded count past the window
            od::scatter();
            ck(memcmp(sec + ol::ARRAYS[0].off, sq, 300 * 0x44) == 0 && sec[ol::ARRAYS[0].off + 400 * 0x44] == 0, "scatter copies the stock window in and ZEROES the wide tail");
            od::finish_scatter(); // PT-CAP9-U7c: no MHOV blob followed, so the clamp stands and is logged here
            ck(cq == 300 && cap_logged("clamped"), "...and clamps a loaded count past the window, logging it (finish_scatter, no blob followed)");
            // census: records fill from 0, non-zero; hw follows
            memset(sec + ol::ARRAYS[1].off, 0, ol::ARRAYS[1].bytes);
            for (int r = 0; r < 700; ++r) memset(sec + ol::ARRAYS[1].off + r * 0x44, 0x11, 0x44);
            od::step();
            ck(od::high_water(od::PENDING) == 700, "step(): the pending high-water is the highest non-empty record (700)");
            for (int r = 700; r < 1500; ++r) memset(sec + ol::ARRAYS[1].off + r * 0x44, 0x11, 0x44);
            od::step();
            ck(od::high_water(od::PENDING) == 1500 && od::refused(od::PENDING) == 0, "...and passes the vanilla 1000 with no refusal counted");
            reinterpret_cast<volatile uint32_t *>(sec + ol::CELL_OVF_QUEUE)[0] = 3;
            od::step();
            ck(od::refused(od::QUEUE) == 3 && cap_logged("ovf q=3"), "a refusal counted by the patched wipe shows in the census line");
            sec[ol::ARRAYS[2].off + ol::ARRAYS[2].bytes + 5] = 0;
            od::step();
            ck((od::guard_hits() & 4u) != 0 && cap_logged("guard=0x4"), "a clobbered guard after the staging array is a red verdict (sticky bit 2 in the census line)");
            // the sender quota
            od::quota_state q = {};
            ck(od::quota_accept(q, 1, false, 0, 0) && od::quota_accept(q, 1, true, 1, 1), "quota: an empty player gets an order and its echo");
            ck(od::quota_accept(q, 1, false, 10, od::QUOTA - 2) && od::quota_accept(q, 1, true, 11, od::QUOTA - 1), "...a primary at QUOTA-2 still fits (with its echo)");
            ck(!od::quota_accept(q, 2, false, 10, od::QUOTA - 1) && !od::quota_accept(q, 2, true, 10, od::QUOTA - 1), "...a primary at QUOTA-1 is refused, and so is its echo (never split)");
            ck(od::quota_accept(q, 3, false, 0, 0) && !od::quota_accept(q, 2, true, 0, 0), "...the orphan flag is per player");
            ck(!od::quota_accept(q, 4, false, od::wide_cap(od::STAGING) - 1, 0), "...a full staging list refuses (a primary needs two free records)");
            ck(od::quota_accept(q, 4, false, 0, 0) && od::quota_accept(q, 4, true, 1, 1), "...and the next order of the player is accepted again");
            od::set_on_for_test(false);
            ck(!od::on() && od::cap(od::QUEUE) == 300, "OFF: back to the vanilla geometry");
            free(sec);
        }
        reset_stock();
        ck(!od::on() && !od::staged(), "reset_stock() clears the order layer");
    }
    free(block);
    mh::caps::set_logger(nullptr);
    reset_stock();
    mh::hook::registry_reset_for_test();
    mh::patch::reset_applied_for_test();
    ck(!relocated() && !raised() && mirrors_match_table(), "reset_stock() clears relocated(), raised() and the staged limit layer too");

    printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
