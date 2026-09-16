//
// sim/sim_bldg_defense_cost.cpp -- see sim_bldg_defense_cost.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_bldg_max_defense_radius_sq_004d3e61.asm,
// tmp/decomp/llm_strat_bldg_total_resource_cost_004e2d52.asm), not from Ghidra's C drafts -- both
// drafts read correctly for the VALUE comparisons and loop shape (independently re-verified below),
// so nothing here corrects them, but every literal, callee, and loop-bound-vs-body-order was
// re-derived from the listings per house rules.
//
#include "sim/sim_bldg_defense_cost.h"

#include "addr/mh_calls.gen.h"  // mh::call::llm_strat_toroidal_dist_sq -- bound live in live_bldg_defense_cost_calls()
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const bldg_defense_cost_calls &live_bldg_defense_cost_calls() {
    static const bldg_defense_cost_calls gc = {
        MH_LIBMH_BIND(llm_strat_toroidal_dist_sq),
    };
    return gc;
}

namespace detail {

uint32_t bldg_max_defense_radius_sq(const sim_view &v, const bldg_defense_cost_calls &c,
                                    int32_t player, int32_t x, int32_t y) {
    // 0x004d3e7c/0x004d3e7e: TEST EDX,EDX / JGE -- only search for the Mothership when x<0.
    if (x < 0) {
        // 0x004d3e9d: MOVZX ECX, buildings[player][0].index -- the live building count, same idiom
        // as ai_nearest_flagged.cpp's `remaining`.
        uint32_t remaining = (uint32_t)(uint16_t)building_of(v, (uint32_t)player, 0).index;
        for (int32_t i = 1; remaining != 0; ++i) {
            const building &b = building_of(v, (uint32_t)player, i);
            // 0x004d3ed8: JZ -- an empty slot skips the DEC entirely (no budget consumed), matching
            // every ai:: roster walker's hole-skipping shape.
            if (b.building_id == 0) continue;
            --remaining; // 0x004d3f35 (reached on a non-match) / implicitly on a match too, but the
                         // match path breaks before it matters either way.
            const uint8_t type = v.cfg_buildings[b.building_id].type;
            // 0x004d3ee7/0x004d3ef0: H_MOTHER (0x1a) then A_MOTHER (0x6).
            if (type == BUILDING_TYPE_H_MOTHER || type == BUILDING_TYPE_A_MOTHER) {
                // 0x004d3f1f-0x004d3f30: overwrite x/y with the found Mothership's own byte
                // coordinates and stop looking (break, not continue -- only the FIRST match counts).
                x = (int32_t)b.x;
                y = (int32_t)b.y;
                break;
            }
        }
    }

    // 0x004d3f3f/0x004d3f43: still negative (no explicit point AND no Mothership found) -> a
    // sentinel, not a real distance. The second scan below never runs in this case.
    if (x < 0) return 0xffffffffu;

    uint32_t max_dist  = 0; // EDI, XOR'd at 0x004d3f4f; stays 0 if no qualifying building exists.
    uint32_t remaining = (uint32_t)(uint16_t)building_of(v, (uint32_t)player, 0).index;
    for (int32_t i = 1; remaining != 0; ++i) {
        const building &b = building_of(v, (uint32_t)player, i);
        if (b.building_id == 0) continue; // 0x004d4031 JZ -- hole, no budget consumed.
        --remaining;
        const uint8_t type = v.cfg_buildings[b.building_id].type;
        // 0x004d3fc0-0x004d400a: eight CMPs, both A_/H_ variants of mother/mine/turret/relay --
        // ALL excluded from the max-distance scan. Constants are sim_order_enqueue.h's own.
        if (type == BUILDING_TYPE_H_MOTHER || type == BUILDING_TYPE_A_MOTHER ||
            type == BUILDING_TYPE_H_MINE || type == BUILDING_TYPE_A_MINE ||
            type == BUILDING_TYPE_H_TURRET || type == BUILDING_TYPE_A_TURRET ||
            type == BUILDING_TYPE_H_RELAY || type == BUILDING_TYPE_A_RELAY) {
            continue;
        }
        // 0x004d400c-0x004d4022: toroidal_dist_sq(origin_x, origin_y, building.x, building.y) --
        // argument order read off the __cdecl push sequence (see the header banner).
        const uint32_t d = c.toroidal_dist_sq(x, y, (int32_t)b.x, (int32_t)b.y);
        // 0x004d402a/0x004d402c: CMP/JBE -- strictly greater only, ties keep the running max (which
        // is value-identical to updating on a tie, since the value doesn't change).
        if (d > max_dist) max_dist = d;
    }
    return max_dist;
}

int32_t bldg_total_resource_cost(const sim_view &v, int32_t building_type) {
    const cfg_building &cb    = v.cfg_buildings[building_type];
    int32_t             total = 0;
    // 0x004e2d66/0x004e2d85: the loop TEST (i<7) runs before the body ever reads resource[i], so
    // resource[7] is never touched (contrast sim_unit_refund.cpp's cfg_unit walk, which reads its
    // 7th-index slot's id before its own bound check). i < CFG_RESOURCE_SLOTS(7).
    for (int32_t i = 0; i < CFG_RESOURCE_SLOTS; ++i) {
        // 0x004e2d75/0x004e2d7c: JZ straight to the function's single exit -- a BREAK on the first
        // undefined slot's id (cfg_enum_E_RESOURCE member 0, "UNDEFINED"), not a skip-and-continue.
        if (cb.resource[i].id == 0) break;
        total += cb.resource[i].val; // 0x004e2d7e
    }
    return total;
}

} // namespace detail

// ---- the public wrappers ------------------------------------------------------------------------

uint32_t bldg_max_defense_radius_sq(int32_t player, int32_t x, int32_t y) {
    const sim_view v = state().read;
    return detail::bldg_max_defense_radius_sq(v, live_bldg_defense_cost_calls(), player, x, y);
}

int32_t bldg_total_resource_cost(int32_t building_type) {
    const sim_view v = state().read;
    return detail::bldg_total_resource_cost(v, building_type);
}


} // namespace mh::sim
