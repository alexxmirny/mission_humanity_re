// seams/ui_info_guard.cpp -- mp:U73: an entity info screen is never opened for a (kind, entity_id) that
// can only produce the "Error: Cannot find info text:" modal. Mechanism, evidence and knobs:
// include/mh_infoguard_export.h. The decision is in ui_info_guard.h (selftested by
// `net_selftest.exe infoguardtest`).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "mh_ini_gate.h" // RL2: the ship gate every ini read goes through
#include <cstdint>
#include <cstring>

#include "include/mh_infoguard_export.h"
#include "addr/mh_addrs.gen.h"   // mh::addr::G_BUILDING_COUNT_TOTAL / G_UNIT_COUNT_TOTAL / PlayerSide
#include "addr/mh_regions.gen.h" // mh::state::live(), RID_UNIT_STORAGE / RID_UNITS
#include "addr/mh_structs.gen.h" // mh_cfg_final_struct_* / mh_map_object_unit*
#include "config/config.h"       // mh::config::mode() -- mode=original keeps retail
#include "hook/detour.h"         // install_trampoline + WATCOM_PROLOGUE
#include "hook/patch.h"          // patch_bytes_guarded
#include "hook/promoted.h"       // promoted_owner_of
#include "state/roster_caps.h"   // live_roster_caps()
#include "net_internal.h"        // seam_log; g_ini
#include "en_guard.h"            // EN-only build gate
#include "ui_info_guard.h"       // the pure decision

#pragma comment(lib, "user32.lib") // wsprintfA

namespace {

using namespace mh::info_guard;

// ---- the addresses (EN, read from the disassembly / DB 2026-10-04) --------------------------------
//
// Raw VAs with provenance, like seams/ui_info_avi.cpp: UI-only globals and code sites, and a manifest
// data entry renumbers the harness hash registry (the fixture-fingerprint trap).
constexpr uintptr_t ADDR_INFO_OPEN = 0x004cad31u; // llm_ui_entity_info_screen_open (__watcall EAX=id EDX=kind)
// llm_strat_ui_storage_bldg_panel (0x00417409): `CMP dword ptr [0x0050a824],0 ; JZ +0x66` -- the test of
// _G_LLM_UI_STORAGE_PANEL_SHOW_INFO_SLOT_REQUEST that guards the docked-unit info open.
constexpr uintptr_t ADDR_STORAGE_REQ_TEST = 0x00417902u;
constexpr uint8_t   REQ_TEST_EXPECT[7]    = {0x83, 0x3D, 0x24, 0xA8, 0x50, 0x00, 0x00};
constexpr uintptr_t ADDR_STORAGE_REQ      = 0x0050a824u; // _G_LLM_UI_STORAGE_PANEL_SHOW_INFO_SLOT_REQUEST (int, row+1)
constexpr uintptr_t ADDR_STORAGE_SCROLL   = 0x00510049u; // _G_LLM_UI_SCROLL_DESC_STORAGE.scroll_row_offset (+0x29)
// [EBP-0x18] in the panel's frame = &buildings[PlayerSide][selected] (consume_thunk pushes it).

constexpr uintptr_t ADDR_CFG_BUILDING = 0x00d9ec80u; // Building[100], stride 0x842
constexpr uintptr_t ADDR_CFG_UNIT     = 0x00e4a098u; // Unit[100], stride 0x23f
constexpr uintptr_t ADDR_CFG_PROJECTS = 0x00be1c60u; // Projects[100], stride 0xd0
constexpr uintptr_t ADDR_CFG_PROGRESS = 0x00e162e4u; // Progress[300] (cfg_final_struct_Invention), stride 0x67
constexpr int       PROGRESS_N        = 300;

constexpr uintptr_t ADDR_INDEX_BEGIN = 0x00654472u; // _G_LLM_UI_INFO_TXT_INDEX_BEGIN (void **)
constexpr uintptr_t ADDR_INDEX_END   = 0x00654476u; // _G_LLM_UI_INFO_TXT_INDEX_END
constexpr uintptr_t ADDR_INDEX_RES   = 0x0065447au; // _G_LLM_UI_INFO_TXT_RESOURCE_PTR (null until first open builds it)

// The scroll-list descriptors a diagnostic line reads: +0x29 scroll_row_offset, +0x2d item_count.
struct list_desc {
    const char *tag;
    uintptr_t   base;
};
constexpr list_desc LISTS[] = {
    {"units", 0x0050ffefu},
    {"stor", 0x00510020u},
    {"proj", 0x00510051u},
    {"ctrl", 0x00510082u},
    {"avail", 0x00510115u},
    {"owned", 0x00510146u},
};
constexpr uintptr_t ADDR_SELECTED_BLDG = 0x00e5813cu; // _G_LLM_STRAT_UI_SELECTED_BLDG_INDEX (ushort)

static_assert(sizeof(mh::game::mh_cfg_final_struct_Building) == 0x842, "cfg Building stride");
static_assert(sizeof(mh::game::mh_cfg_final_struct_Unit) == 0x23f, "cfg Unit stride");
static_assert(sizeof(mh::game::mh_cfg_final_struct_Project) == 0xd0, "cfg Project stride");
static_assert(sizeof(mh::game::mh_map_object_unit_storage) == 0xf4, "unit_storage stride");

template <class T>
T &at(uintptr_t a) { return *reinterpret_cast<T *>(a); }

constexpr int MAX_LOGS = 48;

bool  g_entry_armed   = false;
bool  g_consume_armed = false;
void *g_tramp         = nullptr; // the stolen-prologue thunk: `jmp [g_tramp]` runs the retail function
int   g_logs          = 0;
long  g_blocked       = 0; // entry calls refused
long  g_dropped       = 0; // storage requests dropped at the consume site

// The rig's knobs (`[net] info_guard_test_entry` / `info_guard_test_row`): the n-th call is corrupted.
int g_test_entry_n = 0, g_test_row_n = 0;
int g_test_entry_seen = 0, g_test_row_seen = 0;

// ---- resolving what the retail function resolves ---------------------------------------------------

const char *cfg_key(uint32_t kind, int32_t id) {
    using namespace mh::game;
    if (kind == KIND_UNIT)
        return reinterpret_cast<const mh_cfg_final_struct_Unit *>(ADDR_CFG_UNIT)[id].info_txt;
    if (kind == KIND_BUILDING)
        return reinterpret_cast<const mh_cfg_final_struct_Building *>(ADDR_CFG_BUILDING)[id].info_txt;
    return reinterpret_cast<const mh_cfg_final_struct_Project *>(ADDR_CFG_PROJECTS)[id].info_txt;
}

// Retail's PROJECT fallback (0x004cadde..): a project with no INFO_TXT of its own borrows the key of a
// building/unit whose invention depends on it. No `break` in retail -- the LAST match wins -- and the
// candidate's index is unchecked there; here an index outside its table is skipped (no key to read).
const char *project_key(int32_t id) {
    using namespace mh::game;
    const auto *p   = reinterpret_cast<const mh_cfg_final_struct_Project *>(ADDR_CFG_PROJECTS) + id;
    const char *key = p->info_txt;
    if (key[0] != '\0') return key;
    const uint8_t *prog = reinterpret_cast<const uint8_t *>(ADDR_CFG_PROGRESS);
    for (int i = 0; i < PROGRESS_N; ++i, prog += 0x67) {
        const uint8_t  type = prog[32];
        const uint16_t idx  = *reinterpret_cast<const uint16_t *>(prog + 33);
        for (int k = 0; k < 16; ++k) {
            const uint16_t dep = *reinterpret_cast<const uint16_t *>(prog + 2 * k);
            if ((int32_t)dep == p->invention && type != INV_UNDEFINED) {
                if (type == INV_BUILDING && idx < CFG_TABLE_DEPTH) key = cfg_key(KIND_BUILDING, idx);
                else if (type == INV_UNIT && idx < CFG_TABLE_DEPTH) key = cfg_key(KIND_UNIT, idx);
            }
        }
    }
    return key;
}

// Is `key` in the INFO.Txt index? The index is built lazily by the FIRST open, so before that (or when
// the resource did not load) there is nothing to judge by and the answer is "yes" -- the guard only
// refuses what it can prove retail would refuse. Retail compares with utils_w_str_cmp: exact,
// case-sensitive, over the ANSI key widened by llm_str_ansi_to_wide; an ASCII key widens to itself and
// a key with any high byte is not judged here.
bool key_in_index(const char *key) {
    void **begin = at<void **>(ADDR_INDEX_BEGIN);
    void **end   = at<void **>(ADDR_INDEX_END);
    if (at<void *>(ADDR_INDEX_RES) == nullptr || begin == nullptr || begin == end) return true;
    wchar_t w[INFO_KEY_BYTES + 1];
    size_t  n = 0;
    for (; key[n] && n < INFO_KEY_BYTES; ++n) {
        if ((unsigned char)key[n] >= 0x80) return true;
        w[n] = (wchar_t)(unsigned char)key[n];
    }
    w[n] = 0;
    for (void **e = begin; e != end; e += 2) {
        const wchar_t *k = static_cast<const wchar_t *>(e[0]);
        if (k != nullptr && lstrcmpW(k, w) == 0) return true;
    }
    return false;
}

verdict judge(uint32_t kind, int32_t id, const char *&key_out) {
    static const char kBlank[1] = {0};
    key_out                     = kBlank;
    if (kind > KIND_PROJECT) return verdict::kind_range;
    // The cfg section's .total (G_UNIT_COUNT_TOTAL / G_BUILDING_COUNT_TOTAL) is NOT used to refuse: a
    // record past it has an empty INFO_TXT by construction, which key_shape already catches, and a
    // wrong reading of what .total counts would refuse a valid screen. It is logged instead.
    if (!id_in_range(kind, id, 0)) return verdict::id_range;
    const char *key = kind == KIND_PROJECT ? project_key(id) : cfg_key(kind, id);
    key_out         = key;
    const verdict s = key_shape(key);
    if (s != verdict::ok) return s;
    if (!key_in_index(key)) return verdict::key_not_indexed;
    return verdict::ok;
}

// A printable, bounded copy of a key for the log line (a blank slot prints as "").
void printable(const char *key, char *out, size_t cap) {
    size_t n = 0;
    for (; key[n] && n < INFO_KEY_BYTES && n + 1 < cap; ++n) {
        const unsigned char c = (unsigned char)key[n];
        out[n]                = (c >= 0x20 && c < 0x7f && c != '"') ? (char)c : '.';
    }
    out[n] = 0;
}

// What the panels were showing -- cheap reads only. The callers zero their own request latches BEFORE
// they call the open, so those are no use here; the lists' scroll offset / item count are the state
// that explains an index past the end.
void ui_state(char *out, size_t cap) {
    size_t n = 0;
    for (const auto &l : LISTS) {
        const int32_t scroll = at<int32_t>(l.base + 0x29);
        const int32_t count  = at<int32_t>(l.base + 0x2d);
        n += (size_t)wsprintfA(out + n, n ? " %s=%d/%d" : "%s=%d/%d", l.tag, (int)scroll, (int)count);
        if (n + 24 >= cap) break;
    }
    wsprintfA(out + n, " sel_bldg=%u", (unsigned)at<uint16_t>(ADDR_SELECTED_BLDG));
}

// ---- 1. the entry guard ---------------------------------------------------------------------------

// Returns 1 = run retail, 0 = refuse (no screen, no modal, nothing touched).
int __cdecl guard_check(int32_t id, uint32_t kind, uint32_t ret_addr) {
    const bool injected = mh::info_guard::test_hit(g_test_entry_n, g_test_entry_seen);
    if (injected) { // the rig's forced bad call: the blank slot a stale index lands on
        id   = 0;
        kind = KIND_UNIT;
    }
    const char *key = nullptr;
    verdict     v   = judge(kind, id, key);
    if (injected && v == verdict::ok) v = verdict::key_empty; // knob must always refuse
    if (v == verdict::ok) return 1;
    ++g_blocked;
    if (g_logs < MAX_LOGS) {
        ++g_logs;
        char k[INFO_KEY_BYTES + 1], st[256], m[512];
        printable(key, k, sizeof(k));
        ui_state(st, sizeof(st));
        const int32_t total = kind == KIND_UNIT       ? at<int32_t>(mh::addr::G_UNIT_COUNT_TOTAL)
                              : kind == KIND_BUILDING ? at<int32_t>(mh::addr::G_BUILDING_COUNT_TOTAL)
                                                      : -1;
        wsprintfA(m,
                  "; [info] guard: entity info open REFUSED kind=%u id=%d reason=%s key=\"%s\" caller=%08X "
                  "side=%u cfg_total=%d lists[%s]%s (mp:U73)\n",
                  (unsigned)kind, (int)id, verdict_name(v), k, (unsigned)ret_addr,
                  (unsigned)at<uint16_t>(mh::addr::PlayerSide), (int)total, st,
                  injected ? " TEST-INJECTED" : "");
        seam_log(m);
    }
    return 0;
}

// Detour for llm_ui_entity_info_screen_open: __watcall (EAX = entity_id, EDX = kind), void, plain RET.
// Refusing returns with every register as the caller left it (a Watcom caller expects nothing of EAX
// after a void call); passing continues into the stolen-prologue thunk with EAX/EDX untouched.
// clang-format off
__declspec(naked) void info_open_thunk() {
    __asm {
        pushad
        mov  ecx, dword ptr [esp + 32]  // the caller's return address (above the 8 pushed registers)
        push ecx
        push edx                        // kind
        push eax                        // entity_id
        call guard_check
        add  esp, 12
        test eax, eax
        popad                           // does not touch the flags
        jz   refuse
        jmp  dword ptr [g_tramp]
    refuse:
        ret
    }
}
// clang-format on

// ---- 2. the storage panel's consume site -------------------------------------------------------------

// Called from the panel's frame (EBP = llm_strat_ui_storage_bldg_panel's) at its
// `CMP [_REQUEST],0`. A latched request is the row (+1) a right-click captured; the panel adds the
// scroll offset NOW and reads docked_units[row-1] -- but the docked list is not the one the click saw
// when a unit launched/died/was purged in between (in a lockstep match the UI-side purge is skipped,
// so the SIM's sub-tick purge shrinks the list on its own schedule). A request that does not name a
// live docked unit is dropped here, before retail indexes the list with it.
void __cdecl storage_gate(const uint8_t *bldg) {
    int32_t &req = at<int32_t>(ADDR_STORAGE_REQ);
    if (req == 0) return;
    const bool injected = mh::info_guard::test_hit(g_test_row_n, g_test_row_seen);
    if (injected) req += 40; // the rig's forced stale row: far past any docked list

    const uint32_t p      = at<uint16_t>(mh::addr::PlayerSide);
    const uint32_t slot   = bldg ? bldg[0xc6] : 0; // sub_id: the storage slot
    const auto     caps   = mh::state::live_roster_caps();
    const int32_t  scroll = at<int32_t>(ADDR_STORAGE_SCROLL);
    int32_t        count = 0, did = -1;
    uint32_t       proto = 0;
    if (p < 8 && (int32_t)slot < caps.storage) {
        const auto &lv = mh::state::live();
        const auto *st = reinterpret_cast<const mh::game::mh_map_object_unit_storage *>(lv.base[mh::state::RID_UNIT_STORAGE]) +
                         p * caps.storage + slot;
        count             = st->docked_count;
        const int32_t row = req + scroll;
        if (row >= 1 && row <= 50) did = st->docked_units[row - 1];
        if (did >= 0 && did < caps.units) {
            const auto *un = reinterpret_cast<const mh::game::mh_map_object_unit *>(lv.base[mh::state::RID_UNITS]) +
                             p * caps.units + did;
            proto = un->unit_proto_id;
        }
    }
    const storage_row v = storage_request_check(req, scroll, count, did, caps.units, proto);
    if (v == storage_row::ok && !injected) return;
    if (v == storage_row::ok) { /* knob: row moved by +40 but the check passed (cannot happen) */
    }
    ++g_dropped;
    if (g_logs < MAX_LOGS) {
        ++g_logs;
        char m[360];
        wsprintfA(m,
                  "; [info] guard: storage-panel info request DROPPED reason=%s request=%d scroll=%d docked=%d "
                  "slot=%u docked_unit=%d proto=%u side=%u (mp:U73)%s\n",
                  storage_row_name(v), (int)req, (int)scroll, (int)count, (unsigned)slot, (int)did,
                  (unsigned)proto, (unsigned)p, injected ? " TEST-INJECTED" : "");
        seam_log(m);
    }
    req = 0;
}

// Replaces the 7-byte `CMP dword ptr [REQUEST],0` (the JZ after it is kept): run the gate, then redo
// the compare so the original branch sees the (possibly cleared) latch.
// clang-format off
__declspec(naked) void consume_thunk() {
    __asm {
        pushad
        push dword ptr [ebp - 0x18]     // FRAME_BLDG_PTR_OFF: &buildings[PlayerSide][selected]
        call storage_gate
        add  esp, 4
        popad
        cmp  dword ptr ds:[0x0050a824], 0 // ADDR_STORAGE_REQ
        ret                              // flags survive RET: the caller's JZ reads them
    }
}
// clang-format on

} // namespace

extern "C" int MH_InfoGuard_Install(void) {
    if (!mh::en_build_ok()) return 0;
    const int st = (g_entry_armed ? 1 : 0) | (g_consume_armed ? 2 : 0);
    if (st) return st;
    char m[400];
    if (mh::config::mode() == mh::config::mode_t::original) {
        seam_log("; [info] guard KEPT ([config] mode=original) (mp:U73)\n");
        return 0;
    }
    g_test_entry_n = (int)mh_ini_get_int("net", "info_guard_test_entry", 0, g_ini);
    g_test_row_n   = (int)mh_ini_get_int("net", "info_guard_test_row", 0, g_ini);

    g_entry_armed = mh::hook::install_trampoline(ADDR_INFO_OPEN, (void *)info_open_thunk, &g_tramp, 8,
                                                 mh::hook::entry_claim::exclusive,
                                                 "the U73 entity-info guard");
    if (mh::hook::promoted_owner_of(ADDR_STORAGE_REQ_TEST)) {
        wsprintfA(m, "; [info] guard consume-site DISPLACED at %08X: the storage panel is promoted this run (mp:U73)\n",
                  (unsigned)ADDR_STORAGE_REQ_TEST);
        seam_log(m);
    } else {
        // E8 rel32 (the gate) + two NOPs over the 7-byte compare; the JZ at +7 is untouched.
        uint8_t repl[7];
        repl[0]     = 0xE8;
        int32_t rel = (int32_t)((uintptr_t)&consume_thunk - (ADDR_STORAGE_REQ_TEST + 5));
        std::memcpy(repl + 1, &rel, sizeof(rel));
        repl[5]         = 0x90;
        repl[6]         = 0x90;
        g_consume_armed = mh::hook::patch_bytes_guarded(ADDR_STORAGE_REQ_TEST, REQ_TEST_EXPECT, repl, 7);
    }
    wsprintfA(m, "; [info] guard: entry %s, storage-request consume %s%s%s (mp:U73)\n",
              g_entry_armed ? "guarded" : "NOT guarded (see the [interlock] line)",
              g_consume_armed ? "guarded" : "NOT guarded (bytes differ at 00417902 or promoted)",
              g_test_entry_n ? "; TEST info_guard_test_entry armed" : "",
              g_test_row_n ? "; TEST info_guard_test_row armed" : "");
    seam_log(m);
    return (g_entry_armed ? 1 : 0) | (g_consume_armed ? 2 : 0);
}
