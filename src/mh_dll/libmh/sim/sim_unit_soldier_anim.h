#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// ---- the outward calls -----------------------------------------------------------------------
//
// One struct for the whole TU (matches sim_unit_ctrl_group.h's shape): each member is used by exactly
// one of the two functions below, noted per member. Indirected for the usual reason -- a direct
// mh::call:: inside a detail:: body reaches into the live game image, which makes the body untestable
// by net_selftest.exe simtest.
struct unit_soldier_anim_calls {
    // llm_strat_facing24_from_points @0x004946d2 -- start_walk_anim only. Converts a (from,to) tile
    // delta into one of 24 facing octants; used here purely to pick sprite_frame.
    uint8_t (*facing24_from_points)(char from_x, char from_y, char to_x, char to_y);

    // llm_ui_cursor_apply_anim_frame_offset @0x00486a8a -- pick_lead_soldier_in_direction only. `llm_ui_*`
    // by name (inside SIM-CUT's presentation wall) but one of the profile's three named exceptions: a
    // pure cursor-offset-table QUERY, safe to call through like any other original -- see the sim profile
    // note the conductor's batch context cites. Not stubbed inert, not routed through the effect seam.
    void (*cursor_apply_anim_frame_offset)(char *out_x, char *out_y, int32_t heading);

    // llm_math_manhattan_dist @0x00489164 -- pick_lead_soldier_in_direction only. Pure |dx|+|dy|.
    int32_t (*manhattan_dist)(int32_t x0, int32_t y0, int32_t x1, int32_t y1);
};

const unit_soldier_anim_calls &live_unit_soldier_anim_calls();

namespace detail {

// llm_strat_unit_soldiers_start_walk_anim @0x004897c7. `void __watcall(uint player, int unit_index)`.
//
// Walks unit[player][unit_index]'s soldier chain (head = unit.unit_above, chained via next_soldier,
// terminated at next_soldier==0 -- a DO-WHILE, so a squad with unit_above==0 still visits
// _G_LLM_STRAT_SOLDIERS[player][0], the roster's "used-slot count" metadata slot, exactly once; same
// shape sim_unit_update_soldiers.h documents for its own head-seeded do-while over this identical
// chain). For every visited soldier whose (end_x,end_y) differs from (start_x,start_y) AND whose
// walk_duration bit pattern is +0.0/-0.0 (idle): computes a facing octant from (start->end) for
// sprite_frame, zeroes walk_elapsed, and sets walk_duration = (|dx|+|dy|) * cfg Unit.step_speed[player]
// * 2.0 -- see the .cpp for the x87 multiply-order derivation and the player-indexes-step_speed
// confirmation (both required by the translator brief's hazard list).
void unit_soldiers_start_walk_anim(const sim_view &v, sim_store &own, const unit_soldier_anim_calls &c,
                                   uint32_t player, int32_t unit_index);

// llm_strat_unit_squad_pick_lead_soldier_in_direction @0x0048905e.
// `uint __mh_watcall_ebx_volatile(int player, uint head_soldier_idx, uint heading)`.
//
// Converts `heading` to a direction-vector offset (out_x,out_y) via the cursor animation-offset table
// (pre-seeded to 0x10 before the call -- see the .cpp), then scans the soldier chain STARTING at
// `head_soldier_idx` -- a plain WHILE loop testing the CURRENT soldier's next_soldier before advancing
// (NOT the do-while the sibling chain-walkers use: the head itself is scored once before the loop, and
// the loop only ever visits soldiers reached via next_soldier) -- scoring each visited soldier's
// manhattan distance from (out_x,out_y) and returning the index of the one with the STRICTLY largest
// score, keeping the first (the head) on ties or an empty tail.
uint32_t squad_pick_lead_soldier_in_direction(const sim_view &v, int32_t player, uint32_t head_soldier_idx,
                                              uint32_t heading, const unit_soldier_anim_calls &c);

} // namespace detail

// Live wrappers: the logic applied to state() (and live_unit_soldier_anim_calls()). Match the
// originals' committed signatures (mh_calls.gen.h / mh_export.gen.h's sig_ typedefs) -- the original
// returns `uint` per the .asm header, matching mh_calls.gen.h's uint32_t binding.
void     unit_soldiers_start_walk_anim(uint32_t player, int32_t unit_index);
uint32_t squad_pick_lead_soldier_in_direction(int32_t player, uint32_t head_soldier_idx, uint32_t heading);

namespace detail {
} // namespace detail

} // namespace mh::sim
