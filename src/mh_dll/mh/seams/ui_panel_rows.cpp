//
// seams/ui_panel_rows.cpp -- RES7: the side-panel lists fill the panel at window heights above 768.
//
// THE PROBLEM. P6 (seams/video_mode_guards.cpp) caps every side-panel list at the rows a 768-px window shows,
// because the tables behind the lists hold that many rows: at 1280x960 the available-buildings list shows 10 rows
// and then the panel's empty body, with scrolling on. Uncapped, the rows overwrite the next records of their icon
// group (gfx/panel_rows.h has the table layout).
//
// THE FIX (option A). The tables are rebuilt in DLL memory for the tallest window the session can reach, and the
// game's code is pointed at them:
//
//   1. The icon table and its group-start array (gfx::panel_rows::build_icon_table): every list group's row slots
//      grown, inserted before the group's trailing records; new nested groups (ids >= 56) for the extra rows of the
//      lists whose slots are groups. All 41 code references to the record array (0x0050e817 operand / 0x0050e81b
//      kind) and the 3 to the group starts (0x0050bcf7) are rewritten by one uniform delta. The unpack call in
//      llm_strat_ui_panel_init (0x0041ae9f) builds the DLL table instead of the stock one.
//   2. The HUD widget table (_G_LLM_UI_HUD_WIDGETS, 332 x 28 B): relocated so it can grow past id 331 (104 code
//      references, one uniform delta; the boot fill and llm_ui_hud_widget_define then write the DLL copy). Every
//      per-row widget range is extrapolated past its stock rows (gfx::panel_rows::extend_widgets: same x, size,
//      callback and text, y one row height further per row, cb_arg = the row). The first 332 records are copied back
//      to the stock address after the boot build, so the registry region (UI_HUD_WIDGETS, view only) and every
//      reader of it keep the stock contents; nothing writes the widget table after boot except the scroll bodies
//      below, which are view state.
//   3. The selection lists' per-row state (two int[40] at 0x0050a694 / 0x0050a734, indexed by visible row): moved
//      to DLL arrays of the grown row count (8 references), with the clear loop bound in llm_ui_bldg_panel_open
//      (0x0041c0bb) and the selected-control-group half's offset in llm_ui_selection_panel_refresh (0x004199a7,
//      `add edx,0x28`) set to the grown count.
//   4. llm_ui_icon_group_dispatch's 100-record cap (0x0041b00c) lifted: group 55 holds 2 x the selection rows.
//   5. Control group 0's list (group 54) is rebuilt as one nested group per row (its stock slots hold widget pairs);
//      its descriptor's row kind (0x0051008e) becomes 5.
//   6. The one row drawer that computes a widget id from the row, llm_ui_bldg_prod_queue_slot_draw (the units list:
//      `slot + 0x101`, `slot + 0x10b` into llm_ui_selection_count_row_draw, call site 0x00418fff), gets the
//      extension ids for rows past 10.
//   7. P6's three list row counts take min(WindowHeight, the grown height) (set_panel_list_cap); the selected-unit
//      view's three keep 768. On every height change the eight scroll bodies (0x129 .. 0x13e) reach the window
//      bottom (their stock rects end at 768; control group 0's, which ends at 456 in stock, is left alone) and the
//      row slots past the new row count are hidden.
//
// Every other per-row handler reads only its cb_arg and the list's scroll offset (checked for all eight lists).
// UI only: no sim, save or lockstep state is touched.
//
// Kill switches: `[video] panel_rows=0` arms none of this (P6's 768 cap stays); `[video] mode_guards=0` arms neither.
// Every site is checked against its stock bytes before ANY is written; one mismatch arms nothing.
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "mh_ini_gate.h" // RL2: the ship gate every ini read goes through
#include <cstdarg>
#include <cstring>
#include <stdint.h>

#include "ui_panel_rows.h"
#include "video_mode_guards.h"
#include "include/mh_run_context.h" // mh_proc_path
#include "include/mh_log_sink.h"    // LOG1: async log sink
#include "addr/mh_addrs.gen.h"      // WindowHeight
#include "en_guard.h"
#include "gfx/panel_rows.h"
#include "hook/patch.h"

#pragma comment(lib, "user32.lib") // wvsprintfA

namespace {

namespace pr = mh::gfx::panel_rows;

// ---- raw VAs with provenance (no manifest data entry: that would add a registry region and stale every recorded
// fixture; precedent video_mode_guards.cpp) ------------------------------------------------------------------------
constexpr uintptr_t ADDR_ENTRIES      = 0x0050e817u; // _G_LLM_UI_ICON_ENTRIES, 562 x {int operand; byte kind}
constexpr uintptr_t ADDR_GSTART       = 0x0050bcf7u; // _G_LLM_UI_ICON_GROUP_FIRST_ENTRY, int[56]
constexpr uintptr_t ADDR_WIDGETS      = 0x0050be63u; // _G_LLM_UI_HUD_WIDGETS, llm_ui_hud_widget[332]
constexpr uintptr_t ADDR_BLOB         = 0x005102e8u; // _G_LLM_UI_ICON_GROUP_BLOB, 56 slots + the 0x38 terminator
constexpr uintptr_t ADDR_ROWSTATE_SEL = 0x0050a694u; // _G_LLM_UI_UNASSIGNED_ROW_STATE, int[40]
constexpr uintptr_t ADDR_ROWSTATE_CG  = 0x0050a734u; // the selected control group's row state, int[40]
constexpr int       STOCK_ENTRIES     = 562;
constexpr int       STOCK_ROWSTATE_N  = 40;
constexpr int       BLOB_LEN          = pr::STOCK_GROUPS * pr::BLOB_SLOT + 1;

constexpr uintptr_t ADDR_UNPACK_CALL     = 0x0041ae9fu; // llm_strat_ui_panel_init: CALL llm_ui_icon_table_unpack
constexpr uintptr_t ADDR_UNPACK          = 0x00419b98u;
constexpr uintptr_t ADDR_COUNT_ROW_CALL  = 0x00418fffu; // llm_ui_bldg_prod_queue_slot_draw: CALL llm_ui_selection_count_row_draw
constexpr uintptr_t ADDR_COUNT_ROW_DRAW  = 0x0041172fu;
constexpr uintptr_t ADDR_DISPATCH_CAP    = 0x0041b00cu; // llm_ui_icon_group_dispatch: CMP [EBP-0x20],0x64
constexpr uintptr_t ADDR_SEL_HALF        = 0x004199a7u; // llm_ui_selection_panel_refresh: ADD EDX,0x28
constexpr uintptr_t ADDR_PANEL_OPEN_LOOP = 0x0041c0bbu; // llm_ui_bldg_panel_open: CMP [EBP-0x18],0x28
constexpr uintptr_t ADDR_UA_DESC_KIND    = 0x0051008eu; // _G_LLM_UI_SCROLL_DESC_UNIT_UNASSIGNED + 0xc (row kind)

// The list descriptors (llm_ui_scroll_list_descriptor: +0 row height, +0x21 top y) and the geometry each must have.
struct desc_check {
    uintptr_t addr;
    int       list;
};
constexpr desc_check DESCS[] = {
    {0x00510115u, pr::L_BLDG},
    {0x00510146u, pr::L_BLDG},
    {0x0050ffefu, pr::L_UNITS},
    {0x00510020u, pr::L_STORE},
    {0x00510051u, pr::L_PROJ},
    {0x00510082u, pr::L_UA},
    {0x005100b3u, pr::L_SEL},
    {0x005100e4u, pr::L_SEL},
};

// The scroll-list body widgets (llm_ui_hud_scroll_list_define's base id: body, then the two arrows).
constexpr int BODY_IDS[] = {0x129, 0x12c, 0x12f, 0x132, 0x135, 0x138, 0x13b, 0x13e};
constexpr int N_BODIES   = (int)(sizeof(BODY_IDS) / sizeof(BODY_IDS[0]));

// Every code reference to the three relocated tables and the two row-state arrays: the address of the 4-byte
// absolute operand and the stock value it holds. Found by a byte scan of the EN exe's code section for values in
// each table's extent, every hit disassembled (a hit in a `sub esp,0x50` and one inside a CALL rel32 excluded);
// cross-checked against Ghidra's references.
struct ref_site {
    uintptr_t addr;
    uint32_t  stock;
};
constexpr ref_site WIDGET_REFS[] = {
    {0x00411756u, 0x0050be6fu},
    {0x00411763u, 0x0050be6bu},
    {0x00411788u, 0x0050be6bu},
    {0x004117d1u, 0x0050be6bu},
    {0x004117d7u, 0x0050be73u},
    {0x004117f8u, 0x0050be73u},
    {0x00411a66u, 0x0050be6bu},
    {0x00411a73u, 0x0050be6fu},
    {0x00411e45u, 0x0050be6bu},
    {0x00411e52u, 0x0050be6fu},
    {0x0041306bu, 0x0050be6bu},
    {0x0041307bu, 0x0050be6fu},
    {0x00413389u, 0x0050be6bu},
    {0x0041339cu, 0x0050be6fu},
    {0x004135bcu, 0x0050be6bu},
    {0x004135c9u, 0x0050be6fu},
    {0x00414339u, 0x0050c02bu},
    {0x00414342u, 0x0050c033u},
    {0x00414360u, 0x0050c02fu},
    {0x0041441cu, 0x0050bedfu},
    {0x00414421u, 0x0050bedbu},
    {0x00414539u, 0x0050c02fu},
    {0x0041453fu, 0x0050c02bu},
    {0x004145a5u, 0x0050c02fu},
    {0x004145acu, 0x0050c02bu},
    {0x00415972u, 0x0050bfd7u},
    {0x0041597au, 0x0050bfdbu},
    {0x00415e91u, 0x0050be6bu},
    {0x00415ea4u, 0x0050be6fu},
    {0x00415ef0u, 0x0050be6bu},
    {0x00415f03u, 0x0050be6fu},
    {0x004169c1u, 0x0050bfd7u},
    {0x004169c9u, 0x0050bfdbu},
    {0x00416fecu, 0x0050bfd7u},
    {0x00416ff7u, 0x0050bfdbu},
    {0x0041731cu, 0x0050e1a3u},
    {0x00417324u, 0x0050e1a7u},
    {0x0041732cu, 0x0050e22fu},
    {0x00417332u, 0x0050e1abu},
    {0x004174ecu, 0x0050bfd7u},
    {0x004174f7u, 0x0050bfdbu},
    {0x00417557u, 0x0050c293u},
    {0x0041755fu, 0x0050c297u},
    {0x00417578u, 0x0050c2afu},
    {0x00417591u, 0x0050c2cbu},
    {0x004175acu, 0x0050c1cfu},
    {0x004175b4u, 0x0050c1d3u},
    {0x004175cdu, 0x0050c1ebu},
    {0x00417d01u, 0x0050c207u},
    {0x00417d09u, 0x0050c20bu},
    {0x00417d41u, 0x0050c223u},
    {0x00417d5au, 0x0050c22fu},
    {0x00418084u, 0x0050bfd7u},
    {0x0041808fu, 0x0050bfdbu},
    {0x004181c8u, 0x0050c23fu},
    {0x004181d0u, 0x0050c243u},
    {0x004181efu, 0x0050c25bu},
    {0x00418208u, 0x0050c277u},
    {0x00418221u, 0x0050c283u},
    {0x004194e4u, 0x0050be6bu},
    {0x004194f8u, 0x0050be6fu},
    {0x004199e2u, 0x0050deafu},
    {0x004199eau, 0x0050deb3u},
    {0x00419a5du, 0x0050decbu},
    {0x00419d29u, 0x0050be7bu},
    {0x00419d36u, 0x0050be6bu},
    {0x00419d43u, 0x0050be6fu},
    {0x00419d50u, 0x0050be73u},
    {0x00419d5du, 0x0050be77u},
    {0x00419d6au, 0x0050be63u},
    {0x00419d77u, 0x0050be67u},
    {0x0041ae1eu, 0x0050be63u},
    {0x0041af6au, 0x0050be6bu},
    {0x0041af7cu, 0x0050be73u},
    {0x0041af90u, 0x0050be6fu},
    {0x0041afa4u, 0x0050be77u},
    {0x0041b0edu, 0x0050be7bu},
    {0x0041b140u, 0x0050be7bu},
    {0x0041b191u, 0x0050be67u},
    {0x0041b19au, 0x0050be63u},
    {0x0041b677u, 0x0050be6bu},
    {0x0041b68eu, 0x0050be73u},
    {0x0041b69cu, 0x0050be6fu},
    {0x0041b6b0u, 0x0050be77u},
    {0x0041b705u, 0x0050be6bu},
    {0x0041b719u, 0x0050be73u},
    {0x0041b72du, 0x0050be6fu},
    {0x0041b741u, 0x0050be77u},
    {0x0041b796u, 0x0050be6bu},
    {0x0041b7aau, 0x0050be73u},
    {0x0041b7beu, 0x0050be6fu},
    {0x0041b7d2u, 0x0050be77u},
    {0x0041b827u, 0x0050be6bu},
    {0x0041b83bu, 0x0050be73u},
    {0x0041b84fu, 0x0050be6fu},
    {0x0041b863u, 0x0050be77u},
    {0x0041b8b8u, 0x0050be6bu},
    {0x0041b8ccu, 0x0050be73u},
    {0x0041b8e0u, 0x0050be6fu},
    {0x0041b8f4u, 0x0050be77u},
    {0x0041b973u, 0x0050be6fu},
    {0x0041b97du, 0x0050be6bu},
    {0x0041bab7u, 0x0050be6fu},
    {0x0041bac1u, 0x0050be6bu},
};
constexpr ref_site ENTRY_REFS[] = {
    {0x0041447bu, 0x0050e817u},
    {0x00414498u, 0x0050e817u},
    {0x004144b5u, 0x0050e817u},
    {0x0041590bu, 0x0050e817u},
    {0x0041591eu, 0x0050e817u},
    {0x00415c31u, 0x0050e817u},
    {0x00415c44u, 0x0050e817u},
    {0x00415c57u, 0x0050e817u},
    {0x00415d64u, 0x0050e81bu},
    {0x00415deau, 0x0050e81bu},
    {0x00415e38u, 0x0050e81bu},
    {0x00416ad4u, 0x0050e817u},
    {0x00416c26u, 0x0050e817u},
    {0x00416c3cu, 0x0050e817u},
    {0x00416c52u, 0x0050e817u},
    {0x00416c68u, 0x0050e817u},
    {0x00416c7eu, 0x0050e817u},
    {0x00416c94u, 0x0050e817u},
    {0x00416caau, 0x0050e817u},
    {0x00416cc0u, 0x0050e817u},
    {0x00416cd6u, 0x0050e817u},
    {0x00416ce9u, 0x0050e817u},
    {0x00416cfcu, 0x0050e817u},
    {0x00416d0fu, 0x0050e817u},
    {0x00416d22u, 0x0050e817u},
    {0x00416d35u, 0x0050e817u},
    {0x0041931eu, 0x0050e81bu},
    {0x0041932eu, 0x0050e81bu},
    {0x00419379u, 0x0050e81bu},
    {0x0041941du, 0x0050e817u},
    {0x00419430u, 0x0050e817u},
    {0x00419650u, 0x0050e81bu},
    {0x00419660u, 0x0050e81bu},
    {0x004196abu, 0x0050e81bu},
    {0x004198c1u, 0x0050e81bu},
    {0x004198d1u, 0x0050e81bu},
    {0x0041991cu, 0x0050e81bu},
    {0x00419beau, 0x0050e817u},
    {0x0041b005u, 0x0050e81bu},
    {0x0041b04bu, 0x0050e817u},
    {0x0041b05au, 0x0050e81bu},
};
constexpr ref_site GSTART_REFS[] = {
    {0x00419bd4u, 0x0050bcf7u},
    {0x0041aff9u, 0x0050bcf7u},
    {0x0041b039u, 0x0050bcf7u},
};
constexpr ref_site ROWSTATE_REFS[] = {
    {0x00411f35u, 0x0050a694u},
    {0x00411f6au, 0x0050a734u},
    {0x00412a43u, 0x0050a694u},
    {0x00412c48u, 0x0050a694u},
    {0x00412c81u, 0x0050a734u},
    {0x0041c0d3u, 0x0050a694u},
    {0x0041c0ddu, 0x0050a694u},
    {0x0041c0e9u, 0x0050a734u},
};

// ---- state ------------------------------------------------------------------------------------------------------
pr::plan        g_plan;
uint8_t        *g_entries   = nullptr;
int             g_entries_n = 0;
int32_t        *g_gstart    = nullptr;
pr::hud_widget *g_widgets   = nullptr;
int32_t        *g_rowstate  = nullptr; // [slots] selection, then [slots] selected control group
bool            g_armed     = false;
bool            g_built     = false; // the boot build ran (widgets extended, body rects captured)
int             g_body_stock_bottom[N_BODIES];

// for the units-list remap thunk (asm reads them)
uintptr_t g_count_row_draw = ADDR_COUNT_ROW_DRAW;
int       g_un_ext_n       = 0;
int       g_un_inc_base    = 0;
int       g_un_dec_base    = 0;

char          g_log_path[MAX_PATH];
unsigned long g_log_gen = 0;

// mh_video.log, a twin writer (video.cpp / video_mode_guards.cpp write the same file).
void pr_log(const char *fmt, ...) {
    mh_proc_path(g_log_path, MAX_PATH, "%smh_video.log", &g_log_gen);
    char    line[320];
    va_list ap;
    va_start(ap, fmt);
    wvsprintfA(line, fmt, ap);
    va_end(ap);
    int n = lstrlenA(line);
    if (n < (int)sizeof(line) - 2) {
        line[n++] = '\n';
        line[n]   = 0;
    }
    mh_logq_write(g_log_path, line, lstrlenA(line));
}

// ---- the boot build: runs where llm_strat_ui_panel_init called the stock unpack, after llm_strat_ui_hud_widgets_init
bool descriptors_match() {
    for (const desc_check &d : DESCS) {
        const int rowh = *(const int *)d.addr, top = *(const int *)(d.addr + 0x21);
        if (rowh != pr::LIST_GEOM[d.list].rowh || top != pr::LIST_GEOM[d.list].top) {
            pr_log("; [video] panel rows: list descriptor %08x is %d px rows from y=%d, expected %d from %d -- lists stay "
                   "at the 768 rows",
                   (unsigned)d.addr, rowh, top, pr::LIST_GEOM[d.list].rowh, pr::LIST_GEOM[d.list].top);
            return false;
        }
    }
    return true;
}

void apply_height(int h) {
    if (!g_built) return;
    for (int i = 0; i < N_BODIES; ++i) {
        const int sb                  = g_body_stock_bottom[i];
        g_widgets[BODY_IDS[i]].bottom = (h > pr::STOCK_ROWS_H && sb >= pr::STOCK_ROWS_H && h > sb) ? h : sb;
    }
    pr::hide_rows_beyond(g_entries, g_gstart, g_plan, h);
}

void __cdecl on_height(int h) { apply_height(h); }

void __cdecl on_panel_init_tables() {
    // The icon table, fresh (the stock unpack also resets every operand the panels re-point).
    const int n  = pr::build_icon_table((const uint8_t *)ADDR_BLOB, BLOB_LEN, g_plan, g_entries, g_entries_n, g_gstart,
                                        g_plan.n_groups);
    bool      ok = n == g_entries_n;
    if (!ok) pr_log("; [video] panel rows: icon table rebuild FAILED (%d records, %d expected)", n, g_entries_n);
    if (ok && !pr::extend_widgets(g_widgets, g_plan.n_widgets, g_plan)) {
        pr_log("; [video] panel rows: the per-row widget ranges do not step by their row heights -- not extended");
        ok = false;
    }
    if (ok && !descriptors_match()) ok = false;
    for (int i = 0; i < N_BODIES; ++i) g_body_stock_bottom[i] = g_widgets[BODY_IDS[i]].bottom;
    // the registry region (view only) and every stock-address reader keep the stock contents
    memcpy((void *)ADDR_WIDGETS, g_widgets, (size_t)pr::STOCK_WIDGETS * sizeof(pr::hud_widget));
    g_built = true;
    if (!ok) {
        // the grown tables stay (a superset: the rows a 768 window shows sit where they always did); the lists cap
        mh::video::set_panel_list_cap(pr::STOCK_ROWS_H, on_height);
        return;
    }
    apply_height(*(const int *)mh::addr::WindowHeight);
    static bool said = false;
    if (!said) {
        said = true;
        pr_log("; [video] panel rows: tables built for %d px: %d icon records, %d groups, %d widgets (lists: buildings %d, "
               "units %d, storage %d, projects %d, group 0 %d, selection %d rows)",
               g_plan.h_built, g_entries_n, g_plan.n_groups, g_plan.n_widgets, g_plan.rows[pr::L_BLDG],
               g_plan.rows[pr::L_UNITS], g_plan.rows[pr::L_STORE], g_plan.rows[pr::L_PROJ], g_plan.rows[pr::L_UA],
               g_plan.rows[pr::L_SEL]);
    }
}

// clang-format off
// Replaces `CALL llm_ui_icon_table_unpack` (void, preserves every register).
__declspec(naked) void unpack_thunk() {
    __asm {
        pushad
        pushfd
        call on_panel_init_tables
        popfd
        popad
        ret
    }
}

// Replaces `CALL llm_ui_selection_count_row_draw` in llm_ui_bldg_prod_queue_slot_draw (__watcall EAX = slot + 0x101,
// EDX = slot + 0x10b, EBX = count, ECX = y offset): rows past the tenth take the extension ids.
__declspec(naked) void count_row_thunk() {
    __asm {
        push eax
        sub  eax, 0x101
        cmp  eax, 10
        jb   stock
        sub  eax, 10
        cmp  eax, g_un_ext_n
        jae  stock
        mov  edx, eax
        add  eax, g_un_inc_base
        add  edx, g_un_dec_base
        add  esp, 4
        jmp  dword ptr [g_count_row_draw]
    stock:
        pop  eax
        jmp  dword ptr [g_count_row_draw]
    }
}
// clang-format on

// ---- the patch set: every site checked before any is written --------------------------------------------------
struct patch {
    uintptr_t addr;
    uint8_t   expect[8], repl[8];
    int       len;
};
constexpr int MAX_PATCHES = 200;
patch         g_patches[MAX_PATCHES];
int           g_npatches = 0;

void add_patch(uintptr_t addr, const uint8_t *expect, const uint8_t *repl, int len) {
    if (g_npatches >= MAX_PATCHES) return;
    patch &pt = g_patches[g_npatches++];
    pt.addr   = addr;
    pt.len    = len;
    memcpy(pt.expect, expect, (size_t)len);
    memcpy(pt.repl, repl, (size_t)len);
}

void add_refs(const ref_site *refs, int n, uintptr_t old_base, uintptr_t new_base) {
    for (int i = 0; i < n; ++i) {
        const uint32_t v = refs[i].stock - (uint32_t)old_base + (uint32_t)new_base;
        add_patch(refs[i].addr, (const uint8_t *)&refs[i].stock, (const uint8_t *)&v, 4);
    }
}

void add_call(uintptr_t site, uintptr_t old_target, const void *new_target) {
    uint8_t e[5] = {0xE8}, r[5] = {0xE8};
    int32_t eo = (int32_t)(old_target - (site + 5)), ro = (int32_t)((uintptr_t)new_target - (site + 5));
    memcpy(e + 1, &eo, 4);
    memcpy(r + 1, &ro, 4);
    add_patch(site, e, r, 5);
}

const char *first_mismatch() {
    static char buf[64];
    for (int i = 0; i < g_npatches; ++i)
        if (memcmp((const void *)g_patches[i].addr, g_patches[i].expect, (size_t)g_patches[i].len) != 0) {
            wsprintfA(buf, "%08x", (unsigned)g_patches[i].addr);
            return buf;
        }
    return nullptr;
}

bool apply_all() {
    int done = 0;
    for (; done < g_npatches; ++done) {
        const patch &pt = g_patches[done];
        if (!mh::hook::patch_bytes_guarded(pt.addr, pt.expect, pt.repl, pt.len)) break;
    }
    if (done == g_npatches) return true;
    pr_log("; [video] panel rows: patch at %08x refused -- rolling back %d", (unsigned)g_patches[done].addr, done);
    while (done-- > 0) {
        const patch &pt = g_patches[done];
        mh::hook::patch_bytes_guarded(pt.addr, pt.repl, pt.expect, pt.len);
    }
    return false;
}

void *alloc_zero(size_t bytes) { return VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); }

} // namespace

namespace mh::video {

bool install_panel_rows(const char *ini, int h_max) {
    if (!mh::en_build_ok() || g_armed) return g_armed;
    if (mh_ini_get_int("video", "panel_rows", 1, ini) == 0) {
        pr_log("; [video] panel rows DISABLED ([video] panel_rows=0): side-panel lists keep the 768 rows");
        return false;
    }
    if (h_max <= 0) h_max = pr::H_FALLBACK;
    if (h_max <= pr::STOCK_ROWS_H) return false; // nothing taller than the stock tables hold
    if (!panel_rows_p6_armed()) {
        pr_log("; [video] panel rows NOT armed -- the P6 row-count guard is not armed ([video] mode_guards=0?)");
        return false;
    }
    if (!pr::make_plan(h_max, g_plan)) {
        pr_log("; [video] panel rows NOT armed -- %d px needs more selection rows than the code can index", h_max);
        return false;
    }
    g_entries_n = pr::build_icon_table((const uint8_t *)ADDR_BLOB, BLOB_LEN, g_plan, nullptr, 0, nullptr, 0);
    if (g_entries_n < STOCK_ENTRIES) {
        pr_log("; [video] panel rows NOT armed -- the icon group blob at %08x is not the stock one", (unsigned)ADDR_BLOB);
        return false;
    }
    const int sel_slots = g_plan.slots[pr::G_SEL];
    g_entries           = (uint8_t *)alloc_zero((size_t)g_entries_n * pr::ENTRY_BYTES);
    g_gstart            = (int32_t *)alloc_zero((size_t)g_plan.n_groups * 4);
    g_widgets           = (pr::hud_widget *)alloc_zero((size_t)g_plan.n_widgets * sizeof(pr::hud_widget));
    g_rowstate          = (int32_t *)alloc_zero((size_t)sel_slots * 2 * 4);
    if (!g_entries || !g_gstart || !g_widgets || !g_rowstate) {
        pr_log("; [video] panel rows NOT armed -- allocation failed");
        return false;
    }
    // Valid from the start: the build is a function of the blob; the widgets and row state as they are now.
    pr::build_icon_table((const uint8_t *)ADDR_BLOB, BLOB_LEN, g_plan, g_entries, g_entries_n, g_gstart,
                         g_plan.n_groups);
    memcpy(g_widgets, (const void *)ADDR_WIDGETS, (size_t)pr::STOCK_WIDGETS * sizeof(pr::hud_widget));
    memcpy(g_rowstate, (const void *)ADDR_ROWSTATE_SEL, STOCK_ROWSTATE_N * 4);
    memcpy(g_rowstate + sel_slots, (const void *)ADDR_ROWSTATE_CG, STOCK_ROWSTATE_N * 4);

    g_un_ext_n    = g_plan.ext_n[pr::R_UN_INC];
    g_un_inc_base = g_plan.ext_base[pr::R_UN_INC];
    g_un_dec_base = g_plan.ext_base[pr::R_UN_DEC];

    g_npatches = 0;
    add_refs(WIDGET_REFS, (int)(sizeof(WIDGET_REFS) / sizeof(WIDGET_REFS[0])), ADDR_WIDGETS, (uintptr_t)g_widgets);
    add_refs(ENTRY_REFS, (int)(sizeof(ENTRY_REFS) / sizeof(ENTRY_REFS[0])), ADDR_ENTRIES, (uintptr_t)g_entries);
    add_refs(GSTART_REFS, (int)(sizeof(GSTART_REFS) / sizeof(GSTART_REFS[0])), ADDR_GSTART, (uintptr_t)g_gstart);
    for (const ref_site &r : ROWSTATE_REFS) {
        const bool cg = r.stock == ADDR_ROWSTATE_CG;
        add_refs(&r, 1, cg ? ADDR_ROWSTATE_CG : ADDR_ROWSTATE_SEL, (uintptr_t)(g_rowstate + (cg ? sel_slots : 0)));
    }
    add_call(ADDR_UNPACK_CALL, ADDR_UNPACK, (const void *)unpack_thunk);
    add_call(ADDR_COUNT_ROW_CALL, ADDR_COUNT_ROW_DRAW, (const void *)count_row_thunk);
    {
        static const uint8_t e[4] = {0x83, 0x7D, 0xE0, 0x64}; // CMP dword [EBP-0x20], 100
        const uint8_t        r[4] = {0x83, 0x7D, 0xE0, 0xFF}; // imm8 -1 = 0xffffffff: JB always taken, kind 1 ends
        add_patch(ADDR_DISPATCH_CAP, e, r, 4);
    }
    {
        static const uint8_t e[3] = {0x83, 0xC2, 0x28};
        const uint8_t        r[3] = {0x83, 0xC2, (uint8_t)sel_slots};
        add_patch(ADDR_SEL_HALF, e, r, 3);
    }
    {
        static const uint8_t e[4] = {0x83, 0x7D, 0xE8, 0x28};
        const uint8_t        r[4] = {0x83, 0x7D, 0xE8, (uint8_t)sel_slots};
        add_patch(ADDR_PANEL_OPEN_LOOP, e, r, 4);
    }
    if (g_plan.ua_nested) {
        static const uint8_t e[1] = {pr::K_WIDGET};
        static const uint8_t r[1] = {pr::K_GROUP};
        add_patch(ADDR_UA_DESC_KIND, e, r, 1);
    }

    if (const char *bad = first_mismatch()) {
        pr_log("; [video] panel rows NOT armed -- %d sites checked, %s does not hold the stock bytes", g_npatches, bad);
        return false;
    }
    if (!apply_all()) return false;
    g_armed = true;
    set_panel_list_cap(g_plan.h_built, on_height);
    pr_log("; [video] panel rows armed (%d sites): side-panel lists sized for %d px (tables built at the panel init)",
           g_npatches, g_plan.h_built);
    return true;
}

void panel_rows_on_size_change() {
    if (g_armed) apply_height(*(const int *)mh::addr::WindowHeight);
}

} // namespace mh::video
