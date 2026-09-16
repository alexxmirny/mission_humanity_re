//
// sim/sim_bldg_construct_finalize.cpp -- see sim_bldg_construct_finalize.h. Translated from the
// DISASSEMBLY (tmp/decomp/llm_bldg_construct_finalize_00462e66.asm), not from the Ghidra .c draft
// (the draft's overall shape reads correctly and is cross-checked against it below, but every
// literal, register mapping, and case grouping was re-derived from the raw opcodes per house rules).
//
#include "sim/sim_bldg_construct_finalize.h"

#include "addr/mh_calls.gen.h"  // mh::call::map_CreateBuilding / _link_to_network_if_adjacent /
                                // _notify_state_change / _ai_notify_bldg_constructed -- bound live
                                // in live_bldg_construct_finalize_calls()
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const bldg_construct_finalize_calls &live_bldg_construct_finalize_calls() {
    static const bldg_construct_finalize_calls gc = {
        MH_LIBMH_BIND(map_CreateBuilding),
        MH_LIBMH_BIND(llm_strat_bldg_link_to_network_if_adjacent),
        MH_LIBMH_BIND(llm_strat_bldg_notify_state_change),
        MH_LIBMH_BIND(llm_strat_ai_notify_bldg_constructed),
    };
    return gc;
}

namespace detail {

int32_t bldg_construct_finalize(const sim_view &v, const bldg_construct_finalize_calls &c,
                                uint32_t param_1, int32_t y_b, uint16_t player, char param_4,
                                uint32_t x_b, uint32_t building_id) {
    // 0x00462e8e-0x00462ea7: param_4 == 1 shrinks the GARAGE-family scan bound AND the final
    // buildings-roster scan bound by 9; every other value (including 0) leaves both unshrunk.
    const int32_t shrink = (param_4 == 1) ? 9 : 0;

    // 0x00462f45-0x004630ac: the category-specific sub-slot scan, dispatched on the building type's
    // jump table. sub_slot stays 0 (local_20's initial value, 0x00462e87) for the default/no-match
    // arm and is left unassigned by any case that finds nothing.
    int32_t sub_slot = 0;
    switch (v.cfg_buildings[building_id].type) {
        case BUILDING_TYPE_A_PRODUCTION:
        case BUILDING_TYPE_H_PRODUCTION:
            // 0x00462f6f-0x00462fa9: productions[player][1..PRODUCTIONS_PER_PLAYER).
            for (int32_t i = 1; i < v.caps.productions; ++i) {
                if (v.productions[player * v.caps.productions + i].b_index == 0) {
                    sub_slot = i;
                    break;
                }
            }
            break;
        case BUILDING_TYPE_A_MINE:
        case BUILDING_TYPE_H_MINE:
            // 0x00462feb-0x00463022: mines[player][1..MINES_PER_PLAYER).
            for (int32_t i = 1; i < v.caps.mines; ++i) {
                if (v.mines[player * v.caps.mines + i].b_index == 0) {
                    sub_slot = i;
                    break;
                }
            }
            break;
        // 0x004630a5: no scan at all -- sub_slot = 1 unconditionally.
        case BUILDING_TYPE_A_PLANT:
        case BUILDING_TYPE_A_COLONY:
        case BUILDING_TYPE_A_MOTHER:
        case BUILDING_TYPE_A_MAIN_BASE:
        case BUILDING_TYPE_A_RELAY:
        case BUILDING_TYPE_A_SILOS:
        case BUILDING_TYPE_A_CIVIL:
        case BUILDING_TYPE_H_PLANT:
        case BUILDING_TYPE_H_COLONY:
        case BUILDING_TYPE_H_MOTHER:
        case BUILDING_TYPE_H_MAIN_BASE:
        case BUILDING_TYPE_H_RELAY:
        case BUILDING_TYPE_H_SILOS:
        case BUILDING_TYPE_H_CIVIL:
            sub_slot = 1;
            break;
        case BUILDING_TYPE_A_TURRET:
        case BUILDING_TYPE_H_TURRET:
            // 0x00462fae-0x00462fe6: turrets[player][1..TURRETS_PER_PLAYER).
            for (int32_t i = 1; i < v.caps.turrets; ++i) {
                if (v.turrets[player * v.caps.turrets + i].b_index == 0) {
                    sub_slot = i;
                    break;
                }
            }
            break;
        case BUILDING_TYPE_A_BARRAKS:
        case BUILDING_TYPE_A_GARAGE:
        case BUILDING_TYPE_A_AIRFIELD:
        case BUILDING_TYPE_A_HELIPAD:
        case BUILDING_TYPE_A_PORT:
        case BUILDING_TYPE_A_SHUTTLE:
        case BUILDING_TYPE_H_BARRACKS:
        case BUILDING_TYPE_H_GARAGE:
        case BUILDING_TYPE_H_AIRFIELD:
        case BUILDING_TYPE_H_HELIPAD:
        case BUILDING_TYPE_H_PORT:
        case BUILDING_TYPE_H_SHUTTLE:
            // 0x00463027-0x00463068: unit_storage[player][1..(STORAGE_PER_PLAYER-shrink)) -- the
            // bound subtracts `shrink`, unlike the `_enqueue` sibling's hardcoded 16.
            for (int32_t i = 1; i < v.caps.storage - shrink; ++i) {
                if (v.storage[player * v.caps.storage + i].b_index == 0) {
                    sub_slot = i;
                    break;
                }
            }
            break;
        case BUILDING_TYPE_A_LAB:
        case BUILDING_TYPE_H_LAB:
            // 0x0046306a-0x004630a3: labs[player][1..LABS_PER_PLAYER).
            for (int32_t i = 1; i < v.caps.labs; ++i) {
                if (v.labs[player * v.caps.labs + i].b_index == 0) {
                    sub_slot = i;
                    break;
                }
            }
            break;
        default:
            // caseD_11 (0x004630ac): every unmatched type index, and every out-of-range one
            // (DEC AL > 0x25), lands here -- a no-op, sub_slot stays 0.
            break;
    }

    if (sub_slot == 0) {
        // 0x004630b2-0x004630cf: no category slot at all -- skip the roster scan entirely.
        c.ai_notify_bldg_constructed(player, x_b, 0, building_id, y_b, 2);
        return 0;
    }

    // 0x004630d4-0x00463164: the final buildings-roster scan, bound (BUILDINGS_PER_PLAYER-shrink).
    for (int32_t roster_slot = 1; roster_slot < v.caps.buildings - shrink; ++roster_slot) {
        if (building_of(v, player, roster_slot).building_id == 0) {
            // 0x0046310f-0x00463126: (player, roster_slot, x_b, y_b, building_id, param_1, sub_slot)
            // -- param_1 is forwarded straight through, untouched, per the committed prototype.
            c.create_building(player, (uint32_t)roster_slot, x_b, y_b, (int32_t)building_id, param_1,
                              (uint32_t)sub_slot);
            // 0x0046312b-0x00463137: sibling unit, reached through mh::call:: (header banner).
            c.link_to_network_if_adjacent(player, (uint32_t)roster_slot);
            // 0x0046313a-0x00463143.
            c.notify_state_change(player, (uint32_t)roster_slot);
            // 0x00463145-0x0046315a: success code 0, roster_slot as the notify's slot argument.
            c.ai_notify_bldg_constructed(player, x_b, (uint32_t)roster_slot, building_id, y_b, 0);
            return roster_slot;
        }
    }

    // 0x00463164-0x0046317a: sub-slot existed but the roster scan exhausted -- SAME failure notify
    // as the no-sub-slot case above.
    c.ai_notify_bldg_constructed(player, x_b, 0, building_id, y_b, 2);
    return 0;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

int32_t bldg_construct_finalize(uint32_t param_1, int32_t y_b, uint16_t player, char param_4,
                                uint32_t x_b, uint32_t building_id) {
    const sim_view v = state().read;
    return detail::bldg_construct_finalize(v, live_bldg_construct_finalize_calls(), param_1, y_b,
                                           player, param_4, x_b, building_id);
}


} // namespace mh::sim
