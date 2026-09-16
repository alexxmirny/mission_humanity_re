//
// ai/ai_worker_priority.cpp -- see ai_worker_priority.h. Translated from the DISASSEMBLY
// (tmp/decomp_a4/llm_strat_ai_is_worker_priority_candidate_004e3a77.asm), not from Ghidra's C: the
// decompile's byte-offset arithmetic (player * 0x6aa4 + 0xc3d2a2 + site_index * 0x111, and the
// production-record base 0xdd2648 hidden inside its two literal reads at +0xdd264c/+0xdd264d) is
// exactly what building_of() / production_of() already resolve through the region registry, so none
// of it survives into this file as a byte offset.
//
#include "ai/ai_worker_priority.h"


namespace mh::ai {
namespace detail {

bool is_worker_priority_candidate(const ai_view &v, int32_t player, int32_t site_index) {
    const building    &b             = building_of(v, (uint32_t)player, site_index);
    const player_data &pd            = v.players[player];
    const uint32_t     building_type = b.building_id; // ushort -- NOT a unit_proto_id.

    // The four-way match (0x004e3acd-0x004e3aeb). Compared bit-for-bit against int32_t slots that
    // hold -1 when unset, so an unsigned compare against a zero-extended ushort can never spuriously
    // match a -1 sentinel.
    const bool matched = building_type == (uint32_t)pd.ai_resource_shortage_candidates[0] ||
                         building_type == (uint32_t)pd.ai_resource_shortage_candidates[1] ||
                         building_type == (uint32_t)pd.ai_resource_shortage_candidates[2] ||
                         building_type == (uint32_t)pd.ai_resource_shortage_candidates[3];

    if (matched) {
        // production_of()'s inner index is the building's SLOT (sub_id), not its roster index --
        // unchecked in the original and here.
        const production &prod = production_of(v, (uint32_t)player, b.sub_id);
        if (prod.active_unit_type != 0) return true;

        // 0x004e3b44-0x004e3ba7: INCLUSIVE, UNSIGNED bound (CMP EBX,total ; JBE) -- slot 0 is never
        // inspected, and slot `total` IS. Do not rewrite this as `< total`.
        for (uint32_t i = 1; i <= v.cfg_unit_sec->total; ++i) {
            if (prod.queued_count[i] != 0) return true;
        }
        return false;
    }

    // LAB_004e3bb0: the ELSE path's polarity is INVERTED relative to the four-way match above it --
    // true when the id DIFFERS from ai_build_candidate_cat_0x30, false when it matches. This is the
    // opposite sense from `matched` and is read straight off the branch, not inferred from symmetry.
    return building_type != (uint32_t)pd.ai_build_candidate_cat_0x30;
}

} // namespace detail

bool is_worker_priority_candidate(int32_t player, int32_t site_index) {
    const ai_state st = state();
    return detail::is_worker_priority_candidate(st.read, player, site_index);
}


} // namespace mh::ai
