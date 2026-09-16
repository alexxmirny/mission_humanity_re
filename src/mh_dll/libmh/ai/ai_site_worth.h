//
// ai/ai_site_worth.h -- llm_strat_ai_resource_site_meets_threshold @0x004e62ad (AI1B, layer 6).
//
// "Is this candidate deposit worth putting a mine of type `building_type` on?" The sole caller,
// llm_strat_ai_bldg_scan_resource_site_candidates @0x004e63f5, walks
// player_data[p].ai_resource_sites[], skips entries whose `status` is not 0 ("unresolved, still a
// live build candidate") and passes each survivor's coarse grid_x/grid_y as `(g << 2) + 2`, i.e. the
// CENTRE of the coarse cell expressed in fine coordinates. Nothing is assigned to anything at that
// point -- the function's own Ghidra plate called this the building's "currently assigned"
// extraction site until 2026-08-03, and that was wrong on both counts (see ghidra_findings
// 2026-08-03-1758-3; the plate is now corrected in the database).
//
// THE SECOND PARAMETER IS A cfg BUILDING TYPE, not a roster index: the body reaches it with
// `IMUL EDX,EBP,0x842` @0x004e62e3, the cfg::final::struct::Building stride, whereas the per-player
// roster is map::object::building at stride 0x111.
//
// FOUR PASSES:
//   1. zero extract_mask[1..4]                                    @0x004e62d1
//   2. extract_mask[Building[type].extract_id[i]] = 1 for i in 0..3, BREAKING at the first zero id
//                                                                 @0x004e62e3
//   3. contrib[id] = resources[x >> 2][y >> 2].value[id] (unsigned) * extract_mask[id]
//                    * resource_value_weight(id), summed, for id 1..4
//                                                                 @0x004e6316
//   4. VETO: total = 0 if ANY id in 1..4 has BOTH player_data[p].ai_mine_yield_by_resource[id] == 0
//      AND contrib[id] == 0                                       @0x004e634c
// then `return total >= *mine_worth`, compared UNSIGNED (CMP / SETNC @0x004e6383).
//
// PASS 4 IS NOT "the site must supply something I am short of", and reading it that way is the trap
// this comment exists to head off. contrib[id] is zero for every resource this building TYPE does
// not extract -- extract_mask[id] == 0 kills the product at 0x004e632c -- so the veto also fires on
// resources the candidate mine could never have produced. It therefore depends on the player's whole
// existing yield portfolio across ids 1..4, not only on the extracted ones. Reproduced as written;
// nothing here settles whether that was intended.
//
// UNVERIFIABLE AT THE RIG, stated up front so no tier over-reads a clean run: the return is a single
// bool and, on a developed base, overwhelmingly `true`. The shadow arm therefore counts the calls
// that reached each interesting outcome (a veto firing, a below-threshold answer, an empty
// extract_id list) and the tier rests on those, not on the divergence count.
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

namespace detail {
bool resource_site_meets_threshold(const ai_view &v, int32_t player_idx, int32_t building_type,
                                   int32_t fine_x, int32_t fine_y);
} // namespace detail

bool resource_site_meets_threshold(int32_t player_idx, int32_t building_type, int32_t fine_x,
                                   int32_t fine_y);

} // namespace mh::ai
