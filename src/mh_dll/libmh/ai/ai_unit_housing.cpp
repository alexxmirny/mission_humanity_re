//
// ai/ai_unit_housing.cpp -- see ai_unit_housing.h. Translated from the DISASSEMBLY
// (tmp/decomp_ai/llm_strat_ai_maintain_unit_housing_004e58d1.asm), not from Ghidra's C -- the .c
// draft's CONCAT31(extraout_var, bVar5) idiom around llm_strat_ai_bldg_type_already_queued's uint8
// return is the usual Watcom decompile artifact for a callee that returns in AL only, not a real
// register combine; every `already_queued` test below is the plain `== 0` the assembly branches on
// (TEST AL,AL is not present -- the callee sign-extends its own AL result and the caller TESTs the
// full EAX at 0x004e597c/0x004e59fc/0x004e5a58/0x004e5ac6).
//
// EVERY ABSOLUTE RE-DERIVED against addr/mh_structs.gen.h, not transcribed:
//   0xc38530/34/38/3c = _G_LLM_STRAT_UNIT_HOUSING_STATS[player].{used_vehicles,used_soldiers,
//                        used_planes,used_helis} (SHL 6 = the 0x40 stride, at 0x004e58e8)
//   0xe9656c/70/74/78 = player_data[player].ai_score_cat_0x{20,21,23,22} (the player*0x288fc stride
//                        built by the SHL2/ADD/SHL7/SUB/SHL2/SHL6/ADD sequence at
//                        0x004e593b-0x004e5951, repeated once per block)
//   0xe967ac           = player_data[player].ai_resource_shortage_candidates[0]
//   0xe967bc/c0/c4/c8  = player_data[player + 1].ai_housing_candidate_{heli,plane,vehicle,soldier}
//                        -- the SAME player*0x288fc stride as above, i.e. the NEXT player's record;
//                        see the header and ai_build.cpp for why that is deliberate.
// so no byte offset and no literal VA appears below (Law 1).
//
#include "ai/ai_unit_housing.h"


namespace mh::ai {
namespace detail {

namespace {

// `used / 50 + 1`, unsigned (XOR EDX,EDX before DIV at 0x004e590d/0x004e591d/0x004e5927/0x004e5931
// -- a real DIV instruction against the literal divisor 0x32, not a shift/add signed-division idiom,
// so there is no truncation-direction ambiguity to preserve).
inline uint32_t housing_needed(uint32_t used) { return used / 50u + 1u; }

} // namespace

void maintain_unit_housing(const ai_view &v, const ai_store &own, const ai_calls &gc, int32_t player) {
    (void)own; // this function writes nothing of its own -- every effect goes through `gc`

    const player_data &pd = v.players[player];
    // players[player + 1]: the deliberate +1 aliasing the header documents (same overrun ai_build.cpp
    // writes; for player == MAX_PLAYERS - 1 this reads one record past the array, in .bss, matching
    // the original).
    const player_data   &next = v.players[player + 1];
    const housing_stats &hs   = v.unit_housing[player];

    const uint32_t needed_soldiers = housing_needed((uint32_t)hs.used_soldiers);
    const uint32_t needed_vehicles = housing_needed((uint32_t)hs.used_vehicles);
    const uint32_t needed_planes   = housing_needed((uint32_t)hs.used_planes);
    const uint32_t needed_helis    = housing_needed((uint32_t)hs.used_helis);

    // ---- soldiers: gated on count_by_id(player, ai_resource_shortage_candidates[0]) != 0 ----------
    if (gc.bldg_count_by_id(player, pd.ai_resource_shortage_candidates[0]) != 0 &&
        (uint32_t)pd.ai_score_cat_0x20 < needed_soldiers) {
        const int32_t candidate = next.ai_housing_candidate_soldier;
        if (gc.bldg_type_already_queued(player, (uint32_t)candidate) == 0)
            gc.bldg_queue_construction(player, candidate, -1, 0);
    }

    // ---- vehicles: gated on (cat_0x11 || cat_0x12 || cat_0x13) && cat_0x20 != 0 -------------------
    if ((pd.ai_score_cat_0x11 != 0 || pd.ai_score_cat_0x12 != 0 || pd.ai_score_cat_0x13 != 0) &&
        pd.ai_score_cat_0x20 != 0 && (uint32_t)pd.ai_score_cat_0x21 < needed_vehicles) {
        const int32_t candidate = next.ai_housing_candidate_vehicle;
        if (gc.bldg_type_already_queued(player, (uint32_t)candidate) == 0)
            gc.bldg_queue_construction(player, candidate, -1, 0);
    }

    // ---- planes: gated on side_has_aircraft_producer(player) != 0 && cat_0x21 != 0 ----------------
    if (gc.bldg_side_has_aircraft_producer(player) != 0 && pd.ai_score_cat_0x21 != 0 &&
        (uint32_t)pd.ai_score_cat_0x23 < needed_planes) {
        const int32_t candidate = next.ai_housing_candidate_plane;
        if (gc.bldg_type_already_queued(player, (uint32_t)candidate) == 0)
            gc.bldg_queue_construction(player, candidate, -1, 0);
    }

    // ---- helis: gated on bldg_has_heli_unit(player) != 0 && cat_0x21 != 0 && cat_0x23 != 0 --------
    if (gc.bldg_has_heli_unit(player) != 0 && pd.ai_score_cat_0x21 != 0 && pd.ai_score_cat_0x23 != 0 &&
        (uint32_t)pd.ai_score_cat_0x22 < needed_helis) {
        const int32_t candidate = next.ai_housing_candidate_heli;
        if (gc.bldg_type_already_queued(player, (uint32_t)candidate) == 0) {
            // The original's tail here is `JMP llm_strat_bldg_queue_construction_thunk` -- a shared
            // whole-body thunk, not a callable unit -- whose first instruction (`MOV EAX,ESI`) feeds
            // it this function's own `player`. Inlined per the header/hazard note rather than routed
            // through a thunk callee.
            gc.bldg_queue_construction(player, candidate, -1, 0);
        }
    }
    // Every other exit (`JMP 0x004e792e`) is a shared Watcom epilogue, not a call -- it means `return`.
}

// llm_strat_ai_bldg_has_heli_unit @0x004d8a9d, translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_ai_bldg_has_heli_unit_004d8a9d.asm), which matches Ghidra's own .c exactly
// here (checked instruction-by-instruction).
int32_t bldg_has_heli_unit(const ai_view &v, int32_t player) {
    constexpr uint32_t AI_UNIT_HELI = 6; // confirmed via the disassembly's `CMP dword ...,0x6`

    uint32_t remaining = (uint16_t)building_of(v, (uint32_t)player, 0).index;
    int32_t  idx       = 1;
    while (remaining != 0) {
        const building &b = building_of(v, (uint32_t)player, idx);
        if (b.building_id != 0) {
            for (uint32_t u = 1; u <= v.cfg_unit_sec->total; ++u) {
                if (v.cfg_buildings[b.building_id].unit_quant[u] > 0.0 &&
                    v.cfg_units[u].ai_unit == AI_UNIT_HELI)
                    return 1;
            }
            --remaining;
        }
        ++idx;
    }
    return 0;
}

} // namespace detail

void maintain_unit_housing(int32_t player) {
    const ai_state st = state();
    detail::maintain_unit_housing(st.read, st.own, live_calls(), player);
}

int32_t bldg_has_heli_unit(int32_t player) {
    const ai_state st = state();
    return detail::bldg_has_heli_unit(st.read, player);
}


} // namespace mh::ai
