//
// sim_unit_state_group_marshal_selftest.cpp -- `simtest` cases for llm_strat_unit_state_group_marshal
// @0x00482451 (sim/sim_unit_state_group_marshal.h/.cpp), unit state 0xa GROUP_MARSHAL.
//
// EVERY EXPECTED VALUE BELOW WAS DERIVED FROM THE DISASSEMBLY
// (tmp/decomp/llm_strat_unit_state_group_marshal_00482451.asm), READ INSTRUCTION-BY-INSTRUCTION FOR
// THIS FILE, NOT COPIED FROM THE .cpp. Every assertion cites the address(es) it pins; where the .cpp's
// own header/body comments already cite the same address for the same fact, this file's citation is an
// independent re-derivation, not a copy (field offsets were cross-checked against
// mh_structs.gen.h's mh_map_object_unit / mh_llm_strat_group_scratch_member / mh_cfg_final_struct_Unit
// static_assert'd layouts).
//
// ---- SHAPE RECAP (see the .cpp for the full per-address derivation; this is the map used to plan
// the cases below) --------------------------------------------------------------------------------
//   1. 0x00482469-0x004824ce: ref_x/ref_y = cur_unit.goal_x/goal_y, captured UNCONDITIONALLY (a dead
//      CMP/JZ pair at 0x00482475-0x00482481 sets flags nothing downstream reads).
//   2. 0x004824ce-0x00482645: if order in {ATTACK_BUILDING=0x1c, ATTACK_UNIT=0x1a,
//      ATTACK_UNIT_RETURN=0x1b}: is_move_flag=0, attack_class=target_class(target_ref,target_index)
//      (0x004824da-0x004824f2, EAX=target_ref/2nd read EDX=target_index). If order != ATTACK_BUILDING
//      (0x004824fa/0x004824ff JZ 0x00482645 skips this for BUILDING): read target.energy at the roster
//      slot (target_ref&0xf, (ushort)target_index) (0x00482519-0x0048253d); if <=0 (0x00482546 JNC
//      taken means "not <=0", so the DEAD path is the fallthrough at LAB_004825c3): release target
//      (target_release_ref mode=1, 0x004825c8-0x004825d6), clear target_ref/target_index
//      (0x004825e0/0x004825ee, in THAT order), unit_set_state_order(default_op_code,default_op_code)
//      (0x00482606-0x00482623, cfg field @ 0xe4a185-array-base = default_op_code per
//      mh_cfg_final_struct_Unit's static_assert offset 0xed), unit_notify_status(...,0x66)
//      (0x00482628-0x0048263b), RETURN via JMP 0x00482f24 (0x00482640) -- group_move_register_member
//      is NEVER reached on this path. Else (target alive): unit_get_coords(target_owner,target_index_u,
//      &target_fine_x,&target_fine_y) (0x00482548-0x00482567, EBX=&target_fine_x@+0x8e,
//      ECX=&target_fine_y@+0x92) then goal_x/goal_y recomputed from target_fine_x/y via the signed
//      SAR/SHL/SBB/SAR fine->tile idiom (0x0048257e-0x004825b8) -- ref_x/ref_y captured in step 1 are
//      NOT re-read (stale by design).
//   3. 0x00482645-0x0048283d: register self (group_move_register_member(player,index,&count),
//      0x0048264f-0x00482662) then scan units[player][index+1..99]: peer joins iff
//      state==GROUP_MARSHAL(0xa, 0x00482696), order==u.order (0x004826bc-0x004826c7),
//      order!=ENTER_STORAGE_BEGIN(0x24, 0x004826df-0x004826e7), move_group_id==u.move_group_id
//      (0x004826ff-0x00482711), and EITHER (peer.order in the 3 attack orders,
//      0x0048272d-0x00482777): target_ref==u.target_ref && target_index==u.target_index
//      (0x00482779-0x004827d6) OR (else): goal_x==ref_x && goal_y==ref_y (0x004827d8-0x0048281a).
//      Matching peers call group_move_register_member(player,i,&count) (0x00482821-0x0048282e); the
//      scan stops at count==100 (0x00482833-0x00482837) or index 99.
//   4. 0x0048283d-0x00482876: leg_count = member_count<2 ? member_count :
//      pathfind_route_leg_group_and_sort(member_count,ref_x,ref_y) (0x0048284b-0x0048285f).
//      remaining_count=member_count; wave_cursor=leg_count.
//   5. 0x00482876-0x00482f24: do { group_move_order_commit(ref_x,ref_y,player,leg_count,move_group_id,
//      is_move_flag,attack_class) (reg args EAX=ref_x,EDX=ref_y,EBX=player,ECX=leg_count; STACK args
//      move_group_id,is_move_flag,attack_class in that order, 0x00482876-0x00482892) -- NOTE
//      attack_class ([EBP-0x20]) is written ONLY inside step 2's attack arm (0x004824f2); on a
//      plain-move order this stack slot is NEVER initialized before this PUSH, so the .cpp's
//      deterministic `attack_class=0` is the .cpp's own resolution of a genuinely-uninitialized-stack
//      read, not an independently-asm-provable fact -- flagged as a case caveat below, not asserted as
//      derived ground truth.
//      For w in [0,leg_count): restore passable[member.x][member.y]=scratch.saved_passable
//      (0x004828e0-0x0048290f); if unit_idx==cur_index, zero tick_budget (0x00482921-0x0048292b).
//      Then: if path_slot_id==0xff (0x0048294b-0x00482952): activity_clock+=2.0 (DAT_0050145a,
//      0x0048296a-0x00482976), next. Else if path_buffers[player][slot][0].run_length==0
//      (0x004829b3-0x004829ba, SHORT-CIRCUITS unit_path_step_blocked) OR
//      unit_path_step_blocked(player,unit_idx)!=0 (0x004829bc-0x004829cd): path_free_slot
//      (0x004829d3-0x004829dd); if member.order is an attack order AND
//      unit_in_weapon_range(player,unit_idx,tile_x,tile_y,target_class(member.target_ref,
//      member.target_index)) != 0 (0x00482a48-0x00482af9): unit_set_state_order_of(player,unit_idx,
//      member.order,1) + unit_notify_status(...,0x66) (0x00482b16-0x00482b3b), "redirected". Else:
//      ++path_blocked_retry_count (0x00482b5b); if <4 (0x00482b61-0x00482b68): activity_clock+=2.0
//      (DAT_00501462, 0x00482b80-0x00482b8c). Else (>=4): if target_ref!=0
//      (0x00482bad-0x00482bb5): target_release_ref mode=1 + clear target_ref/target_index
//      (0x00482bb7-0x00482c00); if order==ATTACK_UNIT_RETURN(0x1b) (0x00482c1f-0x00482c27):
//      unit_order_move_auto(player,unit_idx,home_x,home_y) (0x00482c29-0x00482c6d); else:
//      unit_notify_status(...,0x66) (0x00482c74-0x00482c88); then ALWAYS
//      unit_set_state_order_of(player,unit_idx,STOP_TO_DEFAULT=1,1) (0x00482c88-0x00482c9c) and
//      passable[member.x][member.y]=0 (0x00482ca1-0x00482ce2, clearing whatever the restore above set).
//      Else (good path, run_length!=0 && path_step_blocked==0, 0x00482cee): if member.order==
//      member_proto.move_op_code (0x00482d04-0x00482d37): unit_set_order_param(player,unit_idx,1)
//      (0x00482d39-0x00482d48). unit_set_state_of(player,unit_idx, unit_idx==cur_index ?
//      member_proto.move_op_code : MOVE_PATH_12=0xc) (0x00482d4d-0x00482da8). If move_group_id!=0 &&
//      leg_count>1 && member.order not an attack order (0x00482da8-0x00482e1e): goal_x=scratch.tile_col,
//      goal_y=scratch.tile_row (0x00482e1e-0x00482e64). ALWAYS: path_cursor=0,
//      path_blocked_retry_count=0, unit_notify_status(...,100) (0x00482e6a-0x00482eb6).
//      Wave compaction (0x00482ec0-0x00482f1a, only if remaining_count>0): target_rank=
//      scratch[wave_cursor].wave_rank (read BEFORE the wave_cursor<member_count bound check, matching
//      the asm's own evaluation order at 0x00482ede-0x00482eef); slide every following same-rank entry
//      down to the front. } while (remaining_count>0) (0x00482f1a-0x00482f1e JG).
//
#include "sim/sim_unit_state_group_marshal.h"

#include <cstdint>
#include <cstring>
#include <map>
#include <vector>

#include "sim_test_support.h"

namespace mh::sim::test {
namespace {

using namespace mh::sim;

// ---- the order/state domain (this TU's own copy -- see the .cpp's comment on why every sim TU
// re-derives it locally rather than sharing one) ---------------------------------------------------
constexpr uint16_t GM_STATE_GROUP_MARSHAL       = 0x0a;
constexpr uint16_t GM_ORDER_ATTACK_UNIT         = 0x1a;
constexpr uint16_t GM_ORDER_ATTACK_UNIT_RETURN  = 0x1b;
constexpr uint16_t GM_ORDER_ATTACK_BUILDING     = 0x1c;
constexpr uint16_t GM_ORDER_ENTER_STORAGE_BEGIN = 0x24;
constexpr uint16_t GM_STATE_STOP_TO_DEFAULT     = 0x01;
constexpr uint16_t GM_STATE_MOVE_PATH_12        = 0x0c;
constexpr uint32_t GM_NOTIFY_STATUS_TARGET_LOST = 0x66;
constexpr uint32_t GM_NOTIFY_STATUS_WAVE_READY  = 100;

// ---- shared trace -----------------------------------------------------------------------------
std::vector<const char *> g_trace;
void                      tr(const char *tag) { g_trace.push_back(tag); }
// True iff g_trace is exactly `want` in order (same helper shape as the group_step oracle).
bool trace_eq(const std::vector<const char *> &want) {
    if (g_trace.size() != want.size()) return false;
    for (size_t i = 0; i < want.size(); ++i)
        if (std::strcmp(g_trace[i], want[i]) != 0) return false;
    return true;
}
// True iff `tag` appears anywhere in g_trace (used where the exact sequence isn't asserted but a
// callee's total ABSENCE across a multi-member run is).
bool trace_has(const char *tag) {
    for (auto *t : g_trace)
        if (std::strcmp(t, tag) == 0) return true;
    return false;
}

// ---- the fixture the write-back stubs (register_member / route_sort) need direct access to ------
sim_fixture *g_fx = nullptr;

// ---- target_class -------------------------------------------------------------------------------
struct TargetClassCall {
    uint32_t ref;
    int32_t  idx;
};
std::vector<TargetClassCall> g_target_class_calls;
int32_t                      g_target_class_ret = 0;
int32_t                      rec_target_class(uint32_t ref, int32_t idx) {
    tr("target_class");
    g_target_class_calls.push_back({ref, idx});
    return g_target_class_ret;
}

// ---- target_release_ref -------------------------------------------------------------------------
struct ReleaseCall {
    uint32_t player;
    int32_t  idx;
    uint32_t mode;
};
std::vector<ReleaseCall> g_release_calls;
void                     rec_target_release_ref(uint32_t player, int32_t idx, uint32_t mode) {
    tr("target_release_ref");
    g_release_calls.push_back({player, idx, mode});
}

// ---- unit_get_coords ----------------------------------------------------------------------------
struct CoordsCall {
    uint16_t player;
    int32_t  idx;
};
std::vector<CoordsCall> g_coords_calls;
int32_t                 g_coords_fine_x = 0, g_coords_fine_y = 0;
void                    rec_unit_get_coords(uint16_t player, int32_t idx, int32_t *ox, int32_t *oy) {
    tr("unit_get_coords");
    g_coords_calls.push_back({player, idx});
    *ox = g_coords_fine_x;
    *oy = g_coords_fine_y;
}

// ---- unit_set_state_order (dead-target early-return only) ---------------------------------------
struct SetStateOrderCall {
    uint16_t order, state;
};
std::vector<SetStateOrderCall> g_set_state_order_calls;
void                           rec_unit_set_state_order(uint16_t order, uint16_t state) {
    tr("unit_set_state_order");
    g_set_state_order_calls.push_back({order, state});
}

// ---- unit_notify_status --------------------------------------------------------------------------
struct NotifyCall {
    uint32_t player;
    int32_t  idx;
    uint32_t status;
};
std::vector<NotifyCall> g_notify_calls;
void                    rec_unit_notify_status(uint32_t player, int32_t idx, uint32_t status) {
    tr("unit_notify_status");
    g_notify_calls.push_back({player, idx, status});
}

// ---- group_move_register_member -- MUST actually populate scratch + bump the count, per the
// conductor's brief, so the commit loop below has real data to work with. Models the real callee's
// documented tile_col/tile_row/unit_idx writes (mh_structs.gen.h's group_scratch_member field
// comments); saved_passable is TEST-CONTROLLED via g_saved_passable_seed rather than read from the
// fixture's live passable[] (this function is a full stub, not a reimplementation of the original) --
// what matters for the oracle is that detail:: later reads back exactly what was recorded here.
std::map<int32_t, int32_t> g_saved_passable_seed; // unit_idx -> saved_passable
struct RegisterCall {
    int32_t player, unit_idx;
};
std::vector<RegisterCall> g_register_calls;
void                      rec_group_move_register_member(int32_t player, int32_t unit_idx, int32_t *count_ptr) {
    tr("group_move_register_member");
    g_register_calls.push_back({player, unit_idx});
    unit &u          = g_fx->u(player, unit_idx);
    auto &m          = g_fx->group_move_scratch[(size_t)*count_ptr];
    m.tile_col       = u.x;
    m.tile_row       = u.y;
    m.wave_rank      = -1; // unassigned -- real llm_strat_pathfind_route_leg_group_and_sort seeds -1
    m.unit_idx       = unit_idx;
    auto it          = g_saved_passable_seed.find(unit_idx);
    m.saved_passable = (it != g_saved_passable_seed.end()) ? it->second : 0;
    ++(*count_ptr);
}

// ---- pathfind_route_leg_group_and_sort -- test-controlled leg_count + optional explicit wave_rank
// assignment (applied to scratch[0..member_count) when set), so a multi-wave case can be driven.
struct RouteSortCall {
    uint32_t member_count;
    int32_t  ref_x, ref_y;
};
std::vector<RouteSortCall> g_route_sort_calls;
int32_t                    g_route_sort_leg_count_ret = 0;
std::vector<int32_t>       g_route_sort_wave_ranks; // if non-empty, applied 1:1 to scratch[0..N)
int32_t                    rec_pathfind_route_leg_group_and_sort(uint32_t member_count, int32_t ref_x, int32_t ref_y) {
    tr("pathfind_route_leg_group_and_sort");
    g_route_sort_calls.push_back({member_count, ref_x, ref_y});
    for (uint32_t i = 0; i < member_count && i < g_route_sort_wave_ranks.size(); ++i)
        g_fx->group_move_scratch[i].wave_rank = g_route_sort_wave_ranks[i];
    return g_route_sort_leg_count_ret;
}

// ---- group_move_order_commit ----------------------------------------------------------------------
struct CommitCall {
    int32_t  ref_x, ref_y, player, leg_count, move_group_id;
    uint32_t is_move_flag, attack_class;
};
std::vector<CommitCall> g_commit_calls;
void                    rec_group_move_order_commit(int32_t ref_x, int32_t ref_y, int32_t player, int32_t leg_count,
                                                    int32_t move_group_id, uint32_t is_move_flag, uint32_t attack_class) {
    tr("group_move_order_commit");
    g_commit_calls.push_back({ref_x, ref_y, player, leg_count, move_group_id, is_move_flag, attack_class});
}

// ---- unit_path_step_blocked (per-unit_idx controllable) --------------------------------------------
struct BlockedCall {
    uint32_t player;
    int32_t  idx;
};
std::vector<BlockedCall>   g_blocked_calls;
std::map<int32_t, int32_t> g_path_step_blocked_ret_by_idx; // default 0 (not blocked)
int32_t                    rec_unit_path_step_blocked(uint32_t player, int32_t idx) {
    tr("unit_path_step_blocked");
    g_blocked_calls.push_back({player, idx});
    auto it = g_path_step_blocked_ret_by_idx.find(idx);
    return it != g_path_step_blocked_ret_by_idx.end() ? it->second : 0;
}

// ---- path_free_slot ----------------------------------------------------------------------------
struct FreeSlotCall {
    uint16_t player;
    int32_t  idx;
};
std::vector<FreeSlotCall> g_free_slot_calls;
void                      rec_path_free_slot(uint16_t player, int32_t idx) {
    tr("path_free_slot");
    g_free_slot_calls.push_back({player, idx});
}

// ---- unit_in_weapon_range (per-unit_idx controllable) ------------------------------------------
struct WeaponRangeCall {
    int32_t player, unit_idx, tile_x, tile_y, attack_class;
};
std::vector<WeaponRangeCall> g_weapon_range_calls;
std::map<int32_t, uint32_t>  g_in_weapon_range_ret_by_idx; // default 0 (out of range)
uint32_t                     rec_unit_in_weapon_range(int32_t player, int32_t unit_idx, int32_t tile_x, int32_t tile_y,
                                                      int32_t attack_class) {
    tr("unit_in_weapon_range");
    g_weapon_range_calls.push_back({player, unit_idx, tile_x, tile_y, attack_class});
    auto it = g_in_weapon_range_ret_by_idx.find(unit_idx);
    return it != g_in_weapon_range_ret_by_idx.end() ? it->second : 0u;
}

// ---- unit_set_state_order_of ---------------------------------------------------------------------
struct SetStateOrderOfCall {
    int32_t player, idx;
    int16_t state, param;
};
std::vector<SetStateOrderOfCall> g_set_state_order_of_calls;
void                             rec_unit_set_state_order_of(int32_t player, int32_t idx, int16_t state, int16_t param) {
    tr("unit_set_state_order_of");
    g_set_state_order_of_calls.push_back({player, idx, state, param});
}

// ---- unit_order_move_auto -----------------------------------------------------------------------
struct OrderMoveAutoCall {
    uint16_t player;
    int32_t  idx;
    uint32_t x, y;
};
std::vector<OrderMoveAutoCall> g_order_move_auto_calls;
void                           rec_unit_order_move_auto(uint16_t player, int32_t idx, uint32_t x, uint32_t y) {
    tr("unit_order_move_auto");
    g_order_move_auto_calls.push_back({player, idx, x, y});
}

// ---- unit_set_state_of --------------------------------------------------------------------------
struct SetStateOfCall {
    int32_t player, idx;
    int16_t state;
};
std::vector<SetStateOfCall> g_set_state_of_calls;
void                        rec_unit_set_state_of(int32_t player, int32_t idx, int16_t state) {
    tr("unit_set_state_of");
    g_set_state_of_calls.push_back({player, idx, state});
}

// ---- unit_set_order_param -----------------------------------------------------------------------
struct SetOrderParamCall {
    int32_t player, idx;
    int16_t param;
};
std::vector<SetOrderParamCall> g_set_order_param_calls;
void                           rec_unit_set_order_param(int32_t player, int32_t idx, int16_t param) {
    tr("unit_set_order_param");
    g_set_order_param_calls.push_back({player, idx, param});
}

const unit_state_group_marshal_calls g_calls = {
    &rec_target_class,
    &rec_target_release_ref,
    &rec_unit_get_coords,
    &rec_unit_set_state_order,
    &rec_unit_notify_status,
    &rec_group_move_register_member,
    &rec_pathfind_route_leg_group_and_sort,
    &rec_group_move_order_commit,
    &rec_unit_path_step_blocked,
    &rec_path_free_slot,
    &rec_unit_in_weapon_range,
    &rec_unit_set_state_order_of,
    &rec_unit_order_move_auto,
    &rec_unit_set_state_of,
    &rec_unit_set_order_param,
};

void reset_observations() {
    g_trace.clear();
    g_target_class_calls.clear();
    g_target_class_ret = 0;
    g_release_calls.clear();
    g_coords_calls.clear();
    g_coords_fine_x = 0;
    g_coords_fine_y = 0;
    g_set_state_order_calls.clear();
    g_notify_calls.clear();
    g_saved_passable_seed.clear();
    g_register_calls.clear();
    g_route_sort_calls.clear();
    g_route_sort_leg_count_ret = 0;
    g_route_sort_wave_ranks.clear();
    g_commit_calls.clear();
    g_blocked_calls.clear();
    g_path_step_blocked_ret_by_idx.clear();
    g_free_slot_calls.clear();
    g_weapon_range_calls.clear();
    g_in_weapon_range_ret_by_idx.clear();
    g_set_state_order_of_calls.clear();
    g_order_move_auto_calls.clear();
    g_set_state_of_calls.clear();
    g_set_order_param_calls.clear();
}

void begin(sim_fixture &fx) {
    fx.reset();
    g_fx = &fx;
    reset_observations();
}

void run(sim_fixture &fx) {
    sim_store own = fx.store();
    detail::unit_state_group_marshal(fx.view(), own, g_calls);
}

// =====================================================================================================
// T1 -- attack-target-DEAD early return (order=ATTACK_UNIT=0x1a, target.energy<=0). Proves the whole
// peer-gather/commit machinery is UNREACHED on this path (0x004825c3-0x00482640).
// =====================================================================================================
void test_t1_attack_target_dead_early_return(sim_fixture &fx) {
    begin(fx);

    const uint16_t player = 1, index = 2;
    unit          &u   = fx.u(player, index);
    fx.cur_unit_ptr    = &u;
    fx.view_cur_player = player;
    fx.view_cur_index  = index;

    u.order                          = GM_ORDER_ATTACK_UNIT;
    u.target_ref                     = 0x21; // owner = 0x21 & 0xf = 1 (0x00482511 AND EAX,0xf)
    u.target_index                   = 5;
    u.goal_x                         = 55;
    u.goal_y                         = 66; // must survive unchanged -- the dead path never reaches the recompute (step 2 else)
    u.move_group_id                  = 42; // must survive unchanged -- never reached
    u.unit_proto_id                  = 12;
    fx.cfg_units[12].default_op_code = 9; // distinct sentinel, read twice at 0x00482606/0x0048261c

    fx.u(1, 5).energy = 0.0; // the target: dead (<=0, 0x0048253d FCOMP / 0x00482546 JNC -> LAB_004825c3)

    // guard neighbour + guard tile, to prove nothing beyond the documented early-return effects fires
    unit &guard                   = fx.u(1, 3);
    guard.order                   = 0x40;
    guard.move_group_id           = 0x99;
    fx.passable[(50u << 8) | 51u] = 0xAB;

    g_target_class_ret = 7;

    run(fx);

    ck(g_target_class_calls.size() == 1 && g_target_class_calls[0].ref == 0x21u && g_target_class_calls[0].idx == 5,
       "T1: target_class(target_ref=0x21, target_index=5) fires unconditionally on the attack arm entry "
       "(0x004824da-0x004824ed)");
    ck(g_release_calls.size() == 1 && g_release_calls[0].player == 1 && g_release_calls[0].idx == 2 &&
           g_release_calls[0].mode == 1u,
       "T1: target_release_ref(player=1, index=2 [cur unit's OWN slot, not the target's], mode=1) "
       "(0x004825c8-0x004825d6)");
    ck_eq((uint32_t)(uint16_t)u.target_ref, 0u, "T1: target_ref cleared to 0 FIRST (0x004825e0)");
    ck_eq((uint32_t)(uint16_t)u.target_index, 0u, "T1: target_index cleared to 0 SECOND (0x004825ee)");
    ck(g_set_state_order_calls.size() == 1 && g_set_state_order_calls[0].order == 9 &&
           g_set_state_order_calls[0].state == 9,
       "T1: unit_set_state_order(default_op_code, default_op_code) -- BOTH args are the SAME cfg field, "
       "read twice (0x00482606, 0x0048261c)");
    ck(g_notify_calls.size() == 1 && g_notify_calls[0].player == 1 && g_notify_calls[0].idx == 2 &&
           g_notify_calls[0].status == GM_NOTIFY_STATUS_TARGET_LOST,
       "T1: unit_notify_status(player=1, index=2, 0x66) (0x00482628-0x0048263b)");
    ck(g_register_calls.empty(),
       "T1: group_move_register_member NEVER fires -- the dead-target path RETURNS via JMP 0x00482640, "
       "before step 3 (0x00482645) is ever reached");
    ck(g_coords_calls.empty(), "T1: unit_get_coords does NOT fire -- that's the ALIVE (else) branch only");
    ck(trace_eq({"target_class", "target_release_ref", "unit_set_state_order", "unit_notify_status"}),
       "T1: dead-target path call order is target_class -> release_ref -> set_state_order -> notify "
       "(0x004824b1-0x0048263b)");
    ck(guard.order == 0x40 && guard.move_group_id == 0x99, "T1: neighbouring unit slot untouched");
    ck_eq((uint32_t)fx.passable[(50u << 8) | 51u], 0xABu, "T1: untouched guard passable tile stays as seeded");
    ck_eq((uint32_t)u.goal_x, 55u, "T1: goal_x untouched by the dead path");
    ck_eq((uint32_t)u.goal_y, 66u, "T1: goal_y untouched by the dead path");
}

// =====================================================================================================
// T2 -- attack-target-ALIVE path (order=ATTACK_UNIT_RETURN=0x1b): re-fetch coords, recompute goal_x/y
// via the signed fine->tile idiom with a NEGATIVE input (round toward zero, not floor), STALE ref_x/
// ref_y proof in the commit call, empty peer gather, single-member commit (no route-sort call), the
// no-path-slot bad-path sub-branch, passable restore, and tick_budget zeroing for the ticking unit.
// =====================================================================================================
void test_t2_attack_target_alive_recompute(sim_fixture &fx) {
    begin(fx);

    const uint16_t player = 0, index = 1;
    unit          &u   = fx.u(player, index);
    fx.cur_unit_ptr    = &u;
    fx.view_cur_player = player;
    fx.view_cur_index  = index;

    u.order          = GM_ORDER_ATTACK_UNIT_RETURN;
    u.target_ref     = 0x32; // owner = 0x32 & 0xf = 2
    u.target_index   = 9;
    u.goal_x         = 11; // the STALE snapshot the commit call must still see
    u.goal_y         = 22;
    u.move_group_id  = 88;
    u.x              = 70; // self tile -- feeds the register-stub's scratch.tile_col/row and passable addr
    u.y              = 80;
    u.path_slot_id   = 0xff; // no path assigned yet -- the small-clock-bump sub-branch
    u.activity_clock = 100.0;
    fx.tick_budget   = 123.0;

    fx.u(2, 9).energy = 50.0; // the target: alive

    g_coords_fine_x               = -65; // -65/32 truncates to -2 (NOT floor's -3) -- 0x0048257e-0x00482589
    g_coords_fine_y               = -1;  // -1/32 truncates to 0 (NOT floor's -1)   -- 0x00482595-0x004825b8
    g_saved_passable_seed[1]      = 5;
    fx.passable[(70u << 8) | 80u] = 0; // pre-restore value, must be overwritten by the seed above

    g_target_class_ret = 3;

    run(fx);

    ck(g_target_class_calls.size() == 1 && g_target_class_calls[0].ref == 0x32u &&
           g_target_class_calls[0].idx == 9,
       "T2: target_class(0x32, 9) on the attack-arm entry (0x004824da-0x004824ed)");
    ck(g_coords_calls.size() == 1 && g_coords_calls[0].player == 2 && g_coords_calls[0].idx == 9,
       "T2: unit_get_coords(target_owner=2, target_index_u=9, &target_fine_x, &target_fine_y) -- the "
       "ALIVE branch (0x00482548-0x00482567)");
    ck_eq((uint32_t)u.target_fine_x, (uint32_t)-65, "T2: target_fine_x written by the stub");
    ck_eq((uint32_t)u.target_fine_y, (uint32_t)-1, "T2: target_fine_y written by the stub");
    ck_eq((uint32_t)u.goal_x, 254u, // (uint8_t)(-65/32) == (uint8_t)(-2) == 254
          "T2: goal_x = truncate-toward-zero(target_fine_x/32) as a byte (0x0048257e-0x00482589)");
    ck_eq((uint32_t)u.goal_y, 0u, // (uint8_t)(-1/32) == (uint8_t)(0) == 0, NOT floor's 255
          "T2: goal_y = truncate-toward-zero(target_fine_y/32) as a byte, NOT floor (0x00482595-0x004825b8)");
    ck(g_release_calls.empty(), "T2: alive target -- no target_release_ref (proves the JNC side, not JZ)");
    ck(g_register_calls.size() == 1 && g_register_calls[0].player == 0 && g_register_calls[0].unit_idx == 1,
       "T2: self-registration group_move_register_member(player=0, index=1) (0x0048264f-0x00482662)");
    ck(g_route_sort_calls.empty(),
       "T2: member_count(1) < 2 -- pathfind_route_leg_group_and_sort NOT called (0x0048284b JLE)");
    ck(g_commit_calls.size() == 1, "T2: exactly one group_move_order_commit");
    if (g_commit_calls.size() == 1) {
        const auto &c = g_commit_calls[0];
        ck_eq((uint32_t)c.ref_x, 11u,
              "T2: commit's ref_x is the STALE pre-recompute goal_x(11), not the recomputed 254 -- step 1's "
              "capture (0x00482469-0x004824ce) is never re-read after step 2's recompute");
        ck_eq((uint32_t)c.ref_y, 22u, "T2: commit's ref_y is the STALE pre-recompute goal_y(22)");
        ck_eq((uint32_t)c.player, 0u, "T2: commit player");
        ck_eq((uint32_t)c.leg_count, 1u, "T2: commit leg_count == member_count(1) (0x00482864 else-arm)");
        ck_eq((uint32_t)c.move_group_id, 88u, "T2: commit move_group_id");
        ck_eq(c.is_move_flag, 0u, "T2: is_move_flag=0 on the attack path (0x00482469 default 1, cleared 0x004824ce)");
        ck_eq(c.attack_class, 3u, "T2: attack_class == target_class's return, properly initialized on the attack path");
    }
    ck_eq((uint32_t)fx.passable[(70u << 8) | 80u], 5u,
          "T2: passable[self.x][self.y] restored to scratch.saved_passable (0x004828e0-0x0048290f)");
    ck_eq_d(fx.tick_budget, 0.0, "T2: tick_budget zeroed -- unit_idx==cur_index (0x00482921-0x0048292b)");
    ck_eq_d(u.activity_clock, 102.0,
            "T2: path_slot_id==0xff -- activity_clock += 2.0 (DAT_0050145a, 0x0048296a-0x00482976)");
    ck(g_free_slot_calls.empty() && g_blocked_calls.empty() && g_notify_calls.empty(),
       "T2: the no-path-slot sub-branch calls NOTHING else (0x0048294b JNZ skips 0x00482981 entirely)");
    ck(trace_eq({"target_class", "unit_get_coords", "group_move_register_member", "group_move_order_commit"}),
       "T2: exact call order for the alive-target, no-path-slot, single-member path");
    ck(fx.u(0, 2).order == 0 && fx.u(0, 2).state == 0, "T2: an unregistered roster slot stays fully zeroed");
}

// =====================================================================================================
// T3 -- ATTACK_BUILDING (order=0x1c) skips the dead-check/goal-recompute block ENTIRELY (it's gated by
// `order != ATTACK_BUILDING`, 0x004824fa/0x004824ff), even though it's still one of the 3 "attack"
// orders for is_move_flag/attack_class purposes.
// =====================================================================================================
void test_t3_attack_building_skips_dead_check(sim_fixture &fx) {
    begin(fx);

    const uint16_t player = 2, index = 0;
    unit          &u   = fx.u(player, index);
    fx.cur_unit_ptr    = &u;
    fx.view_cur_player = player;
    fx.view_cur_index  = index;

    u.order                  = GM_ORDER_ATTACK_BUILDING;
    u.target_ref             = 0x40;
    u.target_index           = 3;
    u.goal_x                 = 5; // must survive unchanged -- no recompute for ATTACK_BUILDING
    u.goal_y                 = 6;
    u.move_group_id          = 10;
    u.x                      = 90;
    u.y                      = 91;
    u.path_slot_id           = 0xff;
    u.activity_clock         = 1.0;
    fx.tick_budget           = 50.0;
    g_saved_passable_seed[0] = 17;
    g_target_class_ret       = 44;

    run(fx);

    ck(g_target_class_calls.size() == 1 && g_target_class_calls[0].ref == 0x40u &&
           g_target_class_calls[0].idx == 3,
       "T3: target_class still fires on the attack-arm entry (0x004824da-0x004824ed)");
    ck(g_coords_calls.empty(),
       "T3: unit_get_coords does NOT fire -- 0x004824fa/0x004824ff JZ 0x00482645 skips the whole "
       "dead-check/recompute block for ATTACK_BUILDING");
    ck(g_release_calls.empty(), "T3: no dead-check ran at all -- no target_release_ref possible");
    ck_eq((uint32_t)u.goal_x, 5u, "T3: goal_x untouched");
    ck_eq((uint32_t)u.goal_y, 6u, "T3: goal_y untouched");
    ck(g_register_calls.size() == 1 && g_register_calls[0].unit_idx == 0, "T3: self registered");
    ck(g_commit_calls.size() == 1 && g_commit_calls[0].ref_x == 5 && g_commit_calls[0].ref_y == 6 &&
           g_commit_calls[0].player == 2 && g_commit_calls[0].leg_count == 1 &&
           g_commit_calls[0].move_group_id == 10 && g_commit_calls[0].is_move_flag == 0 &&
           g_commit_calls[0].attack_class == 44u,
       "T3: commit args (ref_x=5,ref_y=6,player=2,leg_count=1,move_group_id=10,is_move_flag=0,attack_class=44)");
    ck_eq((uint32_t)fx.passable[(90u << 8) | 91u], 17u, "T3: passable restored to seed");
    ck_eq_d(fx.tick_budget, 0.0, "T3: tick_budget zeroed for self");
    ck_eq_d(u.activity_clock, 3.0, "T3: activity_clock += 2.0 (no-path-slot sub-branch)");
    ck(trace_eq({"target_class", "group_move_register_member", "group_move_order_commit"}),
       "T3: exact call order -- no unit_get_coords anywhere in the trace");
}

// =====================================================================================================
// T4 -- plain-move peer-gather predicate (order not in the attack set): 2 matching peers + 4 distinct
// non-matches (goal, order, state, move_group_id), exact registration ORDER, and non-corruption of the
// rejected peers.
// =====================================================================================================
void test_t4_peer_gather_plain_move(sim_fixture &fx) {
    begin(fx);

    const uint16_t player = 0, index = 1;
    unit          &u   = fx.u(player, index);
    fx.cur_unit_ptr    = &u;
    fx.view_cur_player = player;
    fx.view_cur_index  = index;

    u.order                  = 9; // plain move: not in {0x1a,0x1b,0x1c}
    u.move_group_id          = 77;
    u.goal_x                 = 30;
    u.goal_y                 = 40;
    u.x                      = 200;
    u.y                      = 201;
    u.path_slot_id           = 0xff;
    u.activity_clock         = 10.0;
    fx.tick_budget           = 99.0;
    g_saved_passable_seed[1] = 11;

    unit &p2                 = fx.u(0, 2); // MATCH
    p2.state                 = GM_STATE_GROUP_MARSHAL;
    p2.order                 = 9;
    p2.move_group_id         = 77;
    p2.goal_x                = 30;
    p2.goal_y                = 40;
    p2.x                     = 202;
    p2.y                     = 203;
    p2.path_slot_id          = 0xff;
    p2.activity_clock        = 20.0;
    g_saved_passable_seed[2] = 22;

    unit &p3          = fx.u(0, 3); // goal mismatch
    p3.state          = GM_STATE_GROUP_MARSHAL;
    p3.order          = 9;
    p3.move_group_id  = 77;
    p3.goal_x         = 99;
    p3.goal_y         = 40;
    p3.activity_clock = 1099.0;

    unit &p4          = fx.u(0, 4); // order mismatch
    p4.state          = GM_STATE_GROUP_MARSHAL;
    p4.order          = 8;
    p4.move_group_id  = 77;
    p4.goal_x         = 30;
    p4.goal_y         = 40;
    p4.activity_clock = 1004.0;

    unit &p5          = fx.u(0, 5); // state mismatch (not GROUP_MARSHAL)
    p5.state          = 0x5;
    p5.order          = 9;
    p5.move_group_id  = 77;
    p5.goal_x         = 30;
    p5.goal_y         = 40;
    p5.activity_clock = 1005.0;

    unit &p6          = fx.u(0, 6); // move_group_id mismatch
    p6.state          = GM_STATE_GROUP_MARSHAL;
    p6.order          = 9;
    p6.move_group_id  = 78;
    p6.goal_x         = 30;
    p6.goal_y         = 40;
    p6.activity_clock = 1006.0;

    unit &p7                 = fx.u(0, 7); // MATCH, distinct tile
    p7.state                 = GM_STATE_GROUP_MARSHAL;
    p7.order                 = 9;
    p7.move_group_id         = 77;
    p7.goal_x                = 30;
    p7.goal_y                = 40;
    p7.x                     = 15;
    p7.y                     = 25;
    p7.path_slot_id          = 0xff;
    p7.activity_clock        = 30.0;
    g_saved_passable_seed[7] = 77;

    fx.passable[(99u << 8) | 99u] = 0xCD; // untouched guard tile

    g_route_sort_leg_count_ret = 3; // single wave covering all 3 members

    run(fx);

    ck(g_target_class_calls.empty() && g_coords_calls.empty(),
       "T4: plain-move order never enters the attack arm at all");
    std::vector<RegisterCall> want = {{0, 1}, {0, 2}, {0, 7}};
    ck(g_register_calls.size() == want.size(), "T4: exactly 3 registrations (self + 2 matching peers)");
    if (g_register_calls.size() == want.size())
        for (size_t i = 0; i < want.size(); ++i)
            ck(g_register_calls[i].player == want[i].player && g_register_calls[i].unit_idx == want[i].unit_idx,
               "T4: registration order is self(1), then peer2, then peer7 -- ascending scan order "
               "(0x0048264f-0x00482662 self, 0x00482821-0x0048282e per matching peer)");
    ck(g_route_sort_calls.size() == 1 && g_route_sort_calls[0].member_count == 3 &&
           g_route_sort_calls[0].ref_x == 30 && g_route_sort_calls[0].ref_y == 40,
       "T4: pathfind_route_leg_group_and_sort(member_count=3, ref_x=30, ref_y=40) (0x0048285a)");
    ck(g_commit_calls.size() == 1 && g_commit_calls[0].leg_count == 3 && g_commit_calls[0].move_group_id == 77 &&
           g_commit_calls[0].is_move_flag == 1,
       "T4: commit leg_count=3, move_group_id=77, is_move_flag=1 (never cleared on a plain-move order)");
    ck(g_commit_calls.size() == 1 && g_commit_calls[0].attack_class == 0,
       "T4 (caveat): commit attack_class == 0 -- the .cpp's own deterministic resolution of the "
       "genuinely-UNINITIALIZED [EBP-0x20] stack slot on a plain-move order (never written before "
       "0x00482876's PUSH); not independently provable from the asm alone, see the file banner");
    ck_eq((uint32_t)fx.passable[(200u << 8) | 201u], 11u, "T4: self passable restored");
    ck_eq((uint32_t)fx.passable[(202u << 8) | 203u], 22u, "T4: peer2 passable restored");
    ck_eq((uint32_t)fx.passable[(15u << 8) | 25u], 77u, "T4: peer7 passable restored");
    ck_eq((uint32_t)fx.passable[(99u << 8) | 99u], 0xCDu, "T4: untouched guard tile stays as seeded");
    ck_eq_d(fx.tick_budget, 0.0, "T4: tick_budget zeroed for self only");
    ck_eq_d(u.activity_clock, 12.0, "T4: self activity_clock += 2.0");
    ck_eq_d(p2.activity_clock, 22.0, "T4: peer2 activity_clock += 2.0");
    ck_eq_d(p7.activity_clock, 32.0, "T4: peer7 activity_clock += 2.0");
    ck(g_notify_calls.empty(), "T4: the no-path-slot sub-branch never calls unit_notify_status");
    ck_eq_d(p3.activity_clock, 1099.0, "T4: rejected peer3 (goal mismatch) completely untouched");
    ck_eq_d(p4.activity_clock, 1004.0, "T4: rejected peer4 (order mismatch) completely untouched");
    ck_eq_d(p5.activity_clock, 1005.0, "T4: rejected peer5 (state mismatch) completely untouched");
    ck_eq_d(p6.activity_clock, 1006.0, "T4: rejected peer6 (move_group_id mismatch) completely untouched");
}

// =====================================================================================================
// T5 -- attack-order peer-gather predicate twin: peers matched by target_ref/target_index instead of
// goal_x/goal_y (0x00482779-0x004827d6). Uses ATTACK_BUILDING so the dead-check doesn't have to be
// separately fixtured (T3 already proves it's skipped).
// =====================================================================================================
void test_t5_peer_gather_attack_twin(sim_fixture &fx) {
    begin(fx);

    const uint16_t player = 2, index = 1;
    unit          &u   = fx.u(player, index);
    fx.cur_unit_ptr    = &u;
    fx.view_cur_player = player;
    fx.view_cur_index  = index;

    u.order                  = GM_ORDER_ATTACK_BUILDING;
    u.target_ref             = 0x55;
    u.target_index           = 8;
    u.move_group_id          = 60;
    u.goal_x                 = 70; // ref_x/ref_y -- irrelevant to this predicate but must be set for the commit
    u.goal_y                 = 71;
    u.x                      = 50;
    u.y                      = 51;
    u.path_slot_id           = 0xff;
    g_saved_passable_seed[1] = 1;

    unit &p2                 = fx.u(2, 2); // MATCH
    p2.state                 = GM_STATE_GROUP_MARSHAL;
    p2.order                 = GM_ORDER_ATTACK_BUILDING;
    p2.move_group_id         = 60;
    p2.target_ref            = 0x55;
    p2.target_index          = 8;
    p2.x                     = 52;
    p2.y                     = 53;
    p2.path_slot_id          = 0xff;
    g_saved_passable_seed[2] = 2;

    unit &p3         = fx.u(2, 3); // target_index mismatch
    p3.state         = GM_STATE_GROUP_MARSHAL;
    p3.order         = GM_ORDER_ATTACK_BUILDING;
    p3.move_group_id = 60;
    p3.target_ref    = 0x55;
    p3.target_index  = 9;

    unit &p4         = fx.u(2, 4); // target_ref mismatch
    p4.state         = GM_STATE_GROUP_MARSHAL;
    p4.order         = GM_ORDER_ATTACK_BUILDING;
    p4.move_group_id = 60;
    p4.target_ref    = 0x56;
    p4.target_index  = 8;

    g_target_class_ret         = 5;
    g_route_sort_leg_count_ret = 2;

    run(fx);

    std::vector<RegisterCall> want = {{2, 1}, {2, 2}};
    ck(g_register_calls.size() == want.size(),
       "T5: exactly self + the target-matching peer registered -- index/ref mismatches excluded");
    if (g_register_calls.size() == want.size())
        for (size_t i = 0; i < want.size(); ++i)
            ck(g_register_calls[i].player == want[i].player && g_register_calls[i].unit_idx == want[i].unit_idx,
               "T5: registration order self then peer2 (0x00482779-0x004827d6 attack-twin predicate)");
    ck(g_route_sort_calls.size() == 1 && g_route_sort_calls[0].member_count == 2 &&
           g_route_sort_calls[0].ref_x == 70 && g_route_sort_calls[0].ref_y == 71,
       "T5: route-sort(member_count=2, ref_x=70, ref_y=71)");
    ck(g_commit_calls.size() == 1 && g_commit_calls[0].leg_count == 2 && g_commit_calls[0].attack_class == 5u,
       "T5: commit leg_count=2, attack_class=target_class's return");
}

// =====================================================================================================
// T6 -- ENTER_STORAGE_BEGIN (0x24) peer exclusion: a peer matching EVERY OTHER criterion is still
// excluded because its (and the cur unit's) order is ENTER_STORAGE_BEGIN (0x004826df-0x004826e7).
// =====================================================================================================
void test_t6_enter_storage_begin_excluded(sim_fixture &fx) {
    begin(fx);

    const uint16_t player = 3, index = 0;
    unit          &u   = fx.u(player, index);
    fx.cur_unit_ptr    = &u;
    fx.view_cur_player = player;
    fx.view_cur_index  = index;

    u.order         = GM_ORDER_ENTER_STORAGE_BEGIN;
    u.move_group_id = 33;
    u.goal_x        = 15;
    u.goal_y        = 16;
    u.path_slot_id  = 0xff;

    unit &p1         = fx.u(3, 1); // matches state/order/group/goal, but order==ENTER_STORAGE_BEGIN
    p1.state         = GM_STATE_GROUP_MARSHAL;
    p1.order         = GM_ORDER_ENTER_STORAGE_BEGIN;
    p1.move_group_id = 33;
    p1.goal_x        = 15;
    p1.goal_y        = 16;

    run(fx);

    ck(g_target_class_calls.empty(), "T6: ENTER_STORAGE_BEGIN is not an attack order -- no target_class call");
    ck(g_register_calls.size() == 1 && g_register_calls[0].unit_idx == 0,
       "T6: only self registered -- the peer is excluded by the ENTER_STORAGE_BEGIN gate "
       "(0x004826df CMP 0x24 / 0x004826e7 JZ -> skip)");
    ck(g_route_sort_calls.empty(), "T6: member_count(1) < 2 -- no route-sort call");
}

// =====================================================================================================
// T7 -- plain-move bad-path family (member.order not an attack order, so the redirect check never
// applies): run_length==0 short-circuit vs run_length!=0+blocked (path_step_blocked IS called), retry
// <4 (no escalation) vs retry>=4 (escalation), target_ref==0 vs !=0 (release call gated), and the
// restore-then-CLEAR passable ordering on escalation.
// =====================================================================================================
void test_t7_bad_path_plain_move(sim_fixture &fx) {
    begin(fx);

    const uint16_t player = 4, index = 1;
    unit          &u   = fx.u(player, index);
    fx.cur_unit_ptr    = &u;
    fx.view_cur_player = player;
    fx.view_cur_index  = index;

    u.order                                                                                          = 9;
    u.move_group_id                                                                                  = 200;
    u.goal_x                                                                                         = 1;
    u.goal_y                                                                                         = 2;
    u.x                                                                                              = 1;
    u.y                                                                                              = 1;
    u.path_slot_id                                                                                   = 5;
    u.path_blocked_retry_count                                                                       = 2; // -> 3 after ++, <4: no escalation
    u.activity_clock                                                                                 = 1.0;
    g_saved_passable_seed[1]                                                                         = 44;
    fx.path_buffers[player * PATH_WAYPOINTS_PER_PLAYER + 5 * PATH_WAYPOINTS_PER_SLOT + 0].run_length = 0;

    unit &p2                                                                                         = fx.u(4, 2); // retry>=4, target_ref==0 -> notify_status branch
    p2.state                                                                                         = GM_STATE_GROUP_MARSHAL;
    p2.order                                                                                         = 9;
    p2.move_group_id                                                                                 = 200;
    p2.goal_x                                                                                        = 1;
    p2.goal_y                                                                                        = 2;
    p2.x                                                                                             = 2;
    p2.y                                                                                             = 2;
    p2.path_slot_id                                                                                  = 6;
    p2.path_blocked_retry_count                                                                      = 3; // -> 4, escalate
    p2.target_ref                                                                                    = 0;
    g_saved_passable_seed[2]                                                                         = 55;
    fx.path_buffers[player * PATH_WAYPOINTS_PER_PLAYER + 6 * PATH_WAYPOINTS_PER_SLOT + 0].run_length = 0;

    unit &p3                                                                                         = fx.u(4, 3); // run_length!=0 + BLOCKED, retry>=4, target_ref!=0 -> release fires
    p3.state                                                                                         = GM_STATE_GROUP_MARSHAL;
    p3.order                                                                                         = 9;
    p3.move_group_id                                                                                 = 200;
    p3.goal_x                                                                                        = 1;
    p3.goal_y                                                                                        = 2;
    p3.x                                                                                             = 3;
    p3.y                                                                                             = 3;
    p3.path_slot_id                                                                                  = 7;
    p3.path_blocked_retry_count                                                                      = 3; // -> 4, escalate
    p3.target_ref                                                                                    = 0x77;
    p3.target_index                                                                                  = 9;
    g_saved_passable_seed[3]                                                                         = 66;
    fx.path_buffers[player * PATH_WAYPOINTS_PER_PLAYER + 7 * PATH_WAYPOINTS_PER_SLOT + 0].run_length = 9;
    g_path_step_blocked_ret_by_idx[3]                                                                = 1;

    unit &guard                   = fx.u(4, 4); // never registered
    guard.activity_clock          = 12345.0;
    fx.passable[(77u << 8) | 77u] = 0x11;

    g_route_sort_leg_count_ret = 3;

    run(fx);

    ck(g_blocked_calls.size() == 1 && g_blocked_calls[0].idx == 3,
       "T7: unit_path_step_blocked called ONLY for peer3 (run_length!=0) -- self and peer2 short-circuit "
       "on run_length==0 (0x004829b3-0x004829ba)");
    std::vector<FreeSlotCall> want_free = {{4, 1}, {4, 2}, {4, 3}};
    ck(g_free_slot_calls.size() == want_free.size(),
       "T7: path_free_slot fires for all three bad-path members");
    if (g_free_slot_calls.size() == want_free.size())
        for (size_t i = 0; i < want_free.size(); ++i)
            ck(g_free_slot_calls[i].idx == want_free[i].idx, "T7: path_free_slot call set/order");
    ck(g_weapon_range_calls.empty(),
       "T7: order=9 is not an attack order -- the redirect branch is never entered for any member");

    ck_eq((uint32_t)u.path_blocked_retry_count, 3u, "T7: self retry_count 2->3 (<4, no escalation)");
    ck_eq_d(u.activity_clock, 3.0, "T7: self activity_clock += 2.0 (retry<4 sub-branch, DAT_00501462)");
    ck_eq((uint32_t)fx.passable[(1u << 8) | 1u], 44u, "T7: self passable restore-only (no escalation clear)");
    bool self_released = false;
    for (auto &r : g_release_calls)
        if (r.idx == 1) self_released = true;
    ck(!self_released, "T7: self never escalates -- no target_release_ref for it");

    bool p2_notified = false, p2_released = false, p2_stopped = false;
    for (auto &n : g_notify_calls)
        if (n.idx == 2 && n.status == GM_NOTIFY_STATUS_TARGET_LOST) p2_notified = true;
    for (auto &r : g_release_calls)
        if (r.idx == 2) p2_released = true;
    for (auto &s : g_set_state_order_of_calls)
        if (s.idx == 2 && s.state == (int16_t)GM_STATE_STOP_TO_DEFAULT && s.param == 1) p2_stopped = true;
    ck(p2_notified, "T7: peer2 escalates via unit_notify_status(0x66) -- order(9) != ATTACK_UNIT_RETURN "
                    "(0x00482c74-0x00482c88)");
    ck(!p2_released, "T7: peer2.target_ref==0 -- no target_release_ref (0x00482bb5 JZ)");
    ck(p2_stopped, "T7: peer2 ALWAYS gets unit_set_state_order_of(..., STOP_TO_DEFAULT=1, param=1) on "
                   "escalation (0x00482c88-0x00482c9c)");
    ck_eq((uint32_t)fx.passable[(2u << 8) | 2u], 0u,
          "T7: peer2 passable CLEARED to 0 -- the escalation clear (0x00482ce2) overwrites the earlier "
          "restore-to-55 (0x004828e0-0x0048290f), proving restore-then-clear ORDER");

    bool p3_released = false, p3_notified = false;
    for (auto &r : g_release_calls)
        if (r.idx == 3 && r.mode == 1u) p3_released = true;
    for (auto &n : g_notify_calls)
        if (n.idx == 3 && n.status == GM_NOTIFY_STATUS_TARGET_LOST) p3_notified = true;
    ck(p3_released, "T7: peer3.target_ref(0x77)!=0 -- target_release_ref(player,3,mode=1) fires "
                    "(0x00482bb7-0x00482bcb)");
    ck_eq((uint32_t)(uint16_t)p3.target_ref, 0u, "T7: peer3.target_ref cleared to 0");
    ck_eq((uint32_t)(uint16_t)p3.target_index, 0u, "T7: peer3.target_index cleared to 0");
    ck(p3_notified, "T7: peer3 ALSO takes the notify_status(0x66) branch (order 9 != ATTACK_UNIT_RETURN)");
    ck_eq((uint32_t)fx.passable[(3u << 8) | 3u], 0u, "T7: peer3 passable cleared to 0 on escalation");

    ck(g_order_move_auto_calls.empty(),
       "T7: no member's order is ATTACK_UNIT_RETURN -- unit_order_move_auto never fires (that's T8)");
    ck_eq_d(guard.activity_clock, 12345.0, "T7: never-registered guard slot fully untouched");
    ck_eq((uint32_t)fx.passable[(77u << 8) | 77u], 0x11u, "T7: untouched guard passable tile stays as seeded");
    ck(!trace_has("target_class") && !trace_has("unit_get_coords"),
       "T7: plain-move order never touches the attack machinery anywhere in the trace");
    int commit_count = 0;
    for (auto *t : g_trace)
        if (std::strcmp(t, "group_move_order_commit") == 0) ++commit_count;
    ck(commit_count == 1, "T7: single wave -- exactly one group_move_order_commit");
}

// =====================================================================================================
// T8 -- attack-order bad-path family (order=ATTACK_UNIT_RETURN=0x1b): the in-weapon-range REDIRECT
// (self) vs NOT redirected + escalate-with-order_move_auto (peer), contrasting directly with T7's
// notify_status escalation branch.
// =====================================================================================================
void test_t8_bad_path_attack_redirect_vs_escalate(sim_fixture &fx) {
    begin(fx);

    const uint16_t player = 5, index = 1;
    unit          &u   = fx.u(player, index);
    fx.cur_unit_ptr    = &u;
    fx.view_cur_player = player;
    fx.view_cur_index  = index;

    u.order                                                                                          = GM_ORDER_ATTACK_UNIT_RETURN;
    u.target_ref                                                                                     = 0x63; // owner = 0x63 & 0xf = 3
    u.target_index                                                                                   = 4;
    u.move_group_id                                                                                  = 300;
    u.goal_x                                                                                         = 1; // stale ref, unused by this test's assertions
    u.goal_y                                                                                         = 2;
    u.x                                                                                              = 11;
    u.y                                                                                              = 12;
    u.path_slot_id                                                                                   = 8;
    u.path_blocked_retry_count                                                                       = 2; // must stay UNCHANGED -- redirect skips the ++ entirely
    g_saved_passable_seed[1]                                                                         = 21;
    fx.path_buffers[player * PATH_WAYPOINTS_PER_PLAYER + 8 * PATH_WAYPOINTS_PER_SLOT + 0].run_length = 0;

    fx.u(3, 4).energy = 99.0; // alive target -- avoid the T1 early return
    g_coords_fine_x   = 320;  // -> tile 10
    g_coords_fine_y   = 640;  // -> tile 20

    unit &p6                                                                                         = fx.u(5, 6); // matches: target_ref/index equal cur's (unchanged by the recompute)
    p6.state                                                                                         = GM_STATE_GROUP_MARSHAL;
    p6.order                                                                                         = GM_ORDER_ATTACK_UNIT_RETURN;
    p6.move_group_id                                                                                 = 300;
    p6.target_ref                                                                                    = 0x63;
    p6.target_index                                                                                  = 4;
    p6.x                                                                                             = 13;
    p6.y                                                                                             = 14;
    p6.path_slot_id                                                                                  = 9;
    p6.path_blocked_retry_count                                                                      = 3;  // -> 4, escalate (not redirected)
    p6.target_fine_x                                                                                 = 64; // -> tile 2
    p6.target_fine_y                                                                                 = 96; // -> tile 3
    p6.home_x                                                                                        = 40;
    p6.home_y                                                                                        = 41;
    g_saved_passable_seed[6]                                                                         = 31;
    fx.path_buffers[player * PATH_WAYPOINTS_PER_PLAYER + 9 * PATH_WAYPOINTS_PER_SLOT + 0].run_length = 9;
    g_path_step_blocked_ret_by_idx[6]                                                                = 1;

    g_target_class_ret              = 8;
    g_in_weapon_range_ret_by_idx[1] = 1; // self: IN range -> redirected
    g_in_weapon_range_ret_by_idx[6] = 0; // peer: NOT in range -> escalate

    g_route_sort_leg_count_ret = 2;

    run(fx);

    ck(g_weapon_range_calls.size() == 2, "T8: unit_in_weapon_range called for both attack-order members");
    for (auto &c : g_weapon_range_calls) {
        if (c.unit_idx == 1)
            ck(c.tile_x == 10 && c.tile_y == 20 && c.attack_class == 8,
               "T8: self in-range check uses tile coords from target_fine_x/y (320/32=10, 640/32=20) via "
               "the SAME SAR/SHL/SBB/SAR idiom re-applied at 0x00482a48-0x00482ae4, and member_attack_class "
               "= target_class(member.target_ref, member.target_index) (0x00482a82)");
        if (c.unit_idx == 6)
            ck(c.tile_x == 2 && c.tile_y == 3 && c.attack_class == 8,
               "T8: peer in-range check uses the PEER's own target_fine_x/y (64/32=2, 96/32=3)");
    }

    bool self_redirect_state = false, self_notified = false;
    for (auto &s : g_set_state_order_of_calls)
        if (s.idx == 1 && s.state == (int16_t)GM_ORDER_ATTACK_UNIT_RETURN && s.param == 1) self_redirect_state = true;
    for (auto &n : g_notify_calls)
        if (n.idx == 1 && n.status == GM_NOTIFY_STATUS_TARGET_LOST) self_notified = true;
    ck(self_redirect_state,
       "T8: self REDIRECTED -- unit_set_state_order_of(player, 1, state=member.order=0x1b, param=1) "
       "(0x00482b16-0x00482b27)");
    ck(self_notified, "T8: self redirect also fires unit_notify_status(...,0x66) (0x00482b2c-0x00482b3b)");
    ck_eq((uint32_t)u.path_blocked_retry_count, 2u,
          "T8: self retry_count UNCHANGED -- redirected skips the ++/escalation logic entirely");
    ck_eq((uint32_t)fx.passable[(11u << 8) | 12u], 21u,
          "T8: self passable restore-ONLY -- redirect never reaches the escalation clear");
    bool self_released = false, self_moved_auto = false;
    for (auto &r : g_release_calls)
        if (r.idx == 1) self_released = true;
    for (auto &m : g_order_move_auto_calls)
        if (m.idx == 1) self_moved_auto = true;
    ck(!self_released && !self_moved_auto, "T8: self never escalates -- no release, no order_move_auto");

    bool peer_released = false, peer_moved_auto = false, peer_stopped = false, peer_notified = false;
    for (auto &r : g_release_calls)
        if (r.idx == 6 && r.mode == 1u) peer_released = true;
    for (auto &m : g_order_move_auto_calls)
        if (m.idx == 6 && m.player == 5 && m.x == 40u && m.y == 41u) peer_moved_auto = true;
    for (auto &s : g_set_state_order_of_calls)
        if (s.idx == 6 && s.state == (int16_t)GM_STATE_STOP_TO_DEFAULT && s.param == 1) peer_stopped = true;
    for (auto &n : g_notify_calls)
        if (n.idx == 6 && n.status == GM_NOTIFY_STATUS_TARGET_LOST) peer_notified = true;
    ck(peer_released, "T8: peer NOT redirected, target_ref(0x63)!=0 -- target_release_ref fires");
    ck_eq((uint32_t)(uint16_t)p6.target_ref, 0u, "T8: peer target_ref cleared");
    ck_eq((uint32_t)(uint16_t)p6.target_index, 0u, "T8: peer target_index cleared");
    ck(peer_moved_auto,
       "T8: peer order==ATTACK_UNIT_RETURN -- unit_order_move_auto(player,6,home_x=40,home_y=41) fires "
       "INSTEAD of notify_status (0x00482c29-0x00482c6d) -- the contrast with T7's plain-order escalation");
    ck(!peer_notified, "T8: peer does NOT take the notify_status(0x66) branch (order IS ATTACK_UNIT_RETURN)");
    ck(peer_stopped, "T8: peer ALWAYS gets the STOP_TO_DEFAULT set_state_order_of on escalation");
    ck_eq((uint32_t)fx.passable[(13u << 8) | 14u], 0u, "T8: peer passable cleared to 0 on escalation");
}

// =====================================================================================================
// T9 -- commit-loop GOOD path (run_length!=0 && path_step_blocked==0): the order_param/move_op_code
// gate on both sides, self-vs-other unit_set_state_of target, and the goal-overwrite POSITIVE case
// (move_group_id!=0 && leg_count>1 && order not attack) with DISTINCT per-member tile_col/tile_row.
// =====================================================================================================
void test_t9_good_path_and_goal_overwrite(sim_fixture &fx) {
    begin(fx);

    const uint16_t player = 6, index = 1;
    unit          &u   = fx.u(player, index);
    fx.cur_unit_ptr    = &u;
    fx.view_cur_player = player;
    fx.view_cur_index  = index;

    u.order                                                                                           = 9;
    u.move_group_id                                                                                   = 55;
    u.goal_x                                                                                          = 12; // stale, overwritten by the goal-overwrite branch
    u.goal_y                                                                                          = 13;
    u.x                                                                                               = 150; // -> scratch.tile_col
    u.y                                                                                               = 151; // -> scratch.tile_row
    u.unit_proto_id                                                                                   = 30;
    u.path_slot_id                                                                                    = 10;
    u.path_cursor                                                                                     = 77;
    u.path_blocked_retry_count                                                                        = 5;
    u.activity_clock                                                                                  = 1.0; // unused by the good path, just seeded to a sentinel
    fx.tick_budget                                                                                    = 44.0;
    g_saved_passable_seed[1]                                                                          = 201;
    fx.cfg_units[30].move_op_code                                                                     = 9; // == order -> unit_set_order_param fires for self
    fx.path_buffers[player * PATH_WAYPOINTS_PER_PLAYER + 10 * PATH_WAYPOINTS_PER_SLOT + 0].run_length = 3;

    unit &p5                                                                                          = fx.u(6, 5);
    p5.state                                                                                          = GM_STATE_GROUP_MARSHAL;
    p5.order                                                                                          = 9;
    p5.move_group_id                                                                                  = 55;
    p5.goal_x                                                                                         = 12;
    p5.goal_y                                                                                         = 13;
    p5.x                                                                                              = 160; // -> scratch.tile_col
    p5.y                                                                                              = 161; // -> scratch.tile_row
    p5.unit_proto_id                                                                                  = 31;
    p5.path_slot_id                                                                                   = 11;
    p5.path_cursor                                                                                    = 88;
    p5.path_blocked_retry_count                                                                       = 6;
    g_saved_passable_seed[5]                                                                          = 202;
    fx.cfg_units[31].move_op_code                                                                     = 42; // != order -> unit_set_order_param does NOT fire for peer
    fx.path_buffers[player * PATH_WAYPOINTS_PER_PLAYER + 11 * PATH_WAYPOINTS_PER_SLOT + 0].run_length = 3;

    g_route_sort_leg_count_ret = 2; // leg_count=2 > 1 -- required for the goal-overwrite condition

    run(fx);

    ck(g_blocked_calls.size() == 2, "T9: unit_path_step_blocked called for BOTH members (run_length!=0)");
    ck(g_free_slot_calls.empty(), "T9: good path -- path_free_slot never fires");

    ck(g_set_order_param_calls.size() == 1 && g_set_order_param_calls[0].idx == 1 &&
           g_set_order_param_calls[0].param == 1,
       "T9: unit_set_order_param(player,1,1) fires for self ONLY -- self.order(9)==self_proto.move_op_code(9) "
       "(0x00482d04-0x00482d48); peer.order(9)!=peer_proto.move_op_code(42) so it does NOT fire for peer");

    bool self_state_ok = false, peer_state_ok = false;
    for (auto &s : g_set_state_of_calls) {
        if (s.idx == 1 && s.state == 9) self_state_ok = true;                              // self branch: member_proto.move_op_code
        if (s.idx == 5 && s.state == (int16_t)GM_STATE_MOVE_PATH_12) peer_state_ok = true; // other branch
    }
    ck(self_state_ok, "T9: unit_set_state_of(player,1, move_op_code=9) -- unit_idx==cur_index branch "
                      "(0x00482d4d-0x00482d92)");
    ck(peer_state_ok, "T9: unit_set_state_of(player,5, MOVE_PATH_12=0xc) -- unit_idx!=cur_index branch "
                      "(0x00482d94-0x00482da8)");

    ck_eq((uint32_t)u.goal_x, 150u,
          "T9: self.goal_x = scratch.tile_col(150) -- move_group_id(55)!=0 && leg_count(2)>1 && order(9) "
          "not attack (0x00482e1e-0x00482e3e)");
    ck_eq((uint32_t)u.goal_y, 151u, "T9: self.goal_y = scratch.tile_row(151) (0x00482e44-0x00482e64)");
    ck_eq((uint32_t)p5.goal_x, 160u, "T9: peer.goal_x = scratch.tile_col(160)");
    ck_eq((uint32_t)p5.goal_y, 161u, "T9: peer.goal_y = scratch.tile_row(161)");

    ck_eq((uint32_t)u.path_cursor, 0u, "T9: self.path_cursor reset to 0 (0x00482e6a-0x00482e80)");
    ck_eq((uint32_t)u.path_blocked_retry_count, 0u, "T9: self.path_blocked_retry_count reset to 0 (0x00482ea0)");
    ck_eq((uint32_t)p5.path_cursor, 0u, "T9: peer.path_cursor reset to 0");
    ck_eq((uint32_t)p5.path_blocked_retry_count, 0u, "T9: peer.path_blocked_retry_count reset to 0");

    bool self_ready = false, peer_ready = false;
    for (auto &n : g_notify_calls) {
        if (n.idx == 1 && n.status == GM_NOTIFY_STATUS_WAVE_READY) self_ready = true;
        if (n.idx == 5 && n.status == GM_NOTIFY_STATUS_WAVE_READY) peer_ready = true;
    }
    ck(self_ready && peer_ready, "T9: both members get unit_notify_status(...,100) (0x00482eac-0x00482eb6)");

    ck_eq((uint32_t)fx.passable[(150u << 8) | 151u], 201u, "T9: self passable restored");
    ck_eq((uint32_t)fx.passable[(160u << 8) | 161u], 202u, "T9: peer passable restored");
    ck_eq_d(fx.tick_budget, 0.0, "T9: tick_budget zeroed for self");

    ck(g_commit_calls.size() == 1 && g_commit_calls[0].ref_x == 12 && g_commit_calls[0].ref_y == 13,
       "T9: commit still sees the STALE pre-overwrite goal (12,13)");
}

// =====================================================================================================
// T10 -- goal-overwrite SUPPRESSED when move_group_id==0, even though the good path is taken and
// leg_count>1 and the order is not an attack order (isolates the move_group_id!=0 conjunct).
// =====================================================================================================
void test_t10_goal_overwrite_suppressed_by_zero_group(sim_fixture &fx) {
    begin(fx);

    const uint16_t player = 7, index = 0;
    unit          &u   = fx.u(player, index);
    fx.cur_unit_ptr    = &u;
    fx.view_cur_player = player;
    fx.view_cur_index  = index;

    u.order                                                                                           = 9;
    u.move_group_id                                                                                   = 0; // the suppressing condition
    u.goal_x                                                                                          = 20;
    u.goal_y                                                                                          = 21;
    u.x                                                                                               = 170;
    u.y                                                                                               = 171;
    u.unit_proto_id                                                                                   = 40;
    u.path_slot_id                                                                                    = 12;
    fx.path_buffers[player * PATH_WAYPOINTS_PER_PLAYER + 12 * PATH_WAYPOINTS_PER_SLOT + 0].run_length = 3;

    unit &p1                                                                                          = fx.u(7, 1);
    p1.state                                                                                          = GM_STATE_GROUP_MARSHAL;
    p1.order                                                                                          = 9;
    p1.move_group_id                                                                                  = 0;
    p1.goal_x                                                                                         = 20;
    p1.goal_y                                                                                         = 21;
    p1.x                                                                                              = 180;
    p1.y                                                                                              = 181;
    p1.unit_proto_id                                                                                  = 40;
    p1.path_slot_id                                                                                   = 13;
    fx.path_buffers[player * PATH_WAYPOINTS_PER_PLAYER + 13 * PATH_WAYPOINTS_PER_SLOT + 0].run_length = 3;

    g_route_sort_leg_count_ret = 2; // leg_count=2>1 -- would satisfy the OTHER two conjuncts

    run(fx);

    ck_eq((uint32_t)u.goal_x, 20u, "T10: self.goal_x UNCHANGED -- move_group_id==0 suppresses the overwrite "
                                   "(0x00482da8-0x00482dac JZ 0x00482e6a)");
    ck_eq((uint32_t)u.goal_y, 21u, "T10: self.goal_y UNCHANGED");
    ck_eq((uint32_t)p1.goal_x, 20u, "T10: peer.goal_x UNCHANGED");
    ck_eq((uint32_t)p1.goal_y, 21u, "T10: peer.goal_y UNCHANGED");
    int ready_count = 0;
    for (auto &n : g_notify_calls)
        if (n.status == GM_NOTIFY_STATUS_WAVE_READY) ++ready_count;
    ck(ready_count == 2, "T10: the rest of the good path still runs normally (both members notified 100)");
}

// =====================================================================================================
// T11 -- multi-wave compaction: 2 members in 2 separate single-member waves (wave_rank 0 then 1),
// proving the compaction slides the SECOND wave's entry down to scratch[0] and each member is
// processed EXACTLY ONCE across the two group_move_order_commit calls (0x00482ec0-0x00482f1a).
// =====================================================================================================
void test_t11_wave_compaction(sim_fixture &fx) {
    begin(fx);

    // player MUST be < MAX_PLAYERS (8, i.e. valid range 0..7) -- fx.u()/units[] do the SAME
    // unchecked roster arithmetic the original ASM does (no bounds check, driven by an ambient
    // cur_player the game always keeps in range), so an out-of-range player here is a TEST bug, not
    // a reproduction of original behaviour: it walks off the end of the `units` heap allocation. Was
    // `player = 8` (the 9th player, out of range) -- caught by the ASan gate as a heap-buffer-overflow
    // in sim_fixture's units vector (2026-08-20, SIM1-G1). 7 is the highest valid player.
    const uint16_t player = 7, index = 1;
    unit          &u   = fx.u(player, index);
    fx.cur_unit_ptr    = &u;
    fx.view_cur_player = player;
    fx.view_cur_index  = index;

    u.order                  = 9;
    u.move_group_id          = 10;
    u.goal_x                 = 1;
    u.goal_y                 = 1;
    u.x                      = 1;
    u.y                      = 1;
    u.path_slot_id           = 0xff;
    u.activity_clock         = 5.0;
    fx.tick_budget           = 9.0;
    g_saved_passable_seed[1] = 1;

    unit &p5                 = fx.u(7, 5);
    p5.state                 = GM_STATE_GROUP_MARSHAL;
    p5.order                 = 9;
    p5.move_group_id         = 10;
    p5.goal_x                = 1;
    p5.goal_y                = 1;
    p5.x                     = 2;
    p5.y                     = 2;
    p5.path_slot_id          = 0xff;
    p5.activity_clock        = 6.0;
    g_saved_passable_seed[5] = 2;

    g_route_sort_leg_count_ret = 1;      // first wave has exactly 1 member
    g_route_sort_wave_ranks    = {0, 1}; // scratch[0]=self rank0, scratch[1]=peer rank1

    run(fx);

    std::vector<RegisterCall> want = {{7, 1}, {7, 5}};
    ck(g_register_calls.size() == want.size(), "T11: self then peer registered, in that order");
    ck(g_route_sort_calls.size() == 1 && g_route_sort_calls[0].member_count == 2,
       "T11: pathfind_route_leg_group_and_sort(member_count=2, ...)");
    ck(g_commit_calls.size() == 2, "T11: TWO waves -- two group_move_order_commit calls "
                                   "(0x00482f1a-0x00482f1e JG loops back)");
    if (g_commit_calls.size() == 2) {
        ck(g_commit_calls[0].leg_count == 1 && g_commit_calls[1].leg_count == 1,
           "T11: EACH wave commits exactly 1 member (rank 0 then rank 1, not merged as 2)");
        ck(g_commit_calls[0].ref_x == g_commit_calls[1].ref_x && g_commit_calls[0].ref_y == g_commit_calls[1].ref_y &&
               g_commit_calls[0].player == g_commit_calls[1].player &&
               g_commit_calls[0].move_group_id == g_commit_calls[1].move_group_id &&
               g_commit_calls[0].is_move_flag == g_commit_calls[1].is_move_flag &&
               g_commit_calls[0].attack_class == g_commit_calls[1].attack_class,
           "T11: ref_x/ref_y/player/move_group_id/is_move_flag/attack_class are IDENTICAL across both "
           "waves -- only leg_count/the scratch window differ per iteration");
    }
    ck_eq_d(u.activity_clock, 7.0, "T11: self processed EXACTLY ONCE (5.0+2.0, not +4.0 or unchanged)");
    ck_eq_d(p5.activity_clock, 8.0, "T11: peer processed EXACTLY ONCE (6.0+2.0) -- proves the compaction "
                                    "correctly slid scratch[1] into scratch[0] for the second commit");
    ck_eq((uint32_t)fx.passable[(1u << 8) | 1u], 1u, "T11: self passable restored");
    ck_eq((uint32_t)fx.passable[(2u << 8) | 2u], 2u, "T11: peer passable restored");
    ck_eq_d(fx.tick_budget, 0.0, "T11: tick_budget zeroed for self regardless of which wave processed it");
}

// =====================================================================================================
// T12 -- roster/passable non-corruption of an entirely untouched neighbour, one final dedicated check.
// =====================================================================================================
void test_t12_neighbour_non_corruption(sim_fixture &fx) {
    begin(fx);

    const uint16_t player = 0, index = 0;
    unit          &u   = fx.u(player, index);
    fx.cur_unit_ptr    = &u;
    fx.view_cur_player = player;
    fx.view_cur_index  = index;

    u.order                  = 9;
    u.move_group_id          = 1;
    u.goal_x                 = 1;
    u.goal_y                 = 1;
    u.x                      = 5;
    u.y                      = 5;
    u.path_slot_id           = 0xff;
    g_saved_passable_seed[0] = 3;

    // player MUST be < MAX_PLAYERS (8) -- same OOB-test-bug class as T11 above; was `player = 9`.
    // 7 keeps the guard on a different player from `u` (player 0), which is the point of the test.
    unit &guard                     = fx.u(7, 50);
    guard.order                     = 0x40;
    guard.state                     = 0x40;
    guard.goal_x                    = 9;
    guard.goal_y                    = 9;
    guard.target_ref                = 9;
    guard.target_index              = 9;
    guard.move_group_id             = 9;
    guard.path_slot_id              = 9;
    guard.path_blocked_retry_count  = 9;
    guard.activity_clock            = 999.0;
    guard.home_x                    = 9;
    guard.home_y                    = 9;
    fx.passable[(100u << 8) | 100u] = 0xEE;

    run(fx);

    ck(guard.order == 0x40 && guard.state == 0x40 && guard.goal_x == 9 && guard.goal_y == 9 &&
           guard.target_ref == 9 && guard.target_index == 9 && guard.move_group_id == 9 &&
           guard.path_slot_id == 9 && guard.path_blocked_retry_count == 9 && guard.home_x == 9 &&
           guard.home_y == 9,
       "T12: guard unit's every seeded field is untouched");
    ck_eq_d(guard.activity_clock, 999.0, "T12: guard unit's activity_clock untouched");
    ck_eq((uint32_t)fx.passable[(100u << 8) | 100u], 0xEEu, "T12: guard passable tile untouched");
    ck_eq((uint32_t)fx.passable[(5u << 8) | 5u], 3u, "T12: sanity -- the run DID restore self's own tile");
}

} // namespace

void run_unit_state_group_marshal_tests() {
    sim_fixture fx;
    test_t1_attack_target_dead_early_return(fx);
    test_t2_attack_target_alive_recompute(fx);
    test_t3_attack_building_skips_dead_check(fx);
    test_t4_peer_gather_plain_move(fx);
    test_t5_peer_gather_attack_twin(fx);
    test_t6_enter_storage_begin_excluded(fx);
    test_t7_bad_path_plain_move(fx);
    test_t8_bad_path_attack_redirect_vs_escalate(fx);
    test_t9_good_path_and_goal_overwrite(fx);
    test_t10_goal_overwrite_suppressed_by_zero_group(fx);
    test_t11_wave_compaction(fx);
    test_t12_neighbour_non_corruption(fx);
}

} // namespace mh::sim::test
