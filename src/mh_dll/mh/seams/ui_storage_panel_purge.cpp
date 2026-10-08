// seams/ui_storage_panel_purge.cpp -- mp:D38 row 5: the storage panel's dead-docked purge is left to
// the sim during a lockstep match. Mechanism and scope: include/mh_storagepurge_export.h.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "mh_ini_gate.h" // RL2: the ship gate every ini read goes through
#include <cstdint>
#include <cstring>

#include "include/mh_storagepurge_export.h"
#include "addr/mh_addrs.gen.h"   // mh::addr::storage_panel_purge_call_site / _G_LLM_GAME_SESSION_MODE
#include "addr/mh_calls.gen.h"   // mh::call::llm_strat_storage_purge_dead_docked
#include "addr/mh_regions.gen.h" // mh::state::live(), RID_UNIT_STORAGE / RID_UNITS
#include "addr/mh_structs.gen.h" // mh::game::mh_map_object_unit_storage / mh_map_object_unit
#include "config/config.h"       // mh::config::mode() -- mode=original keeps retail
#include "hook/patch.h"          // patch_bytes_guarded
#include "hook/promoted.h"       // promoted_owner_of
#include "state/roster_caps.h"   // live_roster_caps(): the per-player unit/storage caps the host BOUND
#include "net_internal.h"        // seam_log; g_ini
#include "en_guard.h"            // EN-only build gate

namespace {

constexpr int      CALL_LEN            = 5;
constexpr uint32_t PLAYERS             = 8;
constexpr uint8_t  SESSION_MP_LOCKSTEP = 3; // _G_LLM_GAME_SESSION_MODE is a byte

// EN v406: CALL llm_strat_storage_purge_dead_docked (0x0049b8f5) from 0x0041748a; EAX = PlayerSide
// (movzx word), EDX = buildings[PlayerSide][selected].sub_id (movzx byte); void. The caller reloads
// every register it uses after the call (MOVZX EAX,[PlayerSide] at 0x0041748f).
const uint8_t CALL_EXPECT[CALL_LEN] = {0xE8, 0x66, 0x44, 0x08, 0x00};

constexpr int MAX_LOGS = 8;

bool g_armed = false;
int  g_logs  = 0;

bool in_lockstep_match() { return *(const volatile uint8_t *)mh::addr::_G_LLM_GAME_SESSION_MODE == SESSION_MP_LOCKSTEP; }

// The dead rows the skipped purge would have removed -- evidence only, read-only.
uint32_t count_dead_docked(uint32_t p, uint32_t slot) {
    const auto    caps     = mh::state::live_roster_caps(); // derived, not the stock 25 / 100
    const int32_t per_st   = caps.storage;
    const int32_t per_unit = caps.units;
    if (p >= PLAYERS || (int32_t)slot >= per_st) return 0;
    const auto &lv = mh::state::live();
    const auto *st = reinterpret_cast<const mh::game::mh_map_object_unit_storage *>(lv.base[mh::state::RID_UNIT_STORAGE]) +
                     p * per_st + slot;
    const auto *un = reinterpret_cast<const mh::game::mh_map_object_unit *>(lv.base[mh::state::RID_UNITS]) +
                     p * per_unit;
    uint32_t      dead = 0;
    const int32_t n    = st->docked_count < 0 ? 0 : (st->docked_count > 50 ? 50 : st->docked_count);
    for (int32_t i = 0; i < n; ++i) {
        const int32_t u = st->docked_units[i];
        if (u >= 0 && u < per_unit && un[u].energy <= 0.0) ++dead;
    }
    return dead;
}

void purge_gate(uint32_t player, uint32_t slot) {
    if (!in_lockstep_match()) {
        mh::call::llm_strat_storage_purge_dead_docked((int32_t)player, (int32_t)slot);
        return;
    }
    // Evidence, bounded: the first skip, then (up to MAX_LOGS) every skip that left a dead row for the sim.
    const uint32_t p    = player & 0xffffu;
    const uint32_t s    = slot & 0xffu;
    const uint32_t dead = count_dead_docked(p, s);
    if (g_logs < MAX_LOGS && (g_logs == 0 || dead != 0)) {
        ++g_logs;
        char m[176];
        wsprintfA(m, "; D38: storage-panel purge SKIPPED in lockstep: player %u storage %u, %u dead docked unit(s) "
                     "left for the sim's purge\n",
                  (unsigned)p, (unsigned)s, (unsigned)dead);
        seam_log(m);
    }
}

__declspec(naked) void purge_thunk() {
    __asm {
        pushad
        push edx // storage slot (sub_id)
        push eax // player
        call purge_gate // __cdecl(uint32_t, uint32_t)
        add  esp, 8
        popad
        ret // back to 0x0041748f
    }
}

} // namespace

extern "C" int MH_StoragePurge_Install(void) {
    if (!mh::en_build_ok()) return 0;
    if (g_armed) return 1;
    const uintptr_t site = mh::addr::storage_panel_purge_call_site;
    if (mh_ini_get_int("net", "storage_panel_purge_fix", 1, g_ini) == 0) {
        seam_log("; D38: storage-panel purge KEPT ([net] storage_panel_purge_fix=0): the selecting peer purges "
                 "dead docked units ahead of the sim -- the reproduction arm\n");
        return 0;
    }
    if (mh::config::mode() == mh::config::mode_t::original) {
        seam_log("; D38: storage-panel purge KEPT ([config] mode=original)\n");
        return 0;
    }
    char m[200];
    if (mh::hook::promoted_owner_of(site)) {
        wsprintfA(m, "; D38: storage-panel purge DISPLACED at %08X: the storage panel is promoted this run\n",
                  (unsigned)site);
        seam_log(m);
        return 0;
    }
    uint8_t repl[CALL_LEN];
    repl[0]     = 0xE8;
    int32_t rel = (int32_t)((uintptr_t)&purge_thunk - (site + CALL_LEN));
    std::memcpy(repl + 1, &rel, sizeof(rel));
    g_armed = std::memcmp(reinterpret_cast<const void *>(site), CALL_EXPECT, CALL_LEN) == 0 &&
              mh::hook::patch_bytes_guarded(site, CALL_EXPECT, repl, CALL_LEN);
    wsprintfA(m,
              g_armed ? "; D38: storage-panel purge sim-owned at %08X -- in a lockstep match the panel leaves dead "
                        "docked units to the sim's sub-tick purge\n"
                      : "; D38: storage-panel purge NOT patched at %08X -- bytes differ or the write was refused; retail kept\n",
              (unsigned)site);
    seam_log(m);
    return g_armed ? 1 : 0;
}
