//
// ai/ai_scan_targets.cpp -- see ai_scan_targets.h. Translated from the DISASSEMBLY
// (tmp/decomp_a2/llm_strat_ai_target_list_invalidate_by_id_004ec51d.asm and
// tmp/decomp_a2/llm_strat_ai_target_list_refresh_mothers_004ec58a.asm), not from Ghidra's C: both
// draft .c plates are marked "Unverified lead (low confidence)" / hypothesise a "destroy-time
// invalidation propagation" that appears nowhere in the assembly -- ignored as stale/wrong
// auto-generated plates, same as ai_target.cpp's precedent. The draft .c's own field/argument
// reads (victim_ref, ai_group_index, aggressor_ref, position) do match the assembly, however, and are
// reproduced here as such.
//
#include "ai/ai_scan_targets.h"


namespace mh::ai {
namespace detail {

int32_t target_list_invalidate_by_id(const ai_view &v, const ai_calls &gc, int32_t player,
                                     int32_t target_id) {
    const player_data &pd      = v.players[player];
    int32_t            offered = 0;
    // UNSIGNED compare against the count (JC, not JL) -- same shape as every other ai_target_list
    // walk in this cluster.
    for (uint32_t i = 0; i < (uint32_t)pd.ai_target_list_count; ++i) {
        const target_entry &e = pd.ai_target_list[i];
        if ((e.victim_ref & 0xa0u) == 0) continue;
        // FIELD-IDENTITY: compared against ai_group_index, NOT target_id -- see the header comment
        // and uncertainties[] in the translator report. Literal from the assembly
        // (0x004ec550: CMP EDX,[EBP-0x14] against [entry+0x8]).
        if (e.ai_group_index != target_id) continue;

        // Args are (aggressor_ref, position) off the entry, forwarded positionally into
        // scan_target_list_add's (target_id, target_owner) parameter slots -- a name mismatch on the
        // callee side, not something introduced by this translation; see ai_state.h.
        gc.scan_target_list_add(player, (int32_t)e.aggressor_ref, e.aggressor_index);
        ++offered;
    }
    return offered;
}

int32_t target_list_refresh_mothers(const ai_view &v, const ai_calls &gc, int32_t player) {
    const player_data &pd      = v.players[player];
    int32_t            offered = 0;
    for (uint32_t i = 0; i < (uint32_t)pd.ai_target_list_count; ++i) {
        const target_entry &e = pd.ai_target_list[i];
        if ((e.victim_ref & 0x40u) == 0) continue;

        gc.scan_target_list_add(player, (int32_t)e.aggressor_ref, e.aggressor_index);
        ++offered;
    }
    return offered;
}

} // namespace detail

// ---- the public wrappers ------------------------------------------------------------------------

int32_t target_list_invalidate_by_id(int32_t player, int32_t target_id) {
    const ai_state st = state();
    return detail::target_list_invalidate_by_id(st.read, live_calls(), player, target_id);
}

int32_t target_list_refresh_mothers(int32_t player) {
    const ai_state st = state();
    return detail::target_list_refresh_mothers(st.read, live_calls(), player);
}

// ---- differential-oracle arms -------------------------------------------------------------------
//
// Both run on mh::ai::shadow_calls(). Their only outward call, scan_target_list_add, appends to
// _G_LLM_STRAT_AI_SCAN_TARGETS (not player_data, not ai_target_list), matching ai_target.h's own
// scan-side reader precedent of running its outward calls for real.

} // namespace mh::ai
