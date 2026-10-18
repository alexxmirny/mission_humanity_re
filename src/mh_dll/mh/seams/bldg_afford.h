// seams/bldg_afford.h -- the read-only copy of llm_bldg_pay_build_cost's PASS 1, shared by the D35
// build-click probe (ui_bldg_build_probe.cpp) and the D11 Shift re-arm (ui_bldg_rearm.cpp).
#pragma once
#include <cstdint>

#include "addr/mh_regions.gen.h" // mh::state::live(), RID_BUILDING / RID_PROGRESS / RID_PLAYER_RESOURCES
#include "addr/mh_structs.gen.h" // mh::game::mh_cfg_final_struct_Building / mh_game_progress

namespace mh::seam {

inline constexpr int RESOURCE_SLOTS   = 7;   // cfg_building.resource[7] (CFG_RESOURCE_SLOTS)
inline constexpr int INVENTIONS       = 300; // progress[8][300], 0x384 bytes per player
inline constexpr int RESOURCES_PER_PL = 10;  // player_resources int[8][10], 0x28 bytes per player

// llm_bldg_pay_build_cost's PASS 1 (0x00492ecb-0x00492f8e), read-only. Mirrors libmh's
// bldg_can_afford_build_cost instruction for instruction, including the id-read-before-bound-check
// order: resource[7].id is read (into build_time_2's low bytes, inside the record) before `i < 7`
// ends the walk, which is provably inert -- i reaches 7 only when slots 0..6 were all defined, and
// the bound check then exits whatever the id holds.
// Returns 0 = affordable, 0x13 = the prerequisite invention is not available, else the refusal text id.
//
// `copies` (2026-10-16, the single-player fixed step): how many copies of the cost the stock must cover. The D11 Shift re-arm asks
// for 2 when the click's OWN order is still queued (ORDER_QUEUE_COUNT != 0): that order pays in the next sim step, and a fixed
// 20 ms step leaves frames without one, so the read can land before the payment (retail's step ran every frame).
inline int32_t can_afford_build_cost(uint32_t player, int32_t building_type_id, int32_t copies = 1) {
    using mh::game::mh_cfg_final_struct_Building;
    using mh::game::mh_game_progress;
    const auto    &lv = mh::state::live();
    const uint32_t p  = player & 0xffffu; // MOVZX word at every use site (0x00492edf / 0x00492f43)

    const auto *cb = reinterpret_cast<const mh_cfg_final_struct_Building *>(
        lv.base[mh::state::RID_BUILDING] + (uint32_t)building_type_id * sizeof(mh_cfg_final_struct_Building));
    const auto *progress = reinterpret_cast<const mh_game_progress *>(lv.base[mh::state::RID_PROGRESS]);
    const auto *stock    = reinterpret_cast<const int32_t *>(lv.base[mh::state::RID_PLAYER_RESOURCES]);

    // 0x00492ece-0x00492efb: the prerequisite invention gate, before any resource is looked at.
    if (progress[p * INVENTIONS + cb->invention].available == 0) return 0x13;

    // 0x00492f0e-0x00492f80: every slot, no short-circuit; first shortage id+0x89, any later 0x89.
    int32_t error = 0;
    for (int32_t i = 0;; ++i) {
        const uint32_t id = cb->resource[i].id;                                // 0x00492f1d
        if (id == 0) break;                                                    // 0x00492f2a: UNDEFINED
        if (!(i < RESOURCE_SLOTS)) break;                                      // 0x00492f30: bound check, evaluated second
        if (stock[p * RESOURCES_PER_PL + id] < cb->resource[i].val * copies) { // 0x00492f52-0x00492f5e (signed JGE)
            error = (error == 0) ? (int32_t)(id + 0x89u) : 0x89;
        }
    }
    return error; // 0 = affordable; pass 2 (game_SpendResource per slot) is deliberately absent
}

} // namespace mh::seam
