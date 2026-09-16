//
// sim/sim_diplomacy_set_relation.h -- llm_diplomacy_set_relation, the diplomacy-relation writer
// (RI-SIM / SIM1F).
//
// One function: llm_diplomacy_set_relation @0x0049a0b3 (0x11b bytes). Reached from
// llm_strat_order_queue_dispatch's order-code table (case 15/0xf, order_code 0xf4, high/special
// command range) and from several other all-pairs relation-setup call sites. Writes
// Players[player_a].relation[player_b], builds a "player_a (name_a) player_b (name_b)" wide status
// line into the shared text scratch buffer, appends a friendly/neutral/war phrase depending on the
// new relation value, then hands the derived relation SIGN (not the raw relation byte) to
// llm_diplomacy_ai_relation_swap and rebuilds the chat ally mask.
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// Indirected for the same reason as every other module here: a direct mh::call:: inside a detail::
// body reaches into the live game image, which makes the body untestable by net_selftest.exe simtest.
struct diplomacy_set_relation_calls {
    // llm_str_ansi_to_wide_scratch @0x004cf3e0 -- converts an ANSI player name into the shared wide
    // scratch buffer. Bound directly to mh::call:: (matching its committed `void *(char *)`
    // signature exactly, per the translation lint's positional pairing) -- the const_cast/reinterpret
    // for the const sim_view's `const char*` field lives at the call site in detail:: instead of in
    // a wrapper here.
    void *(*ansi_to_wide_scratch)(char *ansi);
    // w_sprintf's "%d (%s) %d (%s)" shape (SIM-VARARGS).
    int32_t (*w_sprintf_visis)(void *dst, const wchar_t *format, int32_t a0, const wchar_t *a1,
                               int32_t a2, const wchar_t *a3);
    // w_sprintf's "%s" shape (SIM-VARARGS).
    int32_t (*w_sprintf_vs)(void *dst, const wchar_t *format, const wchar_t *a0);
    int32_t (*diplomacy_ai_relation_swap)(int32_t player, int32_t toward_player, int32_t new_relation);
    void (*net_chat_ally_mask_rebuild)();
};

const diplomacy_set_relation_calls &live_diplomacy_set_relation_calls();

namespace detail {

// llm_diplomacy_set_relation @0x0049a0b3.
void diplomacy_set_relation(const sim_view &v, sim_store &own, const diplomacy_set_relation_calls &c,
                            int32_t player_a, int32_t player_b, uint8_t relation);

} // namespace detail

// Live wrapper: the logic applied to state() and live_diplomacy_set_relation_calls(). Matches the
// original's committed __watcall(EAX,EDX,BL) shape.
void diplomacy_set_relation(int32_t player_a, int32_t player_b, uint8_t relation);


} // namespace mh::sim
