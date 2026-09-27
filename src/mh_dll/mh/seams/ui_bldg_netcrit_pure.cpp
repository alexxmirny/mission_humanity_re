// seams/ui_bldg_netcrit_pure.cpp -- mp:D37: the HUD network panel's is_network_critical probe
// leaves no trace in the sim during a lockstep match. Mechanism and scope: include/mh_netcrit_export.h.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <cstring>

#include "include/mh_netcrit_export.h"
#include "addr/mh_addrs.gen.h"   // mh::addr::netcrit_ui_call_site / _G_LLM_GAME_SESSION_MODE
#include "addr/mh_calls.gen.h"   // mh::call::llm_strat_bldg_is_network_critical
#include "addr/mh_regions.gen.h" // mh::state::live(), RID_BUILDINGS / RID_STRAT_PLAYERS
#include "config/config.h"       // mh::config::mode() -- mode=original keeps retail
#include "hook/patch.h"          // patch_bytes_guarded
#include "hook/promoted.h"       // promoted_owner_of
#include "net_internal.h"        // seam_log; g_ini
#include "en_guard.h"            // EN-only build gate

namespace {

constexpr int      CALL_LEN            = 5;
constexpr uint32_t PLAYERS             = 8;
constexpr uint32_t BLDG_ROW_BYTES      = 0x6aa4; // buildings[8][100], 0x111 per record
constexpr uint32_t STRAT_PLAYER_BYTES  = 0x740;  // llm_strat_player_profile[8]
constexpr uint8_t  SESSION_MP_LOCKSTEP = 3;      // _G_LLM_GAME_SESSION_MODE is a byte

// EN v406: CALL llm_strat_bldg_is_network_critical (0x00497405) from 0x0041896c; EAX = PlayerSide
// (movzx word), EDX = _G_LLM_STRAT_UI_SELECTED_BLDG_INDEX (movzx word); result in EAX.
const uint8_t CALL_EXPECT[CALL_LEN] = {0xE8, 0x99, 0xEA, 0x07, 0x00};

constexpr int MAX_LOGS = 8;

bool    g_armed = false;
int     g_logs  = 0;
uint8_t g_row[BLDG_ROW_BYTES];
uint8_t g_prof[STRAT_PLAYER_BYTES];

bool in_lockstep_match() { return *(const volatile uint8_t *)mh::addr::_G_LLM_GAME_SESSION_MODE == SESSION_MP_LOCKSTEP; }

int32_t probe(uint32_t player, uint32_t bldg) {
    const uint32_t p = player & 0xffffu;
    if (!in_lockstep_match() || p >= PLAYERS)
        return mh::call::llm_strat_bldg_is_network_critical((int32_t)player, (int32_t)bldg);
    const auto &lv   = mh::state::live();
    uint8_t    *row  = reinterpret_cast<uint8_t *>(lv.base[mh::state::RID_BUILDINGS] + p * BLDG_ROW_BYTES);
    uint8_t    *prof = reinterpret_cast<uint8_t *>(lv.base[mh::state::RID_STRAT_PLAYERS] + p * STRAT_PLAYER_BYTES);
    std::memcpy(g_row, row, BLDG_ROW_BYTES);
    std::memcpy(g_prof, prof, STRAT_PLAYER_BYTES);
    const int32_t verdict = mh::call::llm_strat_bldg_is_network_critical((int32_t)player, (int32_t)bldg);
    // Evidence, bounded: the first probe, then (up to MAX_LOGS) every probe that actually moved bytes.
    uint32_t moved = 0;
    for (uint32_t i = 0; i < BLDG_ROW_BYTES; ++i) moved += row[i] != g_row[i];
    for (uint32_t i = 0; i < STRAT_PLAYER_BYTES; ++i) moved += prof[i] != g_prof[i];
    std::memcpy(row, g_row, BLDG_ROW_BYTES);
    std::memcpy(prof, g_prof, STRAT_PLAYER_BYTES);
    if (g_logs < MAX_LOGS && (g_logs == 0 || moved != 0)) {
        ++g_logs;
        char m[160];
        wsprintfA(m, "; D37: network-panel probe player %u building %u -> verdict %d, restored %u sim byte(s)\n",
                  (unsigned)p, (unsigned)(bldg & 0xffffu), (int)verdict, (unsigned)moved);
        seam_log(m);
    }
    return verdict;
}

__declspec(naked) void probe_thunk() {
    __asm {
        pushad
        push edx // selected building index
        push eax // player
        call probe // __cdecl(uint32_t, uint32_t) -> int32_t
        add  esp, 8
        mov  [esp + 28], eax // pushad's EAX slot
        popad
        ret // back to 0x0041896c: TEST EAX,EAX
    }
}

} // namespace

extern "C" int MH_NetCrit_Install(void) {
    if (!mh::en_build_ok()) return 0;
    if (g_armed) return 1;
    const uintptr_t site = mh::addr::netcrit_ui_call_site;
    if (GetPrivateProfileIntA("net", "netcrit_ui_pure", 1, g_ini) == 0) {
        seam_log("; D37: network-panel probe KEPT ([net] netcrit_ui_pure=0): selecting a network building "
                 "recomputes this peer's power network -- the reproduction arm\n");
        return 0;
    }
    if (mh::config::mode() == mh::config::mode_t::original) {
        seam_log("; D37: network-panel probe KEPT ([config] mode=original)\n");
        return 0;
    }
    char m[200];
    if (mh::hook::promoted_owner_of(site)) {
        wsprintfA(m, "; D37: network-panel probe DISPLACED at %08X: the HUD panel draw is promoted this run\n",
                  (unsigned)site);
        seam_log(m);
        return 0;
    }
    uint8_t repl[CALL_LEN];
    repl[0]     = 0xE8;
    int32_t rel = (int32_t)((uintptr_t)&probe_thunk - (site + CALL_LEN));
    std::memcpy(repl + 1, &rel, sizeof(rel));
    g_armed = std::memcmp(reinterpret_cast<const void *>(site), CALL_EXPECT, CALL_LEN) == 0 &&
              mh::hook::patch_bytes_guarded(site, CALL_EXPECT, repl, CALL_LEN);
    wsprintfA(m,
              g_armed ? "; D37: network-panel probe sim-pure at %08X -- in a lockstep match the caller's buildings row + "
                        "strat_players record are restored around is_network_critical\n"
                      : "; D37: network-panel probe NOT patched at %08X -- bytes differ or the write was refused; retail kept\n",
              (unsigned)site);
    seam_log(m);
    return g_armed ? 1 : 0;
}
