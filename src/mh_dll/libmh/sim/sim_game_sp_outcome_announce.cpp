//
// sim/sim_game_sp_outcome_announce.cpp -- see sim_game_sp_outcome_announce.h. Translated from the
// DISASSEMBLY (tmp/decomp/llm_game_sp_outcome_announce_0049803b.asm), which agrees exactly with the
// Ghidra .c draft here -- there is no draft/asm disagreement to resolve for this function.
//
#include "sim/sim_game_sp_outcome_announce.h"

#include "addr/mh_calls.gen.h"      // typed callables for the original functions we still call OUT to
#include "lockstep/overlay_hoist.h" // R3b: outcome_dialog's gated panel hoist
#include "ai/ai_state.h"            // ai_say / trace_budget -- the shared trace sink, not AI state
#include "state/host_api.h"
#include "state/host_events.h"

namespace mh::sim {

const game_sp_outcome_announce_calls &live_game_sp_outcome_announce_calls() {
    static const game_sp_outcome_announce_calls c = {
        mh::state::evt::text_queue_id,
        mh::state::evt::snd_play,
        mh::state::evt::outcome_dialog_i32,
        &mh::lockstep::live_overlay_hoist_ops(), // LIB-ABI stage E / R3b
    };
    return c;
}

namespace detail {

void game_sp_outcome_announce(const sim_view &v, sim_store &own,
                              const game_sp_outcome_announce_calls &c) {
    // Until LIB-ABI stage E this function wrote no sim state at all -- its effect was the three
    // calls below. The stage-E hoist at the tail re-stores outcome_dialog's GAME_MODE=3.

    // 0x00498053-0x0049805a: `CMP [_G_LLM_GAME_SESSION_MODE],1 / JNZ LAB_0049807f` -- only when the
    // session mode is exactly SESSION_SP does anything happen; every other mode (including MP
    // lockstep) falls straight through to the epilogue.
    if (*v.session_mode != SESSION_SP) return;

    // 0x0049805c-0x00498065: MOV EAX,0x7e ; CALL llm_ui_print_queue_text_id
    c.ui_print_queue_text_id(TEXT_ID_SP_OUTCOME);
    // 0x00498066-0x00498075: MOV EDX,0x64 ; MOV EAX,0xa6 ; CALL llm_snd_play -- sound 0xa6, volume
    // 100, fixed (no player_race/player branch here, unlike sibling snd_play sites elsewhere in the
    // closure).
    c.snd_play(SND_SP_OUTCOME_ID, SND_SP_OUTCOME_VOLUME);
    // 0x00498075-0x0049807f: MOV EAX,0x4 ; CALL llm_ui_outcome_dialog -- return value discarded,
    // matching both the asm (nothing consumes EAX after the call) and the .c draft.
    (void)c.ui_outcome_dialog(OUTCOME_DIALOG_KIND);
    // LIB-ABI stage E hoist: the callee's GAME_MODE=3 @0x004c6cec is unconditional.
    own.game_mode() = 3;
    // R3b: outcome_dialog's gated panel pair -- see overlay_hoist.h.
    mh::lockstep::hoist_outcome_panel(c.hoist);
}

} // namespace detail

void game_sp_outcome_announce() {
    sim_state st = state();
    detail::game_sp_outcome_announce(st.read, st.own, live_game_sp_outcome_announce_calls());
}


} // namespace mh::sim
