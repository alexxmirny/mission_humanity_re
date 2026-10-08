// seams/ui_diplomacy_echo.cpp -- mp:U39: NOP the diplomacy dialog's optimistic relation echo so the
// 0xf4 order handler is the hashed cell's only writer. The mechanism, the measured skew and the
// cost are in include/mh_diploecho_export.h.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "mh_ini_gate.h" // RL2: the ship gate every ini read goes through
#include <cstdint>
#include <cstring>

#include "include/mh_diploecho_export.h"
#include "addr/mh_addrs.gen.h"    // mh::addr::diplo_echo_write_site
#include "hook/patch.h"           // patch_bytes_guarded
#include "hook/detour.h"          // install_trampoline (mp:U52)
#include "lockstep/turn_engine.h" // mp:U52: fixes().team_relations_fix
#include "state/region_runtime.h" // mp:U52: mh::state::ptr<> -- the ally-victory flag is a RELOCATABLE region
#include "net_internal.h"         // seam_log; g_ini
#include "en_guard.h"             // EN-only build gate

namespace {

constexpr int ECHO_LEN = 22;
// The exact span (docs/symbols.md llm_ui_diplomacy_apply_and_resume, EN v406): a mismatch REFUSES
// -- patch_bytes_guarded writes nothing unless every byte matches, which is what keeps a shifted or
// already-patched function intact.
const uint8_t ECHO_EXPECT[ECHO_LEN] = {0x0F, 0xB7, 0x15, 0x54, 0x83, 0xE5, 0x00, 0x6B, 0xD2, 0x34, 0x03,
                                       0x55, 0xD4, 0x8A, 0x45, 0xD8, 0x88, 0x82, 0xF1, 0x87, 0xE5, 0x00};
const uint8_t ECHO_NOPS[ECHO_LEN]   = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
                                       0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};

bool g_applied = false;

// ---- mp:U46: row advance for every non-self slot -----------------------------------------------
//
// llm_ui_diplomacy_apply_and_resume walks three spinner-row pointers ([EBP-0x24] relation, [EBP-0x1c]
// chat, [EBP-0x20] control, +0x14 per row). The builder (0x004c811c) makes one row for EVERY non-self
// slot below player_count, AI and disabled slots included, but Apply advances the pointers only on
// the human path: the self slot and every non-human slot both take `JMP 0x004c80fe` at 0x004c802e,
// which skips the ADDs at 0x004c80e9. With an AI slot ahead of a human, the human's settings are
// read from the AI's row.
//
// The shared JMP (5 bytes, E9 CB 00 00 00) is redirected to this stub: the self slot still goes to
// 0x004c80fe, any other slot goes to 0x004c80e9 (advance the three pointers, then 0x004c80fe).
// Runs in the function's own frame (EBP intact); EAX is dead at both targets.
constexpr int ROW_LEN             = 5;
const uint8_t ROW_EXPECT[ROW_LEN] = {0xE9, 0xCB, 0x00, 0x00, 0x00};

bool g_rows_applied = false;

// clang-format off
__declspec(naked) void diplo_rows_stub() {
    __asm {
        movzx eax, word ptr ds:[0x00e58354]   // PlayerSide
        cmp   eax, dword ptr [ebp - 0x2c]     // j
        je    self_slot
        mov   eax, 0x004c80e9   // row advance, then the loop tail
        jmp   eax
    self_slot:
        mov   eax, 0x004c80fe   // loop tail, no advance (the self slot has no row)
        jmp   eax
    }
}
// clang-format on

// ---- mp:U52: the relation column is greyed in Team mode ----------------------------------------------
//
// Team mode LOCKS relations (the sim drops every 0xf4 order), so the dialog must not look editable.
// llm_ui_diplomacy_screen_build (0x004c811c) fills one relation widget per non-self slot below the map's
// player count (stride 0x44 at _G_LLM_UI_DIPLO_RELATION_WIDGETS 0x00652b8b); this detour runs the original
// and then sets the DISABLED bit (0x40, the lobby's own convention) on each of them. The lock itself is the
// ally-victory flag (_G_LLM_STRAT_MP_ALLY_VICTORY_RULE_FLAG, region RID_STRAT_MP_ALLY_VICTORY_RULE_FLAG), the same hashed int the sim reads.
constexpr uintptr_t ADDR_DIPLO_BUILD    = 0x004c811cu;
constexpr uintptr_t ADDR_DIPLO_REL_WGTS = 0x00652b8bu;
constexpr uintptr_t ADDR_DIPLO_REL_SPIN = 0x006452f3u; // _G_LLM_UI_DIPLO_RELATION_SPINNERS, stride 0x14
constexpr uintptr_t ADDR_DIPLO_CTL_WGTS = 0x00652dabu; // _G_LLM_UI_DIPLO_CONTROL_WIDGETS (the "vision" column), stride 0x44
constexpr uintptr_t ADDR_DIPLO_CTL_SPIN = 0x00645433u; // _G_LLM_UI_DIPLO_CONTROL_SPINNERS, stride 0x14
constexpr unsigned  DIPLO_SPIN_STRIDE   = 0x14;
constexpr uintptr_t ADDR_PLAYER_SIDE    = 0x00e58354u;
constexpr uintptr_t ADDR_PLAYER_COUNT   = mh::addr::current_map_player_count;
constexpr unsigned  DIPLO_WGT_STRIDE = 0x44, DIPLO_WGT_FLAGS = 0x08;
void               *g_lock_tramp = nullptr;

void on_diplo_built() {
    if (!MH_TeamRel_Enabled()) return; // the knob, valid in configuration (1) too
    if (*mh::state::ptr<const int32_t>(mh::state::RID_STRAT_MP_ALLY_VICTORY_RULE_FLAG) == 0) return;
    const int count = *(const int *)ADDR_PLAYER_COUNT;
    const int side  = (int)*(const uint16_t *)ADDR_PLAYER_SIDE;
    int       row   = 0;
    for (int j = 0; j < count && row < 8; ++j) {
        if (j == side) continue;
        *(uint32_t *)(ADDR_DIPLO_REL_WGTS + (uintptr_t)row * DIPLO_WGT_STRIDE + DIPLO_WGT_FLAGS) |= 0x40;
        // HIDE the checkbox the way retail does for a row the player cannot control: the spinner's sprite offset
        // (+0x10) is 0 -> llm_ui_widget_draw_content draws no sprite at all (user decision, option A).
        *(int32_t *)(ADDR_DIPLO_REL_SPIN + (uintptr_t)row * DIPLO_SPIN_STRIDE + 0x10) = 0;
        // The "vision" column (order 0xf5, shared sight) is team-locked the same way: disabled and hidden.
        *(uint32_t *)(ADDR_DIPLO_CTL_WGTS + (uintptr_t)row * DIPLO_WGT_STRIDE + DIPLO_WGT_FLAGS) |= 0x40;
        *(int32_t *)(ADDR_DIPLO_CTL_SPIN + (uintptr_t)row * DIPLO_SPIN_STRIDE + 0x10) = 0;
        ++row;
    }
}

// clang-format off
__declspec(naked) int diplo_build_detour() {
    __asm {
        call dword ptr [g_lock_tramp]   // the original builder (stolen prologue + the rest); result in EAX
        pushad
        pushfd
        call on_diplo_built
        popfd
        popad
        ret
    }
}
// clang-format on

} // namespace

extern "C" int MH_DiploLock_Install(void) {
    if (!mh::en_build_ok()) return 0;
    if (!mh::hook::install_trampoline(ADDR_DIPLO_BUILD, (void *)diplo_build_detour, &g_lock_tramp, 8,
                                      mh::hook::entry_claim::exclusive, "the mp:U52 diplomacy-dialog relation lock"))
        return 0;
    return 1;
}

extern "C" int MH_DiploEcho_Install(void) {
    if (!mh::en_build_ok()) return 0; // EN-only, like every seam that names an EN VA
    if (g_applied) return 1;
    g_applied = mh::hook::patch_bytes_guarded(mh::addr::diplo_echo_write_site, ECHO_EXPECT, ECHO_NOPS, ECHO_LEN);
    char m[240];
    // clang-format off
    const char *fmt = g_applied
        ? "; U39: diplomacy relation echo NOPed at %08X (22 bytes) -- the 0xf4 order handler is the cell's only writer\n"
        : "; U39: diplomacy relation echo NOT patched at %08X -- bytes differ from the expected span or the parent is promoted (see any [interlock] line); retail echo kept\n";
    // clang-format on
    wsprintfA(m, fmt, (unsigned)mh::addr::diplo_echo_write_site);
    seam_log(m);
    return g_applied ? 1 : 0;
}

// mp:U46 -- see diplo_rows_stub above. Independent of U39's echo NOP (different bytes, own knob).
extern "C" int MH_DiploRows_Install(void) {
    if (!mh::en_build_ok()) return 0;
    if (g_rows_applied) return 1;
    const bool want = mh_ini_get_int("net", "diplo_row_fix", 1, g_ini) != 0;
    if (!want) {
        seam_log("; U46: diplomacy row walk KEPT ([net] diplo_row_fix=0): Apply advances its rows only for human slots -- the reproduction arm\n");
        return 0;
    }
    const intptr_t rel = (intptr_t)(uintptr_t)&diplo_rows_stub - (intptr_t)(mh::addr::diplo_row_skip_site + 5);
    uint8_t        repl[ROW_LEN];
    repl[0]           = 0xE9;
    const int32_t r32 = (int32_t)rel;
    memcpy(repl + 1, &r32, 4);
    g_rows_applied = mh::hook::patch_bytes_guarded(mh::addr::diplo_row_skip_site, ROW_EXPECT, repl, ROW_LEN);
    char m[240];
    // clang-format off
    const char *fmt = g_rows_applied
        ? "; U46: diplomacy row walk SPLICED at %08X -- Apply advances its rows for every non-self slot\n"
        : "; U46: diplomacy row walk NOT patched at %08X -- bytes differ or the parent is promoted (see any [interlock] line); retail walk kept\n";
    // clang-format on
    wsprintfA(m, fmt, (unsigned)mh::addr::diplo_row_skip_site);
    seam_log(m);
    return g_rows_applied ? 1 : 0;
}
