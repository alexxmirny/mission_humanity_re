//
// ai/ai_map_influence.h -- the AI's per-player influence-map recompute (RI-AI / AI1B layer 1).
//
// llm_strat_ai_recompute_map_influence @0x004d7a2e. Its only caller is llm_strat_ai_players_tick
// @0x004db954, gated by `(ai_map_changed_pending & ai_enabled) != 0` -- so it runs once per AI
// player per tick in which something changed the map layout near that player, and never for a human.
//
// THE FUNCTION IS A PURE DRIVER. Its whole observable behaviour is: clear one dword, then emit 28
// __cdecl calls into the two grid primitives with exact arguments. It writes nothing else itself and
// it branches on nothing -- every call runs the identical 28-call sequence. That is why the offline
// test can be near-exhaustive here (assert the entire sequence, all five arguments each) in a way it
// cannot be for a body full of state-dependent branches: the sequence IS the function.
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

namespace detail {

// What one call did, for the shadow arm's histogram and for the offline assertions. There is no
// outcome enum because there is no branch: `flood_calls` is 27 on every call or the translation is
// wrong.
struct influence_report {
    int32_t fill_calls  = 0; // 1
    int32_t flood_calls = 0; // 27 -- see ai_map_influence.cpp for the per-site derivation
    // The extents as read on the LAST call of the pass. The original re-pushes both globals for
    // every one of the 28 calls rather than caching them, so these are recorded to make a
    // translation that hoisted them (and would therefore miss a mid-pass change) distinguishable.
    int32_t width       = 0;
    int32_t height      = 0;
    bool    was_pending = false; // ai_map_changed_pending as it stood on entry
};

// The logic over an EXPLICIT state + call set, so `net_selftest.exe aitest` can drive it over heap
// buffers with recording stubs and no game. The wrapper below is this applied to state().
influence_report recompute_map_influence(const ai_view &v, const ai_store &own, const ai_calls &gc,
                                         int32_t player);

} // namespace detail

void recompute_map_influence(int32_t player);

} // namespace mh::ai
