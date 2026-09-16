//
// ai/ai_group_building_scan.cpp -- see ai_group_building_scan.h. Translated from the DISASSEMBLY
// (tmp/decomp/llm_strat_ai_group_find_nearest_building_of_types_004e98a7.asm and
// tmp/decomp/llm_strat_ai_group_collect_buildings_of_types_004e99be.asm), not from Ghidra's C: the
// two decompiles are already faithful (no extraout_/in_stack_ noise, no phantom stores) and this
// translation follows them, going through building_of()/the struct's own field layout
// (addr/mh_structs.gen.h's mh_map_object_building: index @0x0, building_id @0x2, ..., x @0xc3,
// y @0xc4, pack(1)) rather than the .c draft's inlined byte-offset arithmetic (player_id * 0x6aa4
// + 0xc3d2a0/0xc3d363/0xc3d364 + building_index * 0x111) -- the two agree once the record base and
// stride are subtracted out, and the accessor is what every sibling translation in this module uses.
//
#include "ai/ai_group_building_scan.h"


namespace mh::ai {
namespace detail {

int32_t group_find_nearest_building_of_types(const ai_view &v, const ai_calls &gc,
                                             int32_t player_id, int32_t query_x, int32_t query_y,
                                             uint32_t building_type_1, uint32_t building_type_2,
                                             uint32_t building_type_3, uint32_t building_type_4) {
    int32_t  closest_index = 0;           // [EBP-0x10], MOV .. ,0x0 @0x004e98c4
    uint32_t closest_dist  = 0xffffffffu; // [EBP-0xc],  MOV .. ,0xffffffff @0x004e98cb

    // MOVZX EDI,word ptr [.. + 0xc3d2a0] @0x004e98e6 -- the live count, zero-extended (the same
    // ushort-bit-pattern reinterpretation turret_threat_rescan documents for this field).
    uint32_t remaining = (uint32_t)(uint16_t)building_of(v, (uint32_t)player_id, 0).index;

    // TEST EDI,EDI / JA 0x004e98f8 @0x004e99aa -- run while any live building is unaccounted for.
    for (int32_t building_index = 1; remaining != 0; ++building_index) {
        const building &b = building_of(v, (uint32_t)player_id, building_index);
        // CMP word ptr [.. + 0xc3d2a2],0x0 / JZ 0x004e99a9 @0x004e991f -- lands PAST the DEC EDI
        // (LAB_004e99a8), so an empty slot does not consume the budget.
        if (b.building_id == 0) continue;
        --remaining; // DEC EDI @LAB_004e99a8, reached by every path below

        // llm_strat_bldg_is_alive(player_id, building_index) @0x004e9931.
        if (gc.bldg_is_alive(player_id, building_index) == 0) continue;

        const uint32_t bid = (uint32_t)b.building_id;
        if (bid != building_type_1 && bid != building_type_2 && bid != building_type_3 &&
            bid != building_type_4)
            continue;

        // PUSH query_y / PUSH query_x / PUSH b.y / PUSH b.x then CALL @0x004e9995 --  __cdecl
        // pushes right-to-left, so the LAST push (b.x) lands in the FIRST argument slot:
        // toroidal_dist_sq(b.x, b.y, query_x, query_y).
        const uint32_t dist = gc.toroidal_dist_sq((int32_t)b.x, (int32_t)b.y, query_x, query_y);

        // CMP EAX,[EBP-0xc] / JNC 0x004e99a8 @0x004e99a0 -- unsigned; only a STRICTLY smaller
        // distance replaces the winner, so a tie keeps the first candidate found.
        if (dist < closest_dist) {
            closest_index = building_index;
            closest_dist  = dist;
        }
    }
    return closest_index;
}

void group_collect_buildings_of_types(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                      int32_t player_id, uint32_t unused_edx_slot,
                                      uint32_t unused_ebx_slot, uint32_t building_type_1,
                                      uint32_t building_type_2, uint32_t building_type_3,
                                      uint32_t building_type_4) {
    (void)unused_edx_slot; // EDX at entry -- the committed prototype's own name; never read
    (void)unused_ebx_slot; // EBX at entry -- ditto

    // Identical roster walk to the sibling above (see its comments for the per-line derivation).
    uint32_t remaining = (uint32_t)(uint16_t)building_of(v, (uint32_t)player_id, 0).index;

    for (int32_t building_index = 1; remaining != 0; ++building_index) {
        const building &b = building_of(v, (uint32_t)player_id, building_index);
        if (b.building_id == 0) continue;
        --remaining;

        if (gc.bldg_is_alive(player_id, building_index) == 0) continue;

        const uint32_t bid = (uint32_t)b.building_id;
        if (bid != building_type_1 && bid != building_type_2 && bid != building_type_3 &&
            bid != building_type_4)
            continue;

        // 0x004e9a54-0x004e9a62: append building_index at the current count, then bump it. No
        // capacity check against the list's declared [256] extent -- reproduced, not fixed.
        //
        // DECLARED NEED (see ai_group_building_scan.h and this translation's report): ai_store has
        // no writable `building_candidate_scratch_list` member today, only the count. The line
        // below is written against the member this function NEEDS to exist; it will not compile
        // until the conductor adds it to ai_store (mirroring the const view's
        // `building_candidate_scratch_list`, pointing at the same
        // _G_LLM_STRAT_AI_BUILDING_CANDIDATE_SCRATCH_LIST region already in ai_view).
        const int32_t slot                        = *own.building_candidate_scratch_count;
        own.building_candidate_scratch_list[slot] = building_index;
        *own.building_candidate_scratch_count     = slot + 1;
    }
}

} // namespace detail

// ---- the public wrappers -------------------------------------------------------------------------

int32_t group_find_nearest_building_of_types(int32_t player_id, int32_t query_x, int32_t query_y,
                                             uint32_t building_type_1, uint32_t building_type_2,
                                             uint32_t building_type_3, uint32_t building_type_4) {
    const ai_state st = state();
    return detail::group_find_nearest_building_of_types(st.read, live_calls(), player_id, query_x,
                                                        query_y, building_type_1, building_type_2,
                                                        building_type_3, building_type_4);
}

void group_collect_buildings_of_types(int32_t player_id, uint32_t unused_edx_slot,
                                      uint32_t unused_ebx_slot, uint32_t building_type_1,
                                      uint32_t building_type_2, uint32_t building_type_3,
                                      uint32_t building_type_4) {
    const ai_state st = state();
    detail::group_collect_buildings_of_types(st.read, st.own, live_calls(), player_id,
                                             unused_edx_slot, unused_ebx_slot, building_type_1,
                                             building_type_2, building_type_3, building_type_4);
}


} // namespace mh::ai
