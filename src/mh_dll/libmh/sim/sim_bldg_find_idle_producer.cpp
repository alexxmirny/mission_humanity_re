//
// sim/sim_bldg_find_idle_producer.cpp -- see sim_bldg_find_idle_producer.h. Translated from the
// DISASSEMBLY (tmp/decomp/llm_strat_bldg_find_idle_producer_for_unit_004e2478.asm), not from the
// Ghidra .c draft.
//
#include "sim/sim_bldg_find_idle_producer.h"

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace {

// Ghidra's own `llm_strat_bldg_state` enum member names for the two building states this walk
// accepts (see header banner: the strategic-sim notes independently confirms 1 = idle_activate,
// 0x88 = the generic idle_noop operating state). Local per-TU constants, same naming convention as
// sim_bldg_mother_reelect_primary.cpp's BLDG_STATE_IDLE_NOOP_8C / sim_order_dispatch_bldg.cpp's
// BLDG_STATE_IDLE_NOOP.
inline constexpr uint16_t BLDG_STATE_IDLE_ACTIVATE = 1u;
inline constexpr uint16_t BLDG_STATE_IDLE_NOOP_88  = 0x88u;

} // namespace

namespace detail {

int32_t bldg_find_idle_producer_for_unit(const sim_view &v, int32_t player_id, int32_t unit_id) {
    // 0x004e248b/0x004e24a4: MOVZX ECX, buildings[player_id][0].index -- the live building count,
    // same idiom as sim_bldg_defense_cost.cpp's `remaining` / ai_unit_housing.cpp's
    // bldg_has_heli_unit. Loop is compiled jump-to-condition-check, so a count of 0 returns 0 with
    // the body never entered.
    uint32_t remaining = (uint32_t)(uint16_t)building_of(v, (uint32_t)player_id, 0).index;
    for (int32_t slot = 1; remaining != 0; ++slot) {
        const building &b = building_of(v, (uint32_t)player_id, slot);
        // 0x004e24df: JZ -- an empty slot skips straight to the next slot, no budget consumed (same
        // hole-skipping shape as every other roster walker in this cluster).
        if (b.building_id == 0) continue;

        // 0x004e24e1-0x004e24f4: state must be IDLE_ACTIVATE or IDLE_NOOP_88, else no match.
        if (b.state == BLDG_STATE_IDLE_ACTIVATE || b.state == BLDG_STATE_IDLE_NOOP_88) {
            // 0x004e251a-0x004e2536: `0.0 < cfg_buildings[b.building_id].unit_quant[unit_id]` --
            // FLDZ/FCOMP/FNSTSW/SAHF/JNC per the header banner's derivation.
            if (0.0 < v.cfg_buildings[b.building_id].unit_quant[unit_id]) {
                // 0x004e2538-0x004e253a: MOV EAX,EBX (slot) / JMP <shared epilogue> -- return slot.
                return slot;
            }
        }
        // 0x004e253f: DEC ECX -- reached on any non-match (wrong state, or state ok but capacity
        // check failed); the match path above returns before this ever runs, so whether the
        // decrement happens before or after the checks makes no observable difference here (same
        // reasoning sim_bldg_defense_cost.cpp's own `--remaining` comment records for its analogous
        // break-before-decrement-matters case).
        --remaining;
    }
    // 0x004e2549: live count exhausted with no match -- return 0.
    return 0;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

int32_t bldg_find_idle_producer_for_unit(int32_t player_id, int32_t unit_id) {
    const sim_view v = state().read;
    return detail::bldg_find_idle_producer_for_unit(v, player_id, unit_id);
}


} // namespace mh::sim
