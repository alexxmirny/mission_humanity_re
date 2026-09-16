//
// ai/ai_group_form.cpp -- see ai_group_form.h. Translated from the DISASSEMBLY
// (tmp/decomp_ai/llm_strat_ai_group_form_{standby_from_pool3,surplus_from_pool4,patrol}_*.asm),
// re-exported after EN v223 so the four tuning constants appear by name.
//
#include "ai/ai_group_form.h"


namespace mh::ai {
namespace detail {

namespace {

// The goal values these three census on and stamp. No cfg::enum member is established for the
// `goal` column, so they are spelled as the literals the originals compare against.
constexpr int16_t GOAL_PATROL        = 0x05;
constexpr int16_t GOAL_SURPLUS       = 0x08;
constexpr int16_t GOAL_STANDBY       = 0x0a;
constexpr int32_t POOL_GROUP_PATROL  = 2; // ai_groups[2], form_patrol's source
constexpr int32_t POOL_GROUP_STANDBY = 3; // ai_groups[3]
constexpr int32_t POOL_GROUP_SURPLUS = 4; // ai_groups[4]

// The task codes each enqueues, in the order the original enqueues them. Names are read off
// llm_strat_ai_group_task_activate's own jump table @0x004eb0cd, not guessed.
constexpr uint16_t TASK_MUSTER_FROM_POOL   = 0x10;
constexpr uint16_t TASK_LOITER_WANDER      = 0x0e;
constexpr uint16_t TASK_RECRUIT_FROM_POOL3 = 0x12;
constexpr uint16_t TASK_RECRUIT_FROM_POOL4 = 0x13;
constexpr uint16_t TASK_ATTACK_NEAREST     = 0x15; // form_surplus_from_pool4's second task
constexpr uint16_t TASK_DRAIN_RESERVE      = 0x17; // form_standby_from_pool3's second task

// `true` as soon as ANY group carries `goal`. The bound is re-read every iteration because the
// original re-derives it inside the loop -- nothing in the loop can change it, but writing it
// hoisted would quietly diverge if a future caller made that false.
bool any_group_has_goal(const player_data &pd, int16_t goal, group_form_report &rep) {
    for (uint32_t g = 0; g < (uint32_t)pd.ai_group_count; ++g) { // JC = UNSIGNED below
        ++rep.census_scanned;
        if (pd.ai_groups[g].goal == goal) {
            ++rep.census_matches;
            return true;
        }
    }
    return false;
}

} // namespace

// ---- llm_strat_ai_group_form_standby_from_pool3 @0x004e722a -------------------------------------
//
// NOTE THE UNCHECKED group_create: the original stores through the returned index without testing
// it against -1 (contrast form_patrol @0x004e7b50, which does). A -1 here addresses
// ai_groups[-1] -- 0xa66 bytes before the array and still inside player_data. Preserved
// deliberately: it is reachable only when llm_strat_ai_group_create has no free slot, and "fixing"
// it would change what the shadow site compares. Same in form_surplus_from_pool4 below.
group_form_report form_standby_from_pool3(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                          int32_t player_id) {
    (void)v;
    group_form_report rep{};
    player_data      &pd = own.players[player_id];

    if (any_group_has_goal(pd, GOAL_STANDBY, rep)) { // JZ -> epilogue @0x004e724e
        rep.census_blocked = true;
        return rep;
    }
    // The pool must not be empty (CMP word [g3.member_count],0 / JZ @0x004e7273).
    //
    // ONE READ HERE, TWO IN THE ORIGINAL, and the equivalence rests on an invariant rather than on
    // the obvious argument. The original loads pool group 3's member_count at 0x004e7273 and AGAIN
    // at 0x004e729a, with the CALL to group_create in between; caching it is only safe if
    // group_create cannot write that address. It writes the NEW group's own member_count
    // (0x004d4dfd, addressed off the pre-increment ai_group_count), so the tempting claim -- "it
    // never touches a fixed pool address" -- is false when the new index HAPPENS to be 3, which is
    // exactly what llm_strat_spawn_ai_base's fourth create does. What actually holds is a floor:
    // spawn_ai_base seeds groups 0..4 before the AI's phase bits are set, and the only remover
    // (llm_strat_ai_unit_group_tick's reaper) scans from index 5, so ai_group_count is >= 5 at
    // every call reachable from here and the new index can never be 3 or 4. Same argument for the
    // pool-4 sibling below. (Adjudicated during the 2026-08-05 reimpl-verify pass, which corrected
    // the weaker framing this comment first carried.)
    const int16_t pool_members = pd.ai_groups[POOL_GROUP_STANDBY].member_count;
    if (pool_members == 0) {
        rep.pool_blocked = true;
        return rep;
    }

    const int32_t g      = gc.group_create(player_id);
    rep.group_index      = g;
    rep.formed           = true;
    pd.ai_groups[g].goal = GOAL_STANDBY;
    // MOV BX,[g3.member_count] @0x004e729a, then MOV [g.active_member_count],BX -- a 16-bit copy
    // through the same register the sub_code argument is then zero-extended from.
    pd.ai_groups[g].active_member_count = (uint16_t)pool_members;

    // The FIRST task carries the AI home tile as its anchor pair and the pool headcount as its
    // sub_code; the SECOND carries nothing at all. Argument order is the __watcall one --
    // EAX/EDX/EBX/ECX then five stack slots, with param_5 at the LOWEST address (the thunk
    // s_void_EAX_EDX_EBX_ECX_S4_S4_S4_S4_S4 places a4 at [esp+0], and the original pushes
    // ai_home_tile_x LAST).
    gc.group_task_enqueue(player_id, g, TASK_RECRUIT_FROM_POOL3, /*pending_param*/ 3,
                          (uint32_t)pd.ai_home_tile_x, (uint32_t)pd.ai_home_tile_y, 0, 0,
                          (uint16_t)pool_members);
    gc.group_task_enqueue(player_id, g, TASK_DRAIN_RESERVE, /*pending_param*/ 0, 0, 0, 0, 0, 0);
    rep.tasks_enqueued = 2;
    return rep;
}

// ---- llm_strat_ai_group_form_surplus_from_pool4 @0x004e72ef -------------------------------------
group_form_report form_surplus_from_pool4(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                          int32_t player_id) {
    group_form_report rep{};
    player_data      &pd = own.players[player_id];

    if (any_group_has_goal(pd, GOAL_SURPLUS, rep)) { // JZ -> epilogue @0x004e7313
        rep.census_blocked = true;
        return rep;
    }
    // MOVZX @0x004e7338 (so member_count is read UNSIGNED here, unlike the standby sibling's plain
    // 16-bit compare against zero) and then a SIGNED compare against the threshold, JL @0x004e7345.
    const int32_t pool_members = (int32_t)(uint16_t)pd.ai_groups[POOL_GROUP_SURPLUS].member_count;
    if (pool_members < *v.surplus_group_min_pool4) {
        rep.pool_blocked = true;
        return rep;
    }

    const int32_t g                     = gc.group_create(player_id); // unchecked -- see form_standby_from_pool3
    rep.group_index                     = g;
    rep.formed                          = true;
    pd.ai_groups[g].goal                = GOAL_SURPLUS;
    pd.ai_groups[g].active_member_count = (uint16_t)pool_members;

    gc.group_task_enqueue(player_id, g, TASK_RECRUIT_FROM_POOL4, /*pending_param*/ 3,
                          (uint32_t)pd.ai_home_tile_x, (uint32_t)pd.ai_home_tile_y, 0, 0,
                          (uint16_t)pool_members);
    gc.group_task_enqueue(player_id, g, TASK_ATTACK_NEAREST, /*pending_param*/ 0x183, 0, 0, 0, 0, 0);
    rep.tasks_enqueued = 2;
    return rep;
}

// ---- llm_strat_ai_group_form_patrol @0x004e7ad2 -------------------------------------------------
//
// The one of the three that COUNTS rather than searching, and the one that checks group_create.
group_form_report form_patrol(const ai_view &v, const ai_store &own, const ai_calls &gc,
                              int32_t player_id) {
    group_form_report rep{};
    player_data      &pd = own.players[player_id];

    // SETZ AL / MOVZX / ADD ECX,EAX @0x004e7af8-0x004e7afe -- no early exit, it is a census.
    int32_t patrol_groups = 0;
    for (uint32_t g = 0; g < (uint32_t)pd.ai_group_count; ++g) { // JC = UNSIGNED below
        ++rep.census_scanned;
        if (pd.ai_groups[g].goal == GOAL_PATROL) ++patrol_groups;
    }
    rep.census_matches = patrol_groups;
    // UNSIGNED, and this is the one place in the three where the two readings differ observably.
    // `JNC` at 0x004e7b25 is 0F 83, i.e. JAE -- taken when CF is clear, which after `CMP ECX,mem`
    // means UNSIGNED ECX >= mem. Written as a signed C++ `>=` (which is what this line was until
    // the 2026-08-05 reimpl-verify pass, where three independent reviewers flagged it), a
    // _G_LLM_STRAT_AI_PATROL_GROUP_MAX with its high bit set -- the -1 an "uncapped" mod would
    // reach for -- blocks the body permanently where the original would form a group every time.
    // The shipped value is 2, so the divergence is unreachable in a stock game and reachable in a
    // modded one; it costs two casts either way. The other three gates in this file were re-read at
    // the same time and are correctly signed: JL at 0x004e7345, JLE at 0x004e7b41, and the census
    // loops' JC, which is unsigned and is spelled with the uint32_t cast.
    if ((uint32_t)patrol_groups >= (uint32_t)*v.patrol_group_max) { // CMP/JNC @0x004e7b1f
        rep.census_blocked = true;
        return rep;
    }

    // STRICTLY GREATER than three times the group it is about to form (JLE returns @0x004e7b41).
    // `size * 3` is spelled SHL 2 / SUB in the original, which is the same value for any int.
    const int32_t pool_members = (int32_t)(uint16_t)pd.ai_groups[POOL_GROUP_PATROL].member_count;
    const int32_t size         = *v.patrol_group_size;
    if (pool_members <= size * 3) {
        rep.pool_blocked = true;
        return rep;
    }

    const int32_t g = gc.group_create(player_id);
    rep.group_index = g;
    if (g == -1) { // CMP EAX,-1 / JZ @0x004e7b50 -- only this one of the three tests it
        rep.create_failed = true;
        return rep;
    }
    rep.formed           = true;
    pd.ai_groups[g].goal = GOAL_PATROL;
    // MOV AX,[PATROL_GROUP_SIZE] @0x004e7b6a -- a 16-bit load of a 32-bit constant, so only the low
    // half of the tuning value reaches the field. Immaterial at the shipped value 3; reproduced as
    // the truncation it is rather than as a widening.
    pd.ai_groups[g].active_member_count = (uint16_t)size;

    // Both anchors are -1 here, not the home tile: this group musters wherever it already is.
    gc.group_task_enqueue(player_id, g, TASK_MUSTER_FROM_POOL, /*pending_param*/ 0x183,
                          (uint32_t)-1, (uint32_t)-1, 0, 0, (uint16_t)size);
    // param_9 IS TRUNCATED TO 16 BITS AND THAT IS THE ORIGINAL'S BEHAVIOUR, not this layer's:
    // _G_LLM_STRAT_AI_PATROL_WANDER_BOUNCES is 100000 and the original pushes the full dword, but
    // llm_strat_ai_group_task_enqueue loads it with MOV EBX,[ESP+0x18] and then stores only BX
    // (0x004d4ca9 / 0x004d4cad) into the task slot's 16-bit sub_code. So 100000 lands as -31072
    // either way; the committed prototype's uint16_t param_9 is that fact, not a narrowing here.
    gc.group_task_enqueue(player_id, g, TASK_LOITER_WANDER, /*pending_param*/ 0x193, (uint32_t)-1,
                          (uint32_t)-1, 0, 0, (uint16_t)*v.patrol_wander_bounces);
    rep.tasks_enqueued = 2;
    return rep;
}

} // namespace detail

void form_standby_from_pool3(int32_t player_id) {
    const ai_state st = state();
    (void)detail::form_standby_from_pool3(st.read, st.own, live_calls(), player_id);
}
void form_surplus_from_pool4(int32_t player_id) {
    const ai_state st = state();
    (void)detail::form_surplus_from_pool4(st.read, st.own, live_calls(), player_id);
}
void form_patrol(int32_t player_id) {
    const ai_state st = state();
    (void)detail::form_patrol(st.read, st.own, live_calls(), player_id);
}

// ---- the differential-oracle arms ---------------------------------------------------------------

} // namespace mh::ai
