//
// ai/ai_target.cpp -- see ai_target.h. Translated from the DISASSEMBLY
// (tmp/decomp_ai_a_l1/*.asm), not from Ghidra's C: target_list_add's decompile plate claims it
// "notifies via llm_strat_ai_group_no_action() on success" -- there is no such call, or anything
// resembling one, anywhere in the assembly. Ignored as a stale/wrong auto-generated plate.
//
#include "addr/mh_export.gen.h" // MH_EXPORT_REPLACE / the entry-thunk shapes
#include "ai/ai_target.h"


namespace mh::ai {
namespace detail {

int32_t scan_targets_for_engage(const ai_view &v, const ai_calls &gc, int32_t player_idx,
                                int32_t target_id) {
    const player_data &pd      = v.players[player_idx];
    int32_t            offered = 0;
    // UNSIGNED compare against the count (JC, not JL) -- same shape as ai_engage.h's producer.
    for (uint32_t i = 0; i < (uint32_t)pd.ai_target_list_count; ++i) {
        const target_entry &e = pd.ai_target_list[i];
        if ((e.victim_ref & 0xa0u) == 0) continue;
        if (target_id != e.victim_index) continue;

        // Hostility is a SIGN test: `CMP <relation>,-1 / JG skip` keeps only relation <= -1. Same
        // masked index (aggressor_ref & 0xf) ai_engage.h's producer uses over the same table.
        if (pd.ai_player_relation[e.aggressor_ref & 0xf] > -1) continue;

        gc.engage_candidate_add(e.aggressor_ref, e.aggressor_index);
        ++offered;
    }
    return offered;
}

void target_list_add(const ai_store &own, const ai_calls &gc, int32_t player, int32_t victim_ref,
                     int32_t victim_index, uint32_t aggressor_ref, int32_t aggressor_index) {
    // Never track our own objects: low nibble of aggressor_ref is the owning player.
    if ((aggressor_ref & 0xfu) == (uint32_t)player) return;

    player_data &pd = own.players[player];

    // Dedupe key is BOTH aggressor_index and the FULL (unmasked) aggressor_ref -- not just the owner
    // nibble. Same unsigned (JC) loop-bound compare as the reader above.
    for (uint32_t i = 0; i < (uint32_t)pd.ai_target_list_count; ++i) {
        const target_entry &e = pd.ai_target_list[i];
        if (e.aggressor_index == aggressor_index && e.aggressor_ref == aggressor_ref) return;
    }

    // Cap test is `CMP count,0x40 / JZ return` -- an EQUALITY test. The original relies on nothing
    // ever incrementing the count past 64 rather than range-checking it here.
    if (pd.ai_target_list_count == 64) return;

    target_entry &e = pd.ai_target_list[pd.ai_target_list_count];
    e.victim_index  = victim_index;
    e.victim_ref    = (uint32_t)victim_ref;
    // ai_group_index is looked up only when this player's AI is enabled; otherwise the field is
    // explicitly zeroed rather than left unset (the original's else-branch is a real store, not an
    // omission).
    e.ai_group_index  = (pd.ai_enabled != 0)
                            ? (int32_t)gc.unit_get_ai_group_index(player, victim_index)
                            : 0;
    e.aggressor_index = aggressor_index;
    e.aggressor_ref   = aggressor_ref;
    ++pd.ai_target_list_count; // incremented LAST, after every field of the new entry is written
}

int32_t target_list_remove(const ai_store &own, int32_t player, uint32_t aggressor_ref,
                           int32_t aggressor_index) {
    player_data &pd      = own.players[player];
    int32_t      removed = 0;

    // The bound is RE-READ every iteration (CMP EDX,[..+0xe930e8] / JC at 0x004d6f04, unsigned), so
    // the decrement below genuinely shortens the walk -- it is not a cached count.
    for (uint32_t i = 0; i < (uint32_t)pd.ai_target_list_count; ++i) {
        const target_entry &e = pd.ai_target_list[i];
        if (e.aggressor_index != aggressor_index) continue; // CMP @0x004d6e98, the FIRST test
        if (e.aggressor_ref != aggressor_ref) continue;     // CMP @0x004d6ea4

        // Swap-with-last, SKIPPED when the match is already the last live entry (JZ @0x004d6eb2).
        // The copy is a 5-dword REP MOVSD, i.e. the whole 0x14-byte record.
        const uint32_t last = (uint32_t)pd.ai_target_list_count - 1u;
        if (i != last) pd.ai_target_list[i] = pd.ai_target_list[last];
        --pd.ai_target_list_count;
        ++removed;

        // NO `--i` HERE, DELIBERATELY. The original's INC EDX @0x004d6eed is on the match path as
        // well, so the entry just moved down from the tail is never examined. See the header.
    }
    return removed;
}

} // namespace detail

// ---- the public wrappers ------------------------------------------------------------------------

int32_t scan_targets_for_engage(int32_t player_idx, int32_t target_id) {
    const ai_state st = state();
    return detail::scan_targets_for_engage(st.read, live_calls(), player_idx, target_id);
}

void target_list_add(int32_t player, int32_t victim_ref, int32_t victim_index, uint32_t aggressor_ref,
                     int32_t aggressor_index) {
    const ai_state st = state();
    detail::target_list_add(st.own, live_calls(), player, victim_ref, victim_index, aggressor_ref,
                            aggressor_index);
}

void target_list_remove(int32_t player, uint32_t aggressor_ref, int32_t aggressor_index) {
    const ai_state st = state();
    (void)detail::target_list_remove(st.own, player, aggressor_ref, aggressor_index);
}

// ---- differential-oracle arms -------------------------------------------------------------------
//
// Both run on mh::ai::shadow_calls(). Neither of their callees escapes the declared region set:
// engage_candidate_add writes only the engage scratch and its count (declared by the scan site via
// extra_regions), and unit_get_ai_group_index is a pure read -- so both run FOR REAL, and stubbing
// either would manufacture the divergence it looks like it prevents.
//
// The signatures here are the GENERATED ones (mh::exp::sig_*), typed from the committed Ghidra
// prototype rather than from this module's wrappers -- which is why target_list_add's player is a
// uint32_t here and an int32_t below.

} // namespace mh::ai
