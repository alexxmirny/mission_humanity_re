//
// lockstep/lt_player_by_side_id.cpp -- see lt_player_by_side_id.h.
//
// Translated from tmp/decomp_lib_trans/llm_strat_player_by_side_id_0049e388.asm, NOT from the .c
// beside it (the .c is a faithful transcription here -- no draft/asm divergence on this one -- but
// the asm remains the source per house rule; the dead `MOV EAX,[EBP-0x1c]` load at the loop-increment
// label, present in the asm and correctly dropped by the decompiler, is called out in the header).
//
#include "lockstep/lt_player_by_side_id.h"


namespace mh::lockstep {

namespace detail {

// ---- llm_strat_player_by_side_id @0x0049e388 -------------------------------------------------------
//
// Linear scan of ALL 8 slots, unconditionally (no ALIVE/HUMAN gate -- see the header's point 2),
// returning the FIRST index whose `.side_id` matches (point 3). -1 if none does.
int32_t player_by_side_id(const engine_state &st, int32_t side_id) {
    for (int32_t i = 0; i < MAX_PLAYERS; ++i) {
        if (st.players[i].side_id == side_id) return i; // 0x0049e3c1/0x0049e3c7/0x0049e3ca/0x0049e3cc
    }
    return -1; // 0x0049e3d6 -- reached only once all 8 slots have failed
}

} // namespace detail

// ---- production entry point (bound to the live game state) ----------------------------------------
int32_t player_by_side_id(int32_t side_id) { return detail::player_by_side_id(state(), side_id); }

} // namespace mh::lockstep
