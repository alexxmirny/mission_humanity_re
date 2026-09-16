//
// ai/ai_nearest_flagged.cpp -- see ai_nearest_flagged.h. Translated from the DISASSEMBLY
// (tmp/decomp_ai/llm_strat_ai_find_nearest_flagged_building_004e4e54.asm), not from Ghidra's C.
//
// EVERY ABSOLUTE RE-DERIVED against addr/mh_structs.gen.h rather than transcribed:
//   0xc3d2a0 = buildings + 0x0   = buildings[0][0].index         (row stride 27300 = 100 * 0x111,
//              built by SHL 3 / SUB / SHL 2 / SHL 4 / SUB / SHL 6 at 0x004e4e79-0x004e4e8d;
//              record stride 0x111 = 273, built by SHL 4 / ADD / SHL 4 / ADD at 0x004e4eb2-0x004e4ebe)
//   0xc3d2a2 = buildings + 0x2   = buildings[0][0].building_id
//   0xc3d2a4 = buildings + 0x4   = buildings[0][0].built_flags
//   0xc3d363 / 0xc3d364          = buildings[0][1].x / .y
// so no byte offset and no literal VA appears below (Law 1).
//
#include "ai/ai_nearest_flagged.h"


namespace mh::ai {
namespace detail {

int32_t find_nearest_flagged_building(const ai_view &v, const ai_calls &gc, int32_t player,
                                      int32_t x, int32_t y) {
    int32_t  best_index = -1;          // [EBP-0x10], MOV .. ,ESI @0x004e4e6f with ESI = 0xffffffff
    uint32_t best_dist  = 0xffffffffu; // ESI, MOV ESI,0xffffffff @0x004e4e6a
    // [EBP-0x14]. UNINITIALISED in the original; see the header for why 0 is exactly equivalent.
    uint32_t last_dist = 0;

    // MOVZX ECX,word ptr [.. + 0xc3d2a0] @0x004e4e8d -- the live count, zero-extended.
    uint32_t remaining = (uint32_t)(uint16_t)building_of(v, (uint32_t)player, 0).index;

    // TEST ECX,ECX / JA @0x004e4f03 -- run while any live building is still unaccounted for.
    for (int32_t i = 1; remaining != 0; ++i) {
        const building &b = building_of(v, (uint32_t)player, i);
        // CMP word ptr [.. + 0xc3d2a2],0x0 / JZ 0x004e4f02 @0x004e4ec0 -- lands PAST the DEC ECX,
        // so a hole in the roster does not consume the budget.
        if ((uint16_t)b.building_id == 0) continue;
        --remaining; // DEC ECX @0x004e4f01, reached by every path below
        // TEST byte ptr [.. + 0xc3d2a4],0x1 / JZ @0x004e4eca -- the record's connected bit.
        if ((b.built_flags & 1) == 0) continue;

        // PUSH .y / PUSH .x / PUSH y / PUSH x then CALL @0x004e4ee9. __cdecl pushes right to left,
        // so the argument order is (query x, query y, building x, building y).
        const uint32_t d = gc.toroidal_dist_sq(x, y, (int32_t)b.x, (int32_t)b.y);
        last_dist        = d; // MOV dword ptr [EBP + -0x14],EAX @0x004e4ef1 -- BEFORE both tests

        if (d == 0) continue;         // TEST EAX,EAX / JBE @0x004e4ef4 (unsigned, so only 0)
        if (d >= best_dist) continue; // CMP EAX,ESI / JNC @0x004e4ef8 -- ties keep the first
        best_index = i;               // MOV dword ptr [EBP + -0x10],EBX @0x004e4efc
        best_dist  = d;               // MOV ESI,EAX @0x004e4eff
    }

    // CMP dword ptr [EBP + -0x14],0xf / JA @0x004e4f07. THE LAST CANDIDATE'S DISTANCE, not the
    // winner's -- see the header. Not a transcription slip; do not "fix" it to best_dist.
    return (last_dist > 0xfu) ? best_index : -1;
}

} // namespace detail

int32_t find_nearest_flagged_building(int32_t player, int32_t x, int32_t y) {
    const ai_state st = state();
    return detail::find_nearest_flagged_building(st.read, live_calls(), player, x, y);
}

// ---- the differential-oracle arm ----------------------------------------------------------------
//
// llm_strat_toroidal_dist_sq is pure, so it runs for real -- stubbing it would feed our arm
// different inputs and manufacture a divergence rather than silence a side effect. The body writes
// nothing (the state matrix measures zero cells, direct and transitive), so the site's whole verdict
// is the RETURN value. That is the same shape ai_bldg_weapon_range.cpp arms and it is real evidence.
//
// WHAT A VACUOUS GREEN LOOKS LIKE HERE. -1 is by far the most common answer and there are THREE
// unrelated ways to reach it: an empty roster, no connected neighbour, and the last-candidate gate
// firing. A run in which every call returned -1 has compared almost nothing, so the arm counts the
// calls that EVALUATED at least one candidate (`evaluated`), the calls that found a winner before
// the gate (`had_best`), and the calls the gate then threw away (`gated`). `gated > 0` is the only
// evidence that the branch this whole module is about was ever taken.

} // namespace mh::ai
