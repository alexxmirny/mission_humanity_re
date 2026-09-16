//
// ai/ai_unit_move_bump.h -- the AI's move-with-bump-check order issuer (RI-AI AI1C, the
// 2026-08-05 slice). Lives OUTSIDE the AI address band (0x0046ae0a, in the order layer): its
// callees are the order path, not the group machinery every other function in this batch touches.
//
// One function: llm_strat_ai_player_tick's own move+bump-check issuer, the enqueue counterpart of
// llm_strat_unit_order_move_confirmed_with_bump (same order 0x18, same equivalent-tile bump-sound
// check) but reached directly by the AI rather than through the player move-order API.
//
#pragma once
#include <cstdint>

#include "ai/ai_state.h"

namespace mh::ai {

// The logic over an EXPLICIT state, so `net_selftest.exe aitest` can drive it over heap buffers
// with no game and no rig. The wrapper below is this applied to state(); the split costs one
// inlined call.
namespace detail {

// llm_strat_ai_unit_order_move_with_bump @0x0046ae0a.
//
// Resets the order scratch and fills fields 0/1 with (order_arg0, order_arg1), enqueues a move
// order (op_code is the LITERAL 0x18 -- this function's own order, NOT the unit's cfg
// move_op_code -- paired with the unit's cfg move_op_arg as `arg`), and notifies the unit's status
// with code 0.
//
// THEN, ONLY WHEN `player` IS THE SIM'S PlayerSide -- `(uint16_t)player == *v.player_side`, a
// THIRD player-indexing global distinct from both player_data and the session player records
// (ai_state.h's field comment) -- it computes the placement corner for the unit's cfg equivalent
// building and previews it, playing a bump sound if the spot is blocked.
//
// THAT GATE IS NOT A "the human did this" TEST: under --soak (all-AI), PlayerSide names an AI
// player too and this tail runs for it. A shadow arm must count it rather than assume it is
// unreachable under an all-AI run.
// WHAT A CALL ACTUALLY DID -- the same arrangement every other module in this batch uses, and here
// it exists for one specific reason: the PlayerSide tail is the only branch this function has, and
// a shadow arm that could not see it would report a call count that says nothing about coverage.
// `bump_played` is ALWAYS false in the shadow arm by construction (the preview call is bound inert
// and returns 0); it is reported anyway so that "always 0" stays visible as a narrowing rather
// than reading as a branch the run covered.
struct move_bump_report {
    bool local_tail  = false; // player == PlayerSide: the placement-preview + sound tail ran
    bool bump_played = false; // ...and the preview reported the spot blocked, so a sound was played
};

move_bump_report unit_order_move_with_bump(const ai_view &v, const ai_store &own,
                                           const ai_calls &gc, uint32_t player, int32_t unit_idx,
                                           uint32_t order_arg0, uint32_t order_arg1);

} // namespace detail

// The two order args are uint32_t here because that is what the ORIGINAL's committed
// prototype says (Ghidra `undefined4`), and mh_export.gen.h's sig_* typedef -- which the
// seam bindings check against -- is generated from it. Nothing in this function
// does arithmetic on either value: both are stored, handed to order_scratch_set_field, and
// handed to the placement helper unchanged, so the width is a pass-through, not a reading.
void unit_order_move_with_bump(uint32_t player, int32_t unit_idx, uint32_t order_arg0,
                               uint32_t order_arg1);

} // namespace mh::ai
