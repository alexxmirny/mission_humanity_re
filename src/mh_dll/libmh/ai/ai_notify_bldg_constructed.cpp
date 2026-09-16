//
// ai/ai_notify_bldg_constructed.cpp -- see ai_notify_bldg_constructed.h. Translated from the
// DISASSEMBLY (tmp/decomp/llm_strat_ai_notify_bldg_constructed_004daec2.asm), not from Ghidra's .c.
//
// REGISTER MAPPING, off the .asm header (not the .c's inferred signature, per the calling
// convention __mh_watcall_ecx_ebx_volatile): EAX=player, ECX=x, EDX=order_id (param_3), EBX=
// building_id, Stack[0x4]=y (first stack arg -> EBP+0x8), Stack[0x8]=mode (second stack arg ->
// EBP+0xc). The prologue immediately re-shuffles all six into ESI/EDI/two locals/EBX/EBX-reload,
// which the body below undoes by naming them once at entry.
//
// The record-base multiply `player * 0x288fc` (SHL 2 / ADD / SHL 7 / SUB / SHL 2 / SHL 6 / ADD) is
// re-emitted by the compiler at FIVE separate points in the original rather than cached in a
// register across the whole function (0x004daee1, 0x004daf41 [the other-player loop, a DIFFERENT
// index], 0x004daf77, 0x004daff3 [the mode-0 queue loop condition, re-evaluated every iteration],
// 0x004db091, 0x004db022, 0x004db142, 0x004db1f5) -- that is Watcom not hoisting a common
// subexpression across the intervening branches/calls, not five different quantities. Verified
// field-for-field against addr/mh_structs.gen.h's static_asserts (every absolute displacement used
// below matches a named field's offsetof exactly): ai_enabled=+0x18, ai_turret_rescan_pending=
// +0x38, resource_spent=+0x10054 (elements ARE resource id-1, per that field's own comment),
// is_alien_race=+0x104c0, ai_mother_building_type=+0x288b8, ai_build_candidate_primary=+0x288bc,
// ai_bldg_queue_count=+0x2572c / ai_bldg_queue=+0x25730 (element stride 0x12, matching
// mh_llm_strat_ai_bldg_queue_entry), ai_resource_site_count=+0x25c78 / ai_resource_sites=+0x25c7c
// (element stride 0xa, matching mh_llm_strat_ai_resource_site -- status at +0x4, build_tile_x at
// +0x6, build_tile_y at +0x8).
//
#include "ai/ai_notify_bldg_constructed.h"

#include "ai/ai_queue_reconcile.h" // QUEUE_STATUS_COMMITTED_CONSTRUCTION (0x81) -- one definition
#include "ai/ai_queue_release.h"   // QUEUE_STATUS_REMOVED (0x40) -- likewise

namespace mh::ai {
namespace detail {

void notify_bldg_constructed(const ai_view &v, const ai_store &own, const ai_calls &gc,
                             uint32_t player, uint32_t x, int32_t order_id, uint32_t building_id,
                             uint32_t y, uint32_t mode) {
    if (mode == 0) {
        // "A building just finished appearing." First: is it an enemy turret? If so, flag every
        // OTHER active player's rescan trigger (same idiom as notify_object_removed).
        const uint8_t turret_type = race_turret_type((int32_t)v.players[player].is_alien_race);
        if (v.cfg_buildings[building_id].type == turret_type) {
            for (uint32_t p = 0; p < (uint32_t)*v.active_player_count; ++p) {
                if (v.players[p].ai_enabled != 0 && p != player) {
                    own.players[p].ai_turret_rescan_pending = 1;
                }
            }
        }

        // Reconcile: an inactive AI or a building matching the cached mother-building type
        // dispatches the map-changed broadcast directly; otherwise scan the pending queue for the
        // entry that predicted this building and link it to order_id.
        if (v.players[player].ai_enabled == 0 ||
            building_id == v.players[player].ai_mother_building_type) {
            gc.notify_map_changed((int32_t)player, (int32_t)building_id, (int32_t)x, (int32_t)y);
        } else {
            for (uint32_t i = 0; i < (uint32_t)v.players[player].ai_bldg_queue_count; ++i) {
                auto &q = own.players[player].ai_bldg_queue[i];
                if (q.status == QUEUE_STATUS_COMMITTED_CONSTRUCTION &&
                    (uint32_t)q.tick_or_unit_id == building_id && (uint16_t)q.build_tile_x == x &&
                    (uint16_t)q.resource_reserved[0] == y && q.building_index == -1) {
                    q.building_index = order_id;
                    break;
                }
            }
        }

        // Separately (not exclusive with the branch above -- both run off the same mode==0 body):
        // if the new building is a MINE, resolve the matching resource-site's status and return.
        if (v.players[player].ai_enabled != 0 &&
            (v.cfg_buildings[building_id].type == BLDG_TYPE_A_MINE ||
             v.cfg_buildings[building_id].type == BLDG_TYPE_H_MINE)) {
            for (uint32_t i = 0; i < (uint32_t)v.players[player].ai_resource_site_count; ++i) {
                auto &s = own.players[player].ai_resource_sites[i];
                if ((uint16_t)s.build_tile_x == x && (uint16_t)s.build_tile_y == y) {
                    s.status = (int16_t)order_id;
                    return;
                }
            }
        }
    } else if (mode == 1) {
        // "Confirm a queue entry already linked to this building_index." Only runs when the AI is
        // enabled and the building is NOT the mother building (no mother-building dispatch call on
        // this path -- the original just returns when either condition fails).
        if (v.players[player].ai_enabled != 0 &&
            building_id != v.players[player].ai_mother_building_type) {
            for (uint32_t i = 0; i < (uint32_t)v.players[player].ai_bldg_queue_count; ++i) {
                auto &q = own.players[player].ai_bldg_queue[i];
                if (q.status == QUEUE_STATUS_COMMITTED_CONSTRUCTION &&
                    (uint32_t)q.tick_or_unit_id == building_id && x == (uint16_t)q.build_tile_x &&
                    (uint16_t)q.resource_reserved[0] == y && q.building_index == order_id) {
                    q.status |= QUEUE_STATUS_REMOVED;
                    if (building_id != (uint32_t)v.players[player].ai_build_candidate_primary)
                        return;
                    gc.order_population_delta_enqueue(player,
                                                      v.cfg_buildings[building_id].worker_count);
                    return;
                }
            }
        }
    } else if (mode == 2) {
        // "The placement failed / is being retired." Refund the reserved resource costs of a
        // matching UNRESOLVED entry, then unconditionally broadcast notify_map_changed_2 -- even
        // when the AI is disabled or no entry matched.
        if (v.players[player].ai_enabled != 0) {
            for (uint32_t i = 0; i < (uint32_t)v.players[player].ai_bldg_queue_count; ++i) {
                auto &q = own.players[player].ai_bldg_queue[i];
                if (q.status == QUEUE_STATUS_COMMITTED_CONSTRUCTION &&
                    (uint32_t)q.tick_or_unit_id == building_id && x == (uint16_t)q.build_tile_x &&
                    (uint16_t)q.resource_reserved[0] == y && q.building_index == -1) {
                    q.status |= QUEUE_STATUS_REMOVED;
                    // resource_spent[] is indexed by RESOURCE ID - 1 (ids 1..4 -> [0..3]);
                    // resource_reserved[] is indexed by RESOURCE ID directly, so the refund reads
                    // slots [1..4] into spend slots [0..3] -- see resource_spent's field comment.
                    own.players[player].resource_spent[0] += (uint16_t)q.resource_reserved[1];
                    own.players[player].resource_spent[1] += (uint16_t)q.resource_reserved[2];
                    own.players[player].resource_spent[2] += (uint16_t)q.resource_reserved[3];
                    own.players[player].resource_spent[3] += (uint16_t)q.resource_reserved[4];
                    break;
                }
            }
        }
        gc.notify_map_changed_2((int32_t)player, (int32_t)building_id, (int32_t)x, (int32_t)y);
    }
    // mode > 2: no-op, matching the original's default dispatch arm.
}

} // namespace detail

void notify_bldg_constructed(uint32_t player, uint32_t x, uint32_t order_id, uint32_t building_id,
                             uint32_t y, uint32_t mode) {
    const ai_state st = state();
    detail::notify_bldg_constructed(st.read, st.own, live_calls(), player, x, (int32_t)order_id,
                                    building_id, y, mode);
}


} // namespace mh::ai
