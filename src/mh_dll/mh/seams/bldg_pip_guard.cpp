//
// seams/bldg_pip_guard.cpp -- hotfix rc9.4 `[net] bldg_pip_guard`: the RETAIL CARRIER (configuration (1), no
// libmh.dll; libmh's twin is sim_building_tick.cpp bldg_pip_sanitize).
//
// THE CRASHES (field reports 01M4YBY32EB7JKR2A6J0W64SVR and 01M4RBSXWN1WKC39SPQFF47XD3, triage
// tmp/report_triage_2026-10-15/crash_7908b.md). llm_strat_bldg_update_charge_pips (0x0047908b) and
// llm_strat_bldg_tick_pip_anim (0x004793e1) read the fire-animation table A_OGIEN as `[pip_level*4 + 0x00c3873c]`
// with no range check. A building whose pip_level[slot] held ~0x0384DE82 / ~0x198463AE faulted. Who writes the
// garbage is not identified (suspected a planet/save restore) -- so the guard sits in front of every reader.
//
// LEGAL STATE. pip_level[0..3] each 0..4 (0 = dark, 1..4 = stacked fire level) and pip_active_count 0..16
// (pip_slot_count <= 4 slots x 4 levels; update_charge_pips' target never exceeds it). Anything else is zeroed
// exactly as map_CreateBuilding does (0x004625cd-0x00462779): pip_frame 0, pip_level 0, pip_timer = GAME_CLOCK,
// pip_active_count 0. update_charge_pips then re-lights the pips from the building's real energy. The values are
// sim state, so the zeroing is deterministic (the same record is equally bad on every peer).
//
// THE TWO SITES.
//   (A) llm_strat_building_tick 0x0046fe67 `MOV EAX,[_G_LLM_STRAT_CUR_BUILDING]` (5 bytes, A1 DC 62 E1 00): the
//       first instruction of the pip-anim gate, reached only by fallthrough from `CALL tick_animation_state`
//       at 0x0046fe62 (no reference targets it). -> CALL tick_stub, which checks the current building and
//       returns its pointer in EAX (the displaced load) (flags are dead: a CMP follows). Ahead of BOTH tick_pip_anim (0x0046fe83) and
//       apply_damage (0x0046fef2) -> update_charge_pips, the two field chains.
//   (B) llm_strat_bldg_update_charge_pips 0x00478ecb `MOV [EBP-0x14],EAX / MOV [EBP-0x1c],EDX` (6 bytes,
//       89 45 EC 89 55 E4), right after the prologue, EAX = player, EDX = building index. -> CALL charge_stub +
//       NOP; the stub checks buildings[player][index] and redoes both stores. Needed because two callers reach
//       update_charge_pips for a building NOT yet ticked this step: llm_unit_bldg_energy_refill_full (from
//       llm_strat_order_queue_dispatch, which llm_strat_sim_step runs before any tick) and
//       llm_strat_unit_state_deploy_to_building (unit ticks run before the same player's building ticks). The
//       other two callers (apply_damage, state_charge_step) run inside building_tick after site (A).
//
// THE LOG. One `; [pip] GUARD` line per firing (a record is clean afterwards, so one per corruption event):
// site, player, building index, type (building_id), state, built_flags, the four raw levels, the raw count,
// planet (G_PLANET_INDEX), the replay record's match step and GAME_CLOCK. Capped at 64 lines per process.
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <cstring>

#include "seams/bldg_pip_guard.h"
#include "addr/mh_addrs.gen.h"    // the CUR_* / GAME_CLOCK / G_PLANET_INDEX globals
#include "addr/mh_regions.gen.h"  // mh::state::live(), RID_BUILDINGS
#include "addr/mh_structs.gen.h"  // mh_map_object_building
#include "state/region_runtime.h" // mh::state::ptr<> -- CUR_BUILDING / CUR_INDEX are RELOCATABLE regions
#include "state/roster_caps.h"    // live_roster_caps(): the per-player building cap the host bound
#include "desync/replay_record.h" // replay_rec::current_step() -- the match-step axis the door watch uses
#include "hook/patch.h"           // patch_bytes_guarded
#include "hook/promoted.h"        // promoted_owner_of
#include "net_internal.h"         // seam_log

namespace {

using building = mh::game::mh_map_object_building;

constexpr uintptr_t SITE_TICK        = 0x0046fe67u; // llm_strat_building_tick: MOV EAX,[_G_LLM_STRAT_CUR_BUILDING]
constexpr uintptr_t SITE_CHARGE      = 0x00478ecbu; // llm_strat_bldg_update_charge_pips: MOV [EBP-0x14],EAX / MOV [EBP-0x1c],EDX
const uint8_t       TICK_EXPECT[5]   = {0xA1, 0xDC, 0x62, 0xE1, 0x00};
const uint8_t       CHARGE_EXPECT[6] = {0x89, 0x45, 0xEC, 0x89, 0x55, 0xE4};
constexpr int32_t   LEVEL_MAX        = 4;
constexpr int32_t   ACTIVE_MAX       = 16;
constexpr int       MAX_LINES        = 64;

int g_lines = 0;

template <class T>
T rd(uintptr_t a) { return *reinterpret_cast<const T *>(a); }

bool is_bad(const building &b) {
    if (b.pip_active_count < 0 || b.pip_active_count > ACTIVE_MAX) return true;
    for (int i = 0; i < 4; ++i)
        if (b.pip_level[i] < 0 || b.pip_level[i] > LEVEL_MAX) return true;
    return false;
}

void sanitize(building &b, uint32_t player, uint32_t index, const char *site) {
    if (!is_bad(b)) return;
    const int32_t lv[4] = {b.pip_level[0], b.pip_level[1], b.pip_level[2], b.pip_level[3]};
    const int32_t cnt   = b.pip_active_count;
    const double  now   = rd<double>(mh::addr::_G_LLM_STRAT_GAME_CLOCK);
    for (int i = 0; i < 4; ++i) { // map_CreateBuilding's pip init
        b.pip_frame[i] = 0;
        b.pip_level[i] = 0;
        b.pip_timer[i] = now;
    }
    b.pip_active_count = 0;
    if (g_lines >= MAX_LINES) return;
    ++g_lines;
    char m[400];
    wsprintfA(m,
              "; [pip] GUARD (%s): player %lu building %lu type %u state %u built_flags 0x%02X: pip_level "
              "{%ld,%ld,%ld,%ld} (hex %08lX %08lX %08lX %08lX) active %ld out of range -> pip state zeroed; planet %ld "
              "match step %lu clock %ld ms\n",
              site, (unsigned long)player, (unsigned long)index, (unsigned)b.building_id, (unsigned)b.state,
              (unsigned)b.built_flags, (long)lv[0], (long)lv[1], (long)lv[2], (long)lv[3], (unsigned long)lv[0],
              (unsigned long)lv[1], (unsigned long)lv[2], (unsigned long)lv[3], (long)cnt,
              (long)rd<int32_t>(mh::addr::G_PLANET_INDEX), (unsigned long)mh::desync::replay_rec::current_step(),
              (long)(now * 1000.0));
    seam_log(m);
}

// Site (A): the building being ticked is _G_LLM_STRAT_CUR_BUILDING. Returns that pointer: the stub hands it back
// in EAX, which is what the displaced `MOV EAX,[_G_LLM_STRAT_CUR_BUILDING]` produced.
uintptr_t __cdecl guard_tick() {
    const uintptr_t cur = *mh::state::ptr<uintptr_t>(mh::state::RID_STRAT_CUR_BUILDING);
    if (cur)
        sanitize(*reinterpret_cast<building *>(cur), rd<uint16_t>(mh::addr::_G_LLM_STRAT_CUR_PLAYER),
                 *mh::state::ptr<uint16_t>(mh::state::RID_STRAT_CUR_INDEX), "building_tick");
    return cur;
}

// Site (B): retail addresses buildings[(uint16)player][index] -- the same row the region binds, at the host's cap.
void __cdecl guard_charge(uint32_t player, uint32_t index) {
    player &= 0xffffu; // retail: MOVZX word [EBP-0x14]
    const auto caps = mh::state::live_roster_caps();
    auto      *bl   = reinterpret_cast<building *>(mh::state::live().base[mh::state::RID_BUILDINGS]);
    if (!bl || player >= 8 || caps.buildings <= 0 || index >= (uint32_t)caps.buildings) return;
    sanitize(bl[player * (uint32_t)caps.buildings + index], player, index, "update_charge_pips");
}

// clang-format off
__declspec(naked) void tick_stub() {
    __asm {
        pushad
        call  guard_tick
        mov   dword ptr [esp + 28], eax        // pushad's EAX slot: the displaced MOV EAX,[_G_LLM_STRAT_CUR_BUILDING]
        popad
        ret
    }
}

__declspec(naked) void charge_stub() {
    __asm {
        pushad
        push  edx                              // building index
        push  eax                              // player
        call  guard_charge
        add   esp, 8
        popad
        mov   dword ptr [ebp - 0x14], eax      // the two displaced stores
        mov   dword ptr [ebp - 0x1c], edx
        ret
    }
}
// clang-format on

// CALL rel32 to `stub` from `site`, NOP-padded to 6 bytes (only the first `len` are written).
void call_repl(uint8_t (&repl)[6], uintptr_t site, void (*stub)()) {
    repl[0]           = 0xE8;
    const int32_t rel = (int32_t)((uintptr_t)stub - (site + 5));
    memcpy(repl + 1, &rel, sizeof(rel));
    repl[5] = 0x90;
}

} // namespace

void install_bldg_pip_guard(bool on) {
    if (!on) {
        seam_log("; bldg pip guard OFF ([net] bldg_pip_guard=0): retail -- a building pip_level outside 0..4 indexes "
                 "the 4-entry A_OGIEN table (field crashes 0x0047908b / 0x004793e1; the reproduction arm)\n");
        return;
    }
    const char *displaced = "DISPLACED by promotion (libmh's twin carries it)";
    const char *a, *b;
    uint8_t     repl[6];
    if (mh::hook::promoted_owner_of(SITE_TICK)) {
        a = displaced;
    } else {
        call_repl(repl, SITE_TICK, &tick_stub);
        a = mh::hook::patch_bytes_guarded(SITE_TICK, TICK_EXPECT, repl, 5) ? "PATCHED" : "MISMATCHED";
    }
    if (mh::hook::promoted_owner_of(SITE_CHARGE)) {
        b = displaced;
    } else {
        call_repl(repl, SITE_CHARGE, &charge_stub);
        b = mh::hook::patch_bytes_guarded(SITE_CHARGE, CHARGE_EXPECT, repl, 6) ? "PATCHED" : "MISMATCHED";
    }
    char m[400];
    wsprintfA(m,
              "; bldg pip guard ON ([net] bldg_pip_guard=1): building_tick @0046FE67 %s; update_charge_pips @00478ECB %s"
              " -- a pip_level outside 0..4 or pip_active_count outside 0..16 is zeroed and logged as `[pip] GUARD`%s\n",
              a, b, (a[0] == 'M' || b[0] == 'M') ? " -- THIS RUN DOES NOT HAVE THE FIX at a MISMATCHED site" : "");
    seam_log(m);
}
