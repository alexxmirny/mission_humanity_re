//
// sim/sim_bldg_roster_queries.cpp -- see sim_bldg_roster_queries.h. Translated from the
// DISASSEMBLY (tmp/decomp/llm_strat_bldg_find_by_ai_build_and_type_004d36c4.asm,
// tmp/decomp/llm_strat_bldg_count_by_type_004d37f2.asm, tmp/decomp/llm_strat_bldg_count_by_id_004d388e.asm,
// tmp/decomp/llm_strat_bldg_find_mother_position_indexed_004d44d0.asm), not from Ghidra's C
// drafts -- all four drafts read correctly for VALUE comparisons, but the live-count walk idiom
// (skip-vs-decrement) and the inclusive loop bound in find_by_ai_build_and_type were independently
// re-derived from the listings per house rules.
//
#include "sim/sim_bldg_roster_queries.h"


namespace mh::sim {

namespace detail {

uint32_t bldg_find_by_ai_build_and_type(const sim_view &v, uint32_t ai_build_id,
                                        uint32_t building_type) {
    // 0x004d36fa/0x004d3700: `CMP EDX,total / JBE` -- INCLUSIVE of index == total, not exclusive.
    for (uint32_t i = 0; i <= (uint32_t)v.cfg_building_sec->total; ++i) {
        const cfg_building &b = v.cfg_buildings[i];
        if (b.ai_build == ai_build_id && (uint32_t)b.type == building_type) return i;
    }
    return 0xffffffffu;
}

int32_t bldg_count_by_type(const sim_view &v, int32_t player, int32_t bldg_type) {
    int32_t count_result = 0;
    int32_t slot         = 1;
    // 0x004d3824: MOVZX word -- the occupied-slot live count, read as unsigned.
    uint16_t remaining = (uint16_t)building_of(v, (uint32_t)player, 0).index;
    while (remaining != 0) {
        // A slot with building_id == 0 is SKIPPED, not counted as empty: it does not decrement
        // `remaining` at all (0x004d3854/0x004d385c: CMP/JZ straight past the ADD/DEC pair).
        const uint16_t building_id = building_of(v, (uint32_t)player, slot).building_id;
        if (building_id != 0) {
            if ((int32_t)v.cfg_buildings[building_id].type == bldg_type) ++count_result;
            --remaining;
        }
        ++slot;
    }
    return count_result;
}

int32_t bldg_count_by_id(const sim_view &v, int32_t player, int32_t building_id) {
    int32_t  count_result = 0;
    int32_t  slot         = 1;
    uint16_t remaining    = (uint16_t)building_of(v, (uint32_t)player, 0).index;
    while (remaining != 0) {
        const uint16_t slot_building_id = building_of(v, (uint32_t)player, slot).building_id;
        if (slot_building_id != 0) {
            // Direct roster-id comparison -- NO cfg indirection (unlike bldg_count_by_type above).
            if ((int32_t)slot_building_id == building_id) ++count_result;
            --remaining;
        }
        ++slot;
    }
    return count_result;
}

uint32_t bldg_find_mother_position_indexed(const sim_view &v, int32_t player, uint32_t *out_x,
                                           uint32_t *out_y) {
    // 0x004d44e8/0x004d44ee: both out-params are zeroed BEFORE the scan starts, so a not-found
    // return still leaves them defined.
    *out_x = 0;
    *out_y = 0;

    int32_t  slot      = 1;
    uint16_t remaining = (uint16_t)building_of(v, (uint32_t)player, 0).index;
    while (true) {
        if (remaining == 0) return 0;

        const building &b = building_of(v, (uint32_t)player, slot);
        if (b.building_id != 0) {
            const uint8_t type = v.cfg_buildings[b.building_id].type;
            if (type == BUILDING_TYPE_A_MOTHER || type == BUILDING_TYPE_H_MOTHER) {
                // 0x004d458f-0x004d45a3: writes x/y and returns 1 IMMEDIATELY -- does not keep
                // scanning past the first match.
                *out_x = (uint32_t)b.x;
                *out_y = (uint32_t)b.y;
                return 1;
            }
            --remaining;
        }
        ++slot;
    }
}

} // namespace detail

// ---- the public wrappers --------------------------------------------------------------------

uint32_t bldg_find_by_ai_build_and_type(int32_t /*unused*/, uint32_t ai_build_id,
                                        uint32_t building_type) {
    const sim_view v = state().read;
    return detail::bldg_find_by_ai_build_and_type(v, ai_build_id, building_type);
}

int32_t bldg_count_by_type(int32_t player, int32_t bldg_type) {
    const sim_view v = state().read;
    return detail::bldg_count_by_type(v, player, bldg_type);
}

int32_t bldg_count_by_id(int32_t player, int32_t building_id) {
    const sim_view v = state().read;
    return detail::bldg_count_by_id(v, player, building_id);
}

uint32_t bldg_find_mother_position_indexed(int32_t param_1, uint32_t *param_2, uint32_t *a2) {
    const sim_view v = state().read;
    return detail::bldg_find_mother_position_indexed(v, param_1, param_2, a2);
}


} // namespace mh::sim
