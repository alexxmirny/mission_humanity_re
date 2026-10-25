//
// seams/combat_proto.cpp -- mods:M1C, the COMBAT prototype keys (retail carriers; libmh twins named per key below).
//
//   [net] move_squad_merge_fix   (dev, -1 = follow [net] improved_movement, preset 1)
//   [net] attack_range_footprint (dev, -1 = follow [net] improved_combat,   preset 1)
//
// Both are ALWAYS-INSTALLED call splices, runtime-gated (off = the retail bytes' own behaviour), each refused (key logs
// NOT ACTIVE) when one of its sites MISMATCHED. A site whose owner libmh promotes is DISPLACED and its libmh twin carries
// the rule (reimpl_fixes -> set_fixes). Integer only; no new state (writes go to fields the retail code writes anyway).
// RE: tmp/m1/squad_and_range.md; this change: tmp/m1/combat_fixes.md.
//
// ---- move_squad_merge_fix ------------------------------------------------------------------------------------------
// llm_strat_unit_state_squad_merge 0x0047ea62 step 3b refuses a merge unless the soldier standing on the goal is in
// state 1 (`CMP word [units.state],1` at 0x0047ebe1). A soldier that is firing (0x1a/0x1b ATTACK_UNIT(_RETURN), 0x1c
// ATTACK_BUILDING) never is, so "select the attacking squad and Shift-click one of its members" silently turns every
// mover back into a standalone idle unit. With the key on, a target that is SETTLED on its tile (move_microstep >= 0x1f)
// in one of those three states, and that would pass the merge's remaining checks (3c family size, 3d same soldier type,
// 3e adjacency -- evaluated here with the same inputs and the same wrap-delta calls, so a refused merge never stops an
// attacker), is released the way llm_strat_unit_state_attack_building releases a dead target (target_release_ref mode 1,
// target_ref/target_index = 0, state = order = 1) before the compare runs; the compare then passes. Walking states
// (0xa/0xf: mid-step, a path slot in flight) are NOT accepted. libmh twin: sim_unit_state_squad_merge.cpp.
//
// ---- attack_range_footprint ----------------------------------------------------------------------------------------
// Range to a BUILDING target is measured to the nearest occupied footprint tile instead of the centre (the rule and its
// consistency argument: libmh/sim/sim_attack_range_footprint.h). Every site is a `CALL llm_strat_bldg_get_coords`
// (0x00449b8a) retargeted to a stub:
//   ISSUE  0x0046bfca reposition / 0x0046c24a reposition_alt_enqueue / 0x0046c49f reposition_enqueue / 0x0046c6f4
//          unit_order_attack_building: the order's goal + target_fine + the issue-time in-range test use the aim tile of
//          the attacker (frame: attacker player word [EBP-0xc], unit [EBP-0x20]; target [EBP-0x10]/[EBP-0x18]; out
//          [EBP-0x1c]/[EBP-0x14] -- identical in all four).
//   JITTER 0x0046c19e / 0x0046c648 / 0x0046c731 (CALL llm_strat_bldg_footprint_random_point 0x00449d41) and 0x0046c3f3
//          (CALL llm_strat_bldg_footprint_random_offset 0x00449c61): the IN-RANGE branch jitters the aim point inside the
//          footprint; with the key on the point is put back to the centre first, so the aim spread stays retail.
//          0x0047d114 (move_walker path end, in range: CALL ..._random_offset on target_fine) likewise.
//   WALK   0x0047d016 llm_strat_unit_state_move_walker path end (order 0x1c): retail passes (CUR_PLAYER, CUR_INDEX) -- the
//          attacker's own index as a building index (bytes at 0x0047d002); the key uses the unit's real target.
//   FIRE   0x0048bbce llm_strat_unit_fire_weapon: the range-check-only coords of the auto (slot 4) mode.
//   REAIM  0x00484fc6 llm_strat_unit_state_attack_building weapon re-select: retail passes &target_fine_x twice (so
//          target_fine_x ends up holding the centre Y); the key writes the aim tile into target_fine_x AND _y.
//   STEP   0x004831c0 llm_strat_unit_group_step_ground: the per-step refresh of goal/target_fine for an attack order.
// Not touched: 0x0046c7ce (unit_order_attack_building_enqueue, order 0x1d: no range test, its point is jittered for a
// queued record), the turret sites in order_queue_dispatch (a building attacker), and the planners
// (group_move_order_pathfind / plan_formation_positions / the marshal's exhausted-path check), which measure against
// the goal / target_fine the sites above wrote -- i.e. against the aim tile.
//
#include <windows.h>
#include <cstdint>
#include <cstring>
#include "addr/mh_calls.gen.h"   // mh::call::llm_strat_target_release_ref / llm_map_wrap_delta_x/_y / llm_strat_bldg_get_coords
#include "addr/mh_regions.gen.h" // mh::state::live(), RID_*
#include "addr/mh_structs.gen.h"
#include "hook/patch.h"                     // patch_bytes_guarded
#include "hook/promoted.h"                  // promoted_owner_of
#include "caps/caps.h"                      // mh::caps::rec / g: the unit + building arrays the game addresses (PT-CAP8)
#include "sim/sim_attack_range_footprint.h" // the shared aim-tile rule (header-only)
#include "sim/sim_projectile_facing.h"      // mods:M1C projectile_facing_fix (header-only)
#include "net_internal.h"                   // seam_log
#include "seams/move_carrier.h"             // mods:M1H: site states, requested values, stub hit counters

int g_sqm_rt  = 0; // [net] move_squad_merge_fix in force
int g_arf_rt  = 0; // [net] attack_range_footprint in force
int g_pff_rt  = 0; // [net] projectile_facing_fix in force
int g_pff_rc3 = 0; // TEST ONLY: projectile_facing_fix=2 -> the rc3 wrapped delta (expected-red arm of prj_facing_rl4)

namespace {

using mh::game::mh_cfg_final_struct_Building;
using mh::game::mh_cfg_final_struct_Unit;
using mh::game::mh_cfg_final_struct_Weapon;
using mh::game::mh_llm_strat_map_geom;
using mh::game::mh_map_object_building;
using mh::game::mh_map_object_unit;

constexpr uintptr_t ADDR_CUR_UNIT = 0x00e162e0, ADDR_CUR_PLAYER = 0x00e58144; // _G_LLM_STRAT_CUR_UNIT / _CUR_PLAYER
constexpr uintptr_t FN_BLDG_GET_COORDS = 0x00449b8a, FN_RANDOM_POINT = 0x00449d41, FN_RANDOM_OFFSET = 0x00449c61;

uintptr_t g_fn_bgc = FN_BLDG_GET_COORDS, g_fn_rp = FN_RANDOM_POINT, g_fn_ro = FN_RANDOM_OFFSET; // read by the stubs

mh_map_object_unit *unit_rec(uint32_t player, int32_t idx) {
    const auto &u = mh::caps::g(mh::caps::UNITS);
    if (player >= u.players || idx < 0 || (uint32_t)idx >= u.limit) return nullptr;
    return mh::caps::rec<mh_map_object_unit>(mh::caps::UNITS, player, (uint32_t)idx);
}
const mh_map_object_building *bldg_rec(uint32_t player, int32_t idx) {
    const auto &bg = mh::caps::g(mh::caps::BUILDINGS);
    if (player >= bg.players || idx < 0 || (uint32_t)idx >= bg.limit) return nullptr;
    const auto *b = mh::caps::rec<const mh_map_object_building>(mh::caps::BUILDINGS, player, (uint32_t)idx);
    return b->building_id < 100 ? b : nullptr;
}

// The aim tile of building (tp, ti) for attacker `a` of player `ap` (sim_attack_range_footprint.h). false = no record.
bool aim_fine(const mh_map_object_unit &a, uint32_t ap, uint32_t tp, int32_t ti, int32_t *ox, int32_t *oy) {
    const auto *b = bldg_rec(tp, ti);
    if (!b) return false;
    const auto &lv = mh::state::live();
    const auto *cb = reinterpret_cast<const mh_cfg_final_struct_Building *>(lv.base[mh::state::RID_BUILDING]);
    const auto *cw = reinterpret_cast<const mh_cfg_final_struct_Weapon *>(lv.base[mh::state::RID_WEAPON]);
    const auto *g  = reinterpret_cast<const mh_llm_strat_map_geom *>(lv.base[mh::state::RID_GENERAL]);
    if (!cb || !g) return false;
    mh::sim::footprint_aim_fine_of(a, ap, *b, cb, cw, *g, ox, oy);
    return true;
}

mh_map_object_unit *cur_unit() { return *reinterpret_cast<mh_map_object_unit *const *>(ADDR_CUR_UNIT); }
uint32_t            cur_player() { return *reinterpret_cast<const volatile uint16_t *>(ADDR_CUR_PLAYER); }

// ---- the C halves of the attack_range_footprint stubs. Each returns 1 = handled (coords written), 0 = run retail. ----

// ISSUE: after the original bldg_get_coords ran (its centre is in the out slots), overwrite with the attacker's aim tile.
void __cdecl arf_issue_fix(const uint8_t *ebp) {
    const uint32_t ap = *reinterpret_cast<const uint16_t *>(ebp - 0xc);
    const int32_t  ai = *reinterpret_cast<const int32_t *>(ebp - 0x20);
    const uint32_t tp = *reinterpret_cast<const uint16_t *>(ebp - 0x10);
    const int32_t  ti = *reinterpret_cast<const int32_t *>(ebp - 0x18);
    const auto    *a  = unit_rec(ap, ai);
    int32_t        x, y;
    if (!a || !aim_fine(*a, ap, tp, ti, &x, &y)) return;
    *reinterpret_cast<int32_t *>(const_cast<uint8_t *>(ebp) - 0x1c) = x;
    *reinterpret_cast<int32_t *>(const_cast<uint8_t *>(ebp) - 0x14) = y;
}

// JITTER: put the point back to the footprint centre before the original jitters it (retail aim spread).
void __cdecl arf_centre(uint32_t tp, int32_t ti, int32_t *px, int32_t *py) {
    if (!bldg_rec(tp & 0xffff, ti)) return;
    mh::call::llm_strat_bldg_get_coords(static_cast<uint16_t>(tp), ti, px, py);
}

// WALK: the current unit's REAL target (retail passes CUR_PLAYER/CUR_INDEX).
int __cdecl arf_walk_fix(int32_t *px, int32_t *py) {
    mh_map_object_unit *u = cur_unit();
    if (!u || u->target_ref == 0) return 0;
    return aim_fine(*u, cur_player(), static_cast<uint32_t>(u->target_ref) & 0xfu, static_cast<uint16_t>(u->target_index), px, py) ? 1 : 0;
}

// FIRE: frame of llm_strat_unit_fire_weapon -- player word [EBP-0x1c], unit [EBP-0x68], target_ref [EBP-0x20],
// target_index word [EBP+8]; the out slots are EBX/ECX ([EBP-0x2c] / [EBP-0x44]).
int __cdecl arf_fire_fix(const uint8_t *ebp, int32_t *px, int32_t *py) {
    const uint32_t ap = *reinterpret_cast<const uint16_t *>(ebp - 0x1c);
    const int32_t  ai = *reinterpret_cast<const int32_t *>(ebp - 0x68);
    const uint32_t tr = *reinterpret_cast<const uint32_t *>(ebp - 0x20);
    const int32_t  ti = *reinterpret_cast<const uint16_t *>(ebp + 8);
    const auto    *a  = unit_rec(ap, ai);
    return (a && aim_fine(*a, ap, tr & 0xfu, ti, px, py)) ? 1 : 0;
}

// REAIM: write the aim tile into the current unit's target_fine_x AND target_fine_y.
int __cdecl arf_reaim_fix() {
    mh_map_object_unit *u = cur_unit();
    if (!u) return 0;
    int32_t x, y;
    if (!aim_fine(*u, cur_player(), static_cast<uint32_t>(u->target_ref) & 0xfu, static_cast<uint16_t>(u->target_index), &x, &y)) return 0;
    u->target_fine_x = x;
    u->target_fine_y = y;
    return 1;
}

// STEP: (EAX tp, EDX ti, EBX &target_fine_x, ECX &target_fine_y) of the current unit.
int __cdecl arf_step_fix(uint32_t tp, int32_t ti, int32_t *px, int32_t *py) {
    mh_map_object_unit *u = cur_unit();
    return (u && aim_fine(*u, cur_player(), tp & 0xffff, ti, px, py)) ? 1 : 0;
}

// ---- the C half of the squad-merge stub ----
// Frame of llm_strat_unit_state_squad_merge: [EBP-0x38] = the target's unit index (IMUL EDX,[EBP-0x38],0xe9 at
// 0x0047ebd8). The mover is the current unit.
constexpr uint16_t ST_IDLE = 1, ST_ATTACK_UNIT_RETURN = 0x1a, ST_ATTACK_UNIT = 0x1b, ST_ATTACK_BUILDING = 0x1c;
uint32_t           g_sqm_logs = 0;

void __cdecl sqm_accept(const uint8_t *ebp) {
    const uint32_t      p  = cur_player();
    const int32_t       ti = *reinterpret_cast<const int32_t *>(ebp - 0x38);
    mh_map_object_unit *t  = unit_rec(p, ti);
    mh_map_object_unit *u  = cur_unit();
    if (!t || !u || t == u) return;
    if (t->state == ST_IDLE) return;
    if (t->state != ST_ATTACK_UNIT_RETURN && t->state != ST_ATTACK_UNIT && t->state != ST_ATTACK_BUILDING) return;
    // From here on the target is firing: say why a merge is refused (bounded), so a playtest log shows it.
    auto refuse = [&](const char *why) {
        if (g_sqm_logs >= 100) return;
        ++g_sqm_logs;
        char b[200];
        wsprintfA(b, "; squad merge (mods:M1C) player %u unit %d (state 0x%x) keeps attacking: %s\n", p, ti, (unsigned)t->state, why);
        seam_log(b);
    };
    if (t->move_microstep < 0x1f) return refuse("mid-step"); // its tile changes under it
    const auto *cfg = reinterpret_cast<const mh_cfg_final_struct_Unit *>(mh::state::live().base[mh::state::RID_UNIT]);
    if (!cfg || u->unit_proto_id >= 512 || t->unit_proto_id >= 512) return;
    // 3c/3d/3e, as the original evaluates them (sim_unit_state_squad_merge.cpp steps 1, 3c, 3d, 3e).
    const int32_t self_proto = u->unit_proto_id;
    const int32_t cfgA       = cfg[self_proto].soldier_count;
    const int32_t base       = self_proto - cfgA + 1;
    if (base < 0 || base >= 512) return;
    const uint32_t cfgB         = cfg[base].soldier_type;
    int32_t        family_count = 0, idx = base, expected = 1;
    for (;;) {
        ++family_count;
        ++expected;
        ++idx;
        if (idx >= 512 || cfg[idx].soldier_count != expected || cfg[idx].soldier_type != cfgB) break;
    }
    const auto &tc = cfg[t->unit_proto_id];
    if (tc.soldier_count <= 0 || tc.soldier_count + cfgA > family_count) return refuse("the merged squad would be too large");
    if (tc.soldier_type != cfg[self_proto].soldier_type) return refuse("a different soldier type");
    const int32_t dx  = mh::call::llm_map_wrap_delta_x(u->x, u->y, u->goal_x);
    const int32_t dy  = mh::call::llm_map_wrap_delta_y(u->x, u->y, u->goal_x, u->goal_y);
    const int32_t adj = (dx <= dy) ? dy : dx;
    if (adj > 1) return refuse("not adjacent");
    // Accept: release the target the way attack_building releases a dead one, then idle it.
    if (t->target_ref != 0) mh::call::llm_strat_target_release_ref(p, ti, 1);
    t->target_ref   = 0;
    t->target_index = 0;
    t->state        = ST_IDLE;
    t->order        = ST_IDLE;
    if (g_sqm_logs < 100) {
        ++g_sqm_logs;
        char b[160];
        wsprintfA(b, "; squad merge (mods:M1C) player %u unit %d stopped attacking to take a merge\n", p, ti);
        seam_log(b);
    }
}

int32_t g_sqm_disp = 0; // the displacement of the replaced CMP (the unit-state field of the live roster)

// clang-format off
// 0x0047ebe1 `CMP word ptr [EAX+disp32],1` (8 bytes) becomes CALL + 3 NOP; the stub ends with the same compare, so the
// JZ at 0x0047ebe9 reads its flags. EAX (the target's roster offset) and every other register are kept.
__declspec(naked) void sqm_stub() {
    __asm {
        cmp   g_sqm_rt, 0
        je    s_cmp
        inc   dword ptr [g_mv_hit + 4 * MVH_SQM]
        pushad
        push  ebp
        call  sqm_accept
        add   esp, 4
        popad
    s_cmp:
        push  ecx
        mov   ecx, g_sqm_disp
        cmp   word ptr [eax + ecx], 1
        pop   ecx
        ret
    }
}

// ISSUE: the original call first (args untouched), then the overwrite.
__declspec(naked) void arf_issue_stub() {
    __asm {
        cmp   g_arf_rt, 0
        je    i_retail
        inc   dword ptr [g_mv_hit + 4 * MVH_ARF_ISSUE]
        call  dword ptr [g_fn_bgc]
        pushad
        push  ebp
        call  arf_issue_fix
        add   esp, 4
        popad
        ret
    i_retail:
        jmp   dword ptr [g_fn_bgc]
    }
}

// JITTER: [ESP+4] = &x, [ESP+8] = &y (the two stack args), EBX = target player, ECX = target index. The original
// (RET 8) is tail-jumped, so it returns straight to the call site.
__declspec(naked) void arf_jit_point_stub() {
    __asm {
        cmp   g_arf_rt, 0
        je    jp_go
        inc   dword ptr [g_mv_hit + 4 * MVH_ARF_JIT_POINT]
        pushad
        push  dword ptr [esp + 40]
        push  dword ptr [esp + 40]
        push  ecx
        push  ebx
        call  arf_centre
        add   esp, 16
        popad
    jp_go:
        jmp   dword ptr [g_fn_rp]
    }
}
__declspec(naked) void arf_jit_offset_stub() {
    __asm {
        cmp   g_arf_rt, 0
        je    jo_go
        inc   dword ptr [g_mv_hit + 4 * MVH_ARF_JIT_OFFSET]
        pushad
        push  dword ptr [esp + 40]
        push  dword ptr [esp + 40]
        push  ecx
        push  ebx
        call  arf_centre
        add   esp, 16
        popad
    jo_go:
        jmp   dword ptr [g_fn_ro]
    }
}

// WALK / FIRE / REAIM / STEP: the C half writes the out slots and answers 1, or answers 0 and the original runs.
__declspec(naked) void arf_walk_stub() {
    __asm {
        cmp   g_arf_rt, 0
        je    w_retail
        inc   dword ptr [g_mv_hit + 4 * MVH_ARF_WALK]
        pushad
        push  ecx
        push  ebx
        call  arf_walk_fix
        add   esp, 8
        test  eax, eax
        popad
        jz    w_retail
        ret
    w_retail:
        jmp   dword ptr [g_fn_bgc]
    }
}
__declspec(naked) void arf_fire_stub() {
    __asm {
        cmp   g_arf_rt, 0
        je    f_retail
        inc   dword ptr [g_mv_hit + 4 * MVH_ARF_FIRE]
        pushad
        push  ecx
        push  ebx
        push  ebp
        call  arf_fire_fix
        add   esp, 12
        test  eax, eax
        popad
        jz    f_retail
        ret
    f_retail:
        jmp   dword ptr [g_fn_bgc]
    }
}
__declspec(naked) void arf_reaim_stub() {
    __asm {
        cmp   g_arf_rt, 0
        je    r_retail
        inc   dword ptr [g_mv_hit + 4 * MVH_ARF_REAIM]
        pushad
        call  arf_reaim_fix
        test  eax, eax
        popad
        jz    r_retail
        ret
    r_retail:
        jmp   dword ptr [g_fn_bgc]
    }
}
__declspec(naked) void arf_step_stub() {
    __asm {
        cmp   g_arf_rt, 0
        je    st_retail
        inc   dword ptr [g_mv_hit + 4 * MVH_ARF_STEP]
        pushad
        push  ecx
        push  ebx
        push  edx
        push  eax
        call  arf_step_fix
        add   esp, 16
        test  eax, eax
        popad
        jz    st_retail
        ret
    st_retail:
        jmp   dword ptr [g_fn_bgc]
    }
}
// clang-format on

// The sites. Each is a 5-byte `CALL rel32` to `callee`, retargeted to `stub`.
constexpr uintptr_t ADDR_ARF_ISSUE_REPOS = 0x0046bfca, ADDR_ARF_ISSUE_ALT = 0x0046c24a, ADDR_ARF_ISSUE_ENQ = 0x0046c49f,
                    ADDR_ARF_ISSUE_ATK = 0x0046c6f4;
constexpr uintptr_t ADDR_ARF_JIT_REPOS = 0x0046c19e, ADDR_ARF_JIT_ENQ = 0x0046c648, ADDR_ARF_JIT_ATK = 0x0046c731,
                    ADDR_ARF_JIT_ALT = 0x0046c3f3, ADDR_ARF_JIT_WALK = 0x0047d114;
constexpr uintptr_t ADDR_ARF_WALK = 0x0047d016, ADDR_ARF_FIRE = 0x0048bbce, ADDR_ARF_REAIM = 0x00484fc6, ADDR_ARF_STEP = 0x004831c0;
constexpr uintptr_t ADDR_SQM_STATE_CMP = 0x0047ebe1;

int g_arf_state = 0, g_sqm_state = 0; // 1 carried (incl. DISPLACED sites whose twin carries), -1 a site MISMATCHED

void rel_call(uint8_t *out, uintptr_t site, uintptr_t target) {
    const int32_t rel = static_cast<int32_t>(target - (site + 5));
    out[0]            = 0xE8;
    memcpy(out + 1, &rel, 4);
}

// 1 = patched, 2 = DISPLACED (libmh owns the function; its twin carries the rule), -1 = MISMATCHED.
int arf_site(uintptr_t site, uintptr_t callee, void (*stub)(), const char *owner) {
    if (mh::hook::promoted_owner_of(site)) return 2;
    uint8_t expect[5], repl[5];
    rel_call(expect, site, callee);
    rel_call(repl, site, reinterpret_cast<uintptr_t>(stub));
    bool ok = false;
    // one literal expression per site: lint_dll_patches keys the manifest on (file, expr)
    switch (site) {
        case ADDR_ARF_ISSUE_REPOS: ok = mh::hook::patch_bytes_guarded(ADDR_ARF_ISSUE_REPOS, expect, repl, 5); break;
        case ADDR_ARF_ISSUE_ALT: ok = mh::hook::patch_bytes_guarded(ADDR_ARF_ISSUE_ALT, expect, repl, 5); break;
        case ADDR_ARF_ISSUE_ENQ: ok = mh::hook::patch_bytes_guarded(ADDR_ARF_ISSUE_ENQ, expect, repl, 5); break;
        case ADDR_ARF_ISSUE_ATK: ok = mh::hook::patch_bytes_guarded(ADDR_ARF_ISSUE_ATK, expect, repl, 5); break;
        case ADDR_ARF_JIT_REPOS: ok = mh::hook::patch_bytes_guarded(ADDR_ARF_JIT_REPOS, expect, repl, 5); break;
        case ADDR_ARF_JIT_ENQ: ok = mh::hook::patch_bytes_guarded(ADDR_ARF_JIT_ENQ, expect, repl, 5); break;
        case ADDR_ARF_JIT_ATK: ok = mh::hook::patch_bytes_guarded(ADDR_ARF_JIT_ATK, expect, repl, 5); break;
        case ADDR_ARF_JIT_ALT: ok = mh::hook::patch_bytes_guarded(ADDR_ARF_JIT_ALT, expect, repl, 5); break;
        case ADDR_ARF_JIT_WALK: ok = mh::hook::patch_bytes_guarded(ADDR_ARF_JIT_WALK, expect, repl, 5); break;
        case ADDR_ARF_WALK: ok = mh::hook::patch_bytes_guarded(ADDR_ARF_WALK, expect, repl, 5); break;
        case ADDR_ARF_FIRE: ok = mh::hook::patch_bytes_guarded(ADDR_ARF_FIRE, expect, repl, 5); break;
        case ADDR_ARF_REAIM: ok = mh::hook::patch_bytes_guarded(ADDR_ARF_REAIM, expect, repl, 5); break;
        case ADDR_ARF_STEP: ok = mh::hook::patch_bytes_guarded(ADDR_ARF_STEP, expect, repl, 5); break;
    }
    if (!ok) {
        char b[200];
        wsprintfA(b, "; combat prototype (mods:M1C) attack_range_footprint: splice at %08X in %s MISMATCHED, left untouched\n", (unsigned)site, owner);
        seam_log(b);
        return -1;
    }
    return 1;
}

} // namespace

// ---- [net] projectile_facing_fix (mods:M1C; the rule: libmh/sim/sim_projectile_facing.h) ---------------------------------
// llm_strat_projectile_tick 0x00441424: `SUB EDX,[EAX+0x59]` (EDX = new facing - facing) `MOV EAX,[0x00e1538c]`
// (_G_LLM_STRAT_CUR_PROJECTILE) -- 8 bytes -> CALL pff_stub + 3 NOP. The stub redoes both instructions; with the key on it
// replaces EDX by the rule's delta (0 for weapon types 5/6, otherwise unchanged) before the IMUL by the stride at 0x0044143e.
namespace {
constexpr uintptr_t ADDR_PFF_DELTA      = 0x00441424;
const uint8_t       PFF_DELTA_EXPECT[8] = {0x2B, 0x50, 0x59, 0xA1, 0x8C, 0x53, 0xE1, 0x00};
constexpr uintptr_t PFF_WEAPON_TABLE    = 0x00c3a520; // cfg Weapon[], stride 0x16c, +0 type
int                 g_pff_state         = 0;          // 1 carried, 2 DISPLACED (libmh twin), -1 MISMATCHED

int __cdecl pff_delta(int32_t delta, const uint8_t *proj) {
    const int32_t wid = *reinterpret_cast<const int32_t *>(proj + 4);
    if (wid < 0 || wid >= 32) return delta;
    const uint8_t type = *reinterpret_cast<const uint8_t *>(PFF_WEAPON_TABLE + static_cast<uintptr_t>(wid) * 0x16c);
    return g_pff_rc3 ? mh::sim::projectile_facing_delta_rc3(delta, type) : mh::sim::projectile_facing_delta(delta, type);
}

// clang-format off
__declspec(naked) void pff_stub() {
    __asm {
        sub   edx, dword ptr [eax + 0x59]        // replaced: delta = new facing - facing
        cmp   g_pff_rt, 0
        je    pff_done
        inc   dword ptr [g_mv_hit + 4 * MVH_PFF]
        push  ecx
        push  eax                                // the projectile
        push  edx                                // the delta
        call  pff_delta
        add   esp, 8
        mov   edx, eax
        pop   ecx
    pff_done:
        mov   eax, dword ptr ds:[0x00e1538c]     // replaced: EAX = _G_LLM_STRAT_CUR_PROJECTILE
        ret
    }
}
// clang-format on

void install_projectile_facing() {
    if (mh::hook::promoted_owner_of(ADDR_PFF_DELTA)) {
        g_pff_state = 2;
        return;
    }
    uint8_t       repl[8];
    const int32_t rel = (int32_t)((uintptr_t)pff_stub - (ADDR_PFF_DELTA + 5));
    repl[0]           = 0xE8;
    memcpy(repl + 1, &rel, sizeof(rel));
    repl[5] = repl[6] = repl[7] = 0x90;
    g_pff_state                 = mh::hook::patch_bytes_guarded(ADDR_PFF_DELTA, PFF_DELTA_EXPECT, repl, 8) ? 1 : -1;
}
} // namespace

void install_combat_proto() {
    // attack_range_footprint: all thirteen sites or none in force (a half-applied rule would let a check disagree with
    // the plan). A site already patched stays patched; with the key off every stub runs the retail callee.
    struct site {
        uintptr_t at, callee;
        void (*stub)();
        const char            *owner;
        mh::seams::mv::site_id id; // mods:M1H: the carrier report's name for the site
    };
    const site sites[] = {
        {ADDR_ARF_ISSUE_REPOS, FN_BLDG_GET_COORDS, arf_issue_stub, "llm_strat_unit_order_attack_building_reposition", mh::seams::mv::SITE_ARF_ISSUE_REPOS},
        {ADDR_ARF_ISSUE_ALT, FN_BLDG_GET_COORDS, arf_issue_stub, "llm_strat_unit_order_attack_building_reposition_alt_enqueue", mh::seams::mv::SITE_ARF_ISSUE_ALT},
        {ADDR_ARF_ISSUE_ENQ, FN_BLDG_GET_COORDS, arf_issue_stub, "llm_strat_unit_order_attack_building_reposition_enqueue", mh::seams::mv::SITE_ARF_ISSUE_ENQ},
        {ADDR_ARF_ISSUE_ATK, FN_BLDG_GET_COORDS, arf_issue_stub, "llm_strat_unit_order_attack_building", mh::seams::mv::SITE_ARF_ISSUE_ATK},
        {ADDR_ARF_JIT_REPOS, FN_RANDOM_POINT, arf_jit_point_stub, "llm_strat_unit_order_attack_building_reposition", mh::seams::mv::SITE_ARF_JIT_REPOS},
        {ADDR_ARF_JIT_ENQ, FN_RANDOM_POINT, arf_jit_point_stub, "llm_strat_unit_order_attack_building_reposition_enqueue", mh::seams::mv::SITE_ARF_JIT_ENQ},
        {ADDR_ARF_JIT_ATK, FN_RANDOM_POINT, arf_jit_point_stub, "llm_strat_unit_order_attack_building", mh::seams::mv::SITE_ARF_JIT_ATK},
        {ADDR_ARF_JIT_ALT, FN_RANDOM_OFFSET, arf_jit_offset_stub, "llm_strat_unit_order_attack_building_reposition_alt_enqueue", mh::seams::mv::SITE_ARF_JIT_ALT},
        {ADDR_ARF_JIT_WALK, FN_RANDOM_OFFSET, arf_jit_offset_stub, "llm_strat_unit_state_move_walker", mh::seams::mv::SITE_ARF_JIT_WALK},
        {ADDR_ARF_WALK, FN_BLDG_GET_COORDS, arf_walk_stub, "llm_strat_unit_state_move_walker", mh::seams::mv::SITE_ARF_WALK},
        {ADDR_ARF_FIRE, FN_BLDG_GET_COORDS, arf_fire_stub, "llm_strat_unit_fire_weapon", mh::seams::mv::SITE_ARF_FIRE},
        {ADDR_ARF_REAIM, FN_BLDG_GET_COORDS, arf_reaim_stub, "llm_strat_unit_state_attack_building", mh::seams::mv::SITE_ARF_REAIM},
        {ADDR_ARF_STEP, FN_BLDG_GET_COORDS, arf_step_stub, "llm_strat_unit_group_step_ground", mh::seams::mv::SITE_ARF_STEP},
    };
    int carried = 0, displaced = 0, bad = 0;
    for (const site &s : sites) {
        const int r = arf_site(s.at, s.callee, s.stub, s.owner);
        mh::seams::mv::note_site(s.id, r == 1 ? 1 : (r == 2 ? -2 : -1));
        if (r == 1) ++carried;
        else if (r == 2) ++displaced;
        else ++bad;
    }
    g_arf_state = bad ? -1 : 1;

    // move_squad_merge_fix: the CMP's displacement is read from the image (a roster relocation rewrites it), the
    // opcode bytes around it are the guard.
    const uint8_t *p = reinterpret_cast<const uint8_t *>(ADDR_SQM_STATE_CMP);
    if (mh::hook::promoted_owner_of(ADDR_SQM_STATE_CMP)) {
        g_sqm_state = 2;
    } else if (p[0] == 0x66 && p[1] == 0x83 && p[2] == 0xB8 && p[7] == 0x01) {
        uint8_t expect[8], repl[8];
        memcpy(expect, p, 8);
        memcpy(&g_sqm_disp, p + 3, 4);
        rel_call(repl, ADDR_SQM_STATE_CMP, reinterpret_cast<uintptr_t>(sqm_stub));
        repl[5] = repl[6] = repl[7] = 0x90;
        g_sqm_state                 = mh::hook::patch_bytes_guarded_caps(ADDR_SQM_STATE_CMP, expect, repl, 8) ? 1 : -1;
    } else {
        g_sqm_state = -1;
    }
    mh::seams::mv::note_site(mh::seams::mv::SITE_SQM, g_sqm_state == 1 ? 1 : (g_sqm_state == 2 ? -2 : -1));
    char b[300];
    wsprintfA(b, "; combat prototype splices (mods:M1C): attack_range_footprint %d carried / %d displaced (libmh twin) / %d MISMATCHED; "
                 "squad merge %s\n",
              carried, displaced, bad, g_sqm_state == 1 ? "carried" : (g_sqm_state == 2 ? "DISPLACED (libmh twin)" : "MISMATCHED"));
    seam_log(b);
    install_projectile_facing();
    mh::seams::mv::note_site(mh::seams::mv::SITE_PFF, g_pff_state == 1 ? 1 : (g_pff_state == 2 ? -2 : -1));
    wsprintfA(b, "; combat prototype splice (mods:M1C): projectile facing %s\n",
              g_pff_state == 1 ? "carried" : (g_pff_state == 2 ? "DISPLACED (libmh twin)" : "MISMATCHED"));
    seam_log(b);
}

// The setters set the stubs' gates and tell the carrier report (move_carrier.cpp) the requested value; the report -- late, once the
// unit-state table is filled -- is the one line per key ("retail splice" / "libmh twin" / "NOT ACTIVE: ...").
int mh_set_attack_range_footprint(int on) {
    on       = on != 0;
    g_arf_rt = (on && g_arf_state == 1) ? 1 : 0;
    mh::seams::mv::request(mh::seams::mv::KEY_ATTACK_RANGE_FOOTPRINT, on, 0);
    return (on == 0 || g_arf_rt) ? 1 : 0;
}

int mh_set_move_squad_merge_fix(int on) {
    on       = on != 0;
    g_sqm_rt = (on && g_sqm_state == 1) ? 1 : 0;
    mh::seams::mv::request(mh::seams::mv::KEY_SQUAD_MERGE, on, 0);
    return (on == 0 || g_sqm_rt || g_sqm_state == 2) ? 1 : 0;
}

int mh_set_projectile_facing_fix(int on) {
    g_pff_rc3 = on == 2; // test-only value, see pff_delta
    on        = on != 0;
    g_pff_rt  = (on && g_pff_state == 1) ? 1 : 0;
    mh::seams::mv::request(mh::seams::mv::KEY_PROJECTILE_FACING, on, 0);
    return (on == 0 || g_pff_rt || g_pff_state == 2) ? 1 : 0;
}
