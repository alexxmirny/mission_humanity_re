//
// seams/move_proto_respond.cpp -- mods:M1G step 4c: the movement RESPONSIVENESS prototype keys (design: tmp/m1g/4c_plan.md).
//
// Two jump-splices, ALWAYS installed (install_move_proto_respond, chained from install_move_proto_blocked), runtime-gated by two
// globals the gameplay setters flip; both keys at their retail value = the retail bytes' own behaviour. Host-set ([net], F_HOSTSYNC,
// C_DEV): they change the sim, so every peer must run the same values. The rules are sim/move/respond.h, shared with the libmh twins
// (sim_order_dispatch.cpp retain path, sim_unit_state_move_walker.cpp turn branch).
//
//   [net] move_reverse_now  SITE 0x00466bfe, llm_strat_order_queue_dispatch's RETAIN path (the record was just deferred because its unit
//                           is sliding between tiles): the 7 bytes `IMUL EDX,[EBP-0x34],0x5b04` (69 55 CC 04 5B 00 00) become
//                           JMP stub + 2 NOP. The stub (pushad) asks rs_reverse(player [EBP-0x34], unit [EBP-0x30], slot [EBP-0x24],
//                           boarding [EBP-0x28]); the record is order_queue + slot * 0x44. It then re-executes the displaced IMUL and
//                           rejoins at 0x00466c05, so the retail retain (order_queued = 1, record copy, kept++) runs unchanged.
//   [net] move_turn_rate    SITE 0x0047d5b4, the walker's TURN branch: `FMUL qword [EDX+0xae]; FSTP qword [EBP-0x40]` (DC 8A AE 00 00 00
//                           DD 5D C0, 9 bytes: turn_cost = cfg turn_speed[player] * move_step_speed_scale) becomes JMP stub + 4 NOP.
//                           The stub does the same FMUL, then (key on) FMUL by 2^-k, FSTP, and rejoins at 0x0047d5bd. Scaling by a
//                           power of two is exact in x87 and SSE2, so splice and twin agree bit for bit.
//
// If libmh owns the dispatcher / the walker the site is DISPLACED or unreached and the twin carries the key; the late carrier report
// (move_carrier.cpp) says which.
//
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <cstring>

#include "addr/mh_calls.gen.h"   // mh::call::llm_strat_fow_remove_sight / map_fow_UpdateFoWPlus / llm_strat_unit_soldiers_set_heading
#include "addr/mh_regions.gen.h" // mh::state::live(), RID_*
#include "addr/mh_structs.gen.h"
#include "hook/patch.h"    // patch_bytes_guarded
#include "hook/promoted.h" // promoted_owner_of
#include "caps/caps.h"     // mh::caps::rec / g + mh_caps_row_bytes for rv_stub (PT-CAP8)
#include "caps/orders.h"   // mh::caps::orders::base / cap: the order queue the game addresses (PT-CAP9-U5)
#include "seams/net_internal.h"
#include "seams/move_carrier.h"
#include "en_guard.h"
#include "sim/move/respond.h" // the rules, shared with the libmh twins

#pragma comment(lib, "user32.lib") // wsprintfA

namespace {

using mh::game::mh_cfg_final_struct_Unit;
using mh::game::mh_llm_strat_crew_soldier;
using mh::game::mh_llm_strat_map_geom;
using mh::game::mh_map_object_unit;
using mh::game::mh_map_tile_object_data;

constexpr uintptr_t ADDR_RV_SITE   = 0x00466bfe; // dispatcher retain path: IMUL EDX,[EBP-0x34],0x5b04
constexpr uintptr_t ADDR_RV_RET    = 0x00466c05; // the next instruction (IMUL EAX,[EBP-0x30],0xe9)
const uint8_t       RV_EXPECT[7]   = {0x69, 0x55, 0xCC, 0x04, 0x5B, 0x00, 0x00};
constexpr uintptr_t ADDR_TR_SITE   = 0x0047d5b4; // walker turn branch: FMUL [EDX+0xae]; FSTP [EBP-0x40]
constexpr uintptr_t ADDR_TR_RET    = 0x0047d5bd; // the budget gate that follows
const uint8_t       TR_EXPECT[9]   = {0xDC, 0x8A, 0xAE, 0x00, 0x00, 0x00, 0xDD, 0x5D, 0xC0};
constexpr uintptr_t ADDR_DIR_DELTA = 0x0051de40; // _G_LLM_MAP_DIR_STEP_DELTAS: int8 {dx, dy} per 24-compass heading
constexpr uint32_t  ORDER_STRIDE   = 0x44;

int g_rv_state = 0; // 1 carried, -1 MISMATCHED, -2 DISPLACED, 0 not installed
int g_tr_state = 0;

const char *rs_state_name(int s) {
    return s == 1 ? "carried" : (s == -1 ? "the splice MISMATCHED" : (s == -2 ? "DISPLACED (libmh owns the function; its twin carries the key, see the carrier line)" : "not installed"));
}

// ---- state access (live bases: the hosted exe or the standalone host's own allocation) ---------------------------------------
mh_map_object_unit *unit_rec(uint32_t player, int32_t idx) {
    const auto &u = mh::caps::g(mh::caps::UNITS);
    if (player >= u.players || idx < 0 || (uint32_t)idx >= u.limit) return nullptr;
    return mh::caps::rec<mh_map_object_unit>(mh::caps::UNITS, player, (uint32_t)idx);
}
const mh_cfg_final_struct_Unit *cfg_of(const mh_map_object_unit &u) {
    const auto *c = reinterpret_cast<const mh_cfg_final_struct_Unit *>(mh::state::live().base[mh::state::RID_UNIT]);
    return c ? c + u.unit_proto_id : nullptr;
}

// The retail splice's WORLD for the shared rule (mh::sim::mvc, sim/move/respond.h): live game memory + the game's own functions.
struct splice_world {
    mh_map_object_unit             *unit(uint32_t player, int32_t idx) { return unit_rec(player, idx); }
    const mh_cfg_final_struct_Unit *cfg(const mh_map_object_unit &u) { return cfg_of(u); }
    mh_map_tile_object_data        &tile(uint32_t x, uint32_t y) {
        return reinterpret_cast<mh_map_tile_object_data *>(mh::state::live().base[mh::state::RID_TILE_OBJECTS])[((x & 0xffu) << 8) | (y & 0xffu)];
    }
    uint8_t &pass(uint32_t x, uint32_t y) {
        return reinterpret_cast<uint8_t *>(mh::state::live().base[mh::state::RID_PASSABLE])[((x & 0xffu) << 8) | (y & 0xffu)];
    }
    void dir_delta(int h, int *dx, int *dy) {
        const int8_t *t = reinterpret_cast<const int8_t *>(ADDR_DIR_DELTA);
        *dx             = t[h * 2];
        *dy             = t[h * 2 + 1];
    }
    uint32_t                   mask_x() { return reinterpret_cast<const mh_llm_strat_map_geom *>(mh::state::live().base[mh::state::RID_GENERAL])->width_mask; }
    uint32_t                   mask_y() { return reinterpret_cast<const mh_llm_strat_map_geom *>(mh::state::live().base[mh::state::RID_GENERAL])->height_mask; }
    mh_llm_strat_crew_soldier *soldier(uint32_t player, uint32_t si) {
        if (si >= mh::caps::g(mh::caps::SOLDIERS).limit) return nullptr;
        return mh::caps::rec<mh_llm_strat_crew_soldier>(mh::caps::SOLDIERS, player, si);
    }
    void note_reverse(uint32_t player, int32_t idx, uint32_t fx, uint32_t fy, uint32_t tx, uint32_t ty, int ms) {
        if (g_mv_hit[MVH_REVERSE] >= 200) return; // the evidence line: bounded, a log only (the hit is counted after the apply)
        char b[160];
        wsprintfA(b, "; M1G reverse p%u unit %d %u,%u -> %u,%u ms %d\n", player, (int)idx, fx, fy, tx, ty, ms);
        seam_log(b);
    }
    void fow_remove_sight(uint32_t player, int32_t x, int32_t y, uint8_t sight) { mh::call::llm_strat_fow_remove_sight(player, x, y, sight); }
    void fow_update(uint32_t player, uint32_t x, uint32_t y, uint8_t sight) { mh::call::map_fow_UpdateFoWPlus(player, x, y, sight); }
    void soldiers_set_heading(uint16_t player, int32_t idx, uint8_t h) { mh::call::llm_strat_unit_soldiers_set_heading(player, idx, h); }
};

} // namespace

// Runtime gates (read by the stubs). Retail values: reverse off, turn scale off.
extern "C" {
int    g_rs_rev_on     = 0;   // [net] move_reverse_now in force
int    g_rs_turn_on    = 0;   // [net] move_turn_rate != 0 in force
double g_rs_turn_scale = 1.0; // 2^-k
}

// Called from the dispatcher's retain path with the record just deferred. The rule is mh::sim::mvc::reverse_try (sim/move/respond.h).
static void __cdecl rs_reverse(uint32_t player, int32_t idx, int32_t slot, int32_t boarding) {
    // PT-CAP9-U5: the queue the GAME is using (the wide array with the order layer ON) and its live bound, not the registry's stock mirror.
    const auto *queue = reinterpret_cast<const mh::game::mh_llm_strat_order *>(mh::caps::orders::base(mh::caps::orders::QUEUE));
    if (!queue || slot < 0 || static_cast<uint32_t>(slot) >= mh::caps::orders::cap(mh::caps::orders::QUEUE)) return;
    const mh::game::mh_llm_strat_order &q = queue[slot];
    splice_world                        w;
    if (mh::sim::mvc::reverse_try(w, player, idx, q.args[0], q.args[1], static_cast<uint16_t>(q.order_code), boarding != 0)) mv_hit(MVH_REVERSE);
}

// clang-format off
__declspec(naked) static void rv_stub() {
    __asm {
        pushad
        cmp   g_rs_rev_on, 0
        je    rv_skip
        push  dword ptr [ebp - 0x28]            // is_boarding (the dispatcher's cached value)
        push  dword ptr [ebp - 0x24]            // slot
        push  dword ptr [ebp - 0x30]            // the record's unit index
        push  dword ptr [ebp - 0x34]            // player
        call  rs_reverse
        add   esp, 16
    rv_skip:
        popad
        mov   edx, dword ptr [ebp - 0x34]           // the displaced `imul edx,[ebp-0x34],0x5b04`, with the stride read from
        imul  edx, dword ptr [mh_caps_row_bytes + 4 * MH_CAPS_IDX_UNITS] // the caps table (stock 100*233 = 0x5b04 today)
        push  0x00466c05
        ret
    }
}

__declspec(naked) static void tr_stub() {
    __asm {
        fmul  qword ptr [edx + 0xae]            // turn_cost = turn_speed[player] * move_step_speed_scale (EDX = CUR_UNIT)
        cmp   g_rs_turn_on, 0
        je    tr_done
        fmul  qword ptr g_rs_turn_scale         // x 2^-k: exact
        inc   dword ptr [g_mv_hit + 4 * MVH_TURN]
    tr_done:
        fstp  qword ptr [ebp - 0x40]
        push  0x0047d5bd
        ret
    }
}
// clang-format on

namespace {

// E9 rel32 to `stub`, NOP-padded to `n` bytes (n <= 9).
void make_jmp(uint8_t *repl, size_t n, uintptr_t addr, void *stub) {
    const int32_t rel = (int32_t)((uintptr_t)stub - (addr + 5));
    repl[0]           = 0xE9;
    memcpy(repl + 1, &rel, sizeof(rel));
    for (size_t i = 5; i < n; ++i) repl[i] = 0x90;
}

} // namespace

void install_move_proto_respond() {
    uint8_t repl[9];
    if (mh::hook::promoted_owner_of(ADDR_RV_SITE)) {
        g_rv_state = -2;
    } else {
        make_jmp(repl, sizeof(RV_EXPECT), ADDR_RV_SITE, reinterpret_cast<void *>(&rv_stub));
        g_rv_state = mh::hook::patch_bytes_guarded_caps(ADDR_RV_SITE, RV_EXPECT, repl, sizeof(RV_EXPECT)) ? 1 : -1;
    }
    if (mh::hook::promoted_owner_of(ADDR_TR_SITE)) {
        g_tr_state = -2;
    } else {
        make_jmp(repl, sizeof(TR_EXPECT), ADDR_TR_SITE, reinterpret_cast<void *>(&tr_stub));
        g_tr_state = mh::hook::patch_bytes_guarded(ADDR_TR_SITE, TR_EXPECT, repl, sizeof(TR_EXPECT)) ? 1 : -1;
    }
    mh::seams::mv::note_site(mh::seams::mv::SITE_REVERSE, g_rv_state);
    mh::seams::mv::note_site(mh::seams::mv::SITE_TURN, g_tr_state);
    char b[240];
    wsprintfA(b, "; move prototype splice (mods:M1G responsiveness): reverse_now %s; turn_rate %s\n", rs_state_name(g_rv_state), rs_state_name(g_tr_state));
    seam_log(b);
}

// The setters set the stubs' gates and tell the carrier report (move_carrier.cpp) the requested value.
void mh_set_move_reverse_now(int on) {
    on          = on != 0;
    g_rs_rev_on = 0;
    mh::seams::mv::request(mh::seams::mv::KEY_REVERSE_NOW, on, 0);
    if (!on || g_rv_state != 1) return;
    g_rs_rev_on = 1;
}

void mh_set_move_turn_rate(int k) {
    k            = mh::sim::mvc::turn_rate_clamp(k);
    g_rs_turn_on = 0;
    mh::seams::mv::request(mh::seams::mv::KEY_TURN_RATE, k, 0);
    if (k == 0 || g_tr_state != 1) return;
    g_rs_turn_scale = mh::sim::mvc::turn_scale(k);
    g_rs_turn_on    = 1;
}
