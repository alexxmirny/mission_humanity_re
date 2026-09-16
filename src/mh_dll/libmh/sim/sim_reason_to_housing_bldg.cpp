//
// sim/sim_reason_to_housing_bldg.cpp -- see sim_reason_to_housing_bldg.h. Translated from the
// DISASSEMBLY (tmp/decomp_sim/llm_strat_reason_to_housing_bldg_0047401c.asm), cross-checked against
// the Ghidra .c draft (tmp/decomp_sim/llm_strat_reason_to_housing_bldg_0047401c.c), which agrees
// branch-for-branch with the assembly (same switch arms, same enum-member case constants, same
// 1..99 scan) -- no phantom store, no re-materialisation artifact, no FP compare in this body. The
// opening `CALL utils_assert_stack_capacity` is the inert Watcom prologue helper (translator-brief
// rule 6) and is omitted.
//
#include "sim/sim_reason_to_housing_bldg.h"

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace {

// game_e_race member this function compares `race` against -- same value/name established precedent
// as sim_game_get_starting_unit.h's / sim_bldg_completion_dispatch.cpp's own independent RACE_ALIEN
// declarations (no committed Ghidra enum exists yet either; each TU re-declares it file-locally).
inline constexpr int32_t RACE_ALIEN = 2;

// ---- cfg_enum_E_BUILDING members the four switch arms compare against -- an EXISTING Ghidra enum
// (rule 17a; the Ghidra .c draft renders these as the enum's own symbolic names, not raw literals).
// Re-declared file-local per the established per-TU precedent (sim_bldg_completion_dispatch.cpp,
// sim_bldg_unmap_footprint.cpp, sim_order_dispatch_bldg.cpp all carry the SAME BLDG_TYPE_ constants
// at the SAME values). `A_BARRAKS` (missing a C) is the enum's own spelling.
inline constexpr uint8_t BLDG_TYPE_A_BARRAKS  = 7;
inline constexpr uint8_t BLDG_TYPE_H_BARRACKS = 0x1b;
inline constexpr uint8_t BLDG_TYPE_A_GARAGE   = 8;
inline constexpr uint8_t BLDG_TYPE_H_GARAGE   = 0x1c;
inline constexpr uint8_t BLDG_TYPE_A_HELIPAD  = 0x0a;
inline constexpr uint8_t BLDG_TYPE_H_HELIPAD  = 0x1e;
inline constexpr uint8_t BLDG_TYPE_A_AIRFIELD = 9;
inline constexpr uint8_t BLDG_TYPE_H_AIRFIELD = 0x1d;

} // namespace

namespace detail {

int32_t reason_to_housing_bldg(const sim_view &v, uint32_t reason, int32_t race) {
    // 0x00474057-0x004740e9: `reason - 0xf` unsigned-bounds-checked against 3 (JA -> default/return 0),
    // then a 4-way jump table over the four recognised reason codes. DECLARED NEED (see the header):
    // 0xf/0x10/0x11/0x12 have no Ghidra enum or established name anywhere in the codebase (the Ghidra
    // .c draft itself renders them as bare hex case labels), so they stay literal here rather than
    // being invented into local names -- per rule 17a, that absence is reported, not papered over.
    uint8_t target;
    switch (reason) {
        case 0xf: // BARRACKS-housed group (0x0047406e-0x00474084)
            target = (race == RACE_ALIEN) ? BLDG_TYPE_A_BARRAKS : BLDG_TYPE_H_BARRACKS;
            break;
        case 0x10: // GARAGE-housed group (0x0047408f-0x004740a5)
            target = (race == RACE_ALIEN) ? BLDG_TYPE_A_GARAGE : BLDG_TYPE_H_GARAGE;
            break;
        case 0x11: // HELIPAD-housed group (0x004740ad-0x004740c3)
            target = (race == RACE_ALIEN) ? BLDG_TYPE_A_HELIPAD : BLDG_TYPE_H_HELIPAD;
            break;
        case 0x12: // AIRFIELD-housed group (0x004740cb-0x004740e1)
            target = (race == RACE_ALIEN) ? BLDG_TYPE_A_AIRFIELD : BLDG_TYPE_H_AIRFIELD;
            break;
        default: // 0x004740e9-0x004740f0: unrecognised reason -- return 0 immediately, scan never runs.
            return 0;
    }

    // 0x004740f2-0x0047412d: linear scan Building[1..99] (index 0 never checked -- same "loop starts
    // at 1" shape as sim_game_get_starting_unit.cpp's own cfg-table scan) for the first entry whose
    // `.type` matches `target`; return its index, or 0 if the scan exhausts. `100` is the literal loop
    // bound read off the assembly (`CMP dword ptr [...],0x64`), not the live Building-count global.
    for (int32_t i = 1; i < 100; ++i) {
        if (v.cfg_buildings[i].type == target) return i;
    }
    return 0;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

int32_t reason_to_housing_bldg(uint32_t reason, int32_t race) {
    const sim_view v = state().read;
    return detail::reason_to_housing_bldg(v, reason, race);
}


} // namespace mh::sim
