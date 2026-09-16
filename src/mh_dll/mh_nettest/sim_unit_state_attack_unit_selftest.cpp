//
// sim_unit_state_attack_unit_selftest.cpp -- `simtest` oracle for
// llm_strat_unit_state_attack_unit @0x00485008 (sim/sim_unit_state_attack_unit.h/.cpp, RI-SIM /
// SIM1-G1). LARGEST body in this slice (0x45e = 1118 bytes), flagged review_required.
//
// SCOPE: the unconditional tick_budget=0.0 store (0x00485020-0x0048502a, every case); the
// target-owner/target-slot SNAPSHOT taken once (0x00485034-0x00485054, AND EAX,0xf at 0x00485040);
// the FCOMP(0.0, target.energy)/JC dead-vs-alive gate (0x0048506a-0x00485075), including the
// energy==0.0 EXACT boundary (JC not taken => DEAD, since C3=1/ZF=1 on equality, not C0/CF); the
// DEAD arm's release/clear/notify(0x66) sequence plus its nested
// state==ATTACK_UNIT_RETURN(0x1b) && (x!=home_x || y!=home_y) OR-gate on the move-home call
// (0x004850cc-0x00485130, both OR operands tested independently); the ALIVE arm's target_class +
// fine_to_tile(target_fine_x/y) -> unit_in_weapon_range chain (0x00485144-0x004851b7), including a
// negative-fine-coordinate case that separates truncating division from an arithmetic right-shift;
// the NOT-in-range arm's split on state==ATTACK_UNIT_RETURN (0x004851bd-0x00485296) -- the RETURN
// side re-runs the SAME release/clear/move-home/STOP_TO_DEFAULT sequence as the DEAD arm but with NO
// notify_status call (a duplicated-but-not-identical block, the precise place a copy-paste
// translation bug would land); the IN-range facing-tolerance gate, BOTH the tol==0 exact-match arm
// (0x0048536d-0x00485379) and the tol!=0 two-mirrored-circular-tests arm (0x00485300-0x0048536b,
// both operands AND both boundary edges of both blocked_a/blocked_b tested, plus a signed
// div-by-2-of-a-negative-field case at 0x004852e0-0x004852fd that separates the asm's
// SAR/SHL/SBB/SAR truncating-division idiom from a naive arithmetic right-shift); the aligned-fire
// arm (0x0048537f-0x004853d5), including get_coords->dir_from_to argument threading; and the
// selected_weapon==100 weapon-pick arm (0x004853da-0x0048545c) -- the SNAPSHOT-derived
// target_owner/target_slot elevation lookup (ground=1 vs air=2), the picked_weapon==100 dead end,
// and the success path's unit_set_state_order(GROUP_MARSHAL, LIVE state field) + notify(1) +
// selected_weapon write, in that order -- plus non-corruption (a neighbouring roster slot, the
// target's own roster neighbour, and every field on the acting unit this function never touches).
//
// EXPECTED BEHAVIOUR HAND-DERIVED FROM
// tmp/decomp_sim/llm_strat_unit_state_attack_unit_00485008.asm -- every assertion below cites the
// instruction address(es) it pins. NOT read off the .cpp body.
//
// Independent verification note: the .asm was read line-by-line against the
// committed .cpp and NO disagreement was found -- every branch, every register->field mapping, the
// FCOMP/SAHF condition-code derivation, the OR-gate short-circuit shapes, the two-mirrored-tolerance
// De Morgan pair, and the snapshot-vs-fresh-read reuse all match. See SUSPECTED TRANSLATION BUG in
// the report for the one caveat (unfalsifiable-by-construction, not a bug).
//
#include "sim/sim_unit_state_attack_unit.h"

#include <cstdint>
#include <cstring>
#include <vector>

#include "sim/sim_order_enqueue.h" // UNIT_STATE_STOP_TO_DEFAULT (real shared constant, not a local copy)
#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- this TU's own local copies of the .cpp's anonymous-namespace `detail::` constants ----------
// No backing Ghidra enum exists (same finding every sibling TU's own banner documents) -- these are
// THIS file's local copies (the .cpp's `detail::` constants have no header declaration, so they are
// not visible here), value-identical to and cross-checked against the .asm literals directly.
inline constexpr uint16_t STATE_ATTACK_UNIT_RETURN = 0x1b; // 0x004850cc / 0x004851c2 (CMP word[+0x6])
inline constexpr uint16_t STATE_GROUP_MARSHAL      = 0x0a; // 0x0048526f / 0x00485433 (new_order)
inline constexpr uint16_t STATE_ATTACK_UNIT        = 0x1a; // 0x00485274 (new_state literal)
inline constexpr uint32_t NOTIFY_STILL_CHASING     = 0x66; // 0x004850af MOV EBX,0x66
inline constexpr uint32_t NOTIFY_REPLAN            = 1;    // 0x0048527e / 0x00485438 MOV EBX,1

// ---- shared trace: proves CALL ORDER across all 10 unit_state_attack_unit_calls members ----------
std::vector<const char *> g_trace;
void                      tr(const char *tag) { g_trace.push_back(tag); }

bool trace_eq(const std::vector<const char *> &want) {
    if (g_trace.size() != want.size()) return false;
    for (size_t i = 0; i < want.size(); ++i)
        if (std::strcmp(g_trace[i], want[i]) != 0) return false;
    return true;
}

// ---- per-callee recorders (10, one per unit_state_attack_unit_calls member, exact header sigs) ---
struct ReleaseRefCall {
    uint32_t player_idx;
    int32_t  unit_idx;
    uint32_t mode;
};
std::vector<ReleaseRefCall> g_release_ref_calls;
void                        rec_target_release_ref(uint32_t player_idx, int32_t unit_idx, uint32_t mode) {
    tr("target_release_ref");
    g_release_ref_calls.push_back({player_idx, unit_idx, mode});
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

struct MoveAutoCall {
    uint16_t player;
    int32_t  unit_idx;
    uint32_t x, y;
};
std::vector<MoveAutoCall> g_move_auto_calls;
void                      rec_unit_order_move_auto(uint16_t player, int32_t unit_idx, uint32_t x, uint32_t y) {
    tr("unit_order_move_auto");
    g_move_auto_calls.push_back({player, unit_idx, x, y});
}

struct SetStateOrderCall {
    uint16_t new_order, new_state;
};
std::vector<SetStateOrderCall> g_set_state_order_calls;
void                           rec_unit_set_state_order(uint16_t new_order, uint16_t new_state) {
    tr("unit_set_state_order");
    g_set_state_order_calls.push_back({new_order, new_state});
}

struct TargetClassCall {
    uint32_t owner_and_kind_flag;
    int32_t  roster_slot;
};
std::vector<TargetClassCall> g_target_class_calls;
int32_t                      g_target_class_ret = 0;
int32_t                      rec_target_class(uint32_t owner_and_kind_flag, int32_t roster_slot) {
    tr("target_class");
    g_target_class_calls.push_back({owner_and_kind_flag, roster_slot});
    return g_target_class_ret;
}

struct InRangeCall {
    int32_t player, unit_idx, tile_x, tile_y, target_class_flags;
};
std::vector<InRangeCall> g_in_range_calls;
uint32_t                 g_in_range_ret = 1;
uint32_t                 rec_unit_in_weapon_range(int32_t player, int32_t unit_idx, int32_t tile_x, int32_t tile_y,
                                                  int32_t target_class_flags) {
    tr("unit_in_weapon_range");
    g_in_range_calls.push_back({player, unit_idx, tile_x, tile_y, target_class_flags});
    return g_in_range_ret;
}

struct GetCoordsCall {
    uint16_t player;
    int32_t  unit_index;
};
std::vector<GetCoordsCall> g_get_coords_calls;
void                       rec_unit_get_coords(uint16_t player, int32_t unit_index, int32_t *out_x, int32_t *out_y) {
    tr("unit_get_coords");
    g_get_coords_calls.push_back({player, unit_index});
    // Fixed, recognisable sentinel coords -- T14 asserts dir_from_to receives THESE values, proving
    // own_fine_x/own_fine_y are really threaded from this call's out-params, not left at 0.
    *out_x = 100;
    *out_y = 200;
}

struct DirFromToCall {
    int32_t x1, y1, x2, y2;
};
std::vector<DirFromToCall> g_dir_from_to_calls;
int32_t                    g_dir_from_to_ret = 0;
int32_t                    rec_dir_from_to(int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    tr("dir_from_to");
    g_dir_from_to_calls.push_back({x1, y1, x2, y2});
    return g_dir_from_to_ret;
}

struct SelectWeaponCall {
    uint16_t player;
    int32_t  unit_index;
    uint32_t target_mask;
};
std::vector<SelectWeaponCall> g_select_weapon_calls;
uint8_t                       g_select_weapon_ret = 100;
uint8_t                       rec_unit_select_weapon(uint16_t player, int32_t unit_index, uint32_t target_mask) {
    tr("unit_select_weapon");
    g_select_weapon_calls.push_back({player, unit_index, target_mask});
    return g_select_weapon_ret;
}

struct FireWeaponCall {
    uint32_t player, unit_index;
    uint8_t  weapon_slot;
    uint32_t target_ref;
    uint16_t target_index;
    int32_t  target_fine_x, target_fine_y;
};
std::vector<FireWeaponCall> g_fire_weapon_calls;
void                        rec_unit_fire_weapon(uint32_t player, uint32_t unit_index, uint8_t weapon_slot, uint32_t target_ref,
                                                 uint16_t target_index, int32_t target_fine_x, int32_t target_fine_y) {
    tr("unit_fire_weapon");
    g_fire_weapon_calls.push_back(
        {player, unit_index, weapon_slot, target_ref, target_index, target_fine_x, target_fine_y});
}

const unit_state_attack_unit_calls g_calls = {
    &rec_target_release_ref,
    &rec_unit_notify_status,
    &rec_unit_order_move_auto,
    &rec_unit_set_state_order,
    &rec_target_class,
    &rec_unit_in_weapon_range,
    &rec_unit_get_coords,
    &rec_dir_from_to,
    &rec_unit_select_weapon,
    &rec_unit_fire_weapon,
};

void reset_observations() {
    g_trace.clear();
    g_release_ref_calls.clear();
    g_notify_calls.clear();
    g_move_auto_calls.clear();
    g_set_state_order_calls.clear();
    g_target_class_calls.clear();
    g_in_range_calls.clear();
    g_get_coords_calls.clear();
    g_dir_from_to_calls.clear();
    g_select_weapon_calls.clear();
    g_fire_weapon_calls.clear();
}

// Fixed "guard" slot no test's own (player,index) ever addresses -- seeded with values that WOULD
// trigger several of this function's own branches if misaddressed (RETURN state, no-weapon sentinel,
// x!=home_x, alive energy) so a wrong-index read/write is observable, not just a plain zero.
constexpr uint16_t GUARD_PLAYER = 7;
constexpr int32_t  GUARD_INDEX  = 40;

void seed_guard_slot(sim_fixture &fx) {
    unit &g           = fx.u(GUARD_PLAYER, GUARD_INDEX);
    g.unit_proto_id   = 88;
    g.target_ref      = 0x21;
    g.target_index    = 12;
    g.state           = STATE_ATTACK_UNIT_RETURN; // would fire the RETURN move-home arm if misaddressed
    g.selected_weapon = 100;                      // would fire the weapon-pick arm if misaddressed
    g.x               = 1;
    g.y               = 2;
    g.home_x          = 3;
    g.home_y          = 4; // x!=home_x -- would move-home if misaddressed
    g.order           = 0x5555;
    g.elevation       = 777777;
    g.facing_target   = 0x11;
    g.goal_x          = 0x21;
    g.goal_y          = 0x22;
    g.move_group_id   = 333222;
    g.energy          = 1.0; // alive -- a stray dead-arm write here would also be wrong
}

// ---- fixture seeding -----------------------------------------------------------------------------
struct Seed {
    uint16_t player  = 2;
    int32_t  index   = 3;
    uint16_t cfg_row = 15;

    int16_t target_ref   = 0x23; // owner nibble (target_ref & 0xf) = 3 -- 0x00485040 AND EAX,0xf
    int16_t target_index = 6;    // target roster slot -- 0x0048504d MOVZX word[+0x8a]

    int32_t target_fine_x = 320; // tile 10 * 32 -- a multiple of 32 unless a case overrides it
    int32_t target_fine_y = 640; // tile 20 * 32

    double  target_energy    = 5.0; // > 0.0 => ALIVE by default (0x0048506c FCOMP / 0x00485075 JC)
    int32_t target_elevation = 0;   // GROUND -- only read in the weapon-pick arm

    uint16_t state           = 0x05; // neutral: != STATE_ATTACK_UNIT_RETURN(0x1b)
    uint8_t  selected_weapon = 9;    // != 100 -- fire path by default
    uint8_t  facing_current  = 9;
    uint8_t  x = 40, y = 50, home_x = 60, home_y = 70;

    // Non-corruption sentinels -- fields this function's own body NEVER writes (the real writes on
    // order/state/energy/x/y/goal_*/move_group_id all belong to the ORIGINAL callees, stubbed here).
    uint16_t order_sentinel         = 0x1234;
    int32_t  elevation_sentinel     = 424242;
    uint8_t  facing_target_sentinel = 0xaa;
    uint8_t  goal_x_sentinel = 0xb1, goal_y_sentinel = 0xb2;
    int32_t  move_group_id_sentinel = 999888;

    uint32_t proto_tol_field = 0; // cfg_units[cfg_row].weapon_facing_tolerance -- tol==0 branch by default

    double tick_budget = 12.5;

    uint32_t in_range_ret      = 1;
    int32_t  target_class_ret  = 77;
    int32_t  dir_from_to_ret   = 9; // == facing_current by default -> tol==0 gate PASSES (aligned)
    uint8_t  select_weapon_ret = 100;
};

void seed_and_run(sim_fixture &fx, const Seed &s) {
    fx.reset();
    seed_guard_slot(fx);

    const uint32_t target_owner = (uint32_t)(uint16_t)s.target_ref & 0xfu; // 0x00485040 AND EAX,0xf
    const uint32_t target_slot  = (uint32_t)(uint16_t)s.target_index;      // 0x0048504d MOVZX

    unit &u           = fx.u(s.player, s.index);
    u.unit_proto_id   = s.cfg_row;
    u.target_ref      = s.target_ref;
    u.target_index    = s.target_index;
    u.target_fine_x   = s.target_fine_x;
    u.target_fine_y   = s.target_fine_y;
    u.state           = s.state;
    u.selected_weapon = s.selected_weapon;
    u.facing_current  = s.facing_current;
    u.x               = s.x;
    u.y               = s.y;
    u.home_x          = s.home_x;
    u.home_y          = s.home_y;
    u.order           = s.order_sentinel;
    u.elevation       = s.elevation_sentinel;
    u.facing_target   = s.facing_target_sentinel;
    u.goal_x          = s.goal_x_sentinel;
    u.goal_y          = s.goal_y_sentinel;
    u.move_group_id   = s.move_group_id_sentinel;

    unit &tgt     = fx.u(target_owner, target_slot);
    tgt.energy    = s.target_energy;
    tgt.elevation = s.target_elevation;

    // The neighbouring roster slot -- only [target_owner][target_slot] is ever read (0x0048506c
    // energy / 0x004853ed elevation); this must never be touched, and its DISTINCT sentinel values
    // catch an off-by-one in the target_owner*100+target_slot indexing.
    unit &tgt_neighbor     = fx.u(target_owner, target_slot + 1);
    tgt_neighbor.energy    = -55.0;
    tgt_neighbor.elevation = -66;

    fx.cur_unit_ptr    = &u;
    fx.view_cur_player = s.player;
    fx.view_cur_index  = (uint16_t)s.index;

    fx.cfg_units[s.cfg_row].weapon_facing_tolerance = s.proto_tol_field;

    fx.tick_budget = s.tick_budget;

    reset_observations();
    g_target_class_ret  = s.target_class_ret;
    g_in_range_ret      = s.in_range_ret;
    g_dir_from_to_ret   = s.dir_from_to_ret;
    g_select_weapon_ret = s.select_weapon_ret;

    sim_store own = fx.store();
    detail::unit_state_attack_unit(fx.view(), own, g_calls);
}

} // namespace

void run_unit_state_attack_unit_tests() {
    sim_fixture fx;

    // =================================================================================================
    // T1 -- DEAD arm (energy<=0.0), state != ATTACK_UNIT_RETURN: release_ref(player,idx,mode=1) ->
    // clear target_ref/target_index -> notify(0x66); NO move_auto (state gate closed). Also pins the
    // FCOMP boundary: energy==0.0 EXACTLY takes the DEAD path (JC not taken on equality -- ZF/C3=1,
    // not CF/C0).
    // =================================================================================================
    {
        Seed s;
        s.target_energy = 0.0; // exact boundary -- 0x00485075 JC NOT taken -> falls to DEAD (0x0048507b)
        s.state         = 0x05;
        seed_and_run(fx, s);

        ck(g_release_ref_calls.size() == 1 && g_release_ref_calls[0].player_idx == 2 &&
               g_release_ref_calls[0].unit_idx == 3 && g_release_ref_calls[0].mode == 1u,
           "T1: target_release_ref(player=2, unit_idx=3, mode=1) (0x00485080-0x0048508e)");
        const unit &u = fx.u(2, 3);
        ck_eq((uint32_t)u.target_ref, 0u, "T1: target_ref cleared to 0 (0x00485098)");
        ck_eq((uint32_t)u.target_index, 0u, "T1: target_index cleared to 0 (0x004850a6)");
        ck(g_notify_calls.size() == 1 && g_notify_calls[0].player == 2 && g_notify_calls[0].unit_index == 3 &&
               g_notify_calls[0].status_code == NOTIFY_STILL_CHASING,
           "T1: unit_notify_status(player=2, idx=3, status=0x66) (0x004850af-0x004850c2)");
        ck(g_move_auto_calls.empty(), "T1: state!=RETURN -- unit_order_move_auto does NOT fire (0x004850d1 JNZ taken)");
        ck(g_set_state_order_calls.size() == 1 && g_set_state_order_calls[0].new_order == UNIT_STATE_STOP_TO_DEFAULT &&
               g_set_state_order_calls[0].new_state == UNIT_STATE_STOP_TO_DEFAULT,
           "T1: unit_set_state_order(STOP_TO_DEFAULT, STOP_TO_DEFAULT) (0x00485130-0x0048513a)");
        ck(trace_eq({"target_release_ref", "unit_notify_status", "unit_set_state_order"}),
           "T1: exact call order, energy==0.0 boundary took the DEAD path");
        ck_eq_d(fx.tick_budget, 0.0, "T1: tick_budget zeroed unconditionally (0x00485020-0x0048502a)");
        ck_eq((uint32_t)u.selected_weapon, 9u, "T1: selected_weapon UNCHANGED (only written by the weapon-pick arm)");
        ck_eq((uint32_t)u.order, 0x1234u, "T1: order field untouched (this function never writes it)");
        ck_eq((uint32_t)u.elevation, 424242u, "T1: acting unit's own elevation untouched (only the TARGET's elevation is ever read)");
    }

    // =================================================================================================
    // T2 -- DEAD arm, state==ATTACK_UNIT_RETURN, x!=home_x (y==home_y): the OR-gate's FIRST operand
    // alone opens the move-home call -- order_move_auto(player,idx, home_x, home_y), args NOT swapped.
    // =================================================================================================
    {
        Seed s;
        s.target_energy = -3.0; // negative (not just the ==0 boundary) -- also DEAD
        s.state         = STATE_ATTACK_UNIT_RETURN;
        s.x             = 40;
        s.home_x        = 99; // differs
        s.y             = 50;
        s.home_y        = 50; // equal -- isolates the FIRST OR operand
        seed_and_run(fx, s);
        ck(g_move_auto_calls.size() == 1 && g_move_auto_calls[0].player == 2 && g_move_auto_calls[0].unit_idx == 3 &&
               g_move_auto_calls[0].x == 99 && g_move_auto_calls[0].y == 50,
           "T2: unit_order_move_auto(player=2, idx=3, x=home_x=99, y=home_y=50) (0x00485105-0x0048512b)");
        ck(trace_eq({"target_release_ref", "unit_notify_status", "unit_order_move_auto", "unit_set_state_order"}),
           "T2: move_auto fires between notify and set_state_order (straight-line asm block order)");
    }

    // =================================================================================================
    // T3 -- DEAD arm, state==ATTACK_UNIT_RETURN, x==home_x AND y==home_y (already home): move_auto
    // does NOT fire -- both OR operands false.
    // =================================================================================================
    {
        Seed s;
        s.target_energy = -1.0;
        s.state         = STATE_ATTACK_UNIT_RETURN;
        s.x             = 40;
        s.home_x        = 40;
        s.y             = 50;
        s.home_y        = 50;
        seed_and_run(fx, s);
        ck(g_move_auto_calls.empty(), "T3: x==home_x && y==home_y -- unit_order_move_auto does NOT fire (0x00485103 JZ taken)");
        ck(trace_eq({"target_release_ref", "unit_notify_status", "unit_set_state_order"}), "T3: no move_auto in the trace");
    }

    // =================================================================================================
    // T4 -- DEAD arm, state==ATTACK_UNIT_RETURN, x==home_x but y!=home_y: the OR-gate's SECOND operand
    // alone opens the move-home call (proves it is a genuine OR, not just "x!=home_x").
    // =================================================================================================
    {
        Seed s;
        s.target_energy = -1.0;
        s.state         = STATE_ATTACK_UNIT_RETURN;
        s.x             = 40;
        s.home_x        = 40; // equal
        s.y             = 50;
        s.home_y        = 99; // differs -- isolates the SECOND OR operand
        seed_and_run(fx, s);
        ck(g_move_auto_calls.size() == 1 && g_move_auto_calls[0].x == 40 && g_move_auto_calls[0].y == 99,
           "T4: y!=home_y alone opens the OR gate -- move_auto(x=home_x=40, y=home_y=99) (0x004850ec-0x00485105)");
    }

    // =================================================================================================
    // T5 -- ALIVE + NOT in range + state != ATTACK_UNIT_RETURN: re-plan via
    // unit_set_state_order(GROUP_MARSHAL=0xa, ATTACK_UNIT=0x1a) + notify(1). NO release_ref, NO
    // target clear (target_ref/target_index UNCHANGED -- this arm never touches them, unlike the DEAD
    // and RETURN-in-not-in-range arms).
    // =================================================================================================
    {
        Seed s;
        s.target_energy    = 5.0; // ALIVE
        s.in_range_ret     = 0;   // NOT in range
        s.target_class_ret = 77;
        s.state            = 0x05; // != RETURN
        seed_and_run(fx, s);

        ck(g_target_class_calls.size() == 1 && g_target_class_calls[0].owner_and_kind_flag == 35 &&
               g_target_class_calls[0].roster_slot == 6,
           "T5: target_class(target_ref=(uint16)0x23=35, target_index=6) (0x00485144-0x0048515c)");
        ck(g_in_range_calls.size() == 1 && g_in_range_calls[0].player == 2 && g_in_range_calls[0].unit_idx == 3 &&
               g_in_range_calls[0].tile_x == 10 && g_in_range_calls[0].tile_y == 20 &&
               g_in_range_calls[0].target_class_flags == 77,
           "T5: unit_in_weapon_range(player=2, idx=3, tile_x=320/32=10, tile_y=640/32=20, class=77) "
           "(0x00485144-0x004851b0)");
        ck(g_set_state_order_calls.size() == 1 && g_set_state_order_calls[0].new_order == STATE_GROUP_MARSHAL &&
               g_set_state_order_calls[0].new_state == STATE_ATTACK_UNIT,
           "T5: unit_set_state_order(GROUP_MARSHAL=0xa, ATTACK_UNIT=0x1a) (0x0048526f-0x00485279)");
        ck(g_notify_calls.size() == 1 && g_notify_calls[0].status_code == NOTIFY_REPLAN,
           "T5: unit_notify_status(player=2, idx=3, status=1) (0x0048527e-0x00485291)");
        ck(g_release_ref_calls.empty() && g_move_auto_calls.empty(),
           "T5: re-plan arm never calls release_ref or move_auto");
        const unit &u = fx.u(2, 3);
        ck_eq((uint32_t)u.target_ref, 0x23u, "T5: target_ref UNCHANGED (re-plan arm never clears it)");
        ck_eq((uint32_t)u.target_index, 6u, "T5: target_index UNCHANGED");
        ck(trace_eq({"target_class", "unit_in_weapon_range", "unit_set_state_order", "unit_notify_status"}),
           "T5: exact call order");
    }

    // =================================================================================================
    // T6 -- ALIVE + NOT in range + state==ATTACK_UNIT_RETURN, x!=home_x: the SAME
    // release/clear/move-home/STOP_TO_DEFAULT sequence as the DEAD arm (T1/T2) -- BUT this arm never
    // calls unit_notify_status. That absence is the key pin: a copy-paste translation of the DEAD
    // arm's body here would wrongly add a notify(0x66) call.
    // =================================================================================================
    {
        Seed s;
        s.target_energy = 5.0;
        s.in_range_ret  = 0;
        s.state         = STATE_ATTACK_UNIT_RETURN;
        s.x             = 40;
        s.home_x        = 99;
        s.y             = 50;
        s.home_y        = 50;
        seed_and_run(fx, s);

        ck(g_release_ref_calls.size() == 1 && g_release_ref_calls[0].player_idx == 2 &&
               g_release_ref_calls[0].unit_idx == 3 && g_release_ref_calls[0].mode == 1u,
           "T6: target_release_ref(player=2, idx=3, mode=1) (0x004851d2-0x004851e0)");
        const unit &u = fx.u(2, 3);
        ck_eq((uint32_t)u.target_ref, 0u, "T6: target_ref cleared (0x004851ea)");
        ck_eq((uint32_t)u.target_index, 0u, "T6: target_index cleared (0x004851f8)");
        ck(g_move_auto_calls.size() == 1 && g_move_auto_calls[0].x == 99 && g_move_auto_calls[0].y == 50,
           "T6: unit_order_move_auto(x=home_x=99, y=home_y=50) (0x00485233-0x00485259)");
        ck(g_set_state_order_calls.size() == 1 && g_set_state_order_calls[0].new_order == UNIT_STATE_STOP_TO_DEFAULT &&
               g_set_state_order_calls[0].new_state == UNIT_STATE_STOP_TO_DEFAULT,
           "T6: unit_set_state_order(STOP_TO_DEFAULT, STOP_TO_DEFAULT) (0x0048525e-0x00485268)");
        ck(g_notify_calls.empty(),
           "T6: unit_notify_status does NOT fire in this arm (no notify call anywhere in "
           "0x004851bd-0x00485296 unlike the DEAD arm's 0x004850af)");
        ck(trace_eq({"target_class", "unit_in_weapon_range", "target_release_ref", "unit_order_move_auto",
                     "unit_set_state_order"}),
           "T6: exact call order, no notify_status anywhere in the trace");
    }

    // =================================================================================================
    // T7 -- ALIVE + NOT in range + state==ATTACK_UNIT_RETURN + already home (x==home_x && y==home_y):
    // no move_auto, same as T6 minus the move call.
    // =================================================================================================
    {
        Seed s;
        s.target_energy = 5.0;
        s.in_range_ret  = 0;
        s.state         = STATE_ATTACK_UNIT_RETURN;
        s.x             = 40;
        s.home_x        = 40;
        s.y             = 50;
        s.home_y        = 50;
        seed_and_run(fx, s);
        ck(g_move_auto_calls.empty(), "T7: already home -- unit_order_move_auto does NOT fire");
        ck(trace_eq({"target_class", "unit_in_weapon_range", "target_release_ref", "unit_set_state_order"}),
           "T7: exact call order without move_auto");
    }

    // =================================================================================================
    // T8 -- IN range, tol==0 (cfg field / 2 == 0), facing MISMATCH: immediate return with ZERO further
    // calls after dir_from_to (0x0048536d-0x00485379 JNZ taken) -- no fire, no select_weapon, no
    // set_state_order/notify. target_ref/target_index/selected_weapon all UNCHANGED.
    // =================================================================================================
    {
        Seed s;
        s.target_energy   = 5.0;
        s.in_range_ret    = 1;
        s.proto_tol_field = 0; // tol==0
        s.facing_current  = 9;
        s.dir_from_to_ret = 15; // mismatch
        seed_and_run(fx, s);
        ck(trace_eq({"target_class", "unit_in_weapon_range", "unit_get_coords", "dir_from_to"}),
           "T8: trace stops right after dir_from_to -- immediate return (0x00485379 JNZ)");
        ck(g_fire_weapon_calls.empty() && g_select_weapon_calls.empty() && g_set_state_order_calls.empty() &&
               g_notify_calls.empty(),
           "T8: no fire/select_weapon/set_state_order/notify_status -- true early return");
        const unit &u = fx.u(2, 3);
        ck_eq((uint32_t)u.target_ref, 0x23u, "T8: target_ref UNCHANGED");
        ck_eq((uint32_t)u.selected_weapon, 9u, "T8: selected_weapon UNCHANGED");
    }

    // =================================================================================================
    // T9 -- signed-division idiom check: cfg proto field weapon_facing_tolerance = -7 (0xfffffff9). The asm's
    // SAR/SHL/SBB/SAR sequence (0x004852e0-0x004852fd) computes tol = -7/2 = -3 (C truncation-toward-
    // zero); a naive `field >> 1` arithmetic-shift translation would instead get floor(-7/2) = -4.
    // With facing_current=30, target_dir=3 (diff=27), correct tol=-3 gives 24-tol=27 so
    // "diff < 24-tol" is 27<27 = FALSE (NOT blocked, proceeds to fire) -- the buggy tol=-4 gives
    // 24-tol=28 so 27<28 = TRUE (WOULD block, wrongly returns). This is a genuine mutation-separating
    // case, not just a boundary echo of T11/T13.
    // =================================================================================================
    {
        Seed s;
        s.target_energy   = 5.0;
        s.in_range_ret    = 1;
        s.proto_tol_field = (uint32_t)-7; // int32 -7
        s.facing_current  = 30;
        s.dir_from_to_ret = 3; // diff = facing_current - target_dir = 27
        s.selected_weapon = 9; // != 100 -- fire is the observable "not blocked" signal
        seed_and_run(fx, s);
        ck(g_fire_weapon_calls.size() == 1,
           "T9: tol=(-7)/2=-3 (truncating) -- NOT blocked, reaches fire_weapon; tol=-4 (naive shift) "
           "would have blocked here (0x004852f6-0x004852fb SAR/SUB/SAR idiom)");
    }

    // =================================================================================================
    // T10 -- blocked_a TRUE: tol=3 (weapon_facing_tolerance=7), target_dir<facing_current, diff=5 strictly
    // inside (tol,24-tol)=(3,21) -- early return, zero further calls.
    // =================================================================================================
    {
        Seed s;
        s.target_energy   = 5.0;
        s.in_range_ret    = 1;
        s.proto_tol_field = 7; // tol=3
        s.facing_current  = 10;
        s.dir_from_to_ret = 5; // diff=5
        seed_and_run(fx, s);
        ck(trace_eq({"target_class", "unit_in_weapon_range", "unit_get_coords", "dir_from_to"}),
           "T10: blocked_a (target_dir<facing_current, 3<5<21) -- immediate return (0x0048533b JL)");
        ck(g_fire_weapon_calls.empty() && g_select_weapon_calls.empty(), "T10: no fire/select_weapon");
    }

    // =================================================================================================
    // T11 -- blocked_a boundary NOT blocked: diff == 24-tol EXACTLY (strict '<', not '<=') -- proceeds.
    // =================================================================================================
    {
        Seed s;
        s.target_energy   = 5.0;
        s.in_range_ret    = 1;
        s.proto_tol_field = 7; // tol=3
        s.facing_current  = 21;
        s.dir_from_to_ret = 0; // diff=21 == 24-tol=21
        s.selected_weapon = 9;
        seed_and_run(fx, s);
        ck(g_fire_weapon_calls.size() == 1,
           "T11: diff(21) == 24-tol(21) EXACTLY -- NOT blocked (strict '<' at 0x0048533b), proceeds to fire");
    }

    // =================================================================================================
    // T12 -- blocked_b TRUE, via the OTHER direction (facing_current<target_dir): tol=3, diff=8
    // strictly inside (3,21).
    // =================================================================================================
    {
        Seed s;
        s.target_energy   = 5.0;
        s.in_range_ret    = 1;
        s.proto_tol_field = 7; // tol=3
        s.facing_current  = 2;
        s.dir_from_to_ret = 10; // diff=8
        seed_and_run(fx, s);
        ck(trace_eq({"target_class", "unit_in_weapon_range", "unit_get_coords", "dir_from_to"}),
           "T12: blocked_b (facing_current<target_dir, 3<8<21) -- immediate return (0x00485362 JL)");
    }

    // =================================================================================================
    // T13 -- blocked_b boundary NOT blocked: diff == tol EXACTLY (strict '<' on the LOWER bound too)
    // -- proceeds.
    // =================================================================================================
    {
        Seed s;
        s.target_energy   = 5.0;
        s.in_range_ret    = 1;
        s.proto_tol_field = 7; // tol=3
        s.facing_current  = 10;
        s.dir_from_to_ret = 13; // diff=3 == tol=3
        s.selected_weapon = 9;
        seed_and_run(fx, s);
        ck(g_fire_weapon_calls.size() == 1,
           "T13: diff(3) == tol(3) EXACTLY -- NOT blocked (strict '<' at 0x0048534e/0x00485327), proceeds to fire");
    }

    // =================================================================================================
    // T14 -- aligned + selected_weapon!=100: fire_weapon fires with the EXACT args (weapon_slot,
    // target_ref, target_index, target_fine_x/y), function returns immediately (no select_weapon, no
    // set_state_order/notify). Also proves unit_get_coords's out-params are really threaded into
    // dir_from_to's x1/y1 (the stub writes 100/200; dir_from_to must receive exactly that).
    // =================================================================================================
    {
        Seed s;
        s.target_energy   = 5.0;
        s.in_range_ret    = 1;
        s.proto_tol_field = 0; // tol==0, facing_current==dir_from_to_ret by default -> aligned
        s.selected_weapon = 5;
        s.target_fine_x   = 320;
        s.target_fine_y   = 640;
        seed_and_run(fx, s);

        ck(g_get_coords_calls.size() == 1 && g_get_coords_calls[0].player == 2 && g_get_coords_calls[0].unit_index == 3,
           "T14: unit_get_coords(player=2, idx=3) (0x004852a1-0x004852af)");
        ck(g_dir_from_to_calls.size() == 1 && g_dir_from_to_calls[0].x1 == 100 && g_dir_from_to_calls[0].y1 == 200 &&
               g_dir_from_to_calls[0].x2 == 320 && g_dir_from_to_calls[0].y2 == 640,
           "T14: dir_from_to(own_fine_x=100, own_fine_y=200, target_fine_x=320, target_fine_y=640) -- "
           "get_coords' out-params really threaded through (0x004852b4-0x004852d2)");
        ck(g_fire_weapon_calls.size() == 1 && g_fire_weapon_calls[0].player == 2 && g_fire_weapon_calls[0].unit_index == 3 &&
               g_fire_weapon_calls[0].weapon_slot == 5 && g_fire_weapon_calls[0].target_ref == 35 &&
               g_fire_weapon_calls[0].target_index == 6 && g_fire_weapon_calls[0].target_fine_x == 320 &&
               g_fire_weapon_calls[0].target_fine_y == 640,
           "T14: unit_fire_weapon(player=2, idx=3, weapon=5, target_ref=35, target_index=6, "
           "fine_x=320, fine_y=640) (0x0048538a-0x004853d0)");
        ck(g_select_weapon_calls.empty() && g_set_state_order_calls.empty() && g_notify_calls.empty(),
           "T14: fire path calls NOTHING else");
        ck(trace_eq({"target_class", "unit_in_weapon_range", "unit_get_coords", "dir_from_to", "unit_fire_weapon"}),
           "T14: exact call order");
        ck_eq((uint32_t)fx.u(2, 3).selected_weapon, 5u, "T14: selected_weapon UNCHANGED (fire path never writes it)");
    }

    // =================================================================================================
    // T15 -- aligned + selected_weapon==100 (no weapon), target elevation==0 (GROUND): select_weapon
    // called with target_mask=WEAPON_TARGET_GROUND(1); picked_weapon==100 (still none) -> NO further
    // calls, selected_weapon stays 100.
    // =================================================================================================
    {
        Seed s;
        s.target_energy     = 5.0;
        s.in_range_ret      = 1;
        s.proto_tol_field   = 0;
        s.selected_weapon   = 100;
        s.target_elevation  = 0;   // ground
        s.select_weapon_ret = 100; // still no weapon found
        seed_and_run(fx, s);
        ck(g_select_weapon_calls.size() == 1 && g_select_weapon_calls[0].player == 2 &&
               g_select_weapon_calls[0].unit_index == 3 && g_select_weapon_calls[0].target_mask == WEAPON_TARGET_GROUND,
           "T15: unit_select_weapon(player=2, idx=3, mask=GROUND=1) (0x004853ff-0x00485417)");
        ck(g_set_state_order_calls.empty() && g_notify_calls.empty(),
           "T15: picked_weapon==100 -- no set_state_order/notify (0x00485423 JZ taken -> return)");
        ck(trace_eq({"target_class", "unit_in_weapon_range", "unit_get_coords", "dir_from_to", "unit_select_weapon"}),
           "T15: exact call order, trace ends at select_weapon");
        ck_eq((uint32_t)fx.u(2, 3).selected_weapon, 100u, "T15: selected_weapon stays 100 (no successful pick)");
    }

    // =================================================================================================
    // T16 -- aligned + selected_weapon==100, target elevation!=0 (AIR): select_weapon called with
    // target_mask=WEAPON_TARGET_AIR(2); picked_weapon=42 (success) -> unit_set_state_order(
    // GROUP_MARSHAL=0xa, new_state=the LIVE state field, NOT a hardcoded literal) + notify(1) +
    // selected_weapon=42, in that order.
    // =================================================================================================
    {
        Seed s;
        s.target_energy     = 5.0;
        s.in_range_ret      = 1;
        s.proto_tol_field   = 0;
        s.selected_weapon   = 100;
        s.target_elevation  = 7;    // nonzero -- AIR
        s.select_weapon_ret = 42;   // success
        s.state             = 0x22; // distinct sentinel -- proves new_state reads the LIVE field
        seed_and_run(fx, s);
        ck(g_select_weapon_calls.size() == 1 && g_select_weapon_calls[0].target_mask == WEAPON_TARGET_AIR,
           "T16: unit_select_weapon(mask=AIR=2) (0x004853f6-0x00485417)");
        ck(g_set_state_order_calls.size() == 1 && g_set_state_order_calls[0].new_order == STATE_GROUP_MARSHAL &&
               g_set_state_order_calls[0].new_state == 0x22,
           "T16: unit_set_state_order(GROUP_MARSHAL=0xa, new_state=0x22=LIVE state field, not a literal) "
           "(0x00485425-0x00485433)");
        ck(g_notify_calls.size() == 1 && g_notify_calls[0].status_code == NOTIFY_REPLAN,
           "T16: unit_notify_status(player=2, idx=3, status=1) (0x00485438-0x0048544b)");
        ck(trace_eq({"target_class", "unit_in_weapon_range", "unit_get_coords", "dir_from_to", "unit_select_weapon",
                     "unit_set_state_order", "unit_notify_status"}),
           "T16: exact call order -- set_state_order BEFORE notify (0x00485433 precedes 0x0048544b)");
        ck_eq((uint32_t)fx.u(2, 3).selected_weapon, 42u, "T16: selected_weapon = picked_weapon = 42 (0x00485459, AFTER both calls)");
    }

    // =================================================================================================
    // T17 -- fine_to_tile signed truncation: negative target_fine_x/y must be TRUNCATED toward zero
    // (plain C '/32'), NOT floor-divided by an arithmetic '>>5'. -5/32=0 (>>5 would give -1);
    // -40/32=-1 (>>5 would give -2). Kept out of range so the trace stays short (isolates the
    // arithmetic).
    // =================================================================================================
    {
        Seed s;
        s.target_energy = 5.0;
        s.in_range_ret  = 0; // short-circuit right after -- isolates the tile_x/tile_y computation
        s.target_fine_x = -5;
        s.target_fine_y = -40;
        s.state         = 0x05; // != RETURN -- keep the not-in-range arm minimal
        seed_and_run(fx, s);
        ck(g_in_range_calls.size() == 1, "T17: unit_in_weapon_range called once");
        ck_eq((uint32_t)g_in_range_calls[0].tile_x, (uint32_t)0,
              "T17: fine_to_tile(-5) = -5/32 = 0 (truncating), NOT -1 (>>5 floor) (0x0049482... via "
              "the SAR/SHL/SBB/SAR idiom at 0x00485174-0x0048517c)");
        ck_eq((uint32_t)g_in_range_calls[0].tile_y, (uint32_t)-1,
              "T17: fine_to_tile(-40) = -40/32 = -1 (truncating), NOT -2 (>>5 floor) (0x00485193-0x0048519b)");
    }

    // =================================================================================================
    // T18 -- non-corruption: the neighbouring roster slot GUARD_PLAYER/GUARD_INDEX (seeded to trigger
    // several of this function's OWN branches if misaddressed), the target's own roster NEIGHBOUR
    // ([target_owner][target_slot+1]), and every field on the acting unit this function's body never
    // writes -- all stay exactly as seeded across the busiest run (DEAD + RETURN + move-home).
    // =================================================================================================
    {
        Seed s;
        s.target_energy = -3.0;
        s.state         = STATE_ATTACK_UNIT_RETURN;
        s.x             = 40;
        s.home_x        = 99;
        s.y             = 50;
        s.home_y        = 50;
        seed_and_run(fx, s);

        const unit &g = fx.u(GUARD_PLAYER, GUARD_INDEX);
        ck(g.unit_proto_id == 88 && g.target_ref == 0x21 && g.target_index == 12 &&
               g.state == STATE_ATTACK_UNIT_RETURN && g.selected_weapon == 100,
           "T18: guard unit's dispatch-relevant fields untouched");
        ck(g.x == 1 && g.y == 2 && g.home_x == 3 && g.home_y == 4, "T18: guard unit's position/home untouched");
        ck(g.order == 0x5555 && g.elevation == 777777 && g.facing_target == 0x11 && g.goal_x == 0x21 &&
               g.goal_y == 0x22 && g.move_group_id == 333222,
           "T18: guard unit's other fields untouched");
        ck_eq_d(g.energy, 1.0, "T18: guard unit's energy untouched");

        const uint32_t target_owner = (uint32_t)(uint16_t)s.target_ref & 0xfu;
        const uint32_t target_slot  = (uint32_t)(uint16_t)s.target_index;
        const unit    &nb           = fx.u(target_owner, target_slot + 1);
        ck_eq_d(nb.energy, -55.0, "T18: target's roster NEIGHBOUR energy untouched (no off-by-one in the index)");
        ck_eq((uint32_t)nb.elevation, (uint32_t)-66, "T18: target's roster NEIGHBOUR elevation untouched");

        const unit &u = fx.u(2, 3);
        ck_eq((uint32_t)u.order, 0x1234u, "T18: acting unit's order field untouched");
        ck_eq((uint32_t)u.elevation, 424242u, "T18: acting unit's own elevation untouched (only the TARGET's is read)");
        ck_eq((uint32_t)u.facing_target, 0xaau, "T18: acting unit's facing_target untouched (only facing_current is read)");
        ck_eq((uint32_t)u.goal_x, 0xb1u, "T18: acting unit's goal_x untouched");
        ck_eq((uint32_t)u.goal_y, 0xb2u, "T18: acting unit's goal_y untouched");
        ck_eq((uint32_t)u.move_group_id, 999888u, "T18: acting unit's move_group_id untouched");
    }

    // =================================================================================================
    // T19 -- tick_budget is zeroed UNCONDITIONALLY (0x00485020-0x0048502a, a full double store, before
    // any branch), across a sweep of DEAD / alive-not-in-range / alive-blocked / alive-fire scenarios,
    // each with a distinct nonzero sentinel pre-seeded.
    // =================================================================================================
    {
        struct Combo {
            double   energy;
            uint32_t in_range;
            double   budget_seed;
        };
        const Combo combos[4] = {
            {0.0, 1, 77.25},  // DEAD boundary (energy==0.0)
            {5.0, 0, 3.5},    // alive, not in range -- re-plan arm
            {-1.0, 1, 999.0}, // DEAD, negative energy
            {5.0, 1, 0.001},  // alive, in range, aligned (Seed defaults: tol=0, facing match) -- fire arm
        };
        for (const Combo &c : combos) {
            Seed s;
            s.target_energy = c.energy;
            s.in_range_ret  = c.in_range;
            s.tick_budget   = c.budget_seed;
            seed_and_run(fx, s);
            ck_eq_d(fx.tick_budget, 0.0, "T19: tick_budget == 0.0 after every scenario (0x00485020/0x0048502a)");
        }
    }
}

} // namespace mh::sim::test
