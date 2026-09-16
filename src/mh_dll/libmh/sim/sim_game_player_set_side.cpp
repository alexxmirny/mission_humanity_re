#include "sim/sim_game_player_set_side.h"

#include "addr/mh_calls.gen.h"  // typed callables for the original functions we still call OUT to
#include "ai/ai_state.h"        // ai_say / trace_budget -- the shared trace sink, not AI state
#include "addr/mh_rebind.gen.h" // LIB-REBIND: the config-selected binder
#include "state/rebind_targets.gen.h"

namespace mh::sim {

const game_player_set_side_calls &live_game_player_set_side_calls() {
    static const game_player_set_side_calls c = {
        MH_LIBMH_BIND(llm_map_fog_of_war_recompute),
    };
    return c;
}

namespace detail {

void game_player_set_human(sim_store &own, const game_player_set_side_calls &c, uint8_t player) {
    // 0x0049e774-0x0049e77b: OR the player's bit into _G_LLM_GAME_HUMAN_PLAYER_MASK. `player & 0x1f`
    // mirrors x86's own internal masking of the SHL/CL count (Intel SDM: the count is ANDed with 0x1f
    // regardless of the destination operand's width), so computing the shift at 32-bit width and then
    // narrowing to uint8_t is bit-identical to the assembly's native 8-bit `SHL AL,CL` -- for a count
    // in [8,31] both give a zero byte, since the set bit falls outside the low 8 bits either way.
    own.game_human_player_mask() |= (uint8_t)(1u << ((uint32_t)player & 0x1fu));

    // 0x0049e781-0x0049e788: re-read that SAME byte and zero-extend it into is_human. A second,
    // independent write -- not a cached copy of the OR above (batch context: both writes are real).
    own.is_human_mut() = (uint32_t)own.game_human_player_mask();

    // 0x0049e78d: unconditional tail call, routed through the indirected callee (translator brief
    // rule 3 -- llm_map_fog_of_war_recompute is a real sim sibling migration-set function, not
    // reimplemented here even though it is itself in flight this same slice).
    c.fog_of_war_recompute();
}

void game_player_set_ai(sim_store &own, const game_player_set_side_calls &c, uint8_t player) {
    // 0x0049e7b7-0x0049e7c0: mirror of game_player_set_human above -- AND the player's bit OUT of
    // _G_LLM_GAME_HUMAN_PLAYER_MASK. `NOT AL` in the assembly complements the already-8-bit-truncated
    // shifted value, so complementing the narrowed uint8_t (rather than the wide 32-bit shift result)
    // is the faithful mirror.
    const uint8_t bit = (uint8_t)(1u << ((uint32_t)player & 0x1fu));
    own.game_human_player_mask() &= (uint8_t)~bit;

    // 0x0049e7c6-0x0049e7cd: re-read the SAME byte and zero-extend it into is_human. Second,
    // independent write, same as the human-side mirror.
    own.is_human_mut() = (uint32_t)own.game_human_player_mask();

    // 0x0049e7d2: unconditional tail call, same routing as game_player_set_human above.
    c.fog_of_war_recompute();
}

} // namespace detail

// ---- the public surface --------------------------------------------------------------------------

void game_player_set_human(uint8_t player) {
    sim_state st = state();
    detail::game_player_set_human(st.own, live_game_player_set_side_calls(), player);
}

void game_player_set_ai(uint8_t player) {
    sim_state st = state();
    detail::game_player_set_ai(st.own, live_game_player_set_side_calls(), player);
}


} // namespace mh::sim
