//
// sim/sim_diplomacy_ai_relation_swap.cpp -- see sim_diplomacy_ai_relation_swap.h. Translated from
// the DISASSEMBLY (tmp/decomp/llm_diplomacy_ai_relation_swap_004dc0dd.asm); the exported .c draft
// agrees with it exactly (same read, same store, same return), so this is another case where the
// draft's logic was worth re-deriving as asked and turned out correct.
//
#include "sim/sim_diplomacy_ai_relation_swap.h"

#include "ai/ai_state.h" // ai_say / trace_budget -- the shared trace sink, not AI state

namespace mh::sim {

namespace detail {

int32_t diplomacy_ai_relation_swap(sim_store &own, int32_t player, int32_t toward_player,
                                   int32_t new_relation) {
    // 0x004dc0e7-0x004dc113: the whole body is one record's read-modify-write --
    // player_data[player].ai_player_relation[toward_player] -- via sim_store::player_at(), which
    // resolves the same row-stride (0x288fc) the asm's SHL/ADD/SUB chain folds `player` into. ONE
    // fetch, reused for the read and the write (never re-resolved), matching the house convention.
    player_data &pd                      = own.player_at(static_cast<uint32_t>(player));
    int32_t      old                     = pd.ai_player_relation[toward_player];
    pd.ai_player_relation[toward_player] = new_relation;
    return old;
}

} // namespace detail

// ---- the public wrapper --------------------------------------------------------------------------

int32_t diplomacy_ai_relation_swap(int32_t player, int32_t toward_player, int32_t new_relation) {
    sim_state st = state();
    return detail::diplomacy_ai_relation_swap(st.own, player, toward_player, new_relation);
}


} // namespace mh::sim
