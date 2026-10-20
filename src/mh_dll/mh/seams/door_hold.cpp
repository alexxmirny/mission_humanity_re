//
// seams/door_hold.cpp -- mods:M1BE: [net] storage_door_hold, the retail splices. The rule is mh::sim::mvc::dh_entering_near
// (sim/move/enter_queue.h), the same code the libmh twin runs (sim_bldg_anim_state_online.cpp: door_may_close).
//
// THE SITES. The three storage-door building animation state handlers (the building TICK2 table, _G_LLM_STRAT_BLDG_TICK2_FUNCS):
//   llm_strat_bldg_anim_state_soldiers_h          0x00476e9d  (type 0x1c, the human barracks)
//   llm_strat_bldg_anim_state_vehicles_h          0x004769a9  (type 0x1b)
//   llm_strat_bldg_anim_state_barracks_garage_a   0x00476627  (types 0x07, 0x08)
// share one shape in the OPEN door state (online_state 2, `caseD_2`): `if (storage.door_mutex_unit == 0 && storage.door_waiter_count == 0)
// { anim = closing; online_state = 4 } else { anim = open-hold }`. The case opens with `MOV EAX,[CUR_BUILDING]` (A1 DC 62 E1 00, 5 bytes),
// which becomes `JMP stub`. The stub asks dh_decide(): 1 = an entering friend of this storage is within 2 tiles of the door tile, so jump to
// the handler's own "not idle" branch (the open-hold; the door stays open); 0 = run the displaced MOV and continue at the next instruction.
//
//   handler            site (caseD_2)  continue       open-hold branch
//   soldiers_h         0x00477075      0x0047707a     0x00477100
//   vehicles_h         0x00476b81      0x00476b86     0x00476c0c
//   barracks_garage_a  0x004767ed      0x004767f2     0x00476878
//
// Key 0 = the stubs return 0 at once: the displaced instruction plus the original continuation, so the handlers run their retail bytes.
// Determinism: units scanned in index order, integer reads only (unit records, tile objects, the storage record), no clock, no rand; it writes
// nothing. If libmh fills the building tick2 table with its own thunks these retail bodies are dead and the libmh twin carries the key (the carrier
// report in move_carrier.cpp says which).
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <cstring>

#include "addr/mh_regions.gen.h" // mh::state::live(), RID_*
#include "addr/mh_structs.gen.h"
#include "state/roster_caps.h"
#include "hook/patch.h"    // patch_bytes_guarded
#include "hook/promoted.h" // promoted_owner_of
#include "seams/net_internal.h"
#include "seams/move_carrier.h"
#include "sim/move/enter_queue.h" // the rule, shared with the libmh twin

#pragma comment(lib, "user32.lib") // wsprintfA

namespace {

using mh::game::mh_map_object_unit;
using mh::game::mh_map_object_unit_storage;
using mh::game::mh_map_tile_object_data;

constexpr uintptr_t ADDR_CUR_BUILDING = 0x00e162dc; // _G_LLM_STRAT_CUR_BUILDING (pointer)
constexpr uintptr_t ADDR_CUR_PLAYER   = 0x00e58144; // _G_LLM_STRAT_CUR_PLAYER (word)
constexpr uint32_t  OFF_SUB_ID        = 0xc6;       // building.sub_id (byte): the storage slot

constexpr uintptr_t ADDR_DH_SOLDIERS = 0x00477075; // soldiers_h caseD_2
constexpr uintptr_t ADDR_DH_VEHICLES = 0x00476b81; // vehicles_h caseD_2
constexpr uintptr_t ADDR_DH_BARRACKS = 0x004767ed; // barracks_garage_a caseD_2

const uint8_t DH_EXPECT[5] = {0xA1, 0xDC, 0x62, 0xE1, 0x00}; // MOV EAX,[CUR_BUILDING]

int g_dh_state_soldiers = 0; // 1 carried, -1 MISMATCHED, -2 DISPLACED, 0 not installed
int g_dh_state_vehicles = 0;
int g_dh_state_barracks = 0;

const char *dh_state_name(int s) {
    return s == 1 ? "carried" : (s == -1 ? "the splice MISMATCHED" : (s == -2 ? "DISPLACED (libmh owns the function; its twin carries the key, see the carrier line)" : "not installed"));
}

mh_map_object_unit *unit_rec(uint32_t player, int32_t idx) {
    const auto caps = mh::state::live_roster_caps();
    if (player >= 8 || idx < 0 || idx >= caps.units) return nullptr;
    return reinterpret_cast<mh_map_object_unit *>(mh::state::live().base[mh::state::RID_UNITS]) + player * caps.units + idx;
}

// The retail splice's WORLD for the shared rule (sim/move/enter_queue.h): live game memory.
struct splice_world {
    mh_map_object_unit      *unit(uint32_t player, int32_t idx) { return unit_rec(player, idx); }
    mh_map_tile_object_data &tile(uint32_t x, uint32_t y) {
        return reinterpret_cast<mh_map_tile_object_data *>(mh::state::live().base[mh::state::RID_TILE_OBJECTS])[((x & 0xffu) << 8) | (y & 0xffu)];
    }
    uint32_t mask_x() { return reinterpret_cast<const mh::game::mh_llm_strat_map_geom *>(mh::state::live().base[mh::state::RID_GENERAL])->width_mask; }
    uint32_t mask_y() { return reinterpret_cast<const mh::game::mh_llm_strat_map_geom *>(mh::state::live().base[mh::state::RID_GENERAL])->height_mask; }
    int32_t  unit_limit() { return mh::state::live_roster_caps().units; }
    void     hit_dh() { mv_hit(MVH_DH); }
};

} // namespace

// Runtime gate (read by the stubs). Retail value: off.
extern "C" {
int       g_dh_on        = 0; // [net] storage_door_hold in force
uintptr_t g_dh_ret_sold  = 0x0047707a;
uintptr_t g_dh_hold_sold = 0x00477100;
uintptr_t g_dh_ret_veh   = 0x00476b86;
uintptr_t g_dh_hold_veh  = 0x00476c0c;
uintptr_t g_dh_ret_bar   = 0x004767f2;
uintptr_t g_dh_hold_bar  = 0x00476878;
}

// 1 = keep the open door open. Only a door that is otherwise idle can close, so only then is the scan worth doing.
static int __cdecl dh_decide() {
    if (!g_dh_on) return 0;
    const uint32_t player = *reinterpret_cast<const volatile uint16_t *>(ADDR_CUR_PLAYER);
    const uint8_t *b      = *reinterpret_cast<const uint8_t *const *>(ADDR_CUR_BUILDING);
    if (!b) return 0;
    const uint32_t slot = b[OFF_SUB_ID];
    const auto     caps = mh::state::live_roster_caps();
    if (player >= 8 || slot >= static_cast<uint32_t>(caps.storage)) return 0;
    const auto *st = reinterpret_cast<const mh_map_object_unit_storage *>(mh::state::live().base[mh::state::RID_UNIT_STORAGE]) + player * caps.storage + slot;
    if (st->door_mutex_unit != 0 || st->door_waiter_count != 0) return 0; // retail keeps the door open anyway
    splice_world w;
    return mh::sim::mvc::dh_entering_near(w, player, static_cast<int32_t>(slot), st->exit_tile_x, st->exit_tile_y) ? 1 : 0;
}

// clang-format off
__declspec(naked) static void dh_stub_soldiers() {
    __asm {
        pushad
        call  dh_decide
        test  eax, eax
        jnz   dh_hold
        popad
        mov   eax, dword ptr ds:[0x00e162dc]    // the displaced instruction
        jmp   dword ptr [g_dh_ret_sold]
    dh_hold:
        popad
        jmp   dword ptr [g_dh_hold_sold]
    }
}

__declspec(naked) static void dh_stub_vehicles() {
    __asm {
        pushad
        call  dh_decide
        test  eax, eax
        jnz   dh_hold
        popad
        mov   eax, dword ptr ds:[0x00e162dc]    // the displaced instruction
        jmp   dword ptr [g_dh_ret_veh]
    dh_hold:
        popad
        jmp   dword ptr [g_dh_hold_veh]
    }
}

__declspec(naked) static void dh_stub_barracks() {
    __asm {
        pushad
        call  dh_decide
        test  eax, eax
        jnz   dh_hold
        popad
        mov   eax, dword ptr ds:[0x00e162dc]    // the displaced instruction
        jmp   dword ptr [g_dh_ret_bar]
    dh_hold:
        popad
        jmp   dword ptr [g_dh_hold_bar]
    }
}
// clang-format on

namespace {

// The JMP rel32 from addr to stub. Each site calls patch_bytes_guarded with its own ADDR_DH_* constant so
// lint_dll_patches can key the call site to the manifest entry.
void jmp_to(uintptr_t addr, void *stub, uint8_t (&repl)[5]) {
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(stub) - (addr + 5));
    repl[0]           = 0xE9;
    memcpy(repl + 1, &rel, sizeof(rel));
}

} // namespace

void install_door_hold() {
    uint8_t repl[5];
    if (mh::hook::promoted_owner_of(ADDR_DH_SOLDIERS)) {
        g_dh_state_soldiers = -2;
    } else {
        jmp_to(ADDR_DH_SOLDIERS, reinterpret_cast<void *>(&dh_stub_soldiers), repl);
        g_dh_state_soldiers = mh::hook::patch_bytes_guarded(ADDR_DH_SOLDIERS, DH_EXPECT, repl, 5) ? 1 : -1;
    }
    if (mh::hook::promoted_owner_of(ADDR_DH_VEHICLES)) {
        g_dh_state_vehicles = -2;
    } else {
        jmp_to(ADDR_DH_VEHICLES, reinterpret_cast<void *>(&dh_stub_vehicles), repl);
        g_dh_state_vehicles = mh::hook::patch_bytes_guarded(ADDR_DH_VEHICLES, DH_EXPECT, repl, 5) ? 1 : -1;
    }
    if (mh::hook::promoted_owner_of(ADDR_DH_BARRACKS)) {
        g_dh_state_barracks = -2;
    } else {
        jmp_to(ADDR_DH_BARRACKS, reinterpret_cast<void *>(&dh_stub_barracks), repl);
        g_dh_state_barracks = mh::hook::patch_bytes_guarded(ADDR_DH_BARRACKS, DH_EXPECT, repl, 5) ? 1 : -1;
    }
    mh::seams::mv::note_site(mh::seams::mv::SITE_DOOR_SOLDIERS, g_dh_state_soldiers);
    mh::seams::mv::note_site(mh::seams::mv::SITE_DOOR_VEHICLES, g_dh_state_vehicles);
    mh::seams::mv::note_site(mh::seams::mv::SITE_DOOR_BARRACKS, g_dh_state_barracks);
    char b[320];
    wsprintfA(b, "; move prototype splice (mods:M1BE storage door hold): soldiers_h %s; vehicles_h %s; barracks_garage_a %s\n", dh_state_name(g_dh_state_soldiers),
              dh_state_name(g_dh_state_vehicles), dh_state_name(g_dh_state_barracks));
    seam_log(b);
}

// The setter sets the stubs' gate and tells the carrier report (move_carrier.cpp) the requested value. A site that is not carried (displaced,
// mismatched) leaves its handler on the retail rule; the gate is global, so it is armed whenever at least one splice is in.
void mh_set_storage_door_hold(int on) {
    on      = on != 0;
    g_dh_on = 0;
    mh::seams::mv::request(mh::seams::mv::KEY_STORAGE_DOOR_HOLD, on, 0);
    if (!on || (g_dh_state_soldiers != 1 && g_dh_state_vehicles != 1 && g_dh_state_barracks != 1)) return;
    g_dh_on = 1;
}
