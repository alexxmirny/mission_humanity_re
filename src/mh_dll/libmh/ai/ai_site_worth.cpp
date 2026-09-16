//
// ai/ai_site_worth.cpp -- see ai_site_worth.h. Translated from the DISASSEMBLY
// (tmp/decomp_ai/llm_strat_ai_resource_site_meets_threshold_004e62ad.asm), not from Ghidra's C.
//
// EVERY ABSOLUTE RE-DERIVED against addr/mh_structs.gen.h and addr/mh_addrs.gen.h rather than
// transcribed:
//   0xd9f21d = Building + 0x59d  = cfg::final::struct::Building.extract_id (stride 0x842,
//              IMUL EDX,EBP,0x842 @0x004e62e3)
//   0xc28530 = resources          = map::resources[64][64], 16 bytes per cell (index built by
//              SHL 0xa on x >> 2 and SHL 4 on y >> 2 @0x004e6318-0x004e6320)
//   0x6616a8 = _G_LLM_STRAT_AI_ACTIVE_PLAYER_COUNT, the dword IN FRONT of the weight table -- the
//              real table is _G_LLM_STRAT_AI_RESOURCE_VALUE_WEIGHTS @0x6616ac and the original's
//              index is one-based off the earlier symbol; see resource_value_weight()
//   0xe7df24 = player_data + 0x10064 = ai_mine_yield_by_resource (stride 0x288fc, built by the
//              SHL 2 / ADD / SHL 7 / SUB / SHL 2 / SHL 6 / ADD chain @0x004e634f-0x004e6363)
//   0x66936c = _G_LLM_STRAT_AI_CFG_MINE_WORTH
// so no byte offset and no literal VA appears below (Law 1).
//
#include "ai/ai_site_worth.h"


namespace mh::ai {
namespace detail {

namespace {

// The frame is 0x44 bytes and holds exactly two int[8] locals -- contrib[] at ESP+4 and
// extract_mask[] at ESP+0x24 -- which tile it with nothing left over. Only indices 1..4 are ever
// written or read; the original leaves 0 and 5..7 UNINITIALISED, and we zero them, which is
// observably identical for every extract_id in 1..4 (the only ids the map's `short value[8]` and the
// weight table can both address). Modelled as [8] rather than [5] so that an out-of-domain
// extract_id lands where the original's store would land instead of past the end of our array.
constexpr int32_t SLOTS = 8;

} // namespace

bool resource_site_meets_threshold(const ai_view &v, int32_t player_idx, int32_t building_type,
                                   int32_t fine_x, int32_t fine_y) {
    // SHR ESI,0x2 / SHR EDI,0x2 @0x004e62c4 -- LOGICAL shifts, so the coordinates are consumed
    // unsigned even though the committed prototype types them `int`.
    const uint32_t x = (uint32_t)fine_x;
    const uint32_t y = (uint32_t)fine_y;

    int32_t extract_mask[SLOTS] = {};
    int32_t contrib[SLOTS]      = {};

    // Pass 2. `CMP dword ptr [EDX + 0xd9f21d],0x0` / `JZ 0x004e630d` @0x004e62f0 jumps CLEAR OF the
    // loop, so the first zero id ends the walk -- it is a break, not a skip.
    const cfg_building &cfg = v.cfg_buildings[building_type];
    for (uint32_t i = 0; i < 4u; ++i) {
        const int32_t id = cfg.extract_id[i];
        if (id == 0) break;
        extract_mask[id] = 1;
    }

    // Pass 3. MOVZX @0x004e6324 -- the cell's `short` is read UNSIGNED.
    const map_resources &cell  = resource_cell_at_fine(v, x, y);
    uint32_t             total = 0;
    for (int32_t id = RESOURCE_ID_FIRST; id <= RESOURCE_ID_LAST; ++id) {
        const int32_t c = (int32_t)(uint32_t)(uint16_t)cell.value[id] * extract_mask[id] *
                          resource_value_weight(v, id);
        contrib[id] = c;
        total += (uint32_t)c; // ADD ECX,EBX @0x004e633d
    }

    // Pass 4, the veto. Both tests are `!= 0 -> skip`, so the accumulator is cleared only when the
    // player has NO yield for that resource and this site contributes none either. It keeps running
    // afterwards -- there is no early exit -- which is immaterial once the total is 0 but is what the
    // original does.
    const player_data &pd = v.players[player_idx];
    for (int32_t id = RESOURCE_ID_FIRST; id <= RESOURCE_ID_LAST; ++id) {
        if (pd.ai_mine_yield_by_resource[id] != 0) continue; // CMP .. ,0x0 / JNZ @0x004e636a
        if (contrib[id] != 0) continue;                      // CMP .. ,0x0 / JNZ @0x004e6374
        total = 0;                                           // XOR ECX,ECX @0x004e637b
    }

    // CMP ECX,dword ptr [0x0066936c] / SETNC AL @0x004e6383 -- SETNC is SETAE, i.e. an UNSIGNED
    // >=. Both sides are read as uint32 for that reason.
    return total >= (uint32_t)*v.mine_worth;
}

} // namespace detail

bool resource_site_meets_threshold(int32_t player_idx, int32_t building_type, int32_t fine_x,
                                   int32_t fine_y) {
    const ai_state st = state();
    return detail::resource_site_meets_threshold(st.read, player_idx, building_type, fine_x, fine_y);
}

// ---- the differential-oracle arm ----------------------------------------------------------------
//
// The body writes NOTHING -- the state matrix measures five cells and all five are reads -- so this
// site declares no regions and its entire verdict is the return value. That is the same shape
// ai_nearest_flagged.cpp arms, and the same warning applies with more force here, because the return
// is one bit rather than an index.
//
// WHAT A VACUOUS GREEN LOOKS LIKE. On a developed base the answer is `true` for almost every call:
// the shipped nMineWorth is 5000 and a recorded site was already worth more than 3000 RAW when
// llm_strat_spawn_ai_base put it in the list. So a run of thousands of identical `true`s compares
// almost nothing. The counters below are what the evidence tier actually rests on:
//   below   -- the threshold compare answered false, i.e. pass 3's total was genuinely too small
//   vetoed  -- pass 4 fired at least once on this call (the branch the plate used to omit)
//   noextract -- Building[type].extract_id[0] was zero, so pass 2 broke immediately and every
//                contrib is 0. Distinguishes "a mine type with no extraction list" from a poor site.
// `vetoed > 0` is the only proof the fourth pass was ever exercised; `below > 0` the only proof the
// threshold can answer no.

} // namespace mh::ai
