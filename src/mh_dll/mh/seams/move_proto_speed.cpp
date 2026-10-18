//
// seams/move_proto_speed.cpp -- mods:M1X lane C, the movement PROTOTYPE key [net] move_group_speed (movement plan 4a,
// Pottinger's group speed). Default -1 = follow [net] group_movement (on: 1); 0 = retail.
//
// Retail has no group speed: every unit steps at its own cfg step_speed, so a mixed selection stretches out and arrives
// piecemeal. The carrier is the unit record's own move_step_speed_scale (+0xae, a double): the per-microstep cost in
// llm_strat_unit_state_move_walker 0x0047c903 is cfg step_speed[player] * move_step_speed_scale * dir_step_factor, and
// the turn cost is cfg turn_speed[player] * move_step_speed_scale. Retail averages the scale with the new tile's
// passable value at every tile step (open ground = 1), so in practice it is always 1.0 ("meant as terrain speed, inert").
//
// Two call-splices, ALWAYS installed, runtime-gated by g_group_speed_rt (off = the retail bytes' own behaviour):
//   COMMIT  llm_strat_unit_state_group_marshal 0x00482451, at 0x00482897 -- the `MOV [EBP-0x34],0` right after each
//           wave's CALL llm_strat_group_move_order_commit (the settle loop's `w = 0`). With the key on, when the wave is
//           a move order (is_move) whose GROUND members (cfg type < A_HELI 0xf) have different step_speeds, every member
//           faster than the slowest gets scale = slowest_step_speed / own_step_speed (> 1.0), TAGGED: the low 4 mantissa
//           bits are forced to 0xB (a relative change below 1e-15). Any other ground member of any wave (attack, single,
//           same-speed, or the slowest one) whose scale carries the tag is reset to 1.0. So the wave walks at its slowest
//           member's speed, and the next order's commit clears the factor (a unit leaving the group does not stay slow).
//           An untagged scale is never written: retail's own values stay exactly retail (a unit fresh from a factory
//           starts at 0 and averages up toward 1; stepping onto a tile held by unit index 0 reads passable 5 and pushes
//           the scale ABOVE 1 -- so "> 1.0" alone cannot mean "ours", hence the tag).
//   WALK    llm_strat_unit_state_move_walker, at 0x0047d3d1 -- the averaging `scale = (passable[new] + scale) * 0.5`
//           (0x0047d3d1-0x0047d3f6). With the key on and a TAGGED scale >= 1.0 the scale is kept; otherwise the retail
//           averaging runs. So with the key on and no mixed-speed wave, the sim is the retail sim. (A retail value can
//           only carry the tag after ~50 consecutive halvings of an odd fraction, i.e. effectively never; it would then
//           be held within 1e-15 of its value until the next order.)
//
// Determinism: the scale is the unit record's own field, so it is in the save and in the hash region RID_UNITS (no new
// state). COMMIT's ratio is one IEEE double division of two cfg doubles (SSE2 in this DLL), the same on every peer; the
// walker then uses it with its own x87 ops, unchanged. Units are visited in the wave's scratch order (deterministic).
// The WALK test is an integer compare on the double's high dword. If libmh promotes either owner, the site is DISPLACED
// and the key logs NOT ACTIVE (no libmh twin in this phase).
//
#include <windows.h>
#include <cstdint>
#include <cstring>
#include "addr/mh_regions.gen.h" // mh::state::live(), RID_UNITS / RID_UNIT / RID_STRAT_GROUP_MOVE_SCRATCH
#include "addr/mh_structs.gen.h" // mh_map_object_unit / mh_cfg_final_struct_Unit / mh_llm_strat_group_scratch_member
#include "hook/patch.h"          // patch_bytes_guarded
#include "hook/promoted.h"       // promoted_owner_of
#include "state/roster_caps.h"   // live_roster_caps()
#include "net_internal.h"        // seam_log

int g_group_speed_rt = 0; // [net] move_group_speed in force (1) / off (0)

namespace {

constexpr uintptr_t ADDR_MVS_COMMIT = 0x00482897, ADDR_MVS_WALK = 0x0047d3d1;
const uint8_t       MVS_COMMIT_EXPECT[7] = {0xC7, 0x45, 0xCC, 0x00, 0x00, 0x00, 0x00}; // MOV dword ptr [EBP-0x34],0
const uint8_t       MVS_WALK_EXPECT[38]  = {
    0xDF, 0x45, 0xB0,                    // FILD  word ptr [EBP-0x50]       (passable[new])
    0xDC, 0x82, 0xAE, 0x00, 0x00, 0x00,  // FADD  double ptr [EDX+0xae]
    0xDD, 0x9A, 0xAE, 0x00, 0x00, 0x00,  // FSTP  double ptr [EDX+0xae]
    0xA1, 0xE0, 0x62, 0xE1, 0x00,        // MOV   EAX,[0x00e162e0]          (_G_LLM_STRAT_CUR_UNIT)
    0xDD, 0x80, 0xAE, 0x00, 0x00, 0x00,  // FLD   double ptr [EAX+0xae]
    0xDC, 0x0D, 0xC0, 0x13, 0x50, 0x00,  // FMUL  double ptr [0x005013c0]   (0.5)
    0xDD, 0x98, 0xAE, 0x00, 0x00, 0x00}; // FSTP  double ptr [EAX+0xae]
int                g_state_commit = 0, g_state_walk = 0, g_state = 0;
constexpr uint32_t TAG_MASK = 0xF, TAG = 0xB; // low mantissa bits of a scale this key wrote

bool is_tagged(double d) {
    uint64_t b;
    memcpy(&b, &d, sizeof(b));
    return (b & TAG_MASK) == TAG;
}
double tagged(double d) {
    uint64_t b;
    memcpy(&b, &d, sizeof(b));
    b = (b & ~uint64_t(TAG_MASK)) | TAG;
    memcpy(&d, &b, sizeof(b));
    return d;
}
uint32_t           g_wave_logs      = 0;
constexpr uint32_t MAX_WAVE_LOGS    = 400;
constexpr uint32_t UNIT_TYPE_A_HELI = 0xf; // cfg_enum_E_UNIT_TYPE: types >= this fly (no ground walker, scale not used)

// Called after each wave's group_move_order_commit, with the marshal's frame pointer. Frame slots (EN disassembly
// 0x00482876-0x00482892, the commit call's arguments): [EBP-0x28] leg_count (the wave size; rewritten by the wave
// compaction at 0x00482ed3), [EBP-0x30] move_group_id, [EBP-0x1c] is_move (1 = plain move, 0 = attack).
void __cdecl group_speed_apply(const uint8_t *frame) {
    const int32_t  legs    = *reinterpret_cast<const int32_t *>(frame - 0x28);
    const int32_t  is_move = *reinterpret_cast<const int32_t *>(frame - 0x1c);
    const uint32_t player  = *reinterpret_cast<const volatile uint16_t *>(0x00e58144u); // _G_LLM_STRAT_CUR_PLAYER
    const auto     caps    = mh::state::live_roster_caps();
    if (legs < 1 || legs > 100 || player >= 8) return;
    const auto &lv      = mh::state::live();
    auto       *units   = reinterpret_cast<mh::game::mh_map_object_unit *>(lv.base[mh::state::RID_UNITS]) + player * caps.units;
    const auto *cfg     = reinterpret_cast<const mh::game::mh_cfg_final_struct_Unit *>(lv.base[mh::state::RID_UNIT]);
    const auto *scratch = reinterpret_cast<const mh::game::mh_llm_strat_group_scratch_member *>(lv.base[mh::state::RID_STRAT_GROUP_MOVE_SCRATCH]);

    double  slowest = 0.0, fastest = 0.0;
    int32_t ground = 0;
    for (int32_t w = 0; w < legs; ++w) {
        const int32_t idx = scratch[w].unit_idx;
        if (idx < 0 || idx >= caps.units) continue;
        const auto &c = cfg[units[idx].unit_proto_id];
        if (c.type >= UNIT_TYPE_A_HELI) continue;
        const double ss = c.step_speed[player];
        if (!(ss > 0.0)) continue;
        if (ss > slowest) slowest = ss; // a larger step_speed is a larger per-microstep cost: slower
        if (ground == 0 || ss < fastest) fastest = ss;
        ++ground;
    }
    // (2026-10-14, tmp/m1/attack_stall.md problem 2) The rule follows the ORDER, not the wave. A wave is only
    // the part of the order that marshalled together this tick; spread selections split into several waves, a member whose
    // path ends off its tile or whose first step is blocked re-marshals as a wave of ONE, and a mid-walk re-order lets
    // each member marshal at its own tile end. Each of those waves saw only itself and walked at its own speed. So the
    // slowest is taken over every live ground unit of this player carrying the wave's move_group_id (frame [EBP-0x30];
    // 0 = Ctrl/id-0 orders, which keep the wave-only rule) or holding a queued record of it. Table order, compares only, no
    // new state. Known side effect: a unit's later automatic move (e.g. a passive-engage return walk) that keeps the old
    // move_group_id also walks at the order's slowest speed.
    const uint32_t mgid = *reinterpret_cast<const uint32_t *>(frame - 0x30);
    if (is_move != 0 && ground > 0 && mgid != 0) {
        for (int32_t i = 1; i < caps.units; ++i) {
            const auto &o = units[i];
            if ((uint32_t)o.move_group_id != mgid || !(o.energy > 0.0) || o.unit_proto_id >= 512) continue;
            const auto &c = cfg[o.unit_proto_id];
            if (c.type >= UNIT_TYPE_A_HELI) continue;
            const double ss = c.step_speed[player];
            if (ss > slowest) slowest = ss;
        }
        // ...and every unit whose record of this order still waits in the due-now queue (a mid-walk re-order is taken only at
        // the end of the unit's tile, so the first members to marshal must also count the ones still walking the old order).
        // The queue is sim state (saved, hashed, the same on every peer at this step); records in table order.
        const auto   *q  = reinterpret_cast<const mh::game::mh_llm_strat_order *>(lv.base[mh::state::RID_STRAT_ORDER_QUEUE]);
        const int32_t qn = *reinterpret_cast<const int32_t *>(lv.base[mh::state::RID_STRAT_ORDER_QUEUE_COUNT]);
        for (int32_t k = 0; q && k < qn && k < 300; ++k) {
            const auto &r = q[k];
            if ((r.owner_and_kind & 0xfu) != player || (r.owner_and_kind & 0xf0u) != 0x80u || (uint32_t)r.args[12] != mgid) continue;
            if (r.unit_index >= (uint32_t)caps.units) continue;
            const auto &o = units[r.unit_index];
            if (!(o.energy > 0.0) || o.unit_proto_id >= 512) continue;
            const auto &c = cfg[o.unit_proto_id];
            if (c.type >= UNIT_TYPE_A_HELI) continue;
            const double ss = c.step_speed[player];
            if (ss > slowest) slowest = ss;
        }
    }
    const bool mixed = is_move != 0 && ground > 0 && slowest != fastest;
    for (int32_t w = 0; w < legs; ++w) {
        const int32_t idx = scratch[w].unit_idx;
        if (idx < 0 || idx >= caps.units) continue;
        auto       &u = units[idx];
        const auto &c = cfg[u.unit_proto_id];
        if (c.type >= UNIT_TYPE_A_HELI) continue;
        const double ss = c.step_speed[player];
        if (mixed && ss > 0.0 && ss != slowest) u.move_step_speed_scale = tagged(slowest / ss);                      // > 1.0
        else if (u.move_step_speed_scale > 1.0 && is_tagged(u.move_step_speed_scale)) u.move_step_speed_scale = 1.0; // clear ours
    }
    if (mixed && g_wave_logs < MAX_WAVE_LOGS) {
        ++g_wave_logs;
        char b[200];
        wsprintfA(b, "; group speed (mods:M1X) player %u wave of %d (%d ground): step_speed fastest %d/1000 -> slowest %d/1000\n", player, legs,
                  ground, (int)(fastest * 1000.0), (int)(slowest * 1000.0));
        seam_log(b);
    }
}

// clang-format off
// 0x00482897: `MOV dword ptr [EBP-0x34],0` (7 bytes) becomes CALL + 2 NOP. The stub keeps every register and the flags.
__declspec(naked) void mvs_commit_stub() {
    __asm {
        cmp   g_group_speed_rt, 0
        je    mc_done
        pushad
        pushfd
        push  ebp
        call  group_speed_apply
        add   esp, 4
        popfd
        popad
    mc_done:
        mov   dword ptr [ebp - 0x34], 0          // the instruction this stub replaced
        ret
    }
}

// 0x0047d3d1..0x0047d3f6 (38 bytes, the scale averaging) becomes CALL + 33 NOP. EDX = the current unit on entry (loaded
// at 0x0047d3b7). Both paths leave EAX = _G_LLM_STRAT_CUR_UNIT and the x87 stack balanced, as the original did; the
// following code (0x0047d3f7 MOV EAX,[EBP-0x24]) reads no flags.
__declspec(naked) void mvs_walk_stub() {
    __asm {
        cmp   g_group_speed_rt, 0
        je    mw_retail
        mov   eax, dword ptr [edx + 0xae]       // low dword: the tag in the low 4 mantissa bits?
        and   eax, 0xF
        cmp   eax, 0xB
        jne   mw_retail
        cmp   dword ptr [edx + 0xb2], 0x3FF00000 // and >= 1.0 (high dword, signed: negatives go retail)
        jl    mw_retail
        mov   eax, dword ptr ds:[0x00e162e0]
        ret
    mw_retail:
        fild  word ptr [ebp - 0x50]
        fadd  qword ptr [edx + 0xae]
        fstp  qword ptr [edx + 0xae]
        mov   eax, dword ptr ds:[0x00e162e0]
        fld   qword ptr [eax + 0xae]
        fmul  qword ptr ds:[0x005013c0]
        fstp  qword ptr [eax + 0xae]
        ret
    }
}
// clang-format on

const char *mvs_state_name(int s) {
    return s == 1 ? "carried" : (s == -1 ? "a splice MISMATCHED" : (s == -2 ? "DISPLACED (libmh owns the function; no twin)" : "not installed"));
}

int mvs_call_patch(uintptr_t addr, const uint8_t *expect, int len, void (*stub)(), const char *owner) {
    if (mh::hook::promoted_owner_of(addr)) return -2;
    uint8_t       repl[40];
    const int32_t rel = (int32_t)((uintptr_t)stub - (addr + 5));
    repl[0]           = 0xE8;
    memcpy(repl + 1, &rel, sizeof(rel));
    for (int i = 5; i < len; ++i) repl[i] = 0x90;
    bool ok = false;
    // one literal expression per site: lint_dll_patches keys the manifest on (file, expr)
    switch (addr) {
        case ADDR_MVS_COMMIT: ok = mh::hook::patch_bytes_guarded(ADDR_MVS_COMMIT, expect, repl, len); break;
        case ADDR_MVS_WALK: ok = mh::hook::patch_bytes_guarded(ADDR_MVS_WALK, expect, repl, len); break;
    }
    if (!ok) {
        char b[200];
        wsprintfA(b, "; move prototype (mods:M1X) move_group_speed: splice at %08X in %s MISMATCHED, left untouched\n", (unsigned)addr, owner);
        seam_log(b);
        return -1;
    }
    return 1;
}

} // namespace

void install_move_proto_speed() {
    g_state_commit = mvs_call_patch(ADDR_MVS_COMMIT, MVS_COMMIT_EXPECT, 7, mvs_commit_stub, "llm_strat_unit_state_group_marshal");
    g_state_walk   = mvs_call_patch(ADDR_MVS_WALK, MVS_WALK_EXPECT, 38, mvs_walk_stub, "llm_strat_unit_state_move_walker");
    g_state        = (g_state_commit == 1 && g_state_walk == 1) ? 1 : (g_state_commit < 0 ? g_state_commit : (g_state_walk < 0 ? g_state_walk : 0));
    // The key needs BOTH sites (a reset the averaging erases, or a kept scale nothing resets, is not the rule): it refuses
    // unless both are carried. A lone carried site is inert with the key off.
    char b[200];
    wsprintfA(b, "; move prototype splices (mods:M1X): group speed %s\n", mvs_state_name(g_state));
    seam_log(b);
}

int mh_set_move_group_speed(int on) {
    on         = on != 0;
    int  taken = on && g_state == 1;
    char b[300];
    if (on && taken) {
        g_group_speed_rt = 1;
        wsprintfA(b, "; move prototype [net] move_group_speed=1 active (mods:M1X): every ground unit of a move order walks at the order's slowest speed\n");
        seam_log(b);
    } else {
        g_group_speed_rt = 0;
        if (on) {
            wsprintfA(b, "; move prototype [net] move_group_speed=1 NOT ACTIVE (mods:M1X): %s\n", mvs_state_name(g_state));
            seam_log(b);
        }
    }
    return taken == on;
}
