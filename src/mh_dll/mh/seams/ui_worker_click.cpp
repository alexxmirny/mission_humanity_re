// seams/ui_worker_click.cpp -- mods:RES8: the building panel's worker +/- buttons order one worker on
// every press, including the first one of the process. Mechanism, the patch sites, the three
// [ui] worker_click_fix modes and the contract are in include/mh_workerclick_export.h.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "mh_ini_gate.h" // RL2: the ship gate every ini read goes through
#include <cstdint>
#include <cstring>

#include "include/mh_workerclick_export.h"
#include "addr/mh_addrs.gen.h" // mh::addr::workeradj_press_snd_call_site
#include "hook/patch.h"        // patch_bytes_guarded
#include "hook/promoted.h"     // promoted_owner_of
#include "net_internal.h"      // seam_log; g_ini
#include "en_guard.h"          // EN-only build gate

namespace {

// _G_LLM_UI_BLDG_WORKER_ADJ_BTNS (0x0050bddb), two 20-byte records; pending_worker_amount is +0x0c.
constexpr uintptr_t WORKER_ADJ_BTNS = 0x0050bddbu;
constexpr uintptr_t AMOUNT_CELL[2]  = {WORKER_ADJ_BTNS + 0x0c, WORKER_ADJ_BTNS + 20 + 0x0c}; // 0x0050bde7, 0x0050bdfb

const uint8_t AMOUNT_STOCK[4]    = {0x00, 0x00, 0x00, 0x00};
const uint8_t AMOUNT_PRIMED[4]   = {0x01, 0x00, 0x00, 0x00};
const uint8_t SND_CALL_EXPECT[5] = {0xE8, 0x13, 0x1E, 0x01, 0x00}; // CALL llm_snd_play (rel32 from 0x00413420)

// Mode 2: llm_ui_bldg_worker_adjust_apply's two order CALLs -- [0] assign (+), [1] unassign (-).
const uintptr_t DRAIN_SITE[2]           = {mh::addr::workeradj_apply_assign_call_site,
                                           mh::addr::workeradj_apply_unassign_call_site};
const uint8_t   DRAIN_CALL_EXPECT[2][5] = {
    {0xE8, 0xA8, 0xAA, 0x05, 0x00}, // CALL llm_strat_bldg_order_assign_workers (0x0046dfb6)
    {0xE8, 0x2F, 0xAB, 0x05, 0x00}, // CALL llm_strat_bldg_order_unassign_workers (0x0046e054)
};

uintptr_t g_snd_play = 0; // the thunk's tail-jump target, llm_snd_play: the spliced CALL's own destination
uintptr_t g_assign   = 0; // the drain thunks' tail-jump targets: the spliced CALLs' own destinations
uintptr_t g_unassign = 0;
bool      g_primed   = false;
bool      g_spliced  = false;
int       g_drained  = 0; // drain sites armed (0..2)

// clang-format off
// At the splice the press-edge branch has just stored pending_row; EAX = 0x44 and EDX = 0x64 are
// llm_snd_play's arguments and must reach it unchanged. The tick's frame keeps the record pointer
// (its EBX argument) at [EBP-0x18]. ECX is saved around the store, so every register the original
// CALL saw is what llm_snd_play sees; the return address on the stack is the splice's, so its RET
// lands back at 0x00413420 exactly as before.
__declspec(naked) void press_thunk() {
    __asm {
        push ecx
        mov  ecx, dword ptr [ebp - 0x18]   // the llm_ui_bldg_worker_adjust_btn record
        mov  dword ptr [ecx + 0x0c], 1     // pending_worker_amount = 1
        pop  ecx
        jmp  dword ptr [g_snd_play]
    }
}

// Mode 2. At both drain sites llm_ui_bldg_worker_adjust_apply has loaded EAX = player, EDX = building
// and EBX = the record's pending_worker_amount; its frame keeps the record (its EAX argument) at
// [EBP-0x18]. The thunk zeroes the record's amount -- EBX already carries the count this order
// uses -- and tail-jumps to the order with every register as the original CALL left it. The next
// frames of the same press then order 0 (stock already issues 0-count orders during a fresh press);
// past 200 ms the hold branch of the tick rewrites the amount every frame, so the auto-repeat is stock.
__declspec(naked) void drain_assign_thunk() {
    __asm {
        push ecx
        mov  ecx, dword ptr [ebp - 0x18]   // the llm_ui_bldg_worker_adjust_btn record
        mov  dword ptr [ecx + 0x0c], 0     // pending_worker_amount = 0: this press's worker is ordered
        pop  ecx
        jmp  dword ptr [g_assign]
    }
}
__declspec(naked) void drain_unassign_thunk() {
    __asm {
        push ecx
        mov  ecx, dword ptr [ebp - 0x18]
        mov  dword ptr [ecx + 0x0c], 0
        pop  ecx
        jmp  dword ptr [g_unassign]
    }
}
// clang-format on

// The CALL rel32 at `site` that reaches `thunk` instead; returns the original CALL's destination.
uintptr_t make_call(uintptr_t site, const uint8_t expect[5], void (*thunk)(), uint8_t repl[5]) {
    int32_t orig;
    memcpy(&orig, expect + 1, sizeof(orig));
    repl[0]     = 0xE8;
    int32_t rel = (int32_t)((uintptr_t)thunk - (site + 5));
    memcpy(repl + 1, &rel, sizeof(rel));
    return site + 5 + orig;
}

} // namespace

extern "C" int MH_WorkerClick_Install(void) {
    if (!mh::en_build_ok()) return 0; // EN-only, like every seam that names an EN VA
    if (g_primed || g_spliced) return (g_primed ? 1 : 0) + (g_spliced ? 1 : 0) + g_drained;
    // 0 = stock; 1 = the first press primed (parts 1+2); 2 = one worker per press (parts 1+2+3).
    const int mode = mh_ini_get_int("ui", "worker_click_fix", 1, g_ini);
    if (mode == 0) {
        seam_log("; RES8: worker +/- stock -- mode 0 ([ui] worker_click_fix=0): the first press of each button "
                 "orders 0 workers and a held press orders the stale amount every frame -- the reproduction arm\n");
        return 0;
    }
    char m[320];
    int  primed = 0;
    for (int i = 0; i < 2; ++i)
        if (mh::hook::patch_bytes_guarded(AMOUNT_CELL[i], AMOUNT_STOCK, AMOUNT_PRIMED, 4)) ++primed;
    g_primed = primed == 2;

    const uintptr_t site   = mh::addr::workeradj_press_snd_call_site;
    const char     *refuse = "NOT armed (bytes differ from the expected CALL llm_snd_play)";
    if (mh::hook::promoted_owner_of(site)) {
        refuse = "DISPLACED (llm_ui_bldg_worker_adjust_btn_tick is promoted this run; see the [interlock] line)";
    } else {
        uint8_t repl[5];
        g_snd_play = make_call(site, SND_CALL_EXPECT, press_thunk, repl); // 0x00425233
        g_spliced  = mh::hook::patch_bytes_guarded(site, SND_CALL_EXPECT, repl, 5);
    }

    const char *drain = "off (mode 1)";
    if (mode >= 2) {
        if (mh::hook::promoted_owner_of(DRAIN_SITE[0])) {
            drain = "DISPLACED (llm_ui_bldg_worker_adjust_apply is promoted this run; see the [interlock] line)";
        } else {
            void (*const thunk[2])()   = {drain_assign_thunk, drain_unassign_thunk};
            uintptr_t *const target[2] = {&g_assign, &g_unassign};
            for (int i = 0; i < 2; ++i) {
                uint8_t repl[5];
                *target[i] = make_call(DRAIN_SITE[i], DRAIN_CALL_EXPECT[i], thunk[i], repl);
                if (mh::hook::patch_bytes_guarded(DRAIN_SITE[i], DRAIN_CALL_EXPECT[i], repl, 5)) ++g_drained;
            }
            drain = g_drained == 2 ? "armed" : "NOT armed (bytes differ from the expected order CALLs)";
        }
    }
    wsprintfA(m,
              "; RES8: worker +/- %s: amounts primed %d/2 at %08X/%08X, press splice %s at %08X, "
              "apply drain %s at %08X/%08X -- mode %d ([ui] worker_click_fix)\n",
              mode >= 2 ? "one worker per press" : "first press primed", primed, (unsigned)AMOUNT_CELL[0],
              (unsigned)AMOUNT_CELL[1], g_spliced ? "armed" : refuse, (unsigned)site, drain,
              (unsigned)DRAIN_SITE[0], (unsigned)DRAIN_SITE[1], mode >= 2 ? 2 : 1);
    seam_log(m);
    return (g_primed ? 1 : 0) + (g_spliced ? 1 : 0) + g_drained;
}
