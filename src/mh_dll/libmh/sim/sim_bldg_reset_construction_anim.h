//
// sim/sim_bldg_reset_construction_anim.h -- llm_bldg_reset_construction_anim, the "put this
// building's visuals back to construction-just-(re)started" helper (RI-SIM SIM1B, building_tick
// machinery slice, translated from the 0x00474ae0 disassembly).
//
// llm_bldg_reset_construction_anim @0x00474ae0 (0x11c B) -- Building-order case 2 (param0 0x6b)
//   helper, called alongside llm_bldg_finish_current_order (see sim_bldg_finish_order.h): where
//   that function handles the economy/worker bookkeeping of closing out an order, this one resets
//   the presentation side. It:
//     1. Zeroes buildings[player][building_index].anim[0 .. sprite_quantity-1], where
//        sprite_quantity comes from Building[buildings[player][building_index].building_id]
//        .sprite_quantity (0x00474b04-0x00474b59 -- the loop bound is RE-READ off the cfg record
//        at the top of every iteration, matching the asm exactly rather than being hoisted; see the
//        .cpp for why this is still safe to cache once here).
//     2. Recomputes anim[0] via llm_cfg_anim_frame_at_progress(Building[building_id].anim[0],
//        buildings[player][building_index].cycle_progress / Building[building_id].build_time_2)
//        (0x00474b5b-0x00474bd4) and stores the result back into anim[0].
//     3. Clears buildings[player][building_index].online_state to 0 (0x00474bda-0x00474bea).
//   Statement order 1-2-3 is exactly the asm's: the cycle_progress/build_time_2 division and the
//   callee happen BEFORE online_state is cleared -- see the .cpp for the address citation.
//
// llm_cfg_anim_frame_at_progress @0x0046190d is already a committed, PURE (int32_t, double) ->
// int32_t function (addr/mh_calls.gen.h) -- called via mh::call:: indirected through the `calls`
// struct below, matching every other outward call in this cluster (translator brief rule 3).
//
// ---- DECLARED NEED: no int32-view accessor exists for Building.anim / cfg Building.anim ----------
// mh_map_object_building::anim and mh_cfg_final_struct_Building::anim are BOTH documented as
// `cfg_t_frame_index[12]` (int32) but flattened by Ghidra to `uint8_t anim[48]` (addr/
// mh_structs.gen.h) -- the exact same flattening sim_state.h's own comment on cfg_unit::weapons
// describes ("the sort of detail a translation gets wrong once and never notices"), for which that
// header already provides cfg_unit_weapon_id()/cfg_unit_weapon_enabled() as the sanctioned
// accessors. No equivalent exists for either anim[12] array. This TU cannot add one to sim_state.h
// (conductor-owned), so it defines two narrowly-scoped, file-local helpers in the .cpp
// (building_anim_frame_of / set_building_anim_frame) that read/write a 4-byte slot of the flattened
// byte array via memcpy -- exactly the "already-typed struct field, not a raw address" shape
// cfg_unit_weapon_id() uses, just needing a memcpy instead of a plain index because the element
// width here is 4 bytes rather than 1. Proposed for the conductor: retype `cfg_t_frame_index` onto
// both anim[] fields in Ghidra (as was done for cfg_unit::weapons via the byte-pair accessors), or
// add sim_state.h accessors named the same way.
//
// sprite_quantity (uint8_t, cfg_final_struct_Building) bounds the zero-loop with NO array-size
// check in the assembly beyond that field itself -- see the .cpp's uncertainty note on what happens
// if live cfg data ever set it above 12 (the physical anim[]/anim_dur[] extent).
//
#pragma once
#include <cstdint>

#include "sim/sim_state.h"

namespace mh::sim {

// The one external callee this closure reaches, indirected for offline testability (same reason as
// every other module here: a direct mh::call:: inside a detail:: body reaches into the live game
// image, which makes the body untestable by net_selftest.exe simtest).
struct bldg_reset_construction_anim_calls {
    // llm_cfg_anim_frame_at_progress @0x0046190d. PURE function of its args (no state touched) --
    // already committed in addr/mh_calls.gen.h.
    int32_t (*cfg_anim_frame_at_progress)(int32_t start_frame, double progress_fraction);
};

const bldg_reset_construction_anim_calls &live_bldg_reset_construction_anim_calls();

namespace detail {

// llm_bldg_reset_construction_anim @0x00474ae0. See the header banner above for the full
// derivation; the .cpp carries the per-line address citation.
void bldg_reset_construction_anim(const sim_view &v, sim_store &own,
                                  const bldg_reset_construction_anim_calls &gc, uint32_t player,
                                  int32_t building_index);

} // namespace detail

// ---- the public surface, in original-behaviour terms -----------------------------------------
// Parameter types match the committed prototype in addr/mh_calls.gen.h exactly (the drift gate
// enforces this on export/shadow installation): void(uint32_t player, int32_t building_index).
void bldg_reset_construction_anim(uint32_t player, int32_t building_index);

namespace detail {
} // namespace detail

} // namespace mh::sim
