//
// sim/sim_unit_recruit.cpp -- see sim_unit_recruit.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_unit_recruit_00463ec8.asm), not from Ghidra's C: the draft's overall shape (nested
// if/else-if over cVar2, a single-exit `return local_28`) reads correctly and was used as a guide, but
// every branch, every housing-stats field, and every return-code literal was re-walked against the
// raw CMP/JC/JBE/JMP targets rather than trusted from the .c.
//
#include "sim/sim_unit_recruit.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const unit_recruit_calls &live_unit_recruit_calls() {
    static const unit_recruit_calls c = {
        MH_LIBMH_BIND(llm_strat_unit_housing_count_add),
        MH_LIBMH_BIND(llm_strat_unit_spawn_docked),
        MH_LIBMH_BIND(llm_strat_unit_add_docked),
        MH_LIBMH_BIND(llm_strat_ai_notify_unit_lifecycle),
    };
    return c;
}

namespace detail {

int32_t unit_recruit(const sim_view &v, sim_store &own, const unit_recruit_calls &c, uint32_t player,
                     uint32_t unit_type_id) {
    // ---- (1) order-queue-length cap (0x00463ee5-0x00463f03) --------------------------------------
    // units[player][0].order is (ab)used as a per-player counter here -- see the header hazard note.
    // An ORDERED `+1 > 0x5a` compare (unsigned in the original: MOVZX then plain CMP/JL on a 16-bit
    // field that can never reach a value where signedness would matter).
    if ((int32_t)unit_of(v, player, 0).order + 1 > RECRUIT_ORDER_QUEUE_CAP)
        return RECRUIT_ERR_QUEUE_FULL;

    const uint32_t type = v.cfg_units[unit_type_id].type;

    // ---- (2) per-category housing cap (0x00463f08-0x00464016) ------------------------------------
    // Chain read directly off the CMP/JC/JBE targets; see the header for the full derivation and the
    // cfg_enum_E_UNIT_TYPE member names (UNIT_TYPE_A_WALKER/A_HELI/A_PLANE/A_HELI_MOTHER, all already
    // pinned in sim_unit_type_predicates.h / sim_order_enqueue.h).
    if (type < UNIT_TYPE_A_HELI) {
        if (type != UNIT_TYPE_UNDEFINED) {
            if (type < UNIT_TYPE_A_WALKER) {
                // SOLDIER housing: cap compared against used PLUS this unit type's own soldier_count
                // (a single recruit can occupy more than one soldier slot).
                if (v.unit_housing[player].cap_prev_soldiers <
                    v.unit_housing[player].used_soldiers + v.cfg_units[unit_type_id].soldier_count) {
                    return RECRUIT_ERR_SOLDIER_CAP;
                }
            } else {
                // VEHICLE housing (A_WALKER..A_HELI, exclusive): simple used>=cap, no addend.
                if (v.unit_housing[player].cap_prev_vehicles <= v.unit_housing[player].used_vehicles)
                    return RECRUIT_ERR_VEHICLE_CAP;
            }
        }
        // type == UNDEFINED: no housing check at all.
    } else if (type < UNIT_TYPE_A_PLANE) {
        // HELI housing.
        if (v.unit_housing[player].cap_prev_helis <= v.unit_housing[player].used_helis)
            return RECRUIT_ERR_HELI_CAP;
    } else if (type < UNIT_TYPE_A_HELI_MOTHER) {
        // PLANE housing.
        if (v.unit_housing[player].cap_prev_planes <= v.unit_housing[player].used_planes)
            return RECRUIT_ERR_PLANE_CAP;
    }
    // type >= A_HELI_MOTHER: no housing check at all.

    // ---- (3) soldier-headcount cap, INDEPENDENT of the category branch above (0x00464016-
    // 0x00464065) -- runs for every unit type, but is a no-op unless Unit[unit_type_id].soldier_count
    // != 0. v.soldiers[player][0].owner_unit reuses slot 0 as a per-player used-slot-count scalar,
    // same idiom as units[player][0].order above -- see the header's DECLARED NEED 2 for the
    // llm_unit_create_soldier cross-check.
    const int32_t soldier_count = v.cfg_units[unit_type_id].soldier_count;
    if (soldier_count != 0) {
        const int32_t projected =
            (int32_t)(uint32_t)(uint16_t)v.soldiers[player * v.caps.soldiers + 0].owner_unit +
            soldier_count + 1;
        if (projected >= RECRUIT_SOLDIER_HEADROOM_CAP) {
            // Race-dependent reason code -- see the header's DECLARED NEED 3 (_G_LLM_STRAT_PLAYER_RACE
            // is a plain, un-indexed global scalar, not per-player).
            return (*v.player_race == 1) ? RECRUIT_ERR_SOLDIER_HEADCOUNT_RACE1
                                         : RECRUIT_ERR_SOLDIER_HEADCOUNT_OTHER;
        }
    }

    // ---- (4) grant path (0x0046406a-0x00464106) ---------------------------------------------------
    // Order matters and is preserved exactly: housing_count_add FIRST, THEN the order-queue-length
    // increment, THEN the move_op_code dispatch.
    c.unit_housing_count_add((int32_t)player, (int32_t)unit_type_id);
    own.unit_at(player, 0).order =
        (uint16_t)(own.unit_at(player, 0).order + UNIT_STATE_STOP_TO_DEFAULT);

    const uint16_t unit_type_id16 = (uint16_t)unit_type_id;
    const uint8_t  move_op_code   = v.cfg_units[unit_type_id].move_op_code;

    int32_t unit_id;
    if (move_op_code == MOVE_OP_CODE_GROUND) {
        unit_id = c.unit_spawn_docked(unit_type_id16, (uint16_t)player, 0u);
    } else {
        unit_id = c.unit_add_docked(unit_type_id, (uint16_t)player, 0u);
    }
    if (unit_id == 0) return RECRUIT_ERR_SPAWN_FAILED;

    // Identical on both paths -- mode 4, "unit just created" (the AI notes).
    c.ai_notify_unit_lifecycle((uint16_t)player, unit_type_id16, (uint32_t)unit_id, 4u);
    return RECRUIT_OK;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

int32_t unit_recruit(uint32_t player, uint32_t unit_type_id) {
    sim_state st = state();
    return detail::unit_recruit(st.read, st.own, live_unit_recruit_calls(), player, unit_type_id);
}


} // namespace mh::sim
