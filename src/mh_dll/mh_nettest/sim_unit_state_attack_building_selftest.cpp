//
// sim_unit_state_attack_building_selftest.cpp -- `simtest` oracle for
// llm_strat_unit_state_attack_building @0x00484e10 (sim/sim_unit_state_attack_building.h/.cpp,
// RI-SIM / SIM1-G1).
//
// SCOPE: the unconditional tick_budget drain (0x00484e28-0x00484e36, every run incl. every early
// return); the facing gate (0x00484e3c-0x00484e85, both sides) with the exact unit_get_coords args
// (0x00484e42-0x00484e50) and the exact, NON-SWAPPED dir_from_to operand order (self_fine_x,
// self_fine_y, target_fine_x, target_fine_y, 0x00484e6b-0x00484e76); the target-destroyed gate
// (0x00484e8b-0x00484ec4, FLDZ/FCOMP/FNSTSW/SAHF/JC) tested AT THE EXACT BOUNDARY (energy==0.0 must
// take the destroyed path -- JC is taken only when 0.0<energy, so a `< 0.0` mistranslation would
// mis-route this exact value) plus strictly negative and strictly positive; the destroyed arm's
// release_ref args, target_ref/target_index clearing, unit_set_state_order(STOP_TO_DEFAULT x2), the
// ABSENCE of any notify_status call on this arm (0x00484ec6-0x00484f09), and the immediate return;
// the already-selected-weapon arm (0x00484f13-0x00484f64, both sides of the ==0x64 compare) with the
// exact 7-argument, non-swapped llm_strat_unit_fire_weapon call (register+stack args re-derived from
// the PUSH order at 0x00484f1e-0x00484f3b) and its immediate return; the weapon-not-found arm
// (0x00484f81-0x00484f88, both sides) and its immediate return leaving selected_weapon untouched; and
// the weapon-found "replan" arm (0x00484f8a-0x00484ffb) -- INCLUDING THE PRESERVED ORIGINAL BUG at
// 0x00484f8a-0x00484fc2: the two llm_strat_bldg_get_coords out-pointers (EBX and ECX) are BOTH
// `cur_unit+0x8e` (target_fine_x's own address), so target_fine_y's real memory is never passed to
// the callee at all and must read back EXACTLY as seeded (STALE), no matter what the callee itself
// does -- this is pinned directly by asserting the two recorded pointer arguments are bit-identical
// AND equal to `&u.target_fine_x`, not merely by an indirect value comparison. The arm's
// unit_set_state_order(GROUP_MARSHAL,ATTACK_BUILDING)/unit_notify_status(REPLAN)/selected_weapon
// commit and their exact call order (0x00484fcb-0x00484ffb) are pinned too. Non-corruption: a guard
// roster slot this function never addresses, and a guard building record, read back unchanged.
//
// EXPECTED BEHAVIOUR HAND-DERIVED FROM
// tmp/decomp_sim/llm_strat_unit_state_attack_building_00484e10.asm -- every assertion below cites the
// instruction address(es) it pins. NOT read off the .cpp body.
//
#include "sim/sim_unit_state_attack_building.h"

#include "sim/sim_order_enqueue.h"      // UNIT_STATE_STOP_TO_DEFAULT (shared there)
#include "sim/sim_unit_select_weapon.h" // UNIT_SELECT_WEAPON_NOT_FOUND (shared there)

#include <cstdint>
#include <cstring>
#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- shared trace: proves CALL ORDER across all 7 callees -----------------------------------------
std::vector<const char *> g_trace;
void                      tr(const char *tag) { g_trace.push_back(tag); }

bool trace_eq(const std::vector<const char *> &want) {
    if (g_trace.size() != want.size()) return false;
    for (size_t i = 0; i < want.size(); ++i)
        if (std::strcmp(g_trace[i], want[i]) != 0) return false;
    return true;
}

// ---- per-callee recorders (7, one per unit_state_attack_building_calls member) --------------------
struct GetCoordsCall {
    uint16_t player;
    int32_t  unit_index;
};
std::vector<GetCoordsCall> g_get_coords_calls;
int32_t                    g_uc_out_x = 0, g_uc_out_y = 0;
void                       rec_unit_get_coords(uint16_t player, int32_t unit_index, int32_t *out_fine_x, int32_t *out_fine_y) {
    tr("unit_get_coords");
    g_get_coords_calls.push_back({player, unit_index});
    *out_fine_x = g_uc_out_x;
    *out_fine_y = g_uc_out_y;
}

struct DirFromToCall {
    int32_t x1, y1, x2, y2;
};
std::vector<DirFromToCall> g_dir_calls;
int32_t                    g_dir_ret = 0;
int32_t                    rec_dir_from_to(int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    tr("dir_from_to");
    g_dir_calls.push_back({x1, y1, x2, y2});
    return g_dir_ret;
}

struct ReleaseCall {
    uint32_t player_idx;
    int32_t  unit_idx;
    uint32_t mode;
};
std::vector<ReleaseCall> g_release_calls;
void                     rec_target_release_ref(uint32_t player_idx, int32_t unit_idx, uint32_t mode) {
    tr("target_release_ref");
    g_release_calls.push_back({player_idx, unit_idx, mode});
}

struct SetStateOrderCall {
    uint16_t new_order, new_state;
};
std::vector<SetStateOrderCall> g_set_state_order_calls;
void                           rec_unit_set_state_order(uint16_t new_order, uint16_t new_state) {
    tr("unit_set_state_order");
    g_set_state_order_calls.push_back({new_order, new_state});
}

struct SelectWeaponCall {
    uint16_t player;
    int32_t  unit_index;
    uint32_t target_mask;
};
std::vector<SelectWeaponCall> g_select_calls;
uint8_t                       g_select_ret = 0;
uint8_t                       rec_unit_select_weapon(uint16_t player, int32_t unit_index, uint32_t target_mask) {
    tr("unit_select_weapon");
    g_select_calls.push_back({player, unit_index, target_mask});
    return g_select_ret;
}

struct BldgGetCoordsCall {
    uint16_t player;
    int32_t  building_index;
    void    *out_x_ptr;
    void    *out_y_ptr;
};
std::vector<BldgGetCoordsCall> g_bldg_calls;
void                           rec_bldg_get_coords(uint16_t player, int32_t building_index, int32_t *out_fine_x,
                                                   int32_t *out_fine_y) {
    tr("bldg_get_coords");
    g_bldg_calls.push_back({player, building_index, out_fine_x, out_fine_y});
    // The stub's OWN behaviour, not a claim about the real llm_strat_bldg_get_coords: with the two
    // pointers aliased (the preserved bug), whichever write happens second physically wins. We write
    // two DISTINCT, recognisable values through out_fine_x then out_fine_y so the test can observe
    // which one "won" at the aliased address -- the asm-derived fact this pins is independent of that
    // outcome: out_fine_y's real target (target_fine_y) is never passed to the callee AT ALL, so it
    // must read back exactly as seeded no matter which write order the real callee itself uses.
    *out_fine_x = 0x1111;
    *out_fine_y = 0x2222;
}

struct NotifyCall {
    uint32_t player;
    int32_t  unit_index;
    uint32_t status_code;
};
std::vector<NotifyCall> g_notify_calls;
void                    rec_unit_notify_status(uint32_t player, int32_t unit_index, uint32_t status_code) {
    tr("unit_notify_status");
    g_notify_calls.push_back({player, unit_index, status_code});
}

struct FireWeaponCall {
    uint32_t player, unit_index;
    uint8_t  weapon_slot_select;
    uint32_t target_ref;
    uint16_t target_index;
    int32_t  target_fine_x, target_fine_y;
};
std::vector<FireWeaponCall> g_fire_calls;
void                        rec_unit_fire_weapon(uint32_t player, uint32_t unit_index, uint8_t weapon_slot_select, uint32_t target_ref,
                                                 uint16_t target_index, int32_t target_fine_x, int32_t target_fine_y) {
    tr("unit_fire_weapon");
    g_fire_calls.push_back(
        {player, unit_index, weapon_slot_select, target_ref, target_index, target_fine_x, target_fine_y});
}

const unit_state_attack_building_calls g_calls = {
    &rec_unit_get_coords,
    &rec_dir_from_to,
    &rec_target_release_ref,
    &rec_unit_set_state_order,
    &rec_unit_select_weapon,
    &rec_bldg_get_coords,
    &rec_unit_notify_status,
    &rec_unit_fire_weapon,
};

void reset_observations() {
    g_trace.clear();
    g_get_coords_calls.clear();
    g_dir_calls.clear();
    g_release_calls.clear();
    g_set_state_order_calls.clear();
    g_select_calls.clear();
    g_bldg_calls.clear();
    g_notify_calls.clear();
    g_fire_calls.clear();
}

// Fixed "guard" slot no test's own (player,index) ever touches -- seeded with sentinel nonzero data
// each run so a wrong-index write lands somewhere observable.
constexpr uint16_t GUARD_PLAYER = 4;
constexpr int32_t  GUARD_INDEX  = 6;
// A guard building record, distinct from the target owner/slot every case uses below, so a
// wrong-index building READ (there should be none -- this function only ever reads its OWN computed
// target_owner/target_slot's energy) is also observable in principle.
constexpr int32_t GUARD_BLDG_OWNER = 1;
constexpr int32_t GUARD_BLDG_SLOT  = 50;

void seed_guard_slot(sim_fixture &fx) {
    unit &g           = fx.u(GUARD_PLAYER, GUARD_INDEX);
    g.facing_current  = 0x11;
    g.target_ref      = 0x77;
    g.target_index    = 88;
    g.target_fine_x   = 9001;
    g.target_fine_y   = 9002;
    g.selected_weapon = 3;

    fx.b(GUARD_BLDG_OWNER, GUARD_BLDG_SLOT).energy = 555.0;
}

// ---- fixture seeding -----------------------------------------------------------------------------
struct Seed {
    uint16_t player = 2;
    int32_t  index  = 5;

    uint8_t facing_current = 3;

    // llm_strat_unit_get_coords's out-params -- the stub's return, standing in for "this unit's own
    // fine position" (self_fine_x/y). Distinct from each other and from the target coords below so a
    // swapped dir_from_to operand order is observable (0x00484e6b-0x00484e76).
    int32_t self_fine_x = 111;
    int32_t self_fine_y = 222;

    int32_t dir_from_to_ret = 3; // facing_needed the stub reports

    // target_ref's low nibble is target_owner (0x00484e8b-0x00484e9c); 0x35's upper bits (0x30) are
    // deliberately nonzero so a translation that forgot the &0xf mask would disagree with the fixture
    // (it would compute owner=0x35=53, wildly out of the MAX_PLAYERS=8 range this fixture allocates).
    int16_t target_ref   = 0x35; // owner = 0x35 & 0xf = 5
    int16_t target_index = 12;   // target_slot -- also the building_index arg later
    // Distinct from self_fine_x/y and from each other so no accidental symmetry hides a swap.
    int32_t target_fine_x = 333;
    int32_t target_fine_y = 444;

    double building_energy = 5.0; // >0.0 -- "still standing" by default

    uint8_t selected_weapon   = UNIT_SELECT_WEAPON_NOT_FOUND; // "none selected" by default
    uint8_t select_weapon_ret = UNIT_SELECT_WEAPON_NOT_FOUND; // the stub's return for llm_strat_unit_select_weapon

    double tick_budget = 55.5; // nonzero seed -- must read back 0.0 after every run (unconditional)
};

void seed_and_run(sim_fixture &fx, const Seed &s) {
    fx.reset();
    seed_guard_slot(fx);

    unit &u           = fx.u(s.player, s.index);
    u.facing_current  = s.facing_current;
    u.target_ref      = s.target_ref;
    u.target_index    = s.target_index;
    u.target_fine_x   = s.target_fine_x;
    u.target_fine_y   = s.target_fine_y;
    u.selected_weapon = s.selected_weapon;

    fx.cur_unit_ptr    = &u;
    fx.view_cur_player = s.player;
    fx.view_cur_index  = (uint16_t)s.index;
    fx.tick_budget     = s.tick_budget;

    const int32_t owner                = (int32_t)((uint32_t)(uint16_t)s.target_ref & 0xfu);
    fx.b(owner, s.target_index).energy = s.building_energy;

    reset_observations();
    g_uc_out_x   = s.self_fine_x;
    g_uc_out_y   = s.self_fine_y;
    g_dir_ret    = s.dir_from_to_ret;
    g_select_ret = s.select_weapon_ret;

    sim_store own = fx.store();
    detail::unit_state_attack_building(fx.view(), own, g_calls);
}

} // namespace

void run_unit_state_attack_building_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- the facing gate (0x00484e3c-0x00484e85), both sides, plus the exact unit_get_coords args
    // (0x00484e42-0x00484e50) and the exact, non-swapped dir_from_to operand order (self_fine_x,
    // self_fine_y, target_fine_x, target_fine_y -- 0x00484e6b-0x00484e76: EAX=[EBP-0x1c]=self_fine_x,
    // EDX=[EBP-0x20]=self_fine_y, EBX=cur_unit->target_fine_x, ECX=cur_unit->target_fine_y).
    // =================================================================================================
    {
        Seed s;
        s.facing_current  = 3;
        s.dir_from_to_ret = 7; // != facing_current -- mismatch
        seed_and_run(fx, s);
        ck(g_get_coords_calls.size() == 1 && g_get_coords_calls[0].player == s.player &&
               g_get_coords_calls[0].unit_index == s.index,
           "T1a: llm_strat_unit_get_coords(cur_player, cur_index) (0x00484e42-0x00484e50)");
        ck(g_dir_calls.size() == 1 && g_dir_calls[0].x1 == s.self_fine_x && g_dir_calls[0].y1 == s.self_fine_y &&
               g_dir_calls[0].x2 == s.target_fine_x && g_dir_calls[0].y2 == s.target_fine_y,
           "T1a: dir_from_to(self_fine_x, self_fine_y, target_fine_x, target_fine_y), NOT swapped "
           "(0x00484e6b-0x00484e76)");
        ck(trace_eq({"unit_get_coords", "dir_from_to"}),
           "T1a: facing_current(3) != facing_needed(7) -- early return, ONLY these two calls "
           "(0x00484e85 JNZ taken)");
        ck(g_release_calls.empty() && g_set_state_order_calls.empty() && g_select_calls.empty() &&
               g_bldg_calls.empty() && g_notify_calls.empty() && g_fire_calls.empty(),
           "T1a: no other callee fires on the mismatch path");
        ck_eq_d(fx.tick_budget, 0.0, "T1a: tick_budget == 0.0 even on the early-return path (0x00484e28/0x00484e32)");

        s.dir_from_to_ret = 3;   // == facing_current -- match: proves the gate does NOT stop here
        s.building_energy = 0.0; // destroyed -- so a further call (release_ref) is observable proof of fallthrough
        seed_and_run(fx, s);
        ck(!g_release_calls.empty(),
           "T1b: facing_current(3) == facing_needed(3) -- falls through past the gate (0x00484e85 JNZ not "
           "taken); release_ref firing (detailed in T2) is the proof");
    }

    // =================================================================================================
    // T2 -- the target-destroyed gate (0x00484e8b-0x00484ec4, FLDZ/FCOMP/FNSTSW/SAHF/JC 0x00484ec4).
    // JC is taken (still-standing path) only when 0.0 < energy, so energy==0.0 EXACTLY must take the
    // DESTROYED path -- a `< 0.0` mistranslation would mis-route this exact boundary value. T2a pins
    // that boundary with the full destroyed-arm assertion set; T2b repeats it at a strictly negative
    // energy; T2c is the mirror -- energy > 0.0 must NOT take the destroyed arm.
    // =================================================================================================
    {
        Seed s;
        s.facing_current  = 5;
        s.dir_from_to_ret = 5;   // match -- reach the energy gate
        s.building_energy = 0.0; // EXACT BOUNDARY
        seed_and_run(fx, s);
        ck(g_release_calls.size() == 1 && g_release_calls[0].player_idx == s.player &&
               g_release_calls[0].unit_idx == s.index && g_release_calls[0].mode == 1u,
           "T2a: energy==0.0 -- target_release_ref(cur_player, cur_index, mode=1) (0x00484ecb-0x00484ed9)");
        const unit &u = fx.u(s.player, s.index);
        ck_eq((uint32_t)u.target_ref, 0u, "T2a: target_ref cleared to 0 (0x00484ee3-0x00484eec)");
        ck_eq((uint32_t)u.target_index, 0u, "T2a: target_index cleared to 0 (0x00484ef1-0x00484efa)");
        ck(g_set_state_order_calls.size() == 1 &&
               g_set_state_order_calls[0].new_order == UNIT_STATE_STOP_TO_DEFAULT &&
               g_set_state_order_calls[0].new_state == UNIT_STATE_STOP_TO_DEFAULT,
           "T2a: unit_set_state_order(STOP_TO_DEFAULT=1, STOP_TO_DEFAULT=1) (0x00484efa-0x00484f04)");
        ck(g_notify_calls.empty(),
           "T2a: NO unit_notify_status call on the destroyed path (verified against the asm -- unlike "
           "move_walker's cascade, this one skips it)");
        ck(g_select_calls.empty() && g_bldg_calls.empty() && g_fire_calls.empty(),
           "T2a: immediate return after set_state_order -- no weapon-arm calls");
        ck(trace_eq({"unit_get_coords", "dir_from_to", "target_release_ref", "unit_set_state_order"}),
           "T2a: exact call order");

        s.building_energy = -3.5; // strictly negative -- same arm
        seed_and_run(fx, s);
        ck(g_release_calls.size() == 1, "T2b: energy(-3.5) < 0.0 -- destroyed arm fires too");
        ck_eq((uint32_t)fx.u(s.player, s.index).target_ref, 0u, "T2b: target_ref cleared");

        s.building_energy = 0.5; // strictly positive -- must NOT take the destroyed arm
        s.selected_weapon = 2;   // isolate: weapon-already-selected sub-arm fires next (asserted fully in T3)
        seed_and_run(fx, s);
        ck(g_release_calls.empty(), "T2c: energy(0.5) > 0.0 -- destroyed arm does NOT fire (0x00484ec4 JC taken)");
        ck_eq((uint32_t)fx.u(s.player, s.index).target_ref, (uint32_t)(uint16_t)s.target_ref,
              "T2c: target_ref UNCHANGED (still-standing path never clears it)");
    }

    // =================================================================================================
    // T3 -- weapon already selected (0x00484f13/0x00484f17 JZ on selected_weapon==0x64): the fire arm.
    // Exact 7-argument llm_strat_unit_fire_weapon call, register+stack order re-derived from the PUSH
    // sequence at 0x00484f1e-0x00484f3b (pushed target_fine_y, target_fine_x, target_index in that
    // order -- last-pushed is the first stack param, so the call reads player, unit_index,
    // weapon_slot_select, target_ref, target_index, target_fine_x, target_fine_y), then an immediate
    // return (0x00484f64 JMP).
    // =================================================================================================
    {
        Seed s;
        s.facing_current  = 6;
        s.dir_from_to_ret = 6;
        s.building_energy = 8.0; // still standing
        s.selected_weapon = 2;   // != 0x64 -- already selected
        seed_and_run(fx, s);
        ck(g_fire_calls.size() == 1 &&
               g_fire_calls[0].player == s.player && g_fire_calls[0].unit_index == (uint32_t)s.index &&
               g_fire_calls[0].weapon_slot_select == 2 &&
               g_fire_calls[0].target_ref == (uint32_t)(uint16_t)s.target_ref &&
               g_fire_calls[0].target_index == (uint16_t)s.target_index &&
               g_fire_calls[0].target_fine_x == s.target_fine_x && g_fire_calls[0].target_fine_y == s.target_fine_y,
           "T3: unit_fire_weapon(player, unit_index, weapon_slot_select=2, target_ref, target_index, "
           "target_fine_x, target_fine_y), args NOT swapped (0x00484f19-0x00484f5f)");
        ck(trace_eq({"unit_get_coords", "dir_from_to", "unit_fire_weapon"}),
           "T3: exact call order -- immediate return after fire_weapon (0x00484f64 JMP), no select_weapon "
           "call");
        ck(g_select_calls.empty() && g_bldg_calls.empty() && g_set_state_order_calls.empty() &&
               g_notify_calls.empty(),
           "T3: no other callee fires on the already-selected path");
        ck_eq((uint32_t)fx.u(s.player, s.index).selected_weapon, 2u, "T3: selected_weapon UNCHANGED (this arm never writes it)");
    }

    // =================================================================================================
    // T4 -- weapon not selected (selected_weapon==0x64), llm_strat_unit_select_weapon reports
    // NOT_FOUND (0x00484f81-0x00484f88 JZ taken): immediate return, selected_weapon stays at the
    // sentinel.
    // =================================================================================================
    {
        Seed s;
        s.facing_current    = 9;
        s.dir_from_to_ret   = 9;
        s.building_energy   = 3.0;
        s.selected_weapon   = UNIT_SELECT_WEAPON_NOT_FOUND;
        s.select_weapon_ret = UNIT_SELECT_WEAPON_NOT_FOUND;
        seed_and_run(fx, s);
        ck(g_select_calls.size() == 1 && g_select_calls[0].player == s.player &&
               g_select_calls[0].unit_index == s.index && g_select_calls[0].target_mask == 1u,
           "T4: unit_select_weapon(cur_player, cur_index, target_mask=1) (0x00484f6e-0x00484f7c)");
        ck(trace_eq({"unit_get_coords", "dir_from_to", "unit_select_weapon"}),
           "T4: selected==NOT_FOUND(0x64) -- immediate return, no further calls (0x00484f88 JZ taken)");
        ck(g_bldg_calls.empty() && g_set_state_order_calls.empty() && g_notify_calls.empty() &&
               g_fire_calls.empty(),
           "T4: no weapon-found-arm callee fires");
        ck_eq((uint32_t)fx.u(s.player, s.index).selected_weapon, (uint32_t)UNIT_SELECT_WEAPON_NOT_FOUND,
              "T4: selected_weapon stays at the NOT_FOUND sentinel (never committed on this path)");
    }

    // =================================================================================================
    // T5 -- weapon not selected, select_weapon reports a REAL slot (0x00484f88 JZ not taken): the
    // "replan" arm (0x00484f8a-0x00484ffb). PRESERVED ORIGINAL BUG: llm_strat_bldg_get_coords's two
    // out-pointers are BOTH `cur_unit+0x8e` (target_fine_x's own address, 0x00484f8a-0x00484fc2) --
    // target_fine_y's real memory is never passed to the callee, so it MUST read back exactly as
    // seeded (STALE) regardless of what the callee itself does with its aliased "out_y" argument. This
    // is pinned directly on the two recorded pointer arguments, not just on a downstream value.
    // =================================================================================================
    {
        Seed s;
        s.facing_current    = 11;
        s.dir_from_to_ret   = 11;
        s.building_energy   = 4.0;
        s.selected_weapon   = UNIT_SELECT_WEAPON_NOT_FOUND;
        s.select_weapon_ret = 2; // a real weapon slot
        seed_and_run(fx, s);
        const unit &u = fx.u(s.player, s.index);

        ck(g_bldg_calls.size() == 1, "T5: llm_strat_bldg_get_coords fires exactly once (0x00484fc6 CALL)");
        const int32_t owner = (int32_t)((uint32_t)(uint16_t)s.target_ref & 0xfu);
        ck(g_bldg_calls[0].player == (uint16_t)owner && g_bldg_calls[0].building_index == (int32_t)s.target_index,
           "T5: bldg_get_coords(target_owner, target_slot, ...) (0x00484fa8-0x00484fc6)");
        ck(g_bldg_calls[0].out_x_ptr == g_bldg_calls[0].out_y_ptr,
           "T5 PRESERVED BUG: the two out-pointer ARGUMENTS are bit-identical (0x00484f90 ADD ECX,0x8e / "
           "0x00484f9c ADD EBX,0x8e off the SAME cur_unit base -- no +0x92 anywhere in this sequence)");
        ck(g_bldg_calls[0].out_x_ptr == (void *)&u.target_fine_x,
           "T5 PRESERVED BUG: both aliased pointers equal &target_fine_x specifically (cur_unit+0x8e), "
           "never &target_fine_y (cur_unit+0x92)");
        ck_eq((uint32_t)u.target_fine_y, (uint32_t)s.target_fine_y,
              "T5 PRESERVED BUG: target_fine_y reads back EXACTLY as seeded (STALE) -- its real memory "
              "was never passed to the callee, so no write the callee makes can ever reach it. A "
              "\"fixed\" translation that passed &target_fine_y as the second pointer would change this "
              "value and must NOT pass this assertion.");
        ck_eq((uint32_t)u.target_fine_x, 0x2222u,
              "T5: target_fine_x holds the stub's LAST write through the aliased address (proves the "
              "call's out-pointers really do land on this field's own storage)");

        ck(g_set_state_order_calls.size() == 1 &&
               g_set_state_order_calls[0].new_order == ATTACK_BUILDING_ORDER_GROUP_MARSHAL &&
               g_set_state_order_calls[0].new_state == ATTACK_BUILDING_STATE_ATTACK_BUILDING,
           "T5: unit_set_state_order(GROUP_MARSHAL=0x0a, ATTACK_BUILDING=0x1c) (0x00484fcb-0x00484fd5)");
        ck(g_notify_calls.size() == 1 && g_notify_calls[0].player == s.player &&
               g_notify_calls[0].unit_index == s.index && g_notify_calls[0].status_code == ATTACK_BUILDING_NOTIFY_REPLAN,
           "T5: unit_notify_status(cur_player, cur_index, REPLAN=1) (0x00484fdf-0x00484fed)");
        ck_eq((uint32_t)u.selected_weapon, 2u,
              "T5: selected_weapon committed to the newly-selected slot, AFTER every other call "
              "(0x00484ff2-0x00484ffb)");
        ck(trace_eq({"unit_get_coords", "dir_from_to", "unit_select_weapon", "bldg_get_coords",
                     "unit_set_state_order", "unit_notify_status"}),
           "T5: exact call order matching the asm's straight-line layout (0x00484f7c/fc6/fd5/fed)");
    }

    // =================================================================================================
    // T6 -- tick_budget is drained to 0.0 UNCONDITIONALLY (0x00484e28/0x00484e32, both dwords, before
    // any gate) across every distinct arm this function can take.
    // =================================================================================================
    {
        struct Combo {
            uint8_t facing_current, dir_ret;
            double  energy;
            uint8_t selected_weapon, select_ret;
        };
        const Combo combos[5] = {
            {1, 9, 5.0, 0x64, 0x64}, // T1-style early return (facing mismatch)
            {2, 2, 0.0, 0x64, 0x64}, // T2-style destroyed
            {3, 3, 5.0, 2, 0x64},    // T3-style fire
            {4, 4, 5.0, 0x64, 0x64}, // T4-style weapon-not-found
            {5, 5, 5.0, 0x64, 2},    // T5-style replan
        };
        for (const Combo &c : combos) {
            Seed s;
            s.facing_current    = c.facing_current;
            s.dir_from_to_ret   = c.dir_ret;
            s.building_energy   = c.energy;
            s.selected_weapon   = c.selected_weapon;
            s.select_weapon_ret = c.select_ret;
            s.tick_budget       = 42.75; // nonzero sentinel every run
            seed_and_run(fx, s);
            ck_eq_d(fx.tick_budget, 0.0, "T6: tick_budget == 0.0 after every arm (0x00484e28/0x00484e32)");
        }
    }

    // =================================================================================================
    // T7 -- non-corruption: a guard roster slot this function never addresses (it operates entirely
    // through the ambient cur_unit pointer, never re-indexing units[] by player/index), and a guard
    // building record (a different owner/slot than the one this run's target_ref/target_index name),
    // read back exactly as seeded across the most field-mutating case (T5's replan arm).
    // =================================================================================================
    {
        Seed s;
        s.facing_current    = 11;
        s.dir_from_to_ret   = 11;
        s.building_energy   = 4.0;
        s.selected_weapon   = UNIT_SELECT_WEAPON_NOT_FOUND;
        s.select_weapon_ret = 2;
        seed_and_run(fx, s);

        const unit &g = fx.u(GUARD_PLAYER, GUARD_INDEX);
        ck(g.facing_current == 0x11, "T7: guard unit's facing_current untouched");
        ck_eq((uint32_t)g.target_ref, 0x77u, "T7: guard unit's target_ref untouched");
        ck_eq((uint32_t)g.target_index, 88u, "T7: guard unit's target_index untouched");
        ck_eq((uint32_t)g.target_fine_x, 9001u, "T7: guard unit's target_fine_x untouched");
        ck_eq((uint32_t)g.target_fine_y, 9002u, "T7: guard unit's target_fine_y untouched");
        ck_eq((uint32_t)g.selected_weapon, 3u, "T7: guard unit's selected_weapon untouched");

        ck_eq_d(fx.b(GUARD_BLDG_OWNER, GUARD_BLDG_SLOT).energy, 555.0,
                "T7: guard building record's energy untouched (this function only ever reads its own "
                "computed target_owner/target_slot)");
    }
}

} // namespace mh::sim::test
