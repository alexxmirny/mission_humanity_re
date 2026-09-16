//
// ai/ai_group_task_machine.cpp -- see ai_group_task_machine.h. Translated from the DISASSEMBLY
// (tmp/decomp_ai/llm_strat_ai_group_task_{activate_004eb131,step_004e96b1}.asm) plus a direct read
// of the two jump tables, which the listings do not carry.
//
#include "ai/ai_group_task_machine.h"


namespace mh::ai {
namespace detail {

namespace {

// The highest task code either table indexes. Both bodies test it UNSIGNED and on 16 bits
// (`CMP BX,0x18` / `JA` at 0x004eb185, `CMP DX,0x18` / `JA` at 0x004e96ff) -- see the header.
constexpr uint16_t TASK_CODE_MAX = 0x18;

// ---- ACTIVATION TABLE @0x004eb0cd, 25 dwords, read out of the image ------------------------------
//
// Codes 0, 1 and 0x0a point at the function's own epilogue (0x004eb241), i.e. they are in range and
// do nothing. Code 0x13 is the only arm whose CALL falls THROUGH into that epilogue rather than
// having its own POP/RET triple -- the same instruction-sharing the three group formers use, and
// behaviourally identical.
enum activate_arm : uint8_t {
    ARM_NONE = 0,
    ARM_RALLY_FORMUP,
    ARM_ADVANCE_TO_ANCHOR,
    ARM_DISPERSE_PASSABLE,
    ARM_PATROL_SHUTTLE,
    ARM_NUDGE_STRAGGLERS,
    ARM_WAIT,
    ARM_RECALL_HOME,
    ARM_RECRUIT_FROM_STORAGE,
    ARM_HOLD,
    ARM_SCATTER_RANDOM,
    ARM_LOITER_WANDER,
    ARM_MUSTER_FROM_POOL,
    ARM_RECRUIT_FROM_POOL3,
    ARM_RECRUIT_FROM_POOL4,
    ARM_DISBAND,
    ARM_ATTACK_NEAREST_DEFENDED,
    ARM_ENGAGE_TARGET,
    ARM_DRAIN_RESERVE_ATTACK,
    ARM_ATTACK_RANDOM_TARGET,
};

// Index = task code. The right-hand comment is the table slot's raw target, so this array can be
// diffed against a fresh read of 0x004eb0cd without decoding anything.
constexpr activate_arm ACTIVATE_ARMS[TASK_CODE_MAX + 1] = {
    /* 0x00 */ ARM_NONE,                    // 0x004eb241 (epilogue)
    /* 0x01 */ ARM_NONE,                    // 0x004eb241 (epilogue)
    /* 0x02 */ ARM_RALLY_FORMUP,            // 0x004eb1d9
    /* 0x03 */ ARM_ADVANCE_TO_ANCHOR,       // 0x004eb1f4
    /* 0x04 */ ARM_DISPERSE_PASSABLE,       // 0x004eb1fd
    /* 0x05 */ ARM_PATROL_SHUTTLE,          // 0x004eb1c7
    /* 0x06 */ ARM_NUDGE_STRAGGLERS,        // 0x004eb1eb
    /* 0x07 */ ARM_WAIT,                    // 0x004eb206
    /* 0x08 */ ARM_RECALL_HOME,             // 0x004eb1be
    /* 0x09 */ ARM_RECRUIT_FROM_STORAGE,    // 0x004eb218
    /* 0x0a */ ARM_NONE,                    // 0x004eb241 (epilogue)
    /* 0x0b */ ARM_HOLD,                    // 0x004eb20f
    /* 0x0c */ ARM_ADVANCE_TO_ANCHOR,       // 0x004eb1f4 -- shares 0x03's arm
    /* 0x0d */ ARM_SCATTER_RANDOM,          // 0x004eb1e2
    /* 0x0e */ ARM_LOITER_WANDER,           // 0x004eb1d0
    /* 0x0f */ ARM_MUSTER_FROM_POOL,        // 0x004eb22a
    /* 0x10 */ ARM_MUSTER_FROM_POOL,        // 0x004eb22a
    /* 0x11 */ ARM_MUSTER_FROM_POOL,        // 0x004eb22a
    /* 0x12 */ ARM_RECRUIT_FROM_POOL3,      // 0x004eb233
    /* 0x13 */ ARM_RECRUIT_FROM_POOL4,      // 0x004eb23c (falls through into the epilogue)
    /* 0x14 */ ARM_DISBAND,                 // 0x004eb221
    /* 0x15 */ ARM_ATTACK_NEAREST_DEFENDED, // 0x004eb1a3
    /* 0x16 */ ARM_ENGAGE_TARGET,           // 0x004eb19a
    /* 0x17 */ ARM_DRAIN_RESERVE_ATTACK,    // 0x004eb1b5
    /* 0x18 */ ARM_ATTACK_RANDOM_TARGET,    // 0x004eb1ac
};

// ---- STEP TABLE @0x004e964d, 25 dwords, read out of the image ------------------------------------
enum step_arm : uint8_t {
    STEP_RETURN_ZERO = 0,  // 0x004e97dc: XOR EBX,EBX, return 0
    STEP_INDETERMINATE,    // 0x004e9892: MOV EAX,EBX with EBX never written -- see the header
    STEP_ARRIVAL,          // 0x004e97e3: group_check_arrival_status
    STEP_SCAN_HOSTILE_40,  // 0x004e97f9: group_area_scan_hostile(owner_mask 0x40)
    STEP_SCAN_HOSTILE_C0,  // 0x004e9816: group_area_scan_hostile(owner_mask 0xc0)
    STEP_DEQUEUE_RETURN_1, // 0x004e97d2: EBX = 1, then dequeue
    STEP_SETTLED,          // 0x004e981d: group_all_units_settled
    STEP_NEAR_CENTROID,    // 0x004e97ee: group_check_unit_near_centroid
    STEP_DOCK_RETURN_1,    // 0x004e977d: the dock arm that DISCARDS the busy result, returns 1
    STEP_DOCK_RETURN_FREE, // 0x004e9714: the dock arm that returns (busy == 0)
    STEP_TARGET_ALIVE,     // 0x004e9828: the engage arm
};

constexpr step_arm STEP_ARMS[TASK_CODE_MAX + 1] = {
    /* 0x00 */ STEP_RETURN_ZERO,      // 0x004e97dc
    /* 0x01 */ STEP_INDETERMINATE,    // 0x004e9892
    /* 0x02 */ STEP_ARRIVAL,          // 0x004e97e3
    /* 0x03 */ STEP_ARRIVAL,          // 0x004e97e3
    /* 0x04 */ STEP_ARRIVAL,          // 0x004e97e3
    /* 0x05 */ STEP_RETURN_ZERO,      // 0x004e97dc
    /* 0x06 */ STEP_SCAN_HOSTILE_40,  // 0x004e97f9
    /* 0x07 */ STEP_SCAN_HOSTILE_C0,  // 0x004e9816
    /* 0x08 */ STEP_RETURN_ZERO,      // 0x004e97dc
    /* 0x09 */ STEP_RETURN_ZERO,      // 0x004e97dc
    /* 0x0a */ STEP_DEQUEUE_RETURN_1, // 0x004e97d2
    /* 0x0b */ STEP_SETTLED,          // 0x004e981d
    /* 0x0c */ STEP_ARRIVAL,          // 0x004e97e3
    /* 0x0d */ STEP_NEAR_CENTROID,    // 0x004e97ee
    /* 0x0e */ STEP_RETURN_ZERO,      // 0x004e97dc
    /* 0x0f */ STEP_DOCK_RETURN_1,    // 0x004e977d
    /* 0x10 */ STEP_DOCK_RETURN_1,    // 0x004e977d
    /* 0x11 */ STEP_DOCK_RETURN_1,    // 0x004e977d
    /* 0x12 */ STEP_DOCK_RETURN_FREE, // 0x004e9714
    /* 0x13 */ STEP_DOCK_RETURN_FREE, // 0x004e9714
    /* 0x14 */ STEP_RETURN_ZERO,      // 0x004e97dc
    /* 0x15 */ STEP_RETURN_ZERO,      // 0x004e97dc
    /* 0x16 */ STEP_TARGET_ALIVE,     // 0x004e9828
    /* 0x17 */ STEP_RETURN_ZERO,      // 0x004e97dc
    /* 0x18 */ STEP_RETURN_ZERO,      // 0x004e97dc
};

// The two owner masks the hostile area scan is called with (MOV EBX,0x40 @0x004e97f9,
// MOV EBX,0xc0 @0x004e9816). Only the low byte reaches the callee.
constexpr uint8_t SCAN_MASK_ENEMY      = 0x40;
constexpr uint8_t SCAN_MASK_ENEMY_WIDE = 0xc0;

} // namespace

group_task_report group_task_activate(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                      int32_t player_id, int32_t group_index) {
    group_task_report rep{};
    unit_group       &grp = own.players[player_id].ai_groups[group_index];

    // The three UNCONDITIONAL stamps, in the original's order and BEFORE the bound test -- see the
    // header, point 3. task_start_time is a float widened to a double (FLD float / FSTP double at
    // 0x004eb168-0x004eb16e), not an eight-byte load.
    grp.current_param   = grp.pending_param;                     // 0x004eb15a/0x004eb161
    grp.task_start_time = (double)v.players[player_id].ai_clock; // 0x004eb168/0x004eb16e
    grp.active_flag     = 1;                                     // 0x004eb175
    rep.stamped         = true;

    const uint16_t code = (uint16_t)grp.task_code; // MOV BX,word [..+0x2a] @0x004eb17d
    rep.task_code       = (int32_t)code;
    if (code > TASK_CODE_MAX) { // CMP BX,0x18 / JA @0x004eb185 -- UNSIGNED, 16-bit
        rep.out_of_range = true;
        return rep;
    }

    const activate_arm arm = ACTIVATE_ARMS[code];
    if (arm == ARM_NONE) { // codes 0, 1, 0x0a: in range, table points at the epilogue
        rep.table_no_op = true;
        return rep;
    }
    rep.dispatched = true;
    switch (arm) {
        case ARM_RALLY_FORMUP: gc.group_task_rally_formup(player_id, group_index); break;
        case ARM_ADVANCE_TO_ANCHOR: gc.group_task_advance_to_anchor(player_id, group_index); break;
        case ARM_DISPERSE_PASSABLE: gc.group_task_disperse_passable((uint32_t)player_id, group_index); break;
        case ARM_PATROL_SHUTTLE: gc.group_task_patrol_shuttle(player_id, group_index); break;
        case ARM_NUDGE_STRAGGLERS: gc.group_task_nudge_stragglers((uint32_t)player_id, group_index); break;
        case ARM_WAIT: gc.group_task_wait(); break; // no-op stub @0x004eaf67, no args
        case ARM_RECALL_HOME: gc.group_task_recall_home(player_id, group_index); break;
        case ARM_RECRUIT_FROM_STORAGE: gc.group_task_recruit_from_storage((uint32_t)player_id, group_index); break;
        case ARM_HOLD: gc.group_task_hold(); break; // no-op stub @0x004eab33, no args
        case ARM_SCATTER_RANDOM: gc.group_task_scatter_random(player_id, group_index); break;
        case ARM_LOITER_WANDER: gc.group_task_loiter_wander(player_id, group_index); break;
        case ARM_MUSTER_FROM_POOL: gc.group_task_muster_from_pool((uint32_t)player_id, group_index); break;
        case ARM_RECRUIT_FROM_POOL3: gc.group_task_recruit_from_pool3((uint32_t)player_id, group_index); break;
        case ARM_RECRUIT_FROM_POOL4: gc.group_task_recruit_from_pool4((uint32_t)player_id, group_index); break;
        case ARM_DISBAND: gc.group_task_disband((uint32_t)player_id, group_index); break;
        case ARM_ATTACK_NEAREST_DEFENDED: gc.group_task_attack_nearest_defended(player_id, group_index); break;
        case ARM_ENGAGE_TARGET: gc.group_task_engage_target(player_id, group_index); break;
        case ARM_DRAIN_RESERVE_ATTACK: gc.group_task_drain_reserve_attack((uint32_t)player_id, group_index); break;
        case ARM_ATTACK_RANDOM_TARGET: gc.group_task_attack_random_target(player_id, group_index); break;
        case ARM_NONE: break; // unreachable: handled above
    }
    return rep;
}

group_task_report group_task_step(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                  uint32_t player_id, int32_t group_index,
                                  uint32_t indeterminate_ret) {
    (void)v;
    group_task_report rep{};
    unit_group       &grp = own.players[player_id].ai_groups[group_index];

    // CMP word [..+0x12],0 / JNZ @0x004e96e4 -- an IDLE group (no task records at all) answers 1,
    // i.e. "re-enter me", without touching anything. The caller's loop then re-activates it.
    if (grp.task_queue_count == 0) {
        rep.idle = true;
        rep.ret  = 1;
        return rep;
    }

    const uint16_t code = (uint16_t)grp.task_code; // MOV DX,word [..+0x2a] @0x004e96f8
    rep.task_code       = (int32_t)code;
    if (code > TASK_CODE_MAX) { // CMP DX,0x18 / JA @0x004e96ff -- UNSIGNED, and it lands on caseD_1
        rep.indeterminate = true;
        rep.ret           = indeterminate_ret;
        return rep;
    }

    switch (STEP_ARMS[code]) {
        case STEP_INDETERMINATE: // task code 1. Same arm as out-of-range; see the header.
            rep.indeterminate = true;
            rep.ret           = indeterminate_ret;
            return rep;

        case STEP_RETURN_ZERO: // XOR EBX,EBX @0x004e97dc
            rep.ret = 0;
            return rep;

        case STEP_DEQUEUE_RETURN_1: // MOV EBX,1 @0x004e97d2, then the shared dequeue tail
            gc.group_task_dequeue((int32_t)player_id, group_index);
            rep.dequeued = true;
            rep.ret      = 1;
            return rep;

        case STEP_ARRIVAL:
        case STEP_NEAR_CENTROID:
        case STEP_SCAN_HOSTILE_40:
        case STEP_SCAN_HOSTILE_C0:
        case STEP_SETTLED: {
            // The four PROGRESS PREDICATES all join LAB_004e9807: `MOV EBX,EAX; TEST EAX,EAX; JZ
            // caseD_1` -- a zero answer returns 0 and leaves the task running, a non-zero answer falls
            // into the shared dequeue tail at LAB_004e9889 and is itself the return value. Note it is
            // the PREDICATE's value that comes back, not a normalised 1.
            int32_t r = 0;
            switch (STEP_ARMS[code]) {
                case STEP_ARRIVAL:
                    r = gc.group_check_arrival_status(player_id, group_index);
                    break;
                case STEP_NEAR_CENTROID:
                    // Returns 1 when NO member is near the centroid -- the negation of its name. Its Ghidra
                    // return type was `void` until EN v225 (ghidra_findings 2026-08-05-1343-2).
                    r = gc.group_check_unit_near_centroid((int32_t)player_id, group_index);
                    break;
                case STEP_SCAN_HOSTILE_40:
                    r = gc.group_area_scan_hostile(player_id, group_index, SCAN_MASK_ENEMY);
                    break;
                case STEP_SCAN_HOSTILE_C0:
                    r = gc.group_area_scan_hostile(player_id, group_index, SCAN_MASK_ENEMY_WIDE);
                    break;
                default: // STEP_SETTLED. The callee sets the FULL dword to 0 or 1 (0x004d6bdb / the
                         // fall-through at 0x004d6be0), so the u8 marshalling loses nothing.
                    r = (int32_t)gc.group_all_units_settled((int32_t)player_id, group_index);
                    break;
            }
            if (r == 0) {
                rep.ret = 0;
                return rep;
            }
            gc.group_task_dequeue((int32_t)player_id, group_index);
            rep.dequeued = true;
            rep.ret      = (uint32_t)r;
            return rep;
        }

        case STEP_DOCK_RETURN_FREE: // codes 0x12, 0x13 -- caseD_12 @0x004e9714
        case STEP_DOCK_RETURN_1: {  // codes 0x0f, 0x10, 0x11 -- caseD_f @0x004e977d
            // BOTH arms call dock_slot_is_busy(player, group) FIRST and unconditionally; the difference
            // is that caseD_f throws the answer away (EAX is overwritten by MOV EAX,ESI at 0x004e9786)
            // while caseD_12 spills it to [EBP-0x10] and returns (busy == 0). Reproducing the discarded
            // call matters: the callee is not pure.
            const int32_t busy = gc.dock_slot_is_busy((int32_t)player_id, group_index);
            // reinforce_pending (+0x06) + active_sub_code (+0x2d), both zero-extended from 16 bits.
            // NON-ZERO -> clear active_flag so the caller re-activates the task; ZERO -> dequeue it, and
            // caseD_12 only dequeues when the dock is also free.
            const uint32_t pending = (uint32_t)(uint16_t)grp.reinforce_pending +
                                     (uint32_t)(uint16_t)grp.active_sub_code;
            if (pending != 0) {
                grp.active_flag  = 0; // MOV byte [..+0x2c],0 @0x004e9756 / 0x004e97b6
                rep.cleared_flag = true;
            } else if (STEP_ARMS[code] == STEP_DOCK_RETURN_1 || busy == 0) {
                gc.group_task_dequeue((int32_t)player_id, group_index);
                rep.dequeued = true;
            }
            rep.ret = (STEP_ARMS[code] == STEP_DOCK_RETURN_1) ? 1u : (busy == 0 ? 1u : 0u);
            return rep;
        }

        case STEP_TARGET_ALIVE: { // code 0x16 -- caseD_16 @0x004e9828
            // The gate is resolved_target_INDEX (+0x22), and the aliveness call takes active_param_a /
            // active_param_b (+0x2f / +0x33) -- NOT resolved_target_ref / _index. That pairing looks
            // like a transposition and is not one: 0x004e9831/0x004e9837 load +0x33 into EDX and +0x2f
            // into EAX, and 0x004e9828 tests +0x22.
            int32_t done;
            if (grp.resolved_target_index == 0) {
                done = 1; // MOV EBX,1 @0x004e984e
            } else {
                done = gc.target_ref_is_alive((uint32_t)grp.active_param_a, grp.active_param_b) ? 0 : 1;
            }
            if (done == 0) { // TEST EBX,EBX / JZ caseD_1 @0x004e9853
                rep.ret = 0;
                return rep;
            }
            grp.resolved_target_ref   = 0; // 0x004e9873
            grp.resolved_target_index = 0; // 0x004e987e
            rep.cleared_target        = true;
            gc.group_task_dequeue((int32_t)player_id, group_index);
            rep.dequeued = true;
            rep.ret      = (uint32_t)done;
            return rep;
        }
    }
    return rep; // unreachable -- every enumerator returns above
}

} // namespace detail

void group_task_activate(int32_t player_id, int32_t group_index) {
    const ai_state st = state();
    (void)detail::group_task_activate(st.read, st.own, live_calls(), player_id, group_index);
}

uint32_t group_task_step(uint32_t player_id, int32_t group_index) {
    const ai_state st = state();
    return detail::group_task_step(st.read, st.own, live_calls(), player_id, group_index, 0u).ret;
}

// ---- the differential-oracle arms ---------------------------------------------------------------

} // namespace mh::ai
