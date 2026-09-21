//
// seams/ui_cheat_gate.cpp -- mp:CH1: the single-player cheat console, refused in a network game.
//
// The path, what each table slot does under lockstep, the rule and the hook are in
// include/mh_cheatgate_export.h. Read that first; this file is the implementation and its comments
// assume it.
//
#include <windows.h>
#include <cstdint>
#include <cstring>

#include "include/mh_cheatgate_export.h"
#include "addr/mh_addrs.gen.h" // mh::addr::_G_LLM_CHEAT_CMD_TABLE / _G_LLM_GAME_SESSION_MODE / G_TEXT_TMP
#include "addr/mh_calls.gen.h" // mh::call::llm_str_to_upper_inplace / llm_ui_print_floating_msg_red
#include "hook/detour.h"       // install_trampoline
#include "net_internal.h"      // seam_log -- the `; ` line into mh_net.log; g_ini
#include "en_guard.h"          // EN-only build gate

#pragma comment(lib, "user32.lib") // wsprintfA / wsprintfW

namespace {

using mh::hook::install_trampoline;

// ---- the entry ---------------------------------------------------------------------------------
// llm_chat_history_push opens with the 8-byte Watcom frame (`55 89 E5 68 24 00 00 00`, read back
// from the EN image 2026-09-21); the stack-probe CALL sits at +8, outside the stolen range, so the
// trampoline re-enters it in place. WHY THIS ENTRY AND NOT THE DISPATCHER'S: llm_debug_console_
// dispatch (0x0044b67f) is a NAMED HOOK POINT the determinism harness observes (hook/hookpoint.h
// `point::console`, armed in MH_Harness_Init before any seam), and ONE ENTRY HAS ONE OWNER -- the
// first cut of this gate installed there and was refused in every harness-armed lane (measured
// 2026-09-21: "the CH1 network-game cheat gate wanted this entry EXCLUSIVELY and the harness console
// detour already owns it"). history_push is called by every submit BEFORE the mode split and
// before the `str_cmp(line, "")` that guards the send/dispatch, so a line blanked here is a line
// nothing downstream acts on -- and nothing else owns the entry.
constexpr uintptr_t ADDR_HISTORY = mh::addr::llm_chat_history_push; // 0x00415475, __watcall(void)
constexpr int       STOLEN       = 8;

// The console's table: 48 `char*` slots (typed WCHAR*[48] in the DB, but the dispatcher casts each
// to char* and str_cmp's it -- the strings are ANSI, XOR-0x7a obfuscated). Slot 0 and 0x2e..0x2f
// hold the joke default; the dispatcher scans 1..0x2f.
constexpr uintptr_t ADDR_TABLE  = mh::addr::_G_LLM_CHEAT_CMD_TABLE; // 0x00d033c0
constexpr int       TABLE_FIRST = 1;
constexpr int       TABLE_LAST  = 0x2f;
constexpr uint8_t   OBF_XOR     = 0x7a;
constexpr int       SLOT_TO_IDX = 2; // catalog idx = table slot - 2 (slot 0x27 = idx 0x25 = `_NUCLEAR BOMB`)

// The one slot a lockstep match keeps: `_NETDELAY` (catalog 0xb) prints this peer's step size and
// ping and writes nothing. Its siblings `_NETDELAY UP` / `DOWN` / `SYNC` (0xc..0xe) are local
// lockstep writes and are refused with the rest (mp:CH1 decision, 2026-09-21: a step-size or resync
// change typed on one peer is a one-sided lockstep write; if they are ever wanted in a match they
// ride an order, not the console).
constexpr int SLOT_NETDELAY_PRINT = 0xd;

// The text line the console and the chat both submit: char[81] = a 41-byte edit line (NUL inside)
// followed by the scancode queue. Every consumer reads it as a C string; so do we.
constexpr uintptr_t ADDR_CHAT_LINE = mh::addr::_G_LLM_STRAT_CHAT_INPUT_LINE; // 0x0050a84c
// _G_LLM_CHAT_MODE is spelled as a raw EN VA with its provenance rather than pulled from the address
// manifest, on the gfx_font_guard.cpp / ui_chat_input.cpp precedent -- and here for a measured
// reason: a manifest DATA entry becomes a state-registry region, and inserting one renumbers every
// later RID and the hash-manifest fingerprint (563-line mh_regions.gen.h churn, every libref
// fixture and both --ui-abc oracles STALE -- the SES3b trap, measured 2026-09-21 and reverted).
// docs/symbols.md: 0x005d01d8 `_G_LLM_CHAT_MODE` undefined4, written by llm_strat_input_update's
// Enter arm (1 team / 2 all / 3 console) and llm_net_chat_input_process. Not in a movable region.
constexpr uintptr_t ADDR_CHAT_MODE    = 0x005d01d8u;
constexpr int       LINE_MAX          = 40; // the edit line's own capacity; the copy is bounded by it
constexpr int       CHAT_MODE_CONSOLE = 3;

constexpr uint8_t SESSION_MP_LOCKSTEP = 3; // _G_LLM_GAME_SESSION_MODE is a byte

// ---- state -------------------------------------------------------------------------------------
bool  g_armed   = false;
bool  g_gate_on = true;    // [input] cheat_gate (default 1); 0 = retail behaviour, the reproduction arm
void *g_tramp   = nullptr; // stolen prologue + jmp llm_chat_history_push+8
long  g_refused = 0;
long  g_passed  = 0;

// ---- the matcher -------------------------------------------------------------------------------
// The dispatcher's own match, on a private copy: uppercase through the game's routine (identical
// case folding), XOR every byte, str_cmp against slots 1..0x2f in order, first hit wins. Returns the
// table SLOT or -1. The copy is what keeps this a pure read: the dispatcher uppercases the line IN
// PLACE, and a pre-check that did the same would change what a chat line looks like on the wire.
int match_slot(const char *line) {
    if (!line) return -1;
    char copy[LINE_MAX + 1];
    int  n = 0;
    while (n < LINE_MAX && line[n]) {
        copy[n] = line[n];
        ++n;
    }
    copy[n] = 0;
    if (n == 0) return -1;
    mh::call::llm_str_to_upper_inplace(copy);
    for (int i = 0; i < n; ++i) copy[i] = (char)((uint8_t)copy[i] ^ OBF_XOR);

    const char *const *table = (const char *const *)ADDR_TABLE;
    for (int slot = TABLE_FIRST; slot <= TABLE_LAST; ++slot) {
        const char *t = table[slot];
        if (!t) continue;
        if (strcmp(copy, t) == 0) return slot;
    }
    return -1;
}

// The deobfuscated command text of a slot, for the log line. ASCII by construction of the table
// (every string is plain uppercase text once XORed); anything else is printed as '?'.
void slot_text(int slot, char *out, int cap) {
    const char *const *table = (const char *const *)ADDR_TABLE;
    const char        *t     = (slot >= 0 && slot < 0x30) ? table[slot] : nullptr;
    int                i     = 0;
    for (; t && t[i] && i < cap - 1; ++i) {
        const char c = (char)((uint8_t)t[i] ^ OBF_XOR);
        out[i]       = (c >= 0x20 && c < 0x7f) ? c : '?';
    }
    out[i] = 0;
}

bool in_lockstep_match() { return *(const volatile uint8_t *)mh::addr::_G_LLM_GAME_SESSION_MODE == SESSION_MP_LOCKSTEP; }

// ---- the submit hook (run-AFTER llm_chat_history_push) -----------------------------------------
// Every Enter in the text line calls history_push before the mode split (llm_strat_input_update
// 0x00441c27; llm_net_chat_input_process and the scancode drain's own 0x1c arm are submits too).
// Runs after the original so the line is in the player's history exactly as retail keeps it (a
// refused line stays recallable, like any other); the blanking below is what the CONSUMERS see.
//
// Two things happen here, in this order:
//   1. the submit log -- redacted: length + first character + the catalog index, never the text;
//   2. the gate -- in a lockstep match, a console-mode line (`_G_LLM_CHAT_MODE == 3`) matching any
//      slot but `_NETDELAY` is blanked (`line[0] = 0`): the caller's `str_cmp(line, "")` then skips
//      its whole send/dispatch block and closes the line, so neither the dispatcher nor a chat send
//      ever sees it. Logged, and the player is told on the HUD through the desync notice's path.
void __cdecl on_chat_submit() {
    char     *line = (char *)ADDR_CHAT_LINE;
    const int mode = *(const volatile int32_t *)ADDR_CHAT_MODE;
    int       n    = 0;
    while (n < LINE_MAX && line[n]) ++n;
    const int  slot  = match_slot(line);
    const char first = (n > 0 && line[0] >= 0x20 && line[0] < 0x7f) ? line[0] : (n > 0 ? '?' : ' ');
    char       idx[8];
    if (slot >= 0) wsprintfA(idx, "0x%02x", slot - SLOT_TO_IDX);
    else lstrcpyA(idx, "-");
    char m[192];
    wsprintfA(m, "; [chat] submit mode=%d sess=%d len=%d first='%c' cheat_idx=%s\n", mode,
              (int)*(const volatile uint8_t *)mh::addr::_G_LLM_GAME_SESSION_MODE, n, first, idx);
    seam_log(m);

    if (mode != CHAT_MODE_CONSOLE || slot < 0 || !in_lockstep_match()) return; // chat, or single player
    if (slot == SLOT_NETDELAY_PRINT) {
        ++g_passed;
        return;
    }
    char cmd[48];
    slot_text(slot, cmd, sizeof(cmd));
    if (!g_gate_on) {
        // The reproduction arm: say what retail is about to do, then let it.
        wsprintfA(m, "; CH1: cheat '%s' (idx 0x%02x) RUNS in a network game ([input] cheat_gate=0)\n", cmd,
                  slot - SLOT_TO_IDX);
        seam_log(m);
        ++g_passed;
        return;
    }
    ++g_refused;
    line[0] = 0; // the refusal: an empty line is one every consumer already ignores
    wsprintfA(m, "; CH1: cheat '%s' (idx 0x%02x) refused in a network game\n", cmd, slot - SLOT_TO_IDX);
    seam_log(m);
    // The player's half: the desync notice's path (G_TEXT_TMP -> the red floating queue). Not a
    // cfg::G_TEXT_PTRS string -- retail has no text for a condition it never detected.
    wsprintfW((wchar_t *)mh::addr::G_TEXT_TMP, L"cheats are single player");
    mh::call::llm_ui_print_floating_msg_red((void *)mh::addr::G_TEXT_TMP);
}

// clang-format off
__declspec(naked) void history_detour() {
    __asm {
        pushad
        pushfd
        call dword ptr [g_tramp]         // the ORIGINAL body, whole: stolen prologue + jmp +8 ... leave/ret
        call on_chat_submit
        popfd
        popad
        ret                              // __watcall(void), plain RET, callee-saved regs intact via popad
    }
}
// clang-format on

} // namespace

extern "C" int MH_CheatGate_Install(void) {
    if (!mh::en_build_ok()) return 0; // EN-only, like every seam that names an EN VA
    if (g_armed) return 1;
    g_gate_on = GetPrivateProfileIntA("input", "cheat_gate", 1, g_ini) != 0;

    if (install_trampoline(ADDR_HISTORY, (void *)history_detour, &g_tramp, STOLEN, mh::hook::entry_claim::exclusive,
                           "the CH1 network-game cheat gate"))
        g_armed = true;
    else
        seam_log("; CH1: gate NOT armed: install refused at llm_chat_history_push (see the [interlock] line) -- "
                 "the console keeps retail behaviour in a network game\n");

    char m[200];
    wsprintfA(m, "; CH1: cheat console gate %s -- in a network game only _NETDELAY (idx 0x0b) passes%s\n",
              g_armed ? "armed" : "INERT", g_gate_on ? "" : " -- GATE OFF ([input] cheat_gate=0): retail behaviour");
    seam_log(m);
    return g_armed ? 1 : 0;
}

extern "C" int MH_CheatGate_MatchIndex(const char *line) {
    const int slot = match_slot(line);
    return slot < 0 ? -1 : slot - SLOT_TO_IDX;
}

extern "C" void MH_CheatGate_Stats(long *refused, long *passed) {
    if (refused) *refused = g_refused;
    if (passed) *passed = g_passed;
}
